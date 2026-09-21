#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../include/uapi/qstream_ioctl.h"
#include "../include/uapi/qstream_ring.h"
#include "../protocol/qstream_wire.h"

static std::uint64_t combine_words(std::uint32_t low,
                                   std::uint32_t high)
{
    return static_cast<std::uint64_t>(low) |
           (static_cast<std::uint64_t>(high) << 32);
}

static std::uint32_t calculate_checksum(
    const struct qstream_record &record)
{
    std::uint32_t checksum = 0;

    for (unsigned int i = 0;
         i < QSTREAM_WORD_CHECKSUM;
         i++) {
        checksum ^= record.words[i];
    }

    return checksum;
}

static bool wait_for_data(int fd)
{
    struct pollfd event = {};

    event.fd = fd;
    event.events = POLLIN;

    int result = poll(&event, 1, 5000);

    if (result < 0) {
        std::cerr << "poll failed: "
                  << std::strerror(errno) << '\n';
        return false;
    }

    if (result == 0) {
        std::cerr << "Timed out waiting for data\n";
        return false;
    }

    return (event.revents & POLLIN) != 0;
}

int main()
{
    int fd = open("/dev/qstream0", O_RDWR);

    if (fd < 0) {
        std::cerr << "open failed: "
                  << std::strerror(errno) << '\n';
        return 1;
    }

    void *mapping = mmap(nullptr,
                         QSTREAM_RING_MMAP_SIZE,
                         PROT_READ,
                         MAP_SHARED,
                         fd,
                         0);

    if (mapping == MAP_FAILED) {
        std::cerr << "mmap failed: "
                  << std::strerror(errno) << '\n';
        close(fd);
        return 1;
    }

    const auto *ring =
        static_cast<const struct qstream_shared_ring *>(mapping);

    bool passed = true;

    std::uint32_t old_generation =
        __atomic_load_n(&ring->header.generation,
                        __ATOMIC_ACQUIRE);

    if (ioctl(fd, QSTREAM_IOCTL_START) < 0) {
        std::cerr << "Initial START failed\n";
        passed = false;
    }

    if (passed && !wait_for_data(fd))
        passed = false;

    std::uint64_t before_head =
        __atomic_load_n(&ring->header.head,
                        __ATOMIC_ACQUIRE);

    std::uint64_t before_tail =
        __atomic_load_n(&ring->header.tail,
                        __ATOMIC_ACQUIRE);

    std::cout
        << "Before reset: generation=" << old_generation
        << " pending=" << (before_head - before_tail)
        << '\n';

    if (passed &&
        ioctl(fd, QSTREAM_IOCTL_RESET) < 0) {
        std::cerr << "RESET failed: "
                  << std::strerror(errno) << '\n';
        passed = false;
    }

    std::uint32_t new_generation =
        __atomic_load_n(&ring->header.generation,
                        __ATOMIC_ACQUIRE);

    std::uint64_t reset_head =
        __atomic_load_n(&ring->header.head,
                        __ATOMIC_ACQUIRE);

    std::uint64_t reset_tail =
        __atomic_load_n(&ring->header.tail,
                        __ATOMIC_ACQUIRE);

    std::uint64_t reset_dropped =
        __atomic_load_n(&ring->header.dropped,
                        __ATOMIC_ACQUIRE);

    std::cout
        << "After reset: generation=" << new_generation
        << " head=" << reset_head
        << " tail=" << reset_tail
        << " dropped=" << reset_dropped
        << '\n';

    if (new_generation !=
        static_cast<std::uint32_t>(old_generation + 1)) {
        std::cerr << "Generation did not increase\n";
        passed = false;
    }

    if (reset_head != 0 ||
        reset_tail != 0 ||
        reset_dropped != 0) {
        std::cerr << "Ring counters were not cleared\n";
        passed = false;
    }

    if (passed &&
        ioctl(fd, QSTREAM_IOCTL_START) < 0) {
        std::cerr << "Second START failed\n";
        passed = false;
    }

    if (passed && !wait_for_data(fd))
        passed = false;

    std::uint64_t head =
        __atomic_load_n(&ring->header.head,
                        __ATOMIC_ACQUIRE);

    std::uint64_t tail =
        __atomic_load_n(&ring->header.tail,
                        __ATOMIC_ACQUIRE);

    std::uint64_t available = head - tail;

    if (available == 0 ||
        available > QSTREAM_RING_CAPACITY) {
        std::cerr << "Invalid post-reset record count\n";
        passed = false;
    }

    std::uint64_t checksum_errors = 0;
    std::uint64_t generation_errors = 0;
    std::uint64_t sequence_errors = 0;

    for (std::uint64_t i = 0;
         passed && i < available;
         i++) {
        const struct qstream_record &record =
            ring->records[(tail + i) & QSTREAM_RING_MASK];

        std::uint64_t sequence =
            combine_words(
                record.words[QSTREAM_WORD_SEQUENCE_LO],
                record.words[QSTREAM_WORD_SEQUENCE_HI]);

        if (sequence != i)
            sequence_errors++;

        if (record.words[QSTREAM_WORD_GENERATION] !=
            new_generation) {
            generation_errors++;
        }

        if (calculate_checksum(record) !=
            record.words[QSTREAM_WORD_CHECKSUM]) {
            checksum_errors++;
        }
    }

    std::cout
        << "After restart: records=" << available
        << " checksum_errors=" << checksum_errors
        << " generation_errors=" << generation_errors
        << " sequence_errors=" << sequence_errors
        << '\n';

    if (checksum_errors != 0 ||
        generation_errors != 0 ||
        sequence_errors != 0) {
        passed = false;
    }

    if (available <= QSTREAM_RING_CAPACITY) {
        __u32 consumed =
            static_cast<__u32>(available);

        if (ioctl(fd,
                  QSTREAM_IOCTL_CONSUME,
                  &consumed) < 0) {
            std::cerr << "CONSUME failed\n";
            passed = false;
        }
    }

    if (ioctl(fd, QSTREAM_IOCTL_STOP) < 0) {
        std::cerr << "Final STOP failed\n";
        passed = false;
    }

    std::cout
        << (passed ? "RESET TEST PASSED"
                   : "RESET TEST FAILED")
        << '\n';

    munmap(mapping, QSTREAM_RING_MMAP_SIZE);
    close(fd);

    return passed ? 0 : 1;
}