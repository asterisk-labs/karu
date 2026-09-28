#include "backends/credentials.hpp"
#include "karu/karu.h"
#include "test_support.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

namespace {

namespace fs = std::filesystem;
using namespace karu::test;

std::string make_certificate(const fs::path& directory, const std::string& name) {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_RSA_gen(2048), EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
    if (!key || !certificate)
        throw std::runtime_error("cannot allocate test certificate");
    X509_set_version(certificate.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1);
    X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60);
    X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 86400);
    X509_set_pubkey(certificate.get(), key.get());
    X509_NAME* subject = X509_get_subject_name(certificate.get());
    X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(name.c_str()), -1, -1, 0);
    X509_set_issuer_name(certificate.get(), subject);
    for (auto [nid, value] :
         {std::pair{NID_basic_constraints, "critical,CA:TRUE"},
          std::pair{NID_subject_alt_name, "DNS:localhost,DNS:proxy-only.invalid"}}) {
        std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
            X509V3_EXT_conf_nid(nullptr, nullptr, nid, value), X509_EXTENSION_free);
        if (!extension || !X509_add_ext(certificate.get(), extension.get(), -1))
            throw std::runtime_error("cannot extend test certificate");
    }
    if (!X509_sign(certificate.get(), key.get(), EVP_sha256()))
        throw std::runtime_error("cannot sign test certificate");
    std::unique_ptr<BIO, decltype(&BIO_free)> cert_file(
        BIO_new_file((directory / (name + ".pem")).string().c_str(), "w"), BIO_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> key_file(
        BIO_new_file((directory / (name + ".key")).string().c_str(), "w"), BIO_free);
    if (!cert_file || !key_file || !PEM_write_bio_X509(cert_file.get(), certificate.get()) ||
        !PEM_write_bio_PrivateKey(key_file.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr))
        throw std::runtime_error("cannot write test certificate");
    std::array<char, 16> hash{};
    std::snprintf(hash.data(), hash.size(), "%08lx.0", X509_subject_name_hash(certificate.get()));
    return hash.data();
}

void check_requests(const std::string& url, const karu::HttpRequestOptions& options, bool success) {
    karu_config* config = nullptr;
    karu_client* client = nullptr;
    karu_locator* locator = nullptr;
    EQ(karu_config_create_empty(&config), KARU_OK);
    EQ(karu_config_set_option(config, "KARU_HTTP_CA_BUNDLE", options.ca_bundle.c_str()), KARU_OK);
    if (!options.ca_path.empty())
        EQ(karu_config_set_option(config, "KARU_HTTP_CA_PATH", options.ca_path.c_str()), KARU_OK);
    EQ(karu_config_set_option(config, "KARU_MAX_ATTEMPTS", "1"), KARU_OK);
    EQ(karu_client_create(config, &client), KARU_OK);
    EQ(karu_resolve(url.c_str(), &locator), KARU_OK);
    std::array<unsigned char, 16> bytes{};
    karu_req request{locator, 0, bytes.size(), bytes.data(), nullptr, nullptr};
    EQ(karu_client_fetch(client, &request, 1) == KARU_OK, success);
    if (success)
        for (std::size_t index = 0; index < bytes.size(); ++index)
            EQ(bytes[index], (index * 31 + 7) & 0xff);
    karu_object_info info{};
    info.struct_size = sizeof(info);
    EQ(karu_client_stat(client, locator, &info) == KARU_OK, success);
    EQ(karu_client_read_ends(client, locator, bytes.data(), 8, bytes.data() + 8, 8, &info) ==
           KARU_OK,
       success);
    auto response = karu::backends::credential_request("GET", url, "", {}, 5, options);
    EQ(response.has_value(), success);
    if (response)
        EQ(response->status, 200);
    karu_locator_free(locator);
    karu_client_free(client);
    karu_config_free(config);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--init") {
        const fs::path root(argv[2]);
        fs::create_directory(root / "certs");
        const auto hash = make_certificate(root, "trusted");
        make_certificate(root, "untrusted");
        fs::copy_file(root / "trusted.pem", root / "certs" / hash);
        return 0;
    }
    if (argc != 4)
        return 2;
    SECTION("TLS trust sources");
    const fs::path root(argv[2]);
    const std::string url = "https://localhost:" + std::string(argv[1]) + "/object";
    karu::HttpRequestOptions options;
    options.ca_bundle = (root / "trusted.pem").string();
    check_requests(url, options, true);
    check_requests("https://127.0.0.1:" + std::string(argv[1]) + "/object", options, false);
    options.ca_bundle = (root / "untrusted.pem").string();
    check_requests(url, options, false);

    {
        SECTION("SOCKS5h through the proxy environment");
        const std::string proxy = "socks5h://127.0.0.1:" + std::string(argv[3]);
        ScopedEnvironment all_proxy("ALL_PROXY", proxy.c_str());
        ScopedEnvironment no_proxy("no_proxy", "");
        options.ca_bundle = (root / "trusted.pem").string();
        check_requests("https://proxy-only.invalid:" + std::string(argv[1]) + "/object", options,
                       true);
    }
    options.ca_bundle = (root / "untrusted.pem").string();
    const std::string directory = (root / "certs").string();
    OK(karu::backends::has_hashed_certificates(directory.c_str()));
    if (karu::backends::running_curl_ca_support().tls.starts_with("OpenSSL/")) {
        options.ca_path = directory;
        check_requests(url, options, true);
    }
    const std::string invalid = (root / "invalid").string();
    fs::create_directory(invalid);
    OK(!karu::backends::has_hashed_certificates(invalid.c_str()));
    std::ofstream(fs::path(invalid) / "01234567.0") << "not a certificate";
    OK(!karu::backends::has_hashed_certificates(invalid.c_str()));
    fs::copy_file(root / "trusted.pem", fs::path(invalid) / "12345678.0");
    OK(!karu::backends::has_hashed_certificates(invalid.c_str()));
    OK(!karu::backends::has_hashed_certificates(nullptr));
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
