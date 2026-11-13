#include "comanchecd/assets.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

std::vector<std::uint8_t> picture() {
    std::vector<std::uint8_t> file(128, 0);
    file[8] = 0x47;
    file[9] = 1;
    file[10] = 239;
    std::size_t remaining = 328 * 240;
    while (remaining > 0) {
        const auto count = std::min<std::size_t>(remaining, 63);
        file.push_back(static_cast<std::uint8_t>(0xc0 | count));
        file.push_back(0xc5);
        remaining -= count;
    }
    file.push_back(12);
    file.insert(file.end(), 768, 252);
    return file;
}

void rejects(const std::vector<std::uint8_t>& bytes) {
    try {
        comanchecd::decode_frontend_picture(bytes);
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Malformed picture accepted");
}
}

int main() {
    try {
        const auto file = picture();
        const auto decoded = comanchecd::decode_frontend_picture(file);
        require(decoded.palette[255][2] == 63, "Palette must convert eight-bit channels to six-bit");
        for (const auto& plane : decoded.background) {
            require(plane.size() == comanchecd::background_bytes && plane[19679] == 0xc5 && plane[19680] == 0,
                    "Picture must deinterleave all pixels and retain a zero background tail");
        }
        auto damaged = file;
        damaged.pop_back();
        rejects(damaged);
        damaged = file;
        damaged[128] = 0xc0;
        rejects(damaged);
        damaged = file;
        damaged[10] = 238;
        rejects(damaged);
        damaged.resize(129);
        rejects(damaged);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
