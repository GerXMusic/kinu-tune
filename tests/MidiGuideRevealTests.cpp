// V4c (guia MIDI): boton "Ver guia" (solo accion del usuario, D-378).
//
// 1. Funcion pura computeMidiGuideRevealPlan:
//    - guia dentro del tiempo visible -> sin scroll, pitch = guia;
//    - guia fuera (derecha e izquierda) -> scrollX minimo al inicio de la guia;
//    - activas dentro si caben (una octava de holgura); si no, manda la guia;
//    - sin guia o guia fuera del timeline -> no hay plan;
//    - recorte del scroll al final del timeline;
//    - el plan nunca cambia el zoom horizontal (solo lleva scrollX).
// 2. Camino real del editor (MainComponent::revealMidiGuideInView):
//    - parte de una vista donde la guia no esta (reabrir);
//    - tras pulsar: ghost_visible == ghost_in_view, painted == in_view,
//      pixeles cian > 0 y el inicio de la guia en el borde izquierdo;
//    - sin dirty, sin entrada de undo, sin render pendiente;
//    - pps horizontal intacto.
#include "TestAssert.h"

#include "../Source/Models/Project.h" // pulls JuceHeader first
#include "../Source/Models/MidiGuideGhosts.h"
#include "../Source/UI/MainComponent.h"
#include "../Source/UI/MidiGuideReveal.h"
#include "../Source/UI/PianoRollComponent.h"
#include "../Source/Undo/PitchUndoManager.h"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

// Friend of MainComponent (see there): the editor without a host around it.
struct MainComponentTestAccess
{
    MainComponent &mc;

    PianoRollComponent &roll() { return mc.pianoRoll; }
    void reveal(Project &project) { mc.revealMidiGuideInView(project); }
    bool canUndo() const
    {
        return mc.undoManager != nullptr && mc.undoManager->canUndo();
    }
    int pendingRenderBlocks() const
    {
        return mc.deferredRenders.dirtyBlockCount();
    }
    bool ghostsVisible() const { return mc.showMidiGuideGhosts; }
    void setGhostsVisible(bool visible) { mc.showMidiGuideGhosts = visible; }
    void setGuideSpan(double hostStart, double hostEnd, float pitchMin,
                      float pitchMax)
    {
        mc.hasMidiGuideSpan = true;
        mc.midiGuideHostStart = hostStart;
        mc.midiGuideHostEnd = hostEnd;
        mc.midiGuidePitchMin = pitchMin;
        mc.midiGuidePitchMax = pitchMax;
    }
};

namespace
{
constexpr double kOffset = 5.818182;
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
        ghost.startHostSeconds = kProjectFirst + i * step + kOffset;
        ghost.endHostSeconds = ghost.startHostSeconds + step;
        ghost.pitch = static_cast<float>(53 + (i % 22));
        ghost.label = "C4";
        ghosts.push_back(ghost);
    }
    return ghosts;
}

juce::Image paintToImage(PianoRollComponent& roll)
{
    juce::Image image(juce::Image::ARGB, roll.getWidth(), roll.getHeight(), true);
    juce::Graphics g(image);
    roll.paint(g);
    return image;
}

int countCyanPixels(const juce::Image& image, int tolerance = 70)
{
    int count = 0;
    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
        {
            const auto pixel = image.getPixelAt(x, y);
            const int dr = pixel.getRed() - 53;
            const int dg = pixel.getGreen() - 196;
            const int db = pixel.getBlue() - 207;
            if (dr * dr + dg * dg + db * db <= tolerance * tolerance)
                ++count;
        }
    return count;
}

MidiGuideRevealInput baseInput()
{
    MidiGuideRevealInput input;
    input.hasGuide = true;
    input.guideStartSeconds = 20.0;
    input.guideEndSeconds = 60.0;
    input.guidePitchMin = 53.0f;
    input.guidePitchMax = 74.0f;
    input.visibleStartSeconds = 0.0;
    input.visibleEndSeconds = 30.0;
    input.pixelsPerSecond = kPps;
    input.timelineStartSeconds = 0.0;
    input.timelineEndSeconds = 100.0;
    return input;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    // 1. Guia dentro del tiempo visible: no hay scroll.
    {
        const auto plan = computeMidiGuideRevealPlan(baseInput());
        CHECK(plan.hasGuide);
        CHECK(!plan.scrollRequested);
        CHECK(plan.pitchMin == 53.0f);
        CHECK(plan.pitchMax == 74.0f);
        std::printf("pure_in_time        scroll=%d x=%.3f pitch=[%.1f,%.1f]\n",
                    plan.scrollRequested ? 1 : 0, plan.scrollX,
                    (double)plan.pitchMin, (double)plan.pitchMax);
    }

    // 2. Guia a la derecha: scroll minimo que lleva su inicio al borde.
    {
        auto input = baseInput();
        input.visibleStartSeconds = 70.0;
        input.visibleEndSeconds = 100.0;
        const auto plan = computeMidiGuideRevealPlan(input);
        CHECK(plan.hasGuide);
        CHECK(plan.scrollRequested);
        CHECK(std::abs(plan.scrollX - 20.0 * kPps) < 1.0e-9);
        std::printf("pure_right          scroll=%.3f\n", plan.scrollX);
    }

    // 3. Guia a la izquierda: tambien lleva su inicio al borde izquierdo.
    {
        auto input = baseInput();
        input.visibleStartSeconds = 0.0;
        input.visibleEndSeconds = 10.0;
        const auto plan = computeMidiGuideRevealPlan(input);
        CHECK(plan.scrollRequested);
        CHECK(std::abs(plan.scrollX - 20.0 * kPps) < 1.0e-9);
        std::printf("pure_left           scroll=%.3f\n", plan.scrollX);
    }

    // 4. Activas dentro si caben. 60-62 cae dentro de 53-74 y no ensancha.
    {
        auto input = baseInput();
        input.hasActiveNotes = true;
        input.activePitchMin = 60.0f;
        input.activePitchMax = 62.0f;
        const auto plan = computeMidiGuideRevealPlan(input);
        CHECK(plan.pitchMin == 53.0f);
        CHECK(plan.pitchMax == 74.0f);
    }

    // Activas 46-62: ensanchan 7 semitonos (< 1 octava), se conservan.
    {
        auto input = baseInput();
        input.hasActiveNotes = true;
        input.activePitchMin = 46.0f;
        input.activePitchMax = 62.0f;
        const auto plan = computeMidiGuideRevealPlan(input);
        CHECK(plan.pitchMin == 46.0f);
        CHECK(plan.pitchMax == 74.0f);
        std::printf("pure_actives_fit    pitch=[%.1f,%.1f]\n",
                    (double)plan.pitchMin, (double)plan.pitchMax);
    }

    // Activas 20-30: ensancharian mas de una octava; manda la guia.
    {
        auto input = baseInput();
        input.hasActiveNotes = true;
        input.activePitchMin = 20.0f;
        input.activePitchMax = 30.0f;
        const auto plan = computeMidiGuideRevealPlan(input);
        CHECK(plan.pitchMin == 53.0f);
        CHECK(plan.pitchMax == 74.0f);
        std::printf("pure_actives_drop   pitch=[%.1f,%.1f]\n",
                    (double)plan.pitchMin, (double)plan.pitchMax);
    }

    // 5. Sin guia no hay plan; fuera del timeline tampoco (sin solape).
    {
        auto input = baseInput();
        input.hasGuide = false;
        auto plan = computeMidiGuideRevealPlan(input);
        CHECK(!plan.hasGuide);
        CHECK(plan.pitchMin > plan.pitchMax);

        input = baseInput();
        input.guideStartSeconds = 120.0;
        input.guideEndSeconds = 130.0;
        plan = computeMidiGuideRevealPlan(input);
        CHECK(!plan.hasGuide);
    }

    // 6. Scroll recortado al final del timeline.
    {
        auto input = baseInput();
        input.guideStartSeconds = 95.0;
        input.guideEndSeconds = 99.0;
        input.visibleStartSeconds = 60.0;
        input.visibleEndSeconds = 90.0;
        const auto plan = computeMidiGuideRevealPlan(input);
        CHECK(plan.scrollRequested);
        // ventana 30 s, timeline 100 s -> el borde izquierdo no pasa de 70 s.
        CHECK(std::abs(plan.scrollX - 70.0 * kPps) < 1.0e-9);
        std::printf("pure_clamp_end      scroll=%.3f\n", plan.scrollX);
    }

    // 7. Camino real: reabrir deja la guia fuera; el boton la trae.
    Project project;
    setLength(project, 130.0);
    project.getNotes().push_back(Note(frameFor(10.0), frameFor(12.0), 60.0f));
    project.getNotes().push_back(Note(frameFor(14.0), frameFor(16.0), 62.0f));

    MainComponent mc(false);
    mc.setSize(1000, 400);
    MainComponentTestAccess access{mc};
    auto& roll = access.roll();
    roll.setSize(1000, 400);
    roll.setProject(&project);
    roll.setTimelineDisplayOffset(kOffset);
    roll.setMidiGuideGhosts(makeGhosts());
    roll.setPixelsPerSecond(static_cast<float>(kPps));

    // Vista como al reabrir: lejos de la guia y con una altura que la corta.
    roll.setScrollX(80.0 * kPps);
    roll.setPixelsPerSemitone(30.0f);
    roll.setScrollY(510.0); // arriba ~72, abajo ~60
    paintToImage(roll);

    const float ppsBefore = roll.getPixelsPerSecond();
    const bool undoBefore = access.canUndo();
    const int rendersBefore = access.pendingRenderBlocks();
    const auto dirtyBefore = project.getDirtyFrameRange();
    const size_t notesBefore = project.getNotes().size();
    const auto startBefore = roll.getVisibleProjectRange();

    CHECK(roll.getMidiGuideGhostsInViewCount() == 0);
    CHECK(roll.getMidiGuideGhostsPaintedLast() == 0);
    std::printf("button_before       window=[%.3f,%.3f] in_view=%zu visible=%zu "
                "painted=%d\n",
                startBefore.first, startBefore.second,
                roll.getMidiGuideGhostsInViewCount(),
                roll.getMidiGuideGhostsVisibleCount(),
                roll.getMidiGuideGhostsPaintedLast());

    access.reveal(project);
    const auto image = paintToImage(roll);
    const int cyan = countCyanPixels(image);

    const auto after = roll.getVisibleProjectRange();
    std::printf("button_after        window=[%.3f,%.3f] in_view=%zu visible=%zu "
                "painted=%d cyan=%d\n",
                after.first, after.second, roll.getMidiGuideGhostsInViewCount(),
                roll.getMidiGuideGhostsVisibleCount(),
                roll.getMidiGuideGhostsPaintedLast(), cyan);

    CHECK(after.first <= kProjectFirst + 1.0e-6);
    CHECK(after.second >= kProjectFirst);
    CHECK(roll.getMidiGuideGhostsInViewCount() > 0);
    CHECK(roll.getMidiGuideGhostsVisibleCount() ==
          roll.getMidiGuideGhostsInViewCount());
    CHECK(roll.getMidiGuideGhostsPaintedLast() ==
          static_cast<int>(roll.getMidiGuideGhostsInViewCount()));
    CHECK(cyan > 0);

    // La vista se movio, el proyecto no.
    CHECK(roll.getPixelsPerSecond() == ppsBefore);
    CHECK(access.canUndo() == undoBefore);
    CHECK(access.pendingRenderBlocks() == rendersBefore);
    CHECK(project.getDirtyFrameRange() == dirtyBefore);
    CHECK(project.getNotes().size() == notesBefore);

    // Tick ON (el caso normal): pulsar otra vez no oculta nada ni alterna el
    // estado; el boton no es un toggle y la guia sigue en vista.
    CHECK(access.ghostsVisible());
    const auto inViewOn = roll.getMidiGuideGhostsInViewCount();
    access.reveal(project);
    CHECK(access.ghostsVisible());
    CHECK(roll.getMidiGuideGhostsInViewCount() == inViewOn);

    // Tick OFF: la capa de fantasmas esta vacia por diseno, pero el boton debe
    // traer la guia igual usando el span del modelo (V4c-b). Un clic enciende
    // los fantasmas (ida, no toggle) y deja la vista en el inicio de la guia.
    roll.setScrollX(80.0 * kPps);
    roll.setPixelsPerSemitone(30.0f);
    roll.setScrollY(510.0);
    roll.setMidiGuideGhosts({});
    access.setGhostsVisible(false);
    access.setGuideSpan(kProjectFirst + kOffset, kProjectLast + kOffset,
                        53.0f, 74.0f);
    paintToImage(roll);
    CHECK(roll.getMidiGuideGhostsInViewCount() == 0);

    access.reveal(project);
    paintToImage(roll);
    const auto afterOff = roll.getVisibleProjectRange();
    const double ppsSemiOff = roll.getPixelsPerSemitone();
    const double topOff =
        MAX_MIDI_NOTE - roll.getScrollY() / ppsSemiOff;
    const double bottomOff =
        MAX_MIDI_NOTE -
        (roll.getScrollY() + roll.getVisibleContentHeight()) / ppsSemiOff;
    std::printf("button_tick_off     window=[%.3f,%.3f] pitch=[%.1f,%.1f]\n",
                afterOff.first, afterOff.second, bottomOff, topOff);
    CHECK(access.ghostsVisible());
    CHECK(afterOff.first <= kProjectFirst + 1.0e-6);
    CHECK(topOff >= 74.0);
    CHECK(bottomOff <= 53.0);
    CHECK(roll.getPixelsPerSecond() == ppsBefore);
    CHECK(access.canUndo() == undoBefore);
    CHECK(access.pendingRenderBlocks() == rendersBefore);
    CHECK(project.getDirtyFrameRange() == dirtyBefore);
    CHECK(project.getNotes().size() == notesBefore);

    std::cout << "MidiGuideRevealTests passed (V4c: Show guide, view only)\n";
    return 0;
}
