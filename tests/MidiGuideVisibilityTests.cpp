// V2 (guia MIDI): el recorte es la ventana visible y la capa se ve entera.
//
// Fija con numeros por que desaparecia la guia de la vista:
//   * ghost_count      : fantasmas en store del documento;
//   * ghost_in_view    : fantasmas que solapan la ventana visible (tiempo);
//   * ghost_visible    : ademas dentro del rango de altura visible;
//   * painted_last     : lo que el pintor conto como pintado (ventana visible);
//   * pixeles cian     : lo que de verdad queda en la imagen.
//
// Estados fijados por la fase (fixture = la guia medida, 111 fantasmas de
// proyecto 7.886192-68.143680 s, offset 5.818182 s, pitches 53-74):
//   pps 7.64, scroll 0       -> in_view 111
//   pps 36,   scroll 0       -> 35
//   pps 36,   scroll 600 px  -> 50   (V1 pintaba 21)
//   pps 36,   scroll 1200 px -> 51   (V1 pintaba 0: el bug)
//   pps 100,  scroll 2000 px -> 19   (V1 pintaba 0)
//   altura 60-72             -> visible < in_view antes del ajuste
//
// V2 exige painted == in_view en todos los estados con guia, 0 cuando la vista
// no la contiene, y el ajuste vertical con la guia en tiempo cubriendo 53-74.
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
    //    painted_last mide SOLO el recorte de tiempo. V2: la guia se dibuja con
    //    la ventana visible, asi que painted_last == in_view en TODOS los
    //    estados (en V1 era 21/35 con scroll 600 y 0 con scroll >= 1200).
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
    CHECK(roll.getMidiGuideGhostsPaintedLast() ==
          static_cast<int>(roll.getMidiGuideGhostsInViewCount()));

    // Regresion exacta de V1: en el bug el fantasma estaba en la ventana pero
    // el recorte [0, getWidth()/pps] lo dejaba fuera.
    state.scrollX = 1200.0;
    applyState(roll, state);
    paintToImage(roll);
    report("pps36_scroll1200", roll);
    CHECK(roll.getMidiGuideGhostsInViewCount() > 0);
    CHECK(roll.getMidiGuideGhostsPaintedLast() ==
          static_cast<int>(roll.getMidiGuideGhostsInViewCount()));

    state.pps = 100.0;
    state.scrollX = 2000.0;
    applyState(roll, state);
    paintToImage(roll);
    report("pps100_scroll2000", roll);
    CHECK(roll.getMidiGuideGhostsInViewCount() > 0);
    CHECK(roll.getMidiGuideGhostsPaintedLast() ==
          static_cast<int>(roll.getMidiGuideGhostsInViewCount()));

    // 3. Sin guia en la ventana de tiempo: ni se cuenta ni se pinta.
    state.pps = 36.0;
    state.scrollX = 80.0 * 36.0; // proyecto 80-106 s, guia hasta 68.14
    applyState(roll, state);
    paintToImage(roll);
    report("pps36_out_of_guide", roll);
    CHECK(roll.getMidiGuideGhostsInViewCount() == 0);
    CHECK(roll.getMidiGuideGhostsPaintedLast() == 0);

    // 4. Altura: el estado heredado de V1 (solo 60-72 en pantalla) documenta la
    //    diferencia, y el ajuste vertical con la guia en tiempo la cierra.
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

    // La guia solapa el tiempo visible: su ambito de pitch es 53-74.
    const auto guidePitch =
        roll.getMidiGuideGhostPitchRangeInVisibleTime();
    CHECK(guidePitch.first == 53.0f);
    CHECK(guidePitch.second == 74.0f);

    // El ajuste vertical que hace MainComponent::fitAnalyzedPitchRangeToView
    // (union de notas activas y guia, +1 semitono arriba).
    roll.fitPitchRangeToView(53.0f, 75.0f);
    paintToImage(roll);
    report("fit_guide_in_time", roll);
    const double ppsSemiAfterFit = roll.getPixelsPerSemitone();
    const double pitchTop =
        MAX_MIDI_NOTE - roll.getScrollY() / ppsSemiAfterFit;
    const double pitchBottom =
        MAX_MIDI_NOTE -
        (roll.getScrollY() + roll.getVisibleContentHeight()) / ppsSemiAfterFit;
    std::printf("fit_guide_in_time pitch=[%.3f,%.3f]\n", pitchBottom, pitchTop);
    CHECK(pitchTop >= 74.0);
    CHECK(pitchBottom <= 53.0);
    CHECK(roll.getMidiGuideGhostsVisibleCount() ==
          roll.getMidiGuideGhostsInViewCount());
    CHECK(roll.getMidiGuideGhostsPaintedLast() ==
          static_cast<int>(roll.getMidiGuideGhostsInViewCount()));

    // Sin guia en tiempo, el helper no propone ambito y el zoom no cambia.
    state.pps = 36.0;
    state.scrollX = 80.0 * 36.0;
    state.pixelsPerSemitone = ppsSemiAfterFit;
    state.scrollY = roll.getScrollY();
    applyState(roll, state);
    CHECK(roll.getMidiGuideGhostPitchRangeInVisibleTime().first >
          roll.getMidiGuideGhostPitchRangeInVisibleTime().second);

    // 5. Pixeles: guia en tiempo y altura visibles. painted > 0 con pixeles
    //    cian ~0 significaria capa TAPADA; painted == 0, que manda el RECORTE.
    state.pps = 36.0;
    state.scrollX = 0.0;
    state.pixelsPerSemitone = 10.0;
    state.scrollY = 100.0;
    applyState(roll, state);
    const auto image = paintToImage(roll);
    const int cyan = countCyanPixels(image);
    report("pixels_visible", roll, cyan);
    CHECK(cyan > 0);

    // Y 0 cuando la vista no la contiene (mismo estado de tiempo que el 3).
    state.scrollX = 80.0 * 36.0;
    applyState(roll, state);
    const int cyanOut = countCyanPixels(paintToImage(roll));
    report("pixels_out_of_view", roll, cyanOut);
    CHECK(cyanOut == 0);

    // 6. Oclusion (V2): una nota de un ITEM VECINO (capa inactiva) y una nota
    //    ACTIVA, cada una encima de un fantasma.
    //
    //    Metrica: se pinta la misma escena con y sin fantasmas y se cuentan los
    //    pixeles que CAMBIAN dentro de la caja del fantasma. Si la guia queda
    //    debajo del cuerpo de la nota, casi no cambia nada (el fantasma no se
    //    ve); si queda encima, cambia todo su relleno y contorno. En V1, con la
    //    guia debajo, el total cian con la nota vecina bajaba de 223 a 221.
    state.scrollX = 0.0;
    state.pixelsPerSemitone = 10.0;
    state.scrollY = 100.0;
    applyState(roll, state);
    const double step = (kProjectLast - kProjectFirst) / kGhostCount;
    const float ppsSemi = roll.getPixelsPerSemitone();
    const float ppsNow = roll.getPixelsPerSecond();
    const auto boxFor = [&](int index) {
        const double startProject = kProjectFirst + index * step;
        const double endProject = startProject + step;
        const float pitch = static_cast<float>(53 + (index % 22));
        const int left =
            PianoRollComponent::pianoKeysWidth +
            static_cast<int>(std::lround(startProject * ppsNow -
                                         roll.getScrollX()));
        const int right =
            PianoRollComponent::pianoKeysWidth +
            static_cast<int>(std::lround(endProject * ppsNow -
                                         roll.getScrollX()));
        const int top =
            PianoRollComponent::headerHeight +
            static_cast<int>(std::lround((MAX_MIDI_NOTE - pitch) * ppsSemi -
                                         roll.getScrollY()));
        return juce::Rectangle<int>(
            left + 2, top, std::max(1, right - left - 4),
            std::max(1, static_cast<int>(std::lround(ppsSemi))));
    };
    const auto countDiffIn = [](const juce::Image& a, const juce::Image& b,
                                juce::Rectangle<int> area) {
        int count = 0;
        const auto clipped = area.getIntersection(
            {0, 0, a.getWidth(), a.getHeight()});
        for (int y = clipped.getY(); y < clipped.getBottom(); ++y)
            for (int x = clipped.getX(); x < clipped.getRight(); ++x)
                if (a.getPixelAt(x, y) != b.getPixelAt(x, y))
                    ++count;
        return count;
    };

    Project neighbour;
    setLength(neighbour, 130.0);
    // Nota del item vecino exactamente bajo el fantasma 0 (pitch 53).
    neighbour.getNotes().push_back(
        Note(frameFor(kProjectFirst), frameFor(kProjectFirst + step), 53.0f));
    MainViewRegionPreview neighbourPreview;
    neighbourPreview.key = "nb";
    neighbourPreview.name = "nb";
    neighbourPreview.project = &neighbour;
    neighbourPreview.startSeconds = 0.0;
    neighbourPreview.endSeconds = 130.0;
    neighbourPreview.sourceStartSeconds = 0.0;
    neighbourPreview.sourceEndSeconds = 130.0;
    neighbourPreview.projectHostOffsetSeconds = kOffset;
    neighbourPreview.hasContent = true;
    auto previews = std::make_shared<std::vector<MainViewRegionPreview>>();
    previews->push_back(neighbourPreview);
    roll.setRegionPreviews(previews);

    const auto withNeighbour = paintToImage(roll);
    const int neighbourCyan = countCyanPixels(withNeighbour);
    roll.setMidiGuideGhosts({});
    const auto neighbourNoGhost = paintToImage(roll); // mismo estado, sin fantasmas
    roll.setMidiGuideGhosts(makeGhosts());
    const auto neighbourDiff =
        countDiffIn(withNeighbour, neighbourNoGhost, boxFor(0));
    std::printf("occlusion_neighbour   cyan_total=%d diff_box=%d\n",
                neighbourCyan, neighbourDiff);
    CHECK(neighbourCyan > 0);
    CHECK(neighbourDiff > 0);

    // Nota ACTIVA encima del fantasma 1 (pitch 54): la guia va debajo de las
    // notas, asi que el contorno se redibuja por encima sin tocar el cuerpo.
    project.getNotes().push_back(
        Note(frameFor(kProjectFirst + step),
             frameFor(kProjectFirst + 2.0 * step), 54.0f));
    const auto withActive = paintToImage(roll);
    roll.setMidiGuideGhosts({});
    const auto activeWithoutGhost = paintToImage(roll);
    roll.setMidiGuideGhosts(makeGhosts());
    const auto activeDiff =
        countDiffIn(withActive, activeWithoutGhost, boxFor(1));
    std::printf("occlusion_active      cyan_total=%d diff_box=%d\n",
                countCyanPixels(withActive), activeDiff);
    CHECK(activeDiff > 0);
    project.getNotes().clear();
    roll.setRegionPreviews(nullptr);

    std::cout << "MidiGuideVisibilityTests passed (V2: painted == in_view)\n";
    return 0;
}
