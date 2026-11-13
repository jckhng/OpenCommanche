#include "comanchecd/menu_renderer.hpp"
#include "comanchecd/pilot.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t font_begin = 0x2a47d;
constexpr std::size_t font_end = 0x2b1e5;
constexpr std::size_t palette_begin = 0x2dc90;
constexpr std::size_t transfer_begin = 0x2f160;
constexpr std::size_t theme_begin = 0x2dc76;
constexpr std::size_t descriptor_pointer = 0x2b1ec;
constexpr std::size_t back_page_address = 0x1a944;
constexpr std::size_t pan_address = 0x1aa60;
constexpr std::array<std::size_t, 4> background_addresses{0x40a20, 0x4589a, 0x4a714, 0x4f58e};

Bytes read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot read " + path.string());
    }
    return Bytes(std::istreambuf_iterator<char>(file), {});
}

void write(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw std::runtime_error("Cannot write " + path.string());
    }
}

Bytes slice(const Bytes& bytes, std::size_t offset, std::size_t size) {
    if (offset + size > bytes.size()) {
        throw std::runtime_error("Truncated private input RAM");
    }
    return Bytes(bytes.begin() + offset, bytes.begin() + offset + size);
}

std::uint32_t u32(const Bytes& bytes, std::size_t offset) {
    const auto data = slice(bytes, offset, 4);
    return data[0] | (std::uint32_t(data[1]) << 8) | (std::uint32_t(data[2]) << 16) |
           (std::uint32_t(data[3]) << 24);
}

std::string stage_name(comanchecd::DrawStage stage, std::size_t row) {
    switch (stage) {
    case comanchecd::DrawStage::background: return "background";
    case comanchecd::DrawStage::box: return "box";
    case comanchecd::DrawStage::highlight: return "highlight";
    case comanchecd::DrawStage::text_row: return "row-" + std::to_string(row);
    case comanchecd::DrawStage::complete: return "complete";
    }
    throw std::runtime_error("Unknown draw stage");
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            throw std::runtime_error("Usage: render_probe INPUT OUTPUT");
        }
        const std::filesystem::path input(argv[1]), output(argv[2]);
        std::filesystem::create_directories(output);
        const auto ram = read(input / "input-ram.bin");
        if (ram.size() < 0x60d00) {
            throw std::runtime_error("Truncated private input RAM");
        }
        comanchecd::Palette palette{};
        comanchecd::Transfer transfer{};
        comanchecd::Theme theme{};
        for (std::size_t index = 0; index < palette.size(); ++index) {
            std::copy_n(ram.begin() + palette_begin + index * 3, 3, palette[index].begin());
        }
        std::copy_n(ram.begin() + transfer_begin, transfer.size(), transfer.begin());
        std::copy_n(ram.begin() + theme_begin, theme.size(), theme.begin());
        const auto remap = comanchecd::build_box_remap(palette, transfer);
        write(output / "remap.bin", Bytes(remap.begin(), remap.end()));
        comanchecd::Background background;
        for (std::size_t plane = 0; plane < background.size(); ++plane) {
            background[plane] = slice(ram, background_addresses[plane], comanchecd::background_bytes);
        }
        comanchecd::MenuRenderer renderer(comanchecd::PlanarSurface(read(input / "input-planes.bin")),
            comanchecd::Font(slice(ram, font_begin, font_end - font_begin)), theme, remap,
            background, u32(ram, back_page_address), u32(ram, pan_address));
        const auto descriptor = u32(ram, descriptor_pointer);
        auto end = descriptor;
        while (end < ram.size() && ram[end] != 255) {
            ++end;
        }
        const auto stream = slice(ram, descriptor, end - descriptor + 1);
        std::ifstream request(input / "draws.txt");
        unsigned selection = 0, layout = 0;
        std::size_t draw = 0;
        while (request >> selection >> layout) {
            if (selection > 255 || layout > 6) {
                throw std::runtime_error("Unsupported menu stimulus");
            }
            const auto directory = output / ("draw-" + std::to_string(draw));
            std::filesystem::create_directories(directory);
            if (layout >= 3) {
                comanchecd::SettingsAssets assets;
                const auto table = [&](std::size_t address) {
                    auto end = address;
                    while (ram.at(end) != 255) { ++end; }
                    return slice(ram, address, end - address + 1);
                };
                assets.header = table(0x29590);
                for (std::size_t category = 0; category < assets.menus.size(); ++category) {
                    assets.menus[category] = table(u32(ram, 0x29570 + category * 4));
                    assets.rows[category] = ram.at(0x295c7 + category);
                    assets.widths[category] = ram.at(0x295cf + category);
                }
                comanchecd::SettingsMenu menu;
                for (unsigned category = 3; category < layout; ++category) {
                    menu.consume(comanchecd::SettingsKey::right);
                }
                for (unsigned row = 0; row < selection; ++row) {
                    menu.consume(comanchecd::SettingsKey::down);
                }
                renderer.draw_settings(assets, comanchecd::MenuSettings(slice(ram, 0x28d24, 43)), menu);
                write(directory / "complete.bin", renderer.surface().bytes());
            } else if (layout == 2) {
                const comanchecd::PilotStorage pilots(slice(ram, 0x2cd6e, comanchecd::pilot_record_bytes));
                const auto statistics = pilots.statistics(slice(ram, 0x2bf57, 130));
                write(directory / "text.bin", statistics);
                renderer.draw_statistics(statistics);
                write(directory / "complete.bin", renderer.surface().bytes());
            } else {
                renderer.draw_list(stream, static_cast<std::uint8_t>(selection),
                layout == 0 ? comanchecd::ListLayout::standard : comanchecd::ListLayout::main,
                [&](comanchecd::DrawStage stage, std::size_t row, const comanchecd::PlanarSurface& surface) {
                    write(directory / (stage_name(stage, row) + ".bin"), surface.bytes());
                });
            }
            std::ofstream state(directory / "state.txt");
            state << selection << ' ' << renderer.back_page() << '\n';
            renderer.present();
            ++draw;
        }
        if (!request.eof() || draw == 0) {
            throw std::runtime_error("Missing or malformed draw stimuli");
        }
        std::cout << "Rendered " << draw << " retained menu draws\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
