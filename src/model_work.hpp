#pragma once
#include "comanchecd/world_renderer.hpp"
namespace comanchecd {
enum class ModelProjection { directional, animated };
struct ModelClipResult { bool visible = false; std::uint8_t y = 0; };
ModelClipResult model_clip_work(const TerrainTables& tables, const std::vector<std::uint8_t>& projection,
                               const TerrainFrame& frame, const SceneView& view, const SceneEntity& entity,
                               std::size_t slot, std::int32_t lateral, std::int32_t depth,
                               ModelProjection kind, const CpuWorkSink& work);
} // namespace comanchecd
