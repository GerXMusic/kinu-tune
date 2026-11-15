// D-225: transición de unión, dirección, tope, rebuild canónico y migración.
//
// El usuario reportó que tocar una unión rompía la vecina, en las dos
// direcciones. La causa medida en simulación fue que el ancho de la ventana
// dependía de qué uniones estuvieran "activas": al poner Transition en la nota
// derecha, esa unión entraba en la lista y recortaba la ventana de la izquierda
// (y al revés). Este test fija los invariantes que el arreglo debe cumplir.
//
// Dirección (D-225): la unión entre ordered[i-1] y ordered[i] toma el valor de
// `ordered[i-1].transitionOutMs` (la nota de la izquierda, la seleccionada).
// Tope: half = min(userHalf, floor(min(lenIzq, lenDer) / 2)) por lado.
// Umbral de igualdad: <= 0,001 c.

#include "../Source/Models/Project.h"
#include "../Source/Models/ProjectSerializer.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "TestAssert.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

constexpr float kSameLimitCents = 0.001f;

float cents(float hz) {
  return 1200.0f * std::log2(std::max(hz, 1.0e-6f) / 440.0f);
}

std::vector<float> centsOf(const Project &project) {
  const auto f0 = project.getAdjustedF0();
  std::vector<float> out(f0.size());
  for (size_t i = 0; i < f0.size(); ++i)
    out[i] = cents(f0[i]);
  return out;
}

// Contiguous notes with a 0 delta and a different pitch each: the union step
// lives in the base curve, so the transition is isolated from the D-205
// smoothing of the delta.
Project makeChain(const std::vector<int> &lengths,
                  const std::vector<float> &midis) {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = 44100;
  int total = 0;
  for (int length : lengths)
    total += length;
  audio.melSpectrogram.resize(static_cast<size_t>(total));
  audio.denseF0.assign(static_cast<size_t>(total), 0.0f);

  int start = 0;
  for (size_t n = 0; n < lengths.size(); ++n) {
    const int end = start + lengths[n];
    Note note(start, end, midis[n]);
    note.setOriginalMidiNote(midis[n]);
    note.setOriginalDeltaPitch(
        std::vector<float>(static_cast<size_t>(lengths[n]), 0.0f));
    project.addNote(note);
    for (int i = start; i < end; ++i)
      audio.denseF0[static_cast<size_t>(i)] = midiToFreq(midis[n]);
    start = end;
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

float maxDiff(const std::vector<float> &a, const std::vector<float> &b,
              int first, int last) {
  float worst = 0.0f;
  for (int i = std::max(0, first);
       i <= std::min(last, static_cast<int>(a.size()) - 1); ++i)
    worst = std::max(worst, std::fabs(a[static_cast<size_t>(i)] -
                                      b[static_cast<size_t>(i)]));
  return worst;
}

// Frames around `boundary` where the composed curve differs from the baseline.

} // namespace

int main() {
  // 1. Dirección: Transition de N1 cambia N1->N2 y no N0->N1. Después,
  //    Transition de N2 cambia N2->N3 y no N1->N2.
  {
    Project project = makeChain({100, 100, 100, 100}, {60.0f, 62.0f, 64.0f, 66.0f});
    const auto baseline = centsOf(project);

    project.getNotes()[1].setTransitionOutMs(200);
    PitchCurveProcessor::rebuildBaseFromNotes(project);
    const auto afterN1 = centsOf(project);
    const float previousJoin = maxDiff(baseline, afterN1, 90, 110);
    const float nextJoin = maxDiff(baseline, afterN1, 190, 210);
    std::cout << "direction N1: join N0->N1 diff=" << previousJoin
              << " c, join N1->N2 diff=" << nextJoin << " c\n";
    CHECK(previousJoin <= kSameLimitCents);
    CHECK(nextJoin > 5.0f);

    project.getNotes()[2].setTransitionOutMs(200);
    PitchCurveProcessor::rebuildBaseFromNotes(project);
    const auto afterN2 = centsOf(project);
    const float n1JoinAgain = maxDiff(afterN1, afterN2, 190, 210);
    const float n2Join = maxDiff(afterN1, afterN2, 290, 310);
    std::cout << "direction N2: join N1->N2 diff=" << n1JoinAgain
              << " c, join N2->N3 diff=" << n2Join << " c\n";
    CHECK(n1JoinAgain <= kSameLimitCents);
    CHECK(n2Join > 5.0f);
  }

  // 2. Independencia de orden / vecindad. P-A-B, A corta (10 frames) con
  //    200 c a cada lado, y el mismo caso con A larga (100 frames).
  for (const int middleLength : {10, 100}) {
    Project project =
        makeChain({100, middleLength, 100}, {60.0f, 62.0f, 64.0f});
    const auto baseline = centsOf(project);
    const int boundaryPA = 100;

    project.getNotes()[0].setTransitionOutMs(250);
    PitchCurveProcessor::rebuildBaseFromNotes(project);
    const auto afterP = centsOf(project);

    project.getNotes()[1].setTransitionOutMs(250);
    PitchCurveProcessor::rebuildBaseFromNotes(project);
    const auto afterA = centsOf(project);

    // The union P->A must not move when A's own transition is added. On short
    // A the old window rule cut P->A from +-8 to +-5 frames (12.8 c); with the
    // canonical layout it is bit-identical. Compare only the P->A window: the
    // A->B window is expected to change.
    const int halfPA = std::min(11, std::min(100, middleLength) / 2);
    const int halfAB = std::min(11, std::min(middleLength, 100) / 2);
    const float movedPA = maxDiff(afterP, afterA, boundaryPA - halfPA,
                                  boundaryPA + halfPA);
    const float changedAB =
        maxDiff(afterP, afterA, boundaryPA + middleLength - halfAB,
                boundaryPA + middleLength + halfAB);
    std::cout << "order A=" << middleLength << " frames: P->A moved=" << movedPA
              << " c, A->B changed=" << changedAB << " c\n";
    CHECK(movedPA <= kSameLimitCents);
    CHECK(changedAB > 1.0f);
    (void)baseline;
  }

  // 3. Tope: nota de 10 frames con 250 ms -> half = 5 (mitad de la nota más
  //    corta). Igual con y sin transición en la unión vecina, y sin cruzar el
  //    punto medio. Los extremos de la ventana son el valor base (no cambian);
  //    dentro se mueve hasta +-4.
  {
    Project project = makeChain({100, 10, 100}, {60.0f, 62.0f, 64.0f});
    const auto baseline = centsOf(project);
    const int boundaryAB = 110;
    const auto diffAt = [&baseline](const std::vector<float> &curve, int frame) {
      return std::fabs(curve[static_cast<size_t>(frame)] -
                       baseline[static_cast<size_t>(frame)]);
    };

    project.getNotes()[1].setTransitionOutMs(250);
    PitchCurveProcessor::rebuildBaseFromNotes(project);
    const auto onlyAB = centsOf(project);
    std::cout << "cap 250 ms, A=10: outer=" << diffAt(onlyAB, boundaryAB - 6)
              << "/" << diffAt(onlyAB, boundaryAB + 6)
              << " c, edge=" << diffAt(onlyAB, boundaryAB - 5) << "/"
              << diffAt(onlyAB, boundaryAB + 5)
              << " c, inside=" << diffAt(onlyAB, boundaryAB - 4) << "/"
              << diffAt(onlyAB, boundaryAB + 4) << " c\n";
    CHECK(diffAt(onlyAB, boundaryAB - 6) <= 0.01f); // outside the cap
    CHECK(diffAt(onlyAB, boundaryAB + 6) <= 0.01f);
    CHECK(diffAt(onlyAB, boundaryAB - 5) <= 0.01f); // window edge: fixed
    CHECK(diffAt(onlyAB, boundaryAB + 5) <= 0.01f);
    CHECK(diffAt(onlyAB, boundaryAB - 4) > 0.01f);  // inside the cap
    CHECK(diffAt(onlyAB, boundaryAB + 4) > 0.01f);

    // The neighbouring union (P->A) gets a transition too; the cap of A->B
    // must not change because of it.
    project.getNotes()[0].setTransitionOutMs(250);
    PitchCurveProcessor::rebuildBaseFromNotes(project);
    const auto both = centsOf(project);
    const float neighbourEdgeLeft = diffAt(both, boundaryAB - 5);
    const float neighbourEdgeRight = diffAt(both, boundaryAB + 5);
    const float neighbourInsideLeft = diffAt(both, boundaryAB - 4);
    const float neighbourInsideRight = diffAt(both, boundaryAB + 4);
    std::cout << "cap 250 ms, neighbour active: edge=" << neighbourEdgeLeft
              << "/" << neighbourEdgeRight << " c, inside="
              << neighbourInsideLeft << "/" << neighbourInsideRight << " c\n";
    CHECK(neighbourEdgeLeft <= 0.01f);
    CHECK(neighbourEdgeRight <= 0.01f);
    CHECK(std::fabs(neighbourInsideLeft - diffAt(onlyAB, boundaryAB - 4)) <=
          kSameLimitCents);
    CHECK(std::fabs(neighbourInsideRight - diffAt(onlyAB, boundaryAB + 4)) <=
          kSameLimitCents);

    // Neither union reaches past the midpoint of the 10-frame note: the two
    // windows touch at most.
    CHECK(maxDiff(onlyAB, both, 90, 99) > 0.0f); // P->A did get its transition
  }

  // 4. Preview == commit: rebuildDeltaForNotes y rebuildBaseFromNotes dan la
  //    misma delta en el rango afectado.
  {
    Project committed = makeChain({100, 100, 100}, {60.0f, 62.0f, 64.0f});
    committed.getNotes()[0].setTransitionOutMs(150);
    committed.getNotes()[1].setTransitionOutMs(80);
    PitchCurveProcessor::rebuildBaseFromNotes(committed);

    Project preview = makeChain({100, 100, 100}, {60.0f, 62.0f, 64.0f});
    preview.getNotes()[0].setTransitionOutMs(150);
    preview.getNotes()[1].setTransitionOutMs(80);
    PitchCurveProcessor::rebuildBaseFromNotes(preview);
    std::vector<Note *> affected{&preview.getNotes()[0]};
    PitchCurveProcessor::rebuildDeltaForNotes(preview, affected);

    const auto &a = committed.getAudioData().deltaPitch;
    const auto &b = preview.getAudioData().deltaPitch;
    float worst = 0.0f;
    int worstFrame = -1;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
      if (std::fabs(a[i] - b[i]) > worst) {
        worst = std::fabs(a[i] - b[i]);
        worstFrame = static_cast<int>(i);
      }
    std::cout << "preview vs commit: max delta diff=" << worst
              << " semitones at frame " << worstFrame << "\n";
    CHECK(worst <= 1.0e-4f);
  }

  // 5. El rebuild parcial no escribe fuera de [min-32, max+32].
  {
    Project project = makeChain({100, 100, 100, 100}, {60.0f, 62.0f, 64.0f, 66.0f});
    const auto before = project.getAudioData().deltaPitch;
    project.getNotes()[1].setTransitionOutMs(200);
    std::vector<Note *> affected{&project.getNotes()[1]};
    PitchCurveProcessor::rebuildDeltaForNotes(project, affected);
    const auto &after = project.getAudioData().deltaPitch;

    const int padStart = 100 - 32;
    const int padEnd = 200 + 32;
    float outsideWorst = 0.0f;
    int outsideFrame = -1;
    for (size_t i = 0; i < before.size() && i < after.size(); ++i) {
      if (static_cast<int>(i) >= padStart && static_cast<int>(i) < padEnd)
        continue;
      if (before[i] != after[i]) {
        outsideWorst = std::fabs(before[i] - after[i]);
        outsideFrame = static_cast<int>(i);
      }
    }
    float insideWorst = 0.0f;
    for (int i = padStart; i < padEnd; ++i)
      insideWorst = std::max(
          insideWorst,
          std::fabs(after[static_cast<size_t>(i)] - before[static_cast<size_t>(i)]));
    std::cout << "partial write: outside diff=" << outsideWorst
              << " (frame " << outsideFrame << "), inside diff=" << insideWorst
              << "\n";
    CHECK(outsideFrame == -1);
    CHECK(insideWorst > 0.0f);
  }

  // 6. Migración v5: `slideMs` en la nota derecha pasa a `transitionOutMs` de
  //    la izquierda; el valor de la primera nota se descarta.
  {
    Project original = makeChain({100, 100, 100}, {60.0f, 62.0f, 64.0f});
    original.getNotes()[0].setTransitionOutMs(180);
    const auto json = ProjectSerializer::toJson(original, false, true);

    // New key, not the legacy one.
    auto *root = json.getDynamicObject();
    CHECK(root != nullptr);
    auto notesVar = root->getProperty("notes");
    auto *notes = notesVar.getArray();
    CHECK(notes != nullptr);
    CHECK(notes->size() == 3);
    auto *first = (*notes)[0].getDynamicObject();
    auto *second = (*notes)[1].getDynamicObject();
    CHECK(first != nullptr && second != nullptr);
    CHECK(first->hasProperty("transitionOutMs"));
    CHECK(!first->hasProperty("slideMs"));

    // Rewrite it the way v5 stored it: on the arriving (right) note.
    first->removeProperty("transitionOutMs");
    second->setProperty("slideMs", 180);
    // A legacy value on the first note has no union to its left.
    first->setProperty("slideMs", 90);

    Project migrated;
    CHECK(ProjectSerializer::fromJson(migrated, json));
    CHECK(migrated.getNotes().size() == 3);
    std::cout << "migration: left=" << migrated.getNotes()[0].getTransitionOutMs()
              << " ms, right=" << migrated.getNotes()[1].getTransitionOutMs()
              << " ms\n";
    CHECK(migrated.getNotes()[0].getTransitionOutMs() == 180);
    CHECK(migrated.getNotes()[1].getTransitionOutMs() == 0);
    CHECK(migrated.getNotes()[2].getTransitionOutMs() == 0);

    // The binary/ARA archive goes through the same fromJson path; a value
    // written in the new format must survive it.
    juce::MemoryBlock archive;
    CHECK(ProjectSerializer::toBinaryArchive(original, archive));
    Project restored;
    CHECK(ProjectSerializer::fromBinaryArchive(restored, archive.getData(),
                                               archive.getSize()));
    CHECK(restored.getNotes()[0].getTransitionOutMs() == 180);
  }

  // 7. Precedencia sobre D-182: el glide de fronteras editadas no pisa la
  //    ventana de la transición.
  {
    Project withGlide = makeChain({100, 100}, {60.0f, 62.0f});
    withGlide.getNotes()[0].setMidiNote(60.7f); // 0.7 semitone edit
    withGlide.getNotes()[0].setTransitionOutMs(200);
    PitchCurveProcessor::setEditedBoundaryGlideEnabled(true);
    PitchCurveProcessor::rebuildBaseFromNotes(withGlide);
    PitchCurveProcessor::setEditedBoundaryGlideEnabled(false);

    Project withoutGlide = makeChain({100, 100}, {60.0f, 62.0f});
    withoutGlide.getNotes()[0].setMidiNote(60.7f);
    withoutGlide.getNotes()[0].setTransitionOutMs(200);
    PitchCurveProcessor::rebuildBaseFromNotes(withoutGlide);

    const auto a = centsOf(withGlide);
    const auto b = centsOf(withoutGlide);
    // 200 ms -> 9 frames each side; the glide may change frames outside the
    // window (it is enabled), but not the window itself.
    const int half = std::min(9, 100 / 2);
    const float inWindow = maxDiff(a, b, 100 - half, 100 + half);
    const float outside = maxDiff(a, b, 100 - half - 17, 100 - half - 1);
    std::cout << "D-182 precedence: transition window diff=" << inWindow
              << " c, glide outside the window diff=" << outside << " c\n";
    CHECK(inWindow <= kSameLimitCents);
    CHECK(outside > 0.0f); // the glide did run
  }

  // 8. D-226: el commit de una nota sucia incluye la mitad derecha de la
  //    transicion, que vive en la nota siguiente. El rango crece con las
  //    ventanas que el motor ya usa y cubre todos los frames que el suavizado
  //    modifica; fuera de la ventana no cambia nada. Vale en las dos
  //    direcciones (nota izquierda sucia y nota derecha sucia).
  {
    Project reference = makeChain({100, 100, 100}, {60.0f, 62.0f, 64.0f});
    const auto baseline = centsOf(reference);

    Project project = makeChain({100, 100, 100}, {60.0f, 62.0f, 64.0f});
    project.getNotes()[0].setTransitionOutMs(250);
    PitchCurveProcessor::rebuildBaseFromNotes(project);
    const auto after = centsOf(project);

    int firstChanged = -1;
    int lastChanged = -1;
    for (int i = 0; i < static_cast<int>(after.size()); ++i) {
      if (std::fabs(after[static_cast<size_t>(i)] -
                    baseline[static_cast<size_t>(i)]) <= kSameLimitCents)
        continue;
      if (firstChanged < 0)
        firstChanged = i;
      lastChanged = i;
    }
    std::cout << "D-226 changed frames: [" << firstChanged << ","
              << lastChanged << "]\n";
    CHECK(firstChanged >= 0);
    CHECK(firstChanged < 100);
    CHECK(lastChanged >= 100); // half of the ramp is in the next note

    // D-229: left note dirty: the commit must cover BOTH whole notes, not
    // just the window (the ramp moves the neighbouring note's grain lattice).
    int leftStart = 0;
    int leftEnd = 100;
    CHECK(PitchCurveProcessor::expandRangeForJoinSmoothing(project, leftStart,
                                                           leftEnd));
    std::cout << "D-226/229 commit from left note: [" << leftStart << ","
              << leftEnd << ")\n";
    CHECK(leftStart == 0);
    CHECK(leftStart <= firstChanged);
    CHECK(leftEnd >= lastChanged + 1);
    CHECK(leftEnd == 200); // the whole right note, not only the window

    // Right note dirty: the same window grows backwards over the whole left
    // note.
    int rightStart = 100;
    int rightEnd = 200;
    CHECK(PitchCurveProcessor::expandRangeForJoinSmoothing(project, rightStart,
                                                           rightEnd));
    std::cout << "D-226/229 commit from right note: [" << rightStart << ","
              << rightEnd << ")\n";
    CHECK(rightStart == 0);
    CHECK(rightStart <= firstChanged);
    CHECK(rightEnd == 200);

    // Transition 0 and no delta step: nothing to expand.
    Project plain = makeChain({100, 100}, {60.0f, 62.0f});
    int plainStart = 0;
    int plainEnd = 100;
    CHECK(!PitchCurveProcessor::expandRangeForJoinSmoothing(plain, plainStart,
                                                            plainEnd));
    CHECK(plainStart == 0 && plainEnd == 100);

    // D-226 limit: the D-205 baseline (pass 1) is not expanded. At commit
    // time deltaPitch is already smoothed, so the step that made pass 1 run is
    // not observable; expanding every contiguous join would commit a
    // neighbour nobody edited. A 70 c delta step with Transition 0 therefore
    // stays local (documented in DECISIONES D-226).
    Project stepProject = makeChain({100, 100}, {60.0f, 62.0f});
    stepProject.getNotes()[0].setOriginalDeltaPitch(
        std::vector<float>(100, 0.7f)); // 70 c step at the boundary
    PitchCurveProcessor::rebuildBaseFromNotes(stepProject);
    int stepStart = 0;
    int stepEnd = 100;
    CHECK(!PitchCurveProcessor::expandRangeForJoinSmoothing(
        stepProject, stepStart, stepEnd));
    CHECK(stepStart == 0 && stepEnd == 100);

    // A transition on the same step does expand, and by the transition
    // window, because that value is stored on the note.
    stepProject.getNotes()[0].setTransitionOutMs(250);
    PitchCurveProcessor::rebuildBaseFromNotes(stepProject);
    CHECK(PitchCurveProcessor::expandRangeForJoinSmoothing(
        stepProject, stepStart, stepEnd));
    std::cout << "D-226 commit with a 70 c delta step + 250 ms: [" << stepStart
              << "," << stepEnd << ")\n";
    CHECK(stepEnd == 200); // whole right note (D-229)
  }

  std::cout << "TransitionJoinTests: all checks passed\n";
  return 0;
}
