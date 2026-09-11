#include "locator.hpp"
#include "runtime/batch.hpp"
#include "runtime/planner.hpp"
#include "runtime/transport.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace karu::test {
namespace {

void property_at(int line, bool condition, unsigned seed, const char* message) {
    ++checks;
    if (!condition)
        fail(line, "seed " + std::to_string(seed) + ": " + message);
}

#define PROPERTY(value, seed, message) property_at(__LINE__, (value), (seed), (message))

} // namespace

void test_planner_property() {
    SECTION("planner and scatter properties");

    constexpr std::uint64_t object_size = 1u << 16;
    std::vector<std::byte> object(object_size);
    for (std::uint64_t index = 0; index < object_size; ++index)
        object[index] = static_cast<std::byte>((index * 131 + 17) & 0xff);

    Locator locator{must_resolve("https://example.test/property")};
    for (unsigned seed = 1; seed <= 3'000; ++seed) {
        std::mt19937 random(seed);
        const std::size_t count = std::uniform_int_distribution<std::size_t>(1, 24)(random);

        ClientOptions options;
        options.coalesce_gap = std::uniform_int_distribution<std::uint64_t>(0, 4'096)(random);
        options.coalesce_limit = std::uniform_int_distribution<std::uint64_t>(1, 40'000)(random);
        options.coalesce_parts = std::uniform_int_distribution<std::size_t>(1, 8)(random);
        options.coalesce_amplification =
            std::uniform_int_distribution<std::uint64_t>(1, 20)(random);

        struct Range {
            std::uint64_t offset;
            std::uint64_t length;
        };
        std::vector<Range> ranges;
        std::vector<std::vector<std::byte>> destinations;
        ranges.reserve(count);
        destinations.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const std::uint64_t offset =
                std::uniform_int_distribution<std::uint64_t>(0, object_size - 1)(random);
            const std::uint64_t length = std::uniform_int_distribution<std::uint64_t>(
                1, std::min<std::uint64_t>(object_size - offset, 3'000))(random);
            ranges.push_back({offset, length});
            destinations.emplace_back(static_cast<std::size_t>(length), std::byte{0xab});
        }

        std::vector<Request> requests;
        requests.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            requests.push_back({&locator,
                                ranges[index].offset,
                                ranges[index].length,
                                destinations[index].data(),
                                reinterpret_cast<void*>(static_cast<std::uintptr_t>(index + 1)),
                                {}});
        }

        BatchCore batch;
        karu_status status = KARU_OK;
        TransferPlan plan = plan_transfers(batch, requests, options, status);
        PROPERTY(status == KARU_OK, seed, "planner returned an error");
        PROPERTY(plan.immediate.empty(), seed, "valid request completed immediately");

        std::vector<unsigned char> seen(count);
        for (auto& transfer : plan.transfers) {
            std::uint64_t span_end = 0;
            std::uint64_t useful = 0;
            bool starts_at_zero = false;
            for (const Part& part : transfer->parts) {
                const std::size_t index = reinterpret_cast<std::uintptr_t>(part.tag) - 1;
                PROPERTY(index < count, seed, "part tag is outside the request set");
                if (index >= count)
                    continue;
                ++seen[index];
                span_end = std::max(span_end, part.relative_offset + part.length);
                useful += part.length;
                starts_at_zero = starts_at_zero || part.relative_offset == 0;
                PROPERTY(transfer->offset + part.relative_offset == ranges[index].offset, seed,
                         "part absolute offset differs from its request");
                PROPERTY(part.length == ranges[index].length, seed,
                         "part length differs from its request");
            }

            PROPERTY(starts_at_zero, seed, "no part starts at the transfer offset");
            PROPERTY(span_end == transfer->length, seed, "transfer does not span its parts");
            PROPERTY(transfer->parts.size() <= options.coalesce_parts, seed,
                     "coalesce part limit exceeded");
            PROPERTY(transfer->parts.size() == 1 || transfer->length <= options.coalesce_limit,
                     seed, "coalesce byte limit exceeded");
            PROPERTY(transfer->parts.size() == 1 ||
                         transfer->length <= useful * options.coalesce_amplification,
                     seed, "coalesce amplification limit exceeded");

            const bool transfer_in_object = transfer->offset <= object_size &&
                                            transfer->length <= object_size - transfer->offset;
            PROPERTY(transfer_in_object, seed, "transfer leaves the synthetic object");
            const bool sink_ready = transport::ensure_sink(*transfer);
            PROPERTY(sink_ready, seed, "scatter sink was not prepared");
            if (!transfer_in_object || !sink_ready)
                continue;
            std::uint64_t consumed = 0;
            while (consumed < transfer->length) {
                const std::uint64_t chunk = std::min<std::uint64_t>(
                    transfer->length - consumed,
                    std::uniform_int_distribution<std::uint64_t>(1, 5'000)(random));
                transport::store_payload(*transfer, object.data() + transfer->offset + consumed,
                                         static_cast<std::size_t>(chunk));
                consumed += chunk;
            }
            PROPERTY(transfer->received == transfer->length, seed,
                     "scatter byte count differs from transfer length");
        }

        for (std::size_t index = 0; index < count; ++index) {
            PROPERTY(seen[index] == 1, seed, "request was not planned exactly once");
            PROPERTY(std::memcmp(destinations[index].data(), object.data() + ranges[index].offset,
                                 static_cast<std::size_t>(ranges[index].length)) == 0,
                     seed, "scatter destination has incorrect bytes");
        }
    }
}

} // namespace karu::test
