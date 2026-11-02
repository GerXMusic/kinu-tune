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

#include <cstdlib>

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
  // D-136: reference measurements for this block (read at the same point as
  // bypassRaw). framesRatioNotOne counts the analysis frames of the block
  // window whose pitch ratio differs from 1 (-1 = not computable). maxDiffRaw
  // and maxDiffProcessed are the maximum sample-wise differences between the
  // block output and, respectively, the raw source and the processed audio
  // re-read for the SAME window (-1 = reference not available, e.g. the rates
  // differ and the comparison would not be exact). refFlags says which
  // references were exact: 1 = raw, 2 = processed.
  int framesRatioNotOne = -1;
  float maxDiffRaw = -1.0f;
  float maxDiffProcessed = -1.0f;
  int refFlags = 0;
  // D-137: the bypass parameter as the project has it right now (the INTENT),
  // as opposed to the static flag the renderer reads. -1 = not available.
  float bypassParam = -1.0f;
  // D-137: which thread wrote that intent last (-1 = none).
  juce::int64 bypassWriterThread = -1;
  // D-138: the last write to the static bypass flag before this block read it.
  int writeValue = -2;      // 1 = raw (bypass), 0 = processed, -2 = no write
  int writeSite = -1;       // 0..5, see D-138; -1 = none
  int writeInstance = -1;   // processor instance id; -1 = none
  int writeOffline = -1;    // 1 = the writing call was non-realtime
  juce::int64 writeThread = 0;
  juce::uint64 writeSeq = 0;   // number of writes when the record was taken
  double writeTimeMs = 0.0;
  double readTimeMs = 0.0;
  int renderEpoch = -1;     // D-138: counter of the render/playback this block belongs to
  // D-138: bit 0 = no write happened while the flag was read (attribution is
  // exact); bit 1 = the attributed write's value equals the value read.
  int writeFlags = 0;
  // D-138: the write before that one, for the case where the flag was read in
  // the middle of a write (the value read is then the previous one).
  int prevValue = -2;
  int prevSite = -1;
  int prevInstance = -1;
  int prevOffline = -1;
  // D-139 pre-test: the bypass parameter of the processor that owns the
  // renderer, as the renderer sees it, plus that processor's instance id.
  // -2 = no owning processor, -3 = parameter unavailable.
  int owningParamMilli = -4;
  int owningInstance = -1;
  // D-140: identity of the renderer and of the document controller it is
  // bound to, plus the render number (1..N from the trace batches).
  juce::int64 rendererId = 0;
  juce::int64 docControllerId = 0;
  int renderNumber = -1;
  // D-141: the other association keys and the bypass candidates.
  int editorInstance = -1;
  int editorParamMilli = -2;
  int maxParamInstance = -1;
  int maxParamMilli = -2;
  // D-142: route (c): the last processor that called prepareToPlay.
  int routeCInstance = -1;
  int routeCParamMilli = -2;
  // D-143: identity key. trackHash = hash of the renderer's own track name;
  // keyB = instance resolved by the track-host-ref registry; keyA = marker
  // registered at processor creation (no host track id exists then).
  juce::int64 trackHash = 0;
  juce::int64 trackRef = 0;
  int keyAInstance = -1;
  int keyAParamMilli = -2;
  int keyBInstance = -1;
  int keyBParamMilli = -2;
  // D-145: why a B block renders processed or raw.
  int procLocked = -1;      // tryLockProcessedAudio() succeeded
  int procHasAudio = -1;    // processed data present and non-empty
  float procFingerprint = 0.0f;
  int publishCount = -1;    // publish[edit]/[render] events since process start
  float msSincePublish = -1.0f;
  int editorOpen = -1;      // this document controller has a main component
  int publisherInstance = -1; // instance that published last (-1 = none)
  // D-146: the failed try-lock of the processed audio and its owner.
  int lockFailCount = -1;
  int lockOwnerKind = -1;   // 0=free, 1=render try-lock, 2=full lock
  juce::int64 lockOwnerThread = 0;
  int lockOwnerInstance = -1;
  float lockHeldMs = -1.0f;
  // D-147: bounded-wait outcome after a failed try-lock.
  int lockWaited = 0;
  int lockResolved = 0;
  float lockWaitMs = 0.0f;
};

// D-138: last writes to the static bypass flag (multi-writer, diag only).
// Every write publishes a single 64-bit word (value in the top bit, sequence
// in the rest) and its full metadata in a ring slot indexed by the sequence,
// so a reader can pin the exact write that produced the value it read: take
// the word, read the flag, take the word again; if it did not change, the
// metadata of that sequence is the writer.
struct AraBypassWriteInfo {
  juce::uint64 seq = 0;
  int value = -2;
  int site = -1;
  int instance = -1;
  int offline = -1;
  juce::int64 thread = 0;
  double timeMs = 0.0;
};

inline constexpr int kAraBypassWriteRing = 512;
struct AraBypassWriteSlot {
  std::atomic<juce::uint64> seq{0};
  int value = -2;
  int site = -1;
  int instance = -1;
  int offline = -1;
  juce::int64 thread = 0;
  double timeMs = 0.0;
};
inline AraBypassWriteSlot araBypassRing[kAraBypassWriteRing];
inline std::atomic<juce::uint64> araBypassValueSeq{0};
inline std::atomic<juce::uint64> araBypassWriteCount{0};

// D-138: incremented when the block time goes backwards (a new render or
// playback starts), so a write and a read can be placed in the same run.
inline std::atomic<int> araDiagRenderEpoch{0};

inline constexpr int kAraBlockTraceCapacity = 4096;
inline AraBlockTrace araBlockTrace[kAraBlockTraceCapacity];
inline int araBlockTraceCount = 0;
inline juce::int64 araBlockTraceLastT = -1;

inline void araBlockTraceFlush() {
  if (araBlockTraceCount == 0)
    return;
  juce::String out;
  out.preallocateBytes(araBlockTraceCount * 150);
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
        << " framesRatioNotOne=" << r.framesRatioNotOne
        << " maxDiffRaw=" << r.maxDiffRaw
        << " maxDiffProcessed=" << r.maxDiffProcessed
        << " refFlags=" << r.refFlags
        << " bypassParam=" << r.bypassParam
        << " bypassWriterThread="
        << juce::String::toHexString(r.bypassWriterThread)
        << " writeValue=" << r.writeValue
        << " writeSite=" << r.writeSite
        << " writeInstance=" << r.writeInstance
        << " writeOffline=" << r.writeOffline
        << " writeThread=" << juce::String::toHexString(r.writeThread)
        << " writeSeq=" << r.writeSeq
        << " writeTimeMs=" << juce::String(r.writeTimeMs, 3)
        << " readTimeMs=" << juce::String(r.readTimeMs, 3)
        << " writeFlags=" << r.writeFlags
        << " prevValue=" << r.prevValue
        << " prevSite=" << r.prevSite
        << " prevInstance=" << r.prevInstance
        << " prevOffline=" << r.prevOffline
        << " owningParamMilli=" << r.owningParamMilli
        << " owningInstance=" << r.owningInstance
        << " rendererId=" << juce::String::toHexString(r.rendererId)
        << " docControllerId=" << juce::String::toHexString(r.docControllerId)
        << " renderNumber=" << r.renderNumber
        << " editorInstance=" << r.editorInstance
        << " editorParamMilli=" << r.editorParamMilli
        << " maxParamInstance=" << r.maxParamInstance
        << " maxParamMilli=" << r.maxParamMilli
        << " routeCInstance=" << r.routeCInstance
        << " routeCParamMilli=" << r.routeCParamMilli
        << " trackHash=" << juce::String::toHexString(r.trackHash)
        << " trackRef=" << juce::String::toHexString(r.trackRef)
        << " keyAInstance=" << r.keyAInstance
        << " keyAParamMilli=" << r.keyAParamMilli
        << " keyBInstance=" << r.keyBInstance
        << " keyBParamMilli=" << r.keyBParamMilli
        << " procLocked=" << r.procLocked
        << " procHasAudio=" << r.procHasAudio
        << " procFingerprint=" << juce::String(r.procFingerprint, 6)
        << " publishCount=" << r.publishCount
        << " msSincePublish=" << juce::String(r.msSincePublish, 3)
        << " editorOpen=" << r.editorOpen
        << " publisherInstance=" << r.publisherInstance
        << " lockFailCount=" << r.lockFailCount
        << " lockOwnerKind=" << r.lockOwnerKind
        << " lockOwnerThread=" << juce::String::toHexString(r.lockOwnerThread)
        << " lockOwnerInstance=" << r.lockOwnerInstance
        << " lockHeldMs=" << juce::String(r.lockHeldMs, 3)
        << " lockWaited=" << r.lockWaited
        << " lockResolved=" << r.lockResolved
        << " lockWaitMs=" << juce::String(r.lockWaitMs, 3)
        << " renderEpoch=" << r.renderEpoch
        << " thread=" << juce::String::toHexString(r.thread) << "\n";
  }
  ARA_DIAG(out);
  araBlockTraceCount = 0;
}

inline void araBlockTracePush(const AraBlockTrace &r) {
  if (araBlockTraceCount > 0 && r.t < araBlockTraceLastT) {
    araDiagRenderEpoch.fetch_add(1);
    araBlockTraceFlush();
  }
  if (araBlockTraceCount >= kAraBlockTraceCapacity)
    araBlockTraceFlush();
  AraBlockTrace stored = r;
  stored.renderEpoch = araDiagRenderEpoch.load();
  // D-140: the render number is the epoch (one increment per new block-time
  // range: a new render or playback starts).
  stored.renderNumber = stored.renderEpoch;
  araBlockTrace[araBlockTraceCount++] = stored;
  araBlockTraceLastT = r.t;
}

inline juce::int64 araDiagThreadId() {
  return static_cast<juce::int64>(
      reinterpret_cast<juce::pointer_sized_int>(juce::Thread::getCurrentThreadId()));
}

// D-146: ownership tracking for the processed-audio lock. The owner (thread,
// instance, kind, acquisition time) is stored in atomics so a failed try-lock
// can report who held the lock at that instant. Diagnostics only: enter/exit
// have exactly the same semantics as juce::SpinLock.
enum AraLockKind {
  kAraLockFree = 0,
  kAraLockRender = 1,
  kAraLockFull = 2
};

inline thread_local int araDiagLockInstance = -1;
inline std::atomic<juce::int64> araLockOwnerThread{-1};
inline std::atomic<int> araLockOwnerInstance{-1};
inline std::atomic<int> araLockOwnerKind{kAraLockFree};
inline std::atomic<double> araLockOwnerTimeMs{0.0};
inline std::atomic<int> araLockFailureCount{0};
inline std::atomic<juce::int64> araLockFailThread{-1};
inline std::atomic<int> araLockFailInstance{-1};
inline std::atomic<juce::int64> araLockFailOwnerThread{-1};
inline std::atomic<int> araLockFailOwnerInstance{-1};
inline std::atomic<int> araLockFailOwnerKind{-1};
inline std::atomic<double> araLockFailOwnerHeldMs{-1.0};

struct AraTrackedLock {
  void noteAcquired(int kind) const noexcept {
    araLockOwnerThread.store(araDiagThreadId(), std::memory_order_relaxed);
    araLockOwnerInstance.store(araDiagLockInstance, std::memory_order_relaxed);
    araLockOwnerKind.store(kind, std::memory_order_relaxed);
    araLockOwnerTimeMs.store(juce::Time::getMillisecondCounterHiRes(),
                             std::memory_order_relaxed);
  }

  void enter() const noexcept {
    lock.enter();
    noteAcquired(kAraLockFull);
  }

  void exit() const noexcept {
    araLockOwnerKind.store(kAraLockFree, std::memory_order_relaxed);
    araLockOwnerThread.store(-1, std::memory_order_relaxed);
    araLockOwnerInstance.store(-1, std::memory_order_relaxed);
    lock.exit();
  }

  bool tryEnter() const noexcept {
    if (lock.tryEnter()) {
      noteAcquired(kAraLockRender);
      return true;
    }
    araLockFailureCount.fetch_add(1, std::memory_order_relaxed);
    araLockFailThread.store(araDiagThreadId(), std::memory_order_relaxed);
    araLockFailInstance.store(araDiagLockInstance, std::memory_order_relaxed);
    const int ownerKind = araLockOwnerKind.load(std::memory_order_relaxed);
    araLockFailOwnerThread.store(araLockOwnerThread.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
    araLockFailOwnerInstance.store(
        araLockOwnerInstance.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    araLockFailOwnerKind.store(ownerKind, std::memory_order_relaxed);
    const double acquired = araLockOwnerTimeMs.load(std::memory_order_relaxed);
    araLockFailOwnerHeldMs.store(
        ownerKind == kAraLockFree || acquired <= 0.0
            ? -1.0
            : juce::Time::getMillisecondCounterHiRes() - acquired,
        std::memory_order_relaxed);
    return false;
  }

  mutable juce::SpinLock lock;
};

inline void araBypassNoteWrite(int value, int site, int instance, int offline) {
  const auto seq = araBypassWriteCount.fetch_add(1) + 1;
  auto &slot = araBypassRing[seq % kAraBypassWriteRing];
  slot.seq.store(0, std::memory_order_relaxed);
  slot.value = value;
  slot.site = site;
  slot.instance = instance;
  slot.offline = offline;
  slot.thread = araDiagThreadId();
  slot.timeMs = juce::Time::getMillisecondCounterHiRes();
  slot.seq.store(seq, std::memory_order_release);
  araBypassValueSeq.store((static_cast<juce::uint64>(value & 1) << 63) | seq,
                          std::memory_order_release);
}

inline bool araBypassLookupWrite(juce::uint64 seq, AraBypassWriteInfo &out) {
  if (seq == 0)
    return false;
  auto &slot = araBypassRing[seq % kAraBypassWriteRing];
  if (slot.seq.load(std::memory_order_acquire) != seq)
    return false;
  out.seq = seq;
  out.value = slot.value;
  out.site = slot.site;
  out.instance = slot.instance;
  out.offline = slot.offline;
  out.thread = slot.thread;
  out.timeMs = slot.timeMs;
  return slot.seq.load(std::memory_order_acquire) == seq;
}

// D-141: diagnostic registry of live processor instances, keyed by instance id
// (never by controller pointer). It is only used to read each instance's
// bypass parameter for the render trace.
struct AraProcessorEntry {
  int instance = -1;
  void *processor = nullptr;
};
inline juce::SpinLock araProcessorRegistryLock;
inline std::vector<AraProcessorEntry> araProcessorRegistry;

inline void araRegisterProcessor(int instance, void *processor) {
  const juce::SpinLock::ScopedLockType lock(araProcessorRegistryLock);
  araProcessorRegistry.push_back({instance, processor});
}

inline void araUnregisterProcessor(int instance) {
  const juce::SpinLock::ScopedLockType lock(araProcessorRegistryLock);
  araProcessorRegistry.erase(
      std::remove_if(araProcessorRegistry.begin(), araProcessorRegistry.end(),
                     [instance](const AraProcessorEntry &entry) {
                       return entry.instance == instance;
                     }),
      araProcessorRegistry.end());
}

inline void araSnapshotProcessors(std::vector<AraProcessorEntry> &out) {
  const juce::SpinLock::ScopedTryLockType lock(araProcessorRegistryLock);
  if (!lock.isLocked())
    return;
  out = araProcessorRegistry;
}

// D-142 route (c): the processor that registered last in prepareToPlay (a
// single slot: "the FX registers itself" attempt). Value in milli-units.
inline std::atomic<int> araPrepareInstance{-1};
inline std::atomic<int> araPrepareParamMilli{-2};

// D-145: publish events (publish[edit]/publish[render]) and who published
// last. Only diagnostics: it does not change what is published.
inline std::atomic<int> araPublishCount{0};
inline std::atomic<double> araPublishLastMs{0.0};
inline std::atomic<int> araPublishInstance{-1};

inline void araNotePublish(int instance) {
  araPublishCount.fetch_add(1, std::memory_order_relaxed);
  araPublishLastMs.store(juce::Time::getMillisecondCounterHiRes(),
                         std::memory_order_relaxed);
  araPublishInstance.store(instance, std::memory_order_relaxed);
}

// D-143: track identity. The host's region-sequence host reference is the
// track id (never the controller pointer). Each processor that sees a region
// sequence registers its instance under that id; the renderer looks up its own.
struct AraTrackEntry {
  juce::uint64 ref = 0;
  int instance = -1;
  void *processor = nullptr;
};
inline juce::SpinLock araTrackRegistryLock;
inline std::vector<AraTrackEntry> araTrackRegistry;

inline void araRegisterTrackRef(juce::uint64 ref, int instance,
                                void *processor) {
  if (ref == 0)
    return;
  const juce::SpinLock::ScopedLockType lock(araTrackRegistryLock);
  for (auto &entry : araTrackRegistry) {
    if (entry.ref == ref) {
      entry.instance = instance;
      entry.processor = processor;
      return;
    }
  }
  araTrackRegistry.push_back({ref, instance, processor});
}

inline bool araLookupTrackRef(juce::uint64 ref, int &instance,
                              void *&processor) {
  if (ref == 0)
    return false;
  const juce::SpinLock::ScopedTryLockType lock(araTrackRegistryLock);
  if (!lock.isLocked())
    return false;
  for (const auto &entry : araTrackRegistry) {
    if (entry.ref == ref) {
      instance = entry.instance;
      processor = entry.processor;
      return true;
    }
  }
  return false;
}

inline juce::int64 araHashName(const char *text) {
  // FNV-1a 64
  juce::uint64 hash = 1469598103934665603ull;
  if (text != nullptr)
    for (const char *p = text; *p != '\0'; ++p) {
      hash ^= static_cast<unsigned char>(*p);
      hash *= 1099511628211ull;
    }
  return static_cast<juce::int64>(hash);
}

// D-150: diagnostic-only forcing of track A's output, so the mixed render can
// be measured with A fixed. Read once from the environment; -1 = no forcing,
// 0 = force processed, 1 = force raw. It never affects the production build.
inline int araDiagForceTrackA() {
  static const int value = [] {
    const char *env = std::getenv("PITCHCORE_DIAG_FORCE_A");
    if (env == nullptr)
      return -1;
    const juce::String text(env);
    if (text.equalsIgnoreCase("processed"))
      return 0;
    if (text.equalsIgnoreCase("raw"))
      return 1;
    return -1;
  }();
  return value;
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
