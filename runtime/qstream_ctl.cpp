#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
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

    std::cout << "Streaming started\n";

    sleep(3);

    if (ioctl(fd, QSTREAM_IOCTL_STOP) < 0) {
        std::cerr << "STOP failed: "
                  << std::strerror(errno) << '\n';
        close(fd);
        return 1;
    }

    std::cout << "Streaming stopped\n";

    close(fd);
    return 0;
}