#include "comanchecd/options.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        if (argc != 3 && argc != 4) {
            throw std::invalid_argument("Usage: options_probe INPUT OUTPUT [altitude]");
        }
        std::ifstream input(argv[1]);
        std::ofstream output(argv[2]);
        if (argc == 4) {
            if (std::string(argv[3]) != "altitude") {
                throw std::invalid_argument("Unknown option fixture");
            }
            unsigned enabled, power;
            std::int32_t speed, pitch, roll;
            while (input >> enabled >> power >> speed >> pitch >> roll) {
                if (enabled > 1 || power > 255 || pitch < -133 || pitch > 133 || roll < -33 || roll > 33) {
                    throw std::invalid_argument("Invalid altitude fixture");
                }
                const auto response = comanchecd::altitude_response(enabled ? comanchecd::AltitudeControl::stabilized :
                    comanchecd::AltitudeControl::manual, static_cast<std::uint8_t>(power), speed, pitch, roll);
                output << unsigned(response.power) << ' ' << unsigned(response.descent_shift) << '\n';
            }
            if (!input.eof() || !output) {
                throw std::runtime_error("Invalid altitude policy input/output");
            }
            return 0;
        }
        unsigned chaff, flares, height, nearby, tracking, hit, active_chaff, active_flares;
        while (input >> chaff >> flares >> height >> nearby >> tracking >> hit >> active_chaff >> active_flares) {
            using namespace comanchecd;
            if (chaff > 1 || flares > 1 || nearby > 1 || tracking > 1 || hit > 1 ||
                    height > 255 || active_chaff > 255 || active_flares > 255) {
                throw std::invalid_argument("Invalid defense policy fixture");
            }
            const FlightPolicy policy{chaff != 0 ? DefenseMode::automatic : DefenseMode::manual,
                                      flares != 0 ? DefenseMode::automatic : DefenseMode::manual};
            const DefenseStatus status{static_cast<std::uint8_t>(height),
                static_cast<std::uint8_t>(active_chaff), static_cast<std::uint8_t>(active_flares),
                nearby != 0 ? ThreatProximity::nearby : ThreatProximity::clear, tracking != 0, hit != 0};
            output << unsigned(automatic_defense(policy, status)) << '\n';
        }
        if (!input.eof() || !output) {
            throw std::runtime_error("Invalid defense policy input/output");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
