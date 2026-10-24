#pragma once
#include <string>
#include <vector>

namespace pitchcore {

struct WavData {
    double sampleRate = 0;
    int channels = 0;
    std::vector<float> mono; // mezcla a mono
};

// PCM 16/24/32 y float32. Devuelve false si el formato no se soporta.
bool readWav(const std::string& path, WavData& out, std::string* err = nullptr);

// Escribe PCM 16-bit mono (con saturacion a +-1).
bool writeWav16(const std::string& path, double sampleRate, const std::vector<float>& mono);

// Escribe PCM 24-bit mono (con saturacion a +-1). Formato del corpus entregado.
bool writeWav24(const std::string& path, double sampleRate, const std::vector<float>& mono);

// WAV mono float32
bool writeWavMono(const std::string& path, const std::vector<float>& x, double sampleRate);

} // namespace pitchcore
