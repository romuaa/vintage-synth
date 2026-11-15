#pragma once

// What the Arp + Seq page takes from the audio -> editor feed (docs/architecture.md 9.1.4, 9.3.5).
// JUCE-free: the page reads the feed's atomics and passes plain numbers in.

#include "common/engine/UiFeed.h"

#include <cstdint>
#include <vector>

namespace vs::ui::model
{
/** Tells whether the audio thread is running: the feed's block counter advanced within `kAliveMs`. */
class FeedAlive
{
public:
    static constexpr double kAliveMs = 200.0;

    void update (std::uint32_t blockCount, double nowMs) noexcept
    {
        if (! seen || blockCount != last || nowMs < lastAdvanceMs)
        {
            last = blockCount;
            lastAdvanceMs = nowMs;
            seen = true;
        }
        now = nowMs;
    }
    bool alive() const noexcept { return seen && now - lastAdvanceMs <= kAliveMs; }

private:
    bool seen = false;
    std::uint32_t last = 0;
    double lastAdvanceMs = 0.0, now = 0.0;
};

/** The part whose step the grid highlights: the target part; for Both, Lower, or Upper if Lower is
    silent. In Whole mode only Lower plays. `target`: 0 Lower, 1 Upper, 2 Both; `keyMode`: 0 Whole. */
inline int playingStepOf (bool sequencerMode, bool feedAlive, int target, int keyMode, int lowerStep, int upperStep) noexcept
{
    if (! sequencerMode || ! feedAlive)
        return -1;
    int step = -1;
    if (keyMode == 0 || target == 0)
        step = lowerStep;
    else if (target == 1)
        step = upperStep;
    else
        step = lowerStep >= 0 ? lowerStep : upperStep;
    return step >= 0 && step < 16 ? step : -1;
}

/** The keys the preview plays: the player's keys while any is held and for 2 s after the last was
    released, else none (the page then shows the example chord). One per part. Allocates nothing. */
class HeldKeysMemory
{
public:
    static constexpr double kRememberMs = 2000.0;

    /** `order`: the part's pattern keys in press order from the feed (0xFF = none). Returns true
        if the remembered keys changed. */
    bool update (const UiFeed::Order& order, double nowMs) noexcept
    {
        UiFeed::Order next = keys;
        bool any = false;
        for (auto b : order)
            any = any || b != 0xFF;
        if (any)
        {
            next = order;
            lastHeldMs = nowMs;
        }
        else if (nowMs - lastHeldMs > kRememberMs || nowMs < lastHeldMs)
            next.fill (0xFF);
        const bool changed = next != keys;
        keys = next;
        return changed;
    }
    /** The remembered keys in press order (0xFF = none). */
    const UiFeed::Order& current() const noexcept { return keys; }
    std::vector<int> asVector() const
    {
        std::vector<int> v;
        for (auto b : keys)
            if (b != 0xFF)
                v.push_back (b);
        return v;
    }

private:
    static constexpr UiFeed::Order none() noexcept
    {
        UiFeed::Order o {};
        for (auto& b : o)
            b = 0xFF;
        return o;
    }
    UiFeed::Order keys = none();
    double lastHeldMs = -1.0e12;
};
} // namespace vs::ui::model
