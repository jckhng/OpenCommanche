#include "comanchecd/gameplay.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

comanchecd::MissionFilename name(const std::string& value) {
    comanchecd::MissionFilename result;
    result.fill(' ');
    std::copy(value.begin(), value.end(), result.begin());
    return result;
}
}

int main() {
    try {
        comanchecd::MissionDefinition definition;
        const std::string text = "\r\nA1\r\n1025,258,-3\r\n257,258\r\n1,2,3,4,5,6\r\n12000\r\n"
            "0,1,2,3,4,5,6,257,123456789\r\nF,OVER.RLE,1,2,255\r\nF,OTHER.RLE,3,4,17\r\n"
            "W10,20,3,77\r\nR10,20,300,301,302\r\nS30,40,4,5,6\r\n"
            "*3,258,257,256,1025,258,259,260,261,262,123\r\n<\r\n";
        definition.setup_text.assign(text.begin(), text.end());
        comanchecd::MissionResources resources{};
        resources[2] = name("c1w.dta");
        const auto setup = comanchecd::parse_gameplay(definition, resources, {10, 20, 30, 40, 50, 60}, {});
        require(setup.alternate_start == 1 && setup.preset_reference == 20, "Alternate start and preset lookup");
        require(setup.x == (1u << 22) && setup.heading == (3u << 24), "Positions wrap to u32 and number search skips minus");
        require(setup.map_position[0] == 1 && setup.map_position[1] == 2, "Map coordinates truncate to bytes");
        require(setup.weapons[0] == 255 && setup.weapons[7] == 0 && setup.ammunition == 12345678,
                "Weapon decrement is byte-sized and numeric tokens consume at most eight bytes");
        require(setup.overlay_fields[79] == 255 && setup.overlay_fields[80] == 17 && setup.overlay_fields[81] == 0,
                "Optional overlay bytes must retain the original preceding-field alias");
        require(setup.waypoints[0].altitude == (3u << 24) && setup.waypoints[0].speed == 77,
                "Waypoint altitude scale and optional speed");
        require(setup.lights.size() == 2 && setup.secondary_color[0] == 44 && setup.light_color[2] == 6,
                "R/S records update separate byte color triples");
        require(setup.objects[0].category == 2 && setup.objects[0].angle == 3 && setup.target_count == 1 && setup.category_count == 1,
                "Object truncation and target/category counters");

        const std::vector<std::uint8_t> raw{'K', 'R', 'L', '0', 3, 0, 0, 0, 0xc0, 0xff, 1};
        require(comanchecd::decode_payload(raw, comanchecd::PayloadKind::resource).bytes ==
                std::vector<std::uint8_t>({0xc0, 0xff, 1}), "KRL0 copies raw bytes");
        require(comanchecd::decode_payload(raw, comanchecd::PayloadKind::resource).allocation_suffix ==
                std::vector<std::uint8_t>({0}), "Fresh resource allocations have deterministic alignment tails");
        const std::vector<std::uint8_t> packed{'K', 'R', 'L', '1', 3, 0, 0, 0, 0xc0, 0xff, 0xc3, 0xaa};
        require(comanchecd::decode_payload(packed, comanchecd::PayloadKind::resource).bytes ==
                std::vector<std::uint8_t>(3, 0xaa), "KRL1 expands runs and consumes empty runs");
        auto truncated = packed;
        truncated.pop_back();
        bool rejected = false;
        try {
            comanchecd::decode_payload(truncated, comanchecd::PayloadKind::resource);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "Missing run values must reject");

        for (auto& entry : resources) {
            entry = name("-");
        }
        resources[0] = name("P.DTA");
        resources[1] = name("D.DTA");
        resources[2] = name("C.DTA");
        resources[3] = name("SKY.RLE");
        resources[4] = name("BOOM.RLE");
        resources[5] = name("SPEECH.RLE");
        resources[6] = name("MUSIC.RLE");
        resources[11] = name("MODEL.RLE");
        resources[13] = name("SKIP.RLE");
        std::vector<std::string> order;
        const auto payloads = comanchecd::load_gameplay({}, resources, [&](const auto& filename, auto kind) {
            order.push_back(filename);
            comanchecd::GameplayPayload payload;
            payload.kind = kind;
            return payload;
        });
        require(order[0] == "SND1.RLE" && order[4] == "MODEL.RLE" && order.back() == "CONSOL1S.DTA",
                "Gameplay loader order must run from common sounds through cockpit picture");
        require(std::find(order.begin(), order.end(), "SKIP.RLE") == order.end(),
                "The first optional sentinel skips the remaining optional models");
        require(payloads.size() == order.size(), "Every load must retain its decoded payload");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
