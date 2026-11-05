// D-170: determinism harness for the ARA restore/analysis race.
//
// The real race depends on the host restoring ARA archives on its own thread
// while the canvas analysis callback is queued on the message thread. That
// ordering cannot be forced from a unit test without a host. The closest
// deterministic test is the decision itself: a project restored for region K
// must cancel an analysis already pending for K (and only for K), so the
// analysis completion can never overwrite the restored state.

#include "../Source/Models/Project.h"
#include "../Source/Models/ProjectSerializer.h"
#include "../Source/Plugin/PluginProcessor.h"
#include "TestAssert.h"

#include <cstdint>
#include <iostream>

// Test-only view of the processor's private pending-analysis bookkeeping.
struct PitchNetProcessorTestAccess {
  static void beginPendingAnalysis(PitchNetAudioProcessor &processor,
                                   const juce::String &key,
                                   std::uint64_t generation) {
    processor.pendingRegionCanvasAnalysisKey = key;
    processor.regionCanvasAnalysisGeneration.store(generation);
    processor.regionCanvasAnalysisPending.store(true);
  }

  static bool isPending(const PitchNetAudioProcessor &processor) {
    return processor.regionCanvasAnalysisPending.load();
  }

  static juce::String pendingKey(const PitchNetAudioProcessor &processor) {
    return processor.pendingRegionCanvasAnalysisKey;
  }

  static std::uint64_t generation(const PitchNetAudioProcessor &processor) {
    return processor.regionCanvasAnalysisGeneration.load();
  }

  static bool hasProject(const PitchNetAudioProcessor &processor,
                         const juce::String &key) {
    const auto it = processor.araRegions.find(key);
    return it != processor.araRegions.end() && it->second.project != nullptr;
  }

  static std::uint64_t restoreSerial(const PitchNetAudioProcessor &processor,
                                     const juce::String &key) {
    const auto it = processor.araRegions.find(key);
    return it != processor.araRegions.end() ? it->second.restoreSerial : 0;
  }
};

namespace {
juce::MemoryBlock archiveWithAnalysisData() {
  Project project;
  project.getAudioData().sampleRate = 44100;
  project.getAudioData().f0 = {100.0f};

  juce::MemoryBlock archive;
  CHECK(ProjectSerializer::toBinaryArchive(project, archive));
  CHECK(archive.getSize() > 0);
  return archive;
}
} // namespace

int main() {
  const auto archive = archiveWithAnalysisData();

  {
    PitchNetAudioProcessor processor;
    PitchNetProcessorTestAccess::beginPendingAnalysis(processor, "region-k", 7);

    processor.restoreAraRegionProject("region-k", archive.getData(),
                                      archive.getSize());

    CHECK(PitchNetProcessorTestAccess::hasProject(processor, "region-k"));
    CHECK(PitchNetProcessorTestAccess::restoreSerial(processor, "region-k") == 1);
    CHECK(!PitchNetProcessorTestAccess::isPending(processor));
    CHECK(PitchNetProcessorTestAccess::pendingKey(processor).isEmpty());
    CHECK(PitchNetProcessorTestAccess::generation(processor) == 8);
    std::cout << "restore for the pending region cancelled the analysis\n";
  }

  {
    PitchNetAudioProcessor processor;
    PitchNetProcessorTestAccess::beginPendingAnalysis(processor, "region-k", 7);

    processor.restoreAraRegionProject("region-other", archive.getData(),
                                      archive.getSize());

    CHECK(PitchNetProcessorTestAccess::hasProject(processor, "region-other"));
    CHECK(PitchNetProcessorTestAccess::restoreSerial(processor, "region-k") == 0);
    CHECK(PitchNetProcessorTestAccess::restoreSerial(processor, "region-other") == 1);
    CHECK(PitchNetProcessorTestAccess::isPending(processor));
    CHECK(PitchNetProcessorTestAccess::pendingKey(processor) == "region-k");
    CHECK(PitchNetProcessorTestAccess::generation(processor) == 7);
    std::cout << "restore for another region left the analysis pending\n";
  }

  std::cout << "AraRestoreAnalysisRace tests passed\n";
}
