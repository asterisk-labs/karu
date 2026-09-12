#include "backends/credentials.hpp"
#include "backends/gcs_credentials.hpp"
#include "config_options.hpp"
#include "credential_support.hpp"
#include "test_support.hpp"
#include "text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <list>
#include <memory>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <string>
#include <string_view>
#include <vector>

namespace karu::test {
namespace {

using Loaded = std::expected<ProviderCredentials, RequestError>;

// Every case resolves credentials for the same object, so a path-scoped rule
// written for "gs://bucket/" applies to it.
constexpr std::string_view OBJECT_PATH = "/vsigs/bucket/object";

Loaded load(const ConfigBuilder& builder) {
    return backends::load_gcs_credentials(must_freeze(builder), OBJECT_PATH);
}

// A failure is only interesting if the status and the wording both match: the
// loader has a dozen distinct messages and they are how a user tells the ADC
// arm apart from the private-key arm.
void check_error(int line, const Loaded& result, std::string_view expected) {
    ++checks;
    if (result) {
        fail(line, "expected a credential failure but the load succeeded");
        return;
    }
    if (result.error().status != KARU_ERR_CREDENTIALS)
        fail(line, "expected KARU_ERR_CREDENTIALS, got status " +
                       std::to_string(static_cast<int>(result.error().status)));
    if (result.error().message != expected) {
        fail(line, "message was '" + result.error().message + "', expected '" +
                       std::string(expected) + "'");
    }
}

// curl phrases transport refusals differently across versions, so those cases
// pin only the prefix the loader itself contributes.
void check_error_prefix(int line, const Loaded& result, std::string_view expected) {
    ++checks;
    if (result) {
        fail(line, "expected a credential failure but the load succeeded");
        return;
    }
    if (result.error().status != KARU_ERR_CREDENTIALS)
        fail(line, "expected KARU_ERR_CREDENTIALS, got status " +
                       std::to_string(static_cast<int>(result.error().status)));
    if (!result.error().message.starts_with(expected)) {
        fail(line, "message '" + result.error().message + "' does not start with '" +
                       std::string(expected) + "'");
    }
}

// Token lifetimes are computed from the clock inside the loader, and the
// fixture's own timestamps from the clock inside the server, so both can only
// land at or AFTER the second the case sampled. The window is one-sided on
// purpose: an expiry that comes back earlier than `expected` means the lifetime
// itself was wrong, never that a second ticked over.
void check_expiry(int line, std::int64_t actual, std::int64_t expected) {
    ++checks;
    if (actual < expected || actual > expected + 5) {
        fail(line, "expires_at " + std::to_string(actual) + " is not close to " +
                       std::to_string(expected));
    }
}

void check_empty(int line, const Loaded& result) {
    ++checks;
    if (!result) {
        fail(line, "expected anonymous credentials, got '" + result.error().message + "'");
        return;
    }
    if (!result->bearer_token.empty() || !result->access_key_id.empty() ||
        !result->secret_access_key.empty() || !result->session_token.empty() ||
        result->expires_at != 0) {
        fail(line, "expected an all-empty ProviderCredentials");
    }
}

// How many times the fixture has served a route. Cases that must prove a
// request never left the process read this before and after, which stays
// correct however the suites are ordered.
std::int64_t route_hits(std::string_view route) {
    return backends::json_integer(fixture_request_body(__LINE__, concat("/_count?path=", route)),
                                  "hits")
        .value_or(-1);
}

// The baseline for a "this route was never hit" assertion. route_hits() reports
// -1 when the counter itself cannot be read, and a bare EQ would then compare
// -1 with -1 and pass without the loader ever having been exercised, so the
// baseline is checked for plausibility before it is trusted.
std::int64_t route_baseline(int line, std::string_view route) {
    ++checks;
    const std::int64_t hits = route_hits(route);
    if (hits < 0)
        fail(line, "the fixture did not report a hit count for " + std::string(route));
    return hits;
}

// The recorded requests are serialised in arrival order, so one entry runs from
// its own opening brace to the next one.
std::vector<std::string> recorded_requests() {
    const std::string document = fixture_request_body(__LINE__, "/_requests");
    constexpr std::string_view opening = "{\"path\":";
    std::vector<std::size_t> starts;
    for (std::size_t at = document.find(opening); at != std::string::npos;
         at = document.find(opening, at + 1))
        starts.push_back(at);
    std::vector<std::string> entries;
    for (std::size_t index = 0; index < starts.size(); ++index) {
        const std::size_t end = index + 1 < starts.size() ? starts[index + 1] : document.size();
        entries.push_back(document.substr(starts[index], end - starts[index]));
    }
    return entries;
}

// The most recent request for a route, as a JSON fragment whose "method",
// header names and "body" can be read with json_string.
std::string last_request(std::string_view route) {
    std::string found;
    for (const std::string& entry : recorded_requests()) {
        const std::string recorded_path = backends::json_string(entry, "path").value_or("");
        if (recorded_path == route)
            found = entry;
    }
    return found;
}

std::string request_field(const std::string& entry, std::string_view name) {
    return backends::json_string(entry, name).value_or("");
}

// ConfigBuilder(true) is the only spelling that populates home_directory_ and
// app_data_directory_, and on the way past it slurps every Karu option out of
// the real environment. Clearing all of them first keeps the one
// environment-reading case as hermetic as the rest: a developer's own
// AWS_PROFILE cannot colour the snapshot, and a stray GCS_METADATA_DISABLED=maybe
// cannot make freeze() reject it and fail the case for an unrelated reason.
// Listing the names by hand would go stale the moment an option is added, so the
// table the builder itself reads is the one that is walked.
class ScrubbedOptionEnvironment {
  public:
    ScrubbedOptionEnvironment() {
        config_options::for_each_environment(
            [this](const char* name) { cleared_.emplace_back(name, nullptr); });
    }

  private:
    std::list<ScopedEnvironment> cleared_;
};

// Fixture paths are embedded in JSON documents and on Windows they are full of
// backslashes; an unescaped one makes json_string reject the whole document.
std::string json_path(std::string_view path) {
    std::string result;
    for (const char character : path) {
        if (character == '\\' || character == '"')
            result.push_back('\\');
        result.push_back(character);
    }
    return result;
}

std::vector<std::string> split_on(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t at = text.find(separator, start);
        if (at == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            return parts;
        }
        parts.emplace_back(text.substr(start, at - start));
        start = at + 1;
    }
}

// The library only ever encodes base64url; reading a JWT back apart is the one
// way to prove what json_escape put in the claims.
std::string base64url_decode(std::string_view text) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string result;
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const char character : text) {
        const std::size_t index = alphabet.find(character);
        if (index == std::string_view::npos)
            return {};
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(index);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<char>((accumulator >> bits) & 0xFFu));
        }
    }
    return result;
}

// Ed25519 loads cleanly through PEM_read_bio_PrivateKey and is then rejected by
// EVP_DigestSignInit with EVP_sha256(), which is the only hermetic way to reach
// the signer-initialisation arm of rsa_sha256.
std::string ed25519_key_pem() {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr), EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_keygen_init(context.get()) != 1)
        return {};
    EVP_PKEY* generated = nullptr;
    if (EVP_PKEY_keygen(context.get(), &generated) != 1 || generated == nullptr)
        return {};
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(generated, EVP_PKEY_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    if (!bio)
        return {};
    if (PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1)
        return {};
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio.get(), &data);
    if (size <= 0 || data == nullptr)
        return {};
    return std::string(data, static_cast<std::size_t>(size));
}

// Writes the test key the way a real service-account JSON carries it, then
// mangles every line break into a CR plus a literal backslash-n. json_string
// turns that back into CR + "\\n" and normalize_pem has to undo both.
std::string mangled_private_key() {
    std::string result;
    for (const char character : test_private_key_pem()) {
        if (character == '\n')
            result += "\\r\\\\n";
        else
            result.push_back(character);
    }
    return result;
}

std::string authorized_user_json(std::string_view token_uri) {
    return concat(R"({"type":"authorized_user","client_id":"json-client",)",
                  R"("client_secret":"json-secret","refresh_token":"json-refresh",)",
                  R"("token_uri":")", token_uri, "\"}");
}

// extra is an already-terminated run of JSON members, source the body of the
// credential_source object.
std::string external_account_json(std::string_view token_url, std::string_view extra,
                                  std::string_view source) {
    return concat(R"({"type":"external_account","audience":"//iam.googleapis.com/projects/1",)",
                  R"("subject_token_type":"urn:ietf:params:oauth:token-type:jwt",)",
                  R"("token_url":")", token_url, "\",", extra, R"("credential_source":{)", source,
                  "}}");
}

// The exchange body for the shared external_account fixture, with the default
// scope the loader falls back to.
std::string exchange_body(std::string_view subject_token) {
    return concat("audience=%2F%2Fiam.googleapis.com%2Fprojects%2F1"
                  "&grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Atoken-exchange"
                  "&requested_token_type=urn%3Aietf%3Aparams%3Aoauth%3Atoken-type%3Aaccess_token"
                  "&scope=https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fdevstorage.read_only"
                  "&subject_token_type=urn%3Aietf%3Aparams%3Aoauth%3Atoken-type%3Ajwt"
                  "&subject_token=",
                  subject_token);
}

void direct_option_cases() {
    SECTION("GCS direct options");

    // The HMAC pair is set alongside the bearer token to prove the token
    // returns before the HMAC block ever runs, while still travelling in the
    // same struct.
    auto token_builder = empty_builder();
    OK(token_builder.set("GCS_ACCESS_TOKEN", "direct-bearer"));
    OK(token_builder.set("GCS_HMAC_ACCESS_KEY_ID", "GOOG1"));
    OK(token_builder.set("GCS_HMAC_SECRET_ACCESS_KEY", "s"));
    auto direct = load(token_builder);
    OK(direct.has_value());
    if (direct) {
        EQS(direct->bearer_token, "direct-bearer");
        EQS(direct->access_key_id, "GOOG1");
        EQS(direct->secret_access_key, "s");
        EQ(direct->expires_at, 0);
    }

    // A path-scoped rule outranks the client-wide value, which is how a caller
    // gives one bucket its own token.
    auto scoped_builder = empty_builder();
    OK(scoped_builder.set("GCS_ACCESS_TOKEN", "client-wide"));
    OK(scoped_builder.set_path("gs://bucket/", "GCS_ACCESS_TOKEN", "scoped"));
    auto scoped = load(scoped_builder);
    OK(scoped.has_value());
    if (scoped)
        EQS(scoped->bearer_token, "scoped");

    auto hmac_builder = empty_builder();
    OK(hmac_builder.set("GCS_HMAC_ACCESS_KEY_ID", "GOOG1EXAMPLE"));
    OK(hmac_builder.set("GCS_HMAC_SECRET_ACCESS_KEY", "hmac-secret"));
    auto hmac = load(hmac_builder);
    OK(hmac.has_value());
    if (hmac) {
        EQS(hmac->access_key_id, "GOOG1EXAMPLE");
        EQS(hmac->secret_access_key, "hmac-secret");
        EQS(hmac->bearer_token, "");
    }

    // Half a pair is a configuration mistake, and the message carries the
    // requested path verbatim so the user can see which rule produced it.
    const std::string half_message =
        std::string(OBJECT_PATH) + ": GCS HMAC access key and secret must be set together";
    auto key_only = empty_builder();
    OK(key_only.set("GCS_HMAC_ACCESS_KEY_ID", "GOOG1EXAMPLE"));
    check_error(__LINE__, load(key_only), half_message);
    auto secret_only = empty_builder();
    OK(secret_only.set("GCS_HMAC_SECRET_ACCESS_KEY", "hmac-secret"));
    check_error(__LINE__, load(secret_only), half_message);

    // The refresh-token shortcut hands gcs_authorized_user an empty document,
    // so an incomplete triple fails before the hardcoded Google endpoint is
    // contacted. A complete one would leave the machine, so it is not tested.
    auto refresh_only = empty_builder();
    OK(refresh_only.set("GCS_REFRESH_TOKEN", "refresh-only"));
    check_error(__LINE__, load(refresh_only), "GCS authorized_user credentials are incomplete");

    check_empty(__LINE__, load(empty_builder()));
}

void adc_selection_cases() {
    SECTION("GCS application default credentials");
    TempTree tree("gcs_adc");

    // A non-empty GOOGLE_APPLICATION_CREDENTIALS skips the existence probe and
    // reports the open failure, rather than silently falling through.
    const std::string missing = tree.absent("adc.json");
    auto missing_builder = empty_builder();
    OK(missing_builder.set("GOOGLE_APPLICATION_CREDENTIALS", missing.c_str()));
    check_error(__LINE__, load(missing_builder),
                "Google application credentials: cannot open '" + missing + "'");

    const std::string oversized = tree.write("huge.json", std::string(1100u * 1024u, 'x'));
    auto oversized_builder = empty_builder();
    OK(oversized_builder.set("GOOGLE_APPLICATION_CREDENTIALS", oversized.c_str()));
    check_error(__LINE__, load(oversized_builder),
                "Google application credentials: file exceeds 1 MiB");

    const std::string impersonated = tree.write(
        "magic.json", R"({"type":"impersonated_service_account","source_credentials":{}})");
    auto impersonated_builder = empty_builder();
    OK(impersonated_builder.set("GOOGLE_APPLICATION_CREDENTIALS", impersonated.c_str()));
    check_error(__LINE__, load(impersonated_builder),
                "GOOGLE_APPLICATION_CREDENTIALS has unsupported type "
                "'impersonated_service_account'");

    // A missing type and a non-string type both collapse to the same empty
    // string, because json_string only ever returns quoted values.
    const std::string untyped = tree.write("notype.json", R"({"client_email":"a@b.test"})");
    auto untyped_builder = empty_builder();
    OK(untyped_builder.set("GOOGLE_APPLICATION_CREDENTIALS", untyped.c_str()));
    check_error(__LINE__, load(untyped_builder),
                "GOOGLE_APPLICATION_CREDENTIALS has unsupported type ''");

    const std::string numeric = tree.write("numeric.json", R"({"type":123})");
    auto numeric_builder = empty_builder();
    OK(numeric_builder.set("GOOGLE_APPLICATION_CREDENTIALS", numeric.c_str()));
    check_error(__LINE__, load(numeric_builder),
                "GOOGLE_APPLICATION_CREDENTIALS has unsupported type ''");
}

void service_account_cases() {
    SECTION("GCS service account");
    TempTree tree("gcs_service_account");
    const std::string token_uri = fixture_url("/oauth/ok");

    // One double quote, one backslash, one newline, one carriage return and one
    // tab, so every arm of json_escape runs while the claims are assembled.
    const std::string scope = "scope\"with\\specials\n\r\t";
    const std::string escaped_scope = "scope\\\"with\\\\specials\\n\\r\\t";

    const std::string fixture =
        concat(R"({"type":"service_account","client_email":"svc@fixture.test",)",
               R"("private_key":")", mangled_private_key(), R"(","token_uri":")", token_uri, "\"}");
    const std::string fixture_file = tree.write("service_account.json", fixture);

    auto builder = empty_builder();
    OK(builder.set("GOOGLE_APPLICATION_CREDENTIALS", fixture_file.c_str()));
    OK(builder.set("GCS_SCOPE", scope.c_str()));

    reset_fixture_requests(__LINE__);
    const std::int64_t before = static_cast<std::int64_t>(std::time(nullptr));
    auto signed_in = load(builder);
    OK(signed_in.has_value());
    if (signed_in) {
        EQS(signed_in->bearer_token, "oauth-access-token");
        check_expiry(__LINE__, signed_in->expires_at, before + 3600);
    }

    const std::string exchange = last_request("/oauth/ok");
    EQS(request_field(exchange, "method"), "POST");
    EQS(request_field(exchange, "content-type"), "application/x-www-form-urlencoded");
    const std::string body = request_field(exchange, "body");
    constexpr std::string_view grant =
        "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=";
    OK(body.starts_with(grant));
    const std::vector<std::string> segments =
        split_on(std::string_view(body).substr(std::min(body.size(), grant.size())), '.');
    EQ(segments.size(), 3u);
    if (segments.size() == 3) {
        // 27 header bytes encode without padding, so base64url has nothing to
        // strip here.
        EQS(segments[0], "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9");
        const std::string claims = base64url_decode(segments[1]);
        const std::string expected = concat(R"({"iss":"svc@fixture.test","scope":")", escaped_scope,
                                            R"(","aud":")", token_uri, R"(","iat":)");
        OK(claims.starts_with(expected));
        OK(claims.contains(",\"exp\":"));
        OK(claims.ends_with("}"));
        // A 2048-bit signature is 256 bytes, which base64 pads with two '=';
        // both are stripped, leaving 342 characters.
        EQ(segments[2].size(), 342u);
        OK(!segments[2].contains('='));
        EQ(base64url_decode(segments[2]).size(), 256u);
    }

    // Either half of the pair missing from the JSON is the same failure.
    const std::string no_key =
        tree.write("no_key.json",
                   concat(R"({"type":"service_account",)",
                          R"("client_email":"svc@fixture.test","token_uri":")", token_uri, "\"}"));
    auto no_key_builder = empty_builder();
    OK(no_key_builder.set("GOOGLE_APPLICATION_CREDENTIALS", no_key.c_str()));
    check_error(__LINE__, load(no_key_builder), "GCS service_account JSON is incomplete");

    const std::string no_email =
        tree.write("no_email.json", concat(R"({"type":"service_account","private_key":"x",)",
                                           R"("token_uri":")", token_uri, "\"}"));
    auto no_email_builder = empty_builder();
    OK(no_email_builder.set("GOOGLE_APPLICATION_CREDENTIALS", no_email.c_str()));
    check_error(__LINE__, load(no_email_builder), "GCS service_account JSON is incomplete");

    // Present but empty gets past the optional check and is rejected one level
    // down, with no token_uri in the document so the hardcoded default is the
    // value that is carried there.
    const std::string empty_email = tree.write(
        "empty_email.json", R"({"type":"service_account","client_email":"","private_key":"x"})");
    auto empty_email_builder = empty_builder();
    OK(empty_email_builder.set("GOOGLE_APPLICATION_CREDENTIALS", empty_email.c_str()));
    check_error(__LINE__, load(empty_email_builder), "GCS service_account JSON is incomplete");

    // The mirror of the case above, so both operands of the emptiness check one
    // level down are taken: here the email survives and the key is the half that
    // is present but empty.
    const std::string empty_key = tree.write(
        "empty_key.json",
        R"({"type":"service_account","client_email":"svc@fixture.test","private_key":""})");
    auto empty_key_builder = empty_builder();
    OK(empty_key_builder.set("GOOGLE_APPLICATION_CREDENTIALS", empty_key.c_str()));
    check_error(__LINE__, load(empty_key_builder), "GCS service_account JSON is incomplete");

    // A key OpenSSL cannot parse fails inside the signer, before any socket is
    // opened: the token endpoint must not see a request.
    const std::string invalid = tree.write(
        "invalid.json",
        concat(
            R"({"type":"service_account","client_email":"svc@fixture.test",)",
            R"("private_key":"-----BEGIN PRIVATE KEY-----\nnot-a-key\n-----END PRIVATE KEY-----\n",)",
            R"("token_uri":")", token_uri, "\"}"));
    auto invalid_builder = empty_builder();
    OK(invalid_builder.set("GOOGLE_APPLICATION_CREDENTIALS", invalid.c_str()));
    const std::int64_t hits_before = route_baseline(__LINE__, "/oauth/ok");
    check_error(__LINE__, load(invalid_builder), "service account private_key is invalid");
    EQ(route_hits("/oauth/ok"), hits_before);

    // An Ed25519 key parses and then fails to pair with SHA-256. Older OpenSSL
    // builds without Ed25519 return an empty PEM, and the case steps aside.
    if (const std::string ed25519 = ed25519_key_pem(); !ed25519.empty()) {
        auto ed25519_builder = empty_builder();
        OK(ed25519_builder.set("GCS_PRIVATE_KEY", ed25519.c_str()));
        OK(ed25519_builder.set("GCS_CLIENT_EMAIL", "svc@fixture.test"));
        check_error(__LINE__, load(ed25519_builder), "cannot initialize service account signature");
    }
}

void authorized_user_cases() {
    SECTION("GCS authorized user");
    TempTree tree("gcs_authorized_user");

    // GCS_CLIENT_ID outranks the JSON while the secret and refresh token fall
    // back to it, so one request proves both sides of the precedence.
    const std::string fixture =
        tree.write("adc.json", authorized_user_json(fixture_url("/oauth/ok")));
    auto builder = empty_builder();
    OK(builder.set("GOOGLE_APPLICATION_CREDENTIALS", fixture.c_str()));
    OK(builder.set("GCS_CLIENT_ID", "config-client"));

    reset_fixture_requests(__LINE__);
    const std::int64_t before = static_cast<std::int64_t>(std::time(nullptr));
    auto refreshed = load(builder);
    OK(refreshed.has_value());
    if (refreshed) {
        EQS(refreshed->bearer_token, "oauth-access-token");
        check_expiry(__LINE__, refreshed->expires_at, before + 3600);
    }
    EQS(request_field(last_request("/oauth/ok"), "body"),
        "client_id=config-client&client_secret=json-secret&refresh_token=json-refresh"
        "&grant_type=refresh_token");

    // A ten-second grant is clamped up to the one-minute floor, so a caller
    // never schedules a renewal storm.
    const std::string short_fixture =
        tree.write("short.json", authorized_user_json(fixture_url("/oauth/short")));
    auto short_builder = empty_builder();
    OK(short_builder.set("GOOGLE_APPLICATION_CREDENTIALS", short_fixture.c_str()));
    const std::int64_t short_before = static_cast<std::int64_t>(std::time(nullptr));
    auto short_lived = load(short_builder);
    OK(short_lived.has_value());
    if (short_lived) {
        EQS(short_lived->bearer_token, "short-lived-token");
        check_expiry(__LINE__, short_lived->expires_at, short_before + 60);
    }

    const std::string denied_fixture =
        tree.write("denied.json", authorized_user_json(fixture_url("/oauth/denied")));
    auto denied_builder = empty_builder();
    OK(denied_builder.set("GOOGLE_APPLICATION_CREDENTIALS", denied_fixture.c_str()));
    check_error(__LINE__, load(denied_builder), "credential endpoint returned HTTP 400");

    const std::string empty_fixture =
        tree.write("no_token.json", authorized_user_json(fixture_url("/oauth/no-token")));
    auto empty_response_builder = empty_builder();
    OK(empty_response_builder.set("GOOGLE_APPLICATION_CREDENTIALS", empty_fixture.c_str()));
    check_error(__LINE__, load(empty_response_builder), "credential response has no access_token");

    // The remaining two operands of the completeness check. The refresh-token
    // shortcut case only ever takes the client_id one, so without these a
    // document that is missing just its secret, or just its refresh token, would
    // be exchanged with an empty field rather than refused. Both fail before any
    // socket is opened, which is what the untouched hit count records.
    const std::string token_uri = fixture_url("/oauth/ok");
    const std::int64_t hits_before = route_baseline(__LINE__, "/oauth/ok");
    const std::string no_secret =
        tree.write("no_secret.json",
                   concat(R"({"type":"authorized_user","client_id":"json-client",)",
                          R"("refresh_token":"json-refresh","token_uri":")", token_uri, "\"}"));
    auto no_secret_builder = empty_builder();
    OK(no_secret_builder.set("GOOGLE_APPLICATION_CREDENTIALS", no_secret.c_str()));
    check_error(__LINE__, load(no_secret_builder),
                "GCS authorized_user credentials are incomplete");

    const std::string no_refresh =
        tree.write("no_refresh.json",
                   concat(R"({"type":"authorized_user","client_id":"json-client",)",
                          R"("client_secret":"json-secret","token_uri":")", token_uri, "\"}"));
    auto no_refresh_builder = empty_builder();
    OK(no_refresh_builder.set("GOOGLE_APPLICATION_CREDENTIALS", no_refresh.c_str()));
    check_error(__LINE__, load(no_refresh_builder),
                "GCS authorized_user credentials are incomplete");
    EQ(route_hits("/oauth/ok"), hits_before);
}

void external_account_subject_cases() {
    SECTION("GCS external account subject tokens");
    TempTree tree("gcs_external_account");
    const std::string token_url = fixture_url("/oauth/ok");
    const std::string subject_path = json_path(tree.write("subject.txt", "  file-subject-token\n"));
    const std::string file_source = concat(R"("file":")", subject_path, "\"");

    // Each of the three operands of the completeness check, with no token_url
    // in the document so the hardcoded STS default is never contacted.
    constexpr std::string_view incomplete = "GCS external_account credential_source is incomplete";
    const std::string absent_source =
        concat(R"("file":")", json_path(tree.absent("token.txt")), "\"");
    const std::string no_audience =
        tree.write("no_audience.json",
                   concat(R"({"type":"external_account",)",
                          R"("subject_token_type":"urn:ietf:params:oauth:token-type:jwt",)",
                          R"("credential_source":{)", absent_source, "}}"));
    auto no_audience_builder = empty_builder();
    OK(no_audience_builder.set("GOOGLE_APPLICATION_CREDENTIALS", no_audience.c_str()));
    check_error(__LINE__, load(no_audience_builder), incomplete);

    const std::string no_type =
        tree.write("no_subject_type.json",
                   concat(R"({"type":"external_account","audience":"//iam.googleapis.com/p/1",)",
                          R"("credential_source":{)", absent_source, "}}"));
    auto no_type_builder = empty_builder();
    OK(no_type_builder.set("GOOGLE_APPLICATION_CREDENTIALS", no_type.c_str()));
    check_error(__LINE__, load(no_type_builder), incomplete);

    const std::string no_source =
        tree.write("no_source.json",
                   concat(R"({"type":"external_account","audience":"//iam.googleapis.com/p/1",)",
                          R"("subject_token_type":"urn:ietf:params:oauth:token-type:jwt",)",
                          R"("credential_source":{}})"));
    auto no_source_builder = empty_builder();
    OK(no_source_builder.set("GOOGLE_APPLICATION_CREDENTIALS", no_source.c_str()));
    check_error(__LINE__, load(no_source_builder), incomplete);

    // A named but unreadable subject file is reported with its own purpose, not
    // folded into the incompleteness message above.
    const std::string missing_subject = tree.absent("gone.txt");
    const std::string missing_source =
        tree.write("missing_subject.json",
                   external_account_json(token_url, "",
                                         concat(R"("file":")", json_path(missing_subject), "\"")));
    auto missing_builder = empty_builder();
    OK(missing_builder.set("GOOGLE_APPLICATION_CREDENTIALS", missing_source.c_str()));
    check_error(__LINE__, load(missing_builder),
                "GCS external account subject token: cannot open '" + missing_subject + "'");

    // The happy path: the file's padding is trimmed and the exchange carries
    // the default scope.
    const std::string file_fixture =
        tree.write("file.json", external_account_json(token_url, "", file_source));
    auto file_builder = empty_builder();
    OK(file_builder.set("GOOGLE_APPLICATION_CREDENTIALS", file_fixture.c_str()));
    reset_fixture_requests(__LINE__);
    auto exchanged = load(file_builder);
    OK(exchanged.has_value());
    if (exchanged)
        EQS(exchanged->bearer_token, "oauth-access-token");
    EQS(request_field(last_request("/oauth/ok"), "body"), exchange_body("file-subject-token"));

    // An impersonation URL that is present but empty is the same as no
    // impersonation at all.
    const std::string empty_impersonation =
        tree.write("empty_impersonation.json",
                   external_account_json(token_url, R"("service_account_impersonation_url":"",)",
                                         file_source));
    auto empty_impersonation_builder = empty_builder();
    OK(empty_impersonation_builder.set("GOOGLE_APPLICATION_CREDENTIALS",
                                       empty_impersonation.c_str()));
    auto unimpersonated = load(empty_impersonation_builder);
    OK(unimpersonated.has_value());
    if (unimpersonated)
        EQS(unimpersonated->bearer_token, "oauth-access-token");

    // So is a field name that is present but empty: the raw subject is sent on.
    const std::string empty_field = tree.write(
        "empty_field.json",
        external_account_json(token_url, R"("subject_token_field_name":"",)", file_source));
    auto empty_field_builder = empty_builder();
    OK(empty_field_builder.set("GOOGLE_APPLICATION_CREDENTIALS", empty_field.c_str()));
    reset_fixture_requests(__LINE__);
    auto raw_subject = load(empty_field_builder);
    OK(raw_subject.has_value());
    if (raw_subject)
        EQS(raw_subject->bearer_token, "oauth-access-token");
    EQS(request_field(last_request("/oauth/ok"), "body"), exchange_body("file-subject-token"));

    // A configured field the subject document does not carry stops the load
    // before the exchange is attempted.
    const std::string other_subject = json_path(tree.write("subject.json", R"({"other":"x"})"));
    const std::string missing_field =
        tree.write("missing_field.json",
                   external_account_json(token_url, R"("subject_token_field_name":"id_token",)",
                                         concat(R"("file":")", other_subject, "\"")));
    auto missing_field_builder = empty_builder();
    OK(missing_field_builder.set("GOOGLE_APPLICATION_CREDENTIALS", missing_field.c_str()));
    const std::int64_t hits_before = route_baseline(__LINE__, "/oauth/ok");
    check_error(__LINE__, load(missing_field_builder),
                "GCS external account response has no configured subject token field");
    EQ(route_hits("/oauth/ok"), hits_before);

    // A subject served over HTTP is trimmed the same way the file is; the
    // fixture pads its body with two spaces at each end.
    const std::string url_fixture = tree.write(
        "url.json",
        external_account_json(token_url, "",
                              concat(R"("url":")", fixture_url("/gcs/subject/text"), "\"")));
    auto url_builder = empty_builder();
    OK(url_builder.set("GOOGLE_APPLICATION_CREDENTIALS", url_fixture.c_str()));
    reset_fixture_requests(__LINE__);
    auto from_url = load(url_builder);
    OK(from_url.has_value());
    if (from_url)
        EQS(from_url->bearer_token, "oauth-access-token");
    EQS(request_field(last_request("/oauth/ok"), "body"), exchange_body("subject-token-from-url"));

    const std::string field_fixture = tree.write(
        "url_field.json",
        external_account_json(token_url, R"("subject_token_field_name":"id_token",)",
                              concat(R"("url":")", fixture_url("/gcs/subject/json"), "\"")));
    auto field_builder = empty_builder();
    OK(field_builder.set("GOOGLE_APPLICATION_CREDENTIALS", field_fixture.c_str()));
    reset_fixture_requests(__LINE__);
    auto from_field = load(field_builder);
    OK(from_field.has_value());
    if (from_field)
        EQS(from_field->bearer_token, "oauth-access-token");
    EQS(request_field(last_request("/oauth/ok"), "body"),
        exchange_body("subject-token-from-field"));

    // A field that is there but empty is as useless as an absent one.
    const std::string blank_fixture = tree.write(
        "url_blank.json",
        external_account_json(token_url, R"("subject_token_field_name":"id_token",)",
                              concat(R"("url":")", fixture_url("/gcs/subject/empty-field"), "\"")));
    auto blank_builder = empty_builder();
    OK(blank_builder.set("GOOGLE_APPLICATION_CREDENTIALS", blank_fixture.c_str()));
    check_error(__LINE__, load(blank_builder),
                "GCS external account response has no configured subject token field");

    // credential_request restricts the protocols to http and https, so an ftp
    // URL is refused before a socket is opened. No DNS, no timeout.
    const std::string unreachable =
        tree.write("unreachable.json",
                   external_account_json(token_url, "", R"("url":"ftp://127.0.0.1/karu-subject")"));
    auto unreachable_builder = empty_builder();
    OK(unreachable_builder.set("GOOGLE_APPLICATION_CREDENTIALS", unreachable.c_str()));
    check_error_prefix(__LINE__, load(unreachable_builder), "credential endpoint: ");

    const std::string denied_fixture = tree.write(
        "subject_denied.json",
        external_account_json(token_url, "",
                              concat(R"("url":")", fixture_url("/gcs/subject/denied"), "\"")));
    auto denied_builder = empty_builder();
    OK(denied_builder.set("GOOGLE_APPLICATION_CREDENTIALS", denied_fixture.c_str()));
    check_error(__LINE__, load(denied_builder), "GCS external account subject endpoint failed");

    const std::string exchange_denied =
        tree.write("exchange_denied.json",
                   external_account_json(fixture_url("/oauth/denied"), "", file_source));
    auto exchange_denied_builder = empty_builder();
    OK(exchange_denied_builder.set("GOOGLE_APPLICATION_CREDENTIALS", exchange_denied.c_str()));
    check_error(__LINE__, load(exchange_denied_builder), "credential endpoint returned HTTP 400");
}

void external_account_impersonation_cases() {
    SECTION("GCS external account impersonation");
    TempTree tree("gcs_impersonation");
    const std::string token_url = fixture_url("/oauth/ok");
    const std::string file_source =
        concat(R"("file":")", json_path(tree.write("subject.txt", "file-subject-token")), "\"");

    auto impersonation_fixture = [&](std::string_view label, std::string_view url) {
        return tree.write(label, external_account_json(
                                     token_url,
                                     concat(R"("service_account_impersonation_url":")", url, "\","),
                                     file_source));
    };

    // The scope reaches the impersonation body through json_escape, so it
    // carries a quote and a backslash to prove the escaping happens here too.
    const std::string scope = "imp\"scope\\value";
    const std::string ok_fixture =
        impersonation_fixture("ok.json", fixture_url("/gcs/impersonate/ok"));
    auto builder = empty_builder();
    OK(builder.set("GOOGLE_APPLICATION_CREDENTIALS", ok_fixture.c_str()));
    OK(builder.set("GCS_SCOPE", scope.c_str()));

    reset_fixture_requests(__LINE__);
    const std::int64_t before = static_cast<std::int64_t>(std::time(nullptr));
    auto impersonated = load(builder);
    OK(impersonated.has_value());
    if (impersonated) {
        EQS(impersonated->bearer_token, "impersonated-token");
        // The expiry comes from the RFC 3339 expireTime, not from a lifetime.
        // The fixture stamps it from its own clock, which cannot run behind the
        // one this case just sampled, so the floor is exactly before + 3600.
        OK(impersonated->expires_at > before);
        check_expiry(__LINE__, impersonated->expires_at, before + 3600);
    }
    const std::string call = last_request("/gcs/impersonate/ok");
    EQS(request_field(call, "method"), "POST");
    EQS(request_field(call, "authorization"), "Bearer oauth-access-token");
    EQS(request_field(call, "content-type"), "application/json");
    EQS(request_field(call, "body"),
        "{\"scope\":[\"imp\\\"scope\\\\value\"],\"lifetime\":\"3600s\"}");

    const std::string denied =
        impersonation_fixture("denied.json", fixture_url("/gcs/impersonate/denied"));
    auto denied_builder = empty_builder();
    OK(denied_builder.set("GOOGLE_APPLICATION_CREDENTIALS", denied.c_str()));
    check_error(__LINE__, load(denied_builder), "GCS service account impersonation failed");

    // A 200 with no token and a 200 that is already expired are the same
    // failure, one for each operand of the check.
    const std::string no_token =
        impersonation_fixture("no_token.json", fixture_url("/gcs/impersonate/no-token"));
    auto no_token_builder = empty_builder();
    OK(no_token_builder.set("GOOGLE_APPLICATION_CREDENTIALS", no_token.c_str()));
    check_error(__LINE__, load(no_token_builder), "GCS impersonation response has no token");

    const std::string expired =
        impersonation_fixture("expired.json", fixture_url("/gcs/impersonate/expired"));
    auto expired_builder = empty_builder();
    OK(expired_builder.set("GOOGLE_APPLICATION_CREDENTIALS", expired.c_str()));
    check_error(__LINE__, load(expired_builder), "GCS impersonation response has no token");

    // /gcs/impersonate/expired only proves a well-formed timestamp in the past.
    // Pointing the impersonation call at the OAuth route instead answers 200
    // with a document carrying neither accessToken nor expireTime, so
    // iso8601_epoch is handed an empty string and returns 0 out of its parse
    // failure rather than out of a date. No fixture route serves an accessToken
    // beside an unparseable expireTime, so this cannot tell the expiry operand
    // apart from the token operand; what it does pin is the message for a 200
    // of the wrong shape, and the hit count proves the impersonation POST was
    // really made rather than the load having failed earlier in the exchange.
    const std::string unparsable = impersonation_fixture("unparsable.json", token_url);
    auto unparsable_builder = empty_builder();
    OK(unparsable_builder.set("GOOGLE_APPLICATION_CREDENTIALS", unparsable.c_str()));
    const std::int64_t before_unparsable = route_baseline(__LINE__, "/oauth/ok");
    check_error(__LINE__, load(unparsable_builder), "GCS impersonation response has no token");
    // One hit for the token exchange, one for the impersonation call that
    // followed it to the same route.
    EQ(route_hits("/oauth/ok"), before_unparsable + 2);

    // The exchange has to succeed first, so this one still needs the fixture
    // server even though the impersonation call never opens a socket.
    const std::string unreachable =
        impersonation_fixture("unreachable.json", "ftp://127.0.0.1/karu-impersonate");
    auto unreachable_builder = empty_builder();
    OK(unreachable_builder.set("GOOGLE_APPLICATION_CREDENTIALS", unreachable.c_str()));
    check_error_prefix(__LINE__, load(unreachable_builder), "credential endpoint: ");
}

void cloud_sdk_cases() {
    SECTION("GCS gcloud configuration directory");
    TempTree tree("gcs_cloudsdk");
    (void)tree.write("sdk/application_default_credentials.json",
                     authorized_user_json(fixture_url("/oauth/ok")));

    // The trailing slash is stripped before the file name is appended, so both
    // spellings of CLOUDSDK_CONFIG find the same file.
    for (const std::string& directory : {tree.root() + "/sdk/", tree.root() + "/sdk"}) {
        auto builder = empty_builder();
        OK(builder.set("CLOUDSDK_CONFIG", directory.c_str()));
        auto found = load(builder);
        OK(found.has_value());
        if (found)
            EQS(found->bearer_token, "oauth-access-token");
    }

    // A configured directory without the file is not an error: discovery just
    // stops and the caller falls through to anonymous access.
    const std::string empty_directory = tree.absent("empty-sdk");
    auto missing_builder = empty_builder();
    OK(missing_builder.set("CLOUDSDK_CONFIG", empty_directory.c_str()));
    check_empty(__LINE__, load(missing_builder));

    // The gcloud default path is derived from the home directory, which only an
    // environment-reading builder has. Asserting the path itself keeps the case
    // hermetic: calling the loader here would reach the real metadata server if
    // the platform looked somewhere this tree does not cover.
    TempTree home("gcs_home");
    const ScrubbedOptionEnvironment scrubbed;
    const ScopedEnvironment home_variable("HOME", home.root().c_str());
    const ScopedEnvironment profile_variable("USERPROFILE", home.root().c_str());
    const std::string app_data = home.root() + "/appdata";
    const ScopedEnvironment app_data_variable("APPDATA", app_data.c_str());
    const ConfigBuilder environment_builder(true);
#ifdef _WIN32
    const std::string expected = app_data + "/gcloud/application_default_credentials.json";
#else
    const std::string expected =
        home.root() + "/.config/gcloud/application_default_credentials.json";
#endif
    EQS(must_freeze(environment_builder).default_gcloud_adc_path(), expected);
}

void private_key_option_cases() {
    SECTION("GCS private key options");
    TempTree tree("gcs_private_key");

    auto conflict = empty_builder();
    OK(conflict.set("GCS_PRIVATE_KEY", test_private_key_pem().c_str()));
    const std::string key_file = tree.write("key.pem", test_private_key_pem());
    OK(conflict.set("GCS_PRIVATE_KEY_FILE", key_file.c_str()));
    OK(conflict.set("GCS_CLIENT_EMAIL", "svc@fixture.test"));
    check_error(__LINE__, load(conflict),
                "set only one of GCS_PRIVATE_KEY and GCS_PRIVATE_KEY_FILE");

    // The file is read before the set-together check, so an unreadable file is
    // reported even with no client email configured.
    const std::string missing = tree.absent("nope.pem");
    auto missing_builder = empty_builder();
    OK(missing_builder.set("GCS_PRIVATE_KEY_FILE", missing.c_str()));
    check_error(__LINE__, load(missing_builder),
                "GCS OAuth private key: cannot open '" + missing + "'");

    const std::string oversized = tree.write("huge.pem", std::string(1100u * 1024u, 'x'));
    auto oversized_builder = empty_builder();
    OK(oversized_builder.set("GCS_PRIVATE_KEY_FILE", oversized.c_str()));
    check_error(__LINE__, load(oversized_builder), "GCS OAuth private key: file exceeds 1 MiB");

    constexpr std::string_view together =
        "GCS_CLIENT_EMAIL and a GCS private key must be set together";
    auto email_only = empty_builder();
    OK(email_only.set("GCS_CLIENT_EMAIL", "svc@fixture.test"));
    check_error(__LINE__, load(email_only), together);

    auto key_only = empty_builder();
    OK(key_only.set("GCS_PRIVATE_KEY", test_private_key_pem().c_str()));
    check_error(__LINE__, load(key_only), together);

    // Literal backslash-n sequences in the file exercise normalize_pem on this
    // path too. The key is deliberately unparseable: a valid one here would
    // sign an assertion for the hardcoded Google token endpoint.
    const std::string invalid = tree.write(
        "bad.pem", "-----BEGIN PRIVATE KEY-----\\nnot-a-key\\n-----END PRIVATE KEY-----\\n");
    auto invalid_builder = empty_builder();
    OK(invalid_builder.set("GCS_PRIVATE_KEY_FILE", invalid.c_str()));
    OK(invalid_builder.set("GCS_CLIENT_EMAIL", "svc@fixture.test"));
    check_error(__LINE__, load(invalid_builder), "service account private_key is invalid");
}

void metadata_cases() {
    SECTION("GCS metadata service");

    // With an environment-free builder the metadata block is reachable only
    // because GCS_METADATA_ENDPOINT is set, which is also what turns the
    // endpoint's failures into errors instead of anonymous access.
    auto builder = empty_builder();
    OK(builder.set("GCS_METADATA_ENDPOINT", fixture_url("/gcs/metadata/ok").c_str()));
    reset_fixture_requests(__LINE__);
    const std::int64_t before = static_cast<std::int64_t>(std::time(nullptr));
    auto from_metadata = load(builder);
    OK(from_metadata.has_value());
    if (from_metadata) {
        EQS(from_metadata->bearer_token, "gce-metadata-token");
        check_expiry(__LINE__, from_metadata->expires_at, before + 1800);
    }
    const std::string call = last_request("/gcs/metadata/ok");
    EQS(request_field(call, "method"), "GET");
    EQS(request_field(call, "metadata-flavor"), "Google");

    // Without expires_in the loader assumes an hour.
    auto no_expiry_builder = empty_builder();
    OK(no_expiry_builder.set("GCS_METADATA_ENDPOINT",
                             fixture_url("/gcs/metadata/no-expires-in").c_str()));
    const std::int64_t no_expiry_before = static_cast<std::int64_t>(std::time(nullptr));
    auto without_expiry = load(no_expiry_builder);
    OK(without_expiry.has_value());
    if (without_expiry) {
        EQS(without_expiry->bearer_token, "gce-metadata-token");
        check_expiry(__LINE__, without_expiry->expires_at, no_expiry_before + 3600);
    }

    auto no_token_builder = empty_builder();
    OK(no_token_builder.set("GCS_METADATA_ENDPOINT",
                            fixture_url("/gcs/metadata/no-token").c_str()));
    check_error(__LINE__, load(no_token_builder),
                "GCS credential endpoint response has no access token");

    auto denied_builder = empty_builder();
    OK(denied_builder.set("GCS_METADATA_ENDPOINT", fixture_url("/gcs/metadata/denied").c_str()));
    check_error(__LINE__, load(denied_builder), "GCS credential endpoint returned HTTP 500");

    auto not_found_builder = empty_builder();
    OK(not_found_builder.set("GCS_METADATA_ENDPOINT", fixture_url("/status/404").c_str()));
    check_error(__LINE__, load(not_found_builder), "GCS credential endpoint returned HTTP 404");

    // An ftp endpoint is refused by curl's protocol allowlist, which is a
    // transport failure with no socket and no waiting.
    auto transport_builder = empty_builder();
    OK(transport_builder.set("GCS_METADATA_ENDPOINT", "ftp://127.0.0.1/computeMetadata/v1/token"));
    check_error_prefix(__LINE__, load(transport_builder), "credential endpoint: ");

    // The disable flag is enough on its own to make the block reachable, and it
    // beats a configured endpoint.
    const std::int64_t hits_before = route_baseline(__LINE__, "/gcs/metadata/ok");
    auto disabled = empty_builder();
    OK(disabled.set("GCS_METADATA_DISABLED", "YES"));
    check_empty(__LINE__, load(disabled));

    auto disabled_with_endpoint = empty_builder();
    OK(disabled_with_endpoint.set("GCS_METADATA_DISABLED", "TRUE"));
    OK(disabled_with_endpoint.set("GCS_METADATA_ENDPOINT",
                                  fixture_url("/gcs/metadata/ok").c_str()));
    check_empty(__LINE__, load(disabled_with_endpoint));
    EQ(route_hits("/gcs/metadata/ok"), hits_before);

    // Explicitly not disabled, with an endpoint to keep the request on the
    // loopback fixture rather than metadata.google.internal.
    auto enabled = empty_builder();
    OK(enabled.set("GCS_METADATA_DISABLED", "NO"));
    OK(enabled.set("GCS_METADATA_ENDPOINT", fixture_url("/gcs/metadata/ok").c_str()));
    auto still_reachable = load(enabled);
    OK(still_reachable.has_value());
    if (still_reachable)
        EQS(still_reachable->bearer_token, "gce-metadata-token");
}

} // namespace

void test_gcs_credentials() {
    SECTION("GCS credentials");
    OK(fixture_port() > 0);
    direct_option_cases();
    adc_selection_cases();
    service_account_cases();
    authorized_user_cases();
    external_account_subject_cases();
    external_account_impersonation_cases();
    cloud_sdk_cases();
    private_key_option_cases();
    metadata_cases();
}

} // namespace karu::test
