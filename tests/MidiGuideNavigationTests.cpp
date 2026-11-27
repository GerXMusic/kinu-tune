// V2 (guia MIDI): anclaje de la guia al cambiar de item y al hacer zoom, y el
// ajuste vertical que incluye la guia solo cuando esta en el tiempo visible.
//
// Se mide:
//  - con los offsets de los tres items medidos (5.818182 / 12.914433 /
//    5.818182) y pps 36, la MISMA guia host mantiene la misma x de pantalla
//    (<= 1e-6). El canvas compensa el offset moviendo scrollX (track view);
//  - setPixelsPerSecond alrededor de una x conserva esa x (zoom con cursor);
//  - MainComponent::fitAnalyzedPitchRangeToView con la guia solapando el tiempo
//    visible incluye 53-74 sin sacar las notas activas (60-62), y deja
//    ghost_visible == ghost_in_view; sin solape, no mete la guia en el zoom.
#include "TestAssert.h"

#include "../Source/Models/MidiGuideGhosts.h"
#include "../Source/Models/Project.h" // pulls JuceHeader first
#include "../Source/UI/MainComponent.h"
#include "../Source/UI/PianoRollComponent.h"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

// Friend of MainComponent (see there): the editor without a host around it.
struct MainComponentTestAccess
{
    MainComponent &mc;

    PianoRollComponent &roll() { return mc.pianoRoll; }
    void fit(Project &project) { mc.fitAnalyzedPitchRangeToView(project); }
};

namespace
{
constexpr double kOffsetA = 5.818182;  // item 1 / item 3
constexpr double kOffsetB = 12.914433; // item 2
constexpr double kProjectFirst = 7.886192;
constexpr double kProjectLast = 68.143680;
constexpr int kGhostCount = 111;
constexpr double kPps = 36.0;

int frameFor(double seconds)
{
    return static_cast<int>(
        std::llround(seconds * SAMPLE_RATE / static_cast<double>(HOP_SIZE)));
}

void setLength(Project& project, double seconds)
{
    const auto frames = static_cast<size_t>(frameFor(seconds));
    project.getAudioData().sampleRate = SAMPLE_RATE;
    project.getAudioData().waveform.setSize(
        1, static_cast<int>(frames) * HOP_SIZE);
    project.getAudioData().f0.assign(frames, 220.0f);
    project.getAudioData().basePitch.assign(frames, 60.0f);
    project.getAudioData().deltaPitch.assign(frames, 0.0f);
}

std::vector<MidiGuideGhost> makeGhosts()
{
    constexpr double step =
        (kProjectLast - kProjectFirst) / static_cast<double>(kGhostCount);
    std::vector<MidiGuideGhost> ghosts;
    ghosts.reserve(kGhostCount);
    for (int i = 0; i < kGhostCount; ++i)
    {
        MidiGuideGhost ghost;
        ghost.startHostSeconds = kProjectFirst + i * step +
                                 kOffsetA; // host = project + offset
        ghost.endHostSeconds = ghost.startHostSeconds + step;
        ghost.pitch = static_cast<float>(53 + (i % 22));
        ghost.label = "C4";
        ghosts.push_back(ghost);
    }
    return ghosts;
}

// Screen x (pixels, incluidas las teclas) del inicio de un fantasma con el
// offset que el canvas esta mostrando.
double ghostScreenX(PianoRollComponent& roll, const MidiGuideGhost& ghost)
{
    const double offset = roll.projectToTimeline(0.0);
    const double projectSeconds = ghost.startHostSeconds - offset;
    return PianoRollComponent::pianoKeysWidth +
           projectSeconds * roll.getPixelsPerSecond() - roll.getScrollX();
}

juce::Image paintToImage(PianoRollComponent& roll)
{
    juce::Image image(juce::Image::ARGB, roll.getWidth(), roll.getHeight(), true);
    juce::Graphics g(image);
    roll.paint(g);
    return image;
}

std::pair<double, double> pitchWindow(PianoRollComponent& roll)
{
    const double pps = roll.getPixelsPerSemitone();
    return {MAX_MIDI_NOTE -
                (roll.getScrollY() + roll.getVisibleContentHeight()) / pps,
            MAX_MIDI_NOTE - roll.getScrollY() / pps};
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    Project project;
    setLength(project, 130.0);

    // 1. Anclaje al cambiar de item: offsets medidos, misma guia host.
    PianoRollComponent roll;
    roll.setSize(1000, 400);
    roll.setProject(&project);
    roll.setTrackViewMode(true);
    roll.setTimelineDisplayOffset(kOffsetA);
    roll.setPixelsPerSecond(static_cast<float>(kPps));
    roll.setScrollX(1000.0);
    const auto ghosts = makeGhosts();
    roll.setMidiGuideGhosts(ghosts);
    paintToImage(roll);
    CHECK(roll.getMidiGuideGhostsInViewCount() > 0);
    CHECK(roll.getMidiGuideGhostsPaintedLast() ==
          static_cast<int>(roll.getMidiGuideGhostsInViewCount()));

    const double xA = ghostScreenX(roll, ghosts[40]);
    const double xA2 = ghostScreenX(roll, ghosts[80]);

    roll.setTimelineDisplayOffset(kOffsetB);
    paintToImage(roll);
    const double xB = ghostScreenX(roll, ghosts[40]);
    const double xB2 = ghostScreenX(roll, ghosts[80]);
    std::printf("offset_anchor A=%.9f B=%.9f dx=%.12f dx2=%.12f\n", xA, xB,
                xB - xA, xB2 - xA2);
    CHECK(std::abs(xB - xA) <= 1.0e-6);
    CHECK(std::abs(xB2 - xA2) <= 1.0e-6);
    CHECK(roll.getMidiGuideGhostsPaintedLast() ==
          static_cast<int>(roll.getMidiGuideGhostsInViewCount()));

    roll.setTimelineDisplayOffset(kOffsetA);
    paintToImage(roll);
    const double xBack = ghostScreenX(roll, ghosts[40]);
    std::printf("offset_anchor back=%.9f dx=%.12f\n", xBack, xBack - xA);
    CHECK(std::abs(xBack - xA) <= 1.0e-6);

    // 2. Zoom con cursor: la x bajo el cursor no se mueve.
    roll.setPixelsPerSecond(static_cast<float>(kPps));
    roll.setScrollX(1000.0);
    const double offset = roll.projectToTimeline(0.0);
    const double cursorProject = ghosts[40].startHostSeconds - offset;
    roll.setCursorTime(cursorProject);
    const double xBeforeZoom =
        PianoRollComponent::pianoKeysWidth + cursorProject * kPps - 1000.0;
    roll.setPixelsPerSecond(72.0f, true);
    const double xAfterZoom =
        PianoRollComponent::pianoKeysWidth +
        cursorProject * roll.getPixelsPerSecond() - roll.getScrollX();
    std::printf("zoom_anchor pps=%.3f x_before=%.9f x_after=%.9f dx=%.12f\n",
                roll.getPixelsPerSecond(), xBeforeZoom, xAfterZoom,
                xAfterZoom - xBeforeZoom);
    // Anclaje en pixeles: el scroll de setPixelsPerSecond es float, asi que el
    // residuo es ~1e-4 px; por encima del error de redondeo seria un salto.
    CHECK(std::abs(xAfterZoom - xBeforeZoom) <= 1.0e-3);

    // 3. Ajuste vertical a traves del camino real del editor. Notas activas
    //    60 y 62 (dentro de 53-74) y la guia de 53-74 en el tiempo visible.
    Project active;
    setLength(active, 130.0);
    active.getNotes().push_back(Note(frameFor(10.0), frameFor(12.0), 60.0f));
    active.getNotes().push_back(Note(frameFor(14.0), frameFor(16.0), 62.0f));

    MainComponent mc(false);
    mc.setSize(1000, 400);
    MainComponentTestAccess access{mc};
    auto& pianoRoll = access.roll();
    pianoRoll.setSize(1000, 400);
    pianoRoll.setProject(&active);
    pianoRoll.setTimelineDisplayOffset(kOffsetA);
    pianoRoll.setMidiGuideGhosts(ghosts);
    pianoRoll.setPixelsPerSecond(static_cast<float>(kPps));
    pianoRoll.setScrollX(0.0);
    CHECK(pianoRoll.getVisibleContentHeight() > 0);

    const auto guidePitch =
        pianoRoll.getMidiGuideGhostPitchRangeInVisibleTime();
    const auto navRange = pianoRoll.getVisibleProjectRange();
    std::printf("nav_range=[%.3f,%.3f] scrollX=%.3f guide_pitch=[%.3f,%.3f]\n",
                navRange.first, navRange.second, pianoRoll.getScrollX(),
                guidePitch.first, guidePitch.second);
    CHECK(guidePitch.first == 53.0f);
    CHECK(guidePitch.second == 74.0f);

    access.fit(active);
    paintToImage(pianoRoll);
    const auto window = pitchWindow(pianoRoll);
    std::printf("fit_guide pitch=[%.3f,%.3f] in_view=%zu visible=%zu "
                "painted=%d\n",
                window.first, window.second,
                pianoRoll.getMidiGuideGhostsInViewCount(),
                pianoRoll.getMidiGuideGhostsVisibleCount(),
                pianoRoll.getMidiGuideGhostsPaintedLast());
    CHECK(window.second >= 74.0);
    CHECK(window.first <= 53.0);
    // Las notas activas siguen dentro.
    CHECK(window.first <= 60.0);
    CHECK(window.second >= 62.0);
    CHECK(pianoRoll.getMidiGuideGhostsVisibleCount() ==
          pianoRoll.getMidiGuideGhostsInViewCount());
    CHECK(pianoRoll.getMidiGuideGhostsPaintedLast() ==
          static_cast<int>(pianoRoll.getMidiGuideGhostsInViewCount()));

    // 4. Sin la guia en el tiempo visible, el ajuste NO la mete en el zoom:
    //    la ventana vertical sale del rango de las notas activas.
    pianoRoll.setPixelsPerSecond(static_cast<float>(kPps));
    pianoRoll.setScrollX(80.0 * kPps); // proyecto 80-106 s: guia hasta 68.14
    const auto noGuide = pianoRoll.getMidiGuideGhostPitchRangeInVisibleTime();
    CHECK(noGuide.first > noGuide.second);
    access.fit(active);
    const auto notesWindow = pitchWindow(pianoRoll);
    std::printf("fit_no_guide pitch=[%.3f,%.3f]\n", notesWindow.first,
                notesWindow.second);
    CHECK(notesWindow.second < 74.0);
    CHECK(notesWindow.first > 53.0);
    CHECK(notesWindow.first <= 60.0);
    CHECK(notesWindow.second >= 62.0);

    std::cout << "MidiGuideNavigationTests passed (V2: host anchor + fit)\n";
    return 0;
}
