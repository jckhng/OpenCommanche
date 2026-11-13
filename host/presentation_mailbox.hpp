#pragma once

#include "comanchecd/presentation.hpp"
#include <atomic>

namespace comanchecd::host {
// The consumer holds one immutable frame across both eyes while publication continues.
class PresentationMailbox {
public:
    void publish(std::shared_ptr<const SessionPresentation> frame) {
        std::atomic_store_explicit(&latest_, std::move(frame), std::memory_order_release);
    }
    std::shared_ptr<const SessionPresentation> read() const {
        return std::atomic_load_explicit(&latest_, std::memory_order_acquire);
    }
private:
    std::shared_ptr<const SessionPresentation> latest_;
};
}
