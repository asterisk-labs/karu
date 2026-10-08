# karu

*karu is the Quechua word for far.*

[![CI](https://github.com/asterisk-labs/karu/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/asterisk-labs/karu/actions/workflows/ci.yml)
[![Coverage](https://codecov.io/gh/asterisk-labs/karu/graph/badge.svg?branch=main)](https://codecov.io/gh/asterisk-labs/karu)
[![Release](https://img.shields.io/github/v/release/asterisk-labs/karu?color=4E04EB)](https://github.com/asterisk-labs/karu/releases)
![C++23](https://img.shields.io/badge/C%2B%2B-23-4E04EB.svg)
[![MIT license](https://img.shields.io/badge/license-MIT-2b8a3e.svg)](#license)

Batched byte-range reads from local files, HTTP, S3, GCS, Azure, Hugging Face and
[Source Cooperative](https://source.coop).

## Features

- **URIs or GDAL VSI paths** for every backend, such as `s3://bucket/key` or `/vsis3/bucket/key`.
- **Batched reads** that merge nearby ranges, fetch in parallel and complete one by one.
- **Stateless reads** that name their object, offset and buffer, so they can be reordered and retried.
- **Version pinning** with `stat` and `if_match`, so a changed object fails instead of mixing versions.
- **Retries and resumable transfers** within a per-request deadline.
- **Large file downloads** using parallel requests and bounded memory.
- **Credential discovery** for every backend, plus custom providers.
- **Per-path configuration**, for example anonymous for one bucket and a profile for another.

## Quick start

```cpp
#include <karu/karu.hpp>

auto client = karu::Client::create(karu::Config::from_environment().value()).value();
auto object = karu::Object::parse(
    "hf://datasets/asterisk-labs/rumi-api-fixtures/data/s2-00-tile.rumi").value();
auto bytes = client.read(object, 0, 4096).value();

// Download a complete object with parallel requests and fixed memory use.
auto downloaded = client.download(object, "scene.rumi").value();
```

## Installation

Prebuilt packages for Linux, macOS and Windows are on the
[releases page](https://github.com/asterisk-labs/karu/releases), and CMake finds them
with `find_package`.

```cmake
find_package(karu CONFIG REQUIRED)
target_link_libraries(app PRIVATE karu::karu)
```

## Documentation

[Guide](https://asterisk.coop/karu/) · [Configuration](CONFIGURATION.md) · [Changelog](CHANGELOG.md)

Coding agents can install the Karu skill with `npx skills add asterisk-labs/karu`.

## License

MIT. See [`LICENSE`](LICENSE).

<div align="center">
  <br>
  Made with ♥ by
  <br><br>
  <a href="https://asterisk.coop">
    <img src="https://raw.githubusercontent.com/asterisk-labs/cozip/refs/heads/main/images/asterisk_logo.svg" alt="Asterisk Labs" width="320"/>
  </a>
</div>
