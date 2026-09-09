#include <karu/karu.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>

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
    std::filesystem::remove(path, ignored);
    if (!bytes || bytes->size() != 3)
        return 5;
    return (*bytes)[0] == std::byte{30} && (*bytes)[1] == std::byte{40} &&
                   (*bytes)[2] == std::byte{50}
               ? 0
               : 6;
}
