#ifndef QSTREAM_THREADED_RUNTIME_HPP
#define QSTREAM_THREADED_RUNTIME_HPP

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

#include "qstream_runtime.hpp"

class QStreamThreadedRuntime {
public:
    explicit QStreamThreadedRuntime(
        std::size_t queue_capacity = 4096);

    ~QStreamThreadedRuntime();

    QStreamThreadedRuntime(
        const QStreamThreadedRuntime &) = delete;

    QStreamThreadedRuntime &operator=(
        const QStreamThreadedRuntime &) = delete;

    void reset();
    void start();
    void stop();

    bool wait_and_pop(qstream_record &record,
                      int timeout_ms);

    qstream_stats statistics() const;

    std::size_t queued_records() const;

private:
    void drain_loop();

    QStreamRuntime stream_;
    const std::size_t queue_capacity_;

    mutable std::mutex mutex_;
    std::condition_variable data_ready_;
    std::condition_variable space_ready_;

    std::deque<qstream_record> queue_;
    std::thread drain_thread_;
    std::exception_ptr drain_error_;

    bool running_ = false;
    bool stop_requested_ = false;
};

#endif