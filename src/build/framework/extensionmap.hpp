// build::ExtensionMap — type-erased payload table on Target / Platform / Configuration.
//
// The framework's core types (`build::Target`, `build::Platform`, `build::Configuration`) carry no language
// vocabulary. Anything language- or kind-specific — `cxx::Target`'s sources list, `Tool`'s argv template,
// `Alias`'s selector rules — attaches here, keyed by the extension's concrete type via
// `std::type_index(typeid(Ext))`. Two attachment modes:
//
//   - `add<Ext>(args...)` — owning. The map heap-allocates and deletes on destruction. Idempotent: a second
//     `add<T>` returns the existing instance rather than replacing.
//   - `attach<Ext>(ext)` — non-owning. Stores a back-pointer with a no-op deleter; replaces any existing entry.
//
// `add` is for extensions whose data has no other home. `attach` is what the fluent wrappers (`cxx::Target`,
// `Tool`, `Alias`) use: each wrapper owns its data and attaches itself as the back-pointer, so the rest of the
// framework can look it up via `target->extension<cxx::Target>()`.
//
// `get<Ext>()` returns `nullptr` when absent. No exceptions; the framework follows the `std::expected` discipline.

#pragma once

#include <memory>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace build {

class ExtensionMap {
public:
    template <typename Ext>
    auto has() const -> bool {
        return entries_.contains(std::type_index(typeid(Ext)));
    }

    template <typename Ext>
    auto get() -> Ext* {
        auto it = entries_.find(std::type_index(typeid(Ext)));
        if (it == entries_.end()) {
            return nullptr;
        }
        return static_cast<Ext*>(it->second.ptr.get());
    }

    template <typename Ext>
    auto get() const -> const Ext* {
        auto it = entries_.find(std::type_index(typeid(Ext)));
        if (it == entries_.end()) {
            return nullptr;
        }
        return static_cast<const Ext*>(it->second.ptr.get());
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
