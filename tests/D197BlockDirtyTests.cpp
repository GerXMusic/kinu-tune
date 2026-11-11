// D-197: dirty range per silence-bounded block + vocoder background load.
#include "../Source/Models/Project.h"
#include "../Source/Audio/Vocoder.h"
#include "../Source/Utils/Constants.h"
#include "TestAssert.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
namespace {
constexpr int kSampleRate = 48000;
constexpr int kHop = HOP_SIZE;
constexpr int kFrames = 420;
juce::AudioBuffer<float> makeSource(int silentFromFrame, int silentToFrame) {
    juce::AudioBuffer<float> buffer(1, kFrames * kHop);
    for (int i = 0; i < buffer.getNumSamples(); ++i)
        buffer.setSample(0, i, 0.25f * std::sin(2.0f * 3.14159265f * 220.0f * (float)i / kSampleRate));
    for (int i = silentFromFrame * kHop; i < silentToFrame * kHop && i < buffer.getNumSamples(); ++i)
        buffer.setSample(0, i, 0.0f);
    return buffer;
}
Note makeEditedNote(int startFrame, int endFrame) {
    Note n(startFrame, endFrame, 64.0f);
    n.setOriginalMidiNote(60.0f);
    n.setMidiNote(64.0f);
    return n;
}
void testDirtyStopsAtBlockBoundary() {
    Project p;
    p.getAudioData().sampleRate = kSampleRate;
    p.getAudioData().originalWaveform = makeSource(150, 170);
    p.getAudioData().f0.assign(kFrames, 0.0f);
    p.getNotes().push_back(makeEditedNote(10, 20));
    p.getNotes().push_back(makeEditedNote(300, 320));
    CHECK(p.markEditedNotesDirty() == 2);
    // Contract kept: every edited note is dirty and the F0 range spans all.
    CHECK(p.getNotes()[0].isDirty());
    CHECK(p.getNotes()[1].isDirty());
    const auto span = p.getF0DirtyRange();
    CHECK(span.first == 0);
    CHECK(span.second == 380);

    CHECK(p.hasEditedBlockPlan());
    int firstStart = -1, firstEnd = -1;
    CHECK(p.nextEditedBlockRange(firstStart, firstEnd));
    std::cout << "first block [" << firstStart << "," << firstEnd << ")\n";
    CHECK(firstStart == 0);
    CHECK(firstEnd == 80);
    int secondStart = -1, secondEnd = -1;
    CHECK(p.nextEditedBlockRange(secondStart, secondEnd));
    std::cout << "second block [" << secondStart << "," << secondEnd << ")\n";
    CHECK(secondStart == 240);
    CHECK(secondEnd == 380);
    const int rendered = (firstEnd - firstStart) + (secondEnd - secondStart);
    const int budget = (20 - 10) + (320 - 300) + 2 * 60 * 2;
    std::cout << "rendered=" << rendered << " budget=" << budget << "\n";
    CHECK(rendered <= budget);
    int noneStart = -1, noneEnd = -1;
    CHECK(!p.nextEditedBlockRange(noneStart, noneEnd));
    CHECK(!p.hasEditedBlockPlan());
    Vocoder v;
    CHECK(v.getLoadState() == Vocoder::LoadState::NotLoaded);
    CHECK(!v.isReady());
    const auto t0 = std::chrono::steady_clock::now();
    const bool started = v.beginAsyncLoad(juce::File("Z:/does/not/exist.onnx"));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "beginAsyncLoad=" << started << " elapsed=" << elapsed << " ms\n";
    CHECK(started);
    CHECK(elapsed < 50);
    for (int i = 0; i < 200 && v.getLoadState() == Vocoder::LoadState::Loading; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(v.getLoadState() == Vocoder::LoadState::Failed);
    CHECK(!v.isReady());
}
} // namespace
int main() {
    std::cout << std::unitbuf;
    testDirtyStopsAtBlockBoundary();
    std::cout << "D197 tests passed\n";
    return 0;
}
