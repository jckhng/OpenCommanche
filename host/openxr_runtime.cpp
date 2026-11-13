#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr_platform.h>
#include "openxr_runtime.hpp"
#include "sdl_common.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace comanchecd::host {
namespace {
const char* loader_name() {
#ifdef _WIN32
    return "openxr_loader.dll";
#elif defined(__linux__)
    return "libopenxr_loader.so.1";
#else
    throw std::runtime_error("OpenXR loader selection currently supports Windows and Linux");
#endif
}

void check(XrResult result, const char* operation) {
    if (XR_FAILED(result)) { throw std::runtime_error(std::string(operation) + " failed with OpenXR result " + std::to_string(result)); }
}
}

void SdlOpenXrLoader::Unload::operator()(void* object) const { SDL_UnloadObject(object); }
SdlOpenXrLoader::SdlOpenXrLoader(const std::string& path) : object_(SDL_LoadObject(path.empty() ? loader_name() : path.c_str())) {
    if (!object_) { throw sdl_error("OpenXR loader could not be loaded"); }
    entry_ = reinterpret_cast<PFN_xrGetInstanceProcAddr>(SDL_LoadFunction(object_.get(), "xrGetInstanceProcAddr"));
    if (!entry_) { throw sdl_error("OpenXR loader lacks xrGetInstanceProcAddr"); }
}
PFN_xrGetInstanceProcAddr SdlOpenXrLoader::entry() const { return entry_; }

class OpenXrRuntime::Impl {
public:
    explicit Impl(PFN_xrGetInstanceProcAddr loader) : loader_(loader) {}
    ~Impl() {
        if (instance_ && destroy_) { destroy_(instance_); }
    }
    void initialize() {
        if (!loader_) { throw std::invalid_argument("OpenXR requires a loader entry point"); }
        const auto enumerate = procedure<PFN_xrEnumerateInstanceExtensionProperties>("xrEnumerateInstanceExtensionProperties");
        std::uint32_t count = 0;
        check(enumerate(nullptr, 0, &count, nullptr), "xrEnumerateInstanceExtensionProperties");
        constexpr unsigned maximum_extensions = 4096;
        if (count > maximum_extensions) { throw std::runtime_error("OpenXR extension count exceeds supported bounds"); }
        std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
        const auto capacity = count;
        check(enumerate(nullptr, capacity, &count, extensions.data()), "xrEnumerateInstanceExtensionProperties");
        if (count > capacity) { throw std::runtime_error("OpenXR extension enumeration changed beyond capacity"); }
        extensions.resize(count);
        const char* extension = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
        if (std::none_of(extensions.begin(), extensions.end(), [&](const auto& item) {
            return std::strncmp(item.extensionName, extension, XR_MAX_EXTENSION_NAME_SIZE) == 0;
        })) { throw std::runtime_error("OpenXR runtime lacks XR_KHR_opengl_enable"); }
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strcpy(info.applicationInfo.applicationName, "Comanche SDL2");
        info.applicationInfo.applicationVersion = 1;
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        info.enabledExtensionCount = 1;
        info.enabledExtensionNames = &extension;
        check(procedure<PFN_xrCreateInstance>("xrCreateInstance")(&info, &instance_), "xrCreateInstance");
        destroy_ = procedure<PFN_xrDestroyInstance>("xrDestroyInstance");
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        check(procedure<PFN_xrGetInstanceProperties>("xrGetInstanceProperties")(instance_, &properties), "xrGetInstanceProperties");
        capabilities_.runtime_name.assign(properties.runtimeName,
            std::find(properties.runtimeName, properties.runtimeName + XR_MAX_RUNTIME_NAME_SIZE, '\0'));
        capabilities_.runtime_version = properties.runtimeVersion;
        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
        system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system = XR_NULL_SYSTEM_ID;
        check(procedure<PFN_xrGetSystem>("xrGetSystem")(instance_, &system_info, &system), "xrGetSystem");
        XrGraphicsRequirementsOpenGLKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
        check(procedure<PFN_xrGetOpenGLGraphicsRequirementsKHR>("xrGetOpenGLGraphicsRequirementsKHR")(
            instance_, system, &requirements), "xrGetOpenGLGraphicsRequirementsKHR");
        if (requirements.minApiVersionSupported == 0 || requirements.minApiVersionSupported > requirements.maxApiVersionSupported) {
            throw std::runtime_error("OpenXR returned an invalid OpenGL version range");
        }
        capabilities_.minimum_opengl = requirements.minApiVersionSupported;
        capabilities_.maximum_opengl = requirements.maxApiVersionSupported;
    }
    const OpenXrCapabilities& capabilities() const { return capabilities_; }
private:
    template<class Procedure>
    Procedure procedure(const char* name) const {
        PFN_xrVoidFunction function = nullptr;
        check(loader_(instance_, name, &function), name);
        if (!function) { throw std::runtime_error(std::string("Missing OpenXR function ") + name); }
        return reinterpret_cast<Procedure>(function);
    }
    PFN_xrGetInstanceProcAddr loader_;
    XrInstance instance_ = XR_NULL_HANDLE;
    PFN_xrDestroyInstance destroy_ = nullptr;
    OpenXrCapabilities capabilities_;
};

OpenXrRuntime::OpenXrRuntime(PFN_xrGetInstanceProcAddr loader) : impl_(std::make_unique<Impl>(loader)) { impl_->initialize(); }
OpenXrRuntime::~OpenXrRuntime() = default;
const OpenXrCapabilities& OpenXrRuntime::capabilities() const { return impl_->capabilities(); }
}
