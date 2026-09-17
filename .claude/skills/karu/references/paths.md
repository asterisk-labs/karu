# Paths

Sources: `src/uri.cpp`, `src/uri.hpp`, `src/config.cpp` (`canonical_vsi_path`,
`path_prefix_matches`), `src/backends/url.cpp`, `tests/test_uri_config.cpp`. The
canonical forms and messages below were printed by `karu_resolve` in karu 0.2.3.

## Contents

1. Accepted spellings
2. Canonical identity
3. Keys and encoding
4. Windows: `/vsisubfile/` and `#bytes=`
5. Hugging Face
6. Source Cooperative
7. What is rejected

## 1. Accepted spellings

| Backend | URI | VSI path | Split into | Request URL |
| --- | --- | --- | --- | --- |
| Local file | `/data/a.bin`, `data/a.bin`, `C:\data\a.bin`, `file:///data/a.bin` | | UTF-8 path | positional reads, no URL |
| HTTP(S) | `http://host/x`, `https://host/x?q=1` | `/vsicurl/https://host/x` | the whole URL | the URL as written |
| S3 and compatible | `s3://bucket/key` | `/vsis3/bucket/key` | bucket, key | `https://bucket.s3.<region>.amazonaws.com/key` or the configured endpoint |
| Google Cloud Storage | `gs://bucket/key` | `/vsigs/bucket/key` | bucket, key | `<GCS_ENDPOINT>/bucket/key` |
| Azure Blob | `az://container/blob` | `/vsiaz/container/blob` | container, blob | `https://<account>.blob.core.windows.net/container/blob` |
| Azure Data Lake Gen2 | `abfs://container/path` | `/vsiadls/container/path` | container, path | `https://<account>.dfs.core.windows.net/container/path` |
| Hugging Face | `hf://[datasets/\|spaces/]owner/name[@revision]/path` | `/vsihf/...` | endpoint path | `<HF_ENDPOINT>/[datasets/\|spaces/]owner/name/resolve/<revision>/path` |
| Source Cooperative | `source://account/product/key` | `/vsisource/account/product/key` | account, `product/key` | `<SOURCE_ENDPOINT>/account/product/key` |

- Anything that contains no `://` and does not start with `/vsi` is a local path.
- `file://` removes only the scheme: `file:///data/a.bin` reads `/data/a.bin`. There is no
  host part.
- Schemes and VSI prefixes are case sensitive and lowercase. `S3://bucket/key` is an
  unknown scheme.
- A `/vsicurl/` target must itself start with `http://` or `https://`.
- Cloud URIs need a non-empty container and a non-empty key: `s3://bucket` and
  `s3://bucket/` are malformed.

## 2. Canonical identity

`karu_locator_uri()` (C++ `Object::uri()`) returns the canonical identity:

| Input | Canonical | Window |
| --- | --- | --- |
| `s3://bucket/a//b c.tif` | `/vsis3/bucket/a//b c.tif` | none |
| `/vsicurl/https://example.test/a?x=1` | `https://example.test/a?x=1` | none |
| `file:///tmp/a.bin` | `file:///tmp/a.bin` | none |
| `abfs://container/dir/blob` | `/vsiadls/container/dir/blob` | none |
| `hf://datasets/org/repo@v1.0/data/x.bin` | `/vsihf/datasets/org/repo@v1.0/data/x.bin` | none |
| `/vsisubfile/5_10,s3://bucket/key#bytes=100-199` | `/vsis3/bucket/key` | offset 105, length 10 |

The canonical identity is used for:

- path rules (`karu_config_set_path_option`), after the same `scheme://` to `/vsi.../`
  mapping is applied to the rule prefix;
- credential scopes and cache prefixes;
- planner grouping: only reads with the same backend, canonical identity and `if_match`
  can share a transfer;
- the object named in error messages.

Consequences:

- `s3://b/k` and `/vsis3/b/k` are the same object, and so are `/vsicurl/URL` and `URL`.
- Two spellings of one local file (`data/a.bin`, `/abs/data/a.bin`, `file:///abs/data/a.bin`)
  are different identities and never merge.
- A window is not part of the identity. Reads through different windows of one object
  are planned together and can merge into one transfer.
- HTTP URLs keep their query in the identity, so the same object with a different
  presigned query is a different identity.

## 3. Keys and encoding

- The key is every byte after the first `/` that follows the container. Empty segments
  (`a//b`), spaces and dots are significant; nothing is normalized.
- Write cloud keys unencoded. Karu percent-encodes them when it builds the request,
  keeping `A-Z a-z 0-9 - _ . ~ /`. A key written as `a%20b` is sent as `a%2520b`.
- HTTP URLs are sent exactly as written (`CURLOPT_PATH_AS_IS`), so encode them yourself.
- Local paths are UTF-8 on every platform (wide paths on Windows).

## 4. Windows: `/vsisubfile/` and `#bytes=`

```text
/vsisubfile/OFFSET_LENGTH,<inner path>     bytes [OFFSET, OFFSET + LENGTH)
/vsisubfile/OFFSET,<inner path>            from OFFSET to the end
/vsisubfile/OFFSET_-N,<inner path>         GDAL spelling; also from OFFSET to the end
<terminal URI>#bytes=FIRST-LAST            inclusive bytes FIRST..LAST
<terminal URI>#bytes=FIRST-                from FIRST to the end
```

- The inner path can be any accepted spelling, including another `/vsisubfile/`, up to
  16 levels.
- Nested windows compose: offsets add, and a child length is clipped to its parent. A
  child starting past its parent's end fails with `window starts past the end of its parent`.
- `#bytes=` applies to the terminal URI, inside any `/vsisubfile/`. Only a fragment that
  starts with `#bytes=` is interpreted; `/vsisubfile/5_10,s3://bucket/key#bytes=100-199`
  becomes offset 105, length 10.
- Request offsets are relative to the window start. A read that leaves a bounded window
  completes with `KARU_ERR_RANGE`: `...: range [40, +16) leaves the locator window`.
- `karu_client_size` returns a bounded window's length with no I/O. For an open-ended
  window it returns the object size minus the offset, or 0 past the end.
- Container formats use windows to address members without a second download. cozip,
  tacozip and taco emit `/vsisubfile/{offset}_{size},{base}`, for example
  `/vsisubfile/1048576_8388608,/vsisource/account/product/archive.cozip`.

## 5. Hugging Face

- `hf://owner/name/path` is a model repository; `hf://datasets/owner/name/path` and
  `hf://spaces/owner/name/path` select the other repository kinds.
- `@revision` after the repository name selects a branch, tag or commit; the default is
  `main`. The revision is percent-encoded and cannot contain `/`, so name a branch such
  as `refs/pr/1` by its commit hash.
- `hf://datasets/org/repo@v1.0/data/x.bin` requests
  `https://huggingface.co/datasets/org/repo/resolve/v1.0/data/x.bin`. `HF_ENDPOINT`
  replaces the host part.
- The Hub answers large files with a redirect to a CDN. Karu follows it (credentials are
  not forwarded to the other host), unless `KARU_HTTP_HEADERS` is set, which limits
  redirects to the same origin.

## 6. Source Cooperative

- `source://account/product/key` needs all three parts;
  `source://account/product` fails with `expected source://account/product/key in 'product'`.
- Requests are path style against `SOURCE_ENDPOINT` (default `https://data.source.coop`).
  Signing and anonymous access are covered in `credentials.md`.

## 7. What is rejected

| Input | Status | Message |
| --- | --- | --- |
| `""` | `KARU_ERR_URI` | `empty URI` |
| `/vsizip/archive.zip/a.tif` | `KARU_ERR_UNSUPPORTED` | `unsupported virtual filesystem '/vsizip/'; Karu supports /vsisubfile/, /vsicurl/, /vsis3/, /vsigs/, /vsiaz/, /vsiadls/, /vsihf/ and /vsisource/ (no *_streaming aliases)` |
| `/vsis3_streaming/bucket/key` | `KARU_ERR_UNSUPPORTED` | same message with `'/vsis3_streaming/'` |
| `s3://bucket` | `KARU_ERR_URI` | `expected s3://bucket/key in 'bucket'` |
| `ftp://host/object` | `KARU_ERR_URI` | `unknown scheme in 'ftp://host/object'` |
| `/vsicurl/s3://bucket/key` | `KARU_ERR_URI` | `/vsicurl/ target must be an HTTP URL` |
| `/vsisubfile/1_2` | `KARU_ERR_URI` | `/vsisubfile/ wants OFFSET[_LENGTH],<uri>` |
| `/vsisubfile/abc,/tmp/a` | `KARU_ERR_URI` | `'abc' is not a byte count` |
| `https://example.test/a#bytes=10-4` | `KARU_ERR_URI` | `#bytes= ends before it starts` |
| `hf://datasets/org/repo` | `KARU_ERR_URI` | `expected hf://owner/name/path in 'repo'` |
| 17 nested `/vsisubfile/` | `KARU_ERR_URI` | `/vsisubfile/ nesting exceeds 16 levels` |

Karu has no `/vsimem/`, archives (`/vsizip/`, `/vsitar/`, `/vsigzip/`), `/vsioss/`,
`/vsiswift/` or streaming handlers, by design. Fetch an archive member through its
offset and size with `/vsisubfile/` instead.
