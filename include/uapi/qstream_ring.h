#ifndef QSTREAM_RING_H
#define QSTREAM_RING_H

#include <linux/types.h>

#define QSTREAM_RING_MAGIC        0x51535247u
#define QSTREAM_RING_VERSION      0x00010001u

#define QSTREAM_RING_CAPACITY     1024u
#define QSTREAM_RING_MASK         (QSTREAM_RING_CAPACITY - 1u)
#define QSTREAM_RING_RECORD_WORDS 8u
#define QSTREAM_RING_HEADER_SIZE  4096u

struct qstream_record {
    __u32 words[QSTREAM_RING_RECORD_WORDS];
};

struct qstream_ring_header {
    __u32 magic;
    __u32 version;
    __u32 capacity;
    __u32 record_size;

    __u64 head;
    __u64 tail;
    __u64 dropped;
    __u32 generation;
    __u32 reserved_word;


    __u8 reserved[QSTREAM_RING_HEADER_SIZE - 48u];
};

struct qstream_shared_ring {
    struct qstream_ring_header header;
    struct qstream_record records[QSTREAM_RING_CAPACITY];
};

#define QSTREAM_RING_MMAP_SIZE \
    (QSTREAM_RING_HEADER_SIZE + \
     (QSTREAM_RING_CAPACITY * sizeof(struct qstream_record)))

#endif