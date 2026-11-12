// ngen-packer-copy: packs an asset as its source's bytes, unchanged.
//
// For assets the engine still reads in their source format (USD layers, PNG textures) until a packer for their engine-ready
// format exists. On Linux the copy is a clone where the filesystem supports one (btrfs, XFS: FICLONE), so the packed file
// shares the source's blocks until either is written; elsewhere the bytes are copied. On Windows it is CopyFileW. The
// depfile lists only the source.

#include "packer.h"
#include "profile.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>

#if !defined(_WIN32)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

#if defined(_WIN32)
// Windows: the filesystem's own copy (CopyFileW).
auto copyFile(const std::string& source, const std::string& out) -> bool {
    std::error_code ec;
    std::filesystem::copy_file(source, out, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        std::println(stderr, "ngen-packer-copy: cannot copy {} to {}: {}", source, out, ec.message());
        return false;
    }
    return true;
}
#else

// Copies with read and write, for filesystems that can't clone.
auto copyBytes(int from, int to) -> bool {
    char buffer[1 << 16];
    while (true) {
        auto count = read(from, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return false;
        }
        if (count == 0) {
            return true;
        }
        auto* data = buffer;
        while (count > 0) {
            auto written = write(to, data, (size_t) count);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                return false;
            }
            data += written;
            count -= written;
        }
    }
}

// A clone where the filesystem has one, else the bytes.
auto copyFile(const std::string& source, const std::string& out) -> bool {
    int from = open(source.c_str(), O_RDONLY | O_CLOEXEC);
    if (from < 0) {
        std::println(stderr, "ngen-packer-copy: cannot open {}: {}", source, std::strerror(errno));
        return false;
    }
    int to = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (to < 0) {
        std::println(stderr, "ngen-packer-copy: cannot write {}: {}", out, std::strerror(errno));
        close(from);
        return false;
    }
    bool copied = ioctl(to, FICLONE, from) == 0 || copyBytes(from, to);
    close(from);
    if (close(to) != 0 || !copied) {
        std::println(stderr, "ngen-packer-copy: cannot copy {} to {}: {}", source, out, std::strerror(errno));
        return false;
    }
    return true;
}
#endif

} // namespace

auto main(int argc, char** argv) -> int {
    // A packer runs for milliseconds: Tracy only when NGEN_TRACY=1 asks (its start-up alone takes 200 ms).
    profile::startTracyIfAsked();
    profile::registerThread("Main");
    PROFILE_ZONE("PackerMain", Assets);
    auto args = parsePackerArgs(argc, argv);
    if (!args) {
        std::println(stderr, "ngen-packer-copy: {}", args.error());
        return 2;
    }
    if (!copyFile(args->source, args->out)) {
        return 1;
    }
    std::ofstream depfile(args->depfile, std::ios::trunc);
    depfile << depfileEscape(args->out) << ": " << depfileEscape(args->source) << '\n';
    if (!depfile) {
        std::println(stderr, "ngen-packer-copy: cannot write {}", args->depfile);
        return 1;
    }
    return 0;
}
