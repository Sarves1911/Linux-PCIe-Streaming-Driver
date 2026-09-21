#ifndef QSTREAM_IOCTL_H
#define QSTREAM_IOCTL_H
#include "qstream_stats.h"

#include <linux/ioctl.h>
#include <linux/types.h>
#define QSTREAM_IOCTL_TYPE  'q'

#define QSTREAM_IOCTL_START _IO(QSTREAM_IOCTL_TYPE, 0x01)
#define QSTREAM_IOCTL_STOP  _IO(QSTREAM_IOCTL_TYPE, 0x02)
#define QSTREAM_IOCTL_CONSUME \
    _IOW(QSTREAM_IOCTL_TYPE, 0x03, __u32)

#define QSTREAM_IOCTL_RESET \
    _IO(QSTREAM_IOCTL_TYPE, 0x04)

#define QSTREAM_IOCTL_GET_STATS \
    _IOR(QSTREAM_IOCTL_TYPE, 0x05, struct qstream_stats)
    
#endif