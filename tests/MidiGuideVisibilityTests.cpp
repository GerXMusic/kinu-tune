// V1 (guia MIDI): medir antes de arreglar. Este test NO arregla nada.
//
// Fija con numeros por que desaparece la guia de la vista:
//   * ghost_count      : fantasmas en el store del documento;
//   * ghost_in_view    : fantasmas que solapan la ventana visible (tiempo);
//   * ghost_visible    : ademas dentro del rango de altura visible;
//   * painted_last     : lo que el pintor conto como pintado (pasa el recorte
//                        ACTUAL [0, getWidth()/pps] Y solapa la ventana visible);
//   * pixeles cian     : lo que de verdad queda en la imagen.
//
// Estados fijados por la fase (fixture = la guia medida, 111 fantasmas de
// proyecto 7.886192-68.143680 s, offset 5.818182 s, pitches 53-74):
//   pps 7.64, scroll 0       -> in_view 111
//   pps 36,   scroll 0       -> painted == in_view (el bug aun no se ve)
//   pps 36,   scroll 600 px  -> painted <  in_view (prefijo)
//   pps 36,   scroll 1200 px -> painted == 0       (el bug)
//   pps 100,  scroll 2000 px -> painted == 0
//   altura 60-72             -> visible < in_view  (documenta la diferencia)
//
// V2 actualizara este test a painted == in_view y visible == in_view con la
// altura ajustada; en V1 se documenta el comportamiento actual.
#include "TestAssert.h"

#include "../Source/Models/Project.h" // pulls JuceHeader first
#include "../Source/Models/MidiGuideGhosts.h"
#include "../Source/UI/PianoRollComponent.h"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace
{
constexpr double kOffset = 5.818182;
constexpr double kProjectFirst = 7.886192;  // host 13.704374 - offset
constexpr double kProjectLast = 68.143680;  // host 73.961862 - offset
constexpr int kGhostCount = 111;
constexpr int kComponentWidth = 1000;
constexpr int kComponentHeight = 400;

int frameFor(double seconds)
{
    return static_cast<int>(
        std::llround(seconds * SAMPLE_RATE / static_cast<double>(HOP_SIZE)));
}

void setLength(Project& project, double seconds)
{
    const auto frames = static_cast<size_t>(frameFor(seconds));
    project.getAudioData().sampleRate = SAMPLE_RATE;
    // getDuration() reads the waveform, not the pitch vectors.
    project.getAudioData().waveform.setSize(
        1, static_cast<int>(frames) * HOP_SIZE);
    project.getAudioData().f0.assign(frames, 220.0f);
    project.getAudioData().basePitch.assign(frames, 60.0f);
    project.getAudioData().deltaPitch.assign(frames, 0.0f);
}

// Fixture: 111 fantasmas contiguos que cubren exactamente el tramo medido.
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
        // 53..74 repetido, para que cualquier ventana corta de tiempo tenga
        // fantasmas en todo el rango de altura.
        ghost.pitch = static_cast<float>(53 + (i % 22));
        ghost.label = "C4";
        ghosts.push_back(ghost);
    }
    return ghosts;
}

MidiGuideGhostLayer makeLayer()
{
    MidiGuideGhostLayer layer;
    layer.set(makeGhosts());
    return layer;
}

juce::Image paintToImage(PianoRollComponent& roll)
{
    juce::Image image(juce::Image::ARGB, roll.getWidth(), roll.getHeight(), true);
    juce::Graphics g(image);
    roll.paint(g);
    return image;
}

// Pixels close to the guide cyan (0xFF35C4CF) after the 0.85 alpha outline is
// composited over the dark canvas. Tolerance in RGB distance, so the dashed
// antialiased edges count a bit and a 10 % fill does not.
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

struct State
{
    double pps = 0.0;
    double scrollX = 0.0;
    double pixelsPerSemitone = 10.0;
    double scrollY = 100.0; // top 79, bottom 43: 53-74 inside
};

void applyState(PianoRollComponent& roll, const State& state)
{
    roll.setPixelsPerSecond(static_cast<float>(state.pps));
    roll.setPixelsPerSemitone(static_cast<float>(state.pixelsPerSemitone));
    roll.setScrollY(state.scrollY);
    roll.setScrollX(state.scrollX);
}

void report(const char* name, PianoRollComponent& roll, int cyanPixels = -1)
{
    const auto range = roll.getVisibleProjectRange();
    std::printf(
        "%-22s count=%zu in_view=%zu visible=%zu painted=%d "
        "scroll=%.1f window=[%.3f,%.3f] cyan=%d\n",
        name, roll.getMidiGuideGhostCount(),
        roll.getMidiGuideGhostsInViewCount(),
        roll.getMidiGuideGhostsVisibleCount(),
        roll.getMidiGuideGhostsPaintedLast(), roll.getScrollX(), range.first,
        range.second, cyanPixels);
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    // 1. La funcion pura de conteo: solape inclusivo, ventana invertida = 0 y
    //    el offset de dibujo se aplica (los fantasmas viven en tiempo host).
    {
        MidiGuideGhostLayer layer;
        std::vector<MidiGuideGhost> ghosts(2);
        ghosts[0].startHostSeconds = 10.0;
        ghosts[0].endHostSeconds = 11.0;
        ghosts[1].startHostSeconds = 20.0;
        ghosts[1].endHostSeconds = 21.0;
        layer.set(ghosts);

        CHECK(countMidiGuideGhostsInProjectWindow(layer, 15.0, 16.0, 5.0) == 1);
        // Extremos iguales cuentan (inclusivo) por los dos lados.
        CHECK(countMidiGuideGhostsInProjectWindow(layer, 6.0, 15.0, 5.0) == 2);
        CHECK(countMidiGuideGhostsInProjectWindow(layer, 6.0, 6.0, 5.0) == 1);
        CHECK(countMidiGuideGhostsInProjectWindow(layer, 6.0001, 14.999, 5.0) ==
              0);
        // Ventana invertida.
        CHECK(countMidiGuideGhostsInProjectWindow(layer, 16.0, 15.0, 5.0) == 0);
        // Sin offset el mismo tramo host no coincide.
        CHECK(countMidiGuideGhostsInProjectWindow(layer, 15.0, 16.0) == 0);
    }

    Project project;
    setLength(project, 130.0); // margen para que pps 7.64 no se recorte

    PianoRollComponent roll;
    roll.setSize(kComponentWidth, kComponentHeight);
    roll.setProject(&project);
    roll.setTimelineDisplayOffset(kOffset);
    roll.setMidiGuideGhosts(makeGhosts());
    CHECK(roll.getMidiGuideGhostCount() ==
          static_cast<size_t>(kGhostCount));

    // 2. Estados horizontales. La altura queda fija con 53-74 dentro, asi
    //    painted_last mide SOLO el recorte de tiempo.
    State state;
    state.pps = 7.64;
    state.scrollX = 0.0;
    applyState(roll, state);
    paintToImage(roll);
    report("pps7.64_scroll0", roll);
    CHECK(roll.getMidiGuideGhostsInViewCount() ==
          static_cast<size_t>(kGhostCount));
    CHECK(roll.getMidiGuideGhostsPaintedLast() == kGhostCount);
    CHECK(roll.getMidiGuideGhostsVisibleCount() ==
          static_cast<size_t>(kGhostCount));

    state.pps = 36.0;
    state.scrollX = 0.0;
    applyState(roll, state);
    paintToImage(roll);
    report("pps36_scroll0", roll);
    CHECK(roll.getMidiGuideGhostsInViewCount() > 0);
    CHECK(roll.getMidiGuideGhostsPaintedLast() ==
          static_cast<int>(roll.getMidiGuideGhostsInViewCount()));

    state.scrollX = 600.0;
    applyState(roll, state);
    paintToImage(roll);
    report("pps36_scroll600", roll);
    CHECK(roll.getMidiGuideGhostsInViewCount() > 0);
    CHECK(roll.getMidiGuideGhostsPaintedLast() > 0);
    CHECK(roll.getMidiGuideGhostsPaintedLast() <
          static_cast<int>(roll.getMidiGuideGhostsInViewCount()));

    state.scrollX = 1200.0;
    applyState(roll, state);
    paintToImage(roll);
    report("pps36_scroll1200", roll);
    CHECK(roll.getMidiGuideGhostsInViewCount() > 0);
    CHECK(roll.getMidiGuideGhostsPaintedLast() == 0); // el bug

    state.pps = 100.0;
    state.scrollX = 2000.0;
    applyState(roll, state);
    paintToImage(roll);
    report("pps100_scroll2000", roll);
    CHECK(roll.getMidiGuideGhostsInViewCount() > 0);
    CHECK(roll.getMidiGuideGhostsPaintedLast() == 0);

    // 3. Altura: la ventana de tiempo esta sobre la guia, pero solo 60-72
    //    caben en pantalla. Se documenta la diferencia; V2 la arregla.
    state.pps = 36.0;
    state.scrollX = 0.0;
    state.pixelsPerSemitone = 30.0;
    state.scrollY = 510.0; // top 72, bottom ~60.1
    applyState(roll, state);
    paintToImage(roll);
    report("pps36_height60_72", roll);
    CHECK(roll.getMidiGuideGhostsInViewCount() > 0);
    CHECK(roll.getMidiGuideGhostsVisibleCount() <
          roll.getMidiGuideGhostsInViewCount());
    CHECK(roll.getMidiGuideGhostsVisibleCount() > 0);

    // 4. Pixeles: guia en tiempo y altura visibles. painted > 0 con pixeles
    //    cian ~0 significaria capa TAPADA; painted == 0, que manda el RECORTE.
    //    El caso store vacio solo se puede medir en V4.
    state.pps = 36.0;
    state.scrollX = 0.0;
    state.pixelsPerSemitone = 10.0;
    state.scrollY = 100.0;
    applyState(roll, state);
    const auto image = paintToImage(roll);
    const int cyan = countCyanPixels(image);
    report("pixels_visible", roll, cyan);

    const char* verdict = "store";
    if (roll.getMidiGuideGhostsPaintedLast() == 0)
        verdict = "clip";
    else if (cyan == 0)
        verdict = "covered";
    else
        verdict = "visible_pixels";
    std::printf("V1 verdict: %s (painted=%d, cyan=%d)\n", verdict,
                roll.getMidiGuideGhostsPaintedLast(), cyan);
    CHECK(roll.getMidiGuideGhostsPaintedLast() > 0);
    CHECK(std::string(verdict) == "visible_pixels" ||
          std::string(verdict) == "covered" ||
          std::string(verdict) == "clip");

    std::cout << "MidiGuideVisibilityTests passed (V1 documents current "
                 "behaviour)\n";
    return 0;
}
