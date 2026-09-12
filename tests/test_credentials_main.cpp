// Credential-loading suite. Runs under tests/credential_server.py, which binds
// a loopback fixture and passes its port as argv[1].
//
// The binary doubles as its own credential_process helper: invoked with
// --credential-helper=MODE it prints a credential document and exits. That is
// the only spelling of one command that behaves the same under cmd.exe and sh.
#include "credential_support.hpp"
#include "karu/karu.h"
#include "test_support.hpp"

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    using namespace karu::test;

    int helper_status = 0;
    if (run_credential_helper(argc, argv, helper_status))
        return helper_status;

    set_program_path(argv[0]);
    if (argc > 1)
        set_fixture_port(std::atoi(argv[1]));
    if (fixture_port() <= 0) {
        std::fputs("credential tests need a fixture port; run them through "
                   "tests/credential_server.py\n",
                   stderr);
        return 2;
    }
    if (test_private_key_pem().empty()) {
        std::fputs("could not generate an RSA key for the service account fixtures\n", stderr);
        return 2;
    }

    std::printf("karu %s credentials (%s)\n", karu_version_string(), karu_http_backend());
    test_credential_helpers();
    test_aws_credentials();
    test_gcs_credentials();
    test_azure_credentials_loader();
    test_source_credentials_loader();
    test_transport_options();
    test_credential_gaps();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
