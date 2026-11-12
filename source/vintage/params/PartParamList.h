#pragma once

// Single source of truth for the per-part parameter set.
// Every entry exists twice in the host: "lower.<id>" and "upper.<id>".
//
// The X-macros generate the PartParams snapshot struct, the APVTS layout,
// and the APVTS -> snapshot binding, so the three can never diverge.
//
// Names are UTF-8 (the velocity amounts use an arrow, written as an escape so every compiler
// reads the same bytes). ALWAYS convert a name with juce::String::fromUTF8 (name): building a
// juce::String from it directly asserts in debug builds (non-ASCII char*) and mangles the arrow.
// Guarded by the ProcessorTests "parameter names" test.
// Float params are normalised slider values (0..1, or -1..1 for fine tune and pan).
// Physical mappings live in common/params/Mappings.h, shared by DSP and display text.

#include <array>

namespace vs
{
// ---- Choice labels. Order == enum order in the DSP (static_asserted there). ----
inline constexpr std::array<const char*, 6> kLfoWaveLabels { "Sine", "Triangle", "Saw", "Square", "Random", "Noise" };
inline constexpr std::array<const char*, 3> kVcoModTargetLabels { "VCO-1", "Both", "VCO-2" };
inline constexpr std::array<const char*, 3> kPwmSourceLabels { "LFO", "Manual", "ENV-1" };
inline constexpr std::array<const char*, 6> kVco1RangeLabels { "32'", "16'", "8'", "4'", "2'", "1'" };
inline constexpr std::array<const char*, 4> kVco2RangeLabels { "16'", "8'", "4'", "2'" };
inline constexpr std::array<const char*, 6> kVcoWaveLabels { "Sine", "Triangle", "Saw", "Square", "Pulse", "Noise" };
inline constexpr std::array<const char*, 2> kVco2ModeLabels { "Normal", "Lo Freq" };
inline constexpr std::array<const char*, 2> kOffOnLabels { "Off", "On" };
inline constexpr std::array<const char*, 2> kSlopeLabels { "-12 dB", "-24 dB" };
inline constexpr std::array<const char*, 2> kEnvSourceLabels { "ENV-1", "ENV-2" };
inline constexpr std::array<const char*, 4> kVcaLfoLabels { "0", "1", "2", "3" };
inline constexpr std::array<const char*, 2> kPolarityLabels { "Normal", "Inverted" };
inline constexpr std::array<const char*, 3> kAssignModeLabels { "Poly", "Unison", "Solo" };
inline constexpr std::array<const char*, 4> kUnisonVoicesLabels { "2", "4", "8", "16" };
inline constexpr std::array<const char*, 2> kPortaModeLabels { "Always", "Legato" };
inline constexpr std::array<const char*, 13> kBendRangeLabels { "0 st", "1 st", "2 st", "3 st", "4 st", "5 st", "6 st",
                                                                "7 st", "8 st", "9 st", "10 st", "11 st", "12 st" };

// Octave offset of the "8'" position in each range list.
inline constexpr int kVco1RangeIndexOf8ft = 2;
inline constexpr int kVco2RangeIndexOf8ft = 1;

// X(member, idSuffix, name, min, max, default, displayKind)
#define VS_PART_FLOAT_PARAMS(X)                                                          \
    X (lfoRate,       "lfo.rate",          "LFO Rate",           0.0f, 1.0f, 0.65f, LfoRate)   \
    X (lfoDelay,      "lfo.delay",         "LFO Delay Time",     0.0f, 1.0f, 0.0f,  LfoDelay)  \
    X (vcoModLfo,     "vcomod.lfo_mod",    "VCO LFO Mod",        0.0f, 1.0f, 0.0f,  Panel10)   \
    X (vcoModEnv,     "vcomod.env_mod",    "VCO ENV Mod",        0.0f, 1.0f, 0.0f,  Panel10)   \
    X (pwmDepth,      "vcomod.pwm",        "Pulse Width Mod",    0.0f, 1.0f, 0.0f,  Panel10)   \
    X (vco1CrossMod,  "vco1.cross_mod",    "VCO-1 Cross Mod",    0.0f, 1.0f, 0.0f,  Panel10)   \
    X (vco2FineTune,  "vco2.fine_tune",    "VCO-2 Fine Tune",   -1.0f, 1.0f, 0.06f, Cents)     \
    X (sourceMix,     "mixer.source_mix",  "Source Mix",         0.0f, 1.0f, 0.5f,  Panel10)   \
    X (hpfCutoff,     "hpf.cutoff",        "HPF Cutoff",         0.0f, 1.0f, 0.0f,  HpfHz)     \
    X (vcfCutoff,     "vcf.cutoff",        "VCF Cutoff",         0.0f, 1.0f, 0.6f,  CutoffHz)  \
    X (vcfResonance,  "vcf.resonance",     "VCF Resonance",      0.0f, 1.0f, 0.1f,  Panel10)   \
    X (vcfEnvMod,     "vcf.env_mod",       "VCF ENV Mod",        0.0f, 1.0f, 0.35f, Panel10)   \
    X (vcfLfoMod,     "vcf.lfo_mod",       "VCF LFO Mod",        0.0f, 1.0f, 0.0f,  Panel10)   \
    X (vcfKeyFollow,  "vcf.key_follow",    "VCF Key Follow",     0.0f, 1.0f, 0.5f,  Panel10)   \
    X (vcaLevel,      "vca.level",         "VCA Level",          0.0f, 1.0f, 0.8f,  Panel10)   \
    X (env1Attack,    "env1.attack",       "ENV-1 Attack",       0.0f, 1.0f, 0.05f, EnvTime)   \
    X (env1Decay,     "env1.decay",        "ENV-1 Decay",        0.0f, 1.0f, 0.55f, EnvTime)   \
    X (env1Sustain,   "env1.sustain",      "ENV-1 Sustain",      0.0f, 1.0f, 0.3f,  Panel10)   \
    X (env1Release,   "env1.release",      "ENV-1 Release",      0.0f, 1.0f, 0.45f, EnvTime)   \
    X (env2Attack,    "env2.attack",       "ENV-2 Attack",       0.0f, 1.0f, 0.05f, EnvTime)   \
    X (env2Decay,     "env2.decay",        "ENV-2 Decay",        0.0f, 1.0f, 0.5f,  EnvTime)   \
    X (env2Sustain,   "env2.sustain",      "ENV-2 Sustain",      0.0f, 1.0f, 0.8f,  Panel10)   \
    X (env2Release,   "env2.release",      "ENV-2 Release",      0.0f, 1.0f, 0.45f, EnvTime)   \
    X (portaTime,     "porta.time",        "Portamento Time",    0.0f, 1.0f, 0.0f,  PortaTime) \
    X (unisonDetune,  "unison.detune",     "Unison Detune",      0.0f, 1.0f, 0.3f,  DetuneCents) \
    X (velToVca,      "vel.vca",           "Velocity \xe2\x86\x92 VCA", 0.0f, 1.0f, 0.5f, Percent) \
    X (velToVcf,      "vel.vcf",           "Velocity \xe2\x86\x92 VCF", 0.0f, 1.0f, 0.0f, Percent) \
    X (modWheelDepth, "mod.wheel_vco",     "Mod Wheel Vibrato",  0.0f, 1.0f, 0.3f,  Panel10)   \
    X (pan,           "mix.pan",           "Pan",               -1.0f, 1.0f, 0.0f,  Pan)

// X(member, idSuffix, name, labels, defaultIndex)
#define VS_PART_CHOICE_PARAMS(X)                                                              \
    X (lfoWave,       "lfo.waveform",      "LFO Waveform",       kLfoWaveLabels,      1)       \
    X (vcoModTarget,  "vcomod.target",     "VCO Mod Target",     kVcoModTargetLabels, 1)       \
    X (pwmSource,     "vcomod.pwm_source", "PWM Source",         kPwmSourceLabels,    0)       \
    X (vco1Range,     "vco1.range",        "VCO-1 Range",        kVco1RangeLabels,    kVco1RangeIndexOf8ft) \
    X (vco1Wave,      "vco1.waveform",     "VCO-1 Waveform",     kVcoWaveLabels,      2)       \
    X (vco2Range,     "vco2.range",        "VCO-2 Range",        kVco2RangeLabels,    kVco2RangeIndexOf8ft) \
    X (vco2Mode,      "vco2.mode",         "VCO-2 Mode",         kVco2ModeLabels,     0)       \
    X (vcoSync,       "vco2.sync",         "VCO Sync",           kOffOnLabels,        0)       \
    X (vco2Wave,      "vco2.waveform",     "VCO-2 Waveform",     kVcoWaveLabels,      2)       \
    X (vcfSlope,      "vcf.slope",         "VCF Slope",          kSlopeLabels,        1)       \
    X (vcfEnvSource,  "vcf.env_source",    "VCF ENV Source",     kEnvSourceLabels,    0)       \
    X (vcaLfoMod,     "vca.lfo_mod",       "VCA LFO Mod",        kVcaLfoLabels,       0)       \
    X (env1KeyFollow, "env1.key_follow",   "ENV-1 Key Follow",   kOffOnLabels,        0)       \
    X (env1Polarity,  "env1.polarity",     "ENV-1 Polarity",     kPolarityLabels,     0)       \
    X (env2KeyFollow, "env2.key_follow",   "ENV-2 Key Follow",   kOffOnLabels,        0)       \
    X (assignMode,    "assign.mode",       "Assign Mode",        kAssignModeLabels,   0)       \
    X (unisonVoices,  "assign.unison_voices", "Unison Voices",   kUnisonVoicesLabels, 1)       \
    X (portaMode,     "porta.mode",        "Portamento Mode",    kPortaModeLabels,    0)       \
    X (bendRange,     "bend.range",        "Bend Range",         kBendRangeLabels,    2)

/** Plain snapshot of one part's parameters. Defaults equal the host defaults. */
struct PartParams
{
#define VS_DECLARE_FLOAT(member, id, name, mn, mx, def, kind) float member = def;
#define VS_DECLARE_CHOICE(member, id, name, labels, def) int member = def;
    VS_PART_FLOAT_PARAMS (VS_DECLARE_FLOAT)
    VS_PART_CHOICE_PARAMS (VS_DECLARE_CHOICE)
#undef VS_DECLARE_FLOAT
#undef VS_DECLARE_CHOICE
};

inline constexpr const char* kLowerPrefix = "lower.";
inline constexpr const char* kUpperPrefix = "upper.";
} // namespace vs
