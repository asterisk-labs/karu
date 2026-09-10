// Modern C++ facade for Karu's stable C ABI.
#ifndef KARU_HPP
#define KARU_HPP

#include "karu.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace karu {

struct Error {
    karu_status status = KARU_OK;
    std::string message;
};

template <typename Value> using Result = std::expected<Value, Error>;

inline Error current_error(karu_status status) {
    return Error{status, karu_last_error()};
}

class Config {
  public:
    Config(Config&&) noexcept = default;
    Config& operator=(Config&&) noexcept = default;
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    [[nodiscard]] static Result<Config> from_environment() {
        karu_config* raw = nullptr;
        const karu_status status = karu_config_create(&raw);
        if (status != KARU_OK)
            return std::unexpected(current_error(status));
        return Config(raw);
    }

    [[nodiscard]] static Result<Config> empty() {
        karu_config* raw = nullptr;
        const karu_status status = karu_config_create_empty(&raw);
        if (status != KARU_OK)
            return std::unexpected(current_error(status));
        return Config(raw);
    }

    [[nodiscard]] Result<void> set(std::string_view name, std::string_view value) {
        const std::string stable_name(name);
        const std::string stable_value(value);
        const karu_status status =
            karu_config_set_option(handle_.get(), stable_name.c_str(), stable_value.c_str());
        if (status != KARU_OK)
            return std::unexpected(current_error(status));
        return {};
    }

    [[nodiscard]] Result<void> unset(std::string_view name) {
        const std::string stable_name(name);
        const karu_status status =
            karu_config_set_option(handle_.get(), stable_name.c_str(), nullptr);
        if (status != KARU_OK)
            return std::unexpected(current_error(status));
        return {};
    }

    [[nodiscard]] Result<void> set_path(std::string_view prefix, std::string_view name,
                                        std::string_view value) {
        const std::string stable_prefix(prefix);
        const std::string stable_name(name);
        const std::string stable_value(value);
        const karu_status status = karu_config_set_path_option(
            handle_.get(), stable_prefix.c_str(), stable_name.c_str(), stable_value.c_str());
        if (status != KARU_OK)
            return std::unexpected(current_error(status));
        return {};
    }

    [[nodiscard]] Result<void> unset_path(std::string_view prefix, std::string_view name) {
        const std::string stable_prefix(prefix);
        const std::string stable_name(name);
        const karu_status status = karu_config_set_path_option(handle_.get(), stable_prefix.c_str(),
                                                               stable_name.c_str(), nullptr);
        if (status != KARU_OK)
            return std::unexpected(current_error(status));
        return {};
    }

    [[nodiscard]] Result<void>
    set_credentials_provider(karu_credentials_kind kind, karu_credentials_provider provider,
                             void* user_data, karu_credentials_provider_free release = nullptr) {
        const karu_status status =
            karu_config_set_credentials_provider(handle_.get(), kind, provider, user_data, release);
        if (status != KARU_OK)
            return std::unexpected(current_error(status));
        return {};
    }

    [[nodiscard]] Result<void> clear_credentials_provider(karu_credentials_kind kind) {
        return set_credentials_provider(kind, nullptr, nullptr);
    }

    [[nodiscard]] karu_config* native_handle() const noexcept { return handle_.get(); }

  private:
    explicit Config(karu_config* handle) : handle_(handle) {}
    struct Deleter {
        void operator()(karu_config* value) const noexcept { karu_config_free(value); }
    };
    std::unique_ptr<karu_config, Deleter> handle_;
};

class Object {
  public:
    Object(Object&&) noexcept = default;
    Object& operator=(Object&&) noexcept = default;
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    [[nodiscard]] static Result<Object> parse(std::string_view uri) {
        const std::string stable(uri);
        karu_locator* raw = nullptr;
        const karu_status status = karu_resolve(stable.c_str(), &raw);
        if (status != KARU_OK)
            return std::unexpected(current_error(status));
        return Object(raw);
    }

    [[nodiscard]] std::string_view uri() const noexcept { return karu_locator_uri(handle_.get()); }
    [[nodiscard]] bool is_remote() const noexcept {
        return karu_locator_is_remote(handle_.get()) != 0;
    }
    [[nodiscard]] std::uint64_t window_offset() const noexcept {
        return karu_locator_window_offset(handle_.get());
    }
    [[nodiscard]] std::uint64_t window_length() const noexcept {
        return karu_locator_window_length(handle_.get());
    }
    [[nodiscard]] const karu_locator* native_handle() const noexcept { return handle_.get(); }

  private:
    explicit Object(karu_locator* handle) : handle_(handle) {}
    struct Deleter {
        void operator()(karu_locator* value) const noexcept { karu_locator_free(value); }
    };
    std::unique_ptr<karu_locator, Deleter> handle_;
};

struct Read {
    const Object* object = nullptr;
    std::uint64_t offset = 0;
    // Only the prefix reported by BatchEvent::bytes_read may be inspected.
    std::span<std::byte> destination;
    void* tag = nullptr;
    std::string_view if_match;
};

enum class BatchState { ready, end, timeout };

struct BatchEvent {
    BatchState state = BatchState::end;
    void* tag = nullptr;
    karu_status status = KARU_OK;
    // Equals the requested length on success. An error normally reports zero;
    // KARU_ERR_RANGE may expose a valid prefix.
    std::uint64_t bytes_read = 0;
    std::string message;
};

class Batch {
  public:
    Batch(Batch&&) noexcept = default;
    Batch& operator=(Batch&&) noexcept = default;
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;

    [[nodiscard]] Result<BatchEvent> next(int timeout_ms = -1) {
        karu_done completed{};
        const karu_status status = karu_batch_next(handle_.get(), &completed, timeout_ms);
        if (status == KARU_END)
            return BatchEvent{.state = BatchState::end,
                              .tag = nullptr,
                              .status = KARU_OK,
                              .bytes_read = 0,
                              .message = {}};
        if (status == KARU_TIMEOUT)
            return BatchEvent{.state = BatchState::timeout,
                              .tag = nullptr,
                              .status = KARU_OK,
                              .bytes_read = 0,
                              .message = {}};
        if (status != KARU_OK)
            return std::unexpected(current_error(status));
        return BatchEvent{.state = BatchState::ready,
                          .tag = completed.tag,
                          .status = completed.status,
                          .bytes_read = completed.got,
                          .message = completed.status == KARU_OK ? "" : karu_last_error()};
    }

  private:
    friend class Client;
    explicit Batch(karu_batch* handle) : handle_(handle) {}
    struct Deleter {
        void operator()(karu_batch* value) const noexcept { karu_batch_free(value); }
    };
    std::unique_ptr<karu_batch, Deleter> handle_;
};

class Client {
  public:
    Client(Client&&) noexcept = default;
    Client& operator=(Client&&) noexcept = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    [[nodiscard]] static Result<Client> create(const Config& config) {
        karu_client* raw = nullptr;
        const karu_status status = karu_client_create(config.native_handle(), &raw);
        if (status != KARU_OK)
            return std::unexpected(current_error(status));
        return Client(raw);
    }

    [[nodiscard]] Result<bool> matches(const Config& config) const {
        int match = 0;
        const karu_status status =
            karu_client_matches_config(handle_.get(), config.native_handle(), &match);
        if (status != KARU_OK)
            return std::unexpected(current_error(status));
        return match != 0;
    }

    [[nodiscard]] Result<std::uint64_t> size(const Object& object) const {
        std::uint64_t value = 0;
        const karu_status status = karu_client_size(handle_.get(), object.native_handle(), &value);
        if (status != KARU_OK)
            return std::unexpected(current_error(status));
        return value;
    }

    [[nodiscard]] Result<void> read_into(const Object& object, std::uint64_t offset,
                                         std::span<std::byte> destination,
                                         std::string_view if_match = {}) const {
        if (destination.empty())
            return {};
        const std::string condition(if_match);
        karu_req request{.locator = object.native_handle(),
                         .offset = offset,
                         .length = destination.size(),
                         .buffer = destination.data(),
                         .tag = nullptr,
                         .if_match = condition.empty() ? nullptr : condition.c_str()};
        const karu_status status = karu_client_fetch(handle_.get(), &request, 1);
        if (status != KARU_OK)
            return std::unexpected(current_error(status));
        return {};
    }

    [[nodiscard]] Result<std::vector<std::byte>> read(const Object& object, std::uint64_t offset,
                                                      std::size_t length,
                                                      std::string_view if_match = {}) const {
        std::vector<std::byte> result(length);
        auto completed = read_into(object, offset, result, if_match);
        if (!completed)
            return std::unexpected(std::move(completed.error()));
        return result;
    }

    [[nodiscard]] Result<void> fetch(std::span<const Read> reads) const {
        auto requests = make_requests(reads);
        if (!requests)
            return std::unexpected(std::move(requests.error()));
        const karu_status status =
            karu_client_fetch(handle_.get(), requests->requests.data(), requests->requests.size());
        if (status != KARU_OK)
            return std::unexpected(current_error(status));
        return {};
    }

    [[nodiscard]] Result<Batch> submit(std::span<const Read> reads) const {
        auto requests = make_requests(reads);
        if (!requests)
            return std::unexpected(std::move(requests.error()));
        karu_batch* raw = nullptr;
        const karu_status status = karu_client_submit(handle_.get(), requests->requests.data(),
                                                      requests->requests.size(), &raw);
        if (status != KARU_OK)
            return std::unexpected(current_error(status));
        return Batch(raw);
    }

    [[nodiscard]] int concurrency() const noexcept {
        return karu_client_concurrency(handle_.get());
    }
    [[nodiscard]] std::uint64_t coalesce_gap() const noexcept {
        return karu_client_coalesce_gap(handle_.get());
    }
    [[nodiscard]] int max_attempts() const noexcept {
        return karu_client_max_attempts(handle_.get());
    }
    [[nodiscard]] karu_client* native_handle() const noexcept { return handle_.get(); }

  private:
    struct NativeReads {
        std::vector<std::string> conditions;
        std::vector<karu_req> requests;
    };

    [[nodiscard]] static Result<NativeReads> make_requests(std::span<const Read> reads) {
        NativeReads result;
        result.conditions.reserve(reads.size());
        result.requests.reserve(reads.size());
        for (const Read& read : reads) {
            if (read.object == nullptr)
                return std::unexpected(Error{KARU_ERR_INVALID, "read has no object"});
            if (read.destination.empty())
                return std::unexpected(Error{KARU_ERR_INVALID, "read has an empty destination"});
            result.conditions.emplace_back(read.if_match);
            const char* condition =
                result.conditions.back().empty() ? nullptr : result.conditions.back().c_str();
            result.requests.push_back(karu_req{.locator = read.object->native_handle(),
                                               .offset = read.offset,
                                               .length = read.destination.size(),
                                               .buffer = read.destination.data(),
                                               .tag = read.tag,
                                               .if_match = condition});
        }
        return result;
    }

    explicit Client(karu_client* handle) : handle_(handle) {}
    struct Deleter {
        void operator()(karu_client* value) const noexcept { karu_client_free(value); }
    };
    std::unique_ptr<karu_client, Deleter> handle_;
};

} // namespace karu

#endif
