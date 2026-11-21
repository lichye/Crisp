#pragma once

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <fcntl.h>
#include <ostream>
#include <set>
#include <streambuf>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace veval::util {
namespace atomic_output_detail {

class FileBuffer : public std::streambuf {
    int fd;
    char buffer[8192];
    int error_ = 0;
public:
    explicit FileBuffer(int descriptor) : fd(descriptor) {
        setp(buffer, buffer + sizeof(buffer));
    }
    int error() const { return error_; }
protected:
    int sync() override {
        auto remaining = pptr() - pbase();
        const char* next = pbase();
        while (remaining > 0) {
            const auto written = ::write(fd, next, remaining);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                if (!error_) error_ = written < 0 ? errno : EIO;
                return -1;
            }
            remaining -= written;
            next += written;
        }
        setp(buffer, buffer + sizeof(buffer));
        return 0;
    }
    int_type overflow(int_type ch) override {
        if (sync() < 0) return traits_type::eof();
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            *pptr() = traits_type::to_char_type(ch);
            pbump(1);
        }
        return traits_type::not_eof(ch);
    }
};

struct TemporaryFile {
    std::string path;
    int fd = -1;
    bool created = false;
    ~TemporaryFile() {
        if (fd >= 0) ::close(fd);
        if (created) ::unlink(path.c_str());
    }
};

} // namespace atomic_output_detail

// Capture once at single-threaded program startup, before workers are created.
// Atomic publication preserves this ordinary 0666-and-umask creation policy;
// temporary files remain private until their final permissions are installed.
inline mode_t atomicOutputCreationMode() {
    static const mode_t mode = [] {
        const mode_t mask = ::umask(0);
        ::umask(mask);
        return static_cast<mode_t>(0666 & ~mask);
    }();
    return mode;
}

class AtomicOutputError : public std::system_error {
    bool published_;
public:
    AtomicOutputError(int code, const std::string& operation,
                      const std::string& path, bool published)
        : std::system_error(code, std::generic_category(), operation + ": " + path +
              (published ? " (published but directory durability unconfirmed)" : "")),
          published_(published) {}
    bool published() const noexcept { return published_; }
};

enum class AtomicOutputSync { PerFile, Deferred };

// Publish one complete file, then make its directory entry durable. Final
// targets must be absent or regular files. Symlinks and special files are
// rejected rather than silently replacing them; parent symlinks
// are allowed. This is neither a multi-file transaction nor protection against
// concurrent path replacement. Interruption can leave an unused temporary.
// Directory fsync failures remain fatal (published=true), including EINVAL/
// ENOTSUP: this helper does not claim durability on unsupported filesystems.
// Replacement preserves only ordinary permission bits, not ownership, ACLs,
// extended attributes or setuid/setgid/sticky bits; the new inode belongs to
// the writing process under the destination directory's usual group policy.
inline void writeAtomic(const std::string& path,
                        const std::function<void(std::ostream&)>& writer,
                        AtomicOutputSync durability = AtomicOutputSync::PerFile) {
    using namespace atomic_output_detail;
    const mode_t creationMode = atomicOutputCreationMode();
    bool published = false;
    auto fail = [&](const char* operation, int code) {
        throw AtomicOutputError(code, operation, path, published);
    };
    auto rejectNonregular = [&] {
        struct stat status{};
        if (::lstat(path.c_str(), &status) == 0) {
            if (S_ISLNK(status.st_mode)) fail("reject symlink output", ELOOP);
            if (!S_ISREG(status.st_mode)) fail("reject nonregular output", EINVAL);
        } else if (errno != ENOENT) fail("inspect output", errno);
    };
    rejectNonregular();
    const auto slash = path.find_last_of('/');
    const std::string parent = slash == std::string::npos ? "." :
        (slash == 0 ? "/" : path.substr(0, slash));
    struct Directory {
        int fd;
        ~Directory() { if (fd >= 0) ::close(fd); }
    } directory{::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (directory.fd < 0) fail("open output directory", errno);
    // Keep the temporary in the destination directory for atomic rename, but
    // do not extend a valid target basename beyond the filesystem's NAME_MAX.
    TemporaryFile temporary{parent + "/.veval.tmp.XXXXXX"};
    temporary.fd = ::mkostemp(temporary.path.data(), O_CLOEXEC);
    if (temporary.fd < 0) fail("create temporary output", errno);
    temporary.created = true;
    struct stat previous{};
    if (::lstat(path.c_str(), &previous) == 0) {
        if (S_ISLNK(previous.st_mode)) fail("reject symlink output", ELOOP);
        if (!S_ISREG(previous.st_mode)) fail("reject nonregular output", EINVAL);
        if (::fchmod(temporary.fd, previous.st_mode & 0777) < 0)
            fail("set output permissions", errno);
    } else {
        if (errno != ENOENT) fail("inspect output", errno);
        if (::fchmod(temporary.fd, creationMode) < 0)
            fail("set new output permissions", errno);
    }
    FileBuffer buffer(temporary.fd);
    std::ostream output(&buffer);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    try {
        writer(output);
        output.flush();
    } catch (const std::ios_base::failure&) {
        fail("write output", buffer.error() ? buffer.error() : EIO);
    }
    if (!output) fail("write output", buffer.error() ? buffer.error() : EIO);
    int synced;
    if (durability == AtomicOutputSync::PerFile
#ifdef __APPLE__
        || durability == AtomicOutputSync::Deferred
#endif
    ) {
        do { synced = ::fsync(temporary.fd); } while (synced < 0 && errno == EINTR);
        if (synced < 0) fail("flush output", errno);
    }
    const int descriptor = temporary.fd;
    temporary.fd = -1;
    if (::close(descriptor) < 0) fail("close output", errno);
    rejectNonregular();
    if (::rename(temporary.path.c_str(), path.c_str()) < 0) fail("publish output", errno);
    temporary.created = false;
    published = true;
    if (durability == AtomicOutputSync::PerFile) {
        do { synced = ::fsync(directory.fd); } while (synced < 0 && errno == EINTR);
        if (synced < 0) fail("flush output directory", errno);
    }
    const int directoryDescriptor = directory.fd;
    directory.fd = -1;
    if (::close(directoryDescriptor) < 0) fail("close output directory", errno);
}

// Finish publications and stale-file removals, even when no files were written.
// PerFile has flushed the file contents and only needs directory fsync. A directory
// fsync alone would not flush file contents. Linux syncfs covers data and names
// once per filesystem. On macOS, Deferred writes fsync each file before rename;
// every output directory is fsynced here to complete publication.
// This is not a multi-file transaction: failure or interruption before this
// call leaves complete, atomically published files with unconfirmed durability.
inline void syncAtomicOutputDirectories(const std::vector<std::string>& paths,
                                        AtomicOutputSync durability = AtomicOutputSync::Deferred) {
#ifndef __APPLE__
    std::set<dev_t> syncedDevices;
#endif
    for (const auto& path : paths) {
        struct Directory {
            int fd;
            ~Directory() { if (fd >= 0) ::close(fd); }
        } directory{::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
        auto fail = [&](const char* operation) {
            throw AtomicOutputError(errno, operation, path, true);
        };
        if (directory.fd < 0) fail("open batch output directory");
        struct stat status{};
        if (::fstat(directory.fd, &status) < 0) fail("inspect batch output filesystem");
#ifdef __APPLE__
        {
#else
        if (durability == AtomicOutputSync::PerFile || syncedDevices.insert(status.st_dev).second) {
#endif
            int result;
#ifdef __APPLE__
            const auto flush = ::fsync;
#else
            const auto flush = durability == AtomicOutputSync::PerFile ? ::fsync : ::syncfs;
#endif
            do { result = flush(directory.fd); } while (result < 0 && errno == EINTR);
            if (result < 0)
                fail(durability == AtomicOutputSync::PerFile ? "flush output directory" : "flush batch output filesystem");
        }
        const int descriptor = directory.fd;
        directory.fd = -1;
        if (::close(descriptor) < 0) fail("close batch output directory");
    }
}

} // namespace veval::util
