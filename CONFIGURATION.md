# Configuration reference

Karu reads its configuration once, when a client is created. The client keeps
that copy for as long as it lives, so changing the environment afterwards does
not affect it.

[`karu_config_create()`](https://asterisk.coop/karu/api.html#config) picks up the environment variables listed below, plus
the home and cache directories Karu uses to find credential files. Empty
variables are ignored. [`karu_config_create_empty()`](https://asterisk.coop/karu/api.html#config) starts from nothing, with no
environment, no default credential files, no metadata services, and no system
CA bundle lookup. Use it for hermetic clients and tests. Setting a credential
file, a metadata endpoint, or a `..._DISABLED=NO` option on it turns that one
source back on.

On top of that, options can be set for the whole client with
[`karu_config_set_option()`](https://asterisk.coop/karu/api.html#config), or for one path prefix with
[`karu_config_set_path_option()`](https://asterisk.coop/karu/api.html#config). For each request Karu takes the first value it
finds:

1. the option set for the longest prefix that matches the path,
2. the option set for the whole client,
3. the environment,
4. the built-in default.

Setting an option to `NULL` removes the value set at that level, and the next
value in this order applies again.

A prefix covers the named resource and everything below it, and it only
matches whole path segments, so `/vsis3/team/private` covers
`/vsis3/team/private/file` but not `/vsis3/team/private-copy/file`. `KARU_*`
options apply to the whole client and cannot be set per path.

Option names are case-insensitive, and Karu rejects names it does not know
instead of ignoring them. Aliases are mapped to one name before any of this
happens, so an alias can never override a path option written under the main
name:

- `AWS_DEFAULT_PROFILE` → `AWS_PROFILE`
- `AWS_DEFAULT_REGION` → `AWS_REGION`
- `AWS_ENDPOINT_URL_S3` and `AWS_ENDPOINT_URL` → `AWS_S3_ENDPOINT`
- `KARU_MAX_RETRIES` → `KARU_MAX_ATTEMPTS`
- `HUGGING_FACE_HUB_TOKEN` → `HF_TOKEN`
- `SOURCE_PROXY_URL` → `SOURCE_ENDPOINT`
- `CURL_CA_BUNDLE` and `SSL_CERT_FILE` → `KARU_HTTP_CA_BUNDLE`

If the environment sets more than one spelling, the one listed first in the
tables below wins.

## Runtime

| Option | Default | Purpose |
|---|---:|---|
| `KARU_CONCURRENCY` | `64` | most remote transfers in flight at once, per client |
| `KARU_IO_THREADS` | `4` | libcurl event loops per client, each on its own thread; `KARU_CONCURRENCY` is split between them |
| `KARU_COALESCE_GAP` | `1048576` | largest hole, in bytes, that Karu will read through to merge two ranges; `0` turns merging off |
| `KARU_COALESCE_LIMIT` | `67108864` | largest span of one merged transfer, in bytes |
| `KARU_COALESCE_PARTS` | `1024` | most reads in one merged transfer |
| `KARU_COALESCE_AMPLIFICATION` | `16` | most bytes a merged transfer may fetch per requested byte |
| `KARU_RANGE_FALLBACK_LIMIT` | `8388608` | when a server ignores `Range`, the most bytes Karu will skip to reach the requested offset; `0` means only reads at offset 0 can fall back |
| `KARU_MAX_ATTEMPTS` / `KARU_MAX_RETRIES` | `8` | attempts per transfer for [transient failures](https://asterisk.coop/karu/how-karu-reads.html#recover), within the request timeout; DNS and connection failures stop after 3 |
| `KARU_REQUEST_TIMEOUT` | `120` | seconds one remote transfer may take, retries included, and the limit for each `credential_process`; `0` removes both limits |
| `KARU_CONNECT_TIMEOUT` | `30` | seconds to establish a connection |
| `KARU_LOW_SPEED_TIME` | `60` | seconds a transfer may stay below `KARU_LOW_SPEED_LIMIT` before Karu aborts it |
| `KARU_LOW_SPEED_LIMIT` | `1024` | the slow-transfer threshold, in bytes per second |

These limits are per client. With several worker processes, try both
`KARU_IO_THREADS=1` and `4` on the real workload, including decoding, and keep
the faster one. [One client, tuned from measurements](https://asterisk.coop/karu/how-karu-reads.html#operate)
explains what to measure.

The four `KARU_COALESCE_*` values only decide
[how one batch is split into transfers](https://asterisk.coop/karu/how-karu-reads.html#plan). Nothing from a batch is kept after it ends, and a single batch can override
the four values, as the [API reference](https://asterisk.coop/karu/api.html#reads) describes. Local files only merge ranges that overlap or
touch, so the gap applies to remote reads only.

## AWS S3

| Option | Purpose |
|---|---|
| `AWS_ACCESS_KEY_ID` | static access key ID |
| `AWS_SECRET_ACCESS_KEY` | static secret; set it together with the key ID |
| `AWS_SESSION_TOKEN` | optional session token for temporary keys |
| `AWS_PROFILE` / `AWS_DEFAULT_PROFILE` | profile name; default `default` |
| `AWS_SHARED_CREDENTIALS_FILE` | credentials file; default `~/.aws/credentials` |
| `AWS_CONFIG_FILE` | config file; default `~/.aws/config` |
| `AWS_REGION` / `AWS_DEFAULT_REGION` | signing region; otherwise the region of the profile that supplied the credentials, then `us-east-1` |
| `AWS_S3_ENDPOINT` | service root, as a host or a URL |
| `AWS_ENDPOINT_URL_S3` / `AWS_ENDPOINT_URL` | other spellings of `AWS_S3_ENDPOINT` |
| `AWS_HTTPS` | use `https://` for an endpoint given as a bare host; default `YES` |
| `AWS_VIRTUAL_HOSTING` | put the bucket in the host name; see below for the default |
| `AWS_REQUEST_PAYER` | `requester` for [requester-pays buckets](https://docs.aws.amazon.com/AmazonS3/latest/userguide/RequesterPaysBuckets.html) |
| `AWS_NO_SIGN_REQUEST` | send unsigned requests, for public buckets |
| `AWS_ROLE_ARN` | role to assume with a web identity token |
| `AWS_WEB_IDENTITY_TOKEN_FILE` | OIDC token file for that role |
| `AWS_ROLE_SESSION_NAME` | optional session name for that role |
| `AWS_STS_ENDPOINT` | STS endpoint for web identity; default `https://sts.amazonaws.com/` |
| `AWS_CONTAINER_CREDENTIALS_RELATIVE_URI` | ECS credential path on `169.254.170.2` |
| `AWS_CONTAINER_CREDENTIALS_FULL_URI` | full credential URL; must be HTTPS, loopback, or the ECS host |
| `AWS_CONTAINER_AUTHORIZATION_TOKEN` | `Authorization` value for the container endpoint |
| `AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE` | file holding that value |
| `AWS_EC2_METADATA_DISABLED` | never ask the EC2 [instance metadata service](https://docs.aws.amazon.com/sdkref/latest/guide/feature-imds-credentials.html) (IMDS) |
| `AWS_METADATA_SERVICE_TIMEOUT` | IMDS connect timeout in seconds, `1`–`60` |

Karu looks for credentials in this order and uses the first it finds:

1. `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY`,
2. the [profile](https://docs.aws.amazon.com/sdkref/latest/guide/file-format.html), with static keys,
   [`credential_process`](https://docs.aws.amazon.com/sdkref/latest/guide/feature-process-credentials.html), or a
   [web identity role](https://docs.aws.amazon.com/sdkref/latest/guide/feature-assume-role-credentials.html),
   which `AWS_ROLE_ARN` and `AWS_WEB_IDENTITY_TOKEN_FILE` can also supply,
3. the [ECS container endpoint](https://docs.aws.amazon.com/sdkref/latest/guide/feature-container-credentials.html),
4. IMDS, when Karu reads the environment or `AWS_EC2_METADATA_DISABLED` is set.

Profiles that chain roles through `source_profile` and
[IAM Identity Center](https://docs.aws.amazon.com/sdkref/latest/guide/feature-sso-credentials.html) (SSO) profiles are not
handled natively. Use an AWS SDK and pass the result to Karu through a
[custom provider](#custom-providers). A `credential_process` that runs longer than
`KARU_REQUEST_TIMEOUT` is killed.

If S3 answers that a bucket lives in another region, Karu signs the request
again for that region and remembers it for that bucket for the rest of the
client's life.

`AWS_S3_ENDPOINT` is a service root. With
[virtual hosting](https://docs.aws.amazon.com/AmazonS3/latest/userguide/VirtualHosting.html), an endpoint of
`https://objects.example.test` becomes `https://bucket.objects.example.test/key`;
without it, `https://objects.example.test/bucket/key`. Virtual hosting is on by
default for AWS itself and off for a custom endpoint. Buckets with a dot in the
name also default to path style, because AWS's wildcard certificate does not
match them. Setting `AWS_VIRTUAL_HOSTING` explicitly overrides both defaults.

## Source Cooperative

`source://account/product/key` and `/vsisource/account/product/key` go through
the [Source Data Proxy](https://docs.source.coop/data-proxy). Public data needs
no setup and is read unsigned. Source keeps its own credentials and callback
kind, so AWS credentials are never sent to the proxy.

| Option | Purpose |
|---|---|
| `SOURCE_PROFILE` | AWS-style profile; default `source-coop` once signing is on |
| `SOURCE_CONFIG_FILE` | AWS-style config file; default `~/.aws/config` |
| `SOURCE_SHARED_CREDENTIALS_FILE` | AWS-style credentials file; default `~/.aws/credentials` |
| `SOURCE_ACCESS_KEY_ID` | Source access key ID |
| `SOURCE_SECRET_ACCESS_KEY` | Source secret; set it together with the key ID |
| `SOURCE_SESSION_TOKEN` | optional session token |
| `SOURCE_REGION` | signing region; otherwise the profile's region, then `us-east-1` |
| `SOURCE_ENDPOINT` / `SOURCE_PROXY_URL` | proxy service root; default `https://data.source.coop` |
| `SOURCE_NO_SIGN_REQUEST` | `YES` always reads unsigned, `NO` always signs |

Despite its name, `SOURCE_PROXY_URL` is the Source endpoint, not an HTTP proxy.
Use `KARU_HTTP_PROXY` for a proxy.

Requests are signed as soon as any Source credential option is set or a
[`KARU_CREDENTIALS_SOURCE`](https://github.com/asterisk-labs/karu/blob/v0.5.0/include/karu/karu.h#L89) callback is installed. `SOURCE_NO_SIGN_REQUEST=YES` overrides that.
`SOURCE_NO_SIGN_REQUEST=NO` asks for credentials even without other options and
falls back to the `source-coop` profile.

The [Source CLI](https://github.com/source-cooperative/source-coop-cli) writes
a profile that uses `credential_process`:

```ini
[profile source-coop]
credential_process = source-coop creds
endpoint_url = https://data.source.coop
```

Run `source-coop login` once, then set `SOURCE_PROFILE=source-coop`. Karu runs
the credential process and runs it again when the temporary keys are about to
expire, killing it if it takes longer than `KARU_REQUEST_TIMEOUT`. Karu does
not do the browser login or read the CLI's keyring. It also ignores the
profile's `endpoint_url`, because the endpoint always comes from `SOURCE_ENDPOINT`.

## Google Cloud Storage

| Option | Purpose |
|---|---|
| `GCS_ACCESS_TOKEN` | static bearer token |
| `GCS_HMAC_ACCESS_KEY_ID` | [HMAC key](https://cloud.google.com/storage/docs/authentication/hmackeys) access ID |
| `GCS_HMAC_SECRET_ACCESS_KEY` | HMAC secret; set it together with the access ID |
| `GOOGLE_APPLICATION_CREDENTIALS` | path to an [Application Default Credentials](https://cloud.google.com/docs/authentication/application-default-credentials) (ADC) JSON file |
| `CLOUDSDK_CONFIG` | gcloud configuration directory that holds the ADC file |
| `GCS_REFRESH_TOKEN` | user refresh token |
| `GCS_CLIENT_ID` | OAuth client ID for that refresh token |
| `GCS_CLIENT_SECRET` | OAuth client secret for that refresh token |
| `GCS_PRIVATE_KEY` | service account private key, as inline PEM |
| `GCS_PRIVATE_KEY_FILE` | the same key, from a file |
| `GCS_CLIENT_EMAIL` | service account email for that key |
| `GCS_SCOPE` | OAuth scope; default read-only object access |
| `GCS_USER_PROJECT` | project billed for [requester-pays buckets](https://cloud.google.com/storage/docs/requester-pays) |
| `GCS_ENDPOINT` | XML API service root; default `https://storage.googleapis.com` |
| `GCS_METADATA_ENDPOINT` | [metadata server](https://cloud.google.com/compute/docs/metadata/overview) token URL |
| `GCS_METADATA_DISABLED` | never ask the metadata server |
| `GCS_NO_SIGN_REQUEST` | send unsigned requests, for public buckets |

Karu checks `GCS_ACCESS_TOKEN`, the HMAC pair, `GCS_REFRESH_TOKEN`, the ADC
file, the service account key and last the metadata server, in that order, and
uses the first it finds. Without `GOOGLE_APPLICATION_CREDENTIALS` or `CLOUDSDK_CONFIG`,
the ADC file is gcloud's default one, if it exists.

The ADC file can describe a service account, a user login, or an OIDC
[`external_account`](https://cloud.google.com/iam/docs/workload-identity-federation)
whose subject token comes from a file or a URL, with or without
[service account impersonation](https://cloud.google.com/iam/docs/service-account-impersonation).
For other external account sources, use a [custom provider](#custom-providers).

The metadata server is only tried when Karu reads the environment or when
`GCS_METADATA_ENDPOINT` is set. `GCS_METADATA_DISABLED=YES` turns it off in
every case.

## Azure Storage

| Option | Purpose |
|---|---|
| `AZURE_STORAGE_CONNECTION_STRING` | [connection string](https://learn.microsoft.com/azure/storage/common/storage-configure-connection-string) with the account, endpoints, Shared Key, or SAS |
| `AZURE_STORAGE_ACCOUNT` | storage account name |
| `AZURE_STORAGE_ACCESS_KEY` | Shared Key, base64 encoded |
| `AZURE_STORAGE_SAS_TOKEN` | [SAS](https://learn.microsoft.com/azure/storage/common/storage-sas-overview) token, as an encoded query string |
| `AZURE_STORAGE_ACCESS_TOKEN` | static bearer token |
| `AZURE_STORAGE_ENDPOINT` | Blob or Data Lake service root |
| `AZURE_TENANT_ID` | Microsoft Entra tenant |
| `AZURE_CLIENT_ID` | application or managed identity client ID |
| `AZURE_CLIENT_SECRET` | client secret for that application |
| `AZURE_FEDERATED_TOKEN_FILE` | [workload identity](https://learn.microsoft.com/entra/workload-id/workload-identity-federation) token file |
| `AZURE_AUTHORITY_HOST` | sign-in authority; default `https://login.microsoftonline.com` |
| `AZURE_STORAGE_SCOPE` | OAuth scope; default `https://storage.azure.com/.default` |
| `AZURE_STORAGE_RESOURCE` | managed identity resource; default `https://storage.azure.com/` |
| `AZURE_IMDS_OBJECT_ID` | pick a [managed identity](https://learn.microsoft.com/entra/identity/managed-identities-azure-resources/overview) by object ID |
| `AZURE_IMDS_CLIENT_ID` | pick a managed identity by client ID; defaults to `AZURE_CLIENT_ID` |
| `AZURE_IMDS_MSI_RES_ID` | pick a managed identity by resource ID |
| `IDENTITY_ENDPOINT` / `IDENTITY_HEADER` | [App Service managed identity](https://learn.microsoft.com/azure/app-service/overview-managed-identity) |
| `IMDS_ENDPOINT` | managed identity endpoint, replacing the default |
| `AZURE_NO_SIGN_REQUEST` | send unsigned requests, for public containers |

Karu tries, in order, a connection string, a Shared Key, SAS, or bearer token; a
service principal with a client secret or a workload identity token; App
Service managed identity; and last the instance metadata service. That last one
is only tried when Karu reads the environment or `IMDS_ENDPOINT` is set.

The endpoint comes from `AZURE_STORAGE_ENDPOINT`, then from the connection
string's `BlobEndpoint` or `DfsEndpoint`. Without either, Karu builds
`https://<account>.blob.core.windows.net`, or `.dfs.` for `abfs://` and
`/vsiadls/` paths, and honors the connection string's
`DefaultEndpointsProtocol` and `EndpointSuffix`. For [sovereign clouds](https://learn.microsoft.com/entra/identity-platform/authentication-national-cloud), set
`AZURE_AUTHORITY_HOST`, `AZURE_STORAGE_SCOPE`, and `AZURE_STORAGE_RESOURCE` as
well.

## HTTP and Hugging Face

| Option | Purpose |
|---|---|
| `KARU_HTTP_HEADERS` | extra request headers, one `Name: value` per line |
| `KARU_HTTP_VERSION` | `1.1` (default), `2TLS` or `2`, `2PRIOR_KNOWLEDGE`, or `AUTO` |
| `KARU_HTTP_CA_BUNDLE` / `CURL_CA_BUNDLE` / `SSL_CERT_FILE` | CA bundle file |
| `KARU_HTTP_CA_PATH` | directory of CA certificates |
| `KARU_HTTP_PROXY` | proxy URL, for example `http://` or `socks5h://`; without it, [libcurl's proxy variables](https://curl.se/libcurl/c/CURLOPT_PROXY.html) apply |
| `KARU_HTTP_PROXY_CREDENTIALS` | proxy credentials, as `user:password` |
| `KARU_HTTP_USER_AGENT` | `User-Agent`; default `karu/<version>`, and an empty value set through the API sends none |
| `HF_TOKEN` / `HUGGING_FACE_HUB_TOKEN` | Hugging Face token; wins over token files |
| `HF_TOKEN_PATH` | token file written by Hugging Face tools |
| `HF_HOME` | Hugging Face home; the token is read from `<HF_HOME>/token` |
| `HF_ENDPOINT` | Hub service root; default `https://huggingface.co` |

Without `HF_TOKEN`, Karu looks for a token file the way the
[Hugging Face tools](https://huggingface.co/docs/huggingface_hub/package_reference/environment_variables)
do, checking `$HF_TOKEN_PATH`, then `$HF_HOME/token`, then
`${XDG_CACHE_HOME:-~/.cache}/huggingface/token`. No file means anonymous
access. A client made with [`karu_config_create_empty()`](https://github.com/asterisk-labs/karu/blob/v0.5.0/include/karu/karu.h#L68) skips the default
location but still reads `HF_TOKEN_PATH` or `HF_HOME` when they are set.

Karu reads the token file again for every request attempt, so logging in or
rotating the token takes effect on the next read. Surrounding whitespace is
trimmed, and a file that exists but cannot be read is a credential error.

Plain HTTP and Hugging Face requests follow redirects. Signed cloud requests
report a redirect as an error instead, so their credentials and signatures are
never sent to another address. Hugging Face drops the token when it follows the
redirect to its CDN. When `KARU_HTTP_HEADERS` is set, Karu refuses any redirect
to a different scheme, host, or port, so the configured headers never leave the original
server. A Hugging Face file that redirects to the CDN then fails.

Karu writes `Range` and `Host` itself, along with any header it already adds to
the request, and `KARU_HTTP_HEADERS` cannot replace them. Cloud and Hugging
Face requests also refuse `Authorization`, `Date`, `If-Match`, and any
`x-amz-`, `x-goog-`, or `x-ms-` header; use the credential options or a
[custom provider](#custom-providers) for those.

Transfers and their redirects only use HTTP and HTTPS.

When no CA option is set, a client made with [`karu_config_create()`](https://github.com/asterisk-labs/karu/blob/v0.5.0/include/karu/karu.h#L65) looks for
the system CA bundle on Linux and macOS at run time, instead of relying on the
path libcurl was built with. With OpenSSL, if libcurl's default directory
holds hashed certificates, as on Debian and Ubuntu, Karu uses only that
directory. Otherwise, with libcurl 7.87 or newer, it uses the bundle alone so
libcurl can [cache it](https://curl.se/libcurl/c/CURLOPT_CA_CACHE_TIMEOUT.html) for up to 24 hours; a changed bundle may not be noticed
until then. Setting `KARU_HTTP_CA_PATH` adds a directory. Other TLS backends
keep libcurl's defaults.

## Custom providers

A callback installed with
[`karu_config_set_credentials_provider()`](https://asterisk.coop/karu/api.html#credentials) replaces Karu's own
discovery for AWS, GCS, Azure, or Source Cooperative. Karu calls it with the
canonical [VSI path](https://gdal.org/en/stable/user/virtual_file_systems.html)
of the object it is about to read. The [API reference](https://asterisk.coop/karu/api.html#credentials) gives
the callback's signature, the fields each provider uses and the rules the
callback must follow. This section covers how Karu reuses and refreshes what it
returns.

Without a `cache_prefix`, Karu calls the provider for every request. That is
the right choice when the provider or the cloud SDK already caches. With a
prefix such as `/vsis3/team-bucket/`, Karu reuses the value for every object
under it until it nears expiry. Only use a prefix the credentials are valid for.

Source Cooperative credentials are cached apart from AWS credentials, and
their prefixes start with `/vsisource/`, not `/vsis3/`.

Refreshing is lazy, with no background thread, as
[How Karu reads](https://asterisk.coop/karu/how-karu-reads.html#credentials) describes. When credentials are close to
expiring, the next request for that path calls the provider, and other requests
for the same path wait for that one call. "Close" means the last tenth of their
lifetime, but never more than 60 seconds or less than 1 second before expiry.
Callback values without an expiry
stay valid until an authentication error or the end of the client. Static
credentials Karu loads itself are checked again every minute, so a rotated
profile is picked up.
