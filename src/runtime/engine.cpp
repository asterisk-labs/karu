#include "engine.hpp"

#include "../error.hpp"
#include "../platform.hpp"
#include "../text.hpp"
#include "http_response.hpp"
#include "planner.hpp"
#include "transport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <utility>

namespace karu {
namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr int kFileWorkers = 4;
constexpr int kMaximumRetryAfterSeconds = 60;

CURLcode initialize_curl() {
    static std::once_flag initialized;
    static CURLcode result = CURLE_FAILED_INIT;
    std::call_once(initialized, [] { result = curl_global_init(CURL_GLOBAL_DEFAULT); });
    return result;
}

void share_lock(CURL*, curl_lock_data data, curl_lock_access, void* user_data) {
    auto& state = *static_cast<CurlShareState*>(user_data);
    state.locks[static_cast<std::size_t>(data) % state.locks.size()].lock();
}

void share_unlock(CURL*, curl_lock_data data, void* user_data) {
    auto& state = *static_cast<CurlShareState*>(user_data);
    state.locks[static_cast<std::size_t>(data) % state.locks.size()].unlock();
}

void require_share_option(CURLSH* share, CURLSHoption option, curl_lock_function callback) {
    const CURLSHcode result = curl_share_setopt(share, option, callback);
    if (result != CURLSHE_OK)
        throw std::runtime_error(curl_share_strerror(result));
}

void require_share_option(CURLSH* share, CURLSHoption option, curl_unlock_function callback) {
    const CURLSHcode result = curl_share_setopt(share, option, callback);
    if (result != CURLSHE_OK)
        throw std::runtime_error(curl_share_strerror(result));
}

void require_shared_data(CURLSH* share, curl_lock_data data) {
    const CURLSHcode result = curl_share_setopt(share, CURLSHOPT_SHARE, data);
    if (result != CURLSHE_OK)
        throw std::runtime_error(curl_share_strerror(result));
}

template <typename Value> void require_multi_option(CURLM* multi, CURLMoption option, Value value) {
    const CURLMcode result = curl_multi_setopt(multi, option, value);
    if (result != CURLM_OK)
        throw std::runtime_error(curl_multi_strerror(result));
}

std::chrono::milliseconds retry_delay(const Transfer& transfer, std::mt19937& random) {
    const int requested = std::clamp(transfer.retry_after, 0, kMaximumRetryAfterSeconds);
    if (requested > 0) {
        std::uniform_int_distribution<int> jitter(0, 1000);
        return std::chrono::seconds(requested) + std::chrono::milliseconds(jitter(random));
    }
    const int base = 100 << std::min(transfer.attempt, 6);
    std::uniform_int_distribution<int> jitter(0, base);
    return std::chrono::milliseconds(base + jitter(random));
}

} // namespace

Engine::Engine(ConfigSnapshot config)
    : request_builder_(std::move(config)), options_(request_builder_.config().client_options()) {
    const CURLcode initialized = initialize_curl();
    if (initialized != CURLE_OK) {
        throw std::runtime_error(curl_easy_strerror(initialized));
    }

    multi_.reset(curl_multi_init());
    share_.reset(curl_share_init());
    if (!multi_ || !share_)
        throw std::bad_alloc();

    require_share_option(share_.get(), CURLSHOPT_LOCKFUNC, share_lock);
    require_share_option(share_.get(), CURLSHOPT_UNLOCKFUNC, share_unlock);
    if (const CURLSHcode result =
            curl_share_setopt(share_.get(), CURLSHOPT_USERDATA, &share_state_);
        result != CURLSHE_OK) {
        throw std::runtime_error(curl_share_strerror(result));
    }
    require_shared_data(share_.get(), CURL_LOCK_DATA_DNS);
    require_shared_data(share_.get(), CURL_LOCK_DATA_SSL_SESSION);
    require_multi_option(multi_.get(), CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
    require_multi_option(multi_.get(), CURLMOPT_MAX_TOTAL_CONNECTIONS, 0L);
    require_multi_option(multi_.get(), CURLMOPT_MAX_HOST_CONNECTIONS, 0L);
    easy_pool_.reserve(static_cast<std::size_t>(options_.concurrency));
    active_.reserve(static_cast<std::size_t>(options_.concurrency));

    try {
        io_thread_ = std::thread([this] { io_loop(); });
        file_workers_.reserve(kFileWorkers);
        for (int index = 0; index < kFileWorkers; ++index) {
            file_workers_.emplace_back([this] { file_loop(); });
        }
    } catch (...) {
        stop_workers();
        throw;
    }
}

Engine::~Engine() {
    stop_workers();

    for (auto& transfer : http_queue_) {
        discard_transfer(std::move(transfer));
    }
    for (auto& transfer : file_queue_) {
        discard_transfer(std::move(transfer));
    }
}

void Engine::stop_workers() noexcept {
    {
        // Publish shutdown while holding the mutex used by file workers for
        // their wait predicate. Without this pairing, a notify can land
        // between the predicate check and the worker actually sleeping.
        std::lock_guard lock(queue_mutex_);
        stop_.store(true, std::memory_order_release);
    }
    queue_cv_.notify_all();
    if (multi_)
        curl_multi_wakeup(multi_.get());
    if (io_thread_.joinable())
        io_thread_.join();
    for (std::thread& worker : file_workers_) {
        if (worker.joinable())
            worker.join();
    }
}

std::unique_ptr<BatchCore> Engine::submit(std::span<const Request> requests, karu_status& status) {
    auto batch = std::make_unique<BatchCore>();
    if (requests.empty()) {
        status = KARU_OK;
        return batch;
    }

    TransferPlan plan = plan_transfers(*batch, requests, options_, status);
    if (status != KARU_OK)
        return batch;

    {
        std::lock_guard lock(batch->mutex);
        for (const auto& transfer : plan.transfers) {
            batch->pending_parts += transfer->parts.size();
        }
        batch->pending_parts += plan.immediate.size();
        batch->live_transfers = plan.transfers.size();
    }
    for (Completion& completion : plan.immediate) {
        batch->push(std::move(completion));
    }

    {
        std::lock_guard lock(queue_mutex_);
        for (auto& transfer : plan.transfers) {
            if (transfer->locator->resolved.backend == Backend::File) {
                file_queue_.push_back(std::move(transfer));
            } else {
                http_queue_.push_back(std::move(transfer));
            }
        }
    }
    queue_cv_.notify_all();
    curl_multi_wakeup(multi_.get());
    return batch;
}

void Engine::deliver(Transfer& transfer, karu_status status, const std::string& detail) {
    for (Part& part : transfer.parts) {
        Completion completion{
            .tag = part.tag,
            .status = status,
            .buffer = part.buffer,
            .owned_buffer = std::move(part.owned_buffer),
            .detail = detail,
        };
        if (status == KARU_OK) {
            const std::uint64_t part_end = part.relative_offset + part.length;
            if (part_end > transfer.received) {
                completion.status = KARU_ERR_RANGE;
                completion.got = transfer.received > part.relative_offset
                                     ? transfer.received - part.relative_offset
                                     : 0;
                const auto& resolved = transfer.locator->resolved;
                const std::string uri = resolved.backend == Backend::Http
                                            ? redact_url(resolved.canonical_uri)
                                            : resolved.canonical_uri;
                completion.detail =
                    concat(uri, ": object ended at byte ", transfer.offset + transfer.received,
                           " while reading [", transfer.offset + part.relative_offset, ", +",
                           part.length, ")");
            } else {
                completion.got = part.length;
            }
            if (completion.got > 0 && transfer.scattered) {
                std::memcpy(part.buffer, transfer.scratch.data() + part.relative_offset,
                            static_cast<std::size_t>(completion.got));
            }
        }
        transfer.batch->push(std::move(completion));
    }
}

void Engine::finish_transfer(std::unique_ptr<Transfer> transfer, karu_status status,
                             std::string detail) {
    BatchCore* batch = transfer->batch;
    deliver(*transfer, status, detail);
    if (transfer->easy) {
        curl_easy_reset(transfer->easy.get());
        easy_pool_.push_back(std::move(transfer->easy));
    }
    transfer.reset();
    batch->transfer_finished();
}

void Engine::start_transfer(std::unique_ptr<Transfer> transfer) {
    try {
        if (!transport::ensure_sink(*transfer)) {
            finish_transfer(std::move(transfer), KARU_ERR_NOMEM,
                            "out of memory allocating a coalesced transfer buffer");
            return;
        }

        if (transfer->region_hint.empty()) {
            transfer->region_hint =
                transfer->batch->region_hint(transfer->locator->resolved.canonical_uri);
        }
        auto request =
            request_builder_.prepare(*transfer->locator, transfer->offset, transfer->length,
                                     transfer->region_hint, transfer->if_match);
        if (!request) {
            finish_transfer(std::move(transfer), request.error().status,
                            std::move(request.error().message));
            return;
        }
        transfer->request_url = std::move(request->url);
        transfer->request_headers = std::move(request->headers);
        transfer->http = std::make_unique<HttpRequestOptions>(std::move(request->http));
        if (!request->routing_region.empty())
            transfer->region_hint = std::move(request->routing_region);

        if (easy_pool_.empty()) {
            transfer->easy.reset(curl_easy_init());
        } else {
            transfer->easy = std::move(easy_pool_.back());
            easy_pool_.pop_back();
        }
        if (!transfer->easy) {
            finish_transfer(std::move(transfer), KARU_ERR_NOMEM, "curl_easy_init: out of memory");
            return;
        }
        transfer->http_buffers = std::make_unique<HttpBuffers>();

        if (auto configured = transport::configure(*transfer, share_.get(), options_);
            !configured) {
            finish_transfer(std::move(transfer), KARU_ERR_NETWORK,
                            "could not configure HTTP request: " + configured.error());
            return;
        }

        CURL* easy = transfer->easy.get();
        const CURLMcode added = curl_multi_add_handle(multi_.get(), easy);
        if (added != CURLM_OK) {
            finish_transfer(std::move(transfer), KARU_ERR_NETWORK,
                            concat("curl_multi_add_handle: ", curl_multi_strerror(added)));
            return;
        }
        Transfer* key = transfer.get();
        auto [entry, inserted] = active_.try_emplace(key);
        if (!inserted) {
            curl_multi_remove_handle(multi_.get(), transfer->easy.get());
            finish_transfer(std::move(transfer), KARU_ERR_INVALID,
                            "internal error registering HTTP transfer");
            return;
        }
        entry->second = std::move(transfer);
    } catch (const std::bad_alloc&) {
        if (transfer) {
            if (transfer->easy)
                curl_multi_remove_handle(multi_.get(), transfer->easy.get());
            finish_transfer(std::move(transfer), KARU_ERR_NOMEM, "out of memory preparing request");
        }
    } catch (const std::exception& error) {
        if (transfer) {
            if (transfer->easy)
                curl_multi_remove_handle(multi_.get(), transfer->easy.get());
            finish_transfer(std::move(transfer), KARU_ERR_INVALID,
                            concat("could not prepare request: ", error.what()));
        }
    } catch (...) {
        if (transfer) {
            if (transfer->easy)
                curl_multi_remove_handle(multi_.get(), transfer->easy.get());
            finish_transfer(std::move(transfer), KARU_ERR_INVALID,
                            "could not prepare request: unknown C++ exception");
        }
    }
}

void Engine::discard_transfer(std::unique_ptr<Transfer> transfer) noexcept {
    BatchCore* batch = transfer->batch;
    transfer.reset();
    batch->transfer_finished();
}

void Engine::discard_cancelled_http() {
    for (auto iterator = pending_.begin(); iterator != pending_.end();) {
        if (!(*iterator)->batch->is_cancelled()) {
            ++iterator;
            continue;
        }
        auto transfer = std::move(*iterator);
        iterator = pending_.erase(iterator);
        discard_transfer(std::move(transfer));
    }

    for (auto iterator = retries_.begin(); iterator != retries_.end();) {
        if (!iterator->transfer->batch->is_cancelled()) {
            ++iterator;
            continue;
        }
        auto transfer = std::move(iterator->transfer);
        iterator = retries_.erase(iterator);
        discard_transfer(std::move(transfer));
    }

    for (auto iterator = active_.begin(); iterator != active_.end();) {
        if (!iterator->second->batch->is_cancelled()) {
            ++iterator;
            continue;
        }
        curl_multi_remove_handle(multi_.get(), iterator->second->easy.get());
        auto transfer = std::move(iterator->second);
        iterator = active_.erase(iterator);
        discard_transfer(std::move(transfer));
    }
}

void Engine::io_loop() {
    std::mt19937 random{static_cast<std::mt19937::result_type>(os::pid())};

    while (!stop_.load(std::memory_order_acquire)) {
        std::deque<std::unique_ptr<Transfer>> incoming;
        {
            std::lock_guard lock(queue_mutex_);
            incoming.swap(http_queue_);
        }
        for (auto& transfer : incoming)
            pending_.push_back(std::move(transfer));
        if (cancellation_pending_.exchange(false, std::memory_order_acq_rel))
            discard_cancelled_http();

        const auto now = SteadyClock::now();
        for (auto iterator = retries_.begin(); iterator != retries_.end();) {
            if (iterator->due > now) {
                ++iterator;
                continue;
            }
            pending_.push_back(std::move(iterator->transfer));
            iterator = retries_.erase(iterator);
        }

        const auto limit = static_cast<std::size_t>(options_.concurrency);
        if (limit != last_connection_limit_) {
            last_connection_limit_ = limit;
            curl_multi_setopt(multi_.get(), CURLMOPT_MAXCONNECTS, static_cast<long>(limit));
        }
        while (active_.size() < limit && !pending_.empty()) {
            auto transfer = std::move(pending_.front());
            pending_.pop_front();
            if (transfer->batch->is_cancelled()) {
                discard_transfer(std::move(transfer));
                continue;
            }
            start_transfer(std::move(transfer));
        }

        int running = 0;
        const CURLMcode performed = curl_multi_perform(multi_.get(), &running);
        if (performed != CURLM_OK) {
            for (auto iterator = active_.begin(); iterator != active_.end();) {
                curl_multi_remove_handle(multi_.get(), iterator->second->easy.get());
                auto transfer = std::move(iterator->second);
                iterator = active_.erase(iterator);
                finish_transfer(std::move(transfer), KARU_ERR_NETWORK,
                                concat("curl_multi_perform: ", curl_multi_strerror(performed)));
            }
        }

        int remaining_messages = 0;
        while (CURLMsg* message = curl_multi_info_read(multi_.get(), &remaining_messages)) {
            if (message->msg != CURLMSG_DONE)
                continue;

            Transfer* key = nullptr;
            curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &key);
            auto node = active_.extract(key);
            if (node.empty())
                continue;
            curl_multi_remove_handle(multi_.get(), message->easy_handle);
            std::unique_ptr<Transfer> transfer = std::move(node.mapped());

            if (transfer->http_status == 0) {
                curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE,
                                  &transfer->http_status);
            }
            const CURLcode result = message->data.result;
            if (transport::succeeded(*transfer, result)) {
                finish_transfer(std::move(transfer), KARU_OK);
                continue;
            }

            if (transfer->response_region.empty() &&
                transfer->locator->resolved.backend == Backend::S3) {
                transfer->response_region = transport::detail::s3_region(std::string_view(
                    transfer->http_buffers->error_body.data(), transfer->error_body_size));
            }
            if (!transfer->region_retried && !transfer->response_region.empty() &&
                transfer->response_region != transfer->region_hint &&
                transfer->locator->resolved.backend == Backend::S3) {
                transfer->region_hint = transfer->response_region;
                transfer->batch->remember_region(transfer->locator->resolved.canonical_uri,
                                                 transfer->response_region);
                transfer->region_retried = true;
                curl_easy_reset(transfer->easy.get());
                easy_pool_.push_back(std::move(transfer->easy));
                pending_.push_front(std::move(transfer));
                continue;
            }

            if (!transfer->credentials_retried &&
                transport::detail::credentials_expired(*transfer)) {
                request_builder_.invalidate_credentials(*transfer->locator);
                transfer->credentials_retried = true;
                curl_easy_reset(transfer->easy.get());
                easy_pool_.push_back(std::move(transfer->easy));
                pending_.push_front(std::move(transfer));
                continue;
            }

            if (transfer->attempt + 1 < options_.max_attempts &&
                transport::retryable(*transfer, result)) {
                ++transfer->attempt;
                const auto delay = retry_delay(*transfer, random);
                curl_easy_reset(transfer->easy.get());
                easy_pool_.push_back(std::move(transfer->easy));
                retries_.push_back(Retry{SteadyClock::now() + delay, std::move(transfer)});
                continue;
            }

            transport::Failure failed = transport::failure(*transfer, result);
            finish_transfer(std::move(transfer), failed.status, std::move(failed.detail));
        }

        int timeout_ms = 50;
        if (!pending_.empty() && active_.size() < limit) {
            timeout_ms = 0;
        } else if (!retries_.empty()) {
            const auto next = std::min_element(retries_.begin(), retries_.end(),
                                               [](const Retry& left, const Retry& right) {
                                                   return left.due < right.due;
                                               })
                                  ->due;
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(next - SteadyClock::now())
                    .count();
            timeout_ms = static_cast<int>(std::clamp<std::int64_t>(remaining, 0, 50));
        } else if (running == 0 && pending_.empty()) {
            timeout_ms = 200;
        }
        int descriptors = 0;
        curl_multi_poll(multi_.get(), nullptr, 0, timeout_ms, &descriptors);
    }

    while (!active_.empty()) {
        auto node = active_.extract(active_.begin());
        curl_multi_remove_handle(multi_.get(), node.mapped()->easy.get());
        discard_transfer(std::move(node.mapped()));
    }
    for (auto& transfer : pending_) {
        discard_transfer(std::move(transfer));
    }
    pending_.clear();
    for (Retry& retry : retries_) {
        discard_transfer(std::move(retry.transfer));
    }
    retries_.clear();
}

void Engine::file_loop() {
    while (true) {
        std::unique_ptr<Transfer> transfer;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return stop_.load(std::memory_order_acquire) || !file_queue_.empty();
            });
            if (file_queue_.empty()) {
                if (stop_.load(std::memory_order_acquire))
                    return;
                continue;
            }
            transfer = std::move(file_queue_.front());
            file_queue_.pop_front();
        }

        const std::string& path = transfer->locator->resolved.target;
        if (transfer->batch->is_cancelled()) {
            discard_transfer(std::move(transfer));
            continue;
        }
        if (!transport::ensure_sink(*transfer)) {
            finish_transfer(std::move(transfer), KARU_ERR_NOMEM,
                            concat(path, ": out of memory staging the read"));
            continue;
        }

        os::File file;
        if (const std::error_code error = file.open(path)) {
            finish_transfer(std::move(transfer), KARU_ERR_IO, concat(path, ": ", error.message()));
            continue;
        }

        std::string failure;
        while (transfer->received < transfer->length && !transfer->batch->is_cancelled()) {
            const auto read =
                file.read_at(transfer->sink + transfer->received,
                             static_cast<std::size_t>(transfer->length - transfer->received),
                             transfer->offset + transfer->received);
            if (!read) {
                failure = concat(path, ": ", read.error().message());
                break;
            }
            if (*read == 0)
                break;
            transfer->received += *read;
        }

        if (transfer->batch->is_cancelled()) {
            discard_transfer(std::move(transfer));
        } else if (!failure.empty()) {
            finish_transfer(std::move(transfer), KARU_ERR_IO, std::move(failure));
        } else {
            finish_transfer(std::move(transfer), KARU_OK);
        }
    }
}

void Engine::cancel(BatchCore& batch) {
    batch.cancelled.store(true, std::memory_order_release);
    batch.cv.notify_all();

    {
        std::lock_guard lock(queue_mutex_);
        for (auto iterator = file_queue_.begin(); iterator != file_queue_.end();) {
            if ((*iterator)->batch != &batch) {
                ++iterator;
                continue;
            }
            auto transfer = std::move(*iterator);
            iterator = file_queue_.erase(iterator);
            discard_transfer(std::move(transfer));
        }
    }

    queue_cv_.notify_all();
    cancellation_pending_.store(true, std::memory_order_release);
    curl_multi_wakeup(multi_.get());

    std::unique_lock lock(batch.mutex);
    batch.cv.wait(lock, [&batch] { return batch.live_transfers == 0; });
}

karu_status Engine::size_of(const Locator& locator, std::uint64_t& size) {
    const auto& resolved = locator.resolved;
    if (resolved.window_length != TO_END) {
        size = resolved.window_length;
        return KARU_OK;
    }

    if (resolved.backend == Backend::File) {
        std::error_code error;
        const auto total = std::filesystem::file_size(os::path_from_utf8(resolved.target), error);
        if (error) {
            set_error(concat(resolved.target, ": ", error.message()));
            return KARU_ERR_IO;
        }
        size = total > resolved.window_offset ? total - resolved.window_offset : 0;
        return KARU_OK;
    }

    auto result = transport::size_of(locator, share_.get(), request_builder_, options_);
    if (!result) {
        set_error(result.error().detail);
        return result.error().status;
    }
    size = *result > resolved.window_offset ? *result - resolved.window_offset : 0;
    return KARU_OK;
}

} // namespace karu
