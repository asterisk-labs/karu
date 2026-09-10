#include "karu/karu.h"

#include "client.hpp"
#include "config.hpp"
#include "error.hpp"
#include "locator.hpp"
#include "runtime/batch.hpp"
#include "runtime/engine.hpp"
#include "text.hpp"
#include "uri.hpp"

#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#ifndef KARU_VERSION_STRING
#define KARU_VERSION_STRING "0.0.0"
#endif

namespace {

karu_status exception_status(std::string_view call) noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        karu::set_error(karu::concat(call, ": out of memory"));
        return KARU_ERR_NOMEM;
    } catch (const std::exception& error) {
        karu::set_error(karu::concat(call, ": ", error.what()));
        return KARU_ERR_INVALID;
    } catch (...) {
        karu::set_error(karu::concat(call, ": unknown C++ exception"));
        return KARU_ERR_INVALID;
    }
}

std::vector<karu::Request> convert_requests(const karu_req* requests, std::size_t count,
                                            std::string_view call) {
    std::vector<karu::Request> converted;
    converted.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const karu_req& request = requests[index];
        if (request.locator == nullptr) {
            karu::set_error(karu::concat(call, ": request ", index, " has no locator"));
            return {};
        }
        if (request.length == 0) {
            karu::set_error(karu::concat(call, ": request ", index, " is empty"));
            return {};
        }
        if (request.length == KARU_TO_END) {
            karu::set_error(
                karu::concat(call, ": request ", index, " does not have a finite length"));
            return {};
        }
        if (request.if_match != nullptr &&
            std::string_view(request.if_match).find_first_of("\r\n") != std::string_view::npos) {
            karu::set_error(karu::concat(call, ": request ", index, " has an invalid ETag"));
            return {};
        }
        converted.push_back(karu::Request{request.locator, request.offset, request.length,
                                          request.buffer, request.tag,
                                          request.if_match == nullptr ? "" : request.if_match});
    }
    return converted;
}

} // namespace

extern "C" {

int karu_api_version(void) {
    return KARU_API_VERSION;
}

const char* karu_version_string(void) {
    return KARU_VERSION_STRING;
}

const char* karu_http_backend(void) {
    return curl_version();
}

const char* karu_status_string(karu_status status) {
    switch (status) {
    case KARU_OK:
        return "ok";
    case KARU_END:
        return "batch drained";
    case KARU_TIMEOUT:
        return "timed out";
    case KARU_ERR_URI:
        return "malformed URI";
    case KARU_ERR_UNSUPPORTED:
        return "unsupported virtual filesystem";
    case KARU_ERR_INVALID:
        return "invalid argument";
    case KARU_ERR_NOMEM:
        return "out of memory";
    case KARU_ERR_IO:
        return "file error";
    case KARU_ERR_NETWORK:
        return "network error";
    case KARU_ERR_HTTP:
        return "http error";
    case KARU_ERR_RANGE:
        return "range past end of object";
    case KARU_ERR_AUTH:
        return "not authorized";
    case KARU_ERR_CANCELLED:
        return "cancelled";
    case KARU_ERR_CONFIG:
        return "invalid configuration";
    case KARU_ERR_CREDENTIALS:
        return "credentials unavailable";
    case KARU_ERR_NOT_FOUND:
        return "object not found";
    case KARU_ERR_PRECONDITION:
        return "precondition failed";
    }
    return "unknown status";
}

const char* karu_last_error(void) {
    return karu::last_error();
}

void karu_clear_error(void) {
    karu::clear_error();
}

void karu_free(void* pointer) {
    std::free(pointer);
}

karu_status karu_config_create(karu_config** out) {
    karu::clear_error();
    if (out == nullptr) {
        karu::set_error("karu_config_create: null output");
        return KARU_ERR_INVALID;
    }
    *out = nullptr;
    try {
        *out = new karu_config(true);
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_config_create");
    }
}

karu_status karu_config_create_empty(karu_config** out) {
    karu::clear_error();
    if (out == nullptr) {
        karu::set_error("karu_config_create_empty: null output");
        return KARU_ERR_INVALID;
    }
    *out = nullptr;
    try {
        *out = new karu_config(false);
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_config_create_empty");
    }
}

void karu_config_free(karu_config* config) {
    delete config;
}

karu_status karu_config_set_option(karu_config* config, const char* name, const char* value) {
    karu::clear_error();
    if (config == nullptr || name == nullptr) {
        karu::set_error("karu_config_set_option: null argument");
        return KARU_ERR_INVALID;
    }
    try {
        auto result = config->builder.set(name, value);
        if (!result) {
            karu::set_error(result.error());
            return KARU_ERR_CONFIG;
        }
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_config_set_option");
    }
}

karu_status karu_config_set_path_option(karu_config* config, const char* prefix, const char* name,
                                        const char* value) {
    karu::clear_error();
    if (config == nullptr || prefix == nullptr || name == nullptr) {
        karu::set_error("karu_config_set_path_option: null argument");
        return KARU_ERR_INVALID;
    }
    try {
        auto result = config->builder.set_path(prefix, name, value);
        if (!result) {
            karu::set_error(result.error());
            return KARU_ERR_CONFIG;
        }
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_config_set_path_option");
    }
}

karu_status karu_config_set_credentials_provider(karu_config* config, karu_credentials_kind kind,
                                                 karu_credentials_provider provider,
                                                 void* user_data,
                                                 karu_credentials_provider_free release) {
    karu::clear_error();
    if (config == nullptr) {
        karu::set_error("karu_config_set_credentials_provider: null config");
        return KARU_ERR_INVALID;
    }
    try {
        auto result = config->builder.set_provider(kind, provider, user_data, release);
        if (!result) {
            karu::set_error(result.error());
            return KARU_ERR_CONFIG;
        }
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_config_set_credentials_provider");
    }
}

karu_status karu_client_create(const karu_config* config, karu_client** out) {
    karu::clear_error();
    if (config == nullptr || out == nullptr) {
        karu::set_error("karu_client_create: null argument");
        return KARU_ERR_INVALID;
    }
    *out = nullptr;
    try {
        auto snapshot = config->builder.freeze();
        if (!snapshot) {
            karu::set_error(snapshot.error());
            return KARU_ERR_CONFIG;
        }
        *out = new karu_client(std::move(*snapshot));
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_client_create");
    }
}

void karu_client_free(karu_client* client) {
    delete client;
}

karu_status karu_client_matches_config(const karu_client* client, const karu_config* config,
                                       int* out_match) {
    karu::clear_error();
    if (client == nullptr || config == nullptr || out_match == nullptr) {
        karu::set_error("karu_client_matches_config: null argument");
        return KARU_ERR_INVALID;
    }
    *out_match = 0;
    try {
        auto snapshot = config->builder.freeze();
        if (!snapshot) {
            karu::set_error(snapshot.error());
            return KARU_ERR_CONFIG;
        }
        *out_match = client->config == *snapshot;
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_client_matches_config");
    }
}

int karu_client_concurrency(const karu_client* client) {
    return client == nullptr ? 0 : client->config.client_options().concurrency;
}

uint64_t karu_client_coalesce_gap(const karu_client* client) {
    return client == nullptr ? 0 : client->config.client_options().coalesce_gap;
}

int karu_client_max_attempts(const karu_client* client) {
    return client == nullptr ? 0 : client->config.client_options().max_attempts;
}

karu_status karu_resolve(const char* uri, karu_locator** out) {
    karu::clear_error();
    if (uri == nullptr || out == nullptr) {
        karu::set_error("karu_resolve: null argument");
        return KARU_ERR_INVALID;
    }
    *out = nullptr;
    try {
        auto resolved = karu::resolve(uri);
        if (!resolved) {
            karu::set_error(resolved.error().message);
            return resolved.error().code == karu::ResolveErrorCode::Unsupported
                       ? KARU_ERR_UNSUPPORTED
                       : KARU_ERR_URI;
        }
        *out = new karu::Locator{std::move(*resolved)};
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_resolve");
    }
}

void karu_locator_free(karu_locator* locator) {
    delete locator;
}

const char* karu_locator_uri(const karu_locator* locator) {
    return locator == nullptr ? "" : locator->resolved.canonical_uri.c_str();
}

int karu_locator_is_remote(const karu_locator* locator) {
    return locator != nullptr && locator->resolved.backend != karu::Backend::File;
}

uint64_t karu_locator_window_offset(const karu_locator* locator) {
    return locator == nullptr ? 0 : locator->resolved.window_offset;
}

uint64_t karu_locator_window_length(const karu_locator* locator) {
    return locator == nullptr ? 0 : locator->resolved.window_length;
}

karu_status karu_client_size(karu_client* client, const karu_locator* locator, uint64_t* out_size) {
    karu::clear_error();
    if (client == nullptr || locator == nullptr || out_size == nullptr) {
        karu::set_error("karu_client_size: null argument");
        return KARU_ERR_INVALID;
    }
    try {
        return client->acquire_engine()->size_of(*locator, *out_size);
    } catch (...) {
        return exception_status("karu_client_size");
    }
}

karu_status karu_client_submit(karu_client* client, const karu_req* requests, size_t count,
                               karu_batch** out_batch) {
    karu::clear_error();
    if (client == nullptr || out_batch == nullptr || (count > 0 && requests == nullptr)) {
        karu::set_error("karu_client_submit: null argument");
        return KARU_ERR_INVALID;
    }
    *out_batch = nullptr;
    try {
        const auto converted = convert_requests(requests, count, "karu_client_submit");
        if (converted.size() != count)
            return KARU_ERR_INVALID;
        auto engine = client->acquire_engine();
        karu_status status = KARU_OK;
        auto batch = engine->submit(converted, status);
        if (status != KARU_OK)
            return status;
        batch->owner = std::move(engine);
        *out_batch = batch.release();
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_client_submit");
    }
}

karu_status karu_batch_next(karu_batch* batch, karu_done* out, int timeout_ms) {
    karu::clear_error();
    if (batch == nullptr || out == nullptr) {
        karu::set_error("karu_batch_next: null argument");
        return KARU_ERR_INVALID;
    }
    try {
        karu::Completion completion;
        const karu_status status = batch->next(completion, timeout_ms);
        if (status != KARU_OK)
            return status;
        if (completion.status != KARU_OK && !completion.detail.empty())
            karu::set_error(completion.detail);
        out->tag = completion.tag;
        out->status = completion.status;
        out->got = completion.got;
        out->buffer = completion.release_buffer();
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_batch_next");
    }
}

void karu_batch_free(karu_batch* batch) {
    if (batch == nullptr)
        return;
    std::unique_ptr<karu::BatchCore> owned(batch);
    const auto engine = owned->owner;
    if (engine)
        engine->cancel(*owned);
}

karu_status karu_client_fetch(karu_client* client, const karu_req* requests, size_t count) {
    karu::clear_error();
    if (client == nullptr || (count > 0 && requests == nullptr)) {
        karu::set_error("karu_client_fetch: null argument");
        return KARU_ERR_INVALID;
    }
    try {
        for (std::size_t index = 0; index < count; ++index) {
            if (requests[index].buffer == nullptr) {
                karu::set_error(karu::concat("karu_client_fetch: request ", index,
                                             " has no destination buffer"));
                return KARU_ERR_INVALID;
            }
        }
        const auto converted = convert_requests(requests, count, "karu_client_fetch");
        if (converted.size() != count)
            return KARU_ERR_INVALID;
        auto engine = client->acquire_engine();
        karu_status status = KARU_OK;
        auto batch = engine->submit(converted, status);
        if (status != KARU_OK)
            return status;

        karu_status first_failure = KARU_OK;
        std::string first_detail;
        for (;;) {
            karu::Completion completion;
            const karu_status step = batch->next(completion, -1);
            if (step == KARU_END)
                break;
            if (step != KARU_OK) {
                first_failure = step;
                break;
            }
            if (first_failure == KARU_OK && completion.status != KARU_OK) {
                first_failure = completion.status;
                first_detail = completion.detail;
            }
        }
        engine->cancel(*batch);
        if (!first_detail.empty())
            karu::set_error(first_detail);
        return first_failure;
    } catch (...) {
        return exception_status("karu_client_fetch");
    }
}

} // extern "C"
