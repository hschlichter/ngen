// The USD asset resolver that reads through the asset server.
//
// This file compiles as C++23 (build.cpp overrides sceneusd's C++20 for it) so it can use AssetClient. That works because it
// includes only usd/ar, usd/sdf and base headers: never usd/usd/* or a schema header, which pull in usd/usd/schemaRegistry.h
// and fail to compile as C++23.

#include "usdassetresolver.h"

#include "assetclient.h"
#include "assetid.h"

#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/pxr.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/defineResolver.h>
#include <pxr/usd/ar/filesystemAsset.h>
#include <pxr/usd/ar/filesystemWritableAsset.h>
#include <pxr/usd/ar/inMemoryAsset.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/timestamp.h>
#include <pxr/usd/ar/writableAsset.h>

#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <print>
#include <string>
#include <unordered_map>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

AssetClient* assetClient = nullptr;
// The resource folders of USD's registered plugins: USD's own files (schema definitions) are part of the runtime, not assets,
// and are read from disk.
std::vector<std::string> runtimeRoots;

auto isRuntimePath(const std::string& path) -> bool {
    for (const auto& root : runtimeRoots) {
        if (path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/') {
            return true;
        }
    }
    return false;
}

// The asset's bytes, or empty when it couldn't be fetched (the reason is logged).
using Bytes = std::shared_ptr<const std::vector<std::byte>>;

std::mutex fetchMutex;
std::unordered_map<std::string, std::shared_future<Bytes>> fetching; // ids being fetched, shared by concurrent opens
std::unordered_map<std::string, uint64_t> versions;                  // the last version fetched per id

auto parseVersion(const std::string& text) -> uint64_t {
    try {
        return std::stoull(text, nullptr, 16);
    } catch (...) {
        return 0;
    }
}

// Fetches an asset from the asset server. Concurrent opens of the same id share one request.
auto fetch(const std::string& id) -> Bytes {
    std::promise<Bytes> promise;
    std::shared_future<Bytes> future;
    bool owner = false;
    {
        std::lock_guard lock(fetchMutex);
        auto it = fetching.find(id);
        if (it != fetching.end()) {
            future = it->second;
        } else {
            future = promise.get_future().share();
            fetching.emplace(id, future);
            owner = true;
        }
    }
    if (!owner) {
        return future.get();
    }
    std::vector<std::string> ids = {id};
    assetClient->request(ids);
    assetClient->wait(ids);
    Bytes bytes;
    if (auto asset = assetClient->take(id)) {
        std::lock_guard lock(fetchMutex);
        versions[id] = parseVersion(asset->version);
        bytes = std::make_shared<const std::vector<std::byte>>(std::move(asset->bytes));
    } else {
        std::println(stderr, "NgenAssetResolver: cannot open {}:", id);
        for (const auto& line : assetClient->errors(id)) {
            std::println(stderr, "  {}", line);
        }
    }
    promise.set_value(bytes);
    std::lock_guard lock(fetchMutex);
    fetching.erase(id);
    return bytes;
}

} // namespace

// Identifiers and resolved paths are asset ids: paths relative to the asset server's directory. A relative path is anchored to
// the directory of the asset that refers to it; an absolute path, or one that climbs above the server's directory, doesn't
// resolve.
// Resolving asks the server nothing, so a missing asset fails when it is opened. The exception is USD's own runtime files under
// a registered plugin's resources, which keep their absolute paths and are read from disk.
class NgenAssetResolver : public ArResolver {
protected:
    auto _CreateIdentifier(const std::string& assetPath, const ArResolvedPath& anchorAssetPath) const -> std::string override {
        return idFor(assetPath, anchorAssetPath);
    }

    auto _CreateIdentifierForNewAsset(const std::string& assetPath, const ArResolvedPath& anchorAssetPath) const -> std::string override {
        return idFor(assetPath, anchorAssetPath);
    }

    auto _Resolve(const std::string& assetPath) const -> ArResolvedPath override {
        return ArResolvedPath(idFor(assetPath, ArResolvedPath()));
    }

    auto _ResolveForNewAsset(const std::string& assetPath) const -> ArResolvedPath override {
        return ArResolvedPath(idFor(assetPath, ArResolvedPath()));
    }

    auto _OpenAsset(const ArResolvedPath& resolvedPath) const -> std::shared_ptr<ArAsset> override {
        if (isRuntimePath(resolvedPath.GetPathString())) {
            return ArFilesystemAsset::Open(resolvedPath);
        }
        auto bytes = fetch(resolvedPath.GetPathString());
        if (!bytes) {
            return nullptr;
        }
        // The buffer keeps the bytes alive for as long as USD holds it.
        std::shared_ptr<const char> buffer(reinterpret_cast<const char*>(bytes->data()), [bytes](const char*) {});
        return ArInMemoryAsset::FromBuffer(buffer, bytes->size());
    }

    // Saves go to the file at the id, relative to the working directory: the editor still lives in the view, which runs in the
    // asset server's directory to find it. The next request for the asset repacks it.
    auto _OpenAssetForWrite(const ArResolvedPath& resolvedPath, WriteMode writeMode) const -> std::shared_ptr<ArWritableAsset> override {
        auto path = std::filesystem::current_path() / resolvedPath.GetPathString();
        return ArFilesystemWritableAsset::Create(ArResolvedPath(path.string()), writeMode);
    }

    // The version last fetched, so a layer's Reload sees a change once the asset has been fetched again.
    auto _GetModificationTimestamp(const std::string&, const ArResolvedPath& resolvedPath) const -> ArTimestamp override {
        if (isRuntimePath(resolvedPath.GetPathString())) {
            return ArFilesystemAsset::GetModificationTimestamp(resolvedPath);
        }
        std::lock_guard lock(fetchMutex);
        auto it = versions.find(resolvedPath.GetPathString());
        if (it == versions.end()) {
            return ArTimestamp();
        }
        return ArTimestamp((double) it->second);
    }

private:
    static auto idFor(const std::string& assetPath, const ArResolvedPath& anchor) -> std::string {
        if (assetPath.empty()) {
            return {};
        }
        // Asset ids use forward slashes. Windows-authored assets (NewSponza) write @textures\brick.png@; a '\' in a USD asset path
        // is a separator, not part of a file name.
        std::filesystem::path path(TfStringReplace(assetPath, "\\", "/"));
        if (path.is_absolute() && isRuntimePath(path.lexically_normal().string())) {
            return path.lexically_normal().string();
        }
        if (!anchor.empty() && isRuntimePath(anchor.GetPathString())) {
            return (std::filesystem::path(anchor.GetPathString()).parent_path() / path).lexically_normal().string();
        }
        auto base = anchor.empty() ? std::filesystem::path() : std::filesystem::path(anchor.GetPathString()).parent_path();
        return assetIdForPath(path, base);
    }
};

AR_DEFINE_RESOLVER(NgenAssetResolver, ArResolver);

auto registerAssetResolver(AssetClient* client, const std::filesystem::path& binDirectory) -> bool {
    assetClient = client;
    auto plugins = PlugRegistry::GetInstance().RegisterPlugins((binDirectory / "usdplugins").string());
    if (plugins.empty()) {
        std::println(stderr, "NgenAssetResolver: no plugin in {}", (binDirectory / "usdplugins").string());
        return false;
    }
    for (const auto& plugin : PlugRegistry::GetInstance().GetAllPlugins()) {
        auto resources = std::filesystem::path(plugin->GetResourcePath()).lexically_normal().string();
        if (!resources.empty()) {
            runtimeRoots.push_back(resources);
        }
    }
    ArSetPreferredResolver("NgenAssetResolver");
    return true;
}
