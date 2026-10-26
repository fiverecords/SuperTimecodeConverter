// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// ImageGuard -- checks image bytes STC did not make before JUCE decodes them.
//
// JUCE 9.0.3's PNG reader (juce_PNGLoader.cpp, PNGHelpers::errorCallback)
// calls setjmp() in its error callback and returns, so when libpng reports
// an error its longjmp() lands in a stack frame that no longer exists: a
// damaged PNG (a bad CRC, a broken zlib stream, a file cut short) crashes
// the thread that decodes it instead of failing (#26; reproduced on Linux,
// not established on Windows; macOS decodes PNGs with CoreImage instead).
// Album art reaches that reader from a StageLinQ unit's database, from a
// CDJ's dbserver, and from STC's own artwork cache on disk.
//
// isSafePng() therefore validates the whole file first and accepts only
// what libpng 1.6 (as JUCE's reader drives it) reads without an error:
//  - the 8-byte signature;
//  - every chunk: a length below 2^31, a type of four ASCII letters with
//    the reserved bit clear, the whole chunk inside the data, a correct
//    CRC (ancillary chunks too, although libpng only warns for those);
//  - IHDR first, 13 bytes, 1..maxSide pixels a side, a bit depth valid
//    for its colour type, compression and filter method 0, interlace
//    method 0 or 1;
//  - PLTE at most once, before IDAT and before tRNS/bKGD/hIST, 1..256
//    entries, no more than the bit depth can index for a palette image,
//    none in a greyscale image, required for a palette image;
//  - tRNS at most once, before IDAT, of the length its colour type takes,
//    none with an alpha channel;
//  - no other critical chunk; the IDAT chunks consecutive; IEND, empty,
//    after them (what follows IEND is not read, by libpng either); at
//    most 16 zTXt, iTXt and iCCP chunks in all (below);
//  - the zlib stream of the IDATs (RFC 1950/1951, inflated here, not by
//    zlib) valid as zlib would take it -- header check, window size,
//    no preset dictionary, block types, stored lengths, Huffman code
//    sets (zlib's rule for incomplete ones), symbols, back-references
//    within the output so far and the stream's window -- inflating to
//    exactly the bytes the image needs (Adam7 passes included), with
//    every row's filter byte 0..4 and the Adler-32 matching, in no more
//    deflate blocks than below.
// A refused image is simply no artwork.  Anything else image-like (JPEG,
// GIF, WebP) goes to JUCE as before, neither checked nor bounded here:
// loadUntrustedImage().
//
// What one PNG may cost is bounded too (Limits; libpng accepts more), for
// JUCE's decode cannot be cut short:
//  - the pixel data: the file at most 80 MiB, a side at most 8192
//    pixels, at most 16 Mi pixels (4096 x 4096) and 64 MiB of pixel data
//    (4096 x 4096 RGBA with 8-bit channels; 2896 x 2896 with 16-bit
//    ones), checked at IHDR before anything is inflated.  An 8192 x 8192
//    RGBA photograph (268 MB of it) took 5.3 s to check here and 2.3 s in
//    JUCE's decoder (#26, VERIFY-DB-2 gbench), and a thread stopped with
//    a time limit (StageLinQDbClient::stop) was killed in such a lookup.
//  - what libpng does whatever the image's size: it builds Huffman tables
//    again for every dynamic deflate block, and inflates each zTXt, iTXt
//    and iCCP chunk (up to 8 MB each, 1,000 chunks).  A 1 x 1 image whose
//    zlib stream held 7.4 M empty dynamic blocks (83 MB) took 10 s in
//    JUCE's decoder, one with 1,000 zTXt chunks of 8 MB of zeros (a
//    7.8 MB file) 16 s (VERIFY-DB-3, REPAIR-DB-3).  So the stream may
//    have at most 1024 dynamic blocks plus one a row and one per 1024
//    bytes of the rows, and at most 4096 blocks in all plus two a row and
//    one per 64 bytes (zlib ends a block every 16,384 symbols at its
//    default memLevel, every 128 at its smallest, and a flush per row
//    adds two), and the file at most 16 zTXt, iTXt and iCCP chunks.
// In JUCE's decoder here (-O1) a dynamic block cost 1.3 us (minimal
// tables) to 4.4 us (full 9-bit tables), a fixed or stored one about
// 10 ns, and a zTXt chunk of 8 MB 16 ms; at 4096 x 4096 RGBA the limits
// allow 70,660 dynamic blocks, so these costs come to about 0.3 s for
// the blocks and 0.26 s for the chunks, on top of the pixel data's own
// decode (0.58 s for a 4096 x 4096 RGBA photograph).  That is an estimate
// from those costs, not a proven worst case.
// The Huffman codes are decoded through a table (a bit at a time only
// for codes over 9 bits), and the caller can cut the check short
// (cancelled(), asked at every chunk, every deflate block and every 64 KiB
// inflated): the image is then refused.
//
// Thread: any; no state is shared between calls.
#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace ImageGuard
{
    // Artwork: a side of 1..8192 pixels (libpng's own limit is 1,000,000),
    // at most 16 Mi pixels and 64 MiB of pixel data, a file of 80 MiB
    static constexpr uint32_t kMaxArtworkSide      = 8192;
    static constexpr uint64_t kMaxArtworkPixels    = uint64_t(4096) * 4096;
    static constexpr uint64_t kMaxArtworkBytes     = uint64_t(64) << 20;
    static constexpr size_t   kMaxArtworkFileBytes = size_t(80) << 20;

    // Any PNG: the deflate blocks of its zlib stream (a base, plus so many
    // a row of all the passes, plus one per so many bytes of the rows,
    // filter bytes included), and the chunks libpng inflates (see the header)
    static constexpr uint64_t kMaxDynamicBlocksBase    = 1024;
    static constexpr uint64_t kDynamicBlocksPerRow     = 1;
    static constexpr uint64_t kRowBytesPerDynamicBlock = 1024;
    static constexpr uint64_t kMaxBlocksBase           = 4096;
    static constexpr uint64_t kBlocksPerRow            = 2;
    static constexpr uint64_t kRowBytesPerBlock        = 64;
    static constexpr int      kMaxInflatedTextChunks   = 16;   // zTXt, iTXt and iCCP

    struct Limits
    {
        uint32_t maxSide      = kMaxArtworkSide;
        uint64_t maxPixels    = kMaxArtworkPixels;     // width x height
        uint64_t maxBytes     = kMaxArtworkBytes;      // the rows' bytes, filter bytes not counted
        size_t   maxFileBytes = kMaxArtworkFileBytes;
    };

    // True when the caller wants the check given up (the image is refused)
    using Cancel = std::function<bool()>;

    namespace detail
    {
        inline uint32_t readU32(const uint8_t* p)
        {
            return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        }

        // CRC-32 as PNG defines it (ISO 3309, polynomial 0xEDB88320), eight
        // bytes a step ("slicing by 8": t[k] is the CRC of a byte followed
        // by k zero bytes)
        inline uint32_t crc32(const uint8_t* p, size_t n)
        {
            struct Table
            {
                uint32_t t[8][256];
                Table()
                {
                    for (uint32_t i = 0; i < 256; ++i)
                    {
                        uint32_t c = i;
                        for (int k = 0; k < 8; ++k)
                            c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                        t[0][i] = c;
                    }
                    for (int k = 1; k < 8; ++k)
                        for (uint32_t i = 0; i < 256; ++i)
                            t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFFu];
                }
            };
            static const Table table;
            const auto& t = table.t;
            uint32_t c = 0xFFFFFFFFu;
            size_t i = 0;
            for (; i + 8 <= n; i += 8)
            {
                const uint32_t lo = c ^ (uint32_t(p[i]) | (uint32_t(p[i + 1]) << 8)
                                         | (uint32_t(p[i + 2]) << 16) | (uint32_t(p[i + 3]) << 24));
                c = t[7][lo & 0xFFu] ^ t[6][(lo >> 8) & 0xFFu] ^ t[5][(lo >> 16) & 0xFFu] ^ t[4][lo >> 24]
                  ^ t[3][p[i + 4]] ^ t[2][p[i + 5]] ^ t[1][p[i + 6]] ^ t[0][p[i + 7]];
            }
            for (; i < n; ++i)
                c = t[0][(c ^ p[i]) & 0xFFu] ^ (c >> 8);
            return c ^ 0xFFFFFFFFu;
        }

        // The rows of one interlace pass (one pass when not interlaced)
        struct Pass { uint32_t rows = 0; uint64_t rowBytes = 0; };

        // The rows libpng reads: one pass, or Adam7's seven
        inline std::vector<Pass> rowPasses(uint32_t width, uint32_t height, int depth, int colourType, int interlace)
        {
            static const uint32_t adam7[7][4] = { { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 },
                                                  { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 } };
            const int channels = colourType == 2 ? 3 : colourType == 4 ? 2 : colourType == 6 ? 4 : 1;
            const uint64_t bitsPerPixel = uint64_t(channels) * uint64_t(depth);
            std::vector<Pass> passes;
            if (interlace == 0)
            {
                passes.push_back({ height, (uint64_t(width) * bitsPerPixel + 7) / 8 });
            }
            else
            {
                for (auto& a : adam7)
                {
                    const uint32_t w = width > a[0] ? (width - a[0] + a[2] - 1) / a[2] : 0;
                    const uint32_t h = height > a[1] ? (height - a[1] + a[3] - 1) / a[3] : 0;
                    passes.push_back({ w == 0 ? 0 : h, (uint64_t(w) * bitsPerPixel + 7) / 8 });
                }
            }
            return passes;
        }

        // Receives the inflated bytes, a run at a time: counts them against
        // what the image needs, checks each row's filter byte, keeps the
        // Adler-32
        struct RowSink
        {
            const std::vector<Pass>& passes;
            uint64_t expected = 0, total = 0;
            size_t pass = 0;
            uint32_t row = 0;
            uint64_t inRow = 0;                // bytes of the row so far; at 0 the next is its filter byte
            uint32_t s1 = 1, s2 = 0;           // Adler-32
            uint32_t pending = 0;              // bytes since the last modulo

            explicit RowSink(const std::vector<Pass>& p) : passes(p)
            {
                for (auto& ps : passes) expected += uint64_t(ps.rows) * (ps.rowBytes + 1);
                while (pass < passes.size() && passes[pass].rows == 0) ++pass;
            }

            bool put(const uint8_t* p, size_t n)
            {
                if (uint64_t(n) > expected - total) return false;   // more data than the image holds
                for (size_t i = 0; i < n;)
                {
                    const size_t k = std::min(n - i, size_t(5552 - pending));
                    for (size_t j = 0; j < k; ++j) { s1 += p[i + j]; s2 += s1; }
                    i += k;
                    pending += uint32_t(k);
                    if (pending == 5552) { s1 %= 65521u; s2 %= 65521u; pending = 0; }
                }
                while (n > 0)
                {
                    if (inRow == 0 && *p > 4) return false;           // libpng: "bad adaptive filter value"
                    const uint64_t rowLeft = passes[pass].rowBytes + 1 - inRow;
                    const size_t k = rowLeft < uint64_t(n) ? size_t(rowLeft) : n;
                    p += k;
                    n -= k;
                    total += k;
                    inRow += k;
                    if (inRow > passes[pass].rowBytes)
                    {
                        inRow = 0;
                        if (++row >= passes[pass].rows)
                        {
                            row = 0;
                            ++pass;
                            while (pass < passes.size() && passes[pass].rows == 0) ++pass;
                        }
                    }
                }
                return true;
            }

            uint32_t adler() { s1 %= 65521u; s2 %= 65521u; pending = 0; return (s2 << 16) | s1; }
        };

        // Codes of up to this many bits are decoded by a table lookup
        static constexpr int kFastBits = 9;

        // Canonical Huffman code (RFC 1951 3.2.2)
        struct Huffman
        {
            uint16_t count[16] = {};
            uint16_t symbol[288] = {};
            int maxLength = 0;
            // By the next kFastBits bits of the stream (its first bit lowest):
            // (length << 9) | symbol of the code of up to kFastBits bits they
            // start with; 0 where none does (decoded a bit at a time)
            uint16_t fast[1 << kFastBits] = {};
        };

        // Builds h from n code lengths.  zlib's rule (inftrees.c): an
        // over-subscribed set is refused; an incomplete one only for a
        // literal/length or distance code that is a single code of length
        // 1 (codeLengthCode: no incomplete set at all, and not empty).
        inline bool buildHuffman(Huffman& h, const uint8_t* lengths, int n, bool codeLengthCode)
        {
            for (auto& c : h.count) c = 0;
            for (auto& f : h.fast) f = 0;
            for (int i = 0; i < n; ++i) ++h.count[lengths[i]];
            h.maxLength = 0;
            for (int len = 15; len >= 1; --len) if (h.count[len] != 0) { h.maxLength = len; break; }
            if (h.maxLength == 0)
                return ! codeLengthCode;              // no codes: any use of one fails when decoded
            int left = 1;
            for (int len = 1; len <= 15; ++len)
            {
                left <<= 1;
                left -= h.count[len];
                if (left < 0) return false;           // over-subscribed
            }
            if (left > 0 && (codeLengthCode || h.maxLength != 1))
                return false;                         // incomplete
            uint16_t offs[16] = {};
            for (int len = 1; len < 15; ++len) offs[len + 1] = uint16_t(offs[len] + h.count[len]);
            for (int i = 0; i < n; ++i)
                if (lengths[i] != 0) h.symbol[offs[lengths[i]]++] = uint16_t(i);

            // The table: the codes of each length are consecutive, in symbol
            // order, after the last one of the length before, doubled; the
            // stream carries a code's highest bit first
            int code = 0, index = 0;
            for (int len = 1; len <= kFastBits; ++len, code <<= 1)
            {
                for (int k = 0; k < h.count[len]; ++k, ++code, ++index)
                {
                    int reversed = 0;
                    for (int b = 0; b < len; ++b)
                        reversed |= ((code >> b) & 1) << (len - 1 - b);
                    const auto entry = uint16_t((len << 9) | h.symbol[index]);
                    for (int at = reversed; at < (1 << kFastBits); at += 1 << len)
                        h.fast[at] = entry;
                }
            }
            return true;
        }

        // Reads the stream's bits, the lowest of each byte first (RFC 1951
        // 3.1.1), whole bytes ahead into buf
        struct BitReader
        {
            const uint8_t* in = nullptr;
            size_t n = 0, pos = 0;
            uint64_t buf = 0;
            int count = 0;

            // At least 57 bits in buf, or what the input has left
            void fill()
            {
                while (count <= 56 && pos < n)
                {
                    buf |= uint64_t(in[pos++]) << count;
                    count += 8;
                }
            }

            bool bits(int need, uint32_t& value)
            {
                if (count < need) fill();
                if (count < need) return false;
                value = uint32_t(buf & ((uint64_t(1) << need) - 1));
                buf >>= need;
                count -= need;
                return true;
            }

            // The rest of the current byte is skipped; the whole bytes read
            // ahead go back to the input
            void toByteBoundary()
            {
                pos -= size_t(count >> 3);
                buf = 0;
                count = 0;
            }

            // A symbol of h: by the table, or a bit at a time for a longer
            // code (or one the input has too few bits left for)
            int decode(const Huffman& h)
            {
                if (count < kFastBits) fill();
                if (count >= kFastBits)
                {
                    const uint16_t entry = h.fast[buf & ((uint64_t(1) << kFastBits) - 1)];
                    if (entry != 0)
                    {
                        const int len = entry >> 9;
                        buf >>= len;
                        count -= len;
                        return entry & 0x1FF;
                    }
                }
                int code = 0, first = 0, index = 0;
                for (int len = 1; len <= 15; ++len)
                {
                    uint32_t b;
                    if (! bits(1, b)) return -1;
                    code |= int(b);
                    const int cnt = h.count[len];
                    if (code - cnt < first)
                        return h.symbol[index + (code - first)];
                    index += cnt;
                    first += cnt;
                    first <<= 1;
                    code <<= 1;
                }
                return -1;                            // not a code of this set
            }
        };

        class Inflater
        {
        public:
            // maxBlocks, maxDynamicBlocks: the most deflate blocks the stream
            // may have, in all and of the dynamic type (a refusal past them)
            Inflater(const uint8_t* data, size_t size, RowSink& s, const Cancel& c,
                     uint64_t maxBlocks, uint64_t maxDynamicBlocks)
                : sink(s), cancelled(c), out(kWindow + kChunk),
                  blocksLeft(maxBlocks), dynamicBlocksLeft(maxDynamicBlocks)
            {
                br.in = data;
                br.n = size;
            }

            bool run()
            {
                // zlib header (RFC 1950): deflate, window <= 32 KiB, check bits, no dictionary
                if (br.n < 2) return false;
                const uint8_t cmf = br.in[0], flg = br.in[1];
                if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7) return false;
                if (((uint32_t(cmf) << 8) | flg) % 31 != 0) return false;
                if ((flg & 0x20) != 0) return false;
                window = uint32_t(1) << ((cmf >> 4) + 8);
                br.pos = 2;

                for (bool last = false; ! last;)
                {
                    if (cancelled && cancelled()) return false;
                    uint32_t v;
                    if (! br.bits(1, v)) return false;
                    last = v != 0;
                    if (! br.bits(2, v)) return false;
                    if (blocksLeft == 0 || (v == 2 && dynamicBlocksLeft == 0))
                        return false;                // more blocks than an encoder makes (see the header)
                    --blocksLeft;
                    if (v == 2) --dynamicBlocksLeft;
                    bool ok = false;
                    switch (v)
                    {
                        case 0:  ok = stored(); break;
                        case 1:  ok = fixedBlock(); break;
                        case 2:  ok = dynamicBlock(); break;
                        default: ok = false; break;  // "invalid block type"
                    }
                    if (! ok) return false;
                }
                if (! flush()) return false;

                // Adler-32, big-endian, after the last block's byte
                br.toByteBoundary();
                if (br.pos + 4 > br.n) return false;
                const uint32_t want = readU32(br.in + br.pos);
                br.pos += 4;
                return sink.total == sink.expected && sink.adler() == want;
            }

        private:
            // The output kept: the last 32 KiB (as far back as a distance
            // reaches) and up to kChunk bytes not handed to the sink yet
            static constexpr size_t kWindow = 32768, kChunk = 65536;

            // Hands what was inflated since the last call to the sink, and
            // keeps the window
            bool flush()
            {
                if (! sink.put(out.data() + fed, outPos - fed)) return false;
                if (cancelled && cancelled()) return false;
                if (outPos > kWindow)
                {
                    std::memmove(out.data(), out.data() + (outPos - kWindow), kWindow);
                    outPos = kWindow;
                }
                fed = outPos;
                return true;
            }

            bool stored()
            {
                br.toByteBoundary();
                if (br.pos + 4 > br.n) return false;
                const uint8_t* p = br.in + br.pos;
                const uint32_t len  = uint32_t(p[0]) | (uint32_t(p[1]) << 8);
                const uint32_t nlen = uint32_t(p[2]) | (uint32_t(p[3]) << 8);
                br.pos += 4;
                if (len != (~nlen & 0xFFFFu)) return false;   // "invalid stored block lengths"
                if (len > br.n - br.pos) return false;
                for (size_t done = 0; done < len;)
                {
                    if (outPos == out.size() && ! flush()) return false;
                    const size_t k = std::min(size_t(len) - done, out.size() - outPos);
                    std::memcpy(out.data() + outPos, br.in + br.pos + done, k);
                    outPos += k;
                    outCount += k;
                    done += k;
                }
                br.pos += len;
                return true;
            }

            // A block's symbols.  The loop works on copies of the bit reader
            // and the output position (a byte stored through a pointer could
            // be any member, so they would be read back from memory after
            // each one), written back before anything else uses them.
            bool codes(const Huffman& lit, const Huffman& dist)
            {
                static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
                static const uint8_t  lext[29]  = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                                    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
                static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                                    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                                    8193, 12289, 16385, 24577 };
                static const uint8_t  dext[30]  = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                                    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
                BitReader r = br;
                uint8_t* const o = out.data();       // flush() moves bytes within out, never out itself
                const size_t size = out.size();
                const uint32_t reach = window;
                size_t op = outPos;
                uint64_t inflated = outCount;
                auto writeBack = [&] { br = r; outPos = op; outCount = inflated; };
                auto flushHere = [&]
                {
                    writeBack();
                    const bool ok = flush();
                    op = outPos;
                    return ok;
                };
                for (;;)
                {
                    const int sym = r.decode(lit);
                    if (sym < 0) { writeBack(); return false; }           // "invalid literal/length code"
                    if (sym < 256)
                    {
                        if (op == size && ! flushHere()) return false;
                        o[op++] = uint8_t(sym);
                        ++inflated;
                        continue;
                    }
                    if (sym == 256) { writeBack(); return true; }         // end of block
                    if (sym > 285) { writeBack(); return false; }         // 286, 287: "invalid literal/length code"
                    uint32_t extra;
                    if (! r.bits(lext[sym - 257], extra)) { writeBack(); return false; }
                    const uint32_t len = lbase[sym - 257] + extra;
                    const int dsym = r.decode(dist);
                    if (dsym < 0 || dsym > 29) { writeBack(); return false; }   // "invalid distance code"
                    if (! r.bits(dext[dsym], extra)) { writeBack(); return false; }
                    const uint32_t d = dbase[dsym] + extra;
                    if (d > inflated || d > reach) { writeBack(); return false; }  // "invalid distance too far back"
                    // After a flush op is at most kWindow: kChunk (>= 258)
                    // bytes of room, and d <= op (all the output, or kWindow)
                    if (size - op < len && ! flushHere()) return false;
                    uint8_t* dst = o + op;
                    const uint8_t* src = dst - d;
                    for (uint32_t i = 0; i < len; ++i)
                        dst[i] = src[i];                  // byte by byte: d < len repeats
                    op += len;
                    inflated += len;
                }
            }

            bool fixedBlock()
            {
                static const struct Fixed
                {
                    Huffman lit, dist;
                    Fixed()
                    {
                        uint8_t l[288];
                        int i = 0;
                        for (; i < 144; ++i) l[i] = 8;
                        for (; i < 256; ++i) l[i] = 9;
                        for (; i < 280; ++i) l[i] = 7;
                        for (; i < 288; ++i) l[i] = 8;
                        buildHuffman(lit, l, 288, false);
                        uint8_t d[32];
                        for (auto& x : d) x = 5;
                        buildHuffman(dist, d, 32, false);
                    }
                } fixed;
                return codes(fixed.lit, fixed.dist);
            }

            bool dynamicBlock()
            {
                static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
                uint32_t hlit, hdist, hclen;
                if (! br.bits(5, hlit) || ! br.bits(5, hdist) || ! br.bits(4, hclen)) return false;
                const int nlen = int(hlit) + 257, ndist = int(hdist) + 1, ncode = int(hclen) + 4;
                if (nlen > 286 || ndist > 30) return false;   // "too many length or distance symbols"

                uint8_t lengths[320] = {};
                for (int i = 0; i < ncode; ++i)
                {
                    uint32_t v;
                    if (! br.bits(3, v)) return false;
                    lengths[order[i]] = uint8_t(v);
                }
                Huffman lencode;
                if (! buildHuffman(lencode, lengths, 19, true)) return false;   // "invalid code lengths set"

                for (int i = 0; i < 320; ++i) lengths[i] = 0;
                for (int index = 0; index < nlen + ndist;)
                {
                    const int sym = br.decode(lencode);
                    if (sym < 0) return false;
                    if (sym < 16) { lengths[index++] = uint8_t(sym); continue; }
                    uint8_t len = 0;
                    uint32_t rep;
                    if (sym == 16)
                    {
                        if (index == 0) return false;             // "invalid bit length repeat"
                        len = lengths[index - 1];
                        if (! br.bits(2, rep)) return false;
                        rep += 3;
                    }
                    else if (sym == 17)
                    {
                        if (! br.bits(3, rep)) return false;
                        rep += 3;
                    }
                    else
                    {
                        if (! br.bits(7, rep)) return false;
                        rep += 11;
                    }
                    if (index + int(rep) > nlen + ndist) return false;   // "invalid bit length repeat"
                    while (rep-- > 0) lengths[index++] = len;
                }
                if (lengths[256] == 0) return false;      // "invalid code -- missing end-of-block"

                Huffman lit, dist;
                if (! buildHuffman(lit, lengths, nlen, false)) return false;            // "invalid literal/lengths set"
                if (! buildHuffman(dist, lengths + nlen, ndist, false)) return false;   // "invalid distances set"
                return codes(lit, dist);
            }

            BitReader br;
            uint32_t window = 32768;
            RowSink& sink;
            const Cancel& cancelled;
            std::vector<uint8_t> out;
            size_t outPos = 0, fed = 0;        // out: next byte; first byte not handed to the sink
            uint64_t outCount = 0;             // bytes inflated so far
            uint64_t blocksLeft, dynamicBlocksLeft;
        };
    }

    //==========================================================================
    // True when the bytes are a PNG that libpng reads without an error, and
    // within limits (see the header): a refused image must not reach JUCE's
    // PNG reader.  cancelled, when given, can end the check (a refusal).
    //==========================================================================
    inline bool isSafePng(const void* data, size_t size, const Limits& limits = {}, const Cancel& cancelled = {})
    {
        using namespace detail;
        static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        const auto* d = static_cast<const uint8_t*>(data);
        if (d == nullptr || size < 8 + 25 + 12 + 12 || size > limits.maxFileBytes
            || std::memcmp(d, signature, 8) != 0)
            return false;

        uint32_t width = 0, height = 0;
        int depth = 0, colourType = -1, interlace = 0;
        uint32_t paletteEntries = 0;
        int inflatedTextChunks = 0;
        bool sawPLTE = false, sawTRNS = false, sawBKGDorHIST = false;
        bool sawIDAT = false, idatEnded = false, sawIEND = false;
        std::vector<Pass> passes;
        std::vector<uint8_t> z;

        size_t pos = 8;
        for (bool first = true; pos + 12 <= size; first = false)
        {
            if (cancelled && cancelled()) return false;
            const uint32_t len = readU32(d + pos);
            if (len > 0x7FFFFFFFu || len > size - pos - 12) return false;   // 31-bit length, inside the data
            const uint8_t* type = d + pos + 4;
            const uint8_t* body = d + pos + 8;
            for (int i = 0; i < 4; ++i)
            {
                const uint8_t c = type[i];
                if (! ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
            }
            if ((type[2] & 0x20) != 0) return false;                         // reserved bit
            if (readU32(body + len) != crc32(type, size_t(len) + 4)) return false;
            const bool critical = (type[0] & 0x20) == 0;
            auto is = [type](const char* t) { return std::memcmp(type, t, 4) == 0; };

            if (first != is("IHDR")) return false;                           // IHDR first, and only there
            if (sawIDAT && ! is("IDAT")) idatEnded = true;

            if (is("IHDR"))
            {
                if (len != 13) return false;
                width = readU32(body);
                height = readU32(body + 4);
                depth = body[8];
                colourType = body[9];
                interlace = body[12];
                if (width == 0 || height == 0 || width > limits.maxSide || height > limits.maxSide)
                    return false;
                bool depthOk = false;
                switch (colourType)
                {
                    case 0:  depthOk = depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16; break;
                    case 3:  depthOk = depth == 1 || depth == 2 || depth == 4 || depth == 8; break;
                    case 2: case 4: case 6: depthOk = depth == 8 || depth == 16; break;
                    default: depthOk = false; break;
                }
                if (! depthOk || body[10] != 0 || body[11] != 0 || interlace > 1) return false;

                // The budget, before anything is inflated
                passes = rowPasses(width, height, depth, colourType, interlace);
                uint64_t pixelBytes = 0;
                for (auto& ps : passes) pixelBytes += uint64_t(ps.rows) * ps.rowBytes;
                if (uint64_t(width) * height > limits.maxPixels || pixelBytes > limits.maxBytes)
                    return false;
            }
            else if (is("PLTE"))
            {
                if (sawPLTE || sawIDAT || sawTRNS || sawBKGDorHIST) return false;
                if (colourType == 0 || colourType == 4) return false;
                if (len == 0 || len % 3 != 0 || len > 3 * 256) return false;
                paletteEntries = len / 3;
                if (colourType == 3 && paletteEntries > (1u << depth)) return false;
                sawPLTE = true;
            }
            else if (is("IDAT"))
            {
                if (idatEnded) return false;                                 // IDATs must be consecutive
                if (colourType == 3 && ! sawPLTE) return false;
                z.insert(z.end(), body, body + len);
                sawIDAT = true;
            }
            else if (is("IEND"))
            {
                if (len != 0 || ! sawIDAT) return false;
                sawIEND = true;
                break;                                                       // nothing after it is read
            }
            else if (critical)
            {
                return false;                                                // unknown critical chunk
            }
            else if (is("tRNS"))
            {
                if (sawTRNS || sawIDAT) return false;
                switch (colourType)
                {
                    case 0:  if (len != 2) return false; break;
                    case 2:  if (len != 6) return false; break;
                    case 3:  if (! sawPLTE || len == 0 || len > paletteEntries) return false; break;
                    default: return false;                                   // alpha channel already
                }
                sawTRNS = true;
            }
            else if (is("bKGD") || is("hIST"))
            {
                if (sawIDAT) return false;
                sawBKGDorHIST = true;
            }
            else if (is("zTXt") || is("iTXt") || is("iCCP"))
            {
                if (++inflatedTextChunks > kMaxInflatedTextChunks)
                    return false;                                            // libpng inflates each one
            }
            pos += size_t(len) + 12;
        }
        if (! sawIEND) return false;

        RowSink sink(passes);
        uint64_t rows = 0;
        for (auto& ps : passes) rows += ps.rows;
        const uint64_t maxBlocks = kMaxBlocksBase + kBlocksPerRow * rows + sink.expected / kRowBytesPerBlock;
        const uint64_t maxDynamicBlocks = kMaxDynamicBlocksBase + kDynamicBlocksPerRow * rows
                                        + sink.expected / kRowBytesPerDynamicBlock;
        Inflater inflater(z.data(), z.size(), sink, cancelled, maxBlocks, maxDynamicBlocks);
        return inflater.run();
    }

    // True when JUCE's ImageFileFormat::findImageFormatForStream would take
    // the bytes for a PNG: PNGImageFormat::canUnderstand looks at bytes 1-3
    inline bool looksLikePng(const void* data, size_t size)
    {
        const auto* d = static_cast<const uint8_t*>(data);
        return d != nullptr && size >= 4 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G';
    }

    //==========================================================================
    // Decodes image bytes that came from a device, the network or a device's
    // database, as juce::ImageFileFormat::loadFrom does (the first of JUCE's
    // formats that understands them), except that a PNG must pass
    // isSafePng() first (with these limits; cancelled can end that check).
    // Returns an invalid Image for anything refused.
    //==========================================================================
    inline juce::Image loadUntrustedImage(const void* data, size_t size, const Limits& limits = {},
                                          const Cancel& cancelled = {})
    {
        if (data == nullptr || size == 0) return {};
        juce::MemoryInputStream stream(data, size, false);
        auto* format = juce::ImageFileFormat::findImageFormatForStream(stream);
        if (format == nullptr) return {};
        if ((dynamic_cast<juce::PNGImageFormat*>(format) != nullptr || looksLikePng(data, size))
            && ! isSafePng(data, size, limits, cancelled))
            return {};
        stream.setPosition(0);
        return format->decodeImage(stream);
    }
}
