// Transport options that only a live endpoint can exercise: the TLS and proxy
// knobs, the size probe's fallback when a server answers a range with a whole
// body, and malformed or cross-origin redirect targets guarded by
// KARU_HTTP_HEADERS.
#include "credential_support.hpp"
#include "karu/karu.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace karu::test {
namespace {

karu::Client make_client(const std::vector<std::pair<const char*, std::string>>& options) {
    auto config = karu::Config::empty().value();
    for (const auto& [name, value] : options) {
        if (!config.set(name, value))
            fail(__LINE__, std::string("could not set ") + name);
    }
    auto client = karu::Client::create(config);
    if (!client) {
        fail(__LINE__, "could not create client: " + client.error().message);
        return karu::Client::create(karu::Config::empty().value()).value();
    }
    return std::move(*client);
}

void size_probe_without_content_range() {
    // A server that answers "Range: bytes=0-0" with the whole body and only a
    // Content-Length still has to yield a size.
    const auto client = make_client({});
    auto object = karu::Object::parse(fixture_url("/size/length-only")).value();
    auto size = client.size(object);
    OK(size.has_value());
    if (size)
        EQ(*size, std::uint64_t{4096});

    // A locator window reports its own length without touching the network.
    auto windowed =
        karu::Object::parse("/vsisubfile/100_50," + fixture_url("/size/length-only")).value();
    auto window_size = client.size(windowed);
    OK(window_size.has_value());
    if (window_size)
        EQ(*window_size, std::uint64_t{50});

    // Error statuses map through the same table as a range read.
    auto missing = karu::Object::parse(fixture_url("/status/404")).value();
    auto missing_size = client.size(missing);
    OK(!missing_size.has_value());
    if (!missing_size)
        OK(missing_size.error().status == KARU_ERR_NOT_FOUND);
}

void tls_and_proxy_options() {
    // CAINFO is inert over plain HTTP but must still be applied rather than
    // rejected by the option setter.
    TempTree tree("transport");
    const std::string bundle = tree.write("ca/bundle.pem", "# not a real bundle\n");
    {
        const auto client = make_client({{"KARU_HTTP_CA_BUNDLE", bundle}});
        auto object = karu::Object::parse(fixture_url("/range")).value();
        std::vector<std::byte> buffer(64);
        OK(client.read_into(object, 0, buffer).has_value());
    }

    // CAPATH is not universally available: Schannel, which the Windows CI job
    // builds against, answers CURLE_NOT_BUILT_IN. Either outcome is correct, so
    // assert that the option is wired rather than that it succeeds.
    {
        const auto client = make_client({{"KARU_HTTP_CA_PATH", tree.root()}});
        auto object = karu::Object::parse(fixture_url("/range")).value();
        std::vector<std::byte> buffer(64);
        auto read = client.read_into(object, 0, buffer);
        if (!read) {
            OK(read.error().status == KARU_ERR_NETWORK);
            OK(read.error().message.find("CURLOPT_CAPATH") != std::string::npos);
        } else {
            OK(read.has_value());
        }
    }

    // A proxy that refuses connections must surface as a network failure, which
    // is the only proof the option reached libcurl.
    {
        const auto client = make_client({{"KARU_HTTP_PROXY", "http://127.0.0.1:9"},
                                         {"KARU_HTTP_PROXY_CREDENTIALS", "user:secret"},
                                         {"KARU_CONNECT_TIMEOUT", "1"},
                                         {"KARU_MAX_ATTEMPTS", "1"}});
        auto object = karu::Object::parse(fixture_url("/range")).value();
        std::vector<std::byte> buffer(64);
        auto read = client.read_into(object, 0, buffer);
        OK(!read.has_value());
        if (!read) {
            // The proxy address must never appear with its password attached.
            OK(read.error().message.find("secret") == std::string::npos);
        }
    }

    // A user agent can be replaced or removed outright.
    {
        const auto client = make_client({{"KARU_HTTP_USER_AGENT", "karu-test-agent/1"}});
        auto object = karu::Object::parse(fixture_url("/range")).value();
        std::vector<std::byte> buffer(32);
        OK(client.read_into(object, 0, buffer).has_value());
    }
    {
        const auto client = make_client({{"KARU_HTTP_USER_AGENT", ""}});
        auto object = karu::Object::parse(fixture_url("/range")).value();
        std::vector<std::byte> buffer(32);
        OK(client.read_into(object, 0, buffer).has_value());
    }
}

void redirect_origin_validation() {
    // With KARU_HTTP_HEADERS set, a redirect that leaves the origin is refused
    // before the target is contacted. Every malformed or different-origin
    // target below must fail closed.
    static constexpr std::array<const char*, 7> blocked{
        "/redirect/ipv6",          // bracketed IPv6 host with a port
        "/redirect/ipv6-unclosed", // '[' with no closing ']'
        "/redirect/ipv6-trailing", // text after ']' that is not a port
        "/redirect/bad-port",      // port outside the 16-bit range
        "/redirect/empty-port",    // trailing ':' with no digits
        "/redirect/scheme",        // a scheme that is not http or https
        "/redirect/no-host",       // an authority that is only a port
    };
    const auto guarded =
        make_client({{"KARU_HTTP_HEADERS", "X-Karu-Probe: sentinel"}, {"KARU_MAX_ATTEMPTS", "1"}});
    for (const char* route : blocked) {
        auto object = karu::Object::parse(fixture_url(route)).value();
        std::vector<std::byte> buffer(16);
        auto read = guarded.read_into(object, 0, buffer);
        OK(!read.has_value());
        if (!read) {
            OK(read.error().status == KARU_ERR_HTTP);
            if (read.error().message.find("cross-origin redirect blocked") == std::string::npos)
                fail(__LINE__, std::string(route) +
                                   " was refused for the wrong reason: " + read.error().message);
        }
    }

    // Some libcurl backends pass an empty Location header to the callback and
    // others treat the response as a plain 302. Both outcomes fail safely: an
    // empty header has no redirect target to validate or follow.
    {
        auto object = karu::Object::parse(fixture_url("/redirect/empty-location")).value();
        std::vector<std::byte> buffer(16);
        auto read = guarded.read_into(object, 0, buffer);
        OK(!read.has_value());
        if (!read)
            OK(read.error().status == KARU_ERR_HTTP);
    }

    // Without user headers the same redirects are followed, and fail only
    // because their targets do not answer. The point is that the guard is not
    // armed, so the failure must not be the cross-origin message.
    const auto open_client =
        make_client({{"KARU_MAX_ATTEMPTS", "1"}, {"KARU_CONNECT_TIMEOUT", "1"}});
    auto object = karu::Object::parse(fixture_url("/redirect/ipv6")).value();
    std::vector<std::byte> buffer(16);
    auto read = open_client.read_into(object, 0, buffer);
    OK(!read.has_value());
    if (!read)
        OK(read.error().message.find("cross-origin redirect blocked") == std::string::npos);
}

void negotiated_http_versions() {
    // Every KARU_HTTP_VERSION spelling has to reach libcurl. Only the automatic
    // and 1.1 settings are guaranteed to work: a libcurl built without HTTP/2 —
    // which is what vcpkg installs by default on the Windows job — rejects the
    // HTTP/2 constants at setopt time with CURLE_UNSUPPORTED_PROTOCOL.
    for (const char* version : {"AUTO", "1.1"}) {
        const auto client = make_client({{"KARU_HTTP_VERSION", version}});
        auto object = karu::Object::parse(fixture_url("/range")).value();
        std::vector<std::byte> buffer(48);
        auto read = client.read_into(object, 16, buffer);
        if (!read)
            fail(__LINE__, std::string(version) + ": " + read.error().message);
        OK(read.has_value());
    }
    // The HTTP/2 spellings must be accepted by the configuration layer and then
    // either work or fail as a network error. What must never happen is a
    // configuration rejection, which would mean the option never reached curl.
    for (const char* version : {"2", "2TLS", "2PRIOR_KNOWLEDGE"}) {
        const auto client = make_client({{"KARU_HTTP_VERSION", version},
                                         {"KARU_MAX_ATTEMPTS", "1"},
                                         {"KARU_CONNECT_TIMEOUT", "2"}});
        auto object = karu::Object::parse(fixture_url("/range")).value();
        std::vector<std::byte> buffer(48);
        auto read = client.read_into(object, 0, buffer);
        if (!read)
            OK(read.error().status == KARU_ERR_NETWORK);
        else
            OK(read.has_value());
    }
}

void size_probe_without_credentials() {
    // The size probe builds its request through the same signer as a read, so a
    // cloud object with no credentials fails during preparation rather than on
    // the wire.
    auto config = karu::Config::empty().value();
    OK(config.set("AWS_S3_ENDPOINT", "127.0.0.1:" + std::to_string(fixture_port())).has_value());
    OK(config.set("AWS_HTTPS", "NO").has_value());
    OK(config.set("AWS_VIRTUAL_HOSTING", "NO").has_value());
    const auto client = karu::Client::create(config).value();
    auto object = karu::Object::parse("s3://bucket/key").value();
    auto size = client.size(object);
    OK(!size.has_value());
    if (!size) {
        OK(size.error().status == KARU_ERR_CREDENTIALS);
        OK(size.error().message.find("AWS_ACCESS_KEY_ID") != std::string::npos);
    }
    std::vector<std::byte> buffer(16);
    auto read = client.read_into(object, 0, buffer);
    OK(!read.has_value());
    if (!read)
        OK(read.error().status == KARU_ERR_CREDENTIALS);
}

} // namespace

void test_transport_options() {
    SECTION("transport options, size fallback and redirect origins");
    size_probe_without_content_range();
    size_probe_without_credentials();
    tls_and_proxy_options();
    negotiated_http_versions();
    redirect_origin_validation();
}

} // namespace karu::test
