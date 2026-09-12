#ifndef KARU_CREDENTIAL_SUPPORT_HPP
#define KARU_CREDENTIAL_SUPPORT_HPP

#include "config.hpp"
#include "request.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace karu::test {

// A temporary directory that removes itself. Credential fixtures live here so
// that no test ever reads the developer's real ~/.aws or ~/.config/gcloud.
class TempTree {
  public:
    explicit TempTree(std::string_view label);
    ~TempTree();

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    // Writes contents to a relative path inside the tree, creating parents.
    // Returns the absolute path.
    [[nodiscard]] std::string write(std::string_view relative, std::string_view contents) const;
    // An absolute path inside the tree that is not created.
    [[nodiscard]] std::string absent(std::string_view relative) const;
    [[nodiscard]] const std::string& root() const noexcept { return root_; }

  private:
    std::string root_;
};

// A PEM-encoded RSA private key generated once per process. Real enough for
// PEM_read_bio_PrivateKey and EVP_DigestSign, and never a checked-in secret.
[[nodiscard]] const std::string& test_private_key_pem();

// The absolute path of the running test binary, quoted for the platform shell.
// AWS and Source credential_process fixtures invoke it, which is the only way
// to spell one command that works under both cmd.exe and sh.
void set_program_path(const char* argv0);
[[nodiscard]] std::string credential_process_command(std::string_view mode);

// Handles the credential_process child modes. Returns true when the process was
// started as a helper and should exit with the returned status.
[[nodiscard]] bool run_credential_helper(int argc, char** argv, int& exit_code);

// The loopback fixture port passed by credential_server.py, or 0 when the tests
// run without it.
void set_fixture_port(int port);
[[nodiscard]] int fixture_port();
[[nodiscard]] std::string fixture_url(std::string_view route);
// Checked access to the fixture's shared request log. Keeping this here avoids
// per-provider reset implementations that can silently accept stale requests.
[[nodiscard]] std::string fixture_request_body(int line, std::string_view route);
void reset_fixture_requests(int line);

// Convenience builders used by the per-provider suites.
[[nodiscard]] ConfigBuilder empty_builder();
[[nodiscard]] std::string iso8601_in(std::int64_t seconds_from_now);

// Suites in the credential binary. Each lives in its own translation unit.
void test_aws_credentials();
void test_gcs_credentials();
void test_azure_credentials_loader();
void test_source_credentials_loader();
void test_credential_helpers();
void test_transport_options();
void test_credential_gaps();

} // namespace karu::test

#endif
