#include "pitchcore/wav.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace pitchcore {

static bool fail(std::string* e, const char* m) { if (e) *e = m; return false; }

bool readWav(const std::string& path, WavData& out, std::string* err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail(err, "no se puede abrir el archivo");
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4))
        return fail(err, "no es RIFF/WAVE");

    auto u16 = [&](size_t o) { return (uint32_t)(b[o] | (b[o + 1] << 8)); };
    auto u32 = [&](size_t o) { return (uint32_t)(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | ((uint32_t) b[o + 3] << 24)); };

    int fmt = 0, ch = 0, bits = 0; uint32_t sr = 0;
    size_t dataOff = 0, dataLen = 0;
    for (size_t o = 12; o + 8 <= b.size();) {
        const uint32_t len = u32(o + 4);
        if (!std::memcmp(b.data() + o, "fmt ", 4) && o + 8 + 16 <= b.size()) {
            fmt = (int) u16(o + 8); ch = (int) u16(o + 10); sr = u32(o + 12); bits = (int) u16(o + 22);
            if (fmt == 0xFFFE && len >= 26 && o + 8 + 26 <= b.size()) fmt = (int) u16(o + 8 + 24); // EXTENSIBLE
        } else if (!std::memcmp(b.data() + o, "data", 4)) {
            dataOff = o + 8; dataLen = std::min<size_t>(len, b.size() - dataOff); break;
        }
        o += 8 + (size_t) len + (len & 1);
    }
    if (!dataOff || ch < 1 || !sr) return fail(err, "cabecera WAV incompleta");
    if (!((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32)))
        return fail(err, "formato no soportado (usa PCM 16/24/32 o float32)");

    const int bps = bits / 8;
    const size_t frames = dataLen / (size_t) (bps * ch);
    out.sampleRate = sr; out.channels = ch; out.mono.assign(frames, 0.f);
    for (size_t i = 0; i < frames; ++i) {
        double acc = 0;
        for (int c = 0; c < ch; ++c) {
            const unsigned char* q = b.data() + dataOff + (i * (size_t) ch + (size_t) c) * (size_t) bps;
            double v;
            if (fmt == 3)        { float fl; std::memcpy(&fl, q, 4); v = fl; }
            else if (bits == 16) v = (int16_t)(q[0] | (q[1] << 8)) / 32768.0;
            else if (bits == 24) { int32_t s = (int32_t) ((uint32_t) q[0] << 8 | (uint32_t) q[1] << 16 | (uint32_t) q[2] << 24); v = (s >> 8) / 8388608.0; }
            else                 { int32_t s = (int32_t) ((uint32_t) q[0] | (uint32_t) q[1] << 8 | (uint32_t) q[2] << 16 | (uint32_t) q[3] << 24); v = s / 2147483648.0; }
            acc += v;
        }
        out.mono[i] = (float) (acc / ch);
    }
    return true;
}

bool writeWav16(const std::string& path, double sr, const std::vector<float>& x)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto w32 = [&](uint32_t v) { f.write((const char*) &v, 4); };
    auto w16 = [&](uint16_t v) { f.write((const char*) &v, 2); };
    const uint32_t bytes = (uint32_t) (x.size() * 2);
    f.write("RIFF", 4); w32(36 + bytes); f.write("WAVEfmt ", 8); w32(16); w16(1); w16(1);
    w32((uint32_t) sr); w32((uint32_t) sr * 2); w16(2); w16(16);
    f.write("data", 4); w32(bytes);
    for (float v : x) { double d = std::max(-1.0, std::min(1.0, (double) v)); int16_t s = (int16_t) std::lround(d * 32767.0); w16((uint16_t) s); }
    return (bool) f;
}

bool writeWav24(const std::string& path, double sr, const std::vector<float>& x)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto w32 = [&](uint32_t v) { f.write((const char*) &v, 4); };
    auto w16 = [&](uint16_t v) { f.write((const char*) &v, 2); };
    auto w24 = [&](uint32_t v) {
        const unsigned char b[3] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF),
                                    (unsigned char)((v >> 16) & 0xFF) };
        f.write((const char*) b, 3);
    };
    const uint32_t dataLen = (uint32_t) (x.size() * 3);
    f.write("RIFF", 4); w32(36 + dataLen); f.write("WAVE", 4);
    // formatTag 1 = PCM,  blockAlign 3 bytes por muestra, 24 bits
    f.write("fmt ", 4); w32(16); w16(1); w16(1); w32((uint32_t) sr); w32((uint32_t) sr * 3); w16(3); w16(24);
    f.write("data", 4); w32(dataLen);
    for (float v : x) {
        const double d = std::max(-1.0, std::min(1.0, (double) v));
        w24((uint32_t)(int32_t) std::lround(d * 8388607.0));
    }
    return (bool) f;
}

bool writeWavMono(const std::string& path, const std::vector<float>& x, double sr)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto w32 = [&](uint32_t v) { f.write((const char*) &v, 4); };
    auto w16 = [&](uint16_t v) { f.write((const char*) &v, 2); };
    const uint32_t dataLen = (uint32_t) (x.size() * 4);
    f.write("RIFF", 4); w32(36 + dataLen); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(3); w16(1); w32((uint32_t) sr); w32((uint32_t) sr * 4); w16(4); w16(32);
    f.write("data", 4); w32(dataLen);
    f.write((const char*) x.data(), (std::streamsize) dataLen);
    return (bool) f;
}

} // namespace pitchcore
