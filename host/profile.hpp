#pragma once

#include "comanchecd/pilot.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace comanchecd::host {
class PilotProfile {
public:
    explicit PilotProfile(std::filesystem::path directory) : directory_(std::move(directory)) {}

    std::vector<std::uint8_t> read() const {
        const auto path = directory_ / "COMANCHE.NAM";
        if (!std::filesystem::exists(path)) {
            return {};
        }
        if (std::filesystem::file_size(path) != pilot_record_bytes) {
            throw std::runtime_error("Invalid native pilot profile size");
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Cannot open native pilot profile");
        }
        return {std::istreambuf_iterator<char>(input), {}};
    }

    void write(const std::vector<std::uint8_t>& record) const {
        if (record.size() != pilot_record_bytes) {
            throw std::invalid_argument("Invalid native pilot record");
        }
        std::filesystem::create_directories(directory_);
        const auto path = directory_ / "COMANCHE.NAM";
        // Retain the previous record before replacing a persistent profile.
        if (std::filesystem::exists(path)) {
            write_record(directory_ / "COMANCHE.NAM.bak", read());
        }
        write_record(path, record);
    }

private:
    static void write_record(const std::filesystem::path& path, const std::vector<std::uint8_t>& record) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(record.data()), static_cast<std::streamsize>(record.size()));
        output.close();
        if (!output) {
            throw std::runtime_error("Cannot save native pilot profile");
        }
    }
    std::filesystem::path directory_;
};
} // namespace comanchecd::host
