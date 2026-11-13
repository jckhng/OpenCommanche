#pragma once

#include <openxr/openxr.h>
#include <memory>
#include <string>

namespace comanchecd::host {
class SdlOpenXrLoader {
public:
    explicit SdlOpenXrLoader(const std::string& path = {});
    PFN_xrGetInstanceProcAddr entry() const;
private:
    struct Unload { void operator()(void* object) const; };
    std::unique_ptr<void, Unload> object_;
    PFN_xrGetInstanceProcAddr entry_ = nullptr;
};

struct OpenXrCapabilities {
    std::string runtime_name;
    XrVersion runtime_version = 0, minimum_opengl = 0, maximum_opengl = 0;
};

// Runtime/graphics admission precedes SDL GL context/session creation. The loader outlives this owner.
class OpenXrRuntime {
public:
    explicit OpenXrRuntime(PFN_xrGetInstanceProcAddr loader);
    ~OpenXrRuntime();
    const OpenXrCapabilities& capabilities() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
