// Direct coverage for the helpers every credential backend shares:
// src/backends/credentials.cpp and the provider-independent half of
// src/backends/aws_profile.cpp. Everything here is declared in a header, so the
// cases call the helpers straight rather than driving a provider - that keeps
// the assertions on the helper's own contract instead of a backend's.
#include "backends/aws_profile.hpp"
#include "backends/credentials.hpp"
#include "credential_support.hpp"
#include "karu/karu.h"
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace karu::test {
namespace {

using backends::Header;

// ---------------------------------------------------------------------------
// Assertion helpers. The suite checks hundreds of small outcomes, and a bare
// OK() on an expected<> reports nothing useful when it fails.
// ---------------------------------------------------------------------------

template <typename Value>
void expect_error(int line, const std::expected<Value, RequestError>& result, karu_status status,
                  std::string_view message) {
    if (result) {
        fail(line, "expected the error '" + std::string(message) + "' but the call succeeded");
        return;
    }
    ok_at(line, result.error().status == status, "error status");
    string_at(line, result.error().message, message, "error message");
}

template <typename Value>
void expect_error_prefix(int line, const std::expected<Value, RequestError>& result,
                         karu_status status, std::string_view prefix) {
    if (result) {
        fail(line, "expected an error starting with '" + std::string(prefix) + "'");
        return;
    }
    ok_at(line, result.error().status == status, "error status");
    if (result.error().message.starts_with(prefix))
        ok_at(line, true, "error message prefix");
    else
        fail(line,
             "'" + result.error().message + "' does not start with '" + std::string(prefix) + "'");
}

void expect_text(int line, const std::optional<std::string>& actual, std::string_view expected) {
    if (!actual) {
        fail(line, "expected '" + std::string(expected) + "' but the key was missing");
        return;
    }
    string_at(line, *actual, expected, "json_string");
}

void expect_no_text(int line, const std::optional<std::string>& actual) {
    if (actual)
        fail(line, "expected no value but got '" + *actual + "'");
    else
        ok_at(line, true, "json_string is nullopt");
}

void expect_number(int line, const std::optional<std::int64_t>& actual, std::int64_t expected) {
    if (!actual) {
        fail(line, "expected " + std::to_string(expected) + " but the key was missing");
        return;
    }
    if (*actual == expected)
        ok_at(line, true, "json_integer");
    else
        fail(line, "expected " + std::to_string(expected) + " but got " + std::to_string(*actual));
}

void expect_no_number(int line, const std::optional<std::int64_t>& actual) {
    if (actual)
        fail(line, "expected no value but got " + std::to_string(*actual));
    else
        ok_at(line, true, "json_integer is nullopt");
}

// std::time can tick while a token request is in flight, so lifetimes are
// asserted as an inclusive window rather than a single instant.
void expect_expiry(int line, std::int64_t actual, std::int64_t before, std::int64_t after,
                   std::int64_t lifetime) {
    if (actual >= before + lifetime && actual <= after + lifetime)
        ok_at(line, true, "expires_at window");
    else
        fail(line, "expires_at " + std::to_string(actual) + " is outside [" +
                       std::to_string(before + lifetime) + ", " + std::to_string(after + lifetime) +
                       "]");
}

std::int64_t now_seconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

// libcurl honours http_proxy/HTTP_PROXY/all_proxy/ALL_PROXY by default, so a
// developer or CI machine with a proxy configured would route loopback traffic
// off-box. That breaks hermeticity, not just determinism.
struct DirectTransport {
    ScopedEnvironment lower_http{"http_proxy", nullptr};
    ScopedEnvironment upper_http{"HTTP_PROXY", nullptr};
    ScopedEnvironment lower_all{"all_proxy", nullptr};
    ScopedEnvironment upper_all{"ALL_PROXY", nullptr};
    ScopedEnvironment lower_bypass{"no_proxy", "*"};
    ScopedEnvironment upper_bypass{"NO_PROXY", "*"};
};

// The proxy case is the one transport case that must NOT bypass proxies:
// NO_PROXY="*" makes libcurl ignore CURLOPT_PROXY entirely.
struct ProxiedTransport {
    ScopedEnvironment lower_http{"http_proxy", nullptr};
    ScopedEnvironment upper_http{"HTTP_PROXY", nullptr};
    ScopedEnvironment lower_all{"all_proxy", nullptr};
    ScopedEnvironment upper_all{"ALL_PROXY", nullptr};
    ScopedEnvironment lower_bypass{"no_proxy", nullptr};
    ScopedEnvironment upper_bypass{"NO_PROXY", nullptr};
};

// ---------------------------------------------------------------------------
// path_exists and read_text_file
// ---------------------------------------------------------------------------

void test_file_helpers() {
    SECTION("credential file helpers");
    const TempTree tree("helpers_files");
    const std::string present = tree.write("present.txt", "x");
    const std::string missing = tree.absent("no-such-file.txt");

    auto file_exists = backends::path_exists(present, "unit");
    OK(file_exists.has_value());
    if (file_exists)
        OK(*file_exists);
    auto directory_exists = backends::path_exists(tree.root(), "unit");
    OK(directory_exists.has_value());
    if (directory_exists)
        OK(*directory_exists);
    auto absent_exists = backends::path_exists(missing, "unit");
    OK(absent_exists.has_value());
    if (absent_exists)
        OK(!*absent_exists);
    // The empty path is the hermeticity anchor for read_aws_profile: with an
    // empty home directory default_aws_path() returns "" and must not error.
    auto empty_exists = backends::path_exists("", "unit");
    OK(empty_exists.has_value());
    if (empty_exists)
        OK(!*empty_exists);

    // A path far past NAME_MAX makes std::filesystem::exists set an error_code
    // on POSIX. The MS STL may instead report "not found" with a cleared code,
    // so only the POSIX spelling is pinned here.
    const std::string oversized_path(5000, 'x');
    auto inspected = backends::path_exists(oversized_path, "AWS config file");
    OK(!inspected.has_value() || *inspected == false);
#ifndef _WIN32
    expect_error_prefix(__LINE__, inspected, KARU_ERR_CREDENTIALS,
                        "AWS config file: cannot inspect '");
#endif

    auto unopenable = backends::read_text_file(missing, "AWS config");
    expect_error(__LINE__, unopenable, KARU_ERR_CREDENTIALS,
                 "AWS config: cannot open '" + missing + "'");

    // 4096-byte chunks make 1 MiB exactly 256 reads, so both sides of the cap
    // are real boundaries rather than round numbers.
    const std::string exact = tree.write("exact.bin", std::string(1u << 20, 'a'));
    auto at_limit = backends::read_text_file(exact, "blob");
    OK(at_limit.has_value());
    if (at_limit)
        EQ(at_limit->size(), 1u << 20);

    const std::string over = tree.write("over.bin", std::string(1'100'000, 'x'));
    auto past_limit = backends::read_text_file(over, "blob");
    expect_error(__LINE__, past_limit, KARU_ERR_CREDENTIALS, "blob: file exceeds 1 MiB");

    // Binary mode: embedded NULs and CR survive, which is what lets a real ini
    // file carry a credential_process command containing a NUL.
    const std::string raw("a\0b\r\nc\0d\n", 9);
    const std::string binary = tree.write("bin.txt", raw);
    auto roundtrip = backends::read_text_file(binary, "blob");
    OK(roundtrip.has_value());
    if (roundtrip) {
        EQ(roundtrip->size(), 9u);
        OK(*roundtrip == raw);
    }
}

// ---------------------------------------------------------------------------
// json_string and json_integer
// ---------------------------------------------------------------------------

void test_json_scanner() {
    SECTION("credential JSON scanning");
    expect_text(__LINE__, backends::json_string(R"({"a":"one","b"  :  "two"})", "a"), "one");
    expect_text(__LINE__, backends::json_string(R"({"a":"one","b"  :  "two"})", "b"), "two");

    // json_string is a scanner, not a parser: a textual hit that is not
    // followed by a colon is skipped, so a key that also appears as a value or
    // inside a nested object still resolves to the real binding.
    expect_text(__LINE__, backends::json_string(R"({"x":"name","name":"real"})", "name"), "real");
    expect_text(__LINE__, backends::json_string(R"({"k" 1, "k": "v"})", "k"), "v");
    // The needle is the whole document, so the cursor lands at json.size().
    expect_no_text(__LINE__, backends::json_string(R"("name")", "name"));

    // A value that is not a string is "missing", never a coercion.
    expect_no_text(__LINE__, backends::json_string(R"({"n":5})", "n"));
    expect_no_text(__LINE__, backends::json_string(R"({"n":{"i":1}})", "n"));
    expect_no_text(__LINE__, backends::json_string(R"({"n":)", "n"));

    expect_text(__LINE__, backends::json_string(R"({"s":"q\"b\\s\/f\b\f\n\r\t."})", "s"),
                "q\"b\\s/f\b\f\n\r\t.");

    // \u and every other escape are deliberately unsupported: a response whose
    // field carries one reads as missing rather than as a bad decode.
    expect_no_text(__LINE__, backends::json_string(R"({"s":"\A"})", "s"));
    expect_no_text(__LINE__, backends::json_string(R"({"s":"\x41"})", "s"));
    expect_no_text(__LINE__, backends::json_string("{\"s\":\"abc\\", "s"));
    expect_no_text(__LINE__, backends::json_string("{\"s\":\"abc", "s"));

    expect_no_text(__LINE__, backends::json_string(R"({"a":"1"})", "b"));
    expect_no_text(__LINE__, backends::json_string("", "b"));
    expect_no_text(__LINE__, backends::json_string("not json at all", "b"));

    expect_number(__LINE__, backends::json_integer(R"({"n":"900"})", "n"), 900);
    expect_number(__LINE__, backends::json_integer(R"({"n":"-17"})", "n"), -17);
    expect_number(__LINE__, backends::json_integer(R"({"n":"4294967296"})", "n"), 4'294'967'296);
    // A quoted value with trailing junk fails the whole-string guard and then
    // fails again in the unquoted scan, which sees the opening quote.
    expect_no_number(__LINE__, backends::json_integer(R"({"n":"42x"})", "n"));
    expect_no_number(__LINE__, backends::json_integer(R"({"n":"abc"})", "n"));

    expect_number(__LINE__, backends::json_integer(R"({"n": 3600})", "n"), 3600);
    expect_number(__LINE__, backends::json_integer(R"({"n":-17})", "n"), -17);
    expect_number(__LINE__, backends::json_integer(R"({"expires_in":900,"x":1})", "expires_in"),
                  900);
    expect_number(__LINE__, backends::json_integer(R"({"n":9007199254740993})", "n"),
                  9'007'199'254'740'993);

    expect_no_number(__LINE__, backends::json_integer(R"({"a":1})", "n"));
    expect_no_number(__LINE__, backends::json_integer(R"({"n"})", "n"));
    expect_no_number(__LINE__, backends::json_integer(R"({"n": true})", "n"));
    expect_no_number(__LINE__, backends::json_integer(R"({"n": })", "n"));
}

// ---------------------------------------------------------------------------
// Time and range formatting
// ---------------------------------------------------------------------------

void test_time_and_range_helpers() {
    SECTION("credential time formatting");
    EQ(backends::iso8601_epoch("2035-01-02T03:04:05Z"), 2'051'319'845);
    // substr(0, 19) discards both the fractional seconds and the zone.
    EQ(backends::iso8601_epoch("2035-01-02T03:04:05.123456Z"), 2'051'319'845);
    // The Unix epoch is indistinguishable from the failure return. That is the
    // contract, not a bug: callers treat 0 as "no expiry".
    EQ(backends::iso8601_epoch("1970-01-01T00:00:00Z"), 0);

    EQ(backends::iso8601_epoch(""), 0);
    EQ(backends::iso8601_epoch("not-a-date"), 0);
    // The format has a literal 'T', so a space-separated timestamp is rejected.
    EQ(backends::iso8601_epoch("2035-01-02 03:04:05"), 0);
    EQ(backends::iso8601_epoch("2035-01-02X03:04:05"), 0);
    EQ(backends::iso8601_epoch("garbage-but-exactly-19c"), 0);
    // A date with no time at all ("2035-01-02") is NOT asserted here, and must
    // not be: std::get_time hits end-of-input while looking for the literal 'T'
    // and the standard leaves it open whether that sets failbit. libc++ does not
    // - it returns a value for midnight - so the result is 2051222400 on macOS
    // and would be 0 on an implementation that does set it. Every input above
    // fails on a character that is present, which is well defined everywhere.

    EQS(backends::rfc7231_date(1'440'938'160), "Sun, 30 Aug 2015 12:36:00 GMT");
    EQS(backends::date_utc(1'440'938'160, "%Y%m%dT%H%M%SZ"), "20150830T123600Z");
    EQS(backends::date_utc(1'440'938'160, "%Y%m%d"), "20150830");
    EQS(backends::date_utc(0, "%Y-%m-%dT%H:%M:%SZ"), "1970-01-01T00:00:00Z");

    EQS(backends::range_header(10, 20), "bytes=10-29");
    EQS(backends::range_header(0, 1), "bytes=0-0");
    EQS(backends::range_header(0, 4096), "bytes=0-4095");
    // Guards against a 32-bit truncation regression in the offset arithmetic.
    EQS(backends::range_header(4'294'967'296ull, 2), "bytes=4294967296-4294967297");
}

// ---------------------------------------------------------------------------
// read_ini
// ---------------------------------------------------------------------------

void test_ini_reader() {
    SECTION("credential ini reader");
    const TempTree tree("helpers_ini");
    const std::string missing = tree.absent("absent.ini");
    auto unopenable = backends::read_ini(missing, "AWS config");
    expect_error(__LINE__, unopenable, KARU_ERR_CREDENTIALS,
                 "AWS config: cannot open '" + missing + "'");

    // CRLF is deliberate: std::getline leaves the '\r' behind and trim() has to
    // remove it, or every value on a Windows-authored profile keeps a stray CR.
    const std::string full = tree.write("full.ini", "# leading comment\r\n"
                                                    "; another comment\r\n"
                                                    "\r\n"
                                                    "   [  default  ]   \r\n"
                                                    "   AWS_Access_Key_ID   =   AKIA_ONE   \r\n"
                                                    "aws_secret_access_key: sec:ret=value\r\n"
                                                    "region = us-east-1\r\n"
                                                    "region = eu-west-1\r\n"
                                                    "endpoint = https://a.test/?x=1&y=2\r\n"
                                                    "inline = value # not a comment\r\n"
                                                    "no_separator_line\r\n"
                                                    "[profile dev]\r\n"
                                                    "region=ap-south-1\r\n");
    auto parsed = backends::read_ini(full, "AWS config");
    OK(parsed.has_value());
    if (parsed) {
        EQ(parsed->size(), 2u);
        const auto section = parsed->find("default");
        OK(section != parsed->end());
        if (section != parsed->end()) {
            const backends::IniSection& values = section->second;
            EQ(values.size(), 5u);
            EQS(values.at("aws_access_key_id"), "AKIA_ONE");
            // The split is on the FIRST of "=:", and the remainder is verbatim.
            EQS(values.at("aws_secret_access_key"), "sec:ret=value");
            EQS(values.at("region"), "eu-west-1");
            EQS(values.at("endpoint"), "https://a.test/?x=1&y=2");
            // There is no inline comment stripping.
            EQS(values.at("inline"), "value # not a comment");
            OK(!values.contains("no_separator_line"));
        }
        const auto dev = parsed->find("profile dev");
        OK(dev != parsed->end());
        if (dev != parsed->end())
            EQS(dev->second.at("region"), "ap-south-1");
    }

    // Keys written above the first [section] header are silently dropped, and
    // "[]" resets the section name to empty rather than naming a section.
    const std::string stray = tree.write("stray.ini", "orphan = 1\n"
                                                      "[]\n"
                                                      "also_orphan = 2\n"
                                                      "[real]\n"
                                                      "kept = 3\n");
    auto strays = backends::read_ini(stray, "AWS config");
    OK(strays.has_value());
    if (strays) {
        EQ(strays->size(), 1u);
        const auto real = strays->find("real");
        OK(real != strays->end());
        if (real != strays->end())
            EQS(real->second.at("kept"), "3");
    }

    const std::string empty = tree.write("empty.ini", "");
    auto nothing = backends::read_ini(empty, "AWS config");
    OK(nothing.has_value());
    if (nothing)
        OK(nothing->empty());
}

// ---------------------------------------------------------------------------
// copy_callback_credentials
// ---------------------------------------------------------------------------

std::string g_observed_path;
karu_credentials_kind g_observed_kind = KARU_CREDENTIALS_AWS;

karu_status filling_provider(void*, karu_credentials_kind kind, const char* canonical_path,
                             karu_credentials* out) {
    g_observed_path = canonical_path == nullptr ? "" : canonical_path;
    g_observed_kind = kind;
    out->access_key_id = "AK";
    out->secret_access_key = "SK";
    out->session_token = "ST";
    out->bearer_token = "BT";
    out->sas_token = "SAS";
    out->account_name = "acct";
    out->cache_prefix = "/vsis3/bucket";
    out->expires_at = 2'051'319'845;
    return KARU_OK;
}

karu_status silent_provider(void*, karu_credentials_kind, const char*, karu_credentials*) {
    return KARU_OK;
}

karu_status auth_failure_provider(void*, karu_credentials_kind, const char*, karu_credentials*) {
    return KARU_ERR_AUTH;
}

karu_status timeout_provider(void*, karu_credentials_kind, const char*, karu_credentials*) {
    return KARU_TIMEOUT;
}

karu_status drained_provider(void*, karu_credentials_kind, const char*, karu_credentials*) {
    return KARU_END;
}

// release stays null so ~CredentialCallback has nothing to call.
std::shared_ptr<CredentialCallback> wrap(karu_credentials_provider provider) {
    return std::make_shared<CredentialCallback>(provider, nullptr, nullptr);
}

void test_callback_credentials() {
    SECTION("custom credential provider results");
    g_observed_path.clear();
    auto filled = backends::copy_callback_credentials(wrap(filling_provider), KARU_CREDENTIALS_AWS,
                                                      "/vsis3/bucket/key");
    OK(filled.has_value());
    if (filled) {
        EQS(filled->access_key_id, "AK");
        EQS(filled->secret_access_key, "SK");
        EQS(filled->session_token, "ST");
        EQS(filled->bearer_token, "BT");
        EQS(filled->sas_token, "SAS");
        EQS(filled->account_name, "acct");
        EQS(filled->cache_prefix, "/vsis3/bucket");
        // region is never taken from the callback.
        EQS(filled->region, "");
        EQ(filled->expires_at, 2'051'319'845);
    }
    EQS(g_observed_path, "/vsis3/bucket/key");
    EQ(static_cast<int>(g_observed_kind), static_cast<int>(KARU_CREDENTIALS_AWS));

    // A callback that reports success without touching the struct leaves null
    // pointers behind; they must read as empty strings, never be dereferenced.
    auto silent = backends::copy_callback_credentials(wrap(silent_provider), KARU_CREDENTIALS_GCS,
                                                      "/vsigs/b/o");
    OK(silent.has_value());
    if (silent) {
        EQS(silent->access_key_id, "");
        EQS(silent->secret_access_key, "");
        EQS(silent->session_token, "");
        EQS(silent->bearer_token, "");
        EQS(silent->sas_token, "");
        EQS(silent->account_name, "");
        EQS(silent->cache_prefix, "");
        EQ(silent->expires_at, 0);
    }

    // A negative status is a real karu_status and is preserved verbatim.
    auto denied = backends::copy_callback_credentials(wrap(auth_failure_provider),
                                                      KARU_CREDENTIALS_GCS, "/vsigs/b/o");
    expect_error(__LINE__, denied, KARU_ERR_AUTH,
                 "/vsigs/b/o: custom credential provider returned not authorized");

    // A positive status is not an error code, so it is coerced.
    auto timed_out = backends::copy_callback_credentials(wrap(timeout_provider),
                                                         KARU_CREDENTIALS_AZURE, "/vsiaz/c/b");
    expect_error(__LINE__, timed_out, KARU_ERR_CREDENTIALS,
                 "/vsiaz/c/b: custom credential provider returned timed out");
    auto drained = backends::copy_callback_credentials(wrap(drained_provider),
                                                       KARU_CREDENTIALS_AZURE, "/vsiaz/c/b");
    expect_error(__LINE__, drained, KARU_ERR_CREDENTIALS,
                 "/vsiaz/c/b: custom credential provider returned batch drained");
}

// ---------------------------------------------------------------------------
// add_configured_headers
// ---------------------------------------------------------------------------

ConfigSnapshot headers_config(const char* value) {
    ConfigBuilder builder(false);
    if (value != nullptr)
        OK(builder.set("KARU_HTTP_HEADERS", value));
    return must_freeze(builder);
}

void test_configured_headers() {
    SECTION("KARU_HTTP_HEADERS validation");
    // The generated-header check runs before KARU_HTTP_HEADERS is even read, so
    // no option has to be set for these.
    const ConfigSnapshot bare = headers_config(nullptr);
    const std::vector<std::vector<Header>> invalid{
        {{"", "v"}}, {{"na:me", "v"}}, {{"name", "va\r\nlue"}}, {{"na\nme", "v"}}};
    for (const std::vector<Header>& seed : invalid) {
        std::vector<Header> generated = seed;
        auto rejected = backends::add_configured_headers(bare, "/vsis3/b/k", generated, true);
        expect_error(__LINE__, rejected, KARU_ERR_CONFIG, "a generated HTTP header is invalid");
    }

    for (const char* value : {"NoColonHere", ": novalue"}) {
        std::vector<Header> generated;
        auto rejected =
            backends::add_configured_headers(headers_config(value), "/vsis3/b/k", generated, true);
        expect_error(__LINE__, rejected, KARU_ERR_CONFIG,
                     "KARU_HTTP_HEADERS contains a malformed header");
    }
    {
        // The failure happens on the second line, after the first was already
        // appended: the caller's vector is left partially mutated.
        std::vector<Header> generated;
        auto rejected = backends::add_configured_headers(headers_config("Good: 1\nBadLine"),
                                                         "/vsis3/b/k", generated, true);
        expect_error(__LINE__, rejected, KARU_ERR_CONFIG,
                     "KARU_HTTP_HEADERS contains a malformed header");
        EQ(generated.size(), 1u);
    }

    SECTION("KARU_HTTP_HEADERS parsing");
    {
        std::vector<Header> parsed;
        auto added = backends::add_configured_headers(
            headers_config("X-One: 1\r\nX-Two:  two  \n\n\r\n  X-Three : 3\r\n"), "/vsis3/b/k",
            parsed, true);
        OK(added.has_value());
        EQ(parsed.size(), 3u);
        if (parsed.size() == 3) {
            EQS(parsed[0].first, "X-One");
            EQS(parsed[0].second, "1");
            EQS(parsed[1].first, "X-Two");
            EQS(parsed[1].second, "two");
            EQS(parsed[2].first, "X-Three");
            EQS(parsed[2].second, "3");
        }
    }
    // A run of newlines is skipped in one step, so the only way to produce an
    // empty trimmed line is a segment of spaces.
    for (const char* value : {"X-One: 1\n   ", "   \nX-One: 1", "X-Only: 1"}) {
        std::vector<Header> parsed;
        auto added =
            backends::add_configured_headers(headers_config(value), "/vsis3/b/k", parsed, true);
        OK(added.has_value());
        EQ(parsed.size(), 1u);
        if (parsed.size() == 1)
            EQS(parsed[0].second, "1");
    }
    {
        std::vector<Header> kept{{"X-Kept", "1"}};
        auto added = backends::add_configured_headers(bare, "/vsis3/b/k", kept, true);
        OK(added.has_value());
        EQ(kept.size(), 1u);
        if (kept.size() == 1) {
            EQS(kept[0].first, "X-Kept");
            EQS(kept[0].second, "1");
        }
    }

    SECTION("KARU_HTTP_HEADERS managed names");
    // The rejection message echoes the name as the user wrote it, trimmed but
    // not lowercased, even though the match is case-insensitive.
    struct Managed {
        const char* value;
        const char* rejected;
    };
    static constexpr Managed managed[]{{"Host: evil.test", "Host"},
                                       {"range: bytes=0-1", "range"},
                                       {"Authorization: Bearer x", "Authorization"},
                                       {"Date: now", "Date"},
                                       {"If-Match: \"e\"", "If-Match"},
                                       {"x-amz-acl: public", "x-amz-acl"},
                                       {"X-Goog-Meta-A: 1", "X-Goog-Meta-A"},
                                       {"x-ms-version: 2021", "x-ms-version"}};
    for (const Managed& entry : managed) {
        std::vector<Header> parsed;
        auto rejected = backends::add_configured_headers(headers_config(entry.value), "/vsis3/b/k",
                                                         parsed, true);
        expect_error(__LINE__, rejected, KARU_ERR_CONFIG,
                     std::string("KARU_HTTP_HEADERS cannot override Karu-managed header '") +
                         entry.rejected + "'");
    }
    {
        std::vector<Header> seeded{{"X-Dup", "1"}};
        auto rejected = backends::add_configured_headers(headers_config("x-dup: 2"), "/vsis3/b/k",
                                                         seeded, true);
        expect_error(__LINE__, rejected, KARU_ERR_CONFIG,
                     "KARU_HTTP_HEADERS cannot override Karu-managed header 'x-dup'");
    }
    {
        // Without request signing there is nothing to protect, so the signed
        // header set opens up - but Host and Range stay managed regardless.
        std::vector<Header> parsed;
        auto added = backends::add_configured_headers(
            headers_config("Authorization: Bearer x\nx-amz-acl: private\nDate: d\nIf-Match: e"),
            "/vsicurl/http://a.test/o", parsed, false);
        OK(added.has_value());
        EQ(parsed.size(), 4u);
        if (parsed.size() == 4) {
            EQS(parsed[0].first, "Authorization");
            EQS(parsed[3].first, "If-Match");
        }

        std::vector<Header> hosted;
        auto rejected = backends::add_configured_headers(headers_config("Host: x"),
                                                         "/vsicurl/http://a.test/o", hosted, false);
        expect_error(__LINE__, rejected, KARU_ERR_CONFIG,
                     "KARU_HTTP_HEADERS cannot override Karu-managed header 'Host'");
    }
}

// ---------------------------------------------------------------------------
// credential_request: the cases that need no server
// ---------------------------------------------------------------------------

void test_credential_request_offline() {
    SECTION("credential request validation");
    // Nothing here should reach the network, but an ambient proxy would still
    // have curl open a socket to it, so the guard stays on for the whole case.
    const DirectTransport direct;
    // The header check precedes every curl option, so no socket is opened even
    // though the URL names a port.
    const std::vector<std::vector<Header>> invalid{
        {{"", "v"}}, {{"na:me", "v"}}, {{"name", "va\r\nlue"}}, {{"na\rme", "v"}}};
    for (const std::vector<Header>& seed : invalid) {
        auto rejected =
            backends::credential_request("GET", "http://127.0.0.1:1/x", "", seed, 5, {});
        expect_error(__LINE__, rejected, KARU_ERR_CREDENTIALS, "credential header is invalid");
    }

    // CURLOPT_PROTOCOLS_STR restricts the handle to http and https; curl
    // rejects anything else locally, without touching the network. The file URL
    // deliberately names a path that cannot exist rather than a real one like
    // /etc/hosts: if the protocol restriction ever regressed, this case must not
    // be able to read a file off the developer's machine even momentarily.
    //
    // These run with default options, so there is no HTTP/2 build gate here and
    // the failure is always the perform-time spelling. Assert it exactly: the
    // tolerant "credential endpoint" prefix would also accept a setup failure,
    // which is a different bug wearing the same message.
    for (const char* url : {"file:///karu-no-such-file-6f2a", "ftp://127.0.0.1/x"}) {
        auto rejected = backends::credential_request("GET", url, "", {}, 5, {});
        expect_error_prefix(__LINE__, rejected, KARU_ERR_CREDENTIALS, "credential endpoint: ");
    }

    // "http://" has no host, so curl fails URL parsing before resolving.
    auto malformed = backends::credential_request("GET", "http://", "", {}, 5, {});
    expect_error_prefix(__LINE__, malformed, KARU_ERR_CREDENTIALS, "credential endpoint: ");

    // The HTTP/2 arms of curl_http_version, swept without needing a server that
    // speaks h2. This is the one place the tolerant prefix is earned: a libcurl
    // built without HTTP/2 rejects CURL_HTTP_VERSION_2TLS and _2_PRIOR_KNOWLEDGE
    // at setopt time and says "credential endpoint setup: " instead.
    for (const HttpVersion version : {HttpVersion::Http2Tls, HttpVersion::Http2PriorKnowledge}) {
        HttpRequestOptions options;
        options.version = version;
        auto rejected = backends::credential_request("GET", "http://", "", {}, 5, options);
        expect_error_prefix(__LINE__, rejected, KARU_ERR_CREDENTIALS, "credential endpoint");
    }

    // A connection-refused case is deliberately absent. It adds no line that the
    // two cases above do not already cover, and its cost is the one thing here
    // that depends on the host's network stack: a port that answers with RST is
    // instant, but a port that silently drops turns it into a full
    // CONNECTTIMEOUT second.

    // oauth_token forwards a transport failure verbatim rather than reshaping
    // it into its own "returned HTTP" wording.
    auto forwarded = backends::oauth_token("http://", "x=1", {}, {});
    expect_error_prefix(__LINE__, forwarded, KARU_ERR_CREDENTIALS, "credential endpoint: ");
    OK(!forwarded && !forwarded.error().message.starts_with("credential endpoint returned HTTP"));
}

// ---------------------------------------------------------------------------
// credential_request against the loopback fixture
// ---------------------------------------------------------------------------

void test_credential_request_transport() {
    SECTION("credential request transport");
    const DirectTransport direct;
    const TempTree tree("helpers_transport");

    reset_fixture_requests(__LINE__);
    HttpRequestOptions options;
    options.version = HttpVersion::Automatic;
    options.user_agent = "karu-test/1";
    // An http:// URL never consults the CA bundle, so the path need not exist.
    options.ca_bundle = tree.absent("not-a-real-ca.pem");
    const std::vector<Header> extra{{"X-Karu-Test", "abc"}};
    auto fetched =
        backends::credential_request("GET", fixture_url("/oauth/ok"), "", extra, 5, options);
    OK(fetched.has_value());
    if (fetched) {
        EQ(fetched->status, 200);
        expect_text(__LINE__, backends::json_string(fetched->body, "access_token"),
                    "oauth-access-token");
    }
    const std::string sent = fixture_request_body(__LINE__, "/_requests");
    expect_text(__LINE__, backends::json_string(sent, "method"), "GET");
    expect_text(__LINE__, backends::json_string(sent, "x-karu-test"), "abc");
    expect_text(__LINE__, backends::json_string(sent, "user-agent"), "karu-test/1");

    // CURLOPT_CAPATH is not supported by every TLS backend (Schannel rejects it
    // outright), so both outcomes are legitimate here.
    HttpRequestOptions with_ca_path;
    with_ca_path.ca_path = tree.root();
    auto ca_path_result =
        backends::credential_request("GET", fixture_url("/oauth/ok"), "", {}, 5, with_ca_path);
    if (ca_path_result)
        EQ(ca_path_result->status, 200);
    else
        expect_error_prefix(__LINE__, ca_path_result, KARU_ERR_CREDENTIALS,
                            "credential endpoint setup: ");

    // credential_request deliberately does not interpret the status; only
    // oauth_token does. A 403 is a successful transfer.
    auto forbidden = backends::credential_request("GET", fixture_url("/status/403"), "", {}, 5, {});
    OK(forbidden.has_value());
    if (forbidden) {
        EQ(forbidden->status, 403);
        EQS(forbidden->body, "status fixture");
    }

    // A method that is neither GET nor POST goes to CURLOPT_CUSTOMREQUEST. The
    // method is a string_view, so passing a temporary is the regression guard
    // for the stable_method copy the implementation keeps.
    reset_fixture_requests(__LINE__);
    auto put = backends::credential_request(std::string("PUT"), fixture_url("/oauth/ok"),
                                            "payload-123", {}, 5, {});
    OK(put.has_value());
    if (put)
        EQ(put->status, 200);
    const std::string put_log = fixture_request_body(__LINE__, "/_requests");
    expect_text(__LINE__, backends::json_string(put_log, "method"), "PUT");
    expect_text(__LINE__, backends::json_string(put_log, "body"), "payload-123");

    // The same custom-method path with an empty body never sets POSTFIELDS.
    reset_fixture_requests(__LINE__);
    auto bodiless = backends::credential_request("PUT", fixture_url("/oauth/ok"), "", {}, 5, {});
    OK(bodiless.has_value());
    if (bodiless)
        EQ(bodiless->status, 200);
    const std::string bodiless_log = fixture_request_body(__LINE__, "/_requests");
    expect_text(__LINE__, backends::json_string(bodiless_log, "method"), "PUT");
    expect_text(__LINE__, backends::json_string(bodiless_log, "body"), "");
}

void test_credential_request_proxy() {
    SECTION("credential request proxy options");
    // A .invalid host can never resolve, so this only succeeds if CURLOPT_PROXY
    // is really applied and curl sends the absolute-form request line.
    const ProxiedTransport proxied;
    HttpRequestOptions options;
    options.proxy = fixture_url("");
    options.proxy_user_password = "user:pass";
    auto proxied_fetch = backends::credential_request("GET", "http://credentials.invalid/oauth/ok",
                                                      "", {}, 5, options);
    OK(proxied_fetch.has_value());
    if (proxied_fetch) {
        EQ(proxied_fetch->status, 200);
        expect_text(__LINE__, backends::json_string(proxied_fetch->body, "access_token"),
                    "oauth-access-token");
    }
}

// ---------------------------------------------------------------------------
// oauth_token
// ---------------------------------------------------------------------------

void test_oauth_token() {
    SECTION("oauth token exchange");
    const DirectTransport direct;

    reset_fixture_requests(__LINE__);
    const std::int64_t before = now_seconds();
    auto token =
        backends::oauth_token(fixture_url("/oauth/ok"), "grant_type=client_credentials&scope=a+b",
                              {{"X-Karu-Extra", "yes"}}, {});
    const std::int64_t after = now_seconds();
    OK(token.has_value());
    if (token) {
        EQS(token->bearer_token, "oauth-access-token");
        expect_expiry(__LINE__, token->expires_at, before, after, 3600);
        EQS(token->access_key_id, "");
        EQS(token->secret_access_key, "");
        EQS(token->session_token, "");
        EQS(token->sas_token, "");
        EQS(token->account_name, "");
        EQS(token->region, "");
        EQS(token->cache_prefix, "");
    }
    // The form body reaches the endpoint intact and oauth_token appends the
    // form content type itself.
    const std::string sent = fixture_request_body(__LINE__, "/_requests");
    expect_text(__LINE__, backends::json_string(sent, "method"), "POST");
    expect_text(__LINE__, backends::json_string(sent, "body"),
                "grant_type=client_credentials&scope=a+b");
    expect_text(__LINE__, backends::json_string(sent, "content-type"),
                "application/x-www-form-urlencoded");
    expect_text(__LINE__, backends::json_string(sent, "x-karu-extra"), "yes");

    // The 60 second floor keeps the credential cache from re-fetching a
    // very short token on every single request.
    const std::int64_t short_before = now_seconds();
    auto short_token = backends::oauth_token(fixture_url("/oauth/short"), "x=1", {}, {});
    const std::int64_t short_after = now_seconds();
    OK(short_token.has_value());
    if (short_token) {
        EQS(short_token->bearer_token, "short-lived-token");
        expect_expiry(__LINE__, short_token->expires_at, short_before, short_after, 60);
    }

    // A response with no expires_in at all falls back to one hour. This route
    // belongs to the GCS metadata fixtures but answers any method, and it is
    // the only response shape on the server that omits the field.
    const std::int64_t default_before = now_seconds();
    auto defaulted =
        backends::oauth_token(fixture_url("/gcs/metadata/no-expires-in"), "x=1", {}, {});
    const std::int64_t default_after = now_seconds();
    OK(defaulted.has_value());
    if (defaulted) {
        EQS(defaulted->bearer_token, "gce-metadata-token");
        expect_expiry(__LINE__, defaulted->expires_at, default_before, default_after, 3600);
    }

    auto no_token = backends::oauth_token(fixture_url("/oauth/no-token"), "x=1", {}, {});
    expect_error(__LINE__, no_token, KARU_ERR_CREDENTIALS,
                 "credential response has no access_token");
    // A response that is not JSON at all reads the same way.
    auto not_json = backends::oauth_token(fixture_url("/status/200"), "x=1", {}, {});
    expect_error(__LINE__, not_json, KARU_ERR_CREDENTIALS,
                 "credential response has no access_token");

    auto denied = backends::oauth_token(fixture_url("/oauth/denied"), "x=1", {}, {});
    expect_error(__LINE__, denied, KARU_ERR_CREDENTIALS, "credential endpoint returned HTTP 400");
    auto broken = backends::oauth_token(fixture_url("/status/500"), "x=1", {}, {});
    expect_error(__LINE__, broken, KARU_ERR_CREDENTIALS, "credential endpoint returned HTTP 500");
    // credential_request never sets CURLOPT_FOLLOWLOCATION, so a credential
    // endpoint is never redirect-followed. That is a security property.
    //
    // The route matters: /status/302 sends no Location header, so curl would
    // stop there even WITH CURLOPT_FOLLOWLOCATION and the case would prove
    // nothing. /redirect/ipv6 sends a real Location (http://[::1]:9/object), so
    // a regression that turned redirect-following on would chase it and this
    // assertion would see a transport error instead of HTTP 302.
    auto redirected = backends::oauth_token(fixture_url("/redirect/ipv6"), "x=1", {}, {});
    expect_error(__LINE__, redirected, KARU_ERR_CREDENTIALS,
                 "credential endpoint returned HTTP 302");
}

// ---------------------------------------------------------------------------
// read_aws_profile
// ---------------------------------------------------------------------------

backends::AwsProfile must_read_profile(int line, const ConfigSnapshot& config,
                                       std::string_view profile) {
    auto result = backends::read_aws_profile(config, "/vsis3/b/k", profile, "AWS_CONFIG_FILE",
                                             "AWS_SHARED_CREDENTIALS_FILE", "AWS");
    if (!result) {
        fail(line, "read_aws_profile failed: " + result.error().message);
        return {};
    }
    ok_at(line, true, "read_aws_profile");
    return std::move(*result);
}

void test_aws_profile_reader() {
    SECTION("AWS profile discovery");
    // The hermeticity anchor for the whole file: ConfigBuilder(false) leaves the
    // home directory empty, so default_aws_path() is "" and path_exists("") is
    // a clean false. The developer's real ~/.aws is never opened.
    const ConfigSnapshot bare = must_freeze(ConfigBuilder(false));
    EQS(bare.default_aws_path("config"), "");
    const backends::AwsProfile nothing = must_read_profile(__LINE__, bare, "default");
    OK(!nothing.found);
    OK(nothing.values.empty());

    EQS(nothing.value("region"), "");

    const TempTree tree("helpers_profile");
    const std::string config_file = tree.write("config", "[default]\n"
                                                         "region = us-east-1\n"
                                                         "[profile dev]\n"
                                                         "region = eu-west-1\n"
                                                         "aws_access_key_id = AKIA_CFG\n");
    ConfigBuilder config_only(false);
    OK(config_only.set("AWS_CONFIG_FILE", config_file.c_str()));
    const ConfigSnapshot config_snapshot = must_freeze(config_only);

    // The config file names a non-default profile as "profile NAME"...
    const backends::AwsProfile dev = must_read_profile(__LINE__, config_snapshot, "dev");
    OK(dev.found);
    EQS(dev.value("region"), "eu-west-1");
    EQS(dev.value("aws_access_key_id"), "AKIA_CFG");
    // ...but "default" is spelled bare.
    const backends::AwsProfile fallback = must_read_profile(__LINE__, config_snapshot, "default");
    OK(fallback.found);
    EQS(fallback.value("region"), "us-east-1");
    const backends::AwsProfile absent = must_read_profile(__LINE__, config_snapshot, "missing");
    OK(!absent.found);
    OK(absent.values.empty());
    // The map is keyed on the lowercased name read_ini produces, which is why
    // every call site passes a lowercase literal.
    EQS(dev.value("Region"), "");

    // The shared credentials file uses the bare profile name, never "profile
    // NAME". The asymmetry between the two files is the bug-prone part.
    const std::string credentials_file =
        tree.write("credentials", "[dev]\n"
                                  "aws_access_key_id = AKIA_CRED\n"
                                  "aws_secret_access_key = SECRET_CRED\n");
    ConfigBuilder credentials_only(false);
    OK(credentials_only.set("AWS_SHARED_CREDENTIALS_FILE", credentials_file.c_str()));
    const backends::AwsProfile shared =
        must_read_profile(__LINE__, must_freeze(credentials_only), "dev");
    OK(shared.found);
    EQS(shared.value("aws_access_key_id"), "AKIA_CRED");
    EQS(shared.value("aws_secret_access_key"), "SECRET_CRED");

    const std::string prefixed = tree.write("prefixed", "[profile dev]\n"
                                                        "aws_access_key_id = AKIA_WRONG\n");
    ConfigBuilder prefixed_builder(false);
    OK(prefixed_builder.set("AWS_SHARED_CREDENTIALS_FILE", prefixed.c_str()));
    const backends::AwsProfile not_found =
        must_read_profile(__LINE__, must_freeze(prefixed_builder), "dev");
    OK(!not_found.found);

    // Both files, merged: the credentials file wins key by key, and config-only
    // keys survive rather than being erased.
    const std::string merge_config =
        tree.write("merge-config", "[profile dev]\n"
                                   "region = eu-west-1\n"
                                   "aws_access_key_id = FROM_CONFIG\n"
                                   "credential_process = from-config\n");
    const std::string merge_credentials =
        tree.write("merge-credentials", "[dev]\n"
                                        "aws_access_key_id = FROM_CREDENTIALS\n"
                                        "aws_session_token = TOK\n");
    ConfigBuilder merged(false);
    OK(merged.set("AWS_CONFIG_FILE", merge_config.c_str()));
    OK(merged.set("AWS_SHARED_CREDENTIALS_FILE", merge_credentials.c_str()));
    const backends::AwsProfile combined = must_read_profile(__LINE__, must_freeze(merged), "dev");
    OK(combined.found);
    EQ(combined.values.size(), 4u);
    EQS(combined.value("aws_access_key_id"), "FROM_CREDENTIALS");
    EQS(combined.value("aws_session_token"), "TOK");
    EQS(combined.value("region"), "eu-west-1");
    EQS(combined.value("credential_process"), "from-config");
}

void test_aws_profile_path_scope() {
    SECTION("AWS profile path-specific options");
    // read_aws_profile takes a path, and both option lookups have to honour the
    // path layer - the longest matching prefix wins over the client-wide value.
    const TempTree tree("helpers_profile_scope");
    const std::string wide = tree.write("wide", "[default]\nregion = us-east-1\n");
    const std::string scoped = tree.write("scoped", "[default]\nregion = eu-west-1\n");
    ConfigBuilder builder(false);
    OK(builder.set("AWS_CONFIG_FILE", wide.c_str()));
    OK(builder.set_path("/vsis3/bucket", "AWS_CONFIG_FILE", scoped.c_str()));
    const ConfigSnapshot config = must_freeze(builder);

    auto inside =
        backends::read_aws_profile(config, "/vsis3/bucket/key", "default", "AWS_CONFIG_FILE",
                                   "AWS_SHARED_CREDENTIALS_FILE", "AWS");
    OK(inside.has_value());
    if (inside) {
        OK(inside->found);
        EQS(inside->value("region"), "eu-west-1");
    }
    auto outside =
        backends::read_aws_profile(config, "/vsis3/other/key", "default", "AWS_CONFIG_FILE",
                                   "AWS_SHARED_CREDENTIALS_FILE", "AWS");
    OK(outside.has_value());
    if (outside) {
        OK(outside->found);
        EQS(outside->value("region"), "us-east-1");
    }
}

void test_aws_profile_errors() {
    SECTION("AWS profile errors");
    const TempTree tree("helpers_profile_errors");

    // An unset option with a missing default is fine; an explicitly named file
    // that does not exist is an error.
    const std::string missing_config = tree.absent("no-such-config");
    ConfigBuilder named(false);
    OK(named.set("AWS_CONFIG_FILE", missing_config.c_str()));
    auto missing =
        backends::read_aws_profile(must_freeze(named), "/vsis3/b/k", "default", "AWS_CONFIG_FILE",
                                   "AWS_SHARED_CREDENTIALS_FILE", "AWS");
    expect_error(__LINE__, missing, KARU_ERR_CREDENTIALS,
                 "AWS config file does not exist: '" + missing_config + "'");

    // The label is threaded through every message, which is what lets Source
    // Cooperative reuse the reader with its own option namespace.
    ConfigBuilder source(false);
    OK(source.set("SOURCE_CONFIG_FILE", missing_config.c_str()));
    auto source_missing = backends::read_aws_profile(must_freeze(source), "/vsisource/b/k",
                                                     "default", "SOURCE_CONFIG_FILE",
                                                     "SOURCE_SHARED_CREDENTIALS_FILE", "Source");
    expect_error(__LINE__, source_missing, KARU_ERR_CREDENTIALS,
                 "Source config file does not exist: '" + missing_config + "'");

    // Reached only after the config file was read, so it pins the ordering too.
    const std::string good_config = tree.write("config", "[default]\nregion = us-east-1\n");
    const std::string missing_credentials = tree.absent("no-such-credentials");
    ConfigBuilder half(false);
    OK(half.set("AWS_CONFIG_FILE", good_config.c_str()));
    OK(half.set("AWS_SHARED_CREDENTIALS_FILE", missing_credentials.c_str()));
    auto half_missing =
        backends::read_aws_profile(must_freeze(half), "/vsis3/b/k", "default", "AWS_CONFIG_FILE",
                                   "AWS_SHARED_CREDENTIALS_FILE", "AWS");
    expect_error(__LINE__, half_missing, KARU_ERR_CREDENTIALS,
                 "AWS shared credentials file does not exist: '" + missing_credentials + "'");

    // Both path_exists calls propagate, and they are separate returns. The first
    // one (the config file) is reached by oversizing AWS_CONFIG_FILE...
    const std::string oversized_path(5000, 'x');
    ConfigBuilder uninspectable_config(false);
    OK(uninspectable_config.set("AWS_CONFIG_FILE", oversized_path.c_str()));
    auto uninspectable_config_result =
        backends::read_aws_profile(must_freeze(uninspectable_config), "/vsis3/b/k", "default",
                                   "AWS_CONFIG_FILE", "AWS_SHARED_CREDENTIALS_FILE", "AWS");
    OK(!uninspectable_config_result.has_value());
    if (!uninspectable_config_result)
        EQ(uninspectable_config_result.error().status, KARU_ERR_CREDENTIALS);
#ifndef _WIN32
    expect_error_prefix(__LINE__, uninspectable_config_result, KARU_ERR_CREDENTIALS,
                        "AWS config file: cannot inspect '");
#endif

    // ...and with AWS_CONFIG_FILE unset the first path_exists returns a clean
    // false, so the oversized path is inspected by the second one instead.
    ConfigBuilder uninspectable(false);
    OK(uninspectable.set("AWS_SHARED_CREDENTIALS_FILE", oversized_path.c_str()));
    auto uninspectable_result =
        backends::read_aws_profile(must_freeze(uninspectable), "/vsis3/b/k", "default",
                                   "AWS_CONFIG_FILE", "AWS_SHARED_CREDENTIALS_FILE", "AWS");
    OK(!uninspectable_result.has_value());
    if (!uninspectable_result)
        EQ(uninspectable_result.error().status, KARU_ERR_CREDENTIALS);
#ifndef _WIN32
    // POSIX-verified only: the MS STL may report "not found" with a cleared
    // error code, which produces the "does not exist" message instead.
    expect_error_prefix(__LINE__, uninspectable_result, KARU_ERR_CREDENTIALS,
                        "AWS shared credentials file: cannot inspect '");
#endif

    // An oversized file is the only fixture that fails to read identically on
    // all three platforms: a directory succeeds on macOS, sets badbit on Linux
    // and fails to open on Windows.
    const std::string oversized = "[default]\n" + std::string(1'100'000, 'a');
    const std::string big_config = tree.write("big-config", oversized);
    ConfigBuilder huge_config(false);
    OK(huge_config.set("AWS_CONFIG_FILE", big_config.c_str()));
    auto unreadable_config =
        backends::read_aws_profile(must_freeze(huge_config), "/vsis3/b/k", "default",
                                   "AWS_CONFIG_FILE", "AWS_SHARED_CREDENTIALS_FILE", "AWS");
    expect_error(__LINE__, unreadable_config, KARU_ERR_CREDENTIALS,
                 "AWS config: file exceeds 1 MiB");

    const std::string big_credentials = tree.write("big-credentials", oversized);
    ConfigBuilder huge_credentials(false);
    OK(huge_credentials.set("AWS_CONFIG_FILE", good_config.c_str()));
    OK(huge_credentials.set("AWS_SHARED_CREDENTIALS_FILE", big_credentials.c_str()));
    auto unreadable_credentials =
        backends::read_aws_profile(must_freeze(huge_credentials), "/vsis3/b/k", "default",
                                   "AWS_CONFIG_FILE", "AWS_SHARED_CREDENTIALS_FILE", "AWS");
    // Note the purpose string has no " file" suffix here, unlike the one the
    // path_exists call above uses.
    expect_error(__LINE__, unreadable_credentials, KARU_ERR_CREDENTIALS,
                 "AWS shared credentials: file exceeds 1 MiB");
}

// ---------------------------------------------------------------------------
// aws_json_credentials and credentials_from_aws_profile
// ---------------------------------------------------------------------------

void test_aws_json_credentials() {
    SECTION("AWS credential documents");
    const ProviderCredentials full = backends::aws_json_credentials(
        R"({"Version":1,"AccessKeyId":"AKIA1","SecretAccessKey":"SEC","Token":"TOK",)"
        R"("Expiration":"2035-01-02T03:04:05Z"})");
    EQS(full.access_key_id, "AKIA1");
    EQS(full.secret_access_key, "SEC");
    EQS(full.session_token, "TOK");
    EQ(full.expires_at, 2'051'319'845);
    // This function never sets a region; the caller supplies it.
    EQS(full.region, "");
    EQS(full.bearer_token, "");
    EQS(full.sas_token, "");
    EQS(full.account_name, "");

    const ProviderCredentials fallback =
        backends::aws_json_credentials(R"({"AccessKeyId":"A","SecretAccessKey":"S",)"
                                       R"("SessionToken":"ST"})");
    EQS(fallback.session_token, "ST");
    // Token wins when both spellings are present.
    const ProviderCredentials both =
        backends::aws_json_credentials(R"({"Token":"T","SessionToken":"ST"})");
    EQS(both.session_token, "T");

    for (const char* document : {"", "not json"}) {
        const ProviderCredentials nothing = backends::aws_json_credentials(document);
        EQS(nothing.access_key_id, "");
        EQS(nothing.secret_access_key, "");
        EQS(nothing.session_token, "");
        EQ(nothing.expires_at, 0);
    }
    // An unparseable Expiration takes the branch but yields 0, which is
    // indistinguishable from having no Expiration at all.
    const ProviderCredentials bad_expiry = backends::aws_json_credentials(
        R"({"AccessKeyId":"A","SecretAccessKey":"S","Expiration":"garbage"})");
    EQ(bad_expiry.expires_at, 0);
    const ProviderCredentials no_expiry =
        backends::aws_json_credentials(R"({"AccessKeyId":"A","SecretAccessKey":"S"})");
    EQ(no_expiry.expires_at, 0);
}

backends::AwsProfile profile_with(std::string_view name, std::string value) {
    backends::AwsProfile profile;
    profile.found = true;
    profile.values.emplace(std::string(name), std::move(value));
    return profile;
}

void test_credentials_from_profile() {
    SECTION("AWS profile credentials");
    auto sso =
        backends::credentials_from_aws_profile(profile_with("sso_session", "my-sso"), "AWS", 5);
    expect_error(__LINE__, sso, KARU_ERR_CREDENTIALS,
                 "AWS IAM Identity Center profiles require a custom credential provider");

    // The SSO rejection wins over otherwise usable static keys, and the label
    // is threaded through.
    backends::AwsProfile start_url = profile_with("sso_start_url", "https://d-1.awsapps.com/start");
    start_url.values.emplace("aws_access_key_id", "A");
    start_url.values.emplace("aws_secret_access_key", "S");
    auto started = backends::credentials_from_aws_profile(start_url, "Source", 5);
    expect_error(__LINE__, started, KARU_ERR_CREDENTIALS,
                 "Source IAM Identity Center profiles require a custom credential provider");

    const std::pair<const char*, const char*> assume_role[]{
        {"role_arn", "arn:aws:iam::1:role/r"},
        {"source_profile", "base"},
        {"web_identity_token_file", "/tmp/token"}};
    for (const auto& [key, value] : assume_role) {
        auto rejected = backends::credentials_from_aws_profile(profile_with(key, value), "AWS", 5);
        expect_error(__LINE__, rejected, KARU_ERR_CREDENTIALS,
                     "AWS AssumeRole profiles require a custom credential provider");
    }

    backends::AwsProfile keys = profile_with("aws_access_key_id", "AKIA_X");
    keys.values.emplace("aws_secret_access_key", "SEC_X");
    keys.values.emplace("aws_session_token", "TOK_X");
    keys.values.emplace("region", "ap-south-1");
    auto statics = backends::credentials_from_aws_profile(keys, "AWS", 5);
    OK(statics.has_value());
    if (statics) {
        EQS(statics->access_key_id, "AKIA_X");
        EQS(statics->secret_access_key, "SEC_X");
        EQS(statics->session_token, "TOK_X");
        EQS(statics->region, "ap-south-1");
        EQS(statics->bearer_token, "");
        EQ(statics->expires_at, 0);
    }

    // A profile with no keys at all is a SUCCESS with empty credentials. It is
    // the caller that turns that into "did not provide credentials".
    auto regional =
        backends::credentials_from_aws_profile(profile_with("region", "us-east-1"), "AWS", 5);
    OK(regional.has_value());
    if (regional) {
        EQS(regional->access_key_id, "");
        EQS(regional->secret_access_key, "");
        EQS(regional->region, "us-east-1");
    }

    for (const char* key : {"aws_access_key_id", "aws_secret_access_key"}) {
        auto lonely = backends::credentials_from_aws_profile(profile_with(key, "VALUE"), "AWS", 5);
        expect_error(__LINE__, lonely, KARU_ERR_CREDENTIALS,
                     "AWS profile access key and secret must be set together");
    }
}

void test_credential_process() {
    SECTION("AWS credential_process");
    // Sanitized binaries can take several seconds to start on a busy runner.
    // Only the dedicated hang case below is meant to exercise a short timeout.
    constexpr long helper_timeout = 15;
    // The helper is this test binary re-invoked through the platform shell,
    // which is the only spelling that behaves the same under sh and cmd.exe.
    backends::AwsProfile helper =
        profile_with("credential_process", credential_process_command("ok"));
    helper.values.emplace("region", "sa-east-1");
    const std::int64_t before = now_seconds();
    auto produced = backends::credentials_from_aws_profile(helper, "AWS", helper_timeout);
    OK(produced.has_value());
    if (produced) {
        EQS(produced->access_key_id, "AKIAHELPER");
        EQS(produced->secret_access_key, "helper-secret");
        EQS(produced->session_token, "helper-token");
        // The region comes from the profile, never from the helper's document.
        EQS(produced->region, "sa-east-1");
        OK(produced->expires_at > before);
    }

    // Both halves of the incompleteness test. The harness mode emits an
    // AccessKeyId and no SecretAccessKey, which is the second disjunct; a helper
    // whose output is not a credential document at all leaves BOTH empty and is
    // the only way to evaluate the first one. "echo hello" is the same string
    // under sh and cmd.exe, so it needs no #ifdef.
    auto incomplete = backends::credentials_from_aws_profile(
        profile_with("credential_process", credential_process_command("incomplete")), "AWS",
        helper_timeout);
    expect_error(__LINE__, incomplete, KARU_ERR_CREDENTIALS,
                 "AWS credential_process response is incomplete");

    auto not_a_document = backends::credentials_from_aws_profile(
        profile_with("credential_process", "echo hello"), "AWS", helper_timeout);
    expect_error(__LINE__, not_a_document, KARU_ERR_CREDENTIALS,
                 "AWS credential_process response is incomplete");

    auto failed = backends::credentials_from_aws_profile(
        profile_with("credential_process", credential_process_command("fail")), "AWS",
        helper_timeout);
    expect_error(__LINE__, failed, KARU_ERR_CREDENTIALS,
                 "AWS credential_process exited with status 3");

    // A helper that cannot be executed is reported by the shell as an exit
    // code, not as a spawn failure: sh says 127, cmd.exe says 9009 or 1.
    const TempTree tree("helpers_process");
    auto not_found = backends::credentials_from_aws_profile(
        profile_with("credential_process", "\"" + tree.absent("karu-no-such-helper") + "\""), "AWS",
        helper_timeout);
    expect_error_prefix(__LINE__, not_found, KARU_ERR_CREDENTIALS,
                        "AWS credential_process exited with status ");

    auto flooded = backends::credentials_from_aws_profile(
        profile_with("credential_process", credential_process_command("flood")), "AWS",
        helper_timeout);
    expect_error(__LINE__, flooded, KARU_ERR_CREDENTIALS,
                 "AWS credential_process returned more than 1 MiB");

    // os::run_command rejects a command containing a NUL before creating any
    // process, which is the only way to reach the "could not start" branch.
    auto unstartable = backends::credentials_from_aws_profile(
        profile_with("credential_process", std::string("echo a\0b", 8)), "AWS", 5);
    expect_error_prefix(__LINE__, unstartable, KARU_ERR_CREDENTIALS,
                        "AWS credential_process could not start: ");

    // The one deliberately slow case in this file. The helper sleeps 30 s and
    // the deadline is 1 s, so the kill path has to work for this to return.
    const auto started = std::chrono::steady_clock::now();
    auto timed_out = backends::credentials_from_aws_profile(
        profile_with("credential_process", credential_process_command("hang")), "AWS", 1);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    expect_error(__LINE__, timed_out, KARU_TIMEOUT,
                 "AWS credential_process exceeded the request timeout");
    OK(elapsed < std::chrono::seconds(3));
}

} // namespace

void test_credential_helpers() {
    test_file_helpers();
    test_json_scanner();
    test_time_and_range_helpers();
    test_ini_reader();
    test_callback_credentials();
    test_configured_headers();
    test_credential_request_offline();
    test_credential_request_transport();
    test_credential_request_proxy();
    test_oauth_token();
    test_aws_profile_reader();
    test_aws_profile_path_scope();
    test_aws_profile_errors();
    test_aws_json_credentials();
    test_credentials_from_profile();
    test_credential_process();
}

} // namespace karu::test
