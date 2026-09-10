# Configuration reference

`karu_config_create()` snapshots the supported environment variables and the
OS home and cache directories used for default credential discovery. Calls to
`karu_config_set_option()` override that snapshot. Calls to
`karu_config_set_path_option()` override both for the longest matching VSI
prefix. A client copies the result and never reads the environment again.
`karu_config_create_empty()` starts without environment values or implicit
home-directory or metadata-service credential discovery; it is useful for
hermetic clients and tests. Explicit credential files, metadata endpoints, or
the corresponding `..._DISABLED=NO` options still opt back in.

Names are case-insensitive. Unknown names are rejected instead of being
silently ignored.

Aliases are canonicalized before precedence is evaluated. In particular,
`AWS_DEFAULT_PROFILE`, `AWS_DEFAULT_REGION`, `AWS_ENDPOINT_URL_S3`,
`AWS_ENDPOINT_URL`, and `HUGGING_FACE_HUB_TOKEN` cannot bypass a more specific
path option written under their canonical name. `SOURCE_PROXY_URL` is an alias
for `SOURCE_ENDPOINT`. `CURL_CA_BUNDLE` and `SSL_CERT_FILE` are aliases for
`KARU_HTTP_CA_BUNDLE`.

## Runtime

| Option | Default | Purpose |
|---|---:|---|
| `KARU_CONCURRENCY` | `64` | maximum simultaneous transfers |
| `KARU_COALESCE_GAP` | `1048576` | largest gap, in bytes, considered for merging ranges; `0` disables merging |
| `KARU_COALESCE_LIMIT` | `67108864` | largest merged transfer span |
| `KARU_COALESCE_PARTS` | `1024` | largest number of requests in one merged transfer |
| `KARU_COALESCE_AMPLIFICATION` | `16` | largest ratio of transferred bytes to requested bytes in a merge |
| `KARU_RANGE_FALLBACK_LIMIT` | `8388608` | largest prefix Karu may discard when a server ignores `Range`; `0` disables nonzero-offset fallback |
| `KARU_MAX_ATTEMPTS` / `KARU_MAX_RETRIES` | `3` | total attempts for transient failures |
| `KARU_REQUEST_TIMEOUT` | `120` | total seconds available to one remote transfer, including retry delays; `0` disables the deadline |
| `KARU_CONNECT_TIMEOUT` | `30` | connection timeout in seconds |
| `KARU_LOW_SPEED_TIME` | `60` | seconds below the low-speed threshold before aborting |
| `KARU_LOW_SPEED_LIMIT` | `1024` | low-speed threshold in bytes per second |

The coalescing ceilings split a sparse batch into bounded transfers. They do
not persist object data or metadata beyond that batch.

## AWS S3

| Option | Purpose |
|---|---|
| `AWS_ACCESS_KEY_ID` | static access key ID |
| `AWS_SECRET_ACCESS_KEY` | static secret; must accompany the access key ID |
| `AWS_SESSION_TOKEN` | optional temporary session token |
| `AWS_PROFILE` / `AWS_DEFAULT_PROFILE` | shared profile name |
| `AWS_SHARED_CREDENTIALS_FILE` | credentials file path |
| `AWS_CONFIG_FILE` | config file path |
| `AWS_REGION` / `AWS_DEFAULT_REGION` | SigV4 region; default `us-east-1` |
| `AWS_S3_ENDPOINT` | service-root host or URL |
| `AWS_ENDPOINT_URL_S3` / `AWS_ENDPOINT_URL` | aliases for `AWS_S3_ENDPOINT` |
| `AWS_HTTPS` | add `https://` to a host-only endpoint; default `YES` |
| `AWS_VIRTUAL_HOSTING` | put the bucket before the host; defaults to `YES` for AWS and `NO` for a custom endpoint |
| `AWS_REQUEST_PAYER` | `requester` for requester-pays buckets |
| `AWS_NO_SIGN_REQUEST` | explicit anonymous access |
| `AWS_ROLE_ARN` | role used with web identity |
| `AWS_WEB_IDENTITY_TOKEN_FILE` | OIDC token file |
| `AWS_ROLE_SESSION_NAME` | optional web-identity session name |
| `AWS_STS_ENDPOINT` | web-identity STS endpoint |
| `AWS_CONTAINER_CREDENTIALS_RELATIVE_URI` | ECS credential endpoint path |
| `AWS_CONTAINER_CREDENTIALS_FULL_URI` | HTTPS, loopback, or ECS credential endpoint |
| `AWS_CONTAINER_AUTHORIZATION_TOKEN` | ECS credential endpoint authorization |
| `AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE` | file containing that authorization value |
| `AWS_EC2_METADATA_DISABLED` | disable IMDS discovery |
| `AWS_METADATA_SERVICE_TIMEOUT` | IMDS connect timeout in seconds, `1`–`60` |

Static profiles, `credential_process`, and web-identity profiles are handled
natively. Chained source-profile AssumeRole and IAM Identity Center should be
provided by the application's AWS SDK through the callback API.

`AWS_S3_ENDPOINT` is a service root. With virtual hosting enabled, an endpoint
of `https://objects.example.test` becomes
`https://bucket.objects.example.test/key`. With virtual hosting disabled it
becomes `https://objects.example.test/bucket/key`. Buckets containing a dot
use path-style addressing by default so that AWS's wildcard TLS certificate
still matches. An explicit `AWS_VIRTUAL_HOSTING=YES` overrides that choice.

## Source Cooperative

`source://account/product/key` and `/vsisource/account/product/key` resolve to
the [Source Data Proxy](https://docs.source.coop/data-proxy). Public reads need
no configuration and remain unsigned. Source credentials use their own
namespace and callback kind, so Karu never forwards ambient AWS credentials to
the proxy.

| Option | Purpose |
|---|---|
| `SOURCE_PROFILE` | AWS-format profile; defaults to `source-coop` when signed access is requested |
| `SOURCE_CONFIG_FILE` | AWS-format config file; default `~/.aws/config` |
| `SOURCE_SHARED_CREDENTIALS_FILE` | AWS-format credentials file; default `~/.aws/credentials` |
| `SOURCE_ACCESS_KEY_ID` | explicit Source access key ID |
| `SOURCE_SECRET_ACCESS_KEY` | explicit secret; must accompany the access key ID |
| `SOURCE_SESSION_TOKEN` | optional temporary session token |
| `SOURCE_REGION` | SigV4 region; defaults to the profile region, then `us-east-1` |
| `SOURCE_ENDPOINT` / `SOURCE_PROXY_URL` | proxy service root; default `https://data.source.coop` |
| `SOURCE_NO_SIGN_REQUEST` | force (`YES`) or disable (`NO`) anonymous access |

`SOURCE_PROXY_URL` is a compatibility spelling for the Source data-service
endpoint. It is not an HTTP forward-proxy setting; use `KARU_HTTP_PROXY` for
that.

Installing a Source credential option or a
`KARU_CREDENTIALS_SOURCE` callback enables signed requests automatically.
`SOURCE_NO_SIGN_REQUEST=YES` always wins; `SOURCE_NO_SIGN_REQUEST=NO` requests
credentials explicitly and uses the `source-coop` profile when no profile is
named.

The profile created for the
[Source CLI](https://github.com/source-cooperative/source-coop-cli) uses
`credential_process`:

```ini
[profile source-coop]
credential_process = source-coop creds
endpoint_url = https://data.source.coop
```

Run `source-coop login`, then set `SOURCE_PROFILE=source-coop` in Karu. Karu
executes the profile's credential process and refreshes its temporary result
when needed; it does not implement the browser login or read the CLI keyring.
The profile's `endpoint_url` is intentionally ignored because
`SOURCE_ENDPOINT` owns endpoint selection for `/vsisource/` paths.

## Google Cloud Storage

| Option | Purpose |
|---|---|
| `GCS_ACCESS_TOKEN` | static bearer token |
| `GCS_HMAC_ACCESS_KEY_ID` | interoperable HMAC access ID |
| `GCS_HMAC_SECRET_ACCESS_KEY` | interoperable HMAC secret |
| `GOOGLE_APPLICATION_CREDENTIALS` | ADC JSON path |
| `CLOUDSDK_CONFIG` | directory containing gcloud's ADC JSON |
| `GCS_REFRESH_TOKEN` | authorized-user refresh token |
| `GCS_CLIENT_ID` | authorized-user client ID |
| `GCS_CLIENT_SECRET` | authorized-user client secret |
| `GCS_PRIVATE_KEY` | inline service-account PEM key |
| `GCS_PRIVATE_KEY_FILE` | PEM key file; alternative to the inline key |
| `GCS_CLIENT_EMAIL` | service-account email used with a PEM key |
| `GCS_SCOPE` | OAuth scope; defaults to read-only object access |
| `GCS_USER_PROJECT` | requester-pays billing project header |
| `GCS_ENDPOINT` | XML API service-root URL |
| `GCS_METADATA_ENDPOINT` | metadata token endpoint |
| `GCS_METADATA_DISABLED` | disable metadata discovery |
| `GCS_NO_SIGN_REQUEST` | explicit anonymous access |

ADC supports `service_account`, `authorized_user`, and OIDC
`external_account` sources backed by a file or a simple URL. Service-account
impersonation is supported. Environment-specific external-account suppliers
can use the callback API.

With environment discovery enabled, metadata credentials are attempted after
ADC files. An explicit `GCS_METADATA_ENDPOINT` also enables that source for an
otherwise empty configuration. `GCS_METADATA_DISABLED=YES` always disables it.

## Azure Storage

| Option | Purpose |
|---|---|
| `AZURE_STORAGE_CONNECTION_STRING` | account, endpoint, Shared Key, or SAS connection string |
| `AZURE_STORAGE_ACCOUNT` | storage account name |
| `AZURE_STORAGE_ACCESS_KEY` | base64 Shared Key |
| `AZURE_STORAGE_SAS_TOKEN` | encoded SAS query string |
| `AZURE_STORAGE_ACCESS_TOKEN` | static bearer token |
| `AZURE_STORAGE_ENDPOINT` | Blob or DFS service-root URL |
| `AZURE_TENANT_ID` | Microsoft Entra tenant |
| `AZURE_CLIENT_ID` | application or managed-identity client ID |
| `AZURE_CLIENT_SECRET` | client-secret credential |
| `AZURE_FEDERATED_TOKEN_FILE` | workload-identity assertion |
| `AZURE_AUTHORITY_HOST` | authority root for public or sovereign clouds |
| `AZURE_STORAGE_SCOPE` | OAuth v2 scope; default `https://storage.azure.com/.default` |
| `AZURE_STORAGE_RESOURCE` | managed-identity resource; default `https://storage.azure.com/` |
| `AZURE_IMDS_OBJECT_ID` | select a managed identity by object ID |
| `AZURE_IMDS_CLIENT_ID` | select a managed identity by client ID |
| `AZURE_IMDS_MSI_RES_ID` | select a managed identity by Azure resource ID |
| `IDENTITY_ENDPOINT` / `IDENTITY_HEADER` | App Service managed identity |
| `IMDS_ENDPOINT` | managed-identity endpoint override |
| `AZURE_NO_SIGN_REQUEST` | explicit anonymous access |

Connection-string `DefaultEndpointsProtocol`, `EndpointSuffix`, `BlobEndpoint`,
and `DfsEndpoint` values are honored. `AZURE_AUTHORITY_HOST`,
`AZURE_STORAGE_SCOPE`, and
`AZURE_STORAGE_RESOURCE` keep identity and storage endpoints explicit for
sovereign clouds. `AZURE_STORAGE_ENDPOINT` is a convenience option; the
standard endpoint fields in `AZURE_STORAGE_CONNECTION_STRING` are also
supported.

## HTTP and Hugging Face

`KARU_HTTP_HEADERS` accepts newline-separated `Name: value` entries. HTTP and
Hugging Face requests may follow HTTP(S) redirects; authenticated cloud
requests return a redirect as an error so that credentials and signatures are
never replayed against a different request target.

| Option | Purpose |
|---|---|
| `KARU_HTTP_HEADERS` | newline-separated request headers |
| `KARU_HTTP_VERSION` | `1.1` (default), `2TLS`/`2`, `2PRIOR_KNOWLEDGE`, or `AUTO` |
| `KARU_HTTP_CA_BUNDLE` / `CURL_CA_BUNDLE` / `SSL_CERT_FILE` | explicit CA bundle passed to libcurl |
| `KARU_HTTP_CA_PATH` | directory containing CA certificates |
| `KARU_HTTP_PROXY` | explicit HTTP proxy; libcurl proxy environment variables also work |
| `KARU_HTTP_PROXY_CREDENTIALS` | proxy credentials in `user:password` form |
| `KARU_HTTP_USER_AGENT` | explicit User-Agent value |
| `HF_TOKEN` / `HUGGING_FACE_HUB_TOKEN` | bearer token; takes precedence over token files |
| `HF_TOKEN_PATH` | path to the token written by Hugging Face tooling |
| `HF_HOME` | Hugging Face state directory; the token is read from `<HF_HOME>/token` |
| `HF_ENDPOINT` | Hub service-root URL |

With environment discovery enabled, Karu follows the Hugging Face CLI's
default token location: `$HF_TOKEN_PATH`, then `$HF_HOME/token`, then
`${XDG_CACHE_HOME:-~/.cache}/huggingface/token`. A missing token file means
anonymous access. `karu_config_create_empty()` does not inspect a default
cache, but explicitly setting `HF_TOKEN_PATH` or `HF_HOME` opts into that one
file.

The token file is read while each request attempt is materialized. Logging in
or rotating the file therefore affects the next operation without putting a
credential or cache inside an object. Karu trims surrounding whitespace and
reports an existing but unreadable token file as a credential error.

`Range` and `Host` are always owned by Karu. Cloud and Hugging Face requests
also reserve authentication and provider-signature headers. Use the dedicated
credential options or callback instead of injecting those headers through
`KARU_HTTP_HEADERS`.

Object transfers and their redirects are restricted to HTTP and HTTPS.
On Linux, Karu uses the system CA bundle available at runtime when none of the
CA options above is set. This keeps binaries built in one Linux distribution
from retaining that build machine's certificate path.

## Custom providers

`karu_config_set_credentials_provider()` installs one callback for AWS, GCS,
Azure, or Source Cooperative. The callback receives the canonical VSI path.
It returns strings that Karu copies immediately, an optional Unix expiry, and
an optional canonical `cache_prefix`.

Use a prefix only when the credential is valid for every object beneath it.
For example, `/vsis3/team-bucket/` shares one renewable value across that
bucket. Without a prefix Karu calls the provider for every request, which is
appropriate when the provider or vendor SDK owns its own cache.

Source uses `KARU_CREDENTIALS_SOURCE` and the same access-key, secret, and
session-token fields as AWS, but its cache namespace is separate. A Source
prefix therefore starts with `/vsisource/`, not `/vsis3/`.

Karu coalesces simultaneous callback refreshes for the same path. Callbacks for
different paths may run concurrently, so the callback and its `user_data` must
be thread-safe.
Expiring credentials refresh near the end of their lifetime rather than at a
fixed 60-second boundary. Native static credentials are rechecked after one
minute so profile rotation remains visible; callback values with
`expires_at == 0` remain valid until the callback's documented lifetime.
These operational values do not cache object contents or metadata.

The callback must not re-enter that same client; it should obtain credentials
from the vendor SDK or identity service and return them directly.
