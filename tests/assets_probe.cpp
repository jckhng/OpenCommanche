#include "comanchecd/assets.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
void write(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw std::runtime_error("Cannot write asset output");
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            throw std::invalid_argument("Usage: assets_probe INSTALLATION OUTPUT");
        }
        const comanchecd::Installation installation(argv[1]);
        const auto assets = installation.load_frontend();
        const std::filesystem::path output(argv[2]);
        std::filesystem::create_directories(output);
        write(output / "font.bin", assets.font);
        write(output / "pilot-header.bin", assets.pilot.menu_header);
        write(output / "prompt.bin", assets.pilot.editor_prompt);
        write(output / "record.bin", assets.pilot_record);
        write(output / "main.bin", assets.main_menu);
        write(output / "mission-template.bin", assets.mission_menu);
        write(output / "briefing-choices.bin", assets.briefing_choices);
        write(output / "loading-prompt.bin", assets.loading_prompt);
        write(output / "campaign.bin", assets.campaign_menu);
        for (const auto scene : {comanchecd::FrontendScene::operations, comanchecd::FrontendScene::campaign}) {
            const auto picture = installation.load_picture(scene);
            const auto directory = output / (scene == comanchecd::FrontendScene::operations ? "operations" : "campaign");
            std::filesystem::create_directories(directory);
            std::vector<std::uint8_t> palette;
            for (const auto& color : picture.palette) {
                palette.insert(palette.end(), color.begin(), color.end());
            }
            write(directory / "palette.bin", palette);
            const auto theme = comanchecd::build_theme(picture.palette, assets.transfer);
            write(directory / "theme.bin", {theme.begin(), theme.end()});
            const auto remap = comanchecd::build_box_remap(picture.palette, assets.transfer);
            write(directory / "remap.bin", {remap.begin(), remap.end()});
            for (std::size_t plane = 0; plane < picture.background.size(); ++plane) {
                write(directory / ("background-" + std::to_string(plane) + ".bin"), picture.background[plane]);
            }
        }
        std::cout << "Loaded " << unsigned(assets.campaign_count) << " campaigns\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
