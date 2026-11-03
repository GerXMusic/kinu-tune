#pragma once

#include "../JuceHeader.h"
#include <limits>

#if JucePlugin_Enable_ARA

#include <atomic>
#include <memory>

#include "AraDiagnostics.h"

// PROCESSED audio and project archive owned by the ARA audio modification, so
// the analysed/processed result lives independently of the editor: the timeline
// draws each clip from here and the playback renderer mixes from here, both of
// which work with the UI closed.
//
// State is held in single slots, not keyed containers. ARA gives persistent
// identity to the audio modification, and every playback region referencing one
// is a window onto the same edit layer, so a modification owns exactly one
// project archive and one processed-audio buffer. A key would carry no
// information.
//
// The modification's host-assigned persistent ID is deliberately NOT used as an
// identity here. ARA treats it as an archive-reconnection token and a mutable
// model property - hosts may adjust it on restore or import, and REAPER changes
// it from an absolute to a project-relative path on a project's first save. It
// belongs in the archive stream and in diagnostics, nowhere else. Live identity
// is getLiveKey(), a process-local serial minted at construction.
class PitchNetAudioModification final : public juce::ARAAudioModification {
public:
  // D-152: one immutable publication of the processed audio. A new generation
  // is created for every setProcessedAudio() call; readers load the latest
  // shared pointer and keep it alive for as long as they use it.
  struct ProcessedAudioSnapshot {
    juce::AudioBuffer<float> audio;
    double sampleRate = 0.0;
    juce::int64 startSampleInModification = 0;
    juce::uint64 generation = 0;

    bool hasAudio() const {
      return audio.getNumSamples() > 0 && sampleRate > 0.0;
    }
  };

  PitchNetAudioModification(
      juce::ARAAudioSource *audioSource,
      ARA::ARAAudioModificationHostRef hostRef,
      const juce::ARAAudioModification *optionalModificationToClone)
      : juce::ARAAudioModification(audioSource, hostRef,
                                   optionalModificationToClone),
        liveKey("mod:" + juce::String(nextLiveKeySerial().fetch_add(1))) {
    // ARA defines the source passed here as the object whose internal state the
    // clone must copy, and the host assigns the clone's own properties straight
    // afterwards. Copying synchronously here is therefore complete: nothing can
    // arrive on the source in between, and later edits to the source correctly
    // do not bleed into an existing clone.
    if (const auto *sourceModification =
            dynamic_cast<const PitchNetAudioModification *>(
                optionalModificationToClone)) {
      if (const auto sourceAudio =
              sourceModification->getProcessedAudioSnapshot();
          sourceAudio != nullptr && sourceAudio->hasAudio())
        setProcessedAudio(sourceAudio->audio, sourceAudio->sampleRate,
                          sourceAudio->startSampleInModification);

      juce::MemoryBlock sourceArchive;
      if (sourceModification->copyProjectArchive(sourceArchive))
        setProjectArchive(sourceArchive.getData(), sourceArchive.getSize());
    }
  }

  //============================================================================
  // Live identity
  //
  // Stable for this object's lifetime and unique within the process. A raw
  // address would also be lifetime-stable, but destruction lets the allocator
  // reuse it, so a stale key could come to equal a live object's key. A serial
  // cannot collide.
  //
  // MUST NEVER be written to any archive. It is meaningless across runs.
  const juce::String &getLiveKey() const noexcept { return liveKey; }

#if PITCHNET_ARA_DIAGNOSTICS && PITCHNET_LOGGING_ENABLED
  // D-152: restored state received for this modification. Diagnostic only;
  // the flag never feeds the audio path.
  bool wasRestoredFromArchive() const noexcept {
    return restoredFromArchive.load(std::memory_order_relaxed);
  }

  bool markRestoredFromArchive() noexcept {
    return !restoredFromArchive.exchange(true, std::memory_order_relaxed);
  }
#endif

  //============================================================================
  // Mutators
  void setProcessedAudio(const juce::AudioBuffer<float> &buffer,
                         double sampleRateIn,
                         juce::int64 startSampleInModificationIn) {
    auto snapshot = std::make_shared<ProcessedAudioSnapshot>();
    snapshot->audio.makeCopyOf(buffer);
    snapshot->sampleRate = sampleRateIn;
    snapshot->startSampleInModification = startSampleInModificationIn;
    snapshot->generation = nextProcessedGeneration.fetch_add(1, std::memory_order_relaxed) + 1;

    std::atomic_store_explicit(
        &processedAudio,
        std::shared_ptr<const ProcessedAudioSnapshot>(std::move(snapshot)),
        std::memory_order_release);
  }

  void clearProcessedAudio() {
    std::atomic_store_explicit(
        &processedAudio, std::shared_ptr<const ProcessedAudioSnapshot>{},
        std::memory_order_release);
  }

  void setProjectArchive(const void *data, size_t sizeInBytes) const {
    if (data == nullptr || sizeInBytes == 0)
      return;

    const juce::ScopedLock lock(projectArchiveLock);
    projectArchive = juce::MemoryBlock(data, sizeInBytes);
  }

  //============================================================================
  // Queries
  bool copyProjectArchive(juce::MemoryBlock &dest) const {
    const juce::ScopedLock lock(projectArchiveLock);
    if (projectArchive.getSize() == 0)
      return false;

    dest = projectArchive;
    return true;
  }

  bool hasProjectArchive() const {
    const juce::ScopedLock lock(projectArchiveLock);
    return projectArchive.getSize() > 0;
  }

  std::shared_ptr<const ProcessedAudioSnapshot>
  getProcessedAudioSnapshot() const {
    return std::atomic_load_explicit(&processedAudio, std::memory_order_acquire);
  }

  bool hasProcessedAudio() const {
    const auto snapshot = getProcessedAudioSnapshot();
    return snapshot != nullptr && snapshot->hasAudio();
  }

  bool copyProcessedAudio(juce::AudioBuffer<float> &buffer,
                          double &sampleRateOut,
                          juce::int64 &startSampleOut) const {
    const auto snapshot = getProcessedAudioSnapshot();
    if (snapshot == nullptr || !snapshot->hasAudio())
      return false;

    buffer.makeCopyOf(snapshot->audio);
    sampleRateOut = snapshot->sampleRate;
    startSampleOut = snapshot->startSampleInModification;
    return true;
  }

  // Every playback region referencing this modification shares its edit layer,
  // so they all serve the same processed audio. This reverses the earlier
  // per-region rule, which existed when state was owned by regions rather than
  // by the modification.

  //============================================================================
  // Persistence. Stream this modification's processed audio.
  bool writeProcessedAudioToStream(juce::OutputStream &output) const {
    const auto snapshot = getProcessedAudioSnapshot();
    if (snapshot == nullptr || !snapshot->hasAudio())
      return false;

    const auto &data = *snapshot;
    output.writeDouble(data.sampleRate);
    output.writeInt64(data.startSampleInModification);
    output.writeInt(data.audio.getNumChannels());
    output.writeInt(data.audio.getNumSamples());
    for (int ch = 0; ch < data.audio.getNumChannels(); ++ch)
      if (!output.write(data.audio.getReadPointer(ch),
                        static_cast<size_t>(data.audio.getNumSamples()) *
                            sizeof(float)))
        return false;
    return true;
  }

  bool readProcessedAudioFromStream(juce::InputStream &input) {
    const auto sampleRateIn = input.readDouble();
    const auto startSampleIn = input.readInt64();
    const auto numChannels = input.readInt();
    const auto numSamples = input.readInt();
    const int bytesPerSample = sizeof(float);
    if (!std::isfinite(sampleRateIn) || sampleRateIn <= 0.0 ||
        numChannels < 0 ||
        numSamples < 0 || numSamples > std::numeric_limits<int>::max() / bytesPerSample)
      return false;

    juce::AudioBuffer<float> restored(numChannels, numSamples);
    const int channelBytes = numSamples * bytesPerSample;
    for (int ch = 0; ch < numChannels; ++ch)
      if (input.read(restored.getWritePointer(ch), channelBytes) != channelBytes)
        return false;

    setProcessedAudio(restored, sampleRateIn, startSampleIn);
    return true;
  }

  static bool skipProcessedAudioFromStream(juce::InputStream &input) {
    input.readDouble();
    input.readInt64();
    const auto numChannels = input.readInt();
    const auto numSamples = input.readInt();
    const int bytesPerSample = sizeof(float);
    if (numChannels < 0 || numSamples < 0 ||
        numSamples > std::numeric_limits<int>::max() / bytesPerSample)
      return false;
    juce::HeapBlock<char> skip(static_cast<size_t>(numSamples) * sizeof(float));
    const int channelBytes = numSamples * bytesPerSample;
    for (int ch = 0; ch < numChannels; ++ch)
      if (input.read(skip.get(), channelBytes) != channelBytes)
        return false;
    return true;
  }

private:
  static std::atomic<juce::uint64> &nextLiveKeySerial() {
    static std::atomic<juce::uint64> serial{1};
    return serial;
  }

  const juce::String liveKey;
  mutable juce::CriticalSection projectArchiveLock;
  mutable juce::MemoryBlock projectArchive;
  std::atomic<juce::uint64> nextProcessedGeneration{0};
  std::shared_ptr<const ProcessedAudioSnapshot> processedAudio;
#if PITCHNET_ARA_DIAGNOSTICS && PITCHNET_LOGGING_ENABLED
  std::atomic<bool> restoredFromArchive{false};
#endif
};

#endif // JucePlugin_Enable_ARA
