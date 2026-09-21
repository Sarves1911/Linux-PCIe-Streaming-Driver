#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>

#include "qstream_runtime.hpp"
#include "../protocol/qstream_wire.h"

static std::uint64_t combine_words(std::uint32_t low,
                                   std::uint32_t high)
{
    return static_cast<std::uint64_t>(low) |
           (static_cast<std::uint64_t>(high) << 32);
}

static std::uint32_t calculate_checksum(
    const qstream_record &record)
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

    try {
        QStreamRuntime stream;

        stream.reset();
        stream.start();

        std::cout << "Streaming started\n";

        std::uint64_t processed = 0;
        std::uint64_t checksum_errors = 0;
        std::uint64_t sequence_gaps = 0;
        std::uint64_t previous_sequence = 0;
        bool have_previous_sequence = false;

        while (processed < target_records) {
            if (!stream.wait_for_data(5000))
                throw std::runtime_error(
                    "Timed out waiting for records");

            std::uint64_t count = stream.available();

            for (std::uint64_t i = 0; i < count; i++) {
                const qstream_record &record =
                    stream.record(i);

                std::uint64_t sequence = combine_words(
                    record.words[QSTREAM_WORD_SEQUENCE_LO],
                    record.words[QSTREAM_WORD_SEQUENCE_HI]);

                std::uint64_t timestamp = combine_words(
                    record.words[QSTREAM_WORD_TIMESTAMP_LO],
                    record.words[QSTREAM_WORD_TIMESTAMP_HI]);

                bool checksum_ok =
                    calculate_checksum(record) ==
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

            stream.consume(
                static_cast<std::uint32_t>(count));

            processed += count;
        }

        stream.stop();

        qstream_stats stats = stream.statistics();

        std::cout
            << "Streaming stopped\n"
            << "Summary: processed=" << processed
            << " checksum_errors=" << checksum_errors
            << " sequence_gaps=" << sequence_gaps
            << " hardware_drops=" << stats.hardware_drops
            << " software_drops=" << stats.software_drops
            << " interrupts=" << stats.interrupts
            << " pending=" << stats.records_pending
            << '\n';

        return checksum_errors == 0 &&
                       sequence_gaps == 0
                   ? 0
                   : 1;
    } catch (const std::exception &error) {
        std::cerr << "qstream error: "
                  << error.what() << '\n';
        return 1;
    }
}