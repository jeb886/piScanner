#include "Scanner.h"

#include "Decoder.h"
#include "Dedupe.h"
#include "Log.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {

volatile std::sig_atomic_t shutdownRequested = 0;

struct Work {
    int index = -1;
    Frame frame;
};

struct WorkerSlot {
    bool busy = false;
    std::optional<Work> work;
};

}

void requestShutdownSignal(int)
{
    shutdownRequested = 1;
}

struct Scanner::Impl {
    const Config& config;
    FrameSource& camera;
    std::atomic<std::uint64_t>* sessionEvents;
    ResultSink& sink;
    Trigger* trigger;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::vector<WorkerSlot> slots;
    std::optional<Work> pending;
    std::vector<std::thread> workers;
    std::vector<std::unique_ptr<Decoder>> decoders;
    std::thread captureThread;
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::string errorText;
    bool armed = false;
    std::uint64_t pressTimestampUs = 0;
    std::uint64_t sessionStartedUs = 0;
    int armedFrames = 0; // frames actually submitted for decode this session
    Dedupe dedupe;
    std::atomic<std::uint64_t> captured{0};
    std::atomic<std::uint64_t> processed{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<double> decodeTotalMs{0};
    std::atomic<double> decodeMaxMs{0};

    Impl(const Config& cfg, FrameSource& cam, ResultSink& output, Trigger* input,
         std::atomic<std::uint64_t>* sessions)
        : config(cfg), camera(cam), sessionEvents(sessions), sink(output), trigger(input),
          slots(static_cast<std::size_t>(cfg.threads))
    {
        armed = config.command == "scan";
    }

    void fail(const std::string& message)
    {
        logLine("error " + message);
        {
            std::lock_guard lock(mutex);
            if (!failed.exchange(true))
                errorText = message;
        }
        stop.store(true);
        changed.notify_all();
    }

    void discardPendingLocked()
    {
        if (pending) {
            try {
                camera.requeue(pending->index);
                dropped.fetch_add(1);
            } catch (const std::exception& exception) {
                if (!failed.exchange(true))
                    errorText = exception.what();
            }
            pending.reset();
        }
    }

    void captureLoop()
    {
        try {
            while (!stop.load() && !shutdownRequested) {
                Frame frame;
                int index = -1;
                // Sources that pause while disarmed (file mocks) hold back
                // production until a session is armed so each press maps
                // onto the next frame.
                if (config.command == "trigger" && camera.waitsWhileDisarmed()) {
                    std::unique_lock lock(mutex);
                    if (!armed) {
                        lock.unlock();
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        continue;
                    }
                }
                if (!camera.dequeue(frame, index, 100))
                    continue;
                captured.fetch_add(1);
                std::lock_guard lock(mutex);
                if (stop.load() || !armed) {
                    camera.requeue(index);
                    continue;
                }
                auto idle = std::find_if(slots.begin(), slots.end(), [](const WorkerSlot& slot) {
                    return !slot.busy;
                });
                if (idle != slots.end()) {
                    idle->busy = true;
                    idle->work = Work{index, frame};
                    changed.notify_all();
                } else {
                    if (pending) {
                        camera.requeue(pending->index);
                        dropped.fetch_add(1);
                    }
                    pending = Work{index, frame};
                }
            }
        } catch (const std::exception& exception) {
            fail(exception.what());
        }
    }

    void workerLoop(std::size_t workerIndex)
    {
        Decoder& decoder = *decoders[workerIndex];
        for (;;) {
            Work work;
            bool decodeFrame = false;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return stop.load() || slots[workerIndex].work.has_value(); });
                if (stop.load() && !slots[workerIndex].work)
                    return;
                work = *slots[workerIndex].work;
                slots[workerIndex].work.reset();
                decodeFrame = armed && (config.command != "trigger" || work.frame.timestampUs >= pressTimestampUs);
                if (decodeFrame)
                    ++armedFrames;
            }

            std::vector<ScanResult> results;
            double elapsed = 0;
            if (decodeFrame) {
                try {
                    const auto decodeStart = std::chrono::steady_clock::now();
                    results = decoder.decode(work.frame);
                    elapsed =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - decodeStart)
                            .count();
                    processed.fetch_add(1);
                    decodeTotalMs.fetch_add(elapsed);
                    double maximum = decodeMaxMs.load();
                    while (elapsed > maximum && !decodeMaxMs.compare_exchange_weak(maximum, elapsed)) {
                    }
                } catch (const std::exception& exception) {
                    if (config.verbose)
                        std::cerr << "warning: frame decode failed: " << exception.what() << '\n';
                    logLine(std::string("decode error ") + exception.what());
                }
            }

            for (const auto& result : results) {
                std::optional<ScanResult> emitted;
                {
                    std::lock_guard lock(mutex);
                    if (armed && (config.command != "trigger" || result.frameTimestampUs >= pressTimestampUs)) {
                        if (config.command == "trigger") {
                            armed = false;
                            discardPendingLocked();
                            auto triggerResult = result;
                            triggerResult.triggerTimestampUs = pressTimestampUs;
                            emitted = std::move(triggerResult);
                        } else {
                            if (dedupe.shouldEmit(result.format, result.text, result.frameTimestampUs,
                                                  static_cast<std::uint64_t>(config.dedupeMs) * 1000))
                                emitted = result;
                        }
                    }
                }
                if (emitted) {
                    sink.onResult(*emitted);
                    if (sessionEvents && config.command == "trigger")
                        sessionEvents->fetch_add(1);
                }
            }

            try {
                camera.requeue(work.index);
            } catch (const std::exception& exception) {
                fail(exception.what());
            }
            {
                std::lock_guard lock(mutex);
                auto& slot = slots[workerIndex];
                if (pending && armed && !stop.load()) {
                    slot.work = *pending;
                    pending.reset();
                    changed.notify_all();
                } else {
                    if (pending)
                        discardPendingLocked();
                    slot.busy = false;
                }
            }
        }
    }

    void printStats()
    {
        const auto captureCount = captured.exchange(0);
        const auto processedCount = processed.exchange(0);
        const auto droppedCount = dropped.exchange(0);
        const double total = decodeTotalMs.exchange(0);
        const double maximum = decodeMaxMs.exchange(0);
        const double mean = processedCount == 0 ? 0 : total / static_cast<double>(processedCount);
        std::cerr << std::fixed << std::setprecision(1) << "stats capture_fps=" << captureCount
                  << " decoded_fps=" << processedCount << " dropped=" << droppedCount << " decode_ms_mean=" << mean
                  << " decode_ms_max=" << maximum << '\n';
    }

    int run()
    {
        try {
            decoders.reserve(slots.size());
            for (std::size_t i = 0; i < slots.size(); ++i)
                decoders.push_back(std::make_unique<Decoder>(config));
        } catch (const std::exception& exception) {
            std::cerr << "error: " << exception.what() << '\n';
            return 1;
        }
        workers.reserve(slots.size());
        for (std::size_t i = 0; i < slots.size(); ++i)
            workers.emplace_back([this, i] { workerLoop(i); });
        captureThread = std::thread([this] { captureLoop(); });

        auto nextStats = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!shutdownRequested && !failed.load()) {
            // File sources terminate: once drained, wait for outstanding
            // work to finish, then stop.
            if (camera.exhausted()) {
                std::lock_guard lock(mutex);
                const bool idle = !pending && std::all_of(slots.begin(), slots.end(), [](const WorkerSlot& s) {
                    return !s.busy;
                });
                if (idle)
                    break;
            }
            if (config.command == "trigger" && trigger != nullptr) {
                TriggerEvent event;
                try {
                    if (trigger->wait(50, event)) {
                        {
                            std::lock_guard lock(mutex);
                            if (event.type == TriggerEventType::Pressed) {
                                armed = true;
                                armedFrames = 0;
                                pressTimestampUs = event.timestampUs;
                                sessionStartedUs = event.timestampUs;
                            } else if (config.triggerMode == "level") {
                                armed = false;
                                discardPendingLocked();
                            }
                        }
                        logLine(event.type == TriggerEventType::Pressed ? "trigger press" : "trigger release");
                    }
                } catch (const std::exception& exception) {
                    fail(exception.what());
                }
                std::uint64_t expiredTimestamp = 0;
                {
                    std::lock_guard lock(mutex);
                    const auto now = monotonicNowUs();
                    if (armed && now >= sessionStartedUs &&
                        now - sessionStartedUs >= static_cast<std::uint64_t>(config.scanTimeoutMs) * 1000) {
                        armed = false;
                        expiredTimestamp = now;
                        logLine("scan window armed_frames=" + std::to_string(armedFrames));
                        discardPendingLocked();
                    }
                }
                if (expiredTimestamp != 0) {
                    sink.onTimeout(expiredTimestamp);
                    if (sessionEvents)
                        sessionEvents->fetch_add(1);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }

            if (config.stats && std::chrono::steady_clock::now() >= nextStats) {
                printStats();
                nextStats = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
        }

        stop.store(true);
        {
            std::lock_guard lock(mutex);
            discardPendingLocked();
        }
        changed.notify_all();
        if (captureThread.joinable())
            captureThread.join();
        for (auto& worker : workers) {
            if (worker.joinable())
                worker.join();
        }
        logLine("stop");
        if (failed.load()) {
            std::cerr << "error: " << errorText << '\n';
            return 1;
        }
        return 0;
    }
};

Scanner::Scanner(const Config& config, FrameSource& source, ResultSink& sink, Trigger* trigger,
                 std::atomic<std::uint64_t>* sessionEvents)
    : impl_(std::make_unique<Impl>(config, source, sink, trigger, sessionEvents))
{
}

Scanner::~Scanner() = default;

int Scanner::run()
{
    return impl_->run();
}

const std::string& Scanner::error() const
{
    return impl_->errorText;
}
