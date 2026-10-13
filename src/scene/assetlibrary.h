#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

// Entries the view keeps across rebuilds, by key (an asset id, or a key of the view's own for generated entries such as a constant colour's
// 1x1 texture). A handle names one entry for the whole session and is never reused, so a handle seen before always means the same content.
// Content is immutable: an asset that changes gets a new entry, under the same key, and a new handle. Descriptions are shared with the
// renderer, which keeps them while it uploads.
template <typename Handle, typename Desc>
class AssetLibrary {
public:
    // The entry for a key; a null handle when there is none.
    auto find(const std::string& key) const -> Handle {
        auto it = byKey.find(key);
        if (it == byKey.end()) {
            return {};
        }
        return {.index = it->second};
    }

    // A new entry under the key, replacing the key's earlier one (which stays until removed).
    auto add(const std::string& key, std::shared_ptr<const Desc> desc) -> Handle {
        auto index = nextIndex++;
        entries[index] = {.key = key, .desc = std::move(desc)};
        byKey[key] = index;
        return {.index = index};
    }

    auto remove(Handle handle) -> void {
        auto it = entries.find(handle.index);
        if (it == entries.end()) {
            return;
        }
        if (auto keyed = byKey.find(it->second.key); keyed != byKey.end() && keyed->second == handle.index) {
            byKey.erase(keyed);
        }
        entries.erase(it);
    }

    // Forgets the key, so the next add under it makes a new entry; the old entry stays until removed.
    auto forget(const std::string& key) -> void { byKey.erase(key); }

    auto get(Handle handle) const -> const Desc* {
        auto it = entries.find(handle.index);
        if (it == entries.end()) {
            return nullptr;
        }
        return it->second.desc.get();
    }

    auto shared(Handle handle) const -> std::shared_ptr<const Desc> {
        auto it = entries.find(handle.index);
        if (it == entries.end()) {
            return nullptr;
        }
        return it->second.desc;
    }

    auto key(Handle handle) const -> std::string {
        auto it = entries.find(handle.index);
        if (it == entries.end()) {
            return {};
        }
        return it->second.key;
    }

    auto count() const -> uint32_t { return (uint32_t) entries.size(); }

    // Calls f(handle, desc) for every entry, in no particular order.
    template <typename F>
    auto each(const F& f) const -> void {
        for (const auto& [index, entry] : entries) {
            f(Handle{.index = index}, *entry.desc);
        }
    }

private:
    struct Entry {
        std::string key;
        std::shared_ptr<const Desc> desc;
    };
    std::unordered_map<uint32_t, Entry> entries;
    std::unordered_map<std::string, uint32_t> byKey;
    uint32_t nextIndex = 1;
};
