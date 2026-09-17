# Credentials

Sources: `src/request_builder.cpp`, `src/credential_cache.cpp`,
`src/backends/s3_credentials.cpp`, `src/backends/aws_profile.cpp`,
`src/backends/source_credentials.cpp`, `src/backends/gcs_credentials.cpp`,
`src/backends/azure_credentials.cpp`, `src/backends/{s3,gcs,azure,source,hugging_face}.cpp`,
`src/process.cpp`, `CONFIGURATION.md`, `tests/test_credentials_*.cpp`.

## Contents

1. When credentials are resolved
2. Signed or anonymous
3. AWS S3
4. Source Cooperative
5. Google Cloud Storage
6. Azure Storage
7. Hugging Face
8. Custom providers
9. Cache, refresh and invalidation
10. Not supported natively

## 1. When credentials are resolved

- Never at `karu_resolve` or `karu_client_create`. Cloud transfers (S3, GCS, Azure,
  Source) first visit a credential worker, which resolves credentials for the transfer's
  canonical path and then hands it to the I/O thread. Size probes resolve on the calling
  thread.
- Plain HTTP needs no credentials. Hugging Face reads its token while each request
  attempt is built, without a cache.
- The request is signed after the byte range and region are known, again for every
  attempt, so renewed credentials and corrected regions apply on the next try.
- A failed lookup completes every read of that transfer with the loader's status:
  usually `KARU_ERR_CREDENTIALS`, `KARU_ERR_CONFIG` for contradictory settings, or
  `KARU_TIMEOUT` for a `credential_process` that ran out of time.
- `KARU_REQUEST_TIMEOUT` starts before the lookup. Network lookups carry their own short
  timeouts (1 to 5 seconds to connect); only `credential_process` is killed by the
  request timeout.

## 2. Signed or anonymous

For each transfer (`RequestBuilder::resolve_credentials`):

1. If `<PROVIDER>_NO_SIGN_REQUEST` is set for the path, its value decides.
2. Otherwise S3, GCS and Azure sign. Source signs only when a Source callback is
   installed or any of `SOURCE_ACCESS_KEY_ID`, `SOURCE_SECRET_ACCESS_KEY`,
   `SOURCE_SESSION_TOKEN`, `SOURCE_PROFILE`, `SOURCE_SHARED_CREDENTIALS_FILE`,
   `SOURCE_CONFIG_FILE` is set.
3. A signing transfer uses the custom provider for its kind when one is installed, and
   the native chain otherwise. The callback replaces the chain; it is not a fallback.
4. An empty native result is not an error by itself: the backend then fails with its
   own message, for example
   `/vsis3/bucket/key: no AWS credentials; set AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY, install a custom provider, or set AWS_NO_SIGN_REQUEST=YES for a public object`.

## 3. AWS S3

Native chain, first match wins:

1. `AWS_ACCESS_KEY_ID` with `AWS_SECRET_ACCESS_KEY`, optional `AWS_SESSION_TOKEN`. One
   without the other fails.
2. The profile `AWS_PROFILE` (default `default`) is read from `AWS_CONFIG_FILE` (default
   `~/.aws/config`, section `[profile NAME]` or `[default]`) and
   `AWS_SHARED_CREDENTIALS_FILE` (default `~/.aws/credentials`, section `[NAME]`, whose
   values win). A profile named explicitly but absent fails
   (`AWS profile 'x' was not found`), and so does an explicitly named file that does not
   exist.
3. Web identity: `AWS_ROLE_ARN` with `AWS_WEB_IDENTITY_TOKEN_FILE`, or `role_arn` with
   `web_identity_token_file` in the profile, calls `AssumeRoleWithWebIdentity` at
   `AWS_STS_ENDPOINT` (default `https://sts.amazonaws.com/`). The session name is
   `AWS_ROLE_SESSION_NAME`, the profile's `role_session_name`, or `karu-<unix time>`.
4. A role without a token file (`source_profile` chains) and IAM Identity Center profiles
   (`sso_session`, `sso_start_url`) fail with a message asking for a custom provider.
5. `credential_process` in the profile runs through `/bin/sh -c` (POSIX, spawned in its
   own process group) or `%COMSPEC% /D /S /C` (Windows, inside a job object). It is
   killed when `KARU_REQUEST_TIMEOUT` expires (status `KARU_TIMEOUT`), its output is
   capped at 1 MiB, and it must print `AccessKeyId`, `SecretAccessKey`, optional
   `SessionToken` or `Token`, and optional `Expiration`.
6. Static keys in the profile: `aws_access_key_id`, `aws_secret_access_key`,
   `aws_session_token`.
7. Container credentials: `AWS_CONTAINER_CREDENTIALS_FULL_URI` (HTTPS anywhere, HTTP only
   to `127.0.0.1`, `::1`, `localhost` or `169.254.170.2`) or
   `AWS_CONTAINER_CREDENTIALS_RELATIVE_URI` (appended to `http://169.254.170.2`), with
   `AWS_CONTAINER_AUTHORIZATION_TOKEN` or `AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE`.
   The response must contain a key, secret, token and a future expiration.
8. IMDSv2 at `169.254.169.254`: token, role name, credentials, each step bounded by
   `AWS_METADATA_SERVICE_TIMEOUT` (default 1 second). It is tried only with environment
   discovery (`karu_config_create`) or an explicit `AWS_EC2_METADATA_DISABLED`, and
   `AWS_EC2_METADATA_DISABLED=YES` skips it. There is no endpoint override. Off EC2 the
   probe fails silently after about a second, per lookup.

Request details:

- Region: `AWS_REGION` (alias `AWS_DEFAULT_REGION`), else the `region` of the profile
  that supplied the credentials, else `us-east-1`. When S3 answers with another region
  (`x-amz-bucket-region` header or `<Region>` in the error body), Karu signs again for
  that region once per transfer, outside the attempt budget, and remembers it for that
  object for the rest of the batch.
- Endpoint: `AWS_S3_ENDPOINT` (aliases `AWS_ENDPOINT_URL_S3`, `AWS_ENDPOINT_URL`) is a
  service root. The default is `s3.<region>.amazonaws.com` with virtual hosting
  (`bucket.s3.<region>.amazonaws.com`), except that buckets containing a dot use path
  style so the wildcard certificate still matches. Custom endpoints default to path style
  (`AWS_VIRTUAL_HOSTING=NO`); MinIO and most S3-compatible stores want that.
- `AWS_REQUEST_PAYER=requester` adds a signed `x-amz-request-payer` header.
- Signing is SigV4 with `UNSIGNED-PAYLOAD`; it covers host, range, `x-amz-content-sha256`,
  `x-amz-date`, and when present `if-match`, `x-amz-security-token` and
  `x-amz-request-payer`. The clock of the machine is used, so a skewed clock is rejected
  by S3.
- Signed requests never follow redirects; anonymous ones do.

## 4. Source Cooperative

- Anonymous until configured (section 2). Karu never forwards `AWS_*` credentials to the
  Source data proxy.
- Chain when signing: `SOURCE_ACCESS_KEY_ID` with `SOURCE_SECRET_ACCESS_KEY` (optional
  `SOURCE_SESSION_TOKEN`), then the profile `SOURCE_PROFILE` (default `source-coop`)
  from `SOURCE_CONFIG_FILE` and `SOURCE_SHARED_CREDENTIALS_FILE` (defaults
  `~/.aws/config` and `~/.aws/credentials`). The profile must exist and yield keys,
  through `credential_process` or static keys; role and SSO profiles are refused.
- The Source CLI writes a suitable profile:

```ini
[profile source-coop]
credential_process = source-coop creds
endpoint_url = https://data.source.coop
```

  Run `source-coop login`, then set `SOURCE_PROFILE=source-coop` (or
  `SOURCE_NO_SIGN_REQUEST=NO`). The profile's `endpoint_url` is ignored.
- Region: `SOURCE_REGION`, else the profile region, else `us-east-1`. Endpoint:
  `SOURCE_ENDPOINT` (alias `SOURCE_PROXY_URL`, which is not an HTTP proxy), default
  `https://data.source.coop`, always path style.
- Callbacks use `KARU_CREDENTIALS_SOURCE`, and cache prefixes start with `/vsisource/`.

## 5. Google Cloud Storage

Native chain, first match wins:

1. `GCS_ACCESS_TOKEN`, sent as a bearer token.
2. `GCS_HMAC_ACCESS_KEY_ID` with `GCS_HMAC_SECRET_ACCESS_KEY`, sent as a `GOOG1` HMAC-SHA1
   signature with a `Date` header.
3. `GCS_REFRESH_TOKEN` with `GCS_CLIENT_ID` and `GCS_CLIENT_SECRET` (authorized user).
4. An ADC JSON file: `GOOGLE_APPLICATION_CREDENTIALS`, else
   `CLOUDSDK_CONFIG/application_default_credentials.json`, else (discovery only)
   `~/.config/gcloud/application_default_credentials.json`
   (`%APPDATA%\gcloud\application_default_credentials.json` on Windows). Supported
   `type` values: `service_account` (signed JWT), `authorized_user` (refresh token), and
   `external_account` with a file or URL subject token, optional
   `subject_token_field_name`, STS exchange and optional
   `service_account_impersonation_url`. Other types fail.
5. `GCS_PRIVATE_KEY` or `GCS_PRIVATE_KEY_FILE` (one of them) with `GCS_CLIENT_EMAIL`.
6. The metadata server at `GCS_METADATA_ENDPOINT` (default
   `http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/token`),
   with a 1 second connect timeout, tried with discovery or an explicit metadata option.
   `GCS_METADATA_DISABLED=YES` skips it. Its errors surface only when the endpoint was set
   explicitly.

- OAuth scope: `GCS_SCOPE`, default `https://www.googleapis.com/auth/devstorage.read_only`.
- Token lifetime comes from `expires_in` (default 3600 seconds, at least 60).
- `GCS_USER_PROJECT` adds `x-goog-user-project` for requester-pays buckets, signed or not.
- `GCS_ENDPOINT` (default `https://storage.googleapis.com`) is the XML API root; requests go
  to `<endpoint>/bucket/key`.

## 6. Azure Storage

Credential chain (`load_azure_credentials`), first match wins:

1. `AZURE_STORAGE_CONNECTION_STRING` set: use it together with the explicit options.
   `AccountName`, `AccountKey`, `SharedAccessSignature`, `BlobEndpoint`, `DfsEndpoint`,
   `EndpointSuffix` and `DefaultEndpointsProtocol` are honored.
2. `AZURE_STORAGE_SAS_TOKEN`, `AZURE_STORAGE_ACCESS_TOKEN` or `AZURE_STORAGE_ACCESS_KEY`.
3. Service principal or workload identity: `AZURE_TENANT_ID` and `AZURE_CLIENT_ID` with
   `AZURE_CLIENT_SECRET` or `AZURE_FEDERATED_TOKEN_FILE`, exchanged at
   `<AZURE_AUTHORITY_HOST>/<tenant>/oauth2/v2.0/token` (default authority
   `https://login.microsoftonline.com`) for `AZURE_STORAGE_SCOPE`
   (default `https://storage.azure.com/.default`).
4. App Service identity: `IDENTITY_ENDPOINT` with optional `IDENTITY_HEADER`.
5. Managed identity at `IMDS_ENDPOINT` (default
   `http://169.254.169.254/metadata/identity/oauth2/token`), tried with discovery or an
   explicit `IMDS_ENDPOINT`, for `AZURE_STORAGE_RESOURCE`
   (default `https://storage.azure.com/`).

- Managed identity selectors, at most one: `AZURE_IMDS_OBJECT_ID`, `AZURE_IMDS_CLIENT_ID`
  (defaults to `AZURE_CLIENT_ID`), `AZURE_IMDS_MSI_RES_ID`.
- Request authentication, in this order: `AZURE_NO_SIGN_REQUEST=YES`, a SAS appended to the
  query, a bearer token with `x-ms-version: 2023-11-03`, then Shared Key with
  `AZURE_STORAGE_ACCOUNT` and the base64 `AZURE_STORAGE_ACCESS_KEY` (the signature covers
  `If-Match`, `Range`, `x-ms-date` and `x-ms-version`).
- Endpoint: `AZURE_STORAGE_ENDPOINT`, else the connection string's `BlobEndpoint` or
  `DfsEndpoint`, else `https://<account>.blob.<suffix>` (`.dfs.` for `abfs://` and
  `/vsiadls/`), with suffix `core.windows.net` by default. Without an account or endpoint
  every read fails with `Azure needs AZURE_STORAGE_ACCOUNT or an explicit endpoint`,
  anonymous reads included.
- Azurite: `AZURE_STORAGE_ENDPOINT=http://127.0.0.1:10000/devstoreaccount1`,
  `AZURE_STORAGE_ACCOUNT=devstoreaccount1` and the documented development key. The
  account then appears twice in the Shared Key canonical resource, which Karu handles.

## 7. Hugging Face

- Token: `HF_TOKEN` (alias `HUGGING_FACE_HUB_TOKEN`), else the file `HF_TOKEN_PATH`, else
  `$HF_HOME/token`, else, with discovery only,
  `${XDG_CACHE_HOME:-~/.cache}/huggingface/token` (the file `hf auth login` writes).
- A missing token file means anonymous access. An unreadable one fails with
  `KARU_ERR_CREDENTIALS`. Surrounding whitespace is trimmed.
- The file is read for every attempt, so logging in or rotating the token affects the next
  request without a new client.
- The token is sent as `Authorization: Bearer` to `HF_ENDPOINT`. Redirects to the CDN are
  followed and libcurl (7.83.0 or newer, which Karu requires) drops the header for the
  other host.

## 8. Custom providers

`karu_config_set_credentials_provider(config, kind, provider, user_data, release)`
(C++ `Config::set_credentials_provider`). Signature, field use per kind and a complete
example are in `c-api.md`, section 9.

- Use one when the credential source is an SDK feature Karu does not implement: chained
  AssumeRole, IAM Identity Center, a secret manager, Azure CLI or environment-specific
  external accounts.
- The callback receives the canonical path (`/vsis3/bucket/key`). Return the narrowest
  `cache_prefix` for which the value is valid (`/vsis3/bucket/`), or none when the SDK
  already caches.
- A prefix that does not contain the requested path fails with
  `custom credential cache_prefix does not contain the requested path`.
- Values whose `expires_at` has passed fail with
  `custom credential provider returned expired credentials`.
- Callbacks for different paths may run at the same time; simultaneous refreshes of the
  same path share one call.

## 9. Cache, refresh and invalidation

- One cache per client engine. A forked child starts with an empty one.
- Native credentials are keyed by kind plus the origin of every credential option for
  the path: which path rule, the explicit layer, the environment, or the default. Paths
  whose credential options come from the same places share one entry, so a path rule
  that sets `AWS_PROFILE` for `s3://lab/` gives that prefix its own scope.
- Custom credentials are cached only under the `cache_prefix` they return; the longest
  matching prefix is reused.
- Refresh is lazy. A value with an expiry becomes stale during the last tenth of its
  lifetime, with that margin clamped to 1 to 60 seconds; the first request in that window
  reloads while other requests for the same key wait for it.
- Native values without an expiry (static keys, profiles, `credential_process` output
  without `Expiration`) are reloaded after 60 seconds, so edited profiles show up. Custom
  values with `expires_at == 0` stay until invalidated or the client is destroyed.
- Failed or empty lookups are never cached.
- Invalidation: HTTP 401 from GCS, Azure or Source, or HTTP 403 with `ExpiredToken`,
  `InvalidToken` or `RequestExpired` from S3 or Source, drops the entry and resolves
  again once per transfer, outside the attempt budget. A second rejection is final
  (`KARU_ERR_AUTH`).

## 10. Not supported natively

AWS chained AssumeRole and IAM Identity Center, Azure CLI and developer tool logins,
gcloud user logins other than ADC files, browser logins and OS keyrings. Provide these
through a custom provider backed by the vendor SDK.
