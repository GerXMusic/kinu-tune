#pragma once

// D-144: identity of the FX instance per track, keyed by the host's region
// sequence reference (never by a controller pointer nor by a scan maximum).
//
// The FX instance (the one that holds the host's bypass parameter) registers
// itself under the host reference of its own ARA region sequence. The playback
// renderer looks up its own track's reference and, when it resolves, reads the
// bypass intent from that instance instead of the process-wide static flag.

#include "../JuceHeader.h"

#include <vector>

class PitchNetAudioProcessor;

class PitchNetFxTrackRegistry {
public:
  static void registerTrack(juce::uint64 key,
                            PitchNetAudioProcessor *processor) {
    if (key == 0 || processor == nullptr)
      return;
    const juce::SpinLock::ScopedLockType lock(getLock());
    auto &entries = getEntries();
    for (auto &entry : entries) {
      if (entry.key == key) {
        entry.processor = processor;
        return;
      }
    }
    entries.push_back({key, processor});
  }

  static void unregisterProcessor(PitchNetAudioProcessor *processor) {
    if (processor == nullptr)
      return;
    const juce::SpinLock::ScopedLockType lock(getLock());
    auto &entries = getEntries();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [processor](const Entry &entry) {
                                   return entry.processor == processor;
                                 }),
                  entries.end());
  }

  // Audio-thread side: try-lock only, never blocks. Returns nullptr when the
  // key is unknown or the registry is busy; callers fall back to the static
  // flag in that case.
  static PitchNetAudioProcessor *findTrack(juce::uint64 key) {
    if (key == 0)
      return nullptr;
    const juce::SpinLock::ScopedTryLockType lock(getLock());
    if (!lock.isLocked())
      return nullptr;
    for (const auto &entry : getEntries())
      if (entry.key == key)
        return entry.processor;
    return nullptr;
  }

private:
  struct Entry {
    juce::uint64 key = 0;
    PitchNetAudioProcessor *processor = nullptr;
  };

  static juce::SpinLock &getLock() {
    static juce::SpinLock lock;
    return lock;
  }

  static std::vector<Entry> &getEntries() {
    static std::vector<Entry> entries;
    return entries;
  }
};
