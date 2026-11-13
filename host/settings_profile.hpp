#pragma once

#include "comanchecd/options.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace comanchecd::host {
class SettingsProfile {
public:
    explicit SettingsProfile(std::filesystem::path directory) : directory_(std::move(directory)) {}
    std::vector<std::uint8_t> read() const {
        const auto path = directory_ / "COMANCHE.MNU";
        if (!std::filesystem::exists(path)) {
            return {};
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Cannot read native menu settings");
        }
        return MenuSettings({std::istreambuf_iterator<char>(input), {}}).record();
    }
    void write(const std::vector<std::uint8_t>& record) const {
        const MenuSettings checked(record);
        std::filesystem::create_directories(directory_);
        const auto path = directory_ / "COMANCHE.MNU";
        if (std::filesystem::exists(path)) {
            write_record(directory_ / "COMANCHE.MNU.bak", read());
        }
        write_record(path, checked.record());
    }
private:
    static void write_record(const std::filesystem::path& path, const std::vector<std::uint8_t>& record) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(record.data()), static_cast<std::streamsize>(record.size()));
        output.close();
        if (!output) {
            throw std::runtime_error("Cannot save native menu settings");
        }
    }
    std::filesystem::path directory_;
};
} // namespace comanchecd::host
