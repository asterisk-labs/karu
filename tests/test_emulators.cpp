// Range reads against independent implementations of the storage protocols.
//
// tests/credential_server.py can only check Karu against what we believe the
// protocols do, because we wrote it. MinIO, Azurite and fake-gcs-server verify
// our signatures and compose their own responses, so a signing or range mistake
// shows up here as a rejected request rather than as a passing test.
//
// Start them with tests/emulators/up.sh. Each backend is probed independently
// and skipped when it is not listening, so a laptop with only Azurite still
// gets the Azure coverage. If none of them answers, the whole test reports the
// ctest skip status instead of failing.
#include "karu/karu.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr int kSkipExitCode = 77;
constexpr std::size_t kObjectSize = 64 * 1024;

int failures = 0;
int checks = 0;
int backends_run = 0;

std::vector<std::byte> fixture_object() {
    std::vector<std::byte> object(kObjectSize);
    for (std::size_t index = 0; index < object.size(); ++index)
        object[index] = static_cast<std::byte>((index * 31 + 7) & 0xFF);
    return object;
}

void check(bool condition, const std::string& what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("    FAIL %s\n", what.c_str());
    }
}

std::string environment(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : fallback;
}

// A backend counts as present when something answers on its endpoint. The
// status does not matter: an unauthenticated probe is expected to be refused.
bool listening(const std::string& url) {
    auto config = karu::Config::empty().value();
    if (!config.set("KARU_CONNECT_TIMEOUT", "2") || !config.set("KARU_MAX_ATTEMPTS", "1"))
        return false;
    auto client = karu::Client::create(config);
    if (!client)
        return false;
    auto object = karu::Object::parse(url);
    if (!object)
        return false;
    std::vector<std::byte> probe(1);
    auto read = client->read_into(*object, 0, probe);
    if (read)
        return true;
    // Anything the server answered is proof it is up; only a transport failure
    // means nothing is there.
    return read.error().status != KARU_ERR_NETWORK && read.error().status != KARU_TIMEOUT;
}

// The shared behaviour every backend has to get right. `object` is the blob the
// emulator was seeded with.
void exercise(const char* name, const karu::Client& client, const karu::Object& blob,
              const std::vector<std::byte>& object) {
    ++backends_run;
    std::printf("  %s\n", name);

    struct Range {
        std::uint64_t offset;
        std::size_t length;
    };
    // Single byte, aligned block, unaligned middle, last byte, and a read that
    // ends exactly at the object's end.
    const Range ranges[] = {{0, 1},
                            {0, 4096},
                            {1, 3},
                            {12345, 777},
                            {kObjectSize - 1, 1},
                            {60000, kObjectSize - 60000}};
    for (const Range& range : ranges) {
        std::vector<std::byte> got(range.length);
        auto read = client.read_into(blob, range.offset, got);
        const bool correct =
            read.has_value() &&
            std::equal(got.begin(), got.end(), object.begin() + static_cast<long>(range.offset));
        check(correct, std::string(name) + ": rango [" + std::to_string(range.offset) + ", +" +
                           std::to_string(range.length) + ")" +
                           (read ? "" : " -> " + read.error().message));
    }

    // Many ranges of one object in a single submit, which is the shape the
    // planner coalesces and scatters.
    {
        std::vector<std::vector<std::byte>> buffers(32, std::vector<std::byte>(256));
        std::vector<karu::Read> reads;
        reads.reserve(buffers.size());
        for (std::size_t index = 0; index < buffers.size(); ++index) {
            reads.push_back(karu::Read{
                &blob, static_cast<std::uint64_t>(index) * 1500, buffers[index], nullptr, {}});
        }
        auto fetched = client.fetch(reads);
        bool correct = fetched.has_value();
        for (std::size_t index = 0; correct && index < buffers.size(); ++index) {
            correct = std::equal(buffers[index].begin(), buffers[index].end(),
                                 object.begin() + static_cast<long>(index * 1500));
        }
        check(correct, std::string(name) + ": 32 rangos coalescidos" +
                           (fetched ? "" : " -> " + fetched.error().message));
    }

    // The size probe uses a different code path than a range read and has to
    // agree with it.
    {
        auto size = client.size(blob);
        check(size.has_value() && *size == kObjectSize,
              std::string(name) + ": size() == " + std::to_string(kObjectSize) +
                  (size ? " (dio " + std::to_string(*size) + ")" : " -> " + size.error().message));
    }

    // Past the end must be reported, never silently truncated.
    {
        std::vector<std::byte> got(16);
        auto read = client.read_into(blob, kObjectSize + 4096, got);
        check(!read.has_value() && read.error().status == KARU_ERR_RANGE,
              std::string(name) + ": lectura pasado el final");
    }

    // A missing object must map to not-found rather than to an empty read.
    {
        auto absent = karu::Object::parse(std::string(blob.uri()) + ".absent");
        if (absent) {
            std::vector<std::byte> got(16);
            auto read = client.read_into(*absent, 0, got);
            check(!read.has_value() && read.error().status == KARU_ERR_NOT_FOUND,
                  std::string(name) + ": objeto inexistente");
        }
    }
}

void run_s3(const std::vector<std::byte>& object) {
    const std::string host = environment("KARU_TEST_S3_ENDPOINT", "127.0.0.1:9000");
    if (!listening("http://" + host + "/karu/fixture.bin")) {
        std::printf("  S3 (MinIO): no está escuchando, omitido\n");
        return;
    }
    auto config = karu::Config::empty().value();
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, std::string>>{
             {"AWS_S3_ENDPOINT", host},
             {"AWS_HTTPS", "NO"},
             {"AWS_VIRTUAL_HOSTING", "NO"},
             {"AWS_REGION", "us-east-1"},
             {"AWS_ACCESS_KEY_ID", environment("KARU_TEST_S3_KEY_ID", "karuemulator")},
             {"AWS_SECRET_ACCESS_KEY", environment("KARU_TEST_S3_SECRET", "karuemulator-secret")},
             {"KARU_MAX_ATTEMPTS", "2"}}) {
        if (!config.set(name, value))
            check(false, std::string("S3: no se pudo fijar ") + name);
    }
    auto client = karu::Client::create(config).value();
    auto blob = karu::Object::parse("s3://karu/fixture.bin").value();
    exercise("S3 (MinIO)", client, blob, object);

    // A wrong secret has to be rejected by the server. Our own fixture cannot
    // prove this, because it never verifies a signature.
    auto wrong = karu::Config::empty().value();
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, std::string>>{
             {"AWS_S3_ENDPOINT", host},
             {"AWS_HTTPS", "NO"},
             {"AWS_VIRTUAL_HOSTING", "NO"},
             {"AWS_REGION", "us-east-1"},
             {"AWS_ACCESS_KEY_ID", "karuemulator"},
             {"AWS_SECRET_ACCESS_KEY", "definitely-not-the-secret"},
             {"KARU_MAX_ATTEMPTS", "1"}}) {
        static_cast<void>(wrong.set(name, value));
    }
    auto wrong_client = karu::Client::create(wrong).value();
    std::vector<std::byte> got(16);
    auto read = wrong_client.read_into(blob, 0, got);
    check(!read.has_value() && read.error().status == KARU_ERR_AUTH,
          "S3 (MinIO): firma incorrecta rechazada por el servidor");
}

void run_azure(const std::vector<std::byte>& object) {
    const std::string host =
        environment("KARU_TEST_AZURE_ENDPOINT", "http://127.0.0.1:10000/devstoreaccount1");
    const std::string key = environment(
        "KARU_TEST_AZURE_KEY", "Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/"
                               "K1SZFPTOtr/KBHBeksoGMGw==");
    if (!listening(host + "/karu/fixture.bin")) {
        std::printf("  Azure (Azurite): no está escuchando, omitido\n");
        return;
    }
    auto config = karu::Config::empty().value();
    static_cast<void>(config.set("AZURE_STORAGE_ACCOUNT", "devstoreaccount1"));
    static_cast<void>(config.set("AZURE_STORAGE_ACCESS_KEY", key));
    static_cast<void>(config.set("AZURE_STORAGE_ENDPOINT", host));
    static_cast<void>(config.set("KARU_MAX_ATTEMPTS", "2"));
    auto client = karu::Client::create(config).value();
    auto blob = karu::Object::parse("az://karu/fixture.bin").value();
    exercise("Azure (Azurite)", client, blob, object);

    // An ETag precondition the blob cannot satisfy.
    {
        std::vector<std::byte> got(16);
        auto read = client.read_into(blob, 0, got, "\"not-the-etag\"");
        check(!read.has_value() && read.error().status == KARU_ERR_PRECONDITION,
              "Azure (Azurite): If-Match incorrecto");
    }

    // A wrong shared key must be refused by Azurite, not accepted locally.
    {
        auto wrong = karu::Config::empty().value();
        static_cast<void>(wrong.set("AZURE_STORAGE_ACCOUNT", "devstoreaccount1"));
        static_cast<void>(
            wrong.set("AZURE_STORAGE_ACCESS_KEY", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=="));
        static_cast<void>(wrong.set("AZURE_STORAGE_ENDPOINT", host));
        static_cast<void>(wrong.set("KARU_MAX_ATTEMPTS", "1"));
        auto wrong_client = karu::Client::create(wrong).value();
        std::vector<std::byte> got(16);
        auto read = wrong_client.read_into(blob, 0, got);
        check(!read.has_value() && read.error().status == KARU_ERR_AUTH,
              "Azure (Azurite): firma incorrecta rechazada por el servidor");
    }
}

void run_gcs(const std::vector<std::byte>& object) {
    const std::string host = environment("KARU_TEST_GCS_ENDPOINT", "http://127.0.0.1:4443");
    if (!listening(host + "/karu/fixture.bin")) {
        std::printf("  GCS (fake-gcs-server): no está escuchando, omitido\n");
        return;
    }
    auto config = karu::Config::empty().value();
    static_cast<void>(config.set("GCS_ENDPOINT", host));
    static_cast<void>(config.set("GCS_NO_SIGN_REQUEST", "YES"));
    static_cast<void>(config.set("KARU_MAX_ATTEMPTS", "2"));
    auto client = karu::Client::create(config).value();
    auto blob = karu::Object::parse("gs://karu/fixture.bin").value();
    exercise("GCS (fake-gcs-server)", client, blob, object);
}

} // namespace

int main() {
    std::printf("karu %s contra emuladores (%s)\n", karu_version_string(), karu_http_backend());
    const std::vector<std::byte> object = fixture_object();
    run_s3(object);
    run_azure(object);
    run_gcs(object);

    const bool require_all = environment("KARU_TEST_REQUIRE_ALL_EMULATORS", "0") == "1";
    if (require_all && backends_run != 3) {
        std::printf("se requieren los 3 emuladores, pero solo respondieron %d\n", backends_run);
        return 1;
    }
    if (backends_run == 0) {
        std::printf("ningún emulador disponible; arranca tests/emulators/up.sh\n");
        return kSkipExitCode;
    }
    std::printf("%d backends, %d comprobaciones, %d fallos\n", backends_run, checks, failures);
    return failures == 0 ? 0 : 1;
}
