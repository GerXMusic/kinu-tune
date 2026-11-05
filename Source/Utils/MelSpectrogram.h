#pragma once

#include "../JuceHeader.h"
#include <vector>

/**
 * Mel spectrogram computation.
 */
class MelSpectrogram
{
public:
    MelSpectrogram(int sampleRate = 44100, int nFft = 2048, int hopSize = 512,
                   int numMels = 128, float fMin = 40.0f, float fMax = 16000.0f);
    ~MelSpectrogram() = default;
    
    /**
     * Compute mel spectrogram from audio.
     * @param audio Audio samples
     * @param numSamples Number of samples
     * @param numThreads 0 = choose automatically (up to 4); 1 = serial.
     * @return Mel spectrogram [T, numMels] in log scale
     */
    std::vector<std::vector<float>> compute(const float* audio, int numSamples,
                                            int numThreads = 0);
    
private:
    /** Frames [frameBegin, frameEnd) into distinct mel slots. */
    void computeFrames(const float* audio, int numSamples, int frameBegin,
                       int frameEnd,
                       std::vector<std::vector<float>>& mel) const;
    /** Sparse representation of one mel filter band. */
    struct MelBand {
        int startBin = 0;          // first non-zero FFT bin (inclusive)
        int endBin   = 0;          // last non-zero FFT bin (exclusive)
        std::vector<float> weights; // weights[k - startBin]
    };

    void createMelFilterbank();
    void applyWindow(std::vector<float>& frame);
    std::vector<float> computeFFT(const std::vector<float>& frame);
    
    int sampleRate;
    int nFft;
    int hopSize;
    int numMels;
    float fMin;
    float fMax;
    
    std::vector<float> window;  // Hann window
    std::vector<MelBand> melFilterbank;  // sparse filterbank
    
    juce::dsp::FFT fft;
};
