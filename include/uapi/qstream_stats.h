#ifndef QSTREAM_STATS_H
#define QSTREAM_STATS_H

#include <linux/types.h>

struct qstream_stats {
    __u64 generated;
    __u64 hardware_drops;

    __u64 interrupts;
    __u64 records_drained;
    __u64 checksum_errors;
    __u64 stale_records;

    __u64 software_drops;
    __u64 records_stored;
    __u64 records_consumed;
    __u64 records_pending;

    __u32 generation;
    __u32 reserved;
};

#endif