// D-152 characterization tests.
//
// These tests describe the behaviour that exists BEFORE the stability
// implementation. They are intentionally allowed to change (and fail) when the
// implementation lands; their job is to make that change visible.

#include "../Source/Plugin/ARADocumentController.h"
#include "../Source/Plugin/PitchNetAudioModification.h"
#include "../Source/Plugin/PluginProcessor.h"
#include "TestAssert.h"

#include <atomic>
#include <cmath>
#include <iostream>
#include <optional>
#include <thread>

namespace {

class StoppedPlayHead final : public juce::AudioPlayHead {
public:
  juce::Optional<PositionInfo> getPosition() const override {
    PositionInfo info;
    info.setIsPlaying(false);
    return info;
  }
};

void setFloatParameter(PitchNetAudioProcessor &processor, const char *id,
                       float value) {
  auto *parameter = processor.getAPVTS().getParameter(id);
  CHECK(parameter != nullptr);
  parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

float getFloatParameter(PitchNetAudioProcessor &processor, const char *id) {
  auto *value = processor.getAPVTS().getRawParameterValue(id);
  CHECK(value != nullptr);
  return value->load();
}

void runBypassCharacterization() {
  PitchNetAudioProcessor processor;
  StoppedPlayHead playHead;
  processor.setPlayHead(&playHead);
  processor.prepareToPlay(44100.0, 64);

  juce::AudioBuffer<float> buffer(1, 64);
  juce::MidiBuffer midi;

  // D-153: the APVTS bypass parameter is gone. Per-track bypass belongs to the
  // host; the plugin always renders program when processBlock is called.
  CHECK(processor.getBypassParameter() == nullptr);
  CHECK(processor.getAPVTS().getParameter("bypass") == nullptr);

  buffer.clear();
  buffer.setSample(0, 0, 0.75f);
  processor.processBlock(buffer, midi);
  CHECK(buffer.getSample(0, 0) == 0.0f);

  // A host that still calls the plugin-level bypass leaves the input untouched;
  // in ARA the supported path is disabling the FX in the host.
  buffer.clear();
  buffer.setSample(0, 0, 0.75f);
  processor.processBlockBypassed(buffer, midi);
  CHECK(std::abs(buffer.getSample(0, 0) - 0.75f) < 1.0e-6f);

  std::cout << "bypass: no APVTS bypass; processBlock clears, "
               "processBlockBypassed passes input through\n";
}

void runSavedStateCharacterization() {
  PitchNetAudioProcessor source;
  setFloatParameter(source, "outputGain", -6.0f);
  setFloatParameter(source, "dryWet", 50.0f);
  setFloatParameter(source, "pitchOffset", 2.0f);
  setFloatParameter(source, "formantShift", -1.0f);

  juce::MemoryBlock state;
  source.getStateInformation(state);
  CHECK(state.getSize() > 0);

  PitchNetAudioProcessor loaded;
  loaded.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

  // D-153: the four remaining APVTS parameters round-trip. The old bypass
  // parameter is absent; an old archive carrying it is ignored by APVTS.
  CHECK(loaded.getAPVTS().getParameter("bypass") == nullptr);
  CHECK(std::abs(getFloatParameter(loaded, "outputGain") - (-6.0f)) < 1.0e-4f);
  CHECK(std::abs(getFloatParameter(loaded, "dryWet") - 50.0f) < 1.0e-4f);
  CHECK(std::abs(getFloatParameter(loaded, "pitchOffset") - 2.0f) < 1.0e-4f);
  CHECK(std::abs(getFloatParameter(loaded, "formantShift") - (-1.0f)) < 1.0e-4f);

  std::cout << "saved state: the four APVTS values round-trip without a "
               "bypass parameter\n";
}

void runProcessedAudioCharacterization() {
  // A minimal ARA model graph is enough for the modification's own slot: no
  // document-controller callbacks are used by setProcessedAudio/copy.
  ARA::PlugIn::Document document(nullptr);
  juce::ARAAudioSource source(&document, nullptr);
  PitchNetAudioModification modification(&source, nullptr, nullptr);

  juce::AudioBuffer<float> first(1, 8);
  juce::AudioBuffer<float> second(1, 4);
  for (int i = 0; i < first.getNumSamples(); ++i)
    first.setSample(0, i, static_cast<float>(i) / 10.0f);
  for (int i = 0; i < second.getNumSamples(); ++i)
    second.setSample(0, i, 10.0f + static_cast<float>(i));

  modification.setProcessedAudio(first, 44100.0, 0);
  CHECK(modification.hasProcessedAudio());

  juce::AudioBuffer<float> read;
  double rate = 0.0;
  juce::int64 start = 0;
  CHECK(modification.copyProcessedAudio(read, rate, start));
  CHECK(rate == 44100.0);
  CHECK(start == 0);
  CHECK(read.getNumSamples() == first.getNumSamples());
  for (int i = 0; i < first.getNumSamples(); ++i)
    CHECK(read.getSample(0, i) == first.getSample(0, i));

  // Two consecutive publications: the second is the visible one.
  modification.setProcessedAudio(second, 48000.0, 2);
  CHECK(modification.copyProcessedAudio(read, rate, start));
  CHECK(rate == 48000.0);
  CHECK(start == 2);
  CHECK(read.getNumSamples() == second.getNumSamples());
  for (int i = 0; i < second.getNumSamples(); ++i)
    CHECK(read.getSample(0, i) == second.getSample(0, i));

  std::cout << "processed audio: set then copy returns the same content; the "
               "last publication wins\n";
}

void runConcurrentGenerationsCharacterization() {
  ARA::PlugIn::Document document(nullptr);
  juce::ARAAudioSource source(&document, nullptr);
  PitchNetAudioModification modification(&source, nullptr, nullptr);

  constexpr int kSamples = 64;
  constexpr int kGenerations = 500;
  const auto makeGeneration = [kSamples](int generation) {
    juce::AudioBuffer<float> buffer(1, kSamples);
    for (int i = 0; i < kSamples; ++i)
      buffer.setSample(0, i, static_cast<float>(generation));
    return buffer;
  };

  modification.setProcessedAudio(makeGeneration(0), 44100.0, 0);

  std::atomic<bool> start{false};
  std::atomic<bool> done{false};
  std::atomic<int> reads{0};

  std::thread writer([&] {
    while (!start.load(std::memory_order_acquire))
      std::this_thread::yield();

    for (int generation = 0; generation < kGenerations; ++generation) {
      modification.setProcessedAudio(makeGeneration(generation), 44100.0,
                                     generation);
      // Ensure the reader is actually running before the writer finishes.
      if (generation == 0)
        while (reads.load(std::memory_order_acquire) == 0)
          std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
  });

  std::thread reader([&] {
    while (!start.load(std::memory_order_acquire))
      std::this_thread::yield();

    int lastGeneration = -1;
    while (!done.load(std::memory_order_acquire)) {
      juce::AudioBuffer<float> copy;
      double rate = 0.0;
      juce::int64 startSample = 0;
      if (!modification.copyProcessedAudio(copy, rate, startSample))
        continue;
      reads.fetch_add(1, std::memory_order_release);

      CHECK(copy.getNumSamples() == kSamples);
      CHECK(rate == 44100.0);
      CHECK(startSample >= 0 && startSample < kGenerations);
      const int generation = static_cast<int>(startSample);
      CHECK(generation >= lastGeneration);
      lastGeneration = generation;

      const float expected = static_cast<float>(generation);
      for (int i = 0; i < kSamples; ++i)
        CHECK(copy.getSample(0, i) == expected);
    }
  });

  start.store(true, std::memory_order_release);
  writer.join();
  reader.join();

  juce::AudioBuffer<float> finalCopy;
  double finalRate = 0.0;
  juce::int64 finalStart = 0;
  CHECK(modification.copyProcessedAudio(finalCopy, finalRate, finalStart));
  CHECK(finalRate == 44100.0);
  CHECK(finalStart == kGenerations - 1);
  CHECK(finalCopy.getNumSamples() == kSamples);
  for (int i = 0; i < kSamples; ++i)
    CHECK(finalCopy.getSample(0, i) == static_cast<float>(finalStart));

  std::cout << "concurrent generations: copies are internally consistent and "
               "the visible generation never decreases\n";
}

void runHydrationCharacterization() {
  ARA::ARADocumentControllerHostInstance hostInstance{};
  PitchNetDocumentController controller(nullptr, &hostInstance);

  CHECK(controller.diagDocumentHydrated());

  controller.diagNoteModificationCreated();
  CHECK(!controller.diagDocumentHydrated());

  controller.diagNoteModificationRestored();
  CHECK(controller.diagDocumentHydrated());

  controller.diagNoteModificationCreated();
  CHECK(!controller.diagDocumentHydrated());

  controller.diagNoteModificationRestored();
  CHECK(controller.diagDocumentHydrated());

  controller.diagNoteModificationDestroyed(true);
  CHECK(controller.diagDocumentHydrated());

  controller.diagNoteModificationDestroyed(true);
  CHECK(controller.diagDocumentHydrated());

  std::cout << "hydration: 0 modifications -> 1; pending modification -> 0; "
               "restored -> 1\n";
}

} // namespace

int main(int argc, char **argv) {
  juce::ScopedJuceInitialiser_GUI gui;
  const juce::String group = argc > 1 ? juce::String(argv[1]) : juce::String();

  if (group == "bypass")
    runBypassCharacterization();
  else if (group == "saved-state")
    runSavedStateCharacterization();
  else if (group == "processed-audio")
    runProcessedAudioCharacterization();
  else if (group == "concurrent-generations")
    runConcurrentGenerationsCharacterization();
  else if (group == "hydration")
    runHydrationCharacterization();
  else {
    std::cerr << "unknown group: " << group << "\n";
    return 2;
  }

  return 0;
}
