#pragma once

#include <memory>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace build {

// Type-erased map of extension instances keyed by their concrete type.
//
// Two attachment modes:
//   - add<Ext>(args...)    : ExtensionMap owns the new Ext (heap-allocated, deleted on map destruction).
//                            Idempotent: calling add<T>() twice returns the existing instance instead of replacing.
//   - attach<Ext>(ext)     : non-owning. The map stores a back-pointer with a no-op deleter.
//                            Replaces any previous entry of the same type.
//
// add() is for extensions that have no other home (e.g. build::cxx::Platform — created on demand on a build::Platform).
// attach() is for extensions whose data lives elsewhere (e.g. build::cxx::Target — the fluent wrapper owns the data
// and registers a back-pointer in build::Target's map).
class ExtensionMap {
public:
    template <typename Ext>
    auto has() const -> bool { return entries_.contains(std::type_index(typeid(Ext))); }

    template <typename Ext>
    auto get() -> Ext& {
        auto it = entries_.find(std::type_index(typeid(Ext)));
        if (it == entries_.end()) {
            throw std::runtime_error("extension not found");
        }
        return *static_cast<Ext*>(it->second.ptr.get());
    }

    template <typename Ext>
    auto get() const -> const Ext& {
        auto it = entries_.find(std::type_index(typeid(Ext)));
        if (it == entries_.end()) {
            throw std::runtime_error("extension not found");
        }
        return *static_cast<const Ext*>(it->second.ptr.get());
    }

    template <typename Ext, typename... Args>
    auto add(Args&&... args) -> Ext& {
        auto key = std::type_index(typeid(Ext));
        if (auto it = entries_.find(key); it != entries_.end()) {
            return *static_cast<Ext*>(it->second.ptr.get());
        }
        auto* raw = new Ext(std::forward<Args>(args)...);
        entries_.emplace(
            key,
            Entry{
                std::unique_ptr<void, void (*)(void*)>{
                    raw,
                    [](void* p) { delete static_cast<Ext*>(p); },
                },
            });
        return *raw;
    }

    template <typename Ext>
    auto attach(Ext& ext) -> void {
        auto key = std::type_index(typeid(Ext));
        entries_.insert_or_assign(
            key,
            Entry{
                std::unique_ptr<void, void (*)(void*)>{
                    &ext,
                    [](void*) {},
                },
            });
    }

private:
    struct Entry {
        std::unique_ptr<void, void (*)(void*)> ptr;
    };

    std::unordered_map<std::type_index, Entry> entries_;
};

} // namespace build
