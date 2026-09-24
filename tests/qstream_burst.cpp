#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>

#include "../protocol/qstream_wire.h"
#include "../runtime/qstream_runtime.hpp"

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
    constexpr std::uint64_t target_records = 1024;

    try {
        QStreamRuntime runtime;

        runtime.reset();

        const std::uint32_t generation =
            runtime.statistics().generation;

        std::uint64_t processed = 0;
        std::uint64_t expected_sequence = 0;
        std::uint64_t checksum_errors = 0;
        std::uint64_t generation_errors = 0;
        std::uint64_t sequence_errors = 0;

        auto start_time = std::chrono::steady_clock::now();

        runtime.start();

        while (processed < target_records) {
            if (!runtime.wait_for_data(5000)) {
                throw std::runtime_error(
                    "Timed out during burst test");
            }

            std::uint64_t available = runtime.available();

            for (std::uint64_t i = 0;
                 i < available;
                 i++) {
                const qstream_record &record =
                    runtime.record(i);

                std::uint64_t sequence = combine_words(
                    record.words[QSTREAM_WORD_SEQUENCE_LO],
                    record.words[QSTREAM_WORD_SEQUENCE_HI]);

                if (sequence != expected_sequence)
                    sequence_errors++;

                expected_sequence = sequence + 1;

                if (calculate_checksum(record) !=
                    record.words[QSTREAM_WORD_CHECKSUM]) {
                    checksum_errors++;
                }

                if (record.words[QSTREAM_WORD_GENERATION] !=
                    generation) {
                    generation_errors++;
                }
            }

            runtime.consume(
                static_cast<std::uint32_t>(available));

            processed += available;
        }

        runtime.stop();

        auto end_time = std::chrono::steady_clock::now();

        double seconds =
            std::chrono::duration<double>(
                end_time - start_time).count();

        qstream_stats stats = runtime.statistics();

        double records_per_second =
            processed / seconds;

        double records_per_interrupt =
            stats.interrupts == 0
                ? 0.0
                : static_cast<double>(
                      stats.records_drained) /
                  static_cast<double>(
                      stats.interrupts);

        bool passed =
            processed >= target_records &&
            checksum_errors == 0 &&
            generation_errors == 0 &&
            sequence_errors == 0 &&
            stats.hardware_drops == 0 &&
            stats.software_drops == 0 &&
            stats.records_pending == 0 &&
            stats.records_drained == processed &&
            stats.interrupts > 0 &&
            records_per_interrupt >= 4.0;

        std::cout
            << "processed=" << processed
            << " interrupts=" << stats.interrupts
            << " records_per_interrupt="
            << std::fixed << std::setprecision(2)
            << records_per_interrupt
            << " records_per_second="
            << records_per_second
            << '\n'
            << "checksum_errors=" << checksum_errors
            << " generation_errors=" << generation_errors
            << " sequence_errors=" << sequence_errors
            << " hardware_drops=" << stats.hardware_drops
            << " software_drops=" << stats.software_drops
            << " pending=" << stats.records_pending
            << '\n'
            << (passed
                    ? "INTERRUPT BURST TEST PASSED"
                    : "INTERRUPT BURST TEST FAILED")
            << '\n';

        return passed ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr
            << "Burst test error: "
            << error.what()
            << '\n';

        return 1;
    }
}