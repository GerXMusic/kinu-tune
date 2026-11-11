// D-169: the global "Preserve formants" switch is retired.
//
// Two things must hold with no DAW:
//   1. the Pitchcore adapter always builds a RenderRequest with
//      preserveFormants = false, and DspSynthesisRequest has no member that
//      could turn it on (there is no remaining route to true);
//   2. a project JSON written before D-169, which still contains the
//      "preserveFormants" key, loads without error and the key is ignored.

#include "../Source/Audio/Synthesis/PitchcoreRequest.h"
#include "../Source/Models/Note.h"
#include "../Source/Models/Project.h"
#include "../Source/Models/ProjectSerializer.h"
#include "TestAssert.h"

#include <iostream>
#include <type_traits>
#include <utility>

namespace {

template <typename T, typename = void>
struct HasPreserveFormantsMember : std::false_type {};

template <typename T>
struct HasPreserveFormantsMember<
    T, std::void_t<decltype(std::declval<T &>().preserveFormants)>>
    : std::true_type {};

Project makeProject() {
    Project project;
    project.addNote(Note(0, 2, 57.0f));
    project.addNote(Note(2, 4, 59.0f));
    return project;
}

void testRequestIsAlwaysFalse() {
    // B: after D-169 there is no per-request member, so no caller can ask the
    // plugin to preserve formants; the adapter still fixes it to false.
    CHECK(!HasPreserveFormantsMember<DspSynthesisRequest>::value);

    DspSynthesisRequest request;
    const auto core = pitchcore_adapter::toCoreRequest(request);
    CHECK(!core.preserveFormants);
}

void testLegacyJsonWithSwitchLoads() {
    // D: a project saved while D-166 existed carries the key. It must load
    // without error and without a crash; the key has no reader any more.
    auto legacy = ProjectSerializer::toJson(makeProject());
    auto *object = legacy.getDynamicObject();
    CHECK(object != nullptr);
    if (object == nullptr)
        return;
    object->setProperty("preserveFormants", true);

    Project loaded;
    CHECK(ProjectSerializer::fromJson(loaded, legacy));
    CHECK(loaded.getNotes().size() == 2);
}

void testSwitchIsNotWrittenBack() {
    // After D-169 there is no state to persist: a project that still carries
    // the old key must not write it back.
    auto legacy = ProjectSerializer::toJson(makeProject());
    auto *object = legacy.getDynamicObject();
    CHECK(object != nullptr);
    if (object == nullptr)
        return;
    object->setProperty("preserveFormants", true);

    Project loaded;
    CHECK(ProjectSerializer::fromJson(loaded, legacy));
    const auto rewritten = ProjectSerializer::toJson(loaded);
    CHECK(!rewritten.hasProperty("preserveFormants"));
}

} // namespace

int main() {
    testLegacyJsonWithSwitchLoads();
    testRequestIsAlwaysFalse();
    testSwitchIsNotWrittenBack();

    std::cout << "FormantRetirementTests: all checks passed\n";
    return 0;
}
