#include "sdl_host.hpp"
#include "profile.hpp"
#include "settings_profile.hpp"
#ifdef COMANCHECD_OPENXR_RUNTIME
#include "openxr_runtime.hpp"
#endif

#include <iostream>

int main(int argc, char** argv) {
    try {
        constexpr const char* usage = "comanche_viewer INSTALLATION PAYLOAD_DIRECTORY PROFILE_DIRECTORY [--mode faithful|enhanced] [--controller-mappings FILE]";
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << usage << "\ncomanche_viewer --openxr-info (OpenXR-enabled build)\n";
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--openxr-info") {
#ifdef COMANCHECD_OPENXR_RUNTIME
            const comanchecd::host::SdlOpenXrLoader loader;
            const comanchecd::host::OpenXrRuntime runtime(loader.entry());
            const auto& capabilities = runtime.capabilities();
            std::cout << capabilities.runtime_name << ' ' << XR_VERSION_MAJOR(capabilities.runtime_version) << '.' <<
                XR_VERSION_MINOR(capabilities.runtime_version) << '.' << XR_VERSION_PATCH(capabilities.runtime_version) <<
                "\nOpenGL " << XR_VERSION_MAJOR(capabilities.minimum_opengl) << '.' <<
                XR_VERSION_MINOR(capabilities.minimum_opengl) << " through " << XR_VERSION_MAJOR(capabilities.maximum_opengl) << '.' <<
                XR_VERSION_MINOR(capabilities.maximum_opengl) << '\n';
            return 0;
#else
            throw std::invalid_argument("Configure COMANCHECD_BUILD_OPENXR=ON with OpenXR SDK headers for runtime admission checks");
#endif
        }
        if (argc < 4 || (argc - 4) % 2 != 0) {
            throw std::invalid_argument(usage);
        }
        comanchecd::host::SdlHostOptions options;
        for (int index = 4; index < argc; index += 2) {
            const std::string option(argv[index]), value(argv[index + 1]);
            if (option == "--mode" && (value == "faithful" || value == "enhanced")) {
                options.input = value == "faithful" ? comanchecd::host::InputProfile::faithful : comanchecd::host::InputProfile::enhanced;
            } else if (option == "--controller-mappings" && !value.empty()) {
                options.controller_mappings = value;
            } else {
                throw std::invalid_argument(usage);
            }
        }
        if (!options.controller_mappings.empty() && options.input != comanchecd::host::InputProfile::enhanced) {
            throw std::invalid_argument("Controller mappings require enhanced mode");
        }
        const auto profile_path = std::filesystem::absolute(argv[3]).lexically_normal();
        for (const auto root : {argv[1], argv[2]}) {
            if (profile_path == std::filesystem::absolute(root).lexically_normal() ||
                    (std::filesystem::exists(profile_path) && std::filesystem::equivalent(profile_path, root))) {
                throw std::invalid_argument("Use a separate native profile directory");
            }
        }
        const comanchecd::host::PilotProfile profile(profile_path);
        const comanchecd::host::SettingsProfile settings(profile_path);
        comanchecd::GameSession session(comanchecd::Installation(argv[1], argv[2]),
            [&](const auto& record) { profile.write(record); }, profile.read(), settings.read(),
            [&](const auto& record) { settings.write(record); }, comanchecd::SessionStart::cold);
        comanchecd::host::SdlFlightHost host(std::move(options));
        std::cout << "Arrows/Enter navigate | Tab/Enter acquires targets | Space fires | E/D throttle | P pauses | Alt-A ends mission | Esc leaves debrief\n";
        comanchecd::host::run_session(session, host);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
