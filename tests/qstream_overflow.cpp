#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
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
    constexpr unsigned int wait_seconds = 40;

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

    if (ring->header.magic != QSTREAM_RING_MAGIC ||
        ring->header.version != QSTREAM_RING_VERSION ||
        ring->header.capacity != QSTREAM_RING_CAPACITY ||
        ring->header.record_size != sizeof(struct qstream_record)) {
        std::cerr << "Invalid ring layout\n";
        munmap(mapping, QSTREAM_RING_MMAP_SIZE);
        close(fd);
        return 1;
    }
    if (ioctl(fd, QSTREAM_IOCTL_RESET) < 0) {
        perror("RESET");
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

    std::cout
        << "Streaming without consuming for "
        << wait_seconds
        << " seconds\n";

    sleep(wait_seconds);

    if (ioctl(fd, QSTREAM_IOCTL_STOP) < 0) {
        std::cerr << "STOP failed: "
                  << std::strerror(errno) << '\n';
        munmap(mapping, QSTREAM_RING_MMAP_SIZE);
        close(fd);
        return 1;
    }

    std::uint64_t tail =
        __atomic_load_n(&ring->header.tail,
                        __ATOMIC_ACQUIRE);

    std::uint64_t head =
        __atomic_load_n(&ring->header.head,
                        __ATOMIC_ACQUIRE);

    std::uint64_t dropped =
        __atomic_load_n(&ring->header.dropped,
                        __ATOMIC_ACQUIRE);

    std::uint64_t available = head - tail;

    std::cout
        << "head=" << head
        << " tail=" << tail
        << " available=" << available
        << " dropped=" << dropped
        << '\n';

    bool passed = true;

    if (available != QSTREAM_RING_CAPACITY) {
        std::cerr
            << "Expected a full "
            << QSTREAM_RING_CAPACITY
            << "-record ring\n";
        passed = false;
    }

    if (dropped == 0) {
        std::cerr
            << "Expected records to be dropped after filling\n";
        passed = false;
    }

    if (available > QSTREAM_RING_CAPACITY) {
        std::cerr << "Ring exceeded its fixed capacity\n";
        passed = false;
        available = QSTREAM_RING_CAPACITY;
    }

    std::uint64_t checksum_errors = 0;
    std::uint64_t sequence_gaps = 0;
    std::uint64_t first_sequence = 0;
    std::uint64_t previous_sequence = 0;

    for (std::uint64_t i = 0; i < available; i++) {
        const struct qstream_record &record =
            ring->records[(tail + i) & QSTREAM_RING_MASK];

        std::uint64_t sequence =
            combine_words(
                record.words[QSTREAM_WORD_SEQUENCE_LO],
                record.words[QSTREAM_WORD_SEQUENCE_HI]);

        if (i == 0) {
            first_sequence = sequence;
        } else if (sequence != previous_sequence + 1) {
            sequence_gaps++;
        }

        previous_sequence = sequence;

        if (calculate_checksum(record) !=
            record.words[QSTREAM_WORD_CHECKSUM]) {
            checksum_errors++;
        }
    }

    std::cout
        << "first_sequence=" << first_sequence
        << " last_sequence=" << previous_sequence
        << " checksum_errors=" << checksum_errors
        << " sequence_gaps=" << sequence_gaps
        << '\n';

    if (checksum_errors != 0 || sequence_gaps != 0)
        passed = false;

    __u32 consumed = static_cast<__u32>(available);

    if (ioctl(fd,
              QSTREAM_IOCTL_CONSUME,
              &consumed) < 0) {
        std::cerr << "CONSUME failed: "
                  << std::strerror(errno) << '\n';
        passed = false;
    }

    std::uint64_t final_tail =
        __atomic_load_n(&ring->header.tail,
                        __ATOMIC_ACQUIRE);

    if (final_tail != head) {
        std::cerr << "Ring did not become empty\n";
        passed = false;
    }

    std::cout
        << (passed ? "OVERFLOW TEST PASSED"
                   : "OVERFLOW TEST FAILED")
        << '\n';

    munmap(mapping, QSTREAM_RING_MMAP_SIZE);
    close(fd);

    return passed ? 0 : 1;
}