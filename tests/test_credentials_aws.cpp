#include "backends/aws_profile.hpp"
#include "backends/credentials.hpp"
#include "backends/s3_credentials.hpp"
#include "config_options.hpp"
#include "credential_support.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace karu::test {
namespace {

using Loaded = std::expected<ProviderCredentials, RequestError>;

// The AWS loader never keeps a reference to the snapshot, so freezing inside
// the call keeps every case to a single statement.
Loaded load(const ConfigBuilder& builder, std::string_view path = "/vsis3/bucket/key") {
    return backends::load_aws_credentials(must_freeze(builder), path);
}

// Failure assertions take the caller's line: a mismatch must name the case,
// not this helper.
void denied(int line, const Loaded& result, std::string_view message) {
    if (result) {
        fail(line, "expected a credential failure, got access key '" + result->access_key_id + "'");
        return;
    }
    if (result.error().status != KARU_ERR_CREDENTIALS) {
        fail(line, "expected KARU_ERR_CREDENTIALS, got status " +
                       std::to_string(static_cast<int>(result.error().status)));
    }
    string_at(line, result.error().message, message, "error message");
}

// For failures whose tail is libcurl's platform-specific text.
void denied_prefix(int line, const Loaded& result, std::string_view prefix) {
    if (result) {
        fail(line, "expected a credential failure, got access key '" + result->access_key_id + "'");
        return;
    }
    if (result.error().status != KARU_ERR_CREDENTIALS) {
        fail(line, "expected KARU_ERR_CREDENTIALS, got status " +
                       std::to_string(static_cast<int>(result.error().status)));
    }
    if (!result.error().message.starts_with(prefix)) {
        fail(line, "expected a message starting with '" + std::string(prefix) + "', got '" +
                       result.error().message + "'");
    }
}

void expect_credentials(int line, const Loaded& result, std::string_view key,
                        std::string_view secret, std::string_view token, std::string_view region) {
    if (!result) {
        fail(line, "expected credentials, got '" + result.error().message + "'");
        return;
    }
    string_at(line, result->access_key_id, key, "access_key_id");
    string_at(line, result->secret_access_key, secret, "secret_access_key");
    string_at(line, result->session_token, token, "session_token");
    string_at(line, result->region, region, "region");
}

// Everything the loopback fixture has been asked for, as the JSON array it
// serves. Cases that must prove what went out on the wire read it back rather
// than relying on a route that echoes, which the fixture does not offer.
std::string fixture_requests() {
    auto response = backends::credential_request("GET", fixture_url("/_requests"), {}, {}, 5, {});
    if (!response)
        return {};
    return response->body;
}

// Clears that log. Every "what went out on the wire" assertion below resets
// first: the log is shared with every other case and suite, so without a reset
// an earlier request satisfies the contains() and the check can no longer fail.
void reset_fixture_log() {
    static_cast<void>(backends::credential_request("GET", fixture_url("/_reset"), {}, {}, 5, {}));
}

void contains(int line, const std::string& haystack, std::string_view needle) {
    if (haystack.find(needle) == std::string::npos)
        fail(line, "the fixture never saw '" + std::string(needle) + "'");
}

void lacks(int line, const std::string& haystack, std::string_view needle) {
    if (haystack.find(needle) != std::string::npos)
        fail(line, "the fixture unexpectedly saw '" + std::string(needle) + "'");
}

void direct_keys() {
    SECTION("AWS explicit access keys");
    ConfigBuilder complete = empty_builder();
    OK(complete.set("AWS_ACCESS_KEY_ID", "AKIDIRECT"));
    OK(complete.set("AWS_SECRET_ACCESS_KEY", "secret-direct"));
    OK(complete.set("AWS_SESSION_TOKEN", "token-direct"));
    // An unroutable container endpoint and a profile that does not exist are
    // both fatal further down; reaching neither is what proves the keys win.
    OK(complete.set("AWS_CONTAINER_CREDENTIALS_FULL_URI", "http://creds.example.com/creds"));
    OK(complete.set("AWS_PROFILE", "nope"));
    auto direct = load(complete);
    expect_credentials(__LINE__, direct, "AKIDIRECT", "secret-direct", "token-direct", "");
    if (direct)
        EQ(direct->expires_at, 0);

    // A path rule overrides the global keys only for paths under its prefix,
    // and the s3:// spelling has to canonicalise to /vsis3/ to match.
    ConfigBuilder scoped = empty_builder();
    OK(scoped.set("AWS_ACCESS_KEY_ID", "AKIGLOBAL"));
    OK(scoped.set("AWS_SECRET_ACCESS_KEY", "secret-global"));
    OK(scoped.set_path("/vsis3/bucket", "AWS_ACCESS_KEY_ID", "AKIPATH"));
    OK(scoped.set_path("/vsis3/bucket", "AWS_SECRET_ACCESS_KEY", "secret-path"));
    const ConfigSnapshot scoped_config = must_freeze(scoped);
    expect_credentials(__LINE__, backends::load_aws_credentials(scoped_config, "s3://bucket/key"),
                       "AKIPATH", "secret-path", "", "");
    expect_credentials(__LINE__, backends::load_aws_credentials(scoped_config, "/vsis3/other/key"),
                       "AKIGLOBAL", "secret-global", "", "");

    // The path is quoted back verbatim, so the raw s3:// spelling must survive.
    ConfigBuilder key_only = empty_builder();
    OK(key_only.set("AWS_ACCESS_KEY_ID", "AKIDONLY"));
    denied(__LINE__, load(key_only, "/vsis3/bucket/key"),
           "/vsis3/bucket/key: AWS access key and secret must be set together");

    ConfigBuilder secret_only = empty_builder();
    OK(secret_only.set("AWS_SECRET_ACCESS_KEY", "secret-only"));
    denied(__LINE__, load(secret_only, "s3://bucket/key"),
           "s3://bucket/key: AWS access key and secret must be set together");
}

void profile_files() {
    SECTION("AWS profile files");
    const TempTree tree("aws_profile_files");
    const std::string config_path = tree.write("config", "[profile other]\nregion = us-east-2\n");
    const std::string credentials_path =
        tree.write("credentials", "[other]\naws_access_key_id = AKIOTHER\n"
                                  "aws_secret_access_key = other\n");

    // Both files parse; the named profile simply is not in either of them.
    ConfigBuilder named = empty_builder();
    OK(named.set("AWS_CONFIG_FILE", config_path.c_str()));
    OK(named.set("AWS_SHARED_CREDENTIALS_FILE", credentials_path.c_str()));
    OK(named.set("AWS_PROFILE", "work"));
    denied(__LINE__, load(named), "AWS profile 'work' was not found");

    // AWS_DEFAULT_PROFILE folds onto AWS_PROFILE, so has_option must see it too
    // or the explicit-profile guard would silently stop firing for that spelling.
    ConfigBuilder alias = empty_builder();
    OK(alias.set("AWS_CONFIG_FILE", config_path.c_str()));
    OK(alias.set("AWS_SHARED_CREDENTIALS_FILE", credentials_path.c_str()));
    OK(alias.set("AWS_DEFAULT_PROFILE", "work"));
    denied(__LINE__, load(alias), "AWS profile 'work' was not found");

    const std::string absent_config = tree.absent("absent-config");
    ConfigBuilder no_config = empty_builder();
    OK(no_config.set("AWS_CONFIG_FILE", absent_config.c_str()));
    denied(__LINE__, load(no_config), "AWS config file does not exist: '" + absent_config + "'");

    // The config file exists and parses first, so reaching the credentials-file
    // complaint proves the config half did not short-circuit.
    const std::string default_config =
        tree.write("default-config", "[default]\nregion = us-east-1\n");
    const std::string absent_credentials = tree.absent("absent-credentials");
    ConfigBuilder no_credentials = empty_builder();
    OK(no_credentials.set("AWS_CONFIG_FILE", default_config.c_str()));
    OK(no_credentials.set("AWS_SHARED_CREDENTIALS_FILE", absent_credentials.c_str()));
    denied(__LINE__, load(no_credentials),
           "AWS shared credentials file does not exist: '" + absent_credentials + "'");

    // An over-length name is the portable way to make std::filesystem::exists
    // report an error rather than "missing". Windows maps some long-path
    // failures onto "not found", so the message is only checked when the stat
    // actually failed.
    const std::string oversized(5000, 'x');
    ConfigBuilder unstattable = empty_builder();
    OK(unstattable.set("AWS_SHARED_CREDENTIALS_FILE", oversized.c_str()));
    auto inspected = load(unstattable);
    OK(!inspected);
    if (!inspected) {
        EQ(inspected.error().status, KARU_ERR_CREDENTIALS);
        if (inspected.error().message.find("cannot inspect") != std::string::npos) {
            OK(inspected.error().message.starts_with(
                "AWS shared credentials file: cannot inspect '"));
        }
    }

    constexpr std::size_t one_mib = 1u << 20;
    const std::string too_large = tree.write("big-config", std::string(one_mib + 1, 'x'));
    ConfigBuilder oversize = empty_builder();
    OK(oversize.set("AWS_CONFIG_FILE", too_large.c_str()));
    denied(__LINE__, load(oversize), "AWS config: file exceeds 1 MiB");

    // Exactly 1 MiB is still readable; it parses to nothing and the loader
    // falls through to the anonymous result. This pins the off-by-one.
    const std::string at_limit_path = tree.write("exact-config", std::string(one_mib, 'x'));
    ConfigBuilder at_limit = empty_builder();
    OK(at_limit.set("AWS_CONFIG_FILE", at_limit_path.c_str()));
    expect_credentials(__LINE__, load(at_limit), "", "", "", "");
}

void profile_values() {
    SECTION("AWS profile values");
    const TempTree tree("aws_profile_values");
    // The leading orphan assignment has no section, the two comment forms and
    // the separator-less line are all dropped, and the upper-case key in the
    // credentials file has to be folded before it can override the config file.
    const std::string config_path =
        tree.write("config", "orphan = 1\n; comment\n# another\n[profile work]\n"
                             "region = us-east-2\naws_access_key_id = FROM_CONFIG\n"
                             "aws_secret_access_key = from-config\n"
                             "stray line with no separator\n");
    const std::string credentials_path =
        tree.write("credentials", "[work]\nAWS_ACCESS_KEY_ID = FROM_CREDENTIALS\n"
                                  "aws_session_token = sess-from-credentials\n");
    ConfigBuilder merged = empty_builder();
    OK(merged.set("AWS_CONFIG_FILE", config_path.c_str()));
    OK(merged.set("AWS_SHARED_CREDENTIALS_FILE", credentials_path.c_str()));
    OK(merged.set("AWS_PROFILE", "work"));
    auto profile = load(merged);
    expect_credentials(__LINE__, profile, "FROM_CREDENTIALS", "from-config",
                       "sess-from-credentials", "us-east-2");
    if (profile)
        EQ(profile->expires_at, 0);

    const std::string key_only_path =
        tree.write("key-only", "[default]\naws_access_key_id = AKIPROFILE\n");
    ConfigBuilder key_only = empty_builder();
    OK(key_only.set("AWS_SHARED_CREDENTIALS_FILE", key_only_path.c_str()));
    denied(__LINE__, load(key_only), "AWS profile access key and secret must be set together");

    const std::string secret_only_path =
        tree.write("secret-only", "[default]\naws_secret_access_key = profile-secret\n");
    ConfigBuilder secret_only = empty_builder();
    OK(secret_only.set("AWS_SHARED_CREDENTIALS_FILE", secret_only_path.c_str()));
    denied(__LINE__, load(secret_only), "AWS profile access key and secret must be set together");

    const std::string sso_session_path =
        tree.write("sso-session", "[default]\nsso_session = corp\nsso_account_id = 1234\n");
    ConfigBuilder sso_session = empty_builder();
    OK(sso_session.set("AWS_CONFIG_FILE", sso_session_path.c_str()));
    denied(__LINE__, load(sso_session),
           "AWS IAM Identity Center profiles require a custom credential provider");

    // read_ini splits on the first of "=:", so the '=' here wins and the URL
    // keeps its scheme; a key written with ':' instead would truncate it.
    const std::string sso_url_path =
        tree.write("sso-url", "[default]\nsso_start_url = https://example.awsapps.com/start\n");
    ConfigBuilder sso_url = empty_builder();
    OK(sso_url.set("AWS_CONFIG_FILE", sso_url_path.c_str()));
    denied(__LINE__, load(sso_url),
           "AWS IAM Identity Center profiles require a custom credential provider");

    const std::string source_path =
        tree.write("source", "[default]\nsource_profile = base\nregion = eu-west-1\n");
    ConfigBuilder source_profile = empty_builder();
    OK(source_profile.set("AWS_CONFIG_FILE", source_path.c_str()));
    denied(__LINE__, load(source_profile),
           "AWS AssumeRole profiles require a custom credential provider");

    // A web identity token file with no role never reaches the STS exchange,
    // so it falls into the same AssumeRole rejection.
    const std::string token_path = tree.write("token", "fixture-token\n");
    const std::string orphan_token_config =
        tree.write("orphan-token", "[default]\nweb_identity_token_file = " + token_path + "\n");
    ConfigBuilder orphan_token = empty_builder();
    OK(orphan_token.set("AWS_CONFIG_FILE", orphan_token_config.c_str()));
    denied(__LINE__, load(orphan_token),
           "AWS AssumeRole profiles require a custom credential provider");

    constexpr std::string_view chained =
        "AWS chained AssumeRole profiles require a custom credential provider; "
        "credential_process and web identity profiles are supported natively";
    ConfigBuilder role_from_option = empty_builder();
    OK(role_from_option.set("AWS_ROLE_ARN", "arn:aws:iam::123456789012:role/karu"));
    denied(__LINE__, load(role_from_option), chained);

    const std::string role_config =
        tree.write("role", "[default]\nrole_arn = arn:aws:iam::123456789012:role/karu\n");
    ConfigBuilder role_from_profile = empty_builder();
    OK(role_from_profile.set("AWS_CONFIG_FILE", role_config.c_str()));
    denied(__LINE__, load(role_from_profile), chained);
}

void credential_process_cases() {
    SECTION("AWS credential_process");
    const TempTree tree("aws_credential_process");
    // The helper modes run this very binary, which is the only command spelling
    // that behaves identically under cmd.exe and sh.
    const std::string ok_config =
        tree.write("ok-config", "[default]\nregion = ap-south-1\ncredential_process = " +
                                    credential_process_command("ok") + "\n");
    ConfigBuilder helper = empty_builder();
    OK(helper.set("AWS_CONFIG_FILE", ok_config.c_str()));
    auto helped = load(helper);
    // The helper spells the token "SessionToken", so a success here is also the
    // proof that aws_json_credentials falls back from "Token".
    expect_credentials(__LINE__, helped, "AKIAHELPER", "helper-secret", "helper-token",
                       "ap-south-1");
    if (helped)
        OK(helped->expires_at > static_cast<std::int64_t>(std::time(nullptr)));

    const std::string incomplete_config = tree.write(
        "incomplete-config",
        "[default]\ncredential_process = " + credential_process_command("incomplete") + "\n");
    ConfigBuilder incomplete = empty_builder();
    OK(incomplete.set("AWS_CONFIG_FILE", incomplete_config.c_str()));
    denied(__LINE__, load(incomplete), "AWS credential_process response is incomplete");

    const std::string failing_config =
        tree.write("failing-config",
                   "[default]\ncredential_process = " + credential_process_command("fail") + "\n");
    ConfigBuilder failing = empty_builder();
    OK(failing.set("AWS_CONFIG_FILE", failing_config.c_str()));
    denied(__LINE__, load(failing), "AWS credential_process exited with status 3");

    const std::string flood_config =
        tree.write("flood-config",
                   "[default]\ncredential_process = " + credential_process_command("flood") + "\n");
    ConfigBuilder flood = empty_builder();
    OK(flood.set("AWS_CONFIG_FILE", flood_config.c_str()));
    const auto flood_started = std::chrono::steady_clock::now();
    denied(__LINE__, load(flood), "AWS credential_process returned more than 1 MiB");
    // The command runs under the 120 s default request timeout, so a regression
    // that stops enforcing the cap would otherwise stall the whole suite.
    OK(std::chrono::steady_clock::now() - flood_started < std::chrono::seconds(10));

    // An embedded NUL is the one portable way to make run_command refuse before
    // it spawns anything; read_text_file is binary, so the byte survives the ini.
    std::string nul_contents = "[default]\ncredential_process = echo";
    nul_contents.push_back('\0');
    nul_contents += "hi\n";
    const std::string nul_config = tree.write("nul-config", nul_contents);
    ConfigBuilder cannot_start = empty_builder();
    OK(cannot_start.set("AWS_CONFIG_FILE", nul_config.c_str()));
    denied_prefix(__LINE__, load(cannot_start), "AWS credential_process could not start: ");
}

void json_documents() {
    SECTION("AWS credential JSON decoding");
    // aws_json_credentials is the only AWS entry into json_string. The fixture
    // routes and credential_process helper modes all serve well-formed
    // documents, so the malformed shapes are exercised here directly.
    const auto shadowed_key = backends::aws_json_credentials(
        R"({"note":"AccessKeyId","AccessKeyId":"AKIAREAL","SecretAccessKey":"s"})");
    EQS(shadowed_key.access_key_id, "AKIAREAL");
    EQS(shadowed_key.secret_access_key, "s");
    EQ(shadowed_key.expires_at, 0);

    const auto numeric =
        backends::aws_json_credentials(R"({"AccessKeyId":123,"SecretAccessKey":"s"})");
    EQS(numeric.access_key_id, "");

    const auto escaped = backends::aws_json_credentials(
        "{\"AccessKeyId\":\"A\\/B\",\"SecretAccessKey\":\"x\\ty\\nz\\\\w\\\"q\\b\\f\\r\"}");
    EQS(escaped.access_key_id, "A/B");
    EQS(escaped.secret_access_key, "x\ty\nz\\w\"q\b\f\r");

    const auto bad_escape =
        backends::aws_json_credentials(R"({"AccessKeyId":"a\qb","SecretAccessKey":"s"})");
    EQS(bad_escape.access_key_id, "");

    const auto unterminated = backends::aws_json_credentials(R"({"AccessKeyId":"unterminated)");
    EQS(unterminated.access_key_id, "");

    // "Token" must not be matched inside "SessionToken": the needle carries the
    // leading quote, which is what makes the fallback at line 103 fire.
    const auto session_spelling = backends::aws_json_credentials(
        R"({"AccessKeyId":"A","SecretAccessKey":"s","SessionToken":"t"})");
    EQS(session_spelling.session_token, "t");
    const auto token_spelling = backends::aws_json_credentials(
        R"({"AccessKeyId":"A","SecretAccessKey":"s","Token":"t","SessionToken":"ignored"})");
    EQS(token_spelling.session_token, "t");

    // An unparsable Expiration becomes zero rather than an error, which is why
    // the loaders can treat "expired" and "malformed" as one rejection.
    const auto bad_expiry = backends::aws_json_credentials(
        R"({"AccessKeyId":"A","SecretAccessKey":"s","Token":"t","Expiration":"not-a-date"})");
    EQ(bad_expiry.expires_at, 0);
    const auto past_expiry =
        backends::aws_json_credentials(R"({"AccessKeyId":"A","SecretAccessKey":"s","Token":"t",)"
                                       R"("Expiration":"2001-02-03T04:05:06Z"})");
    EQ(past_expiry.expires_at, 981'173'106);
}

void web_identity_failures() {
    SECTION("AWS web identity failures");
    const TempTree tree("aws_web_identity_failures");
    const std::string absent_token = tree.absent("absent-token");
    ConfigBuilder missing_token = empty_builder();
    OK(missing_token.set("AWS_ROLE_ARN", "arn:aws:iam::123456789012:role/karu"));
    OK(missing_token.set("AWS_WEB_IDENTITY_TOKEN_FILE", absent_token.c_str()));
    // Nothing listens on port 9; the token read comes first, so it is never dialled.
    OK(missing_token.set("AWS_STS_ENDPOINT", "http://127.0.0.1:9/unused"));
    denied(__LINE__, load(missing_token),
           "AWS web identity token: cannot open '" + absent_token + "'");

    const std::string huge_token = tree.write("huge-token", std::string((1u << 20) + 1, 'A'));
    ConfigBuilder oversize_token = empty_builder();
    OK(oversize_token.set("AWS_ROLE_ARN", "arn:aws:iam::123456789012:role/karu"));
    OK(oversize_token.set("AWS_WEB_IDENTITY_TOKEN_FILE", huge_token.c_str()));
    OK(oversize_token.set("AWS_STS_ENDPOINT", "http://127.0.0.1:9/unused"));
    denied(__LINE__, load(oversize_token), "AWS web identity token: file exceeds 1 MiB");

    const std::string token_path = tree.write("token", "fixture-token\n");
    const auto with_endpoint = [&](const std::string& endpoint) {
        ConfigBuilder builder = empty_builder();
        OK(builder.set("AWS_ROLE_ARN", "arn:aws:iam::123456789012:role/karu"));
        OK(builder.set("AWS_WEB_IDENTITY_TOKEN_FILE", token_path.c_str()));
        OK(builder.set("AWS_STS_ENDPOINT", endpoint.c_str()));
        return load(builder);
    };

    // A refused connection must surface as a transport error, never as an
    // "HTTP 0" response that the status check would then misreport.
    auto refused = with_endpoint("http://127.0.0.1:9/sts");
    denied_prefix(__LINE__, refused, "credential endpoint: ");
    if (!refused)
        lacks(__LINE__, refused.error().message, "AWS web identity exchange returned HTTP 0");

    denied(__LINE__, with_endpoint(fixture_url("/aws/sts/denied")),
           "AWS web identity exchange returned HTTP 403");
    // The fixture omits SecretAccessKey, so one response walks both the found
    // and the missing-open-tag arms of the XML reader.
    denied(__LINE__, with_endpoint(fixture_url("/aws/sts/incomplete")),
           "AWS web identity response is incomplete");

    // The third arm, a missing close tag, needs a body no fixture route serves.
    // /_count echoes its `path` query value back inside its JSON reply, which
    // puts one on the wire without adding a route: AccessKeyId has both tags,
    // SecretAccessKey has neither, and SessionToken is never closed.
    const std::string unterminated = fixture_url(
        "/_count?path=<AccessKeyId>ASIASTSFIXTURE</AccessKeyId><SessionToken>unterminated");
    denied(__LINE__, with_endpoint(unterminated), "AWS web identity response is incomplete");
    // Every string is present here; only the expiry check can reject it.
    denied(__LINE__, with_endpoint(fixture_url("/aws/sts/expired")),
           "AWS web identity response is incomplete");
}

void web_identity_success() {
    SECTION("AWS web identity exchange");
    const TempTree tree("aws_web_identity");
    // The padded token file pins the trim before form encoding; the role, the
    // token path, the absent session name and the region all arrive through the
    // profile fallbacks rather than through options.
    const std::string token_path = tree.write("token", "  fixture-token\n");
    const std::string config_path =
        tree.write("config", "[default]\nrole_arn = arn:aws:iam::123456789012:role/karu\n"
                             "web_identity_token_file = " +
                                 token_path + "\nregion = us-east-2\n");
    ConfigBuilder from_profile = empty_builder();
    OK(from_profile.set("AWS_CONFIG_FILE", config_path.c_str()));
    OK(from_profile.set("AWS_STS_ENDPOINT", fixture_url("/aws/sts/ok").c_str()));
    // The failure cases above already posted a well-formed AssumeRole form to
    // this fixture. Without the reset, every contains() below would be
    // satisfied by one of those and could not fail for this exchange.
    reset_fixture_log();
    auto generated = load(from_profile);
    expect_credentials(__LINE__, generated, "ASIASTSFIXTURE", "sts-secret", "sts-session",
                       "us-east-2");
    if (generated)
        OK(generated->expires_at > static_cast<std::int64_t>(std::time(nullptr)));

    // Read the fixture log before the explicit-name exchange below. After the
    // reset it holds exactly this one exchange, so each assertion names it.
    const std::string after_generated = fixture_requests();
    contains(__LINE__, after_generated, "Action=AssumeRoleWithWebIdentity&Version=2011-06-15");
    lacks(__LINE__, after_generated, "RoleSessionName=karu-test-session");
    // The padding in the token file must not reach the wire.
    contains(__LINE__, after_generated, "WebIdentityToken=fixture-token");
    lacks(__LINE__, after_generated, "WebIdentityToken=%20%20fixture-token");
    const std::size_t name_start = after_generated.rfind("RoleSessionName=karu-");
    if (name_start == std::string::npos) {
        fail(__LINE__, "the fixture never saw a generated role session name");
    } else {
        const std::size_t digits = name_start + std::string_view("RoleSessionName=karu-").size();
        const std::size_t end = after_generated.find('&', digits);
        const std::string suffix = after_generated.substr(
            digits, end == std::string::npos ? std::string::npos : end - digits);
        // The generated name is "karu-" plus the current epoch second.
        OK(!suffix.empty());
        OK(suffix.find_first_not_of("0123456789") == std::string::npos);
        if (!suffix.empty() && suffix.find_first_not_of("0123456789") == std::string::npos) {
            const std::int64_t stamp = std::stoll(suffix);
            OK(std::llabs(stamp - static_cast<std::int64_t>(std::time(nullptr))) < 300);
        }
    }

    // No profile at all, an explicit session name, and a role whose '+' and '/'
    // must reach the wire percent-encoded.
    const std::string plain_token = tree.write("plain-token", "fixture-token");
    ConfigBuilder from_options = empty_builder();
    OK(from_options.set("AWS_ROLE_ARN", "arn:aws:iam::123456789012:role/karu+slash/path"));
    OK(from_options.set("AWS_WEB_IDENTITY_TOKEN_FILE", plain_token.c_str()));
    OK(from_options.set("AWS_ROLE_SESSION_NAME", "karu-test-session"));
    OK(from_options.set("AWS_STS_ENDPOINT", fixture_url("/aws/sts/ok").c_str()));
    reset_fixture_log();
    auto explicit_session = load(from_options);
    // The profile region is empty, so the success path assigns "" over whatever
    // the exchange produced.
    expect_credentials(__LINE__, explicit_session, "ASIASTSFIXTURE", "sts-secret", "sts-session",
                       "");
    if (explicit_session)
        OK(explicit_session->expires_at > static_cast<std::int64_t>(std::time(nullptr)));

    const std::string after_explicit = fixture_requests();
    contains(__LINE__, after_explicit, "RoleSessionName=karu-test-session");
    // '+' and '/' in the role ARN have to arrive percent-encoded.
    contains(__LINE__, after_explicit,
             "RoleArn=arn%3Aaws%3Aiam%3A%3A123456789012%3Arole%2Fkaru%2Bslash%2Fpath");
}

void container_endpoint_rules() {
    SECTION("AWS container endpoint allowlist");
    constexpr std::string_view unsafe =
        "AWS_CONTAINER_CREDENTIALS_FULL_URI must use HTTPS or a loopback/ECS host";
    // None of these opens a socket or resolves a name: the check is pure string
    // work and every input fails it.
    const auto rejected = [&](int line, const char* uri) {
        ConfigBuilder builder = empty_builder();
        if (!builder.set("AWS_CONTAINER_CREDENTIALS_FULL_URI", uri))
            fail(line, std::string("could not set the container URI to '") + uri + "'");
        denied(line, load(builder), unsafe);
    };

    rejected(__LINE__, "http://creds.example.com:8080/v2/credentials");
    // No ':' at all in the authority, and an upper-case host that only matches
    // the allowlist once it has been lowered - which it still must not.
    rejected(__LINE__, "http://LOCALHOST.example.com/creds");
    // Userinfo is rejected inside the URL splitter, before the host allowlist.
    rejected(__LINE__, "http://user:pass@127.0.0.1:8080/creds");
    rejected(__LINE__, "127.0.0.1/creds");
    rejected(__LINE__, "ftp://127.0.0.1/creds");
    rejected(__LINE__, "http:///creds");
    // The authority stops at the first '/', so this bracket is never closed.
    rejected(__LINE__, "http://[::1/creds");
    // ']' is found but what follows it is not a port.
    rejected(__LINE__, "http://[::1]x:8080/creds");
    // A closing bracket at the very end of the authority, and a host that is
    // not one of the four accepted literals.
    rejected(__LINE__, "http://[fe80::1]/creds");

    const auto relative_rejected = [&](int line, const char* uri) {
        ConfigBuilder builder = empty_builder();
        if (!builder.set("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI", uri))
            fail(line, std::string("could not set the relative container URI to '") + uri + "'");
        denied(line, load(builder), unsafe);
    };

    // The relative URI is composed onto the ECS literal and then rejected on
    // the fragment and on the userinfo respectively, so 169.254.170.2 is never
    // contacted from a unit test.
    relative_rejected(__LINE__, "/v2/credentials/abc#fragment");
    relative_rejected(__LINE__, "@evil.example");
}

void container_requests() {
    SECTION("AWS container credentials");
    const TempTree tree("aws_container");

    // https short-circuits the host allowlist. The host is deliberately outside
    // it, so a transport error - rather than the allowlist message - is what
    // proves the scheme was enough. Port 9 is never listening.
    ConfigBuilder secure = empty_builder();
    OK(secure.set("AWS_CONTAINER_CREDENTIALS_FULL_URI", "https://0.0.0.0:9/creds"));
    denied_prefix(__LINE__, load(secure), "credential endpoint: ");

    // The fixture binds IPv4 only, so ::1 on its port is refused; what matters
    // is that the bracketed loopback form was accepted by the allowlist.
    ConfigBuilder bracketed = empty_builder();
    const std::string ipv6_url =
        "http://[::1]:" + std::to_string(fixture_port()) + "/aws/container/unserved";
    OK(bracketed.set("AWS_CONTAINER_CREDENTIALS_FULL_URI", ipv6_url.c_str()));
    denied_prefix(__LINE__, load(bracketed), "credential endpoint: ");

    // "LOCALHOST" only reaches the allowlist through lower(), and this is the
    // one input that makes the host == "localhost" comparison true; every other
    // accepted case in this file spells the loopback address numerically.
    // Nothing listens on port 9, and it is refused on both ::1 and 127.0.0.1,
    // so the failure is immediate however the name resolves - and it must be a
    // transport error, not the allowlist message.
    ConfigBuilder named_loopback = empty_builder();
    OK(named_loopback.set("AWS_CONTAINER_CREDENTIALS_FULL_URI", "http://LOCALHOST:9/creds"));
    denied_prefix(__LINE__, load(named_loopback), "credential endpoint: ");

    ConfigBuilder unreachable = empty_builder();
    OK(unreachable.set("AWS_CONTAINER_CREDENTIALS_FULL_URI", "http://127.0.0.1:9/creds"));
    auto refused = load(unreachable);
    denied_prefix(__LINE__, refused, "credential endpoint: ");
    if (!refused)
        lacks(__LINE__, refused.error().message, "AWS container credentials request failed");

    // The token file is padded, so the Authorization header only matches once
    // it has been trimmed.
    const std::string token_path = tree.write("container-token", "  Bearer fixture-token\n");
    ConfigBuilder from_file = empty_builder();
    OK(from_file.set("AWS_CONTAINER_CREDENTIALS_FULL_URI",
                     fixture_url("/aws/container/ok").c_str()));
    OK(from_file.set("AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE", token_path.c_str()));
    // Each of the two requests is read back on its own. Both send the same
    // header, so a shared log would let either one satisfy the assertion for
    // the other and the trim below could regress unnoticed.
    reset_fixture_log();
    auto with_token = load(from_file);
    expect_credentials(__LINE__, with_token, "ASIACONTAINER", "container-secret", "container-token",
                       "");
    if (with_token)
        OK(with_token->expires_at > static_cast<std::int64_t>(std::time(nullptr)));
    const std::string after_file_token = fixture_requests();
    contains(__LINE__, after_file_token, "\"authorization\": \"Bearer fixture-token\"");
    lacks(__LINE__, after_file_token, "  Bearer fixture-token");

    // A direct token wins outright: the token file here does not exist, so any
    // attempt to read it would surface as "cannot open".
    ConfigBuilder direct_token = empty_builder();
    OK(direct_token.set("AWS_CONTAINER_CREDENTIALS_FULL_URI",
                        fixture_url("/aws/container/ok").c_str()));
    OK(direct_token.set("AWS_CONTAINER_AUTHORIZATION_TOKEN", "Bearer fixture-token"));
    OK(direct_token.set("AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE",
                        tree.absent("absent-token").c_str()));
    reset_fixture_log();
    expect_credentials(__LINE__, load(direct_token), "ASIACONTAINER", "container-secret",
                       "container-token", "");
    // A direct token is never trimmed, so this one proves the header survived
    // the round trip rather than that trim() ran.
    contains(__LINE__, fixture_requests(), "\"authorization\": \"Bearer fixture-token\"");

    ConfigBuilder absent_token_file = empty_builder();
    OK(absent_token_file.set("AWS_CONTAINER_CREDENTIALS_FULL_URI", "http://127.0.0.1:9/creds"));
    const std::string absent_token = tree.absent("absent-container-token");
    OK(absent_token_file.set("AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE", absent_token.c_str()));
    denied(__LINE__, load(absent_token_file),
           "AWS container authorization token: cannot open '" + absent_token + "'");

    const std::string huge_token = tree.write("huge-token", std::string((1u << 20) + 1, 'A'));
    ConfigBuilder oversize_token = empty_builder();
    OK(oversize_token.set("AWS_CONTAINER_CREDENTIALS_FULL_URI", "http://127.0.0.1:9/creds"));
    OK(oversize_token.set("AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE", huge_token.c_str()));
    denied(__LINE__, load(oversize_token), "AWS container authorization token: file exceeds 1 MiB");

    // A direct token is not trimmed, so an embedded CRLF reaches the header
    // builder. It must be refused before the request goes out.
    ConfigBuilder injected = empty_builder();
    OK(injected.set("AWS_CONTAINER_CREDENTIALS_FULL_URI", "http://127.0.0.1:9/creds"));
    OK(injected.set("AWS_CONTAINER_AUTHORIZATION_TOKEN", "Bearer x\r\nX-Injected: y"));
    denied(__LINE__, load(injected), "credential header is invalid");

    const auto anonymous = [&](int line, const std::string& route) {
        ConfigBuilder builder = empty_builder();
        if (!builder.set("AWS_CONTAINER_CREDENTIALS_FULL_URI", fixture_url(route).c_str()))
            fail(line, "could not set the container URI");
        return load(builder);
    };

    // No authorization option at all, and the fixture spells the token
    // "SessionToken".
    auto unauthenticated = anonymous(__LINE__, "/aws/container/session-token-field");
    expect_credentials(__LINE__, unauthenticated, "ASIACONTAINER", "container-secret",
                       "container-session-token", "");
    if (unauthenticated)
        OK(unauthenticated->expires_at > static_cast<std::int64_t>(std::time(nullptr)));

    denied(__LINE__, anonymous(__LINE__, "/aws/container/denied"),
           "AWS container credentials request failed");
    // The status check is a 2xx range test, so a redirect is a failure too and
    // no Location is followed.
    denied(__LINE__, anonymous(__LINE__, "/status/301"),
           "AWS container credentials request failed");

    // No fields at all, then a document missing only the token.
    denied(__LINE__, anonymous(__LINE__, "/oauth/no-token"),
           "AWS container credentials are incomplete");
    denied(__LINE__, anonymous(__LINE__, "/aws/container/incomplete"),
           "AWS container credentials are incomplete");
    // Every string is present here; only the expiry check can reject it.
    denied(__LINE__, anonymous(__LINE__, "/aws/container/expired"),
           "AWS container credentials are incomplete");
}

void instance_metadata_gate() {
    SECTION("AWS instance metadata gate");
    // The anonymous path the S3 backend relies on. The elapsed-time assertion
    // is the guard against a future change quietly probing 169.254.169.254
    // from the unit suite.
    const ConfigBuilder nothing = empty_builder();
    const auto started = std::chrono::steady_clock::now();
    auto anonymous = load(nothing);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    expect_credentials(__LINE__, anonymous, "", "", "", "");
    if (anonymous) {
        EQS(anonymous->bearer_token, "");
        EQ(anonymous->expires_at, 0);
    }
    OK(elapsed < std::chrono::seconds(1));

    // Every spelling option_is_true accepts, paired with both ends of the
    // metadata timeout range. AWS_EC2_METADATA_DISABLED=NO is deliberately
    // absent: it would open the gate and really dial the link-local address.
    const char* const disabled[] = {"YES", "true", "ON", "1"};
    const char* const timeouts[] = {"60", "1"};
    for (const char* value : disabled) {
        for (const char* timeout : timeouts) {
            ConfigBuilder builder = empty_builder();
            OK(builder.set("AWS_EC2_METADATA_DISABLED", value));
            OK(builder.set("AWS_METADATA_SERVICE_TIMEOUT", timeout));
            const auto gate_started = std::chrono::steady_clock::now();
            expect_credentials(__LINE__, load(builder), "", "", "", "");
            OK(std::chrono::steady_clock::now() - gate_started < std::chrono::seconds(1));
        }
    }
}

void web_identity_token_path_expansion() {
    SECTION("AWS web identity token path expansion");
    const TempTree tree("aws_tilde");
    // This is the only case that needs ConfigBuilder(true), because it is the
    // only builder that fills home_directory_. The entire option table is unset
    // first, plus the two directory variables the builder also reads, so no
    // exported value can reach the snapshot; every override is constructed
    // before the builder, which samples the environment in its constructor.
    std::vector<std::unique_ptr<ScopedEnvironment>> overrides;
    config_options::for_each_environment([&](const char* name) {
        overrides.push_back(std::make_unique<ScopedEnvironment>(name, nullptr));
    });
    for (const char* name : {"XDG_CACHE_HOME", "APPDATA"})
        overrides.push_back(std::make_unique<ScopedEnvironment>(name, nullptr));
    // POSIX reads HOME, Windows reads USERPROFILE; setting both keeps the
    // expansion pointed at the temporary tree everywhere.
    overrides.push_back(std::make_unique<ScopedEnvironment>("HOME", tree.root().c_str()));
    overrides.push_back(std::make_unique<ScopedEnvironment>("USERPROFILE", tree.root().c_str()));

    ConfigBuilder builder(true);
    OK(builder.set("AWS_ROLE_ARN", "arn:aws:iam::123456789012:role/karu"));
    OK(builder.set("AWS_WEB_IDENTITY_TOKEN_FILE", "~/absent-token"));
    OK(builder.set("AWS_STS_ENDPOINT", "http://127.0.0.1:9/unused"));
    // The token read fails before any request, and before the metadata gate
    // that discover_default_credentials() would otherwise open.
    denied(__LINE__, load(builder),
           "AWS web identity token: cannot open '" + tree.root() + "/absent-token'");
}

} // namespace

void test_aws_credentials() {
    direct_keys();
    profile_files();
    profile_values();
    credential_process_cases();
    json_documents();
    web_identity_failures();
    web_identity_success();
    container_endpoint_rules();
    container_requests();
    instance_metadata_gate();
    web_identity_token_path_expansion();
}

} // namespace karu::test
