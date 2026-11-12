#pragma once

// Which keys are in the arpeggiator / sequencer pattern. Fixed size, no allocation.
//
// A key is "held" while it is physically down. It is "in the pattern" while held, or after its
// release while something retains it: the sustain pedal or Hold (latch).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace vs::seq
{
class KeyState
{
public:
    void reset() noexcept { *this = KeyState {}; }

    /** Key down. With Hold on and no key physically down, the latched pattern is discarded first
        (a new pattern starts); returns true in that case. A key that is already in the pattern
        (pressed again) moves to the end of the press order. */
    bool noteOn (int note, float velocity, bool hold) noexcept
    {
        if (! valid (note))
            return false;
        bool newPattern = false;
        if (hold && heldCount_ == 0 && orderCount_ > 0)
        {
            clearPattern();
            newPattern = true;
        }
        if (! held_[idx (note)])
        {
            held_[idx (note)] = true;
            ++heldCount_;
        }
        velocity_[idx (note)] = sanitiseVelocity (velocity);
        if (inPattern_[idx (note)])
            removeFromOrder (note);
        inPattern_[idx (note)] = true;
        order_[idx (orderCount_)] = static_cast<std::uint8_t> (note);
        ++orderCount_;
        return newPattern;
    }

    /** Key up. The key leaves the pattern unless the pedal or Hold retains it. A note-off for a
        key that is not physically down is ignored. */
    void noteOff (int note, bool hold) noexcept
    {
        if (! valid (note) || ! held_[idx (note)])
            return;
        held_[idx (note)] = false;
        --heldCount_;
        if (! pedal_ && ! hold)
            removeFromPattern (note);
    }

    void setPedal (bool down, bool hold) noexcept
    {
        pedal_ = down;
        if (! down)
            prune (hold);
    }

    /** Drop released keys that nothing retains any more (call after Hold or the pedal changed). */
    void prune (bool hold) noexcept
    {
        if (pedal_ || hold)
            return;
        int w = 0;
        for (int r = 0; r < orderCount_; ++r)
        {
            const auto n = order_[idx (r)];
            if (held_[n])
                order_[idx (w++)] = n;
            else
                inPattern_[n] = false;
        }
        orderCount_ = w;
    }

    /** All keys up, latch and pedal cleared (All Notes Off / All Sound Off). */
    void clearAll() noexcept
    {
        held_.fill (false);
        inPattern_.fill (false);
        heldCount_ = 0;
        orderCount_ = 0;
        pedal_ = false;
    }

    bool empty() const noexcept { return orderCount_ == 0; }
    int size() const noexcept { return orderCount_; }
    bool pedal() const noexcept { return pedal_; }
    int heldCount() const noexcept { return heldCount_; }
    bool contains (int note) const noexcept { return valid (note) && inPattern_[idx (note)]; }
    bool isHeld (int note) const noexcept { return valid (note) && held_[idx (note)]; }
    /** The i-th pattern key in press order. */
    int ordered (int i) const noexcept { return order_[idx (i)]; }
    float velocity (int note) const noexcept { return valid (note) ? velocity_[idx (note)] : 0.0f; }

    /** Lowest note in the pattern, or -1 when empty. */
    int lowest() const noexcept
    {
        for (int n = 0; n < 128; ++n)
            if (inPattern_[idx (n)])
                return n;
        return -1;
    }

    /** Velocity as the module emits it: finite, in [1/127, 1]; NaN becomes 0.8. */
    static float sanitiseVelocity (float v) noexcept
    {
        if (! std::isfinite (v))
            return 0.8f;
        return std::clamp (v, 1.0f / 127.0f, 1.0f);
    }

private:
    static constexpr bool valid (int note) noexcept { return note >= 0 && note < 128; }
    static constexpr std::size_t idx (int i) noexcept { return static_cast<std::size_t> (i); }

    void clearPattern() noexcept
    {
        inPattern_.fill (false);
        orderCount_ = 0;
    }

    void removeFromOrder (int note) noexcept
    {
        int w = 0;
        for (int r = 0; r < orderCount_; ++r)
            if (order_[idx (r)] != note)
                order_[idx (w++)] = order_[idx (r)];
        orderCount_ = w;
    }

    void removeFromPattern (int note) noexcept
    {
        inPattern_[idx (note)] = false;
        removeFromOrder (note);
    }

    std::array<bool, 128> held_ {};
    std::array<bool, 128> inPattern_ {};
    std::array<float, 128> velocity_ {};
    std::array<std::uint8_t, 128> order_ {};
    int heldCount_ = 0;
    int orderCount_ = 0;
    bool pedal_ = false;
};
} // namespace vs::seq
