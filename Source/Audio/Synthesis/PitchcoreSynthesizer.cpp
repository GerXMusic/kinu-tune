#include "PitchcoreSynthesizer.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "pitchcore/psola.h"

#include "../../Utils/AppLogger.h"
#include "PitchcoreRequest.h"
#include "PitchcoreVoicedGate.h"

namespace {

/// Un worker propio, hermano del de PsolaSynthesizer: mismo reparto (un hilo
/// largo, cola de una tarea, porque IncrementalSynthesizer ya cancela el
/// trabajo anterior) pero sin tocar el fichero del PSOLA del fork: el nivel 2
/// de D-093 exige que PSOLA quede intacto con el adaptador compilado.
struct AsyncTask
{
    DspSynthesisRequest request;
    std::function<void(std::vector<float>)> callback;
    std::shared_ptr<std::atomic<bool>> cancelFlag;
};

bool isCancelled(const std::shared_ptr<std::atomic<bool>> &flag)
{
    return flag != nullptr && flag->load();
}

#if PITCHNET_LOGGING_ENABLED
/// D-103: volca la peticion REAL que recibe el motor, para el nivel 4.
///
/// Binario little-endian, sin conversiones: los mismos float32/float64/uint8
/// que se le pasan al motor. El arnes lo ejecuta por toCoreRequest +
/// ClassicEngine y cuenta las marcas de periodo del tramo.
juce::String dumpRequest(const DspSynthesisRequest &request)
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("opencode")
                         .getChildFile("bounce");
    if (!dir.createDirectory())
        return {};
    // Un fichero por tramo: un render puede sintetizar varias regiones y no
    // queremos que la ultima pise a la de la nota. D-106: tampoco se pisa una
    // fixtura anterior; si el nombre ya existe se anade sufijo _v2, _v3...
    const juce::String stem =
        "pitchcore_request_" + juce::String(request.sourceStartSample) + "_" +
        juce::String(request.numOutputFrames);
    juce::File target = dir.getChildFile(stem + ".bin");
    for (int version = 2; target.existsAsFile() && version < 100; ++version)
        target = dir.getChildFile(stem + "_v" + juce::String(version) + ".bin");
    std::ofstream f(target.getFullPathName().toStdString(),
                    std::ios::binary | std::ios::trunc);
    if (!f)
        return {};

    const char magic[8] = {'P', 'C', 'R', 'E', 'Q', '1', 0, 0};
    f.write(magic, 8);
    auto w32 = [&f](int32_t v) { f.write(reinterpret_cast<const char *>(&v), 4); };
    w32(1);
    w32(request.hopSize);
    w32(request.sampleRate);
    w32(request.sourceStartSample);
    w32(request.numOutputFrames);
    const uint8_t formants = request.preserveFormants ? 1 : 0;
    f.write(reinterpret_cast<const char *>(&formants), 1);
    w32(static_cast<int32_t>(request.source.size()));
    w32(static_cast<int32_t>(request.sourceFrame.size()));
    w32(static_cast<int32_t>(request.pitchRatio.size()));
    w32(static_cast<int32_t>(request.sourceF0.size()));
    w32(static_cast<int32_t>(request.sourceVoiced.size()));

    auto wbytes = [&f](const void *p, size_t bytes) {
        f.write(reinterpret_cast<const char *>(p), static_cast<std::streamsize>(bytes));
    };
    if (!request.source.empty())
        wbytes(request.source.data(), request.source.size() * sizeof(float));
    if (!request.sourceFrame.empty())
        wbytes(request.sourceFrame.data(), request.sourceFrame.size() * sizeof(double));
    if (!request.pitchRatio.empty())
        wbytes(request.pitchRatio.data(), request.pitchRatio.size() * sizeof(float));
    if (!request.sourceF0.empty())
        wbytes(request.sourceF0.data(), request.sourceF0.size() * sizeof(float));
    if (!request.sourceVoiced.empty())
        wbytes(request.sourceVoiced.data(), request.sourceVoiced.size());

    f.flush();
    return f ? target.getFullPathName() : juce::String();
}
#endif

class RenderWorker
{
public:
    static RenderWorker &get()
    {
        static RenderWorker instance;
        return instance;
    }

    void post(AsyncTask task)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.push_back(std::move(task));
        }
        condition.notify_one();
    }

private:
    RenderWorker() { thread = std::thread([this]() { run(); }); }

    ~RenderWorker()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            shuttingDown = true;
        }
        condition.notify_all();
        if (thread.joinable())
            thread.join();
    }

    void run()
    {
        for (;;)
        {
            AsyncTask task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this]() { return shuttingDown || !queue.empty(); });
                if (shuttingDown)
                    return;
                task = std::move(queue.front());
                queue.pop_front();
            }

            std::vector<float> result;
            if (!isCancelled(task.cancelFlag))
                result = PitchcoreSynthesizer::render(task.request, task.cancelFlag);

            if (task.callback)
                task.callback(std::move(result));
        }
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::deque<AsyncTask> queue;
    bool shuttingDown = false;
    std::thread thread;
};

} // namespace

PitchcoreSynthesizer::PitchcoreSynthesizer() = default;
PitchcoreSynthesizer::~PitchcoreSynthesizer() = default;

void PitchcoreSynthesizer::renderAsync(
    Request request, std::function<void(std::vector<float>)> callback,
    std::shared_ptr<std::atomic<bool>> cancelFlag)
{
    RenderWorker::get().post(
        AsyncTask{std::move(request), std::move(callback), std::move(cancelFlag)});
}

pitchcore::RenderRequest PitchcoreSynthesizer::toCoreRequest(const Request &request)
{
    // La traduccion vive en PitchcoreRequest.h, sin JUCE, para que el arnes de
    // D-093 la ejercite sin enlazar el plugin (tools/pitchcore_adapter_check).
    return pitchcore_adapter::toCoreRequest(request);
}

std::vector<float> PitchcoreSynthesizer::render(
    const Request &requestIn,
    const std::shared_ptr<std::atomic<bool>> &cancelFlag)
{
    // D-106: compuerta de voiced SOLO para este motor. Se aplica antes del log
    // y del volcado (D-103) para que la fixtura lleve la mascara que recibe el
    // motor; no toca IncrementalSynthesizer (peticion del PSOLA del fork) ni el
    // nucleo vendorizado.
    // D-109: DESACTIVADA por defecto hasta que pase el porton: cambia otros
    // tramos (19 -> 1049 marcas en la fixtura de D-107) y no esta evaluada.
    // Para medirla se pone a true y se recompila.
    constexpr bool kVoicedGateEnabled = false;
    DspSynthesisRequest gatedRequest;
    int voicedCleared = 0;
    if (kVoicedGateEnabled)
    {
        gatedRequest = requestIn;
        voicedCleared = pitchcore_voiced_gate::apply(gatedRequest);
    }
    const Request &request = kVoicedGateEnabled ? gatedRequest : requestIn;

#if PITCHNET_LOGGING_ENABLED
    // I-10 (D-101): que el motor se invoque, con que peticion y que devuelve.
    // Sin esto, un fallo del adaptador y uno del despacho son indistinguibles
    // (el `err` del motor se descartaba).
    const double renderT0 = juce::Time::getMillisecondCounterHiRes();
    const double firstSource =
        request.sourceFrame.empty() ? 0.0 : request.sourceFrame.front();
    const int firstFrame = static_cast<int>(std::llround(firstSource));
    float ratioMin = 1.0f, ratioMax = 1.0f;
    for (float r : request.pitchRatio) {
      ratioMin = std::min(ratioMin, r);
      ratioMax = std::max(ratioMax, r);
    }
    int voicedInSpan = 0;
    for (int i = firstFrame;
         i < firstFrame + request.numOutputFrames &&
         i < static_cast<int>(request.sourceVoiced.size());
         ++i)
      voicedInSpan += request.sourceVoiced[static_cast<size_t>(i)] ? 1 : 0;
    LOG(juce::String("pitchcore adapter: invocado frames=") +
        juce::String(request.numOutputFrames) + " hop=" +
        juce::String(request.hopSize) + " sr=" +
        juce::String(request.sampleRate) + " primeraFrame=" +
        juce::String(firstFrame) + " src0=" +
        juce::String(request.sourceStartSample) + " src=" +
        juce::String(static_cast<int>(request.source.size())) + " ratio=[" +
        juce::String(ratioMin, 4) + "," + juce::String(ratioMax, 4) + "] f0=" +
        juce::String(static_cast<int>(request.sourceF0.size())) + " voiced=" +
        juce::String(voicedInSpan));
    // D-103: la fixtura del nivel 4 sale de la MISMA peticion que se registra
    // y se le pasa al motor.
    LOG(juce::String("pitchcore adapter: voiced gate: activa=") +
        juce::String(kVoicedGateEnabled ? 1 : 0) + " limpiados=" +
        juce::String(voicedCleared) + " (D-107/D-109)");
    const juce::String dumpPath = dumpRequest(request);
    if (dumpPath.isNotEmpty())
        LOG("pitchcore adapter: peticion volcada para el nivel 4 (D-103) en " + dumpPath);
    else
        LOG("pitchcore adapter: no se pudo volcar la peticion (D-103)");
#endif

    if (request.numOutputFrames <= 0 || request.hopSize <= 0 || request.source.empty())
    {
#if PITCHNET_LOGGING_ENABLED
        LOG(juce::String("pitchcore adapter: peticion vacia frames=") +
            juce::String(request.numOutputFrames) + " hop=" +
            juce::String(request.hopSize) + " src=" +
            juce::String(static_cast<int>(request.source.size())));
#endif
        return {};
    }

    if (isCancelled(cancelFlag))
    {
#if PITCHNET_LOGGING_ENABLED
        LOG("pitchcore adapter: cancelado antes de renderizar");
#endif
        return {};
    }

    const pitchcore::RenderRequest coreRequest = toCoreRequest(request);

    pitchcore::ClassicEngine engine;
    std::vector<float> out;
    std::string err;
    if (!engine.render(coreRequest, out, err))
    {
#if PITCHNET_LOGGING_ENABLED
        LOG(juce::String("pitchcore adapter: motor=false err='") + err.c_str() +
            "'");
#endif
        return {};
    }

    if (isCancelled(cancelFlag))
    {
#if PITCHNET_LOGGING_ENABLED
        LOG("pitchcore adapter: cancelado despues de renderizar");
#endif
        return {};
    }

#if PITCHNET_LOGGING_ENABLED
    {
        // El origen de SALIDA es la primera frame de la peticion, no source[0]
        // (D-096/D-097). Si rms/max salen 0, el motor devolvio el original tal
        // cual: el camino de "sin marcas de periodo" (psola.cpp:246-258).
        const long outOrigin =
            std::lround(firstSource * static_cast<double>(request.hopSize));
        double energy = 0.0, maxDiff = 0.0;
        long compared = 0;
        for (size_t i = 0; i < out.size(); ++i) {
          const long idx = outOrigin + static_cast<long>(i) -
                           static_cast<long>(request.sourceStartSample);
          if (idx < 0 || idx >= static_cast<long>(request.source.size()))
            continue;
          const double d = static_cast<double>(out[i]) -
                           static_cast<double>(request.source[static_cast<size_t>(idx)]);
          energy += d * d;
          maxDiff = std::max(maxDiff, std::fabs(d));
          ++compared;
        }
        const double rmsDiff =
            compared > 0 ? std::sqrt(energy / static_cast<double>(compared)) : -1.0;
        LOG(juce::String("pitchcore adapter: fin out=") +
            juce::String(static_cast<int>(out.size())) + " err='" + err.c_str() +
            "' vsOriginal rms=" + juce::String(rmsDiff, 8) + " max=" +
            juce::String(maxDiff, 8) + " n=" + juce::String(static_cast<int>(compared)) +
            " took=" +
            juce::String(juce::Time::getMillisecondCounterHiRes() - renderT0, 1) +
            " ms");
    }
#endif

    return out;
}
