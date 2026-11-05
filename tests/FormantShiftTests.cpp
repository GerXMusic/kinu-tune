#include "../Source/Audio/Synthesis/FormantShifter.h"
#include "../Source/Models/Note.h"
#include "../Source/Models/ProjectSerializer.h"
#include "../Source/Audio/Synthesis/PsolaSynthesizer.h"
#include "../Source/Utils/TransformParams.h"
#include "TestAssert.h"
#include <iostream>

// A periodic source with a broad resonance at 1 kHz. Measure individual
// harmonics, rather than trusting FFT-bin positions as a pitch test.
static float amplitude(const std::vector<float>& audio, float hz, int rate) {
    double re = 0, im = 0;
    const int begin = rate / 4, end = 3 * rate / 4;
    for (int i = begin; i < end; ++i) {
        const double phase = 2 * 3.141592653589793 * hz * i / rate;
        re += audio[i] * std::cos(phase);
        im += audio[i] * std::sin(phase);
    }
    return static_cast<float>(std::hypot(re, im) / (end - begin));
}
int main(int argc, char** argv) {
    constexpr int rate = 44100, hop = 512;
    std::vector<float> source(rate, 0.0f), f0(rate / hop + 1, 100.0f);
    for (int harmonic = 1; harmonic <= 100; ++harmonic) {
        const float hz = 100.0f * harmonic;
        const float gain = 0.002f + 0.08f * std::exp(-0.5f * std::pow((hz - 1000.0f) / 200.0f, 2));
        for (int i = 0; i < rate; ++i)
            source[i] += gain * std::sin(2 * 3.141592653589793 * hz * i / rate);
    }
    auto bypass = source;
    CHECK(formant::process(bypass, std::vector<float>(f0.size(), 0), f0, hop, rate));
    CHECK(bypass == source);
    for (float shift : {-5.0f, 5.0f}) {
        auto output = source;
        CHECK(formant::process(output, std::vector<float>(f0.size(), shift), f0, hop, rate));
        CHECK(output.size() == source.size());
        for (float v : output) CHECK(std::isfinite(v));
        float peak = 0; int peakHz = 0;
        for (int hz = 400; hz <= 2000; hz += 100) {
            const float a = amplitude(output, hz, rate);
            if (a > peak) { peak = a; peakHz = hz; }
        }
        std::cout << shift << " st: envelope peak " << peakHz << " Hz\n";
        CHECK(shift > 0 ? peakHz >= 1200 : peakHz <= 900);
        // Periodicity remains 100 Hz even when the resonance moves.
        double err = 0, energy = 0;
        for (int i = rate / 4; i < rate * 3 / 4; ++i) {
            err += std::pow(output[i] - output[i + 441], 2);
            energy += output[i] * output[i];
        }
        std::cout << "periodicity error " << err / energy << '\n';
        CHECK(err / energy < 0.01);
    }
    auto silence = std::vector<float>(4096, 0);
    CHECK(formant::process(silence, {12}, {}, hop, rate));
    for (float v : silence) CHECK(v == 0);
    std::atomic<bool> cancelled{true};
    auto output = source;
    CHECK(!formant::process(output, {5}, f0, hop, rate, &cancelled));
    CHECK(output == source);
    Note note(0, 10, 60);
    const auto old = TransformParams::fromNote(note);
    note.setFormantShift(3.4f);
    CHECK(!note.isNeutralForOriginalWaveform());
    CHECK(note.getMidiNote() == 60);
    const auto changed = TransformParams::fromNote(note);
    old.applyToNote(note);
    CHECK(note.getFormantShift() == 0 && note.isNeutralForOriginalWaveform());
    changed.applyToNote(note);
    CHECK(note.getFormantShift() == 3.4f && note.getMidiNote() == 60);
    Project project;
    project.setFormantShift(-1.2f);
    project.addNote(note);
    Project restored;
    CHECK(ProjectSerializer::fromJson(restored, ProjectSerializer::toJson(project)));
    CHECK(restored.getNotes().front().getFormantShift() == 3.4f);
    CHECK(restored.getFormantShift() == -1.2f);
    auto legacy = ProjectSerializer::toJson(project);
    legacy.getDynamicObject()->getProperty("notes").getArray()->getReference(0).getDynamicObject()->removeProperty("formantShift");
    CHECK(ProjectSerializer::fromJson(restored, legacy));
    CHECK(restored.getNotes().front().getFormantShift() == 0);

    PsolaSynthesizer::Request request;
    request.source = source;
    request.sourceF0 = f0;
    request.sourceVoiced.assign(f0.size(), 1);
    request.numOutputFrames = static_cast<int>(f0.size()) - 1;
    request.pitchRatio.assign(request.numOutputFrames, 1.0f);
    for (int i = 0; i < request.numOutputFrames; ++i) request.sourceFrame.push_back(i);
    auto psola = PsolaSynthesizer::render(request);
    CHECK(!psola.empty());
    CHECK(formant::process(psola, {5}, f0, hop, rate));
    CHECK(amplitude(psola, 1300, rate) > amplitude(psola, 1000, rate));

    if (argc > 1) {
        project.clearNotes();
        Note fixture(0, request.numOutputFrames, 43.3499577f);
        fixture.setF0Values(f0);
        fixture.setDeltaPitch(std::vector<float>(request.numOutputFrames, 0));
        fixture.setOriginalDeltaPitch(fixture.getDeltaPitch());
        project.addNote(fixture);
        project.setFormantShift(0);
        auto& audio = project.getAudioData();
        audio.sampleRate = rate;
        audio.waveform.setSize(1, rate);
        audio.waveform.copyFrom(0, 0, source.data(), rate);
        audio.originalWaveform.makeCopyOf(audio.waveform);
        audio.f0 = audio.denseF0 = audio.rawF0 = audio.baseF0 = f0;
        audio.basePitch.assign(f0.size(), 43.3499577f);
        audio.deltaPitch.assign(f0.size(), 0);
        audio.voicedMask.assign(f0.size(), true);
        audio.melSpectrogram.assign(f0.size(), std::vector<float>(128, -5));
        CHECK(ProjectSerializer::saveToFile(project, juce::File(argv[1])));
    }
    std::cout << "Formant tests passed\n";
}

