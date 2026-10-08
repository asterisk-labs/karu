#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <karu/karu.hpp>

int main() {
    auto config = karu::Config::empty();
    if (!config)
        return 1;
    auto client = karu::Client::create(*config);
    if (!client)
        return 2;

    const auto path = std::filesystem::temp_directory_path() / "karu-package-consumer.bin";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        const std::array<unsigned char, 6> contents{10, 20, 30, 40, 50, 60};
        output.write(reinterpret_cast<const char*>(contents.data()), contents.size());
        if (!output)
            return 3;
    }

    auto object = karu::Object::parse(path.string());
    if (!object) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return 4;
    }
    auto bytes = client->read(*object, 2, 3);
    std::error_code ignored;
    const auto downloaded_path =
        std::filesystem::temp_directory_path() / "karu-package-consumer-download.bin";
    std::filesystem::remove(downloaded_path, ignored);
    auto downloaded = client->download(*object, downloaded_path.string());
    if (!downloaded || downloaded->size != 6 || std::filesystem::file_size(downloaded_path) != 6) {
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(downloaded_path, ignored);
        return 5;
    }
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(downloaded_path, ignored);
    if (!bytes || bytes->size() != 3)
        return 6;
    const karu::SubmitOptions options{.coalesce_gap = 0};
    const std::span<const karu::Read> no_reads;
    if (!client->fetch(no_reads, options))
        return 7;
    return (*bytes)[0] == std::byte{30} && (*bytes)[1] == std::byte{40} &&
                   (*bytes)[2] == std::byte{50}
               ? 0
               : 8;
}
