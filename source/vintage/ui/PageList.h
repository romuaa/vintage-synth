#pragma once

// The Vintage editor's pages (docs/architecture.md 9.3), in rail order. JUCE-free: the processor
// uses the names to save the editor's UI state by name (9.1.3).

#include <array>

namespace vs::ui
{
enum class VintagePage
{
    Sound,
    Envelopes,
    Lfo,
    Play,
    ArpSeq,
    Effects,
    Browser
};

inline constexpr std::array<const char*, 7> kVintagePageNames { "sound", "envelopes", "lfo", "play", "arpseq", "effects", "browser" };
} // namespace vs::ui
