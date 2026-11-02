#pragma once

#include "../JuceHeader.h"

// ARA model-graph and render diagnostics, compiled out unless the build is
// configured with -DPITCHNET_ARA_DIAGNOSTICS=ON.
//
// Routes through the project's existing AppLogger so every event lands in one
// chronological file:
//   %APPDATA%/Kinu Tune/Logs/debug_<session>.log
//
// AppLogger reopens the file on every call, so this is NOT realtime-safe.
// Model-graph events are low frequency by nature. Render-path reporting must
// only fire when an outcome CHANGES, never per block.

#include "../Utils/AppLogger.h"

#if PITCHNET_ARA_DIAGNOSTICS && PITCHNET_LOGGING_ENABLED

#define ARA_DIAG(msg) AppLogger::log(juce::String("[ARA] ") + (msg))
#define ARA_DIAG_PTR(p)                                                        \
  juce::String::toHexString((juce::int64)(juce::pointer_sized_int)(p))
#define ARA_DIAG_TID()                                                         \
  juce::String::toHexString(                                                   \
      (juce::int64)(juce::pointer_sized_int)juce::Thread::getCurrentThreadId())

// Cheap content fingerprint of an audio buffer.
//
// A blob's dimensions never change on an edit - the project spans the whole
// modification, so every republish is the same length - which makes a content
// change invisible to anything tracking sizes. This gives the publish sites a
// comparable value, so a blob can be followed from where it is written to
// where it is served.
//
// Must be identical at every call site or the comparison means nothing, hence
// one definition here. Samples a fixed number of taps rather than the whole
// buffer, so it stays O(1) in the blob length.
inline float araDiagFingerprint(const juce::AudioBuffer<float> &buffer) {
  const int n = buffer.getNumSamples();
  if (n <= 0 || buffer.getNumChannels() <= 0)
    return 0.0f;
  const float *p = buffer.getReadPointer(0);
  constexpr int kTaps = 16;
  float sum = 0.0f;
  for (int i = 0; i < kTaps; ++i) {
    const int idx =
        n == 1 ? 0
               : static_cast<int>(static_cast<juce::int64>(i) * (n - 1) /
                                  (kTaps - 1));
    sum += p[idx] * static_cast<float>(i + 1);
  }
  return sum;
}

// D-134: per-block render trace.
//
// Writing one log line per block reopens the log file ~100 times per second of
// render, which is slow enough to change the timing of the very start-up race
// this trace exists to measure (the first diagnostic build made the cold render
// audible). Records are therefore kept in a fixed array and written in one
// batch: when the block time goes backwards (a new render or playback starts),
// or when the array fills. Nothing is written while a render is running, so the
// audio path only pays a store per block.
struct AraBlockTrace {
  juce::int64 t = 0;
  int n = 0;
  // 0 = renderProcessedRegions ran; 1..4 = early exits of the renderer's
  // processBlock; 5 = the processor's ARA branch did not handle the block.
  int exitCode = 0;
  int bypassRaw = 0;
  int regions = 0;
  int intersecting = 0;
  int processedRendered = 0;
  int rawAttempted = 0;
  int rawHadResources = 0;
  int renderedAny = 0;
  int nonZero = 0;
  int bound = -1; // only for exitCode 5: isBoundToARA()
  juce::int64 thread = 0;
};

inline constexpr int kAraBlockTraceCapacity = 4096;
inline AraBlockTrace araBlockTrace[kAraBlockTraceCapacity];
inline int araBlockTraceCount = 0;
inline juce::int64 araBlockTraceLastT = -1;

inline void araBlockTraceFlush() {
  if (araBlockTraceCount == 0)
    return;
  juce::String out;
  out.preallocateBytes(araBlockTraceCount * 130);
  for (int i = 0; i < araBlockTraceCount; ++i) {
    const auto &r = araBlockTrace[i];
    out << "blk t=" << r.t << " n=" << r.n << " exit=" << r.exitCode
        << " bypassRaw=" << r.bypassRaw << " regions=" << r.regions
        << " intersecting=" << r.intersecting
        << " processedRendered=" << r.processedRendered
        << " rawAttempted=" << r.rawAttempted
        << " rawHadResources=" << r.rawHadResources
        << " renderedAny=" << r.renderedAny << " nonZero=" << r.nonZero
        << " bound=" << r.bound
        << " thread=" << juce::String::toHexString(r.thread) << "\n";
  }
  ARA_DIAG(out);
  araBlockTraceCount = 0;
}

inline void araBlockTracePush(const AraBlockTrace &r) {
  if (araBlockTraceCount > 0 && r.t < araBlockTraceLastT)
    araBlockTraceFlush();
  if (araBlockTraceCount >= kAraBlockTraceCapacity)
    araBlockTraceFlush();
  araBlockTrace[araBlockTraceCount++] = r;
  araBlockTraceLastT = r.t;
}

inline juce::int64 araDiagThreadId() {
  return static_cast<juce::int64>(
      reinterpret_cast<juce::pointer_sized_int>(juce::Thread::getCurrentThreadId()));
}

inline void araBlockTracePushExit(int exitCode, juce::int64 t, int n,
                                  juce::int64 thread) {
  AraBlockTrace record;
  record.t = t;
  record.n = n;
  record.exitCode = exitCode;
  record.thread = thread;
  araBlockTracePush(record);
}

#else

#define ARA_DIAG(msg) ((void)0)
#define ARA_DIAG_PTR(p) juce::String()
#define ARA_DIAG_TID() juce::String()

#endif
