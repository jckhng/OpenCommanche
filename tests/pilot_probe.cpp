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
constexpr std::size_t pilot_storage = 0x2cd6e;
constexpr std::array<std::size_t, 4> backgrounds{0x40a20, 0x4589a, 0x4a714, 0x4f58e};

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

Bytes slice(const Bytes& data, std::size_t offset, std::size_t count) {
    if (offset + count > data.size()) {
        throw std::runtime_error("Truncated private RAM input");
    }
    return Bytes(data.begin() + offset, data.begin() + offset + count);
}

std::uint32_t u32(const Bytes& data, std::size_t offset) {
    const auto value = slice(data, offset, 4);
    return value[0] | (std::uint32_t(value[1]) << 8) | (std::uint32_t(value[2]) << 16) |
           (std::uint32_t(value[3]) << 24);
}

void sort_cases(const std::filesystem::path& input, const std::filesystem::path& output) {
    unsigned count = 0;
    std::ifstream(input / "count.txt") >> count;
    if (count == 0 || count > 256) {
        throw std::runtime_error("Invalid sorting case count");
    }
    for (unsigned index = 0; index < count; ++index) {
        const auto name = "case-" + std::to_string(index);
        unsigned selection = 0;
        std::ifstream(input / name / "selection.txt") >> selection;
        if (selection >= comanchecd::pilot_count) {
            throw std::runtime_error("Invalid sorting selection");
        }
        comanchecd::PilotStorage storage(read(input / name / "record.bin"));
        std::string operation = "normalize";
        std::ifstream(input / name / "operation.txt") >> operation;
        std::uint8_t result = 0;
        if (operation == "normalize") {
            result = storage.normalize(static_cast<std::uint8_t>(selection));
        } else if (operation == "commit") {
            comanchecd::EditorBuffer editor{};
            comanchecd::PilotAssets assets{};
            const auto content = read(input / name / "editor.bin");
            const auto prefix = read(input / "row-prefix.bin");
            const auto tail = read(input / "row-tail.bin");
            if (content.size() != editor.size() || prefix.size() != assets.row_prefix.size() ||
                    tail.size() != assets.row_tail.size()) {
                throw std::runtime_error("Malformed replacement fixture");
            }
            std::copy(content.begin(), content.end(), editor.begin());
            std::copy(prefix.begin(), prefix.end(), assets.row_prefix.begin());
            std::copy(tail.begin(), tail.end(), assets.row_tail.begin());
            result = storage.commit(static_cast<std::uint8_t>(selection), editor, assets);
        } else {
            throw std::runtime_error("Unknown storage operation");
        }
        std::filesystem::create_directories(output / name);
        write(output / name / "record.bin", storage.record());
        std::ofstream(output / name / "selection.txt") << unsigned(result) << '\n';
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 4 && (std::string(argv[1]) == "--sort" || std::string(argv[1]) == "--storage")) {
            sort_cases(argv[2], argv[3]);
            return 0;
        }
        if (argc != 3) {
            throw std::runtime_error("Usage: pilot_probe INPUT OUTPUT");
        }
        const std::filesystem::path input(argv[1]), output(argv[2]);
        std::filesystem::create_directories(output);
        const auto ram = read(input / "input-ram.bin");
        if (ram.size() < 0x60d00) {
            throw std::runtime_error("Truncated private input RAM");
        }
        comanchecd::PilotAssets assets{};
        assets.menu_header = slice(ram, 0x2cd5c, pilot_storage - 0x2cd5c);
        assets.editor_prompt = slice(ram, 0x2b411, 54);
        std::copy_n(ram.begin() + 0x2b3fc, assets.row_prefix.size(), assets.row_prefix.begin());
        std::copy_n(ram.begin() + 0x2b405, assets.row_tail.size(), assets.row_tail.begin());
        std::copy_n(ram.begin() + 0x2a485, assets.glyph_widths.size(), assets.glyph_widths.begin());
        std::copy_n(ram.begin() + 0x19fd, assets.normal_keys.size(), assets.normal_keys.begin());
        std::copy_n(ram.begin() + 0x197d, assets.shifted_keys.size(), assets.shifted_keys.begin());
        comanchecd::Palette palette{};
        for (std::size_t index = 0; index < palette.size(); ++index) {
            std::copy_n(ram.begin() + 0x2dc90 + index * 3, 3, palette[index].begin());
        }
        comanchecd::Transfer transfer{};
        comanchecd::Theme theme{};
        std::copy_n(ram.begin() + 0x2f160, transfer.size(), transfer.begin());
        std::copy_n(ram.begin() + 0x2dc76, theme.size(), theme.begin());
        comanchecd::Background background;
        for (std::size_t plane = 0; plane < background.size(); ++plane) {
            background[plane] = slice(ram, backgrounds[plane], comanchecd::background_bytes);
        }
        comanchecd::MenuRenderer renderer(comanchecd::PlanarSurface(read(input / "input-planes.bin")),
            comanchecd::Font(slice(ram, 0x2a47d, 0x2b1e5 - 0x2a47d)), theme,
            comanchecd::build_box_remap(palette, transfer), background, u32(ram, 0x1a944), u32(ram, 0x1aa60));
        comanchecd::PilotController controller(comanchecd::PilotStorage(slice(ram, pilot_storage,
            comanchecd::pilot_record_bytes)), std::move(assets), ram[0x2b1e5]);
        std::size_t checkpoint = 0, writes = 0;
        comanchecd::PilotFrontend frontend(std::move(controller), std::move(renderer),
            [&](const Bytes& record) {
                write(output / ("save-" + std::to_string(writes++) + ".nam"), record);
            },
            [&](const comanchecd::PilotController& state, const comanchecd::MenuRenderer& drawing) {
                const auto directory = output / ("step-" + std::to_string(checkpoint++));
                std::filesystem::create_directories(directory);
                write(directory / "planes.bin", drawing.surface().bytes());
                write(directory / "record.bin", state.storage().record());
                write(directory / "editor.bin", Bytes(state.editor_buffer().begin(), state.editor_buffer().end()));
                std::ofstream(directory / "state.txt") << static_cast<unsigned>(state.mode()) << ' '
                    << unsigned(state.selection()) << ' ' << unsigned(state.editor_length()) << ' '
                    << drawing.back_page() << '\n';
            });
        frontend.start();
        std::ifstream actions(input / "actions.txt");
        unsigned scan = 0, left = 0, right = 0;
        while (actions >> scan >> left >> right) {
            if (scan >= 128 || left > 1 || right > 1) {
                throw std::runtime_error("Invalid scan event");
            }
            const auto shift = static_cast<comanchecd::PilotShift>(left | (right << 1));
            frontend.consume({static_cast<std::uint8_t>(scan), shift});
        }
        if (!actions.eof()) {
            throw std::runtime_error("Malformed scan events");
        }
        std::ofstream(output / "writes.txt") << writes << '\n';
        std::cout << "Pilot checkpoints " << checkpoint << ", saves " << writes << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
