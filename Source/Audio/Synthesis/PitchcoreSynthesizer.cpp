#include "PitchcoreSynthesizer.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "pitchcore/psola.h"

#include "PitchcoreRequest.h"

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
    // La traduccion vive en PitchcoreRequest.h, sin JUCE, para que el arnes de
    // D-093 la ejercite sin enlazar el plugin (tools/pitchcore_adapter_check).
    return pitchcore_adapter::toCoreRequest(request);
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
