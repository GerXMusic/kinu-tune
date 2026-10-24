#include "PitchcoreSynthesizer.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "pitchcore/psola.h"

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
    pitchcore::RenderRequest core;

    core.original = &request.source;
    // El tramo no empieza en la muestra 0 del proyecto: el nucleo lee por
    // posiciones absolutas y este campo le dice donde cae source[0] (D-096).
    core.originalStartSample = request.sourceStartSample;
    core.sourceStartSample = request.sourceStartSample;
    core.numSamples = request.numOutputFrames * request.hopSize;
    core.curveHop = static_cast<double>(request.hopSize);
    core.sampleRate = static_cast<double>(request.sampleRate);
    core.preserveFormants = request.preserveFormants;

    // Las dos curvas del fork vienen indexadas por frame LOCAL de la region
    // (indice 0 = primer frame renderizado). El nucleo las muestrea en
    // frame = muestra_absoluta / hop, asi que se rellena el tramo anterior con
    // lo mismo que el propio nucleo devuelve fuera de rango: identidad para
    // sourceFrame y plano para pitchRatio. Asi el indice local y el absoluto
    // coinciden sin tocar el contrato.
    const int hop = request.hopSize;
    const int base = hop > 0
                         ? static_cast<int>(std::floor(
                               static_cast<double>(request.sourceStartSample) / hop))
                         : 0;
    const int frames = std::max(0, request.numOutputFrames);
    core.totalFrames = base + frames;

    core.sourceFrame.assign(static_cast<size_t>(core.totalFrames), 0.0);
    core.pitchRatio.assign(static_cast<size_t>(core.totalFrames), 1.0f);

    const double firstSource =
        request.sourceFrame.empty() ? 0.0 : request.sourceFrame.front();
    const float firstRatio =
        request.pitchRatio.empty() ? 1.0f : request.pitchRatio.front();
    for (int j = 0; j < base; ++j)
    {
        core.sourceFrame[static_cast<size_t>(j)] =
            firstSource - static_cast<double>(base - j);
        core.pitchRatio[static_cast<size_t>(j)] = firstRatio;
    }

    const int copyFrames =
        std::min(frames, static_cast<int>(std::min(request.sourceFrame.size(),
                                                   request.pitchRatio.size())));
    for (int i = 0; i < copyFrames; ++i)
    {
        core.sourceFrame[static_cast<size_t>(base + i)] =
            request.sourceFrame[static_cast<size_t>(i)];
        core.pitchRatio[static_cast<size_t>(base + i)] =
            request.pitchRatio[static_cast<size_t>(i)];
    }

    // F0 y voicing ya vienen en indexacion ABSOLUTA de la fuente: se copian.
    core.sourceF0 = request.sourceF0;
    core.sourceVoiced = request.sourceVoiced;

    return core;
}

std::vector<float> PitchcoreSynthesizer::render(
    const Request &request,
    const std::shared_ptr<std::atomic<bool>> &cancelFlag)
{
    if (request.numOutputFrames <= 0 || request.hopSize <= 0 || request.source.empty())
        return {};

    if (isCancelled(cancelFlag))
        return {};

    const pitchcore::RenderRequest coreRequest = toCoreRequest(request);

    pitchcore::ClassicEngine engine;
    std::vector<float> out;
    std::string err;
    if (!engine.render(coreRequest, out, err))
        return {};

    if (isCancelled(cancelFlag))
        return {};

    return out;
}
