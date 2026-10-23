// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include <cstdint>

// The ports of Euclidelay (docs/design.md, "Ports"). Indices and symbols are
// permanent once a patch is saved with them: new ports go at the end.
// kPorts is the one source of ranges and defaults: the plugin clamps to it,
// the tests run with it, and tests/ttl_check.py compares the Turtle with it.
namespace euclidelay {

inline constexpr char kURI[] = "https://github.com/ABustos2606/euclidelay#euclidelay";

enum class Kind { audio_in, audio_out, control_in };

struct PortSpec {
    const char* symbol;
    Kind kind;
    float min, def, max;
};

// Global ports, by index.
enum Port : uint32_t {
    in = 0,
    out_l = 1,
    out_r = 2,
    enabled = 3,
    bpm = 4,
    step = 5,
    dry = 6,
    echoes = 7,
    tone = 8,
    first_voice = 9,
    port_count = 37
};

// Voice n (0 to 3) port p is at first_voice + n * kVoicePorts + p.
enum VoicePort : uint32_t { voice = 0, steps, hits, shift, feedback, level, pan };
inline constexpr uint32_t kVoices = 4, kVoicePorts = 7;
inline constexpr uint32_t voice_port(uint32_t n, VoicePort p) { return first_voice + n * kVoicePorts + p; }

// Step, as a note value in beats (the enumeration's values are the indices).
inline constexpr const char* kStepNames[] = {"1/32", "1/16T", "1/16", "1/8T", "1/8", "1/8 dot", "1/4"};
inline constexpr double kStepBeats[] = {0.125, 1.0 / 6, 0.25, 1.0 / 3, 0.5, 0.75, 1.0};

// Each voice's loop buffer: the longest loop (16 steps of 1/4 at 30 BPM,
// 32 s) plus the 30 ms crossfade, in seconds.
inline constexpr double kBufferSeconds = 32.03;

// symbol, kind, min, default, max
inline constexpr PortSpec kPorts[] = {
    {"in", Kind::audio_in, 0, 0, 0},
    {"out_l", Kind::audio_out, 0, 0, 0},
    {"out_r", Kind::audio_out, 0, 0, 0},
    {"enabled", Kind::control_in, 0, 1, 1},
    {"bpm", Kind::control_in, 30, 120, 300},
    {"step", Kind::control_in, 0, 2, 6},
    {"dry", Kind::control_in, 0, 1, 1},
    {"echoes", Kind::control_in, 0, 0.8f, 1},
    {"tone", Kind::control_in, 0, 0.7f, 1},
    {"voice_1", Kind::control_in, 0, 1, 1},
    {"steps_1", Kind::control_in, 1, 8, 16},
    {"hits_1", Kind::control_in, 0, 3, 16},
    {"shift_1", Kind::control_in, 0, 0, 15},
    {"feedback_1", Kind::control_in, 0, 0.5f, 0.95f},
    {"level_1", Kind::control_in, 0, 1, 1},
    {"pan_1", Kind::control_in, -1, -0.6f, 1},
    {"voice_2", Kind::control_in, 0, 1, 1},
    {"steps_2", Kind::control_in, 1, 5, 16},
    {"hits_2", Kind::control_in, 0, 2, 16},
    {"shift_2", Kind::control_in, 0, 0, 15},
    {"feedback_2", Kind::control_in, 0, 0.5f, 0.95f},
    {"level_2", Kind::control_in, 0, 1, 1},
    {"pan_2", Kind::control_in, -1, 0.6f, 1},
    {"voice_3", Kind::control_in, 0, 0, 1},
    {"steps_3", Kind::control_in, 1, 7, 16},
    {"hits_3", Kind::control_in, 0, 4, 16},
    {"shift_3", Kind::control_in, 0, 0, 15},
    {"feedback_3", Kind::control_in, 0, 0.5f, 0.95f},
    {"level_3", Kind::control_in, 0, 1, 1},
    {"pan_3", Kind::control_in, -1, -0.2f, 1},
    {"voice_4", Kind::control_in, 0, 0, 1},
    {"steps_4", Kind::control_in, 1, 4, 16},
    {"hits_4", Kind::control_in, 0, 3, 16},
    {"shift_4", Kind::control_in, 0, 0, 15},
    {"feedback_4", Kind::control_in, 0, 0.5f, 0.95f},
    {"level_4", Kind::control_in, 0, 1, 1},
    {"pan_4", Kind::control_in, -1, 0.2f, 1},
};

static_assert(sizeof kPorts / sizeof kPorts[0] == port_count, "one PortSpec per port");
static_assert(first_voice + kVoices * kVoicePorts == port_count, "four voices of seven ports end the list");
static_assert(sizeof kStepNames / sizeof kStepNames[0] == sizeof kStepBeats / sizeof kStepBeats[0], "a value per Step name");

} // namespace euclidelay
