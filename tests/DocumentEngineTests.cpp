// Per-document synthesis engine (D-174, I-19).
//
// I-19: two different audio documents, each with its own Kinu Tune and a
// different engine, collapse to the last modified engine after save/reopen.
// The engine that the editor and the renderers use has to travel inside the
// document (the Project metadata the ARA modification archive stores), not
// only in the process-wide settings file.
//
// These checks cover the serialization half: a document that carries an
// engine keeps its own through a JSON and binary round trip, two documents
// with different engines do not contaminate each other, a document without
// the field stays without it (legacy fallback to the shared preference), and
// the per-note engine map still survives on its own.
//
// Before the fix the JSON round trip dropped the property (fromJson ignored
// it and toJson did not write it), so the first check failed.

#include "../Source/Models/Project.h"
#include "../Source/Models/ProjectSerializer.h"
#include "../Source/Utils/Constants.h"
#include "TestAssert.h"

#include <iostream>

namespace {

Project makeProject() {
    Project project;
    auto &audio = project.getAudioData();
    audio.sampleRate = SAMPLE_RATE;
    audio.f0.assign(8, 220.0f);
    audio.rawF0 = audio.f0;
    audio.cleanedF0 = audio.f0;
    audio.denseF0 = audio.f0;

    project.addNote(Note(0, 2, 57.0f));
    project.addNote(Note(2, 4, 59.0f));
    project.addNote(Note(4, 6, 61.0f));
    project.addNote(Note(6, 8, 63.0f));
    return project;
}

juce::var jsonWithEngine(const Project &project, const juce::String &engine) {
    auto json = ProjectSerializer::toJson(project, false, false);
    json.getDynamicObject()->setProperty("synthesisEngine", engine);
    return json;
}

juce::String engineField(const juce::var &json) {
    return json.getProperty("synthesisEngine", juce::var()).toString();
}

Project restoredFrom(const juce::var &json) {
    Project restored;
    CHECK(ProjectSerializer::fromJson(restored, json));
    return restored;
}

void documentEngineSurvivesTheJsonRoundTrip() {
    for (const auto *engine : {"PSOLA", "Pitchcore"}) {
        const auto source = makeProject();
        const auto restored = restoredFrom(jsonWithEngine(source, engine));
        const auto again =
            ProjectSerializer::toJson(restored, false, false);
        CHECK(engineField(again) == juce::String(engine));
    }
}

void twoDocumentsKeepTheirOwnEngine() {
    const auto first = restoredFrom(jsonWithEngine(makeProject(), "PSOLA"));
    const auto second =
        restoredFrom(jsonWithEngine(makeProject(), "Pitchcore"));

    const auto firstAgain = ProjectSerializer::toJson(first, false, false);
    const auto secondAgain = ProjectSerializer::toJson(second, false, false);
    CHECK(engineField(firstAgain) == "PSOLA");
    CHECK(engineField(secondAgain) == "Pitchcore");
}

void absentEngineStaysAbsent() {
    const auto source = makeProject();
    const auto restored = restoredFrom(
        ProjectSerializer::toJson(source, false, false));
    const auto again = ProjectSerializer::toJson(restored, false, false);
    CHECK(!again.hasProperty("synthesisEngine"));
}

void perNoteEngineMapStillSurvives() {
    auto source = makeProject();
    auto &notes = source.getNotes();
    notes[0].setEngine(NoteEngine::Psola);
    notes[1].setEngine(NoteEngine::Pitchcore);
    notes[2].setEngine(NoteEngine::Psola);
    notes[3].setEngine(NoteEngine::Pitchcore);

    const auto restored =
        restoredFrom(ProjectSerializer::toJson(source, false, false));
    const auto &restoredNotes = restored.getNotes();
    CHECK(restoredNotes[0].getEngine() == NoteEngine::Psola);
    CHECK(restoredNotes[1].getEngine() == NoteEngine::Pitchcore);
    CHECK(restoredNotes[2].getEngine() == NoteEngine::Psola);
    CHECK(restoredNotes[3].getEngine() == NoteEngine::Pitchcore);
}

} // namespace

int main() {
    documentEngineSurvivesTheJsonRoundTrip();
    twoDocumentsKeepTheirOwnEngine();
    absentEngineStaysAbsent();
    perNoteEngineMapStillSurvives();
    std::cout << "DocumentEngineTests passed" << std::endl;
    return 0;
}
