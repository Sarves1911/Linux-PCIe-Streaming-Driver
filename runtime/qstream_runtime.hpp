#ifndef QSTREAM_RUNTIME_HPP
#define QSTREAM_RUNTIME_HPP

#include <cstdint>

#include "../include/uapi/qstream_ring.h"
#include "../include/uapi/qstream_stats.h"

class QStreamRuntime {
public:
    explicit QStreamRuntime(
        const char *device_path = "/dev/qstream0");

    ~QStreamRuntime();

    QStreamRuntime(const QStreamRuntime &) = delete;
    QStreamRuntime &operator=(const QStreamRuntime &) = delete;

    void reset();
    void start();
    void stop();

    bool wait_for_data(int timeout_ms);

    std::uint64_t available() const;

    const qstream_record &record(
        std::uint64_t offset) const;

    void consume(std::uint32_t count);

    qstream_stats statistics() const;

private:
    int fd_ = -1;
    void *mapping_ = nullptr;
    const qstream_shared_ring *ring_ = nullptr;
    bool streaming_ = false;
};

#endif