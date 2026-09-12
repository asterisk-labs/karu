// Branches the per-provider suites could not reach on their own, because each
// needs a response shape or a helper behaviour that only the fixture can
// produce, plus the null-argument contract of the C accessors.
#include "backends/aws_profile.hpp"
#include "backends/credentials.hpp"
#include "backends/s3_credentials.hpp"
#include "credential_support.hpp"
#include "karu/karu.h"
#include "test_support.hpp"

#include <string>

namespace karu::test {
namespace {

void null_accessors() {
    // The C API promises a defined answer for a null handle rather than a crash,
    // which is what a binding does while unwinding a failed constructor.
    EQ(karu_client_concurrency(nullptr), 0);
    EQ(karu_client_coalesce_gap(nullptr), std::uint64_t{0});
    EQ(karu_client_max_attempts(nullptr), 0);
    EQS(karu_locator_uri(nullptr), "");
    EQ(karu_locator_is_remote(nullptr), 0);
    EQ(karu_locator_window_offset(nullptr), std::uint64_t{0});
    EQ(karu_locator_window_length(nullptr), std::uint64_t{0});

    // Freeing a null handle is a no-op on every type.
    karu_config_free(nullptr);
    karu_client_free(nullptr);
    karu_locator_free(nullptr);
    karu_batch_free(nullptr);
    karu_free(nullptr);
    OK(true);
}

void oauth_response_shapes() {
    // A 200 whose access_token is present but empty is not a credential. The
    // provider suites cannot ask for this shape, because the token endpoint they
    // reach is fixed by the provider's own URL construction.
    auto empty = backends::oauth_token(fixture_url("/oauth/empty-token"), "grant_type=none");
    OK(!empty.has_value());
    if (!empty) {
        EQ(empty.error().status, KARU_ERR_CREDENTIALS);
        OK(empty.error().message.find("no access_token") != std::string::npos);
    }

    // expires_in arrives quoted from some issuers, so the lifetime has to come
    // out of a JSON string as well as a JSON number.
    auto quoted = backends::oauth_token(fixture_url("/oauth/quoted-expiry"), "grant_type=none");
    OK(quoted.has_value());
    if (quoted) {
        EQS(quoted->bearer_token, "quoted-expiry-token");
        const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        OK(quoted->expires_at > now + 600);
        OK(quoted->expires_at <= now + 900);
    }
}

void oversized_credential_response() {
    // Credential replies are capped: a malicious or misconfigured endpoint must
    // not be able to make karu buffer an unbounded body.
    auto response = backends::credential_request("GET", fixture_url("/oversized"), {}, {});
    OK(!response.has_value());
    if (!response)
        EQ(response.error().status, KARU_ERR_CREDENTIALS);
}

void container_reply_without_secret() {
    // A container reply carrying only an access key is incomplete. The other
    // two disjuncts of that check already have fixtures; this is the third.
    ConfigBuilder builder(false);
    OK(builder
           .set("AWS_CONTAINER_CREDENTIALS_FULL_URI",
                fixture_url("/aws/container/key-only").c_str())
           .has_value());
    auto loaded = backends::load_aws_credentials(must_freeze(builder), "/vsis3/bucket/key");
    OK(!loaded.has_value());
    if (!loaded) {
        EQ(loaded.error().status, KARU_ERR_CREDENTIALS);
        OK(loaded.error().message.find("incomplete") != std::string::npos);
    }
}

void unterminated_sts_element() {
    // xml_value finds the opening tag and then runs off the end of the body.
    // A truncated response must read as incomplete, never as a partial success.
    TempTree tree("gaps-sts");
    const std::string token = tree.write("web-identity-token", "fixture-token\n");
    ConfigBuilder builder(false);
    OK(builder.set("AWS_ROLE_ARN", "arn:aws:iam::123456789012:role/karu").has_value());
    OK(builder.set("AWS_WEB_IDENTITY_TOKEN_FILE", token.c_str()).has_value());
    OK(builder.set("AWS_STS_ENDPOINT", fixture_url("/aws/sts/unterminated").c_str()).has_value());
    auto loaded = backends::load_aws_credentials(must_freeze(builder), "/vsis3/bucket/key");
    OK(!loaded.has_value());
    if (!loaded) {
        EQ(loaded.error().status, KARU_ERR_CREDENTIALS);
        OK(loaded.error().message.find("incomplete") != std::string::npos);
    }
}

void credential_process_without_access_key() {
    // The helper prints a secret and no key. Both halves of the completeness
    // check have to reject, not just the missing-secret half.
    TempTree tree("gaps-process");
    const std::string credentials = tree.write(
        "credentials",
        "[default]\ncredential_process = " + credential_process_command("no-key") + "\n");
    ConfigBuilder builder(false);
    OK(builder.set("AWS_SHARED_CREDENTIALS_FILE", credentials.c_str()).has_value());
    auto loaded = backends::load_aws_credentials(must_freeze(builder), "/vsis3/bucket/key");
    OK(!loaded.has_value());
    if (!loaded) {
        EQ(loaded.error().status, KARU_ERR_CREDENTIALS);
        OK(loaded.error().message.find("credential_process response is incomplete") !=
           std::string::npos);
    }
}

void ini_section_without_close() {
    // A line that opens a section and never closes it is a key/value line with
    // no separator, so it is skipped rather than treated as a section header.
    TempTree tree("gaps-ini");
    const std::string path = tree.write("config", "[unterminated\n"
                                                  "[default]\n"
                                                  "aws_access_key_id = AKIAINI\n"
                                                  "aws_secret_access_key = ini-secret\n");
    auto parsed = backends::read_ini(path, "gap config");
    OK(parsed.has_value());
    if (parsed) {
        EQ(parsed->count("unterminated"), std::size_t{0});
        EQ(parsed->count("default"), std::size_t{1});
        if (parsed->count("default") == 1)
            EQS(parsed->at("default").at("aws_access_key_id"), "AKIAINI");
    }
}

} // namespace

void test_credential_gaps() {
    SECTION("fixture-only credential branches and null handles");
    null_accessors();
    oauth_response_shapes();
    oversized_credential_response();
    container_reply_without_secret();
    unterminated_sts_element();
    credential_process_without_access_key();
    ini_section_without_close();
}

} // namespace karu::test
