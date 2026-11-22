// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

//==============================================================================
// UpdateChecker
//
// Queries the GitHub Releases API on a background thread to check whether a
// newer version is available.  The result is stored in atomic/thread-safe
// members that the UI thread can poll during its timer callback.
//
// Usage:
//   UpdateChecker checker;
//   checker.checkAsync("1.4");          // current app version
//   ...
//   if (checker.hasResult()) {
//       if (checker.isUpdateAvailable())
//           // show link to checker.getLatestVersion() / getReleaseUrl()
//   }
//==============================================================================
class UpdateChecker : private juce::Thread
{
public:
    UpdateChecker() : juce::Thread("UpdateChecker") {}

    ~UpdateChecker() override
    {
        // The thread blocks inside the request (connect, then the read of
        // the answer) and looked at threadShouldExit() only between the
        // two, so a quit while the check was out waited for the network to
        // give up: 7 s with the window frozen for a server that accepts and
        // never answers, and after 10 s of an answer that stalls JUCE
        // killed the thread (AUD11 CONC-2).  The request is cancelled
        // first -- WebInputStream::cancel() ends a blocking connect or read
        // from another thread -- and the thread then leaves at once.  The
        // exit flag is raised before the lock is taken: run() either has
        // its stream registered by then, and it is cancelled, or finds the
        // flag when it comes to register one.
        signalThreadShouldExit();
        {
            const juce::ScopedLock lock(requestLock);
            if (request != nullptr)
                request->cancel();
        }
        // Still longer than the connection timeout (8000 ms), should a
        // platform's cancel() not end a request in progress.
        stopThread(10000);
    }

    //--------------------------------------------------------------------------
    // Trigger an async check.  Safe to call from the UI thread.
    // currentVersion: e.g. "1.4" (no leading 'v').
    //--------------------------------------------------------------------------
    void checkAsync(const juce::String& currentVersion)
    {
        if (isThreadRunning())
            return;     // already checking

        currentVer = currentVersion;
        resultReady.store(false, std::memory_order_relaxed);
        updateAvailable.store(false, std::memory_order_relaxed);
        checkFailed.store(false, std::memory_order_relaxed);
        latestVer  = {};
        releaseUrl = {};
        releaseNotes = {};

        startThread(juce::Thread::Priority::low);
    }

    //--------------------------------------------------------------------------
    // Poll from UI thread
    //--------------------------------------------------------------------------
    bool hasResult()          const { return resultReady.load(std::memory_order_acquire); }
    bool isUpdateAvailable()  const { return updateAvailable.load(std::memory_order_relaxed); }
    bool didCheckFail()       const { return checkFailed.load(std::memory_order_relaxed); }

    // Only valid after hasResult() returns true -- the acquire/release pair on
    // resultReady guarantees these non-atomic String members are fully visible.
    juce::String getLatestVersion() const { jassert(hasResult()); return latestVer; }
    juce::String getReleaseUrl()    const { jassert(hasResult()); return releaseUrl; }
    juce::String getReleaseNotes()  const { jassert(hasResult()); return releaseNotes; }

private:
    //--------------------------------------------------------------------------
    // Background thread
    //--------------------------------------------------------------------------
    void run() override
    {
        // GitHub API: get latest release.  /releases/latest is the newest
        // release not marked as a pre-release, so a beta is offered only if
        // it is published as a full release (none has been, to V1.9.13);
        // what a beta user is told of is the final release (AUDIT NET-5).
        juce::URL url("https://api.github.com/repos/fiverecords/SuperTimecodeConverter/releases/latest");

        auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                           .withConnectionTimeoutMs(8000)
                           .withExtraHeaders("Accept: application/vnd.github+json\r\n"
                                             "User-Agent: SuperTimecodeConverter/" + currentVer + "\r\n");

        // What url.createInputStream(options) does, step by step, so that
        // the destructor can reach the stream while connect() or the read
        // blocks (AUD11 CONC-2): the same request with the same headers,
        // timeout and redirects.
        auto stream = std::make_unique<juce::WebInputStream>(url, false);
        stream->withExtraHeaders(options.getExtraHeaders());
        stream->withConnectionTimeout(options.getConnectionTimeoutMs());
        stream->withNumRedirectsToFollow(options.getNumRedirectsToFollow());
        {
            const juce::ScopedLock lock(requestLock);
            if (threadShouldExit())
                return;
            request = stream.get();
        }
        // Whatever way run() leaves from here on, the stream is taken out of
        // the destructor's reach before it is destroyed.
        struct Unregister
        {
            UpdateChecker& owner;
            ~Unregister() { const juce::ScopedLock lock(owner.requestLock); owner.request = nullptr; }
        } unregister { *this };

        const bool connected = stream->connect(nullptr) && ! stream->isError();

        if (threadShouldExit())
            return;

        if (! connected)
        {
            checkFailed.store(true, std::memory_order_relaxed);
            resultReady.store(true, std::memory_order_release);
            return;
        }

        juce::String response = stream->readEntireStreamAsString();

        if (threadShouldExit())
            return;

        // Parse JSON
        auto json = juce::JSON::parse(response);
        if (!json.isObject())
        {
            checkFailed.store(true, std::memory_order_relaxed);
            resultReady.store(true, std::memory_order_release);
            return;
        }

        juce::String tagName = json.getProperty("tag_name", "").toString();
        juce::String htmlUrl = json.getProperty("html_url", "").toString();
        juce::String body    = json.getProperty("body", "").toString();

        if (tagName.isEmpty())
        {
            checkFailed.store(true, std::memory_order_relaxed);
            resultReady.store(true, std::memory_order_release);
            return;
        }

        // Strip leading 'v' or 'V' from tag  (e.g. "v1.5" -> "1.5")
        juce::String remoteVersion = tagName.trimCharactersAtStart("vV");

        latestVer    = remoteVersion;
        releaseUrl   = htmlUrl.isNotEmpty() ? htmlUrl
                       : "https://github.com/fiverecords/SuperTimecodeConverter/releases/latest";
        releaseNotes = body;

        updateAvailable.store(isNewer(remoteVersion, currentVer), std::memory_order_relaxed);
        checkFailed.store(false, std::memory_order_relaxed);
        resultReady.store(true, std::memory_order_release);
    }

    //--------------------------------------------------------------------------
    // Version precedence, Semantic Versioning 2.0.0 section 11.  The numbers
    // ("1.9.14") compare part by part as numbers, a missing part counting as
    // 0.  With equal numbers a pre-release ("-beta2") comes BEFORE the
    // release, and two pre-releases compare identifier by identifier (split
    // on dots): numeric ones as numbers, numeric before alphanumeric, a list
    // after its own prefix.  Inside an identifier that mixes letters and
    // digits, each run of digits compares as a number, so beta23 comes after
    // beta3 as STC's tags mean (plain ASCII order would put it first).  Build
    // metadata ("+...") is ignored.  Returns true if 'remote' is strictly
    // newer than 'local'.  It compared the numbers only, so 1.9.14-beta2
    // equalled 1.9.14 and a beta was never told of the release it led to,
    // and every beta of one version equalled the others (AUDIT NET-5).
    //--------------------------------------------------------------------------
    static bool isNewer(const juce::String& remote, const juce::String& local)
    {
        return compareVersions(remote.toStdString(), local.toStdString()) > 0;
    }

    static int compareVersions(const std::string& a, const std::string& b)
    {
        auto split = [](const std::string& v, std::string& core, std::string& pre)
        {
            const std::string noBuild = v.substr(0, v.find('+'));
            const auto dash = noBuild.find('-');
            core = noBuild.substr(0, dash);
            pre  = (dash == std::string::npos) ? std::string() : noBuild.substr(dash + 1);
        };
        std::string coreA, preA, coreB, preB;
        split(a, coreA, preA);
        split(b, coreB, preB);

        // The numbers: the leading digits of each part (as getIntValue read
        // them), compared as text so no length overflows.
        const auto na = splitOn(coreA, '.'), nb = splitOn(coreB, '.');
        for (size_t i = 0; i < std::max(na.size(), nb.size()); ++i)
        {
            const std::string x = i < na.size() ? leadingDigits(na[i]) : std::string();
            const std::string y = i < nb.size() ? leadingDigits(nb[i]) : std::string();
            if (const int c = compareDigits(x, y))
                return c;
        }

        // The pre-release: none is newer than any.
        if (preA.empty() || preB.empty())
            return preA.empty() == preB.empty() ? 0 : (preA.empty() ? 1 : -1);
        const auto ia = splitOn(preA, '.'), ib = splitOn(preB, '.');
        for (size_t i = 0; i < std::min(ia.size(), ib.size()); ++i)
            if (const int c = compareIdentifiers(ia[i], ib[i]))
                return c;
        return ia.size() == ib.size() ? 0 : (ia.size() > ib.size() ? 1 : -1);
    }

    static std::vector<std::string> splitOn(const std::string& s, char sep)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        for (;;)
        {
            const auto end = s.find(sep, start);
            parts.push_back(s.substr(start, end - start));
            if (end == std::string::npos)
                return parts;
            start = end + 1;
        }
    }

    static bool isDigit(char c) { return c >= '0' && c <= '9'; }

    static std::string leadingDigits(const std::string& s)
    {
        size_t n = 0;
        while (n < s.size() && isDigit(s[n])) ++n;
        return s.substr(0, n);
    }

    /// Two runs of digits compared as the numbers they write, any length.
    static int compareDigits(std::string x, std::string y)
    {
        auto dropLeadingZeros = [](std::string& d) { const auto nz = d.find_first_not_of('0'); d = (nz == std::string::npos) ? std::string() : d.substr(nz); };
        dropLeadingZeros(x);
        dropLeadingZeros(y);
        if (x.size() != y.size())
            return x.size() < y.size() ? -1 : 1;
        return x == y ? 0 : (x < y ? -1 : 1);
    }

    static int compareIdentifiers(const std::string& x, const std::string& y)
    {
        auto numeric = [](const std::string& s) { return ! s.empty() && s.find_first_not_of("0123456789") == std::string::npos; };
        if (numeric(x) != numeric(y))
            return numeric(x) ? -1 : 1;   // numeric identifiers come first (11.4.3)
        size_t i = 0, j = 0;
        while (i < x.size() && j < y.size())
        {
            if (isDigit(x[i]) && isDigit(y[j]))
            {
                size_t ie = i, je = j;
                while (ie < x.size() && isDigit(x[ie])) ++ie;
                while (je < y.size() && isDigit(y[je])) ++je;
                if (const int c = compareDigits(x.substr(i, ie - i), y.substr(j, je - j)))
                    return c;
                i = ie;
                j = je;
            }
            else
            {
                if (x[i] != y[j])
                    return (unsigned char) x[i] < (unsigned char) y[j] ? -1 : 1;
                ++i;
                ++j;
            }
        }
        if (i < x.size()) return 1;
        if (j < y.size()) return -1;
        return 0;
    }

    //--------------------------------------------------------------------------
    juce::String currentVer;

    // The request in progress, for the destructor to cancel (AUD11 CONC-2).
    // Set and cleared by run() under requestLock; never owned here.
    juce::CriticalSection requestLock;
    juce::WebInputStream* request = nullptr;

    // Results -- written by background thread, read by UI thread
    std::atomic<bool> resultReady     { false };
    std::atomic<bool> updateAvailable { false };
    std::atomic<bool> checkFailed     { false };
    juce::String latestVer;
    juce::String releaseUrl;
    juce::String releaseNotes;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UpdateChecker)
};
