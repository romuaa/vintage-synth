#pragma once

// Audio thread -> editor data (docs/architecture.md 9.1.4). JUCE-free.
//
// Written by the audio thread once per host block (the last step of the block algorithm, 8.2.3
// step 9), read by the editor on the message thread. Owned by the processor and lives as long as
// it; nothing the audio thread does depends on whether an editor reads it. Every member is a
// lock-free atomic (static_asserted below); all accesses are relaxed except the held-key order,
// which is a seqlock (writeOrder / readOrder).

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace vs
{
struct UiFeed
{
    // Output after the limiter and the clamp: peak |x| per channel since the editor last took it.
    std::atomic<float> peak[2] { 0.0f, 0.0f };
    // Smallest limiter gain since the editor last took it (1 = no reduction).
    std::atomic<float> limiterGainMin { 1.0f };
    std::atomic<std::uint32_t> blockCount { 0 };  // +1 per processed block (bypassed blocks too)
    std::atomic<float> internalRate { 96000.0f }; // real-time voice rate (host rate x real-time oversampling); prepareToPlay
    std::atomic<float> hostBpm { 120.0f };        // tempo the arp/seq used in the last block
    std::atomic<bool> hostPlaying { false };

    struct Part
    {
        std::atomic<std::uint64_t> held[2] { 0, 0 };     // keys the player holds that route to this part (bit n%64 of word n/64)
        std::atomic<std::uint64_t> sounding[2] { 0, 0 }; // notes of this part's sounding voices (after the arp/seq)
        std::atomic<std::int32_t> seqStep { -1 };        // 0..15: step that fired last while mode is Sequencer; -1 otherwise (M7d)
        std::atomic<std::uint32_t> stepCount { 0 };      // steps fired by this part's arp/seq since reset (M7d)
        // Held-key order for the arpeggiator preview (seqlock; up to 16 keys in press order; M7d writes it).
        std::atomic<std::uint32_t> orderSeq { 0 };            // odd while being written
        std::atomic<std::uint64_t> order[2] { ~0ull, ~0ull }; // byte i = i-th key in press order, 0xFF = none
        // (U4, M7f) newest sounding voice's cutoff, log2 Hz; NaN = not published.
        std::atomic<float> liveCutoffLog2 { std::numeric_limits<float>::quiet_NaN() };
    } part[2];

    static constexpr int kMaxOrderKeys = 16;
    using Order = std::array<std::uint8_t, kMaxOrderKeys>; // 0xFF = none

    // ---- Writer side (audio thread; one writer) ----

    /** Raises a channel's peak to `p` if `p` is larger. A concurrent take may lose this update;
        it can only hide a peak no larger than one already shown. */
    void raisePeak (int channel, float p) noexcept
    {
        auto& a = peak[channel & 1];
        if (p > a.load (std::memory_order_relaxed))
            a.store (p, std::memory_order_relaxed);
    }

    /** Lowers the stored limiter gain to `g` if `g` is smaller. */
    void lowerLimiterGain (float g) noexcept
    {
        if (g < limiterGainMin.load (std::memory_order_relaxed))
            limiterGainMin.store (g, std::memory_order_relaxed);
    }

    static void storeMask (std::atomic<std::uint64_t> (&mask)[2], std::uint64_t lo, std::uint64_t hi) noexcept
    {
        mask[0].store (lo, std::memory_order_relaxed);
        mask[1].store (hi, std::memory_order_relaxed);
    }

    /** Publishes a part's held-key order (seqlock writer: odd while writing). */
    void writeOrder (int p, const Order& keys) noexcept
    {
        auto& pt = part[p & 1];
        const std::uint32_t s = pt.orderSeq.load (std::memory_order_relaxed);
        pt.orderSeq.store (s + 1u, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);
        pt.order[0].store (pack (keys, 0), std::memory_order_relaxed);
        pt.order[1].store (pack (keys, 8), std::memory_order_relaxed);
        pt.orderSeq.store (s + 2u, std::memory_order_release);
    }

    // ---- Reader side (message thread) ----

    /** The peak since the last take (and resets it). */
    float takePeak (int channel) noexcept { return peak[channel & 1].exchange (0.0f, std::memory_order_relaxed); }

    /** The smallest limiter gain since the last take (and resets it to 1). */
    float takeLimiterGainMin() noexcept { return limiterGainMin.exchange (1.0f, std::memory_order_relaxed); }

    static bool hasNote (const std::atomic<std::uint64_t> (&mask)[2], int note) noexcept
    {
        if (note < 0 || note > 127)
            return false;
        return ((mask[note >> 6].load (std::memory_order_relaxed) >> (note & 63)) & 1u) != 0;
    }

    /** Reads a part's held-key order (seqlock reader). False if no consistent copy was read in 4
        attempts (keep the previous order then). */
    bool readOrder (int p, Order& out) const noexcept
    {
        const auto& pt = part[p & 1];
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const std::uint32_t s1 = pt.orderSeq.load (std::memory_order_acquire);
            if ((s1 & 1u) != 0)
                continue;
            const std::uint64_t w0 = pt.order[0].load (std::memory_order_relaxed);
            const std::uint64_t w1 = pt.order[1].load (std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_acquire);
            const std::uint32_t s2 = pt.orderSeq.load (std::memory_order_relaxed);
            if (s1 != s2)
                continue;
            unpack (w0, out, 0);
            unpack (w1, out, 8);
            return true;
        }
        return false;
    }

private:
    static std::uint64_t pack (const Order& keys, int first) noexcept
    {
        std::uint64_t w = 0;
        for (int i = 0; i < 8; ++i)
            w |= static_cast<std::uint64_t> (keys[static_cast<size_t> (first + i)]) << (8 * i);
        return w;
    }
    static void unpack (std::uint64_t w, Order& out, int first) noexcept
    {
        for (int i = 0; i < 8; ++i)
            out[static_cast<size_t> (first + i)] = static_cast<std::uint8_t> ((w >> (8 * i)) & 0xFFu);
    }
};

static_assert (std::atomic<float>::is_always_lock_free, "UiFeed: lock-free float atomics");
static_assert (std::atomic<bool>::is_always_lock_free, "UiFeed: lock-free bool atomics");
static_assert (std::atomic<std::uint32_t>::is_always_lock_free, "UiFeed: lock-free 32-bit atomics");
static_assert (std::atomic<std::int32_t>::is_always_lock_free, "UiFeed: lock-free 32-bit atomics");
static_assert (std::atomic<std::uint64_t>::is_always_lock_free, "UiFeed: lock-free 64-bit atomics (all five targets are 64-bit)");
} // namespace vs
