#include <cstdint>
#include <exception>
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

int main()
{
    try {
        QStreamRuntime stream;

        stream.reset();
        stream.start();

        std::cout << "Waiting for qstream data\n";

        if (!stream.wait_for_data(5000))
            throw std::runtime_error("Timed out waiting for data");

        std::uint64_t count = stream.available();

        if (count == 0)
            throw std::runtime_error("Runtime woke without records");

        const qstream_record &first = stream.record(0);

        std::uint64_t sequence = combine_words(
            first.words[QSTREAM_WORD_SEQUENCE_LO],
            first.words[QSTREAM_WORD_SEQUENCE_HI]);

        std::cout
            << "Received " << count
            << " records; first sequence=" << sequence
            << '\n';

        stream.consume(static_cast<std::uint32_t>(count));
        stream.stop();

        qstream_stats stats = stream.statistics();

        std::cout
            << "generated=" << stats.generated
            << " drained=" << stats.records_drained
            << " stored=" << stats.records_stored
            << " consumed=" << stats.records_consumed
            << " pending=" << stats.records_pending
            << '\n';

        if (stats.records_pending != 0 ||
            stats.records_stored != stats.records_consumed) {
            std::cerr << "RUNTIME TEST FAILED\n";
            return 1;
        }

        std::cout << "RUNTIME TEST PASSED\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Runtime error: " << error.what() << '\n';
        return 1;
    }
}