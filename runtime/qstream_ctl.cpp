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
    constexpr std::uint64_t target_records = 20;

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

    std::cout << "Streaming started\n";

    std::uint64_t processed = 0;
    std::uint64_t checksum_errors = 0;
    std::uint64_t sequence_gaps = 0;
    std::uint64_t previous_sequence = 0;
    bool have_previous_sequence = false;
    int exit_code = 0;

    while (processed < target_records && exit_code == 0) {
        struct pollfd event = {};
        event.fd = fd;
        event.events = POLLIN;

        int poll_result = poll(&event, 1, 5000);

        if (poll_result < 0) {
            std::cerr << "poll failed: "
                      << std::strerror(errno) << '\n';
            exit_code = 1;
            break;
        }

        if (poll_result == 0) {
            std::cerr << "Timed out waiting for records\n";
            exit_code = 1;
            break;
        }

        if (!(event.revents & POLLIN)) {
            std::cerr << "Unexpected poll event: "
                      << event.revents << '\n';
            exit_code = 1;
            break;
        }

        std::uint64_t tail =
            __atomic_load_n(&ring->header.tail,
                            __ATOMIC_ACQUIRE);

        std::uint64_t head =
            __atomic_load_n(&ring->header.head,
                            __ATOMIC_ACQUIRE);

        if (head < tail) {
            std::cerr << "Invalid ring counters\n";
            exit_code = 1;
            break;
        }

        std::uint64_t available = head - tail;

        if (available == 0)
            continue;

        if (available > QSTREAM_RING_CAPACITY) {
            std::cerr << "Ring contains impossible count: "
                      << available << '\n';
            exit_code = 1;
            break;
        }

        for (std::uint64_t i = 0; i < available; i++) {
            const struct qstream_record &record =
                ring->records[(tail + i) &
                              QSTREAM_RING_MASK];

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

            if (!checksum_ok)
                checksum_errors++;

            if (have_previous_sequence &&
                sequence != previous_sequence + 1) {
                sequence_gaps++;
            }

            previous_sequence = sequence;
            have_previous_sequence = true;

            std::cout
                << "sequence=" << sequence
                << " timestamp=" << timestamp
                << " value=0x" << std::hex
                << record.words[QSTREAM_WORD_VALUE]
                << std::dec
                << " checksum="
                << (checksum_ok ? "OK" : "BAD")
                << '\n';
        }

        __u32 consumed =
            static_cast<__u32>(available);

        if (ioctl(fd,
                  QSTREAM_IOCTL_CONSUME,
                  &consumed) < 0) {
            std::cerr << "CONSUME failed: "
                      << std::strerror(errno) << '\n';
            exit_code = 1;
            break;
        }

        processed += available;
    }

    if (ioctl(fd, QSTREAM_IOCTL_STOP) < 0) {
        std::cerr << "STOP failed: "
                  << std::strerror(errno) << '\n';
        exit_code = 1;
    }

    std::uint64_t dropped =
        __atomic_load_n(&ring->header.dropped,
                        __ATOMIC_ACQUIRE);

    std::cout
        << "Streaming stopped\n"
        << "Summary: processed=" << processed
        << " checksum_errors=" << checksum_errors
        << " sequence_gaps=" << sequence_gaps
        << " dropped=" << dropped
        << '\n';

    munmap(mapping, QSTREAM_RING_MMAP_SIZE);
    close(fd);

    return exit_code;
}