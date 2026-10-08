// The few places where POSIX and Windows differ.
#ifndef KARU_PLATFORM_HPP
#define KARU_PLATFORM_HPP

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <expected>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace karu::os {

int pid();

// UTC broken-down time to Unix time.
std::time_t timegm(std::tm& tm);

// karu's paths are UTF-8 everywhere; Windows wants them wide.
std::filesystem::path path_from_utf8(std::string_view utf8);

// Whether a failed POSIX link means the destination filesystem cannot provide
// the hard-link publication primitive used for no-replace downloads.
bool hard_links_unavailable(int error) noexcept;

// A file open for positional reads. One per transfer, so an implementation
// may move the file pointer.
class File {
  public:
    File() = default;
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    std::error_code open(std::string_view utf8_path);
    // Bytes read; zero at end of file.
    std::expected<std::size_t, std::error_code> read_at(void* buf, std::size_t n,
                                                        std::uint64_t offset);

  private:
    int fd_ = -1;
};

// A temporary file created beside a destination and published only after all
// positional writes succeed.
class OutputFile {
  public:
    OutputFile() = default;
    ~OutputFile();
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;

    std::error_code create_near(std::string_view utf8_destination, bool overwrite);
    std::error_code resize(std::uint64_t size);
    std::error_code write_at(const void* buf, std::size_t n, std::uint64_t offset);
    std::error_code publish(bool overwrite);

  private:
    std::error_code close();

    int fd_ = -1;
    std::filesystem::path temporary_;
    std::filesystem::path destination_;
};

} // namespace karu::os

#endif
