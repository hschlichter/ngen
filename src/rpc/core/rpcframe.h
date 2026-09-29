#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// One message on the wire: a JSON text and an optional binary attachment.
//
//   u32 jsonLength, u32 attachmentLength (little-endian), JSON bytes, attachment bytes
//
// A message refers to its attachment by offset and length inside the attachment bytes.
struct RpcFrame {
    std::string json;
    std::vector<std::byte> attachment;
};

inline constexpr uint32_t rpcFrameHeaderSize = 8;
// Larger frames are a protocol error; the connection is closed.
inline constexpr uint64_t rpcMaxFrameSize = 256ull * 1024 * 1024;

auto encodeRpcFrame(std::string_view json, std::span<const std::byte> attachment) -> std::vector<std::byte>;

// Accumulates bytes from a stream and yields complete frames in order.
class RpcFrameReader {
public:
    auto append(std::span<const std::byte> bytes) -> void;
    // The next complete frame, if one has arrived. Sets `error` and returns nullopt on a frame
    // larger than rpcMaxFrameSize; the reader is unusable after that.
    auto next(std::string& error) -> std::optional<RpcFrame>;

private:
    std::vector<std::byte> buffer;
    size_t readOffset = 0;
    bool failed = false;
};
