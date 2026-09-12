#include "karu/karu.h"
#include "karu/karu.hpp"
#include "locator.hpp"
#include "runtime/batch.hpp"
#include "runtime/engine.hpp"
#include "runtime/http_response.hpp"
#include "runtime/planner.hpp"
#include "runtime/transport.hpp"
#include "test_support.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace karu::test {

void test_local_engine() {
    SECTION("local engine");
    const auto data = make_fixture(1u << 20);
    karu_config* config = nullptr;
    karu_client* client = nullptr;
    karu_locator* object = nullptr;
    EQ(karu_config_create_empty(&config), KARU_OK);
    EQ(karu_config_set_option(config, "KARU_COALESCE_GAP", "4096"), KARU_OK);
    EQ(karu_client_create(config, &client), KARU_OK);
    EQ(karu_resolve(fixture_path.c_str(), &object), KARU_OK);

    std::uint64_t size = 0;
    EQ(karu_client_size(client, object, &size), KARU_OK);
    EQ(size, data.size());

    constexpr std::size_t count = 128;
    constexpr std::size_t length = 511;
    std::vector<std::array<unsigned char, length>> buffers(count);
    std::vector<karu_req> requests(count);
    for (std::size_t index = 0; index < count; ++index) {
        requests[index] = karu_req{
            object, index * 4096, length, buffers[index].data(), reinterpret_cast<void*>(index + 1),
            nullptr};
    }
    karu_batch* batch = nullptr;
    EQ(karu_client_submit(client, requests.data(), requests.size(), &batch), KARU_OK);
    std::size_t completed = 0;
    for (;;) {
        karu_done done{};
        const karu_status status = karu_batch_next(batch, &done, -1);
        if (status == KARU_END)
            break;
        EQ(status, KARU_OK);
        EQ(done.status, KARU_OK);
        EQ(done.got, length);
        const std::size_t index = reinterpret_cast<std::size_t>(done.tag) - 1;
        OK(std::memcmp(buffers[index].data(), data.data() + index * 4096, length) == 0);
        ++completed;
    }
    EQ(completed, count);
    karu_batch_free(batch);
    karu_locator_free(object);
    karu_client_free(client);
    karu_config_free(config);
}

void test_windows_allocations_and_errors() {
    SECTION("windows, allocation and errors");
    const auto data = make_fixture(8192);
    karu_config* config = nullptr;
    karu_client* client = nullptr;
    karu_locator* window = nullptr;
    EQ(karu_config_create_empty(&config), KARU_OK);
    EQ(karu_client_create(config, &client), KARU_OK);
    const std::string window_uri = "/vsisubfile/1000_2000," + fixture_path;
    EQ(karu_resolve(window_uri.c_str(), &window), KARU_OK);

    std::uint64_t size = 0;
    EQ(karu_client_size(client, window, &size), KARU_OK);
    EQ(size, 2000u);

    std::array<unsigned char, 100> destination{};
    karu_req good{window, 50, destination.size(), destination.data(), nullptr, nullptr};
    EQ(karu_client_fetch(client, &good, 1), KARU_OK);
    OK(std::memcmp(destination.data(), data.data() + 1050, destination.size()) == 0);

    karu_req outside{window, 1950, destination.size(), destination.data(), nullptr, nullptr};
    EQ(karu_client_fetch(client, &outside, 1), KARU_ERR_RANGE);

    karu_req unbounded{window, 0, KARU_TO_END, destination.data(), nullptr, nullptr};
    EQ(karu_client_fetch(client, &unbounded, 1), KARU_ERR_INVALID);

    karu_req allocated{window, 128, 256, nullptr, reinterpret_cast<void*>(42), nullptr};
    karu_batch* batch = nullptr;
    EQ(karu_client_submit(client, &allocated, 1, &batch), KARU_OK);
    karu_done done{};
    EQ(karu_batch_next(batch, &done, -1), KARU_OK);
    EQ(done.status, KARU_OK);
    EQ(done.got, 256u);
    OK(done.buffer != nullptr);
    if (done.buffer != nullptr)
        OK(std::memcmp(done.buffer, data.data() + 1128, 256) == 0);
    karu_free(done.buffer);
    EQ(karu_batch_next(batch, &done, -1), KARU_END);
    karu_batch_free(batch);
    EQ(karu_client_fetch(client, &allocated, 1), KARU_ERR_INVALID);

    karu_locator* missing = nullptr;
    const std::string missing_path = fixture_path + ".missing";
    std::filesystem::remove(missing_path);
    EQ(karu_resolve(missing_path.c_str(), &missing), KARU_OK);
    EQ(karu_client_size(client, missing, &size), KARU_ERR_IO);
    karu_req missing_read{missing, 0, destination.size(), destination.data(), nullptr, nullptr};
    EQ(karu_client_fetch(client, &missing_read, 1), KARU_ERR_IO);

    karu_locator* malformed = nullptr;
    EQ(karu_resolve("", &malformed), KARU_ERR_URI);
    EQ(karu_resolve("/vsizip/archive.zip/file", &malformed), KARU_ERR_UNSUPPORTED);
    EQ(karu_resolve(nullptr, &malformed), KARU_ERR_INVALID);
    OK(std::strlen(karu_last_error()) > 0);

    karu_locator_free(missing);
    karu_locator_free(window);
    karu_client_free(client);
    karu_config_free(config);
}

void test_batch_lifetimes_and_concurrency() {
    SECTION("batch lifetime and concurrency");
    const auto data = make_fixture(4u << 20);
    karu_config* config = nullptr;
    karu_client* client = nullptr;
    karu_locator* object = nullptr;
    EQ(karu_config_create_empty(&config), KARU_OK);
    EQ(karu_config_set_option(config, "KARU_COALESCE_GAP", "0"), KARU_OK);
    EQ(karu_client_create(config, &client), KARU_OK);
    EQ(karu_resolve(fixture_path.c_str(), &object), KARU_OK);

    constexpr std::size_t count = 256;
    std::vector<std::array<unsigned char, 4096>> buffers(count);
    std::vector<karu_req> requests(count);
    for (std::size_t index = 0; index < count; ++index) {
        requests[index] = karu_req{object,
                                   index * 8192,
                                   buffers[index].size(),
                                   buffers[index].data(),
                                   reinterpret_cast<void*>(index + 1),
                                   nullptr};
    }
    karu_batch* batch = nullptr;
    EQ(karu_client_submit(client, requests.data(), requests.size(), &batch), KARU_OK);

    // A batch owns the engine and copies locator identity; all three setup
    // objects can go away while the submitted buffers remain valid.
    karu_locator_free(object);
    object = nullptr;
    karu_client_free(client);
    client = nullptr;
    karu_config_free(config);
    config = nullptr;
    karu_done first{};
    const karu_status polled = karu_batch_next(batch, &first, 0);
    OK(polled == KARU_OK || polled == KARU_TIMEOUT);
    karu_batch_free(batch);

    // Fresh shared client: concurrent positional reads do not share a cursor.
    EQ(karu_config_create_empty(&config), KARU_OK);
    EQ(karu_client_create(config, &client), KARU_OK);
    EQ(karu_resolve(fixture_path.c_str(), &object), KARU_OK);
    std::atomic<int> errors{0};

    // Repeatedly abandon batches from several submitter threads. This keeps
    // cancellation, Karu-owned buffers, and a shared engine under contention.
    std::vector<std::thread> cancellers;
    for (std::size_t thread_index = 0; thread_index < 4; ++thread_index) {
        cancellers.emplace_back([&, thread_index] {
            for (std::size_t iteration = 0; iteration < 20; ++iteration) {
                std::array<karu_req, 24> cancelled{};
                for (std::size_t index = 0; index < cancelled.size(); ++index) {
                    const std::size_t offset =
                        (thread_index * 65537 + iteration * 4099 + index * 8191) %
                        (data.size() - 257);
                    cancelled[index] = karu_req{object, offset, 257, nullptr, nullptr, nullptr};
                }
                karu_batch* abandoned = nullptr;
                if (karu_client_submit(client, cancelled.data(), cancelled.size(), &abandoned) !=
                    KARU_OK) {
                    ++errors;
                    continue;
                }
                karu_batch_free(abandoned);
            }
        });
    }
    for (std::thread& canceller : cancellers)
        canceller.join();
    EQ(errors.load(), 0);

    std::vector<std::thread> readers;
    for (std::size_t thread_index = 0; thread_index < 8; ++thread_index) {
        readers.emplace_back([&, thread_index] {
            std::array<unsigned char, 257> output{};
            for (std::size_t iteration = 0; iteration < 16; ++iteration) {
                const std::size_t offset =
                    (thread_index * 65537 + iteration * 4099) % (data.size() - output.size());
                karu_req request{object, offset, output.size(), output.data(), nullptr, nullptr};
                if (karu_client_fetch(client, &request, 1) != KARU_OK ||
                    std::memcmp(output.data(), data.data() + offset, output.size()) != 0) {
                    ++errors;
                }
            }
        });
    }
    for (std::thread& reader : readers)
        reader.join();
    EQ(errors.load(), 0);

    // A batch is a concurrent completion queue: multiple consumers must
    // collectively observe every completion exactly once and may all reach END.
    constexpr std::size_t shared_count = 256;
    std::vector<std::array<unsigned char, 257>> shared_buffers(shared_count);
    std::vector<karu_req> shared_requests(shared_count);
    for (std::size_t index = 0; index < shared_count; ++index) {
        const std::size_t offset = index * 4099;
        shared_requests[index] =
            karu_req{object,  offset, shared_buffers[index].size(), shared_buffers[index].data(),
                     nullptr, nullptr};
    }
    karu_batch* shared_batch = nullptr;
    EQ(karu_client_submit(client, shared_requests.data(), shared_requests.size(), &shared_batch),
       KARU_OK);
    std::atomic<std::size_t> shared_ready{0};
    std::atomic<int> shared_ends{0};
    std::array<std::thread, 8> consumers;
    for (std::thread& consumer : consumers) {
        consumer = std::thread([&] {
            for (;;) {
                karu_done done{};
                const karu_status status = karu_batch_next(shared_batch, &done, -1);
                if (status == KARU_END) {
                    ++shared_ends;
                    return;
                }
                if (status != KARU_OK || done.status != KARU_OK) {
                    ++errors;
                    return;
                }
                ++shared_ready;
            }
        });
    }
    for (std::thread& consumer : consumers)
        consumer.join();
    EQ(errors.load(), 0);
    EQ(shared_ready.load(), shared_count);
    EQ(shared_ends.load(), static_cast<int>(consumers.size()));
    for (std::size_t index = 0; index < shared_count; ++index)
        OK(std::memcmp(shared_buffers[index].data(), data.data() + index * 4099,
                       shared_buffers[index].size()) == 0);
    karu_batch_free(shared_batch);

    // Undrained Karu-owned buffers remain owned by the batch.
    std::array<karu_req, 32> owned{};
    for (std::size_t index = 0; index < owned.size(); ++index)
        owned[index] = karu_req{object, index * 1024, 512, nullptr, nullptr, nullptr};
    batch = nullptr;
    EQ(karu_client_submit(client, owned.data(), owned.size(), &batch), KARU_OK);
    karu_batch_free(batch);

    karu_locator_free(object);
    karu_client_free(client);
    karu_config_free(config);
}

void test_engine_shutdown() {
    SECTION("engine shutdown");
    const karu::ConfigSnapshot config = must_freeze(karu::ConfigBuilder(false));
    for (int iteration = 0; iteration < 64; ++iteration) {
        karu::Engine engine(config);
    }
    OK(true);
}

void test_planner_scope() {
    SECTION("batch-only coalescing");
    karu::Locator object{must_resolve("https://example.test/a")};
    std::array<std::byte, 32> first{};
    std::array<std::byte, 32> second{};
    const karu::Request requests[] = {
        {&object, 0, first.size(), first.data(), nullptr, {}},
        {&object, first.size(), second.size(), second.data(), nullptr, {}},
    };
    karu::BatchCore no_merge;
    karu_status status = KARU_OK;
    karu::ClientOptions options;
    options.coalesce_gap = 0;
    auto separate = karu::plan_transfers(no_merge, requests, options, status);
    EQ(status, KARU_OK);
    EQ(separate.transfers.size(), 2u);
    OK(separate.transfers[0]->locator == separate.transfers[1]->locator);
    karu::BatchCore merge;
    options.coalesce_gap = 1;
    auto together = karu::plan_transfers(merge, requests, options, status);
    EQ(status, KARU_OK);
    EQ(together.transfers.size(), 1u);

    const karu::Request conditioned[] = {
        {&object, 0, first.size(), first.data(), nullptr, "\"one\""},
        {&object, first.size(), second.size(), second.data(), nullptr, "\"two\""},
    };
    karu::BatchCore identities;
    auto isolated = karu::plan_transfers(identities, conditioned, options, status);
    EQ(status, KARU_OK);
    EQ(isolated.transfers.size(), 2u);

    const karu::Request sparse[] = {
        {&object, 0, 1, first.data(), nullptr, {}},
        {&object, 100, 1, second.data(), nullptr, {}},
    };
    options.coalesce_gap = 100;
    options.coalesce_limit = 64;
    karu::BatchCore span_limited;
    auto spans = karu::plan_transfers(span_limited, sparse, options, status);
    EQ(spans.transfers.size(), 2u);

    options.coalesce_limit = 1024;
    options.coalesce_amplification = 16;
    karu::BatchCore amplification_limited;
    auto amplified = karu::plan_transfers(amplification_limited, sparse, options, status);
    EQ(amplified.transfers.size(), 2u);

    options.coalesce_amplification = 1024;
    options.coalesce_parts = 1;
    karu::BatchCore part_limited;
    auto parts = karu::plan_transfers(part_limited, sparse, options, status);
    EQ(parts.transfers.size(), 2u);

    // Duplicate logical reads share one transfer and retain distinct parts.
    const karu::Request duplicates[] = {
        {&object, 300, first.size(), first.data(), reinterpret_cast<void*>(1), {}},
        {&object, 300, second.size(), second.data(), reinterpret_cast<void*>(2), {}},
    };
    options = {};
    karu::BatchCore duplicate_batch;
    auto duplicate_plan = karu::plan_transfers(duplicate_batch, duplicates, options, status);
    EQ(status, KARU_OK);
    EQ(duplicate_plan.transfers.size(), 1u);
    EQ(duplicate_plan.transfers.front()->length, first.size());
    EQ(duplicate_plan.transfers.front()->parts.size(), 2u);
    EQ(duplicate_plan.transfers.front()->parts[0].relative_offset, 0u);
    EQ(duplicate_plan.transfers.front()->parts[1].relative_offset, 0u);

    // Keep the amplification guard honest for a sparse batch. This is the
    // synthetic pattern used by benchmarks/coalescing.cpp.
    constexpr std::size_t request_count = 1'000;
    constexpr std::uint64_t request_length = 64u << 10;
    constexpr std::uint64_t request_stride = 1u << 20;
    std::vector<karu::Request> strided(request_count);
    for (std::size_t index = 0; index < strided.size(); ++index) {
        strided[index] = {&object, index * request_stride, request_length, first.data(), nullptr,
                          {}};
    }
    karu::BatchCore strided_batch;
    auto strided_plan = karu::plan_transfers(strided_batch, strided, {}, status);
    EQ(status, KARU_OK);
    EQ(strided_plan.transfers.size(), 16u);
    std::uint64_t fetched = 0;
    for (const auto& transfer : strided_plan.transfers)
        fetched += transfer->length;
    const std::uint64_t requested = request_count * request_length;
    OK(fetched <= requested * karu::ClientOptions{}.coalesce_amplification);

    // Local batches may share one read only when their ranges touch. Reading
    // a local gap provides no network round-trip to amortize.
    karu::Locator file{must_resolve(fixture_path)};
    const karu::Request local_sparse[] = {
        {&file, 0, first.size(), first.data(), nullptr, {}},
        {&file, 64, second.size(), second.data(), nullptr, {}},
    };
    options = {};
    karu::BatchCore local_sparse_batch;
    auto local_separate = karu::plan_transfers(local_sparse_batch, local_sparse, options, status);
    EQ(status, KARU_OK);
    EQ(local_separate.transfers.size(), 2u);

    const karu::Request local_contiguous[] = {
        {&file, 0, first.size(), first.data(), nullptr, {}},
        {&file, first.size(), second.size(), second.data(), nullptr, {}},
    };
    karu::BatchCore local_contiguous_batch;
    auto local_together =
        karu::plan_transfers(local_contiguous_batch, local_contiguous, options, status);
    EQ(status, KARU_OK);
    EQ(local_together.transfers.size(), 1u);

    // Exercise each arithmetic rejection independently. The planner must
    // report these as immediate range completions without wrapping offsets.
    std::byte invalid_destination{};
    karu::Locator outside_window{must_resolve("https://example.test/window")};
    outside_window.resolved.window_length = 8;
    karu::Locator absolute_overflow{must_resolve("https://example.test/absolute?token=secret")};
    absolute_overflow.resolved.window_offset = std::numeric_limits<std::uint64_t>::max() - 3;
    karu::Locator end_overflow{must_resolve("https://example.test/end")};
    const std::array<karu::Request, 3> invalid_ranges{{
        {&outside_window, 9, 1, &invalid_destination, reinterpret_cast<void*>(1), {}},
        {&absolute_overflow, 4, 1, &invalid_destination, reinterpret_cast<void*>(2), {}},
        {&end_overflow,
         std::numeric_limits<std::uint64_t>::max() - 3,
         8,
         &invalid_destination,
         reinterpret_cast<void*>(3),
         {}},
    }};
    karu::BatchCore invalid_batch;
    auto invalid_plan = karu::plan_transfers(invalid_batch, invalid_ranges, {}, status);
    EQ(status, KARU_OK);
    OK(invalid_plan.transfers.empty());
    EQ(invalid_plan.immediate.size(), invalid_ranges.size());
    for (const auto& completion : invalid_plan.immediate)
        EQ(completion.status, KARU_ERR_RANGE);
    OK(invalid_plan.immediate[0].detail.find("leaves the locator window") != std::string::npos);
    OK(invalid_plan.immediate[1].detail.find("overflows its absolute offset") != std::string::npos);
    OK(invalid_plan.immediate[1].detail.find("secret") == std::string::npos);
    OK(invalid_plan.immediate[1].detail.find("<redacted>") != std::string::npos);
    OK(invalid_plan.immediate[2].detail.find("overflows its end offset") != std::string::npos);
}

void test_transport_statuses() {
    SECTION("transport status classification");
    karu::Transfer transfer;
    transfer.http_buffers = std::make_unique<karu::HttpBuffers>();
    transfer.request_url = "https://example.test/object";
    transfer.http_status = 302;
    OK(!karu::transport::succeeded(transfer, CURLE_OK));
    EQ(karu::transport::failure(transfer, CURLE_OK).status, KARU_ERR_HTTP);
    transfer.http_status = 404;
    EQ(karu::transport::failure(transfer, CURLE_OK).status, KARU_ERR_NOT_FOUND);
    transfer.http_status = 412;
    EQ(karu::transport::failure(transfer, CURLE_OK).status, KARU_ERR_PRECONDITION);
    transfer.request_url = "https://example.test/object?token=secret";
    transfer.http_status = 404;
    const auto redacted = karu::transport::failure(transfer, CURLE_OK);
    OK(redacted.detail.find("secret") == std::string::npos);
    OK(redacted.detail.find("<redacted>") != std::string::npos);

    auto transfer_locator = std::make_shared<karu::Locator>();
    transfer_locator->resolved.backend = karu::Backend::S3;
    transfer.locator = std::move(transfer_locator);
    transfer.http_status = 403;
    constexpr std::string_view expired = "<Error><Code>ExpiredToken</Code></Error>";
    std::memcpy(transfer.http_buffers->error_body.data(), expired.data(), expired.size());
    transfer.error_body_size = expired.size();
    OK(karu::transport::detail::credentials_expired(transfer));
    EQS(karu::transport::detail::s3_region("<Error><Region>eu-west-1</Region></Error>"),
        "eu-west-1");
    EQ(karu::transport::detail::retry_after_seconds("17", 0), 17);
    EQ(karu::transport::detail::retry_after_seconds("Wed, 21 Oct 2015 07:28:00 GMT", 1'445'412'420),
       60);
    EQ(karu::transport::detail::retry_after_seconds("invalid", 0), 0);

    std::array<std::byte, 24> payload{};
    std::array<std::byte, 6> first_part{};
    std::array<std::byte, 6> duplicate_part{};
    std::array<std::byte, 8> overlap_part{};
    std::array<std::byte, 6> final_part{};
    for (std::size_t index = 0; index < payload.size(); ++index)
        payload[index] = static_cast<std::byte>(index);

    karu::Transfer scattered;
    scattered.locator = std::make_shared<karu::Locator>(must_resolve("https://example.test/body"));
    scattered.length = payload.size();
    scattered.scattered = true;
    auto part = [](std::uint64_t offset, auto& buffer) {
        karu::Part value;
        value.relative_offset = offset;
        value.length = buffer.size();
        value.buffer = buffer.data();
        return value;
    };
    scattered.parts.push_back(part(0, first_part));
    scattered.parts.push_back(part(0, duplicate_part));
    scattered.parts.push_back(part(4, overlap_part));
    scattered.parts.push_back(part(18, final_part));

    OK(karu::transport::ensure_sink(scattered));
    OK(scattered.sink == nullptr);
    OK(scattered.scratch.empty());
    karu::transport::store_payload(scattered, payload.data(), 3);
    karu::transport::store_payload(scattered, payload.data() + 3, 8);
    karu::transport::store_payload(scattered, payload.data() + 11, 13);
    EQ(scattered.received, payload.size());
    OK(scattered.scratch.empty());
    OK(std::memcmp(first_part.data(), payload.data(), first_part.size()) == 0);
    OK(std::memcmp(duplicate_part.data(), payload.data(), duplicate_part.size()) == 0);
    OK(std::memcmp(overlap_part.data(), payload.data() + 4, overlap_part.size()) == 0);
    OK(std::memcmp(final_part.data(), payload.data() + 18, final_part.size()) == 0);

    for (std::size_t index = 0; index < payload.size(); ++index)
        payload[index] = static_cast<std::byte>(payload.size() - index);
    scattered.received = 0;
    scattered.scatter_cursor = 0;
    karu::transport::store_payload(scattered, payload.data(), payload.size());
    OK(std::memcmp(first_part.data(), payload.data(), first_part.size()) == 0);
    OK(std::memcmp(duplicate_part.data(), payload.data(), duplicate_part.size()) == 0);
    OK(std::memcmp(overlap_part.data(), payload.data() + 4, overlap_part.size()) == 0);
    OK(std::memcmp(final_part.data(), payload.data() + 18, final_part.size()) == 0);
}

void test_cpp_facade() {
    SECTION("C++ facade");
    auto config = karu::Config::empty();
    OK(config.has_value());
    if (!config)
        return;
    OK(config->set("KARU_CONCURRENCY", "3"));
    auto client = karu::Client::create(*config);
    auto object = karu::Object::parse(fixture_path);
    OK(client.has_value());
    OK(object.has_value());
    if (!client || !object)
        return;
    auto matches = client->matches(*config);
    OK(matches.has_value() && *matches);
    OK(config->set("KARU_CONCURRENCY", "4"));
    matches = client->matches(*config);
    OK(matches.has_value() && !*matches);
    EQ(client->concurrency(), 3);
    OK(!object->is_remote());
    auto bytes = client->read(*object, 17, 23);
    OK(bytes.has_value());
    if (bytes)
        EQ(bytes->size(), 23u);

    std::array<std::byte, 1> conditioned{};
    const karu::Read local_condition{&*object, 0, conditioned, nullptr, "\"etag\""};
    auto unsupported = client->submit(std::span(&local_condition, 1));
    OK(unsupported.has_value());
    if (unsupported) {
        auto event = unsupported->next();
        OK(event.has_value());
        if (event) {
            EQ(event->status, KARU_ERR_UNSUPPORTED);
            OK(!event->message.empty());
        }
    }

    std::array<std::byte, 8> first{};
    std::array<std::byte, 8> second{};
    const karu::Object* const object_value = &object.value();
    std::array<karu::Read, 2> reads{};
    reads[0].object = object_value;
    reads[0].destination = first;
    reads[0].tag = first.data();
    reads[1].object = object_value;
    reads[1].offset = 64;
    reads[1].destination = second;
    reads[1].tag = second.data();
    karu::SubmitOptions submit_options;
    submit_options.coalesce_gap = 0;
    auto batch = client->submit(std::span<const karu::Read>(reads), submit_options);
    OK(batch.has_value());
    int ready = 0;
    while (batch) {
        auto event = batch->next();
        OK(event.has_value());
        if (!event || event->state == karu::BatchState::end)
            break;
        if (event->state == karu::BatchState::ready) {
            EQ(event->status, KARU_OK);
            ++ready;
        }
    }
    EQ(ready, 2);
}

} // namespace karu::test
