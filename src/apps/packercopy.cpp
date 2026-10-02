// ngen-packer-copy: packs an asset as its source's bytes, unchanged.
//
// For assets the engine still reads in their source format (USD layers, PNG textures) until a packer for their engine-ready
// format exists. The copy is a clone where the filesystem supports one (btrfs, XFS: FICLONE), so the packed file shares
// the source's blocks until either is written; elsewhere the bytes are copied. The depfile lists only the source.

#include "packer.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <linux/fs.h>
#include <print>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// Make escapes spaces, '#' and '\' in depfile paths.
auto depfileEscape(const std::string& path) -> std::string {
    std::string out;
    for (char ch : path) {
        if (ch == ' ' || ch == '#' || ch == '\\') {
            out += '\\';
        }
        out += ch;
    }
    return out;
}

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

} // namespace

auto main(int argc, char** argv) -> int {
    auto args = parsePackerArgs(argc, argv);
    if (!args) {
        std::println(stderr, "ngen-packer-copy: {}", args.error());
        return 2;
    }
    int from = open(args->source.c_str(), O_RDONLY | O_CLOEXEC);
    if (from < 0) {
        std::println(stderr, "ngen-packer-copy: cannot open {}: {}", args->source, std::strerror(errno));
        return 1;
    }
    int to = open(args->out.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (to < 0) {
        std::println(stderr, "ngen-packer-copy: cannot write {}: {}", args->out, std::strerror(errno));
        close(from);
        return 1;
    }
    bool copied = ioctl(to, FICLONE, from) == 0 || copyBytes(from, to);
    close(from);
    if (close(to) != 0 || !copied) {
        std::println(stderr, "ngen-packer-copy: cannot copy {} to {}: {}", args->source, args->out, std::strerror(errno));
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
