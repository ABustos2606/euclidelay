// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#include "voice.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace euclidelay {

float tone_pole(float tone, double rate) {
    constexpr double darkest = 1500.0;  // Hz at Tone 0 (tune)
    const double t = tone < 0 ? 0 : tone > 1 ? 1 : tone;
    const double pole = std::exp(-2.0 * M_PI * darkest / rate);
    // The square root keeps most of the knob's travel in the darker half
    // (tune): Tone 0.7 is about 6 kHz at 48 kHz.
    return static_cast<float>(pole * std::sqrt(1.0 - t));
}

Voice::~Voice() { std::free(buffer_); }

bool Voice::allocate(double seconds, double rate) {
    std::free(buffer_);
    size_ = static_cast<std::size_t>(std::ceil(seconds * rate)) + 1;
    buffer_ = static_cast<float*>(std::calloc(size_, sizeof(float)));
    if (!buffer_) size_ = 0;
    write_ = 0;
    tone_ = 0;
    return buffer_;
}

void Voice::clear() {
    if (buffer_) std::memset(buffer_, 0, size_ * sizeof(float));
    write_ = 0;
    tone_ = 0;
}

} // namespace euclidelay
