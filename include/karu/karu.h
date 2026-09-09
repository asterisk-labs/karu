// karu: stateless positional reads for local files, HTTP and object storage.
//
// The public surface deliberately resembles GDAL VSI: /vsis3/, /vsigs/ and
// /vsiaz/ paths use the same option names and path-specific overrides. The
// execution model is object-store-like: there is no cursor and no semantic
// cache. A client only retains transport pools and renewable credentials.
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

#define KARU_API_VERSION 2
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

// A mutable builder. Creating it snapshots supported environment variables;
// changing the process environment afterwards cannot alter an existing
// client. Builders need not be synchronized. Clients are immutable and safe
// to share between threads.
typedef struct karu_config karu_config;
typedef struct karu_client karu_client;

KARU_API karu_status karu_config_create(karu_config** out);
// Hermetic builder: no environment snapshot, default credential files, or
// implicit metadata probes. Setting their paths/endpoints explicitly opts in.
KARU_API karu_status karu_config_create_empty(karu_config** out);
KARU_API void karu_config_free(karu_config* config);

// Names are case-insensitive. GDAL names such as AWS_REGION,
// AWS_ACCESS_KEY_ID, GS_NO_SIGN_REQUEST and AZURE_STORAGE_ACCESS_KEY are used
// unchanged. Passing value == NULL removes an explicit option.
KARU_API karu_status karu_config_set_option(karu_config* config, const char* name,
                                            const char* value);

// The longest matching canonical VSI prefix wins. s3://bucket/key,
// gs://bucket/key, az://container/key and source://account/product/key are
// matched as /vsis3/bucket/key, /vsigs/bucket/key, /vsiaz/container/key and
// /vsisource/account/product/key respectively. Empty path segments in object
// keys are significant and are never collapsed.
KARU_API karu_status karu_config_set_path_option(karu_config* config, const char* prefix,
                                                 const char* name, const char* value);

// Custom credentials are fetched lazily and copied before the callback
// returns. expires_at is Unix time, or 0 when the value does not expire.
// AWS and Source use access_key_id/secret_access_key/session_token, with
// separate callback kinds. GCS normally uses bearer_token (or the
// access/secret pair for HMAC). Azure uses bearer_token, sas_token, or
// account_name plus secret_access_key for Shared Key.
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
    // Optional canonical VSI prefix under which Karu may reuse this value
    // until expires_at. NULL/empty asks Karu to call the provider every time.
    const char* cache_prefix;
    int64_t expires_at;
} karu_credentials;

typedef karu_status (*karu_credentials_provider)(void* user_data, karu_credentials_kind kind,
                                                 const char* canonical_path, karu_credentials* out);
typedef void (*karu_credentials_provider_free)(void* user_data);

// Passing a NULL provider with NULL user_data and release clears that kind.
KARU_API karu_status karu_config_set_credentials_provider(karu_config* config,
                                                          karu_credentials_kind kind,
                                                          karu_credentials_provider provider,
                                                          void* user_data,
                                                          karu_credentials_provider_free release);

// Construction validates the snapshot's client-wide and scalar values.
// Config can be freed as soon as this returns: the client owns an immutable
// copy. Endpoint-specific validation occurs when an object is materialized.
KARU_API karu_status karu_client_create(const karu_config* config, karu_client** out);
KARU_API void karu_client_free(karu_client* client);

// Compare a client's immutable snapshot with a builder after freezing it.
// This performs no I/O and allows callers to reuse transport connections
// without overlooking configuration changes between operations.
KARU_API karu_status karu_client_matches_config(const karu_client* client,
                                                const karu_config* config, int* out_match);

KARU_API int karu_client_concurrency(const karu_client* client);
KARU_API uint64_t karu_client_coalesce_gap(const karu_client* client);
KARU_API int karu_client_max_attempts(const karu_client* client);

// Locators -----------------------------------------------------------------

typedef struct karu_locator karu_locator;

// Supported terminal forms: https://, http://, s3://, gs://, az://, hf://,
// source://, file:// and bare paths. VSI forms are /vsicurl/, /vsis3/,
// /vsigs/, /vsiaz/, /vsiadls/, /vsihf/, /vsisource/ and /vsisubfile/. Karu
// intentionally does not accept GDAL's *_streaming spellings: every Karu
// remote read is stateless.
KARU_API karu_status karu_resolve(const char* uri, karu_locator** out);
KARU_API void karu_locator_free(karu_locator* locator);
KARU_API const char* karu_locator_uri(const karu_locator* locator);
// Nonzero for HTTP and object-storage locators, zero for local files and NULL.
KARU_API int karu_locator_is_remote(const karu_locator* locator);
KARU_API uint64_t karu_locator_window_offset(const karu_locator* locator);
KARU_API uint64_t karu_locator_window_length(const karu_locator* locator);

// Metadata is an explicit operation and is never cached. A fixed
// /vsisubfile/ window can answer locally.
KARU_API karu_status karu_client_size(karu_client* client, const karu_locator* locator,
                                      uint64_t* out_size);

// Positional reads ---------------------------------------------------------

typedef struct {
    const karu_locator* locator;
    uint64_t offset;
    uint64_t length;
    // Caller-owned destination. NULL asks karu_client_submit() to allocate;
    // the returned completion buffer must then be released with karu_free().
    void* buffer;
    void* tag;
    // Optional remote-object ETag. The request fails if the object has
    // changed; local files reject this precondition. Copied by submit, so the
    // string only needs to live until that call returns.
    const char* if_match;
} karu_req;

typedef struct {
    void* tag;
    karu_status status;
    uint64_t got;
    // The request destination, or an allocated buffer owned by the caller.
    void* buffer;
} karu_done;

typedef struct karu_batch karu_batch;

// Coalescing is scoped to this submission only. A zero KARU_COALESCE_GAP
// disables it, including for adjacent ranges.
// The request array, locators, and if_match strings are copied. Caller-owned
// destination buffers must remain alive until the batch is freed.
KARU_API karu_status karu_client_submit(karu_client* client, const karu_req* requests, size_t count,
                                        karu_batch** out_batch);
// KARU_OK means a completion was produced; inspect out->status for that
// request's result. KARU_END and KARU_TIMEOUT do not populate out.
KARU_API karu_status karu_batch_next(karu_batch* batch, karu_done* out, int timeout_ms);
// Cancels pending work and waits until no worker can touch request buffers.
KARU_API void karu_batch_free(karu_batch* batch);
// Blocking convenience for caller-owned buffers. NULL request buffers are
// rejected because this function has no completion through which to return
// an allocated buffer.
KARU_API karu_status karu_client_fetch(karu_client* client, const karu_req* requests, size_t count);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // KARU_H
