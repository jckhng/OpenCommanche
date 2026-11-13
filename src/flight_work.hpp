#pragma once
#include "comanchecd/execution_clock.hpp"
#include "comanchecd/scene.hpp"

namespace comanchecd { struct TerrainFrame; struct SceneView; struct FlightState; struct FlightPolicy; }
namespace comanchecd { struct DefenseStatus; }
namespace comanchecd { enum class ControlSource; }
namespace comanchecd { enum class CloudMode; }

namespace comanchecd::flight_work {
void loop_prefix(std::uint32_t iteration, const FlightState& state, std::array<std::uint8_t, 6>& warned,
                 const std::function<void(std::uint8_t)>& announce, const CpuWorkSink& work,
                 const std::function<void()>& update_pending = {});
void polling(const FlightState& state, ControlSource source, std::uint8_t directions,
             const std::function<std::uint32_t()>& random, const CpuWorkSink& work);
void present(ExecutionClock& clock);
void sky(std::uint32_t heading, const CpuWorkSink& work);
void terrain(const TerrainFrame& frame, unsigned levels, const CpuWorkSink& work);
void monitor(const TerrainFrame& frame, unsigned levels, std::uint32_t heading, CloudMode clouds,
             const CpuWorkSink& work);
struct CameraInputs {
    bool exiting = false;
    std::uint8_t ground_height = 0, eye_height = 0;
    std::uint32_t subject_dx = 0, subject_dy = 0;
};
void camera(SceneView& view, ExecutionClock& clock, const std::function<std::int32_t()>& shake,
            const CameraInputs& inputs = {});
void fog(const TerrainFrame& frame, unsigned levels, const CpuWorkSink& work);
void model_projection(unsigned active, const CpuWorkSink& work);
void model_sort(const std::array<std::int32_t, scene_slots>& depth, const CpuWorkSink& work);
void dynamics(const FlightState& state, const FlightPolicy& policy, std::int32_t horizontal,
              std::int32_t vertical, const CpuWorkSink& work);
void motion(const FlightPolicy& policy, const DefenseStatus& status, const CpuWorkSink& work);
void ground_response(const FlightState& state, const FlightPolicy& policy, std::uint8_t ground,
                     std::uint8_t color, std::uint8_t previous_height, std::uint8_t terrain_variant,
                     const CpuWorkSink& work);
}
