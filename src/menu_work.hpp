#pragma once
#include "comanchecd/cpu_work.hpp"
#include "comanchecd/menu_renderer.hpp"

namespace comanchecd::menu_work {
struct Colors { Theme theme; ColorMap remap; };
// Includes the caller's CALL and the builder's RET.
Colors colors(const Palette& palette, const Transfer& transfer, const CpuWorkSink& work);
void background(const CpuWorkSink& work);
void save_background(const CpuWorkSink& work);
void copy_page(unsigned height, const CpuWorkSink& work);
void box(unsigned width, unsigned height, const CpuWorkSink& work);
void highlight(unsigned width, unsigned height, const CpuWorkSink& work);
}
