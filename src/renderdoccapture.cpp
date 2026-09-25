#include "renderdoccapture.h"

#include <print>

#ifdef NGEN_INTROSPECTION

#include <dlfcn.h>
#include <filesystem>
#include <renderdoc_app.h>

namespace {
auto rdoc(void* api) -> RENDERDOC_API_1_6_0* {
    return static_cast<RENDERDOC_API_1_6_0*>(api);
}
} // namespace

auto RenderDocCapture::load(bool forceLoad) -> void {
    // Injected by RenderDoc already? RTLD_NOLOAD only finds a library that is loaded.
    void* library = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
    if (library == nullptr && forceLoad) {
        library = dlopen("librenderdoc.so", RTLD_NOW);
        if (library == nullptr) {
            std::println(stderr, "RenderDoc: cannot load librenderdoc.so: {}", dlerror());
            return;
        }
    }
    if (library == nullptr) {
        return;
    }
    auto getApi = (pRENDERDOC_GetAPI) dlsym(library, "RENDERDOC_GetAPI");
    if (getApi == nullptr) {
        std::println(stderr, "RenderDoc: RENDERDOC_GetAPI not found");
        return;
    }
    if (getApi(eRENDERDOC_API_Version_1_6_0, &api) != 1 || api == nullptr) {
        std::println(stderr, "RenderDoc: API 1.6.0 not available");
        api = nullptr;
        return;
    }
    std::filesystem::create_directories("captures");
    rdoc(api)->SetCaptureFilePathTemplate("captures/ngen");
    // The engine draws its own UI; RenderDoc's overlay would only cover it.
    rdoc(api)->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
    int major = 0;
    int minor = 0;
    int patch = 0;
    rdoc(api)->GetAPIVersion(&major, &minor, &patch);
    std::println("RenderDoc: API {}.{}.{} loaded, captures go to captures/", major, minor, patch);
}

auto RenderDocCapture::triggerCapture() -> bool {
    if (api == nullptr) {
        return false;
    }
    rdoc(api)->TriggerCapture();
    return true;
}

auto RenderDocCapture::pollNewCaptures() -> std::vector<std::string> {
    std::vector<std::string> paths;
    if (api == nullptr) {
        return paths;
    }
    auto count = rdoc(api)->GetNumCaptures();
    for (auto i = seenCaptures; i < count; i++) {
        uint32_t length = 0;
        if (rdoc(api)->GetCapture(i, nullptr, &length, nullptr) == 0 || length == 0) {
            continue;
        }
        std::string path(length, '\0');
        rdoc(api)->GetCapture(i, path.data(), &length, nullptr);
        path.resize(path.find('\0') == std::string::npos ? path.size() : path.find('\0'));
        latest = path;
        paths.push_back(std::move(path));
    }
    seenCaptures = count;
    return paths;
}

auto RenderDocCapture::openInUi(const std::string& path) -> bool {
    if (api == nullptr || path.empty()) {
        return false;
    }
    return rdoc(api)->LaunchReplayUI(1, path.c_str()) != 0;
}

#else

auto RenderDocCapture::load(bool /*forceLoad*/) -> void {
}
auto RenderDocCapture::triggerCapture() -> bool {
    return false;
}
auto RenderDocCapture::pollNewCaptures() -> std::vector<std::string> {
    return {};
}
auto RenderDocCapture::openInUi(const std::string& /*path*/) -> bool {
    return false;
}

#endif
