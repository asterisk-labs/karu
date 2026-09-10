#include "config.hpp"
#include "locator.hpp"
#include "runtime/batch.hpp"
#include "runtime/planner.hpp"
#include "uri.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Pattern {
    std::string_view name;
    std::size_t count;
    std::uint64_t length;
    std::uint64_t stride;
};

struct Policy {
    std::string_view name;
    karu::ClientOptions options;
};

struct Result {
    std::size_t transfers = 0;
    std::uint64_t requested = 0;
    std::uint64_t fetched = 0;
    double microseconds = 0;
};

Result run(const karu::Locator& object, const Pattern& pattern, const Policy& policy,
           std::size_t iterations) {
    std::byte sink{};
    std::vector<karu::Request> requests(pattern.count);
    for (std::size_t index = 0; index < requests.size(); ++index) {
        requests[index] = karu::Request{
            .locator = &object,
            .offset = static_cast<std::uint64_t>(index) * pattern.stride,
            .length = pattern.length,
            .buffer = &sink,
        };
    }

    Result result;
    result.requested = static_cast<std::uint64_t>(pattern.count) * pattern.length;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        karu::BatchCore batch;
        karu_status status = KARU_OK;
        auto plan = karu::plan_transfers(batch, requests, policy.options, status);
        if (status != KARU_OK) {
            std::fprintf(stderr, "planner failed with status %d\n", static_cast<int>(status));
            return {};
        }
        if (iteration + 1 == iterations) {
            result.transfers = plan.transfers.size();
            for (const auto& transfer : plan.transfers)
                result.fetched += transfer->length;
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    result.microseconds = std::chrono::duration<double, std::micro>(elapsed).count() /
                          static_cast<double>(iterations);
    return result;
}

std::size_t iteration_count(int argc, char** argv) {
    if (argc == 1)
        return 200;
    if (argc != 2)
        return 0;
    std::size_t value = 0;
    const std::string_view text(argv[1]);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value > 0 ? value
                                                                                            : 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t iterations = iteration_count(argc, argv);
    if (iterations == 0) {
        std::fprintf(stderr, "usage: karu_coalescing_benchmark [iterations]\n");
        return 2;
    }

    auto resolved = karu::resolve("https://example.test/object");
    if (!resolved) {
        std::fprintf(stderr, "could not create benchmark locator\n");
        return 1;
    }
    const karu::Locator object{std::move(*resolved)};

    constexpr std::array patterns{
        Pattern{"contiguous", 1'000, 64u << 10, 64u << 10},
        Pattern{"alternating", 1'000, 64u << 10, 128u << 10},
        Pattern{"sparse", 1'000, 64u << 10, 1u << 20},
    };

    karu::ClientOptions current;
    karu::ClientOptions moderate = current;
    moderate.coalesce_limit = 16u << 20;
    moderate.coalesce_amplification = 4;
    karu::ClientOptions strict = moderate;
    strict.coalesce_amplification = 2;
    karu::ClientOptions separate = current;
    separate.coalesce_gap = 0;
    const std::array policies{
        Policy{"current", current},
        Policy{"limit16_amp4", moderate},
        Policy{"limit16_amp2", strict},
        Policy{"separate", separate},
    };

    std::puts(
        "pattern,policy,requests,transfers,requested_bytes,fetched_bytes,amplification,plan_us");
    for (const Pattern& pattern : patterns) {
        for (const Policy& policy : policies) {
            const Result result = run(object, pattern, policy, iterations);
            const double amplification =
                result.requested == 0
                    ? 0
                    : static_cast<double>(result.fetched) / static_cast<double>(result.requested);
            std::printf("%.*s,%.*s,%zu,%zu,%llu,%llu,%.4f,%.2f\n",
                        static_cast<int>(pattern.name.size()), pattern.name.data(),
                        static_cast<int>(policy.name.size()), policy.name.data(), pattern.count,
                        result.transfers, static_cast<unsigned long long>(result.requested),
                        static_cast<unsigned long long>(result.fetched), amplification,
                        result.microseconds);
        }
    }
    return 0;
}
