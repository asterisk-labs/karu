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
#include <span>
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
    const karu::Read reads[] = {
        {&*object, 0, first, reinterpret_cast<void*>(1), {}},
        {&*object, 64, second, reinterpret_cast<void*>(2), {}},
    };
    auto batch = client->submit(reads);
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
