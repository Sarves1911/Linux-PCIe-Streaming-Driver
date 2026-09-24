#include "qstream_runtime.hpp"

#include <cerrno>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include "../include/uapi/qstream_ioctl.h"

QStreamRuntime::QStreamRuntime(const char *device_path)
{
    fd_ = open(device_path, O_RDWR | O_CLOEXEC);

    if (fd_ < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to open qstream device");
    }

    mapping_ = mmap(nullptr,
                    QSTREAM_RING_MMAP_SIZE,
                    PROT_READ,
                    MAP_SHARED,
                    fd_,
                    0);

    if (mapping_ == MAP_FAILED) {
        int saved_error = errno;

        mapping_ = nullptr;
        close(fd_);
        fd_ = -1;

        throw std::system_error(
            saved_error,
            std::generic_category(),
            "Failed to map qstream ring");
    }

    ring_ = static_cast<const qstream_shared_ring *>(mapping_);

    if (ring_->header.magic != QSTREAM_RING_MAGIC ||
        ring_->header.version != QSTREAM_RING_VERSION ||
        ring_->header.capacity != QSTREAM_RING_CAPACITY ||
        ring_->header.record_size != sizeof(qstream_record)) {
        munmap(mapping_, QSTREAM_RING_MMAP_SIZE);
        close(fd_);

        ring_ = nullptr;
        mapping_ = nullptr;
        fd_ = -1;

        throw std::runtime_error(
            "Driver returned an incompatible qstream ring");
    }

}

QStreamRuntime::~QStreamRuntime()
{
    if (fd_ >= 0 && streaming_)
        ioctl(fd_, QSTREAM_IOCTL_STOP);

    if (mapping_)
        munmap(mapping_, QSTREAM_RING_MMAP_SIZE);

    if (fd_ >= 0)
        close(fd_);
}

void QStreamRuntime::reset()
{
    if (ioctl(fd_, QSTREAM_IOCTL_RESET) < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Qstream RESET failed");
    }

    streaming_ = false;
}

void QStreamRuntime::start()
{
    if (streaming_)
        return;

    if (ioctl(fd_, QSTREAM_IOCTL_START) < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Qstream START failed");
    }

    streaming_ = true;
}

void QStreamRuntime::stop()
{
    if (!streaming_)
        return;

    if (ioctl(fd_, QSTREAM_IOCTL_STOP) < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Qstream STOP failed");
    }

    streaming_ = false;
}

bool QStreamRuntime::wait_for_data(int timeout_ms)
{
    struct pollfd event = {};

    event.fd = fd_;
    event.events = POLLIN;

    for (;;) {
        int result = poll(&event, 1, timeout_ms);

        if (result < 0 && errno == EINTR)
            continue;

        if (result < 0) {
            throw std::system_error(
                errno,
                std::generic_category(),
                "Qstream poll failed");
        }

        if (result == 0)
            return false;

        if (event.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            throw std::runtime_error(
                "Qstream returned an invalid poll event");
        }

        return (event.revents & POLLIN) != 0;
    }
}

std::uint64_t QStreamRuntime::available() const
{
    std::uint64_t tail =
        __atomic_load_n(&ring_->header.tail, __ATOMIC_ACQUIRE);

    std::uint64_t head =
        __atomic_load_n(&ring_->header.head, __ATOMIC_ACQUIRE);

    if (head < tail)
        throw std::runtime_error("Invalid qstream ring counters");

    std::uint64_t count = head - tail;

    if (count > QSTREAM_RING_CAPACITY) {
        throw std::runtime_error(
            "Qstream ring contains an impossible record count");
    }

    return count;
}
const qstream_record &QStreamRuntime::record(
    std::uint64_t offset) const
{
    std::uint64_t count = available();

    if (offset >= count)
        throw std::out_of_range("Qstream record offset is unavailable");

    std::uint64_t tail =
        __atomic_load_n(&ring_->header.tail, __ATOMIC_ACQUIRE);

    return ring_->records[
        (tail + offset) & QSTREAM_RING_MASK];
}

void QStreamRuntime::consume(std::uint32_t count)
{
    if (count == 0)
        return;

    if (count > available()) {
        throw std::out_of_range(
            "Cannot consume more qstream records than are available");
    }

    __u32 requested = count;

    if (ioctl(fd_, QSTREAM_IOCTL_CONSUME, &requested) < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Qstream CONSUME failed");
    }
}

qstream_stats QStreamRuntime::statistics() const
{
    qstream_stats stats {};

    if (ioctl(fd_, QSTREAM_IOCTL_GET_STATS, &stats) < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Qstream GET_STATS failed");
    }

    return stats;
}