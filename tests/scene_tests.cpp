#include "comanchecd/scene.hpp"
#include "comanchecd/execution_clock.hpp"
#include "comanchecd/timing.hpp"

#include <iostream>
#include <stdexcept>

namespace {
class SceneClock final : public comanchecd::ExecutionClock {
public:
    SceneClock(comanchecd::IrqState& state, unsigned interrupt_at) : state_(state), interrupt_at_(interrupt_at) {}
    void execute(comanchecd::CpuOperation, unsigned count) override { cycles_ += count; }
    void instruction(const std::function<void()>& action) override {
        ++cycles_;
        action();
        if (++actions_ == interrupt_at_) { state_.interrupt(); }
    }
    void read_file(unsigned) override { throw std::runtime_error("Scene construction must not read files"); }
    bool read_retrace() override { return false; }
    std::uint64_t cycles() const override { return cycles_; }
    std::uint64_t irq_ticks() const override { return 0; }
    std::uint64_t sample_irq_ticks() override { return 0; }
private:
    comanchecd::IrqState& state_;
    unsigned interrupt_at_, actions_ = 0;
    std::uint64_t cycles_ = 0;
};

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

comanchecd::GameplayPayload sprite() {
    comanchecd::GameplayPayload result;
    result.kind = comanchecd::PayloadKind::resource;
    result.bytes.resize(18 + 769);
    result.bytes[0] = 'S';
    result.bytes[1] = 'G';
    result.bytes[4] = 18;
    result.bytes[8] = 2;
    result.bytes[10] = 1;
    result.bytes[12] = 16;
    result.bytes[16] = result.bytes[17] = 1;
    result.bytes[18] = 12;
    result.bytes[22] = 63;
    return result;
}
}

int main() {
    try {
        comanchecd::GameplayPayload picture;
        picture.kind = comanchecd::PayloadKind::picture;
        picture.width = picture.height = 512;
        picture.bytes.resize(512 * 512, 200);
        picture.bytes[0] = 100;
        const auto expanded = comanchecd::expand_terrain(picture);
        require(expanded[0] == 100 && expanded[1] == 22 && expanded[1024] == 22,
                "Terrain interpolation truncates byte addition before halving");
        require(expanded[1023] == 22 && expanded[1023 * 1024] == 22,
                "Terrain interpolation wraps the last column and row");
        comanchecd::SceneAssets assets{};
        assets.terrain.height = expanded;
        comanchecd::GameplaySetup setup;
        comanchecd::GameplayObject player{};
        player.type = 0;
        setup.objects.push_back(player);
        comanchecd::GameplayObject target{};
        target.type = 4;
        target.attribute2 = 1;
        target.category = 3;
        target.attribute8 = 255;
        setup.objects.push_back(target);
        const auto scene = comanchecd::construct_scene(setup, assets, 0x12345678);
        require(scene.entities[61].active && scene.entities[60].active && !scene.entities[59].active,
                "Player uses fixed slot 61 and other entities allocate backward from 60");
        require(scene.entities[60].speed == (255u << 21) && scene.wingman == 60 &&
                scene.entities[60].category_color == 8, "Scene entity field scaling and category lookup");
        require(scene.random_seed == 0x4a7f6fcdu && scene.entities[60].random_phase == 0xcd,
                "Constructor RNG negates, arithmetic shifts and applies sign feedback");
        for (unsigned interrupt_at : {1u, 2u}) {
            comanchecd::IrqState live{0x12345678};
            SceneClock clock(live, interrupt_at);
            const auto clocked = comanchecd::construct_scene(setup, assets, 0, {}, &live, &clock);
            comanchecd::IrqState expected{0x4a7f6fcd};
            if (interrupt_at == 2) { expected.interrupt(); }
            require(clocked.entities[60].random_phase == 0xcd && clocked.random_seed == expected.random_seed &&
                    live.random_seed == expected.random_seed,
                    "Construction must load live RNG and preserve register/IRQ ownership across its RNG store");
        }
        require(scene.player_height == 205 && scene.entities[60].height == 209,
                "Player and entity heights use separate offsets");
        std::vector<comanchecd::GameplayPayload> payloads(17);
        payloads[1].palette.resize(768);
        payloads[1].palette[3] = 63;
        payloads[1].palette[6] = 3;
        payloads[1].palette[7] = 25;
        payloads[1].palette[8] = 7;
        payloads[2].width = payloads[2].height = 1024;
        payloads[2].bytes.resize(1024 * 1024);
        payloads[3].bytes.resize(1024 * 1024);
        payloads[3].palette.resize(768);
        payloads[7] = sprite();
        payloads[8] = sprite();
        setup.preset = 1;
        const auto night = comanchecd::materialize_scene(setup, payloads, {});
        require(night.models[0].bytes[16] == 2 && night.models[1].bytes[16] == 1,
                "Night preset transforms rocket colors while bullets retain their original source palette");
        auto& shortened = payloads[7];
        shortened.bytes.resize(18 + 770 + 50, 0);
        shortened.bytes[18 + 770] = '3';
        shortened.bytes[18 + 771] = 'D';
        shortened.allocation_suffix = {0xc2};
        const auto carried = comanchecd::materialize_scene(setup, payloads, {});
        require(carried.models[0].metadata.size() == 51 && carried.models[0].metadata.back() == 0xc2,
                "Short SG metadata must copy the declared allocation-tail byte");
        shortened.allocation_suffix[0] = 0x17;
        const auto changed = comanchecd::materialize_scene(setup, payloads, {});
        require(changed.models[0].metadata.back() == 0x17,
                "Allocation history must remain an input to the SG copy");
        shortened.allocation_suffix.clear();
        const auto complete_tables = comanchecd::materialize_scene(setup, payloads, {});
        require(complete_tables.models[0].metadata.size() == 50,
                "Complete direction/elevation tables do not require an unread trailing allocation byte");
        payloads.resize(18);
        payloads[4] = sprite();
        setup.overlays.resize(1);
        setup.overlay_fields.fill(0);
        setup.overlay_fields[0] = 255;
        setup.overlay_fields[1] = 3;
        payloads[2].bytes[1023] = 11;
        payloads[2].bytes[1024] = 22;
        const auto crossing = comanchecd::materialize_scene(setup, payloads, {});
        require(crossing.terrain.height[1023] == 11 && crossing.terrain.height[1024] == 11 &&
                crossing.terrain.color[1024] != 0,
                "Overlay pixels cross the right edge into the next linear terrain row");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
