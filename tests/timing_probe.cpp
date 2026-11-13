#include "comanchecd/timing.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            throw std::invalid_argument("Usage: timing_probe INPUT OUTPUT");
        }
        std::ifstream input(argv[1]);
        std::ofstream output(argv[2]);
        comanchecd::IrqState state;
        while (input >> state.random_seed) {
            if (!(input >> state.shake_ticks >> state.shake_offset)) {
                throw std::invalid_argument("Truncated IRQ state");
            }
            state.interrupt();
            output << state.random_seed << ' ' << state.shake_ticks << ' ' << state.shake_offset << '\n';
        }
        if (!input.eof() || !output) {
            throw std::runtime_error("Cannot read or write IRQ observations");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
