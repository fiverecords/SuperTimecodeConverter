# Super Timecode Converter v1.9.3 -- Release Notes

**Release date:** TBD
**Built on:** v1.9.2

---

## New Feature — On-Air Gate

A new **ON-AIR ONLY** toggle in the output panel makes the engine only produce active timecode when the current CDJ is flagged on-air by the DJM mixer. This lets the DJ load, cue and preview tracks on a deck without triggering downstream timecode until they bring the deck in.

The on-air flag is computed by the DJM itself from the full mixer state (channel fader, cross-fader, EQ kill, mute), transmitted to each CDJ, and is what lights up the ON AIR indicator on the CDJ display. Using it here means STC follows exactly what the DJ sees — no threshold or channel mapping needed.

Only available when the input source is **Pro DJ Link**.

---

## New Feature — Sortable Track Map Columns

The Track Map Editor now has a sortable **#** column showing each track's position in the imported playlist order (or "--" for tracks not in any playlist). Plus **Artist**, **Title**, and **Offset** columns are clickable to sort — just like Finder / Explorer. Click a column header to sort ascending; click again to sort descending.

The default view sorts by playlist position ascending, so when you import a playlist the tracks appear in setlist order. To return to that view after sorting by any other column, click the **#** header.

Artist sort uses Title as tiebreak. BPM, Trig, Cues and Notes remain non-sortable.

---

## Bug Fix — Frame rate reset to 30 fps on every restart when using ProDJLink / StageLinQ

When using ProDJLink or StageLinQ as the input source, the configured frame rate (e.g. 25 fps) was silently overwritten with 30 fps every time STC started up, or when settings were restored from a backup.

The cause was a line in `startProDJLinkInput` / `startStageLinQInput` that set `currentFps = outputFps` on every start. Since `outputFps` defaults to 30 fps and is only updated when the user explicitly enables FPS Conversion, the engine's current frame rate was being reset to 30 every time the input source was started — which happens automatically at boot.

The offending line has been removed. `currentFps` now stays at the value loaded from settings (or the user's button selection).

---

## Bug Fix — Playlist import failed when folder names contain spaces

When importing a rekordbox XML playlist located inside a folder whose name contained a space, the import returned no tracks. The path parser used `juce::StringArray::addTokens` which treats its second argument as a set of separator **characters**, not as a substring — so " / " was interpreted as "separate on space OR slash", splitting "My Folder / Saturday" into four tokens instead of two.

Replaced the character-based split with a proper substring search using `indexOf(" / ")`. Playlists in folders with spaces in their names now import correctly.

---

## Bug Fix — Race condition in Pro DJ Link player invalidation

Fixed a latent race condition between the ProDJLink thread and the DbServerClient worker thread. When a CDJ disappeared from the network, the `onPlayerLost` callback (running on the ProDJLink thread) would close TCP sockets directly while the DbServerClient worker thread could be in the middle of a `socket->write()` using those same sockets — a classic use-after-free that could cause crashes when players drop off the network during active metadata queries.

Invalidation is now deferred: the ProDJLink thread queues the player IP in a lock-protected list, and the DbServerClient worker thread processes the queue between TCP queries (at the start of each worker loop iteration, max ~500ms latency). Since connection close and query execution now happen on the same thread, there is no race.

This was a rare bug — it required a CDJ to disconnect at the exact moment of an active TCP query — but when it did happen, it could crash STC. Most users would never have hit it, but it's the kind of bug that manifests during long shows with unstable network hardware.

---

## Bug Fix — Safer shutdown thread ordering

Reordered shutdown sequences (application exit, input source switching, and auto-stop of unused shared inputs) so the packet receiver threads (ProDJLink, StageLinQ) are joined **before** their corresponding database clients are stopped or their callbacks are cleared. Prevents a `std::function` race that could fire when a player disconnect coincided with a shutdown.

---

## Bug Fix — Track Map import preserves existing entries

When importing a rekordbox XML collection, the "Import Tracks" flow no longer overwrites existing Track Map entries. Previously, if a user manually selected a duplicate track in the Import Preview dialog, the import would reset that track's cues, triggers, offsets, and notes. Now existing entries are preserved — the import only adds new tracks. The summary count reflects only the actual additions.

This aligns with the "Apply Playlist Order" behavior introduced in v1.9.2: imports never destroy user configuration.

---

## Files Changed

- `TimecodeEngine.h` — On-air gate state + `isOnAirGateOpen()` helper + hook into `sourceActive`; removed `currentFps = outputFps` overwrite in `startProDJLinkInput` / `startStageLinQInput`
- `AppSettings.h` — On-air gate serialization; playlist path split fix (indexOf instead of addTokens)
- `DbServerClient.h` — Deferred invalidation via `pendingInvalidateIPs` queue
- `MainComponent.h` / `MainComponent.cpp` — On-air gate toggle in output panel; reordered shutdown sequences
- `TrackMapEditor.h` — Sortable Artist/Title/Offset columns; Import Preview preserves existing entries; corrected summary count
- `Main.cpp` — Version bump to 1.9.3
- `README.md` — Version bump
