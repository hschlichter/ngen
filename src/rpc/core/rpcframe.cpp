#include "rpcframe.h"

#include <cstring>
#include <format>

namespace {

auto putU32(std::vector<std::byte>& out, uint32_t value) -> void {
    for (int i = 0; i < 4; i++) {
        out.push_back((std::byte) ((value >> (8 * i)) & 0xFFu));
    }
}

auto getU32(const std::byte* p) -> uint32_t {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        value |= (uint32_t) p[i] << (8 * i);
    }
    return value;
}

} // namespace

auto encodeRpcFrame(std::string_view json, std::span<const std::byte> attachment) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(rpcFrameHeaderSize + json.size() + attachment.size());
    putU32(out, (uint32_t) json.size());
    putU32(out, (uint32_t) attachment.size());
    const auto* jsonBytes = reinterpret_cast<const std::byte*>(json.data());
    out.insert(out.end(), jsonBytes, jsonBytes + json.size());
    out.insert(out.end(), attachment.begin(), attachment.end());
    return out;
}

auto RpcFrameReader::append(std::span<const std::byte> bytes) -> void {
    // Drop consumed bytes before growing, so a long-lived connection doesn't keep them.
    if (readOffset > 0 && readOffset == buffer.size()) {
        buffer.clear();
        readOffset = 0;
    } else if (readOffset > 64 * 1024) {
        buffer.erase(buffer.begin(), buffer.begin() + (std::ptrdiff_t) readOffset);
        readOffset = 0;
    }
    buffer.insert(buffer.end(), bytes.begin(), bytes.end());
}

auto RpcFrameReader::next(std::string& error) -> std::optional<RpcFrame> {
    if (failed) {
        return std::nullopt;
    }
    auto available = buffer.size() - readOffset;
    if (available < rpcFrameHeaderSize) {
        return std::nullopt;
    }
    const auto* header = buffer.data() + readOffset;
    auto jsonLength = getU32(header);
    auto attachmentLength = getU32(header + 4);
    auto total = (uint64_t) rpcFrameHeaderSize + jsonLength + attachmentLength;
    if (total > rpcMaxFrameSize) {
        failed = true;
        error = std::format("frame of {} bytes exceeds the {} byte limit", total, rpcMaxFrameSize);
        return std::nullopt;
    }
    if (available < total) {
        return std::nullopt;
    }
    RpcFrame frame;
    const auto* body = header + rpcFrameHeaderSize;
    frame.json.assign(reinterpret_cast<const char*>(body), jsonLength);
    frame.attachment.assign(body + jsonLength, body + jsonLength + attachmentLength);
    readOffset += (size_t) total;
    return frame;
}
