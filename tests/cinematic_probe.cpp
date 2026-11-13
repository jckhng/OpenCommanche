#include "comanchecd/assets.hpp"
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc != 4 || std::filesystem::exists(argv[3])) {
            throw std::invalid_argument("cinematic_probe INSTALLATION PAYLOAD_DIRECTORY FRESH_OUTPUT");
        }
        const comanchecd::Installation installation(argv[1], argv[2]);
        const auto resources = installation.load_demo_cinematics();
        const std::filesystem::path output(argv[3]);
        std::filesystem::create_directories(output);
        const auto write = [&](const std::string& name, const auto& bytes) {
            std::ofstream stream(output / name, std::ios::binary);
            stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (!stream) {
                throw std::runtime_error("Cannot write cinematic fixture");
            }
        };
        write("bank.bin", resources.sound_bank);
        write("cues.bin", resources.cues);
        write("fanfare.bin", resources.fanfare);
        write("canyon.bin", resources.canyon_music);
        write("hangar.bin", resources.hangar_music);
        for (const auto item : {std::make_pair("animation", &resources.animation), std::make_pair("title", &resources.title)}) {
            for (std::size_t frame = 0; frame < item.second->frame_count(); ++frame) {
                write(std::string(item.first) + "-" + std::to_string(frame) + ".bin", item.second->frame(frame));
            }
            std::ofstream palette(output / (std::string(item.first) + "-palette.bin"), std::ios::binary);
            for (const auto& color : item.second->palette()) {
                palette.write(reinterpret_cast<const char*>(color.data()), color.size());
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
