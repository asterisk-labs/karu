// C API for positional byte-range reads.
//
// Karu reads local paths, HTTP URLs, and objects in S3, GCS, Azure,
// Hugging Face, and Source Cooperative.
//
// Copyright (c) 2026 Asterisk Labs. MIT.
#ifndef KARU_H
#define KARU_H

#include <stddef.h>
#include <stdint.h>

#define KARU_API

#ifdef __cplusplus
extern "C" {
#endif

#define KARU_API_VERSION 1
// Returned by karu_locator_window_length() for an unbounded locator. This is
// not a valid karu_req.length; range reads must always have a finite length.
#define KARU_TO_END UINT64_MAX

KARU_API int karu_api_version(void);
KARU_API const char* karu_version_string(void);
KARU_API const char* karu_http_backend(void);

typedef enum {
    KARU_OK = 0,
    KARU_END = 1,
    KARU_TIMEOUT = 2,
    KARU_ERR_URI = -1,
    KARU_ERR_UNSUPPORTED = -2,
    KARU_ERR_INVALID = -3,
    KARU_ERR_NOMEM = -4,
    KARU_ERR_IO = -5,
    KARU_ERR_NETWORK = -6,
    KARU_ERR_HTTP = -7,
    KARU_ERR_RANGE = -8,
    KARU_ERR_AUTH = -9,
    KARU_ERR_CANCELLED = -10,
    KARU_ERR_CONFIG = -11,
    KARU_ERR_CREDENTIALS = -12,
    KARU_ERR_NOT_FOUND = -13,
    KARU_ERR_PRECONDITION = -14
} karu_status;

KARU_API const char* karu_status_string(karu_status status);
KARU_API const char* karu_last_error(void);
KARU_API void karu_clear_error(void);
KARU_API void karu_free(void* ptr);

// Configuration ------------------------------------------------------------

// Configuration is mutable until a client is created. Clients copy the
// configuration and may be shared between threads.
typedef struct karu_config karu_config;
typedef struct karu_client karu_client;

// Initializes a builder from the supported environment variables.
KARU_API karu_status karu_config_create(karu_config** out);
// Initializes an empty builder. Default credential files and metadata
// services are not used unless configured explicitly.
KARU_API karu_status karu_config_create_empty(karu_config** out);
KARU_API void karu_config_free(karu_config* config);

// Option names are case-insensitive. A NULL value removes the option.
KARU_API karu_status karu_config_set_option(karu_config* config, const char* name,
                                            const char* value);

// Sets an option for a path prefix. URIs are normalized to VSI paths before
// matching, and the longest matching prefix wins. A NULL value removes it.
KARU_API karu_status karu_config_set_path_option(karu_config* config, const char* prefix,
                                                 const char* name, const char* value);

// Credential callbacks run when a request needs credentials and may run
// concurrently for different paths. Karu copies all returned strings before
// the callback returns.
typedef enum {
    KARU_CREDENTIALS_AWS = 1,
    KARU_CREDENTIALS_GCS = 2,
    KARU_CREDENTIALS_AZURE = 3,
    KARU_CREDENTIALS_SOURCE = 4
} karu_credentials_kind;

typedef struct {
    const char* access_key_id;
    const char* secret_access_key;
    const char* session_token;
    const char* bearer_token;
    const char* sas_token;
    const char* account_name;
    // Optional VSI prefix for credential reuse. NULL or empty disables reuse.
    const char* cache_prefix;
    // Expiration as Unix time, or 0 for credentials without an expiration.
    int64_t expires_at;
} karu_credentials;

typedef karu_status (*karu_credentials_provider)(void* user_data, karu_credentials_kind kind,
                                                 const char* canonical_path, karu_credentials* out);
typedef void (*karu_credentials_provider_free)(void* user_data);

// Pass NULL for provider, user_data, and release to remove the callback.
KARU_API karu_status karu_config_set_credentials_provider(karu_config* config,
                                                          karu_credentials_kind kind,
                                                          karu_credentials_provider provider,
                                                          void* user_data,
                                                          karu_credentials_provider_free release);

// Creates a client from a copy of config. The config may be freed afterwards.
KARU_API karu_status karu_client_create(const karu_config* config, karu_client** out);
KARU_API void karu_client_free(karu_client* client);

// Reports whether config would produce the client's current configuration.
// No I/O is performed.
KARU_API karu_status karu_client_matches_config(const karu_client* client,
                                                const karu_config* config, int* out_match);

KARU_API int karu_client_concurrency(const karu_client* client);
KARU_API uint64_t karu_client_coalesce_gap(const karu_client* client);
KARU_API int karu_client_max_attempts(const karu_client* client);

// Locators -----------------------------------------------------------------

typedef struct karu_locator karu_locator;

// Resolves a local path, supported URI, or VSI path.
KARU_API karu_status karu_resolve(const char* uri, karu_locator** out);
KARU_API void karu_locator_free(karu_locator* locator);
KARU_API const char* karu_locator_uri(const karu_locator* locator);
// Returns nonzero for HTTP and object-storage locators.
KARU_API int karu_locator_is_remote(const karu_locator* locator);
KARU_API uint64_t karu_locator_window_offset(const karu_locator* locator);
KARU_API uint64_t karu_locator_window_length(const karu_locator* locator);

// Gets the size visible through the locator. Results are not cached.
KARU_API karu_status karu_client_size(karu_client* client, const karu_locator* locator,
                                      uint64_t* out_size);

// Positional reads ---------------------------------------------------------

typedef struct {
    const karu_locator* locator;
    uint64_t offset;
    uint64_t length;
    // Destination buffer. If NULL, submit allocates one and the caller must
    // release the completion buffer with karu_free().
    void* buffer;
    void* tag;
    // Optional ETag precondition for remote objects. Copied by submit.
    const char* if_match;
} karu_req;

typedef struct {
    void* tag;
    karu_status status;
    uint64_t got;
    // Request destination. If Karu allocated it, release it with karu_free().
    void* buffer;
} karu_done;

typedef struct karu_batch karu_batch;

// Submits a batch. Karu copies the requests, locators, and ETags. Destination
// buffers supplied by the caller must remain valid until the batch is freed.
KARU_API karu_status karu_client_submit(karu_client* client, const karu_req* requests, size_t count,
                                        karu_batch** out_batch);
// Returns KARU_OK with one completion. KARU_END and KARU_TIMEOUT leave out
// unchanged.
KARU_API karu_status karu_batch_next(karu_batch* batch, karu_done* out, int timeout_ms);
// Cancels pending work and waits for active workers to release the buffers.
KARU_API void karu_batch_free(karu_batch* batch);
// Blocking batch read. Every request must provide a destination buffer.
KARU_API karu_status karu_client_fetch(karu_client* client, const karu_req* requests, size_t count);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // KARU_H
