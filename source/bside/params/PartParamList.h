#pragma once

// Single source of truth for B-Side's per-part parameter set (docs/architecture.md 8.7).
// Every entry exists twice in the host: "lower.<id>" and "upper.<id>".
//
// One X-macro list, in the contract's order (which mixes float, integer and choice parameters, so the
// list takes three entry macros: F for float, I for stepped integer, C for choice). It generates the
// PartParams snapshot struct, the APVTS layout and the binding, so the three can never diverge, and
// lower.* and upper.* cannot differ. The three oscN.* blocks expand from one definition.
//
// Names are UTF-8 (the velocity amounts use an arrow, written as an escape so every compiler reads the
// same bytes). ALWAYS convert a name with juce::String::fromUTF8 (name).
//
// Value units: physical, as the contract's table (cents, percent, register units, integer steps);
// the conversions to chip units live in PartMappings.h so that display text and DSP agree.
//
// SMOOTHING (not built in this step, nothing reads these on the audio thread yet): the continuous
// parameters are the F entries: oscN.fine, oscN.pw, filter.cutoff, filter.key_follow, chip.variation,
// tick.free_hz, macro.*, vel.*, vibrato.rate, mod.wheel_vibrato, unison.detune, porta.time, mix.pan.
// The I entries (integers) and the C entries (switches and choices) are stepped and are never smoothed.

#include <array>

#include "common/params/CommonLabels.h"

namespace bs
{
// ---- Choice labels. The order is the order of the values in the DSP. ----
inline constexpr std::array<const char*, 9> kOscWaveLabels { "Off", "Triangle", "Saw", "Pulse", "Noise", "Tri+Saw", "Tri+Pulse", "Saw+Pulse", "Tri+Saw+Pulse" };
inline constexpr int kOscWavePulse = 3;
inline constexpr int kOscWaveOff = 0;
inline constexpr std::array<const char*, 8> kFilterModeLabels { "Off", "Low", "Band", "High", "Low+Band", "Band+High", "Low+High", "All" };
inline constexpr std::array<const char*, 2> kFilterTopologyLabels { "Per Note", "Shared" };
inline constexpr std::array<const char*, 2> kChipModelLabels { "Model A", "Model B" };
inline constexpr std::array<const char*, 2> kChipResolutionLabels { "Smooth", "Authentic" };
inline constexpr std::array<const char*, 3> kRestartPolicyLabels { "Clean", "Precondition", "Authentic" };
inline constexpr std::array<const char*, 3> kTickRateLabels { "Frame", "Free", "Tempo" };
inline constexpr std::array<const char*, 4> kTickSyncLabels { "6 per beat", "12 per beat", "24 per beat", "48 per beat" };
inline constexpr std::array<const char*, 3> kAssignModeLabels { "Poly", "Unison", "Solo" };
inline constexpr std::array<const char*, 4> kUnisonVoicesLabels { "2", "4", "8", "16" };
inline constexpr std::array<const char*, 2> kPortaModeLabels { "Always", "Legato" };
inline constexpr std::array<const char*, 13> kBendRangeLabels { "0 st", "1 st", "2 st", "3 st", "4 st", "5 st", "6 st",
                                                                "7 st", "8 st", "9 st", "10 st", "11 st", "12 st" };
inline constexpr std::array<const char*, 2> kChipClockLabels { "50 Hz system", "60 Hz system" };

/** How a float parameter is shown and parsed (PartMappings.h has the conversions). */
enum class Disp
{
    Cents,         // plain cents, "+12 ct"
    Percent,       // plain percent, "50.0 %"
    PercentSigned, // plain percent with a sign, "+10.0 %"
    Register,      // plain register units, "1024"
    Hz,            // plain Hz, "5.50 Hz"
    TickHz,        // 10 ... 400 Hz on an exponential scale, "50.0 Hz"
    WheelCents,    // 0 ... 1 shown as 0 ... 100 ct
    UnisonCents,   // 0 ... 1 shown as 0 ... 50 ct
    Unit,          // 0 ... 1 shown as a percentage ("50 %"), as Vintage's velocity amounts
    Porta,         // 0 ... 1 shown as Vintage shows portamento time
    Pan            // -1 ... +1, as Vintage
};

/** How a stepped integer parameter is shown and parsed. */
enum class IntDisp
{
    Plain,    // "15"
    Semitones, // "+5 st"
    Attack,   // the nominal attack time of the setting
    Decay,    // the nominal decay time (3 x attack)
    Release,  // the nominal release time (3 x attack)
    Multiplier // "x1"
};

// One oscillator's 11 parameters (8.7). N is 1, 2 or 3; WAVEDEF the default wave index.
// F(member, idSuffix, name, min, max, default, Disp)
// I(member, idSuffix, name, min, max, default, IntDisp)
// C(member, idSuffix, name, labels, defaultIndex)
#define BS_OSC_PARAMS(N, WAVEDEF, F, C, I)                                                                   \
    C (osc##N##Wave,    "osc" #N ".wave",    "Osc " #N " Wave",        kOscWaveLabels, WAVEDEF)              \
    I (osc##N##Coarse,  "osc" #N ".coarse",  "Osc " #N " Coarse",      -24, 24, 0, Semitones)                \
    F (osc##N##Fine,    "osc" #N ".fine",    "Osc " #N " Fine",        -100.0f, 100.0f, 0.0f, Cents)         \
    F (osc##N##Pw,      "osc" #N ".pw",      "Osc " #N " Pulse Width", 0.0f, 100.0f, 50.0f, Percent)         \
    I (osc##N##Attack,  "osc" #N ".attack",  "Osc " #N " Attack",      0, 15, 0, Attack)                     \
    I (osc##N##Decay,   "osc" #N ".decay",   "Osc " #N " Decay",       0, 15, 0, Decay)                      \
    I (osc##N##Sustain, "osc" #N ".sustain", "Osc " #N " Sustain",     0, 15, 15, Plain)                     \
    I (osc##N##Release, "osc" #N ".release", "Osc " #N " Release",     0, 15, 6, Release)                    \
    C (osc##N##Sync,    "osc" #N ".sync",    "Osc " #N " Sync",        vs::kOffOnLabels, 0)                  \
    C (osc##N##Ring,    "osc" #N ".ring",    "Osc " #N " Ring Mod",    vs::kOffOnLabels, 0)                  \
    C (osc##N##Filter,  "osc" #N ".filter",  "Osc " #N " Filter",      vs::kOffOnLabels, 1)

// The 63 per-part parameters, in the contract's order.
#define BS_PART_PARAMS(F, C, I)                                                                                         \
    BS_OSC_PARAMS (1, 3, F, C, I)                                                                                       \
    BS_OSC_PARAMS (2, 0, F, C, I)                                                                                       \
    BS_OSC_PARAMS (3, 0, F, C, I)                                                                                       \
    C (osc3Mute,        "osc3.mute",            "Osc 3 Mute",           vs::kOffOnLabels, 0)                            \
    F (filterCutoff,    "filter.cutoff",        "Filter Cutoff",        0.0f, 2047.0f, 1024.0f, Register)               \
    I (filterResonance, "filter.resonance",     "Filter Resonance",     0, 15, 0, Plain)                                \
    C (filterMode,      "filter.mode",          "Filter Mode",          kFilterModeLabels, 1)                           \
    F (filterKeyFollow, "filter.key_follow",    "Filter Key Follow",    0.0f, 100.0f, 0.0f, Percent)                    \
    C (filterTopology,  "filter.topology",      "Filter Topology",      kFilterTopologyLabels, 0)                       \
    C (chipModel,       "chip.model",           "Chip Model",           kChipModelLabels, 1)                            \
    F (chipVariation,   "chip.variation",       "Chip Variation",       0.0f, 100.0f, 50.0f, Percent)                   \
    I (chipVolume,      "chip.volume",          "Chip Volume",          0, 15, 15, Plain)                               \
    C (chipResolution,  "chip.resolution",      "Resolution",           kChipResolutionLabels, 0)                       \
    C (chipVolumeDc,    "chip.volume_dc",       "Volume DC",            vs::kOffOnLabels, 0)                            \
    C (restartPolicy,   "restart.policy",       "Restart Policy",       kRestartPolicyLabels, 0)                        \
    C (tickRate,        "tick.rate",            "Tick Rate",            kTickRateLabels, 0)                             \
    F (tickFreeHz,      "tick.free_hz",         "Tick Free Rate",       10.0f, 400.0f, 50.0f, TickHz)                   \
    I (tickMult,        "tick.mult",            "Tick Multiplier",      1, 8, 1, Multiplier)                            \
    C (tickSync,        "tick.sync",            "Tick Sync",            kTickSyncLabels, 2)                             \
    F (macroPwOffset,   "macro.pw_offset",      "PW Offset",            -50.0f, 50.0f, 0.0f, PercentSigned)             \
    F (macroCutoffOffset, "macro.cutoff_offset", "Cutoff Offset",       -100.0f, 100.0f, 0.0f, PercentSigned)           \
    F (macroSweepDepth, "macro.sweep_depth",    "Sweep Depth",          0.0f, 200.0f, 100.0f, Percent)                  \
    F (velVolume,       "vel.volume",           "Velocity \xe2\x86\x92 Volume", 0.0f, 1.0f, 0.5f, Unit)                 \
    F (velCutoff,       "vel.cutoff",           "Velocity \xe2\x86\x92 Cutoff", 0.0f, 1.0f, 0.0f, Unit)                 \
    F (vibratoRate,     "vibrato.rate",         "Vibrato Rate",         0.5f, 12.0f, 5.5f, Hz)                          \
    F (modWheelVibrato, "mod.wheel_vibrato",    "Mod Wheel Vibrato",    0.0f, 1.0f, 0.3f, WheelCents)                   \
    C (assignMode,      "assign.mode",          "Assign Mode",          kAssignModeLabels, 0)                           \
    C (unisonVoices,    "assign.unison_voices", "Unison Voices",        kUnisonVoicesLabels, 1)                         \
    F (unisonDetune,    "unison.detune",        "Unison Detune",        0.0f, 1.0f, 0.3f, UnisonCents)                  \
    F (portaTime,       "porta.time",           "Portamento Time",      0.0f, 1.0f, 0.0f, Porta)                        \
    C (portaMode,       "porta.mode",           "Portamento Mode",      kPortaModeLabels, 0)                            \
    C (bendRange,       "bend.range",           "Bend Range",           kBendRangeLabels, 2)                            \
    F (mixPan,          "mix.pan",              "Pan",                  -1.0f, 1.0f, 0.0f, Pan)

/** Number of per-part parameters (8.7). */
inline constexpr int kPartParamCount = 63;

/** Plain snapshot of one part's parameters. Defaults equal the host defaults. */
struct PartParams
{
#define BS_DECLARE_FLOAT(member, id, name, mn, mx, def, kind) float member = def;
#define BS_DECLARE_INT(member, id, name, mn, mx, def, kind) int member = def;
#define BS_DECLARE_CHOICE(member, id, name, labels, def) int member = def;
    BS_PART_PARAMS (BS_DECLARE_FLOAT, BS_DECLARE_CHOICE, BS_DECLARE_INT)
#undef BS_DECLARE_FLOAT
#undef BS_DECLARE_INT
#undef BS_DECLARE_CHOICE
};

inline constexpr const char* kLowerPrefix = "lower.";
inline constexpr const char* kUpperPrefix = "upper.";
inline constexpr const char* kChipClockId = "chip.clock";
} // namespace bs
