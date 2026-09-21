#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdint>
#include <iostream>

#include "../include/uapi/qstream_ioctl.h"

int main()
{
    int fd = open("/dev/qstream0", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (ioctl(fd, QSTREAM_IOCTL_RESET) < 0) {
        perror("RESET");
        close(fd);
        return 1;
    }

    if (ioctl(fd, QSTREAM_IOCTL_START) < 0) {
        perror("START");
        close(fd);
        return 1;
    }

    std::cout << "Collecting records for 2 seconds\n";
    sleep(2);

    if (ioctl(fd, QSTREAM_IOCTL_STOP) < 0) {
        perror("STOP");
        close(fd);
        return 1;
    }

    qstream_stats stats {};

    if (ioctl(fd, QSTREAM_IOCTL_GET_STATS, &stats) < 0) {
        perror("GET_STATS");
        close(fd);
        return 1;
    }

    close(fd);

    std::cout
        << "generation=" << stats.generation << '\n'
        << "generated=" << stats.generated << '\n'
        << "hardware_drops=" << stats.hardware_drops << '\n'
        << "interrupts=" << stats.interrupts << '\n'
        << "records_drained=" << stats.records_drained << '\n'
        << "checksum_errors=" << stats.checksum_errors << '\n'
        << "stale_records=" << stats.stale_records << '\n'
        << "software_drops=" << stats.software_drops << '\n'
        << "records_stored=" << stats.records_stored << '\n'
        << "records_consumed=" << stats.records_consumed << '\n'
        << "records_pending=" << stats.records_pending << '\n';

    bool drain_accounting =
        stats.records_drained ==
        stats.records_stored +
        stats.software_drops +
        stats.checksum_errors +
        stats.stale_records;

    bool ring_accounting =
        stats.records_stored ==
        stats.records_consumed + stats.records_pending;

    bool hardware_accounting =
        stats.generated == stats.records_drained;

    bool passed =
        stats.generated > 0 &&
        stats.interrupts > 0 &&
        stats.hardware_drops == 0 &&
        drain_accounting &&
        ring_accounting &&
        hardware_accounting;

    if (!passed) {
        std::cerr << "STATISTICS TEST FAILED\n";
        return 1;
    }

    std::cout << "STATISTICS TEST PASSED\n";
    return 0;
}