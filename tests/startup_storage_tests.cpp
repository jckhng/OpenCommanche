#include "../src/startup_storage.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
constexpr std::size_t allocation_rows = 250, row_bytes = 256, margin_bytes = 32;
constexpr std::size_t scene_bytes = 1024 * 1024, resource_header_bytes = 8;
void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

std::vector<std::uint8_t> resource(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> result{'K', 'R', 'L', '0'};
    for (unsigned byte = 0; byte < 4; ++byte) {
        result.push_back(static_cast<std::uint8_t>(size >> (byte * 8)));
    }
    for (std::size_t index = 0; index < size; ++index) {
        result.push_back(static_cast<std::uint8_t>((index + seed) % 251));
    }
    return result;
}

std::vector<std::uint8_t> reference(const std::vector<std::vector<std::uint8_t>>& resources, unsigned phase) {
    std::vector<std::uint8_t> arena(4 * 1024 * 1024);
    std::size_t cursor = phase;
    for (const auto& bytes : resources) {
        while (cursor % 4 != 0) { ++cursor; }
        std::copy(bytes.begin() + resource_header_bytes, bytes.end(), arena.begin() + cursor);
        cursor += bytes.size() - resource_header_bytes;
    }
    cursor = phase + 2 * scene_bytes;
    while (cursor % row_bytes != 0) { ++cursor; }
    cursor += allocation_rows * row_bytes;
    std::vector<std::uint8_t> result;
    for (unsigned row = 0; row < allocation_rows; ++row) {
        result.insert(result.end(), arena.begin() + cursor, arena.begin() + cursor + margin_bytes);
        result.insert(result.end(), arena.begin() + cursor + row_bytes - margin_bytes, arena.begin() + cursor + row_bytes);
        cursor += row_bytes;
    }
    return result;
}
}

int main() {
    try {
        using namespace comanchecd;
        const auto fanfare = resource(3, 17);
        StartupMovieAssets movie;
        movie.score.encoded = resource(5, 23);
        movie.sound.encoded = resource(9, 31);
        movie.movie.encoded = resource(2 * scene_bytes + 2 * allocation_rows * row_bytes, 43);
        for (const auto phase : {0, 1, 3, 63, 127, 192, 254, 255}) {
            const auto padding = startup_depth_padding(fanfare, movie, static_cast<std::uint8_t>(phase));
            require(padding == reference({fanfare, movie.score.encoded, movie.sound.encoded, movie.movie.encoded}, phase),
                    "Depth margins must inherit resource writes across the aligned arena rewind");
            TerrainTables tables;
            for (const auto levels : {100u, 180u, 250u}) {
                tables.levels = levels;
                apply_depth_padding(tables, padding);
                require(tables.depth_padding.size() == levels * margin_bytes * 2 &&
                        std::equal(tables.depth_padding.begin(), tables.depth_padding.end(), padding.begin()),
                        "Detail changes must retain the prefix of the same depth allocation margins");
            }
        }
        movie.movie.encoded = resource(1, 0);
        const auto empty = startup_depth_padding(fanfare, movie);
        require(std::all_of(empty.begin(), empty.end(), [](auto value) { return value == 0; }),
                "Resource writes below the depth allocation must leave its initial contents intact");
        TerrainTables invalid;
        invalid.levels = 180;
        bool rejected = false;
        try { apply_depth_padding(invalid, {}); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Incomplete retained allocation margins must be rejected");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
