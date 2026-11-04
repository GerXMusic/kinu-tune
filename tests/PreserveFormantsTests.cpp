// Global "Preserve formants" switch (D-166).
//
// The switch is a project/plugin setting, off by default. It only reaches the
// Pitchcore engine: the fork's PSOLA ignores the field and the vocoder does not
// take a DspSynthesisRequest at all.
//
// These checks cover the three things that can regress without a DAW:
//   1. the project flag survives JSON and binary archives, and a project
//      written before the field existed loads with it off;
//   2. turning it on counts as a region edit, so the ARA store path keeps the
//      published audio after a render;
//   3. the project flag is what the Pitchcore adapter copies into
//      RenderRequest::preserveFormants (on and off).

#include "../Source/Audio/Synthesis/PitchcoreRequest.h"
#include "../Source/Models/Note.h"
#include "../Source/Models/Project.h"
#include "../Source/Models/ProjectSerializer.h"
#include "../Source/Utils/Constants.h"
#include "TestAssert.h"

#include <iostream>

namespace {

Project makeProject() {
    Project project;
    project.addNote(Note(0, 2, 57.0f));
    project.addNote(Note(2, 4, 59.0f));
    return project;
}

void testDefaultIsOffAndOldProjectLoadsOff() {
    Project project;
    CHECK(!project.getPreserveFormants());

    // Off is the default and is omitted, so a project written before the field
    // existed keeps serialising exactly as it used to.
    auto json = ProjectSerializer::toJson(project);
    CHECK(!json.hasProperty("preserveFormants"));

    Project loaded;
    CHECK(ProjectSerializer::fromJson(loaded, json));
    CHECK(!loaded.getPreserveFormants());

    // Same for an explicit legacy object with no property at all.
    auto legacy = ProjectSerializer::toJson(makeProject());
    CHECK(!legacy.hasProperty("preserveFormants"));
    Project fromLegacy;
    CHECK(ProjectSerializer::fromJson(fromLegacy, legacy));
    CHECK(!fromLegacy.getPreserveFormants());
}

void testOnSurvivesJsonAndBinaryArchive() {
    Project project = makeProject();
    project.setPreserveFormants(true);
    CHECK(project.getPreserveFormants());

    const auto json = ProjectSerializer::toJson(project);
    CHECK(static_cast<bool>(json.getProperty("preserveFormants", false)));

    Project loaded;
    CHECK(ProjectSerializer::fromJson(loaded, json));
    CHECK(loaded.getPreserveFormants());

    juce::MemoryBlock archive;
    CHECK(ProjectSerializer::toBinaryArchive(
        project, archive, ProjectSerializer::BinaryArchiveMode::hostBackedARA));
    Project fromArchive;
    CHECK(ProjectSerializer::fromBinaryArchive(fromArchive, archive.getData(),
                                               archive.getSize()));
    CHECK(fromArchive.getPreserveFormants());
}

void testSwitchMarksRegionEditAndSurvivesRenderBookkeeping() {
    Project project = makeProject();
    CHECK(!project.hasRegionEdits());

    project.setPreserveFormants(true);
    CHECK(project.hasRegionEdits());

    // The ARA publish gate reads hasRegionEdits after the render has copied the
    // rendered note state back into the persistent project. It must still be
    // true then, or the save clears the processed audio (D-163 family).
    Project rendered = project;
    rendered.clearAllDirty();
    project.applyRenderedNoteState(rendered);
    CHECK(project.hasRegionEdits());

    project.setPreserveFormants(false);
    CHECK(!project.hasRegionEdits());
}

void testProjectFlagReachesPitchcoreRenderRequest() {
    Project project;
    project.setPreserveFormants(true);

    DspSynthesisRequest request;
    request.preserveFormants = project.getPreserveFormants();
    auto core = pitchcore_adapter::toCoreRequest(request);
    CHECK(core.preserveFormants);

    project.setPreserveFormants(false);
    request.preserveFormants = project.getPreserveFormants();
    core = pitchcore_adapter::toCoreRequest(request);
    CHECK(!core.preserveFormants);
}

} // namespace

int main() {
    testDefaultIsOffAndOldProjectLoadsOff();
    testOnSurvivesJsonAndBinaryArchive();
    testSwitchMarksRegionEditAndSurvivesRenderBookkeeping();
    testProjectFlagReachesPitchcoreRenderRequest();

    std::cout << "PreserveFormantsTests: all checks passed\n";
    return 0;
}
