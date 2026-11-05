// D-180: the mel rebuild that host-backed ARA archives do on load blocked the
// message thread for ~0.8 s. Frames are independent, so the threaded path must
// be bit-identical and measurably faster; short buffers stay serial.
#include "../Source/Utils/MelSpectrogram.h"
#include "TestAssert.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
double timeCompute(MelSpectrogram &mel, const std::vector<float> &audio,
                   int threads)
{
    const auto start = std::chrono::steady_clock::now();
    const auto result = mel.compute(audio.data(),
                                    static_cast<int>(audio.size()), threads);
    const auto end = std::chrono::steady_clock::now();
    CHECK(!result.empty());
    return std::chrono::duration<double, std::milli>(end - start).count();
}
} // namespace

int main()
{
    constexpr int sampleRate = 48000;
    const int numSamples = sampleRate * 60;
    std::vector<float> audio(static_cast<size_t>(numSamples));
    for (int i = 0; i < numSamples; ++i)
        audio[static_cast<size_t>(i)] =
            0.10f * std::sin(2.0f * 3.14159265358979323846f * 220.0f * i /
                             sampleRate) +
            0.05f * std::sin(2.0f * 3.14159265358979323846f * 373.0f * i /
                             sampleRate);

    MelSpectrogram mel(sampleRate, 2048, 240, 80, 40.0f, 16000.0f);
    const auto serial = mel.compute(audio.data(), numSamples, 1);
    const auto threaded = mel.compute(audio.data(), numSamples, 0);

    CHECK(!serial.empty());
    CHECK(serial.size() == threaded.size());
    for (size_t frame = 0; frame < serial.size(); ++frame)
    {
        CHECK(serial[frame].size() == threaded[frame].size());
        for (size_t bin = 0; bin < serial[frame].size(); ++bin)
            CHECK(serial[frame][bin] == threaded[frame][bin]);
    }

    // The threshold is deliberately loose (25%): it must fail when the auto
    // path is serial and pass on a two-core machine without depending on the
    // exact core count.
    const unsigned cores = std::thread::hardware_concurrency();
    if (cores >= 2)
    {
        const double serialMs = timeCompute(mel, audio, 1);
        const double autoMs = timeCompute(mel, audio, 0);
        std::cout << "mel serial=" << serialMs << " ms auto=" << autoMs
                  << " ms cores=" << cores << "\n";
        CHECK(autoMs < serialMs * 0.75);
    }
    else
    {
        std::cout << "mel timing skipped: single core\n";
    }

    std::cout << "Mel spectrogram tests passed\n";
    return 0;
}
