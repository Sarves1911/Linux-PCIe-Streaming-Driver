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

int main()
{
    int fd = open("/dev/qstream0", O_RDWR);

    if (fd < 0) {
        std::cerr << "Failed to open /dev/qstream0: "
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

    if (ring->header.magic != QSTREAM_RING_MAGIC ||
        ring->header.version != QSTREAM_RING_VERSION ||
        ring->header.capacity != QSTREAM_RING_CAPACITY ||
        ring->header.record_size != sizeof(struct qstream_record)) {
        std::cerr << "Invalid qstream ring layout\n";
        munmap(mapping, QSTREAM_RING_MMAP_SIZE);
        close(fd);
        return 1;
    }

    if (ioctl(fd, QSTREAM_IOCTL_START) < 0) {
        std::cerr << "START failed: "
                  << std::strerror(errno) << '\n';
        munmap(mapping, QSTREAM_RING_MMAP_SIZE);
        close(fd);
        return 1;
    }

    std::cout << "Streaming started; waiting for a record\n";

    struct pollfd event = {};
    event.fd = fd;
    event.events = POLLIN;

    int poll_result = poll(&event, 1, 5000);
    int exit_code = 0;

    if (poll_result < 0) {
        std::cerr << "poll failed: "
                  << std::strerror(errno) << '\n';
        exit_code = 1;
    } else if (poll_result == 0) {
        std::cerr << "Timed out waiting for a record\n";
        exit_code = 1;
    } else if (event.revents & POLLIN) {
        std::uint64_t head =
            __atomic_load_n(&ring->header.head,
                            __ATOMIC_ACQUIRE);

        std::uint64_t tail =
            __atomic_load_n(&ring->header.tail,
                            __ATOMIC_ACQUIRE);

        if (head == tail) {
            std::cerr << "Driver woke us without a record\n";
            exit_code = 1;
        } else {
            const struct qstream_record &record =
                ring->records[tail & QSTREAM_RING_MASK];

            std::uint64_t sequence =
                combine_words(
                    record.words[QSTREAM_WORD_SEQUENCE_LO],
                    record.words[QSTREAM_WORD_SEQUENCE_HI]);

            std::uint64_t timestamp =
                combine_words(
                    record.words[QSTREAM_WORD_TIMESTAMP_LO],
                    record.words[QSTREAM_WORD_TIMESTAMP_HI]);

            std::uint32_t expected_checksum =
                calculate_checksum(record);

            bool checksum_ok =
                expected_checksum ==
                record.words[QSTREAM_WORD_CHECKSUM];

            std::cout
                << "Record sequence=" << sequence
                << " timestamp=" << timestamp
                << " value=0x" << std::hex
                << record.words[QSTREAM_WORD_VALUE]
                << std::dec
                << " generation="
                << record.words[QSTREAM_WORD_GENERATION]
                << " checksum="
                << (checksum_ok ? "OK" : "BAD")
                << '\n';

            __u32 consumed = 1;

            if (ioctl(fd,
                      QSTREAM_IOCTL_CONSUME,
                      &consumed) < 0) {
                std::cerr << "CONSUME failed: "
                          << std::strerror(errno) << '\n';
                exit_code = 1;
            } else {
                std::cout << "Consumed one record\n";
            }
        }
    } else {
        std::cerr << "Unexpected poll event: "
                  << event.revents << '\n';
        exit_code = 1;
    }

    if (ioctl(fd, QSTREAM_IOCTL_STOP) < 0) {
        std::cerr << "STOP failed: "
                  << std::strerror(errno) << '\n';
        exit_code = 1;
    } else {
        std::cout << "Streaming stopped\n";
    }

    munmap(mapping, QSTREAM_RING_MMAP_SIZE);
    close(fd);

    return exit_code;
}