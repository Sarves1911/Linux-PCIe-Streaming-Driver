#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../include/uapi/qstream_ioctl.h"

int main()
{
    int fd = open("/dev/qstream0", O_RDWR);

    if (fd < 0) {
        std::cerr << "Failed to open /dev/qstream0: "
                  << std::strerror(errno) << '\n';
        return 1;
    }

    if (ioctl(fd, QSTREAM_IOCTL_START) < 0) {
        std::cerr << "START failed: "
                  << std::strerror(errno) << '\n';
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
        std::cout << "Driver says a record is available\n";
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

    close(fd);
    return exit_code;
}