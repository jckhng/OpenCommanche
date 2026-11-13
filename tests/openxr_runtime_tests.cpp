#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr_platform.h>
#include "openxr_runtime.hpp"

#include <cstring>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
enum class Fault { none, extension, headset, graphics, version, procedure };
struct Runtime {
    Fault fault = Fault::none;
    unsigned created = 0, destroyed = 0;
};
Runtime* runtime = nullptr;
const auto instance = reinterpret_cast<XrInstance>(std::uintptr_t(1));

void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

XrResult XRAPI_CALL extensions(const char*, std::uint32_t capacity, std::uint32_t* count, XrExtensionProperties* output) {
    *count = runtime->fault == Fault::extension ? 0 : 1;
    if (capacity && *count) {
        require(output[0].type == XR_TYPE_EXTENSION_PROPERTIES, "Extension output must be initialized");
        std::strcpy(output[0].extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL create(const XrInstanceCreateInfo* info, XrInstance* output) {
    require(info->type == XR_TYPE_INSTANCE_CREATE_INFO && info->applicationInfo.apiVersion == XR_MAKE_VERSION(1, 0, 0) &&
            info->enabledExtensionCount == 1 && std::strcmp(info->enabledExtensionNames[0], XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0,
            "Runtime admission must request the explicit OpenGL extension");
    ++runtime->created;
    *output = instance;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy(XrInstance owner) {
    require(owner == instance, "Instance destruction must retain its owner");
    ++runtime->destroyed;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL properties(XrInstance owner, XrInstanceProperties* output) {
    require(owner == instance && output->type == XR_TYPE_INSTANCE_PROPERTIES, "Runtime properties need an initialized instance output");
    std::strcpy(output->runtimeName, "Synthetic OpenXR");
    output->runtimeVersion = XR_MAKE_VERSION(1, 1, 0);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL system(XrInstance owner, const XrSystemGetInfo* info, XrSystemId* output) {
    require(owner == instance && info->type == XR_TYPE_SYSTEM_GET_INFO && info->formFactor == XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
            "Runtime admission must request a headset system");
    if (runtime->fault == Fault::headset) { return XR_ERROR_FORM_FACTOR_UNAVAILABLE; }
    *output = 2;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL graphics(XrInstance owner, XrSystemId id, XrGraphicsRequirementsOpenGLKHR* output) {
    require(owner == instance && id == 2 && output->type == XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR,
            "OpenGL requirements must use the admitted headset and initialized output");
    if (runtime->fault == Fault::graphics) { return XR_ERROR_RUNTIME_FAILURE; }
    output->minApiVersionSupported = XR_MAKE_VERSION(3, 3, 0);
    output->maxApiVersionSupported = runtime->fault == Fault::version ? XR_MAKE_VERSION(2, 0, 0) : XR_MAKE_VERSION(4, 6, 0);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL entry(XrInstance owner, const char* name, PFN_xrVoidFunction* output) {
    *output = nullptr;
    if (std::strcmp(name, "xrEnumerateInstanceExtensionProperties") == 0) {
        require(owner == XR_NULL_HANDLE, "Global extension enumeration must precede instance creation");
        *output = reinterpret_cast<PFN_xrVoidFunction>(extensions);
    } else if (std::strcmp(name, "xrCreateInstance") == 0) {
        require(owner == XR_NULL_HANDLE, "Instance creation must use the global entry point");
        *output = reinterpret_cast<PFN_xrVoidFunction>(create);
    } else {
        require(owner == instance, "Instance procedures require the created instance");
        if (std::strcmp(name, "xrDestroyInstance") == 0) { *output = reinterpret_cast<PFN_xrVoidFunction>(destroy); }
        if (std::strcmp(name, "xrGetInstanceProperties") == 0) { *output = reinterpret_cast<PFN_xrVoidFunction>(properties); }
        if (std::strcmp(name, "xrGetSystem") == 0 && runtime->fault != Fault::procedure) { *output = reinterpret_cast<PFN_xrVoidFunction>(system); }
        if (std::strcmp(name, "xrGetOpenGLGraphicsRequirementsKHR") == 0) { *output = reinterpret_cast<PFN_xrVoidFunction>(graphics); }
    }
    return XR_SUCCESS;
}
}

int main() {
    try {
        using namespace comanchecd::host;
        for (const auto fault : {Fault::none, Fault::extension, Fault::headset, Fault::graphics, Fault::version, Fault::procedure}) {
            Runtime test{fault};
            runtime = &test;
            bool rejected = false;
            OpenXrCapabilities capabilities;
            try {
                OpenXrRuntime admitted(entry);
                capabilities = admitted.capabilities();
            } catch (const std::runtime_error&) { rejected = true; }
            require(rejected == (fault != Fault::none), "Admission failure must match its declared runtime fault");
            if (!rejected) {
                require(capabilities.runtime_name == "Synthetic OpenXR" &&
                        capabilities.minimum_opengl == XR_MAKE_VERSION(3, 3, 0) &&
                        capabilities.maximum_opengl == XR_MAKE_VERSION(4, 6, 0),
                        "Runtime admission must retain graphics requirements");
            }
            require(test.created == test.destroyed && test.created == (fault == Fault::extension ? 0u : 1u),
                    "Successful and failed admission must release each created instance exactly once");
        }
        bool rejected = false;
        try { OpenXrRuntime invalid(nullptr); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "A missing loader entry must reject before runtime calls");
        rejected = false;
        try { SdlOpenXrLoader invalid("__comanche_missing_openxr_loader__"); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "A missing loader library must provide a recoverable host error");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
