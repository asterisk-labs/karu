#include "platform.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace karu::os {

int pid() {
#ifdef _WIN32
    return ::_getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

std::time_t timegm(std::tm& tm) {
#ifdef _WIN32
    return ::_mkgmtime(&tm);
#else
    return ::timegm(&tm);
#endif
}

std::filesystem::path path_from_utf8(std::string_view utf8) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

bool hard_links_unavailable(int error) noexcept {
#ifdef _WIN32
    static_cast<void>(error);
    return false;
#else
    return error == ENOTSUP || error == EOPNOTSUPP || error == ENOSYS || error == EPERM;
#endif
}

File::~File() {
    if (fd_ < 0)
        return;
#ifdef _WIN32
    ::_close(fd_);
#else
    ::close(fd_);
#endif
}

std::error_code File::open(std::string_view utf8_path) {
    if (fd_ >= 0)
        return std::make_error_code(std::errc::device_or_resource_busy);
    const std::filesystem::path path = path_from_utf8(utf8_path);
#ifdef _WIN32
    fd_ = ::_wopen(path.c_str(), _O_RDONLY | _O_BINARY);
#else
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
#endif
    if (fd_ < 0)
        return {errno, std::generic_category()};
    return {};
}

std::expected<std::size_t, std::error_code> File::read_at(void* buf, std::size_t n,
                                                          std::uint64_t offset) {
    if (fd_ < 0)
        return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
#ifdef _WIN32
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<long long>::max())) {
        return std::unexpected(std::make_error_code(std::errc::value_too_large));
    }
    if (::_lseeki64(fd_, static_cast<long long>(offset), SEEK_SET) < 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    const auto want = static_cast<unsigned int>(std::min<std::size_t>(n, 1u << 30));
    const int got = ::_read(fd_, buf, want);
    if (got < 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    return static_cast<std::size_t>(got);
#else
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return std::unexpected(std::make_error_code(std::errc::value_too_large));
    }
    n = std::min(n, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    for (;;) {
        const ssize_t got = ::pread(fd_, buf, n, static_cast<off_t>(offset));
        if (got >= 0)
            return static_cast<std::size_t>(got);
        if (errno == EINTR)
            continue;
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
#endif
}

OutputFile::~OutputFile() {
    static_cast<void>(close());
    if (!temporary_.empty()) {
        std::error_code ignored;
        std::filesystem::remove(temporary_, ignored);
    }
}

std::error_code OutputFile::create_near(std::string_view utf8_destination, bool overwrite) {
    if (fd_ >= 0 || !temporary_.empty())
        return std::make_error_code(std::errc::device_or_resource_busy);
    destination_ = path_from_utf8(utf8_destination);
    if (destination_.empty())
        return std::make_error_code(std::errc::invalid_argument);

    if (!overwrite) {
        std::error_code exists_error;
        const std::filesystem::file_status destination_status =
            std::filesystem::symlink_status(destination_, exists_error);
        if (exists_error && exists_error != std::errc::no_such_file_or_directory)
            return exists_error;
        if (!exists_error && std::filesystem::exists(destination_status))
            return std::make_error_code(std::errc::file_exists);
    }

    static std::atomic<std::uint64_t> next{0};
    for (int attempt = 0; attempt < 100; ++attempt) {
        temporary_ = destination_;
        temporary_ += ".karu.part." + std::to_string(pid()) + "." +
                      std::to_string(next.fetch_add(1, std::memory_order_relaxed));
#ifdef _WIN32
        errno_t error =
            ::_wsopen_s(&fd_, temporary_.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                        _SH_DENYNO, _S_IREAD | _S_IWRITE);
        if (error == 0)
            return {};
        fd_ = -1;
        if (error != EEXIST) {
            temporary_.clear();
            return {static_cast<int>(error), std::generic_category()};
        }
#else
        fd_ = ::open(temporary_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd_ >= 0)
            return {};
        if (errno != EEXIST) {
            const std::error_code error(errno, std::generic_category());
            temporary_.clear();
            return error;
        }
#endif
    }
    temporary_.clear();
    return std::make_error_code(std::errc::file_exists);
}

std::error_code OutputFile::resize(std::uint64_t size) {
    if (fd_ < 0)
        return std::make_error_code(std::errc::bad_file_descriptor);
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::make_error_code(std::errc::value_too_large);
#ifdef _WIN32
    const errno_t result = ::_chsize_s(fd_, static_cast<std::int64_t>(size));
    if (result != 0)
        return {static_cast<int>(result), std::generic_category()};
#else
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0)
        return {errno, std::generic_category()};
#endif
    return {};
}

std::error_code OutputFile::write_at(const void* buf, std::size_t n, std::uint64_t offset) {
    if (fd_ < 0)
        return std::make_error_code(std::errc::bad_file_descriptor);
    const auto* bytes = static_cast<const unsigned char*>(buf);
    std::size_t written = 0;
    while (written < n) {
        if (offset > std::numeric_limits<std::uint64_t>::max() - written)
            return std::make_error_code(std::errc::value_too_large);
        const std::uint64_t position = offset + written;
#ifdef _WIN32
        if (position > static_cast<std::uint64_t>(std::numeric_limits<long long>::max()))
            return std::make_error_code(std::errc::value_too_large);
        if (::_lseeki64(fd_, static_cast<long long>(position), SEEK_SET) < 0)
            return {errno, std::generic_category()};
        const auto want = static_cast<unsigned int>(
            std::min<std::size_t>(n - written, static_cast<std::size_t>(1u << 30)));
        const int count = ::_write(fd_, bytes + written, want);
        if (count < 0)
            return {errno, std::generic_category()};
#else
        if (position > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
            return std::make_error_code(std::errc::value_too_large);
        const std::size_t want =
            std::min(n - written, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t count = ::pwrite(fd_, bytes + written, want, static_cast<off_t>(position));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return {errno, std::generic_category()};
        }
#endif
        if (count == 0)
            return std::make_error_code(std::errc::io_error);
        written += static_cast<std::size_t>(count);
    }
    return {};
}

std::error_code OutputFile::close() {
    if (fd_ < 0)
        return {};
#ifdef _WIN32
    const int result = ::_close(fd_);
#else
    const int result = ::close(fd_);
#endif
    fd_ = -1;
    return result == 0 ? std::error_code{} : std::error_code(errno, std::generic_category());
}

std::error_code OutputFile::publish(bool overwrite) {
    if (const std::error_code error = close())
        return error;
    if (temporary_.empty() || destination_.empty())
        return std::make_error_code(std::errc::bad_file_descriptor);
#ifdef _WIN32
    const DWORD flags = overwrite ? MOVEFILE_REPLACE_EXISTING : 0;
    if (!::MoveFileExW(temporary_.c_str(), destination_.c_str(), flags))
        return {static_cast<int>(::GetLastError()), std::system_category()};
#else
    if (overwrite) {
        if (::rename(temporary_.c_str(), destination_.c_str()) != 0)
            return {errno, std::generic_category()};
    } else {
        if (::link(temporary_.c_str(), destination_.c_str()) == 0) {
            // Both names refer to the completed file. Failure to remove the private
            // name must not turn a successfully published destination into failure.
            static_cast<void>(::unlink(temporary_.c_str()));
        } else {
            const int link_errno = errno;
            const std::error_code link_error(link_errno, std::generic_category());
            if (!hard_links_unavailable(link_errno))
                return link_error;

            // Some mounted and removable filesystems cannot create hard links or
            // perform an exclusive rename. Preserve the common no-replace case,
            // then use the filesystem's atomic rename. Another process may create
            // destination between these calls; this is the unavoidable fallback
            // race on filesystems without a no-replace primitive.
            std::error_code exists_error;
            const std::filesystem::file_status destination_status =
                std::filesystem::symlink_status(destination_, exists_error);
            if (exists_error && exists_error != std::errc::no_such_file_or_directory)
                return exists_error;
            if (!exists_error && std::filesystem::exists(destination_status))
                return std::make_error_code(std::errc::file_exists);
            if (::rename(temporary_.c_str(), destination_.c_str()) != 0)
                return {errno, std::generic_category()};
        }
    }
#endif
    temporary_.clear();
    destination_.clear();
    return {};
}

} // namespace karu::os
