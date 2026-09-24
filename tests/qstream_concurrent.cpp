#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <thread>
#include <vector>

#include "../protocol/qstream_wire.h"
#include "../runtime/qstream_threaded_runtime.hpp"

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
    constexpr std::size_t worker_count = 4;
    constexpr std::size_t records_per_worker = 64;
    constexpr std::size_t expected_records =
        worker_count * records_per_worker;

    try {
        QStreamThreadedRuntime runtime(128);

        runtime.reset();

        const std::uint32_t generation =
            runtime.statistics().generation;

        runtime.start();

        std::array<std::vector<std::uint64_t>,
                   worker_count> sequences;

        std::array<std::uint64_t,
                   worker_count> checksum_errors {};

        std::array<std::uint64_t,
                   worker_count> generation_errors {};

        std::array<std::exception_ptr,
                   worker_count> worker_errors {};

        std::array<std::thread,
                   worker_count> workers;

        for (std::size_t worker = 0;
             worker < worker_count;
             worker++) {
            workers[worker] = std::thread(
                [&, worker] {
                    try {
                        sequences[worker].reserve(
                            records_per_worker);

                        for (std::size_t i = 0;
                             i < records_per_worker;
                             i++) {
                            qstream_record record {};

                            if (!runtime.wait_and_pop(
                                    record, 5000)) {
                                throw std::runtime_error(
                                    "Timed out waiting for a record");
                            }

                            std::uint64_t sequence =
                                combine_words(
                                    record.words[
                                        QSTREAM_WORD_SEQUENCE_LO],
                                    record.words[
                                        QSTREAM_WORD_SEQUENCE_HI]);

                            sequences[worker].push_back(sequence);

                            if (calculate_checksum(record) !=
                                record.words[
                                    QSTREAM_WORD_CHECKSUM]) {
                                checksum_errors[worker]++;
                            }

                            if (record.words[
                                    QSTREAM_WORD_GENERATION] !=
                                generation) {
                                generation_errors[worker]++;
                            }
                        }
                    } catch (...) {
                        worker_errors[worker] =
                            std::current_exception();
                    }
                });
        }

        for (std::thread &worker : workers)
            worker.join();

        runtime.stop();

        for (const std::exception_ptr &error :
             worker_errors) {
            if (error)
                std::rethrow_exception(error);
        }

        std::vector<std::uint64_t> all_sequences;
        all_sequences.reserve(expected_records);

        std::uint64_t total_checksum_errors = 0;
        std::uint64_t total_generation_errors = 0;

        for (std::size_t worker = 0;
             worker < worker_count;
             worker++) {
            std::cout
                << "consumer[" << worker << "] processed="
                << sequences[worker].size()
                << '\n';

            all_sequences.insert(
                all_sequences.end(),
                sequences[worker].begin(),
                sequences[worker].end());

            total_checksum_errors +=
                checksum_errors[worker];

            total_generation_errors +=
                generation_errors[worker];
        }

        std::sort(all_sequences.begin(),
                  all_sequences.end());

        std::uint64_t sequence_errors = 0;

        for (std::size_t i = 0;
             i < all_sequences.size();
             i++) {
            if (all_sequences[i] != i)
                sequence_errors++;
        }

        qstream_stats stats = runtime.statistics();

        bool passed =
            all_sequences.size() == expected_records &&
            total_checksum_errors == 0 &&
            total_generation_errors == 0 &&
            sequence_errors == 0;

        std::cout
            << "processed=" << all_sequences.size()
            << " checksum_errors=" << total_checksum_errors
            << " generation_errors="
            << total_generation_errors
            << " sequence_errors=" << sequence_errors
            << " interrupts=" << stats.interrupts
            << " kernel_pending=" << stats.records_pending
            << " userspace_queued="
            << runtime.queued_records()
            << '\n';

        std::cout
            << (passed
                    ? "CONCURRENT CONSUMER TEST PASSED"
                    : "CONCURRENT CONSUMER TEST FAILED")
            << '\n';

        return passed ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr
            << "Concurrent runtime error: "
            << error.what()
            << '\n';

        return 1;
    }
}