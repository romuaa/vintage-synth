#pragma once

// Arpeggiator / step sequencer: transport, settings and plain data types.
// JUCE-free, header-only, no allocation. See docs/architecture.md section 6.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace vs::seq
{
/** What the host tells us about time for one process call. `ppqPosition` is the musical position
    (quarter notes) at the START of the block. */
struct Transport
{
    double sampleRate; // Hz, > 0
    double bpm;        // quarter notes per minute, > 0 while `playing`
    double ppqPosition;
    bool playing;
};

/** What sits in front of the part. */
enum class Mode : std::uint8_t
{
    Off, // events pass through bit-exactly
    Arp,
    Seq
};

/** Note division of one step. Order == the future host parameter's choice order. */
enum class Rate : std::uint8_t
{
    Quarter,          // 1/4
    Eighth,           // 1/8
    EighthTriplet,    // 1/8T
    Sixteenth,        // 1/16
    SixteenthTriplet, // 1/16T
    ThirtySecond      // 1/32
};

/** Steps per quarter note for a rate (1, 2, 3, 4, 6, 8). */
constexpr int stepsPerQuarter (Rate r) noexcept
{
    switch (r)
    {
        case Rate::Quarter: return 1;
        case Rate::Eighth: return 2;
        case Rate::EighthTriplet: return 3;
        case Rate::Sixteenth: return 4;
        case Rate::SixteenthTriplet: return 6;
        case Rate::ThirtySecond: return 8;
    }
    return 4;
}

enum class ArpPattern : std::uint8_t
{
    Up,
    Down,
    UpDown, // end notes are not repeated
    Random, // seeded, reproducible
    AsPlayed
};

enum class StepType : std::uint8_t
{
    Note,
    Rest,
    Tie // extends the previous note without retriggering
};

struct SeqStep
{
    int semitones = 0;   // -24..+24, added to the transposed root
    StepType type = StepType::Note;
    float velocity = 0.8f; // 0..1 (clamped to at least 1/127 for a Note)
};

inline constexpr int kMaxSeqSteps = 16;
inline constexpr int kMaxOctaves = 4;

/** Everything the user can set. Every field is in musical/physical units; `sanitised()` clamps
    out-of-range or non-finite values (the module applies it on `setSettings`). Settings are
    applied at the start of the next `process` call; mode/hold/rate changes act at that sample (a
    rate change continues after the last step that fired), everything else (pattern, octaves,
    steps, gate, swing) from the next step. */
struct Settings
{
    // ---- Shared ---------------------------------------------------------------------------
    Mode mode = Mode::Off;
    Rate rate = Rate::Sixteenth;    // step length
    double swing = 0.5;             // 0.50..0.75; every second step starts at `swing` of the step pair (double: 0.6 must not land a sample late)
    double gate = 0.5;              // 0.05..1.0 of the (swung) step; 1.0 = legato: releases exactly at the next step, after the next note starts
    bool hold = false;              // latch; ignored while mode == Off
    double internalBpm = 120.0;     // 20..300; clock tempo while the host transport is not playing

    // ---- Arpeggiator ------------------------------------------------------------------------
    ArpPattern arpPattern = ArpPattern::Up;
    int arpOctaves = 1;             // 1..4
    std::uint32_t seed = 1;         // Random pattern seed (any value)

    // ---- Sequencer --------------------------------------------------------------------------
    int seqLength = 16;             // 1..16
    std::array<SeqStep, kMaxSeqSteps> seqSteps {};

    Settings sanitised() const noexcept
    {
        Settings s = *this;
        s.swing = clampValue (s.swing, 0.5, 0.75, 0.5);
        s.gate = clampValue (s.gate, 0.05, 1.0, 0.5);
        s.internalBpm = clampValue (s.internalBpm, 20.0, 300.0, 120.0);
        s.arpOctaves = std::clamp (s.arpOctaves, 1, kMaxOctaves);
        s.seqLength = std::clamp (s.seqLength, 1, kMaxSeqSteps);
        for (auto& st : s.seqSteps)
        {
            st.semitones = std::clamp (st.semitones, -24, 24);
            st.velocity = clampValue (st.velocity, 0.0f, 1.0f, 0.8f);
        }
        return s;
    }

private:
    template <typename T>
    static T clampValue (T v, T lo, T hi, T fallback) noexcept
    {
        if (! std::isfinite (v))
            return fallback;
        return std::clamp (v, lo, hi);
    }
};

/** What one clock step asks the front-end to do. */
struct StepAction
{
    enum class Kind : std::uint8_t
    {
        Rest,
        Note,
        Tie
    };
    Kind kind = Kind::Rest;
    int note = 0;
    float velocity = 0.0f;
    bool tieFollows = false; // the next step of the sequence is a Tie (keep the note open)
};
} // namespace vs::seq
