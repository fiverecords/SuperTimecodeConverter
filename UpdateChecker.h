// Super Timecode Converter
// Copyright (c) 2026 Fiverecords — MIT License
// https://github.com/fiverecords/SuperTimecodeConverter

#pragma once
#include <JuceHeader.h>
#include <atomic>

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
        // Timeout must exceed the HTTP connection timeout (8000ms) to prevent
        // juce::Thread's destructor from waiting indefinitely if we're mid-request.
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

    // Only valid after hasResult() returns true — the acquire/release pair on
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
        // GitHub API: get latest release
        juce::URL url("https://api.github.com/repos/fiverecords/SuperTimecodeConverter/releases/latest");

        auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                           .withConnectionTimeoutMs(8000)
                           .withExtraHeaders("Accept: application/vnd.github+json\r\n"
                                             "User-Agent: SuperTimecodeConverter/" + currentVer + "\r\n");

        std::unique_ptr<juce::InputStream> stream = url.createInputStream(options);

        if (threadShouldExit())
            return;

        if (!stream)
        {
            checkFailed.store(true, std::memory_order_relaxed);
            resultReady.store(true, std::memory_order_release);
            return;
        }

        juce::String response = stream->readEntireStreamAsString();
        stream.reset();

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

        // Strip leading 'v' or 'V' from tag  (e.g. "v1.5" → "1.5")
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
    // Simple semantic version comparison: "1.5" > "1.4", "1.4.1" > "1.4"
    // Returns true if 'remote' is strictly newer than 'local'.
    //--------------------------------------------------------------------------
    static bool isNewer(const juce::String& remote, const juce::String& local)
    {
        auto splitVersion = [](const juce::String& v) -> juce::Array<int>
        {
            juce::Array<int> parts;
            juce::StringArray tokens;
            tokens.addTokens(v, ".", "");
            for (auto& t : tokens)
                parts.add(t.getIntValue());
            return parts;
        };

        auto r = splitVersion(remote);
        auto l = splitVersion(local);

        int count = juce::jmax(r.size(), l.size());
        for (int i = 0; i < count; i++)
        {
            int rv = (i < r.size()) ? r[i] : 0;
            int lv = (i < l.size()) ? l[i] : 0;
            if (rv > lv) return true;
            if (rv < lv) return false;
        }
        return false;   // equal
    }

    //--------------------------------------------------------------------------
    juce::String currentVer;

    // Results — written by background thread, read by UI thread
    std::atomic<bool> resultReady     { false };
    std::atomic<bool> updateAvailable { false };
    std::atomic<bool> checkFailed     { false };
    juce::String latestVer;
    juce::String releaseUrl;
    juce::String releaseNotes;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UpdateChecker)
};
