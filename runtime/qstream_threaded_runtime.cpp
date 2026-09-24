#include "qstream_threaded_runtime.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

QStreamThreadedRuntime::QStreamThreadedRuntime(
    std::size_t queue_capacity)
    : queue_capacity_(queue_capacity)
{
    if (queue_capacity_ == 0) {
        throw std::invalid_argument(
            "Threaded runtime queue capacity cannot be zero");
    }
}

QStreamThreadedRuntime::~QStreamThreadedRuntime()
{
    try {
        stop();
    } catch (...) {
        /*
         * Destructors must not throw. Explicit stop() calls still report
         * errors to the application.
         */
    }
}

void QStreamThreadedRuntime::reset()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (running_) {
            throw std::logic_error(
                "Cannot reset while threaded streaming is active");
        }

        queue_.clear();
        drain_error_ = nullptr;
        stop_requested_ = false;
    }

    stream_.reset();
}

void QStreamThreadedRuntime::start()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (running_)
            return;

        queue_.clear();
        drain_error_ = nullptr;
        stop_requested_ = false;
        running_ = true;
    }

    try {
        stream_.start();

        drain_thread_ = std::thread(
            &QStreamThreadedRuntime::drain_loop,
            this);
    } catch (...) {
        try {
            stream_.stop();
        } catch (...) {
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            stop_requested_ = true;
        }

        data_ready_.notify_all();
        space_ready_.notify_all();
        throw;
    }
}

void QStreamThreadedRuntime::stop()
{
    std::exception_ptr stop_error;
    std::exception_ptr background_error;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_)
            return;
    }

    try {
        stream_.stop();
    } catch (...) {
        stop_error = std::current_exception();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
    }

    data_ready_.notify_all();
    space_ready_.notify_all();

    if (drain_thread_.joinable())
        drain_thread_.join();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        background_error = drain_error_;
    }

    if (stop_error)
        std::rethrow_exception(stop_error);

    if (background_error)
        std::rethrow_exception(background_error);
}

void QStreamThreadedRuntime::drain_loop()
{
    try {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex_);

                if (stop_requested_)
                    break;
            }

            if (!stream_.wait_for_data(100))
                continue;

            std::uint64_t available = stream_.available();
            std::uint32_t consumed = 0;

            for (std::uint64_t i = 0;
                 i < available;
                 i++) {
                qstream_record copy = stream_.record(i);

                std::unique_lock<std::mutex> lock(mutex_);

                space_ready_.wait(
                    lock,
                    [this] {
                        return stop_requested_ ||
                               queue_.size() < queue_capacity_;
                    });

                if (stop_requested_)
                    break;

                queue_.push_back(copy);
                consumed++;

                lock.unlock();
                data_ready_.notify_one();
            }

            if (consumed != 0)
                stream_.consume(consumed);

            {
                std::lock_guard<std::mutex> lock(mutex_);

                if (stop_requested_)
                    break;
            }
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            drain_error_ = std::current_exception();
            stop_requested_ = true;
        }

        data_ready_.notify_all();
        space_ready_.notify_all();
    }
}

bool QStreamThreadedRuntime::wait_and_pop(
    qstream_record &record,
    int timeout_ms)
{
    std::unique_lock<std::mutex> lock(mutex_);

    auto ready = [this] {
        return !queue_.empty() ||
               drain_error_ ||
               stop_requested_;
    };

    bool awakened;

    if (timeout_ms < 0) {
        data_ready_.wait(lock, ready);
        awakened = true;
    } else {
        awakened = data_ready_.wait_for(
            lock,
            std::chrono::milliseconds(timeout_ms),
            ready);
    }

    if (!awakened)
        return false;

    if (!queue_.empty()) {
        record = queue_.front();
        queue_.pop_front();

        lock.unlock();
        space_ready_.notify_one();

        return true;
    }

    if (drain_error_) {
        std::exception_ptr error = drain_error_;
        lock.unlock();
        std::rethrow_exception(error);
    }

    return false;
}

qstream_stats QStreamThreadedRuntime::statistics() const
{
    return stream_.statistics();
}

std::size_t QStreamThreadedRuntime::queued_records() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}