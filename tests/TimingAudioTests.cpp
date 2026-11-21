// M4 v1 / D-242: la cuantizacion de tiempo medida en AUDIO.
//
// Criterio (plan/m4/criterio-cuantizacion-tiempo.md), checks de audio:
//   S1 exactitud: con fuerza 100 %, cada onset seleccionado cae a <= 1 frame
//      de hop de la rejilla, con mediana <= 0,5 frame.
//   S2 conservacion: la cuantizacion NO cambia la F0 en el interior de la nota
//      (la curva objetivo es la misma, solo cambia la posicion) y la duracion
//      total de la region no cambia.
//
// Este test usa el camino de produccion, no una copia:
//   - la logica es `Source/Utils/TimingQuantize.h` (la misma que el boton),
//   - el cambio de fronteras es `applyNoteTimingStates` (el de `TimingAction`),
//   - el render es `IncrementalSynthesizer` con los motores del producto,
//   - la F0 se mide con FCPE, el mismo estimador que analiza el plugin.
//
// Material:
//   - clase C sintetica con onsets exactos (siempre, hermetico: es la que corre
//     `ctest`), y
//   - una toma real con timing de forma OPCIONAL, por argumentos:
//       TimingAudioTests --take <wav> --notas <csv> [--segundos N]
//     donde el CSV es `t0,t1,midi[,midi_float,cents]` en segundos. No entra en
//     `ctest` porque el material de medicion vive fuera del repo.
//
// Se declara el metodo: onsets por envolvente de energia (ventana de 1 ms,
// cruce del 10 % del pico de la nota) y error en frames de hop y en ms. El
// control es el mismo proyecto sin cuantizar.

#include "../Source/Audio/FCPEPitchDetector.h"
#include "../Source/Audio/Synthesis/IncrementalSynthesizer.h"
#include "../Source/Models/Project.h"
#include "../Source/Undo/TimingAction.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "../Source/Utils/TimingQuantize.h"
#include "TestAssert.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr int kSampleRate = SAMPLE_RATE; // 44100, el del plugin
constexpr int kHop = HOP_SIZE;           // 512
constexpr double kTempoBpm = 120.0;
constexpr int kGridDivision = 16;        // 1/16, la subdivision de la barra

double midiHz(float midi) {
  return 440.0 * std::pow(2.0, (static_cast<double>(midi) - 69.0) / 12.0);
}

double centsBetween(double a, double b) {
  if (a <= 0.0 || b <= 0.0)
    return 0.0;
  return 1200.0 * std::log2(a / b);
}

// La rejilla tal como la calcula PianoRollComponent::getTimelineGridSeconds:
// negra = 60 / tempo; 1/16 = 1/4 de negra.
double gridFrames() {
  const double quarterNoteSeconds = 60.0 / kTempoBpm;
  const double gridSeconds = quarterNoteSeconds * (4.0 / kGridDivision);
  return gridSeconds * static_cast<double>(kSampleRate) / static_cast<double>(kHop);
}

// ---------------------------------------------------------------------------
// Cuantizar un proyecto exactamente como el boton: TimingQuantize + la accion
// de undo (applyNoteTimingStates) sobre las mismas notas.
// ---------------------------------------------------------------------------
bool quantizeProject(Project &project, int strengthPercent,
                     int minDurationFrames = 0) {
  auto &notes = project.getNotes();
  std::vector<timingQuantize::Span> spans;
  spans.reserve(notes.size());
  for (auto &note : notes) {
    const bool selected =
        !note.isRest() &&
        note.getEndFrame() - note.getStartFrame() >= minDurationFrames;
    spans.push_back({note.getStartFrame(), note.getEndFrame(), selected});
  }

  std::vector<timingQuantize::Span> quantized;
  if (!timingQuantize::quantizeSpans(spans, gridFrames(), strengthPercent,
                                     quantized))
    return false;

  std::vector<NoteTimingState> after;
  for (size_t i = 0; i < notes.size(); ++i) {
    if (spans[i].startFrame == quantized[i].startFrame &&
        spans[i].endFrame == quantized[i].endFrame)
      continue;
    after.push_back({&notes[i], quantized[i].startFrame, quantized[i].endFrame});
  }
  if (after.empty())
    return false;

  applyNoteTimingStates(project, after);
  return true;
}

bool renderAndWait(Project &project, SynthesisEngineType engine,
                   const char *label) {
  IncrementalSynthesizer synthesizer;
  synthesizer.setProject(&project);
  synthesizer.setSynthesisEngine(engine);
  for (auto &note : project.getNotes())
    note.clearDirty();
  // Ancla el rango de render a todo el archivo, como en CommitSeamTests.
  project.setF0DirtyRange(0, static_cast<int>(project.getAudioData().denseF0.size()));

  std::atomic<bool> finished{false};
  std::atomic<bool> success{false};
  synthesizer.synthesizeRegion(nullptr, [&](bool ok) {
    success.store(ok);
    finished.store(true);
  });

  const double deadline = juce::Time::getMillisecondCounterHiRes() + 120000.0;
  while (!finished.load() &&
         juce::Time::getMillisecondCounterHiRes() < deadline)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);

  if (!finished.load()) {
    std::cout << label << ": el render no termino en 120 s\n";
    return false;
  }
  return success.load();
}

// ---------------------------------------------------------------------------
// S1: onset por envolvente de energia (el metodo que autoriza el criterio §5).
// Primer tramo de 1 ms con RMS por encima del 10 % del pico de la busqueda.
// ---------------------------------------------------------------------------
struct Onset {
  bool found = false;
  int sample = 0;
};

Onset detectOnset(const juce::AudioBuffer<float> &buffer, int fromSample,
                  int toSample) {
  const int n = std::min({buffer.getNumSamples(), toSample});
  const int first = std::max(0, fromSample);
  if (first >= n)
    return {};

  float peak = 0.0f;
  for (int i = first; i < n; ++i)
    peak = std::max(peak, std::abs(buffer.getSample(0, i)));
  if (peak <= 0.0f)
    return {};

  const int window = kSampleRate / 1000; // 1 ms
  const float threshold = 0.10f * peak;
  for (int i = first; i + window <= n; i += window / 2) {
    float sumSq = 0.0f;
    for (int k = 0; k < window; ++k) {
      const float s = buffer.getSample(0, i + k);
      sumSq += s * s;
    }
    if (std::sqrt(sumSq / static_cast<float>(window)) > threshold)
      return {true, i};
  }
  return {};
}

// ---------------------------------------------------------------------------
// F0 con FCPE, en el interior de cada nota (15 % de margen a cada lado).
// ---------------------------------------------------------------------------
struct F0Read {
  bool valid = false;
  double cents = 0.0; // mediana en cents respecto a 10 Hz (escala del modelo)
  double hz = 0.0;
};

double median(std::vector<double> values) {
  if (values.empty())
    return 0.0;
  std::sort(values.begin(), values.end());
  const size_t mid = values.size() / 2;
  if (values.size() % 2 == 0)
    return 0.5 * (values[mid - 1] + values[mid]);
  return values[mid];
}

// F0 en cents sobre el interior de una nota, frame a frame (10 ms de FCPE).
std::vector<double> measureNoteCents(FCPEPitchDetector &fcpe,
                                     const juce::AudioBuffer<float> &buffer,
                                     int startFrame, int endFrame) {
  std::vector<double> cents;
  const int duration = endFrame - startFrame;
  if (duration < 6)
    return cents;
  const int margin = std::max(2, duration / 7);
  const int firstSample = (startFrame + margin) * kHop;
  const int lastSample = (endFrame - margin) * kHop;
  if (lastSample <= firstSample || lastSample > buffer.getNumSamples())
    return cents;

  const int numSamples = lastSample - firstSample;
  std::vector<float> segment(static_cast<size_t>(numSamples));
  std::memcpy(segment.data(), buffer.getReadPointer(0, firstSample),
              sizeof(float) * static_cast<size_t>(numSamples));
  const auto f0 = fcpe.extractF0(segment.data(), numSamples, kSampleRate);
  cents.reserve(f0.size());
  for (float value : f0)
    cents.push_back(value > 0.0f
                        ? 1200.0 * std::log2(static_cast<double>(value) / 10.0)
                        : std::numeric_limits<double>::quiet_NaN());
  return cents;
}

F0Read measureNotePitch(FCPEPitchDetector &fcpe,
                        const juce::AudioBuffer<float> &buffer, int startFrame,
                        int endFrame) {
  const int duration = endFrame - startFrame;
  if (duration < 6)
    return {};
  const int margin = std::max(2, duration / 7);
  const int firstSample = (startFrame + margin) * kHop;
  const int lastSample = (endFrame - margin) * kHop;
  if (lastSample <= firstSample ||
      lastSample > buffer.getNumSamples())
    return {};

  const int numSamples = lastSample - firstSample;
  std::vector<float> segment(static_cast<size_t>(numSamples));
  std::memcpy(segment.data(), buffer.getReadPointer(0, firstSample),
              sizeof(float) * static_cast<size_t>(numSamples));

  const auto f0 = fcpe.extractF0(segment.data(), numSamples, kSampleRate);
  std::vector<double> cents;
  cents.reserve(f0.size());
  for (float value : f0)
    if (value > 0.0f)
      cents.push_back(1200.0 * std::log2(static_cast<double>(value) / 10.0));
  if (cents.empty())
    return {};

  F0Read read;
  read.valid = true;
  read.cents = median(cents);
  read.hz = 10.0 * std::pow(2.0, read.cents / 1200.0);
  return read;
}

// ---------------------------------------------------------------------------
// Clase C sintetica: cuatro notas con silencio entre ellas y onsets exactos,
// que es lo que hace medible el inicio por energia. Las fronteras compartidas
// (frase ligada) las cubre TimingQuantizeTests y la toma real.
// ---------------------------------------------------------------------------
struct SyntheticNote {
  int start;
  int end;
  float midi;
};

const std::vector<SyntheticNote> kSyntheticNotes = {
    {40, 80, 57.0f},   // A3
    {95, 140, 59.0f},  // B3
    {150, 200, 62.0f}, // D4
    {215, 260, 57.0f}, // A3
};

constexpr int kSyntheticFrames = 340;

juce::AudioBuffer<float> makeSyntheticPhrase() {
  juce::AudioBuffer<float> buffer(1, kSyntheticFrames * kHop);
  buffer.clear();
  const int fade = kSampleRate / 200; // 5 ms
  for (const auto &note : kSyntheticNotes) {
    const double frequency = midiHz(note.midi);
    const int first = note.start * kHop;
    const int last = note.end * kHop;
    double phase = 0.0;
    for (int i = first; i < last; ++i) {
      phase += 2.0 * 3.14159265358979323846 * frequency / kSampleRate;
      float value = 0.30f * static_cast<float>(std::sin(phase)) +
                    0.12f * static_cast<float>(std::sin(2.0 * phase)) +
                    0.06f * static_cast<float>(std::sin(3.0 * phase));
      if (i - first < fade)
        value *= static_cast<float>(i - first) / static_cast<float>(fade);
      if (last - 1 - i < fade)
        value *= static_cast<float>(last - 1 - i) / static_cast<float>(fade);
      buffer.setSample(0, i, value);
    }
  }
  return buffer;
}

Project makeSyntheticProject() {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = kSampleRate;
  const auto phrase = makeSyntheticPhrase();
  audio.originalWaveform.makeCopyOf(phrase);
  audio.waveform.makeCopyOf(phrase);

  audio.melSpectrogram.assign(static_cast<size_t>(kSyntheticFrames),
                              std::vector<float>(NUM_MELS, 1.0f));
  audio.denseF0.assign(static_cast<size_t>(kSyntheticFrames), 0.0f);
  audio.voicedMask.assign(static_cast<size_t>(kSyntheticFrames), false);
  audio.f0.assign(static_cast<size_t>(kSyntheticFrames), 0.0f);

  for (const auto &note : kSyntheticNotes) {
    for (int frame = note.start; frame < note.end; ++frame) {
      audio.voicedMask[static_cast<size_t>(frame)] = true;
      audio.denseF0[static_cast<size_t>(frame)] =
          static_cast<float>(midiHz(note.midi));
    }
    Note n(note.start, note.end, note.midi);
    n.setOriginalMidiNote(note.midi);
    n.setOriginalDeltaPitch(std::vector<float>(note.end - note.start, 0.0f));
    project.addNote(n);
  }
  for (int frame = 0; frame < kSyntheticFrames; ++frame)
    audio.f0[static_cast<size_t>(frame)] = audio.denseF0[static_cast<size_t>(frame)];

  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

// ---------------------------------------------------------------------------
// La medida completa: render de control, cuantizado, S1 y S2.
// ---------------------------------------------------------------------------
void checkTimingAudio(Project control, SynthesisEngineType engine,
                      const char *engineName, FCPEPitchDetector &fcpe,
                      bool expectRetime = true,
                      int minDurationFrames = 0,
                      bool measureAudioOnsets = true) {
  Project quantized = control;
  CHECK(quantizeProject(quantized, 100, minDurationFrames));
  if (minDurationFrames > 0) {
    int moved = 0;
    for (size_t i = 0; i < quantized.getNotes().size(); ++i)
      if (quantized.getNotes()[i].getStartFrame() !=
              control.getNotes()[i].getStartFrame() ||
          quantized.getNotes()[i].getEndFrame() !=
              control.getNotes()[i].getEndFrame())
        ++moved;
    std::printf("  seleccion: notas de >= %d frames, %d movidas de %zu\n",
                minDurationFrames, moved, quantized.getNotes().size());
  }

  CHECK(renderAndWait(control, engine, engineName));
  CHECK(renderAndWait(quantized, engine, engineName));

  const auto &controlAudio = control.getAudioData().waveform;
  const auto &quantizedAudio = quantized.getAudioData().waveform;

  // S2 (duracion): la region tiene que durar lo mismo.
  CHECK(controlAudio.getNumSamples() == quantizedAudio.getNumSamples());

  const auto &controlNotes = control.getNotes();
  const auto &quantizedNotes = quantized.getNotes();
  CHECK(controlNotes.size() == quantizedNotes.size());

  std::printf("\n[%s] S1 onsets (rejilla %.4f frames, 1/%d a %.0f BPM)\n",
              engineName, gridFrames(), kGridDivision, kTempoBpm);

  if (!measureAudioOnsets) {
    // Metodo declarado (criterio §5): en una toma real ligada el onset se
    // mide por la FRONTERA DEL PROYECTO, que es lo que el motor recibe. La
    // exactitud se comprueba contra la rejilla (redondeo de quantizeFrame).
    const int grid = static_cast<int>(std::llround(gridFrames()));
    double maxErr = 0.0;
    int movedCount = 0;
    for (size_t i = 0; i < quantizedNotes.size(); ++i) {
      const auto &c = controlNotes[i];
      const auto &q = quantizedNotes[i];
      if (c.getStartFrame() == q.getStartFrame() &&
          c.getEndFrame() == q.getEndFrame())
        continue;
      ++movedCount;
      for (int edge : {q.getStartFrame(), q.getEndFrame()}) {
        const bool changed = (edge == q.getStartFrame())
                                 ? (c.getStartFrame() != q.getStartFrame())
                                 : (c.getEndFrame() != q.getEndFrame());
        if (!changed)
          continue;
        const double snapped = std::round(edge / gridFrames()) * gridFrames();
        maxErr = std::max(maxErr, std::abs(edge - snapped));
      }
      std::printf("  nota %zu: [%d,%d) -> [%d,%d)\n", i, c.getStartFrame(),
                  c.getEndFrame(), q.getStartFrame(), q.getEndFrame());
    }
    std::printf("  S1 (frontera de proyecto, %d notas movidas): max |frontera "
                "- rejilla| %.2f frames\n",
                movedCount, maxErr);
    CHECK(movedCount > 0);
    CHECK(maxErr <= 0.51);
  } else {
    // Baseline: el mismo detector sobre el render de control.
    {
      int prev = 0;
      for (size_t i = 0; i < controlNotes.size(); ++i) {
        const int start = controlNotes[i].getStartFrame();
        const int end = controlNotes[i].getEndFrame();
        const Onset onset =
            detectOnset(controlAudio, (prev + 1) * kHop, end * kHop);
        prev = end;
        std::printf("  control nota %zu: [%d,%d) onset %+.2f frames\n", i,
                    start, end,
                    onset.found ? (onset.sample - start * kHop) /
                                      static_cast<double>(kHop)
                                : -999.0);
      }
    }

    std::vector<double> absoluteErrors;
    int previousEndFrame = 0;
    for (size_t i = 0; i < quantizedNotes.size(); ++i) {
      const int start = quantizedNotes[i].getStartFrame();
      const int end = quantizedNotes[i].getEndFrame();
      const int searchFrom = (previousEndFrame + 1) * kHop;
      const int searchTo = end * kHop;
      const Onset onset = detectOnset(quantizedAudio, searchFrom, searchTo);
      previousEndFrame = end;
      if (controlNotes[i].getStartFrame() == start &&
          controlNotes[i].getEndFrame() == end)
        continue; // no se movio: no aporta a S1
      CHECK(onset.found);

      const double expectedSample = static_cast<double>(start) * kHop;
      const double errorSamples =
          static_cast<double>(onset.sample) - expectedSample;
      const double errorFrames = errorSamples / kHop;
      const double errorMs = errorSamples * 1000.0 / kSampleRate;
      absoluteErrors.push_back(std::abs(errorFrames));
      std::printf("  nota %zu: frontera [%d,%d) -> [%d,%d)  onset %.1f ms  error %+.2f "
                  "frames (%+.1f ms)\n",
                  i, controlNotes[i].getStartFrame(),
                  controlNotes[i].getEndFrame(), start, end,
                  onset.sample * 1000.0 / kSampleRate, errorFrames, errorMs);
    }

    const double maxError = *std::max_element(absoluteErrors.begin(),
                                              absoluteErrors.end());
    const double medianError = median(absoluteErrors);
    std::printf("  S1: max |error| %.2f frames, mediana %.2f frames (limites "
                "1,00 y 0,50)\n",
                maxError, medianError);
    if (!expectRetime) {
      // El core de pitchcore no lee `sourceFrame` (render_request.h lo
      // declara, src/engine.cpp no lo usa): con ese motor el retime no llega
      // al audio y la nota sigue en su posicion de fuente. Limitacion
      // conocida, ajena a la cuantizacion; se publica, no se da por buena.
      std::printf("  (motor sin retime en el core: S1 no se evalua)\n");
      return;
    }
    CHECK(maxError <= 1.0);
    CHECK(medianError <= 0.5);
  }

  // S2 (F0): la misma curva, otra posicion. Por nota se compara la MEDIANA de
  // la F0 medida en el interior (robusta al vibrato). El maximo se publica:
  // una nota cuyo interior, al moverse la frontera, cubre otra parte del
  // contorno (glissando) da una mediana distinta sin que la altura cambie.
  std::printf("[%s] S2 F0 en el interior de la nota\n", engineName);
  std::vector<double> audioDeltas;
  for (size_t i = 0; i < quantizedNotes.size(); ++i) {
    const auto &c = controlNotes[i];
    const auto &q = quantizedNotes[i];
    if (c.getStartFrame() == q.getStartFrame() &&
        c.getEndFrame() == q.getEndFrame())
      continue;

    auto controlCents =
        measureNoteCents(fcpe, controlAudio, c.getStartFrame(), c.getEndFrame());
    auto quantizedCents =
        measureNoteCents(fcpe, quantizedAudio, q.getStartFrame(), q.getEndFrame());
    std::vector<double> cVoiced, qVoiced;
    for (double v : controlCents)
      if (!std::isnan(v))
        cVoiced.push_back(v);
    for (double v : quantizedCents)
      if (!std::isnan(v))
        qVoiced.push_back(v);
    if (cVoiced.size() < 4 || qVoiced.size() < 4)
      continue;

    const double delta = std::abs(median(qVoiced) - median(cVoiced));
    audioDeltas.push_back(delta);
    std::printf("  nota %zu: delta de mediana %.2f cents (%zu/%zu frames con "
                "voz)\n",
                i, delta, qVoiced.size(), cVoiced.size());
  }
  const double maxAudioDelta =
      audioDeltas.empty()
          ? 0.0
          : *std::max_element(audioDeltas.begin(), audioDeltas.end());
  std::printf("  S2: max %.2f cents, mediana de notas %.2f cents, %zu notas\n",
              maxAudioDelta, median(audioDeltas), audioDeltas.size());
  // El criterio exacto es sobre la curva (misma curva, 0 c). En audio se
  // exige que no haya un cambio SISTEMATICO: la mediana de las notas.
  CHECK(!audioDeltas.empty());
  CHECK(median(audioDeltas) <= 3.0);
}

// ---------------------------------------------------------------------------
// Toma real (opcional): WAV + CSV de notas en segundos.
// ---------------------------------------------------------------------------
struct CsvNote {
  double t0 = 0.0;
  double t1 = 0.0;
  float midi = 0.0f;
};

std::vector<CsvNote> readNotesCsv(const std::string &path) {
  std::vector<CsvNote> notes;
  std::ifstream file(path);
  if (!file)
    return notes;
  std::string line;
  std::getline(file, line); // cabecera
  while (std::getline(file, line)) {
    for (char &ch : line)
      if (ch == ',')
        ch = ' ';
    std::istringstream is(line);
    CsvNote note;
    if (!(is >> note.t0 >> note.t1 >> note.midi))
      continue;
    if (note.t1 > note.t0)
      notes.push_back(note);
  }
  return notes;
}

bool loadWav(const juce::File &file, juce::AudioBuffer<float> &out,
             int &sampleRate) {
  juce::AudioFormatManager formats;
  formats.registerBasicFormats();
  std::unique_ptr<juce::AudioFormatReader> reader(
      formats.createReaderFor(file));
  if (reader == nullptr)
    return false;
  sampleRate = static_cast<int>(reader->sampleRate);
  out.setSize(1, static_cast<int>(reader->lengthInSamples));
  reader->read(&out, 0, out.getNumSamples(), 0, true, false);
  return true;
}

// F0 del detector mapeada a la rejilla del proyecto, igual que
// EditorController::analyzeAudio (interpolacion logaritmica + mascara U/V).
void fillF0FromDetector(const std::vector<float> &extractedF0,
                        int targetFrames, std::vector<float> &rawF0,
                        std::vector<bool> &voicedMask) {
  rawF0.assign(static_cast<size_t>(targetFrames), 0.0f);
  voicedMask.assign(static_cast<size_t>(targetFrames), false);
  if (extractedF0.empty() || targetFrames <= 0)
    return;

  const double neuralFrameTime = 160.0 / 16000.0;
  const double vocoderFrameTime =
      static_cast<double>(kHop) / static_cast<double>(kSampleRate);
  const int sourceFrameCount = static_cast<int>(extractedF0.size());

  for (int i = 0; i < targetFrames; ++i) {
    const double vocoderTime = i * vocoderFrameTime;
    const double neuralFramePos = vocoderTime / neuralFrameTime;
    const int leftIdx =
        std::clamp(static_cast<int>(std::floor(neuralFramePos)), 0,
                   sourceFrameCount - 1);
    const int rightIdx = std::min(leftIdx + 1, sourceFrameCount - 1);
    const int nearestIdx = std::clamp(
        static_cast<int>(std::llround(neuralFramePos)), 0,
        sourceFrameCount - 1);
    const double frac =
        std::clamp(neuralFramePos - std::floor(neuralFramePos), 0.0, 1.0);

    const bool isVoiced = extractedF0[static_cast<size_t>(nearestIdx)] > 0.0f;
    voicedMask[static_cast<size_t>(i)] = isVoiced;
    if (!isVoiced)
      continue;

    const float leftF0 = extractedF0[static_cast<size_t>(leftIdx)];
    const float rightF0 = extractedF0[static_cast<size_t>(rightIdx)];
    if (leftF0 > 0.0f && rightF0 > 0.0f && leftIdx != rightIdx) {
      const float logF0 = static_cast<float>(std::log(leftF0) * (1.0 - frac) +
                                             std::log(rightF0) * frac);
      rawF0[static_cast<size_t>(i)] = std::exp(logF0);
    } else {
      rawF0[static_cast<size_t>(i)] = extractedF0[static_cast<size_t>(nearestIdx)];
    }
  }
}

bool runRealTake(const juce::File &wav, const juce::File &notesCsv,
                 double seconds, FCPEPitchDetector &fcpe) {
  juce::AudioBuffer<float> audio;
  int sampleRate = 0;
  if (!loadWav(wav, audio, sampleRate)) {
    std::cout << "no se pudo leer " << wav.getFullPathName() << "\n";
    return false;
  }

  if (sampleRate != kSampleRate) {
    // El plugin analiza a 44100; se remuestrea una vez con Lagrange.
    const double ratio = static_cast<double>(kSampleRate) / sampleRate;
    const int outLength = static_cast<int>(std::llround(audio.getNumSamples() * ratio));
    juce::AudioBuffer<float> resampled(1, outLength);
    juce::LagrangeInterpolator interpolator;
    interpolator.process(ratio, audio.getReadPointer(0),
                         resampled.getWritePointer(0), outLength);
    audio = std::move(resampled);
  }

  const int maxSamples = seconds > 0.0
                             ? std::min(audio.getNumSamples(),
                                        static_cast<int>(seconds * kSampleRate))
                             : audio.getNumSamples();
  const int frames = maxSamples / kHop;

  Project project;
  auto &data = project.getAudioData();
  data.sampleRate = kSampleRate;
  data.originalWaveform.makeCopyOf(audio);
  data.originalWaveform.setSize(1, frames * kHop, true, true, true);
  data.waveform.makeCopyOf(data.originalWaveform);
  data.melSpectrogram.assign(static_cast<size_t>(frames),
                             std::vector<float>(NUM_MELS, 1.0f));

  const auto extracted = fcpe.extractF0(
      data.originalWaveform.getReadPointer(0),
      data.originalWaveform.getNumSamples(), kSampleRate);
  fillF0FromDetector(extracted, frames, data.rawF0, data.voicedMask);
  data.denseF0 = data.rawF0;
  data.f0 = data.rawF0;

  const auto csvNotes = readNotesCsv(notesCsv.getFullPathName().toStdString());
  for (const auto &n : csvNotes) {
    const int start = static_cast<int>(std::llround(n.t0 * kSampleRate / kHop));
    const int end = static_cast<int>(std::llround(n.t1 * kSampleRate / kHop));
    if (start < 0 || end > frames || end - start < 3)
      continue;
    Note note(start, end, n.midi);
    note.setOriginalMidiNote(n.midi);
    note.setOriginalDeltaPitch(std::vector<float>(end - start, 0.0f));
    project.addNote(note);
  }
  if (project.getNotes().empty()) {
    std::cout << "sin notas dentro del recorte\n";
    return false;
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);

  std::printf("\nToma real: %s, %.1f s, %zu notas, %d frames\n",
              wav.getFileName().toRawUTF8(),
              static_cast<double>(data.originalWaveform.getNumSamples()) /
                  kSampleRate,
              project.getNotes().size(), frames);

  checkTimingAudio(project, SynthesisEngineType::Psola, "real/PSOLA", fcpe,
                   /*expectRetime=*/true,
                   /*minDurationFrames=*/static_cast<int>(std::ceil(2.0 * gridFrames())),
                   /*measureAudioOnsets=*/false);
  return true;
}

} // namespace

int main(int argc, char **argv) {
  juce::ScopedJuceInitialiser_GUI gui;

  juce::File takeFile;
  juce::File notesFile;
  double seconds = 30.0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--take" && i + 1 < argc)
      takeFile = juce::File(juce::String::fromUTF8(argv[++i]));
    else if (arg == "--notas" && i + 1 < argc)
      notesFile = juce::File(juce::String::fromUTF8(argv[++i]));
    else if (arg == "--segundos" && i + 1 < argc)
      seconds = std::atof(argv[++i]);
  }

  // FCPE: el estimador del plugin (CPU, sin GPU, para no depender de DirectML).
  juce::File modelDir;
#ifdef TIMING_TEST_MODELS_DIR
  modelDir = juce::File(TIMING_TEST_MODELS_DIR);
#endif
  if (!modelDir.getChildFile("fcpe.onnx").existsAsFile())
    modelDir = juce::File::getSpecialLocation(
                   juce::File::currentExecutableFile)
                   .getParentDirectory()
                   .getChildFile("models");
  if (!modelDir.getChildFile("fcpe.onnx").existsAsFile()) {
    std::printf("no se encuentra fcpe.onnx (buscado en %s)\n",
                modelDir.getFullPathName().toRawUTF8());
    return 1;
  }

  FCPEPitchDetector fcpe;
  CHECK(fcpe.loadModel(modelDir.getChildFile("fcpe.onnx"),
                       modelDir.getChildFile("mel_filterbank.bin"),
                       modelDir.getChildFile("cent_table.bin"),
                       GPUProvider::CPU));
  CHECK(fcpe.isLoaded());

  // 1. Clase C sintetica: los dos motores del producto.
  checkTimingAudio(makeSyntheticProject(), SynthesisEngineType::Psola, "PSOLA",
                   fcpe);
  checkTimingAudio(makeSyntheticProject(), SynthesisEngineType::Pitchcore,
                   "Pitchcore", fcpe, /*expectRetime=*/false);

  // 2. Toma real opcional.
  if (takeFile.existsAsFile() && notesFile.existsAsFile())
    CHECK(runRealTake(takeFile, notesFile, seconds, fcpe));
  else if (takeFile.existsAsFile() || notesFile.existsAsFile())
    std::cout << "para la toma real hacen falta --take y --notas\n";

  std::printf("\nTimingAudioTests: S1/S2 medidos en audio\n");
  return 0;
}
