// Per-note engine field (M2 fase 1, D-125).
//
// The field travels in the project metadata JSON - the one the ARA
// modification archive stores - and it is optional: a project written before
// it existed must load as Inherit and keep its audio. These checks cover the
// three values through a save/load round trip, the missing and unknown value
// fallbacks, and the SHA-256 of the rendered waveform before and after an old
// project is loaded.
//
// The "render" here is the rendered waveform the project carries (the audio a
// bounce would publish). This test does not run a DAW bounce: it proves that
// loading a project without noteEngine leaves the audio bit for bit the same.

#include "../Source/Models/Note.h"
#include "../Source/Models/Project.h"
#include "../Source/Models/ProjectSerializer.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/SHA256Utils.h"
#include "TestAssert.h"

#include <cmath>
#include <cstring>
#include <iostream>

namespace {

constexpr int kFrames = 8;

Project makeProject() {
    Project project;
    auto &audio = project.getAudioData();
    audio.sampleRate = SAMPLE_RATE;

    const int numSamples = kFrames * HOP_SIZE;
    audio.originalWaveform.setSize(1, numSamples);
    audio.waveform.setSize(1, numSamples);
    for (int i = 0; i < numSamples; ++i) {
        const float value =
            0.4f * std::sin(2.0f * 3.14159265f * 220.0f *
                            static_cast<float>(i) / SAMPLE_RATE);
        audio.originalWaveform.setSample(0, i, value);
        audio.waveform.setSample(0, i, 0.5f * value); // stand-in for a render
    }
    audio.f0.assign(kFrames, 220.0f);
    audio.rawF0 = audio.f0;
    audio.cleanedF0 = audio.f0;
    audio.denseF0 = audio.f0;
    audio.baseF0 = audio.f0;
    audio.basePitch.assign(kFrames, 57.0f);
    audio.deltaPitch.assign(kFrames, 0.0f);
    audio.voicedMask.assign(kFrames, true);
    audio.vadMask.assign(kFrames, true);
    audio.segmentChunkRanges.emplace_back(0, kFrames);

    project.addNote(Note(0, 2, 57.0f));
    project.addNote(Note(2, 4, 59.0f));
    project.addNote(Note(4, 6, 61.0f));
    return project;
}

juce::var noteJsonAt(const juce::var &json, int index) {
    auto *notes = json.getProperty("notes", juce::var()).getArray();
    CHECK(notes != nullptr);
    CHECK(index < notes->size());
    return (*notes)[index];
}

juce::String noteEngineField(const juce::var &json, int index) {
    return noteJsonAt(json, index)
        .getProperty("noteEngine", juce::var())
        .toString();
}

juce::String sha256OfBuffer(const juce::AudioBuffer<float> &buffer) {
    auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                    .getNonexistentChildFile("kinu-note-engine-test", ".raw",
                                             false);
    {
        juce::FileOutputStream out(file);
        CHECK(out.openedOk());
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            CHECK(out.write(buffer.getReadPointer(channel),
                            static_cast<size_t>(buffer.getNumSamples()) *
                                sizeof(float)));
        out.flush();
    }
    const auto sha = SHA256Utils::fileSHA256(file);
    file.deleteFile();
    CHECK(sha.isNotEmpty());
    return sha;
}

void checkSameAudio(const juce::AudioBuffer<float> &expected,
                    const juce::AudioBuffer<float> &actual) {
    CHECK(expected.getNumChannels() == actual.getNumChannels());
    CHECK(expected.getNumSamples() == actual.getNumSamples());
    for (int channel = 0; channel < expected.getNumChannels(); ++channel)
        CHECK(std::memcmp(expected.getReadPointer(channel),
                          actual.getReadPointer(channel),
                          static_cast<size_t>(expected.getNumSamples()) *
                              sizeof(float)) == 0);
}

void testThreeValuesRoundTrip() {
    Project project = makeProject();
    auto &notes = project.getNotes();
    CHECK(notes.size() == 3);

    notes[0].setEngine(NoteEngine::Inherit);
    notes[1].setEngine(NoteEngine::Psola);
    notes[2].setEngine(NoteEngine::Pitchcore);

    const auto json = ProjectSerializer::toJson(project);
    Project loaded;
    CHECK(ProjectSerializer::fromJson(loaded, json));
    CHECK(loaded.getNotes().size() == 3);
    CHECK(loaded.getNotes()[0].getEngine() == NoteEngine::Inherit);
    CHECK(loaded.getNotes()[1].getEngine() == NoteEngine::Psola);
    CHECK(loaded.getNotes()[2].getEngine() == NoteEngine::Pitchcore);

    // The same metadata JSON is what the ARA modification archive stores.
    juce::MemoryBlock archive;
    CHECK(ProjectSerializer::toBinaryArchive(
        project, archive, ProjectSerializer::BinaryArchiveMode::hostBackedARA));
    Project fromArchive;
    CHECK(ProjectSerializer::fromBinaryArchive(fromArchive, archive.getData(),
                                               archive.getSize()));
    CHECK(fromArchive.getNotes().size() == 3);
    CHECK(fromArchive.getNotes()[0].getEngine() == NoteEngine::Inherit);
    CHECK(fromArchive.getNotes()[1].getEngine() == NoteEngine::Psola);
    CHECK(fromArchive.getNotes()[2].getEngine() == NoteEngine::Pitchcore);
}

void testFieldIsOptional() {
    Project project = makeProject();
    project.getNotes()[0].setEngine(NoteEngine::Inherit);
    project.getNotes()[1].setEngine(NoteEngine::Psola);
    project.getNotes()[2].setEngine(NoteEngine::Pitchcore);

    const auto json = ProjectSerializer::toJson(project);

    // Inherit is omitted, so an untouched project (and every project written
    // before the field existed) serialises exactly as it used to.
    CHECK(!noteJsonAt(json, 0).hasProperty("noteEngine"));
    CHECK(noteEngineField(json, 1) == "PSOLA");
    CHECK(noteEngineField(json, 2) == "Pitchcore");

    CHECK(juce::String(noteEngineToString(NoteEngine::Inherit)) == "Inherit");
    CHECK(juce::String(noteEngineToString(NoteEngine::Psola)) == "PSOLA");
    CHECK(juce::String(noteEngineToString(NoteEngine::Pitchcore)) ==
          "Pitchcore");
}

void testLegacyJsonLoadsInherit() {
    Project project = makeProject();
    project.getNotes()[0].setEngine(NoteEngine::Psola);
    project.getNotes()[1].setEngine(NoteEngine::Pitchcore);

    // A project written before the field existed: same JSON, no noteEngine.
    const juce::var legacy = juce::JSON::parse(
        juce::JSON::toString(ProjectSerializer::toJson(project), false));
    auto *notes = legacy.getProperty("notes", juce::var()).getArray();
    CHECK(notes != nullptr);
    CHECK(notes->size() == 3);
    for (auto &noteVar : *notes) {
        auto *noteObject = noteVar.getDynamicObject();
        CHECK(noteObject != nullptr);
        noteObject->removeProperty("noteEngine");
    }

    Project loaded;
    CHECK(ProjectSerializer::fromJson(loaded, legacy));
    CHECK(loaded.getNotes().size() == 3);
    for (const auto &note : loaded.getNotes())
        CHECK(note.getEngine() == NoteEngine::Inherit);
}

void testUnknownValueLoadsInherit() {
    Project project = makeProject();
    auto json = ProjectSerializer::toJson(project);
    auto *notes = json.getProperty("notes", juce::var()).getArray();
    CHECK(notes != nullptr);
    auto *first = (*notes)[0].getDynamicObject();
    CHECK(first != nullptr);
    first->setProperty("noteEngine", "Vocoder");

    Project loaded;
    CHECK(ProjectSerializer::fromJson(loaded, json));
    CHECK(loaded.getNotes()[0].getEngine() == NoteEngine::Inherit);
}

void testOldProjectKeepsItsAudio() {
    // Every note is Inherit, which is what an old project loads as.
    Project original = makeProject();
    for (const auto &note : original.getNotes())
        CHECK(note.getEngine() == NoteEngine::Inherit);

    // Nothing writes the field, so the archive a loaded project re-emits is
    // the one the old code emitted.
    const auto json = ProjectSerializer::toJson(original);
    for (int i = 0; i < static_cast<int>(original.getNotes().size()); ++i)
        CHECK(!noteJsonAt(json, i).hasProperty("noteEngine"));

    const auto beforeSha = sha256OfBuffer(original.getAudioData().waveform);
    const auto beforeSourceSha =
        sha256OfBuffer(original.getAudioData().originalWaveform);

    juce::MemoryBlock archive;
    CHECK(ProjectSerializer::toBinaryArchive(
        original, archive, ProjectSerializer::BinaryArchiveMode::selfContained));

    Project loaded;
    CHECK(ProjectSerializer::fromBinaryArchive(loaded, archive.getData(),
                                               archive.getSize()));

    checkSameAudio(original.getAudioData().waveform,
                   loaded.getAudioData().waveform);
    checkSameAudio(original.getAudioData().originalWaveform,
                   loaded.getAudioData().originalWaveform);
    const auto afterSha = sha256OfBuffer(loaded.getAudioData().waveform);
    const auto afterSourceSha =
        sha256OfBuffer(loaded.getAudioData().originalWaveform);
    CHECK(afterSha == beforeSha);
    CHECK(afterSourceSha == beforeSourceSha);

    // Evidence for the acceptance record: the same edit, same audio hash.
    std::cout << "  waveform sha256 before/after: " << beforeSha << " / "
              << afterSha << "\n"
              << "  original waveform sha256 before/after: " << beforeSourceSha
              << " / " << afterSourceSha << "\n";

    for (const auto &note : loaded.getNotes())
        CHECK(note.getEngine() == NoteEngine::Inherit);
}

} // namespace

int main() {
    testThreeValuesRoundTrip();
    testFieldIsOptional();
    testLegacyJsonLoadsInherit();
    testUnknownValueLoadsInherit();
    testOldProjectKeepsItsAudio();

    std::cout << "NoteEngineTests: all checks passed\n";
    return 0;
}
