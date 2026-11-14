#pragma once

// Non-parameter UI state saved with the session (docs/architecture.md 9.1.3). JUCE-free.
//
// "Panel Mode is UI state", generalised: what the editor shows (page, edited part, the Arp + Seq
// view while the mode is Off, the window size, the delay/reverb level the power switches restore)
// is saved with the session, but it is not a parameter, not automatable, not part of a program,
// tone or bank, and never marks the program modified. Every member is an atomic (relaxed): the
// editor writes it on the message thread, a host saves or restores the session on any thread.
// The text form (names, not indices, so reordering pages never breaks a session) is defined here;
// the state tree child is written and read by the shared skeleton (ShellSupport).

#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <span>
#include <string>
#include <string_view>

namespace vs::shell
{
struct EditorState
{
    static constexpr int kMinSizePercent = 75, kMaxSizePercent = 200;
    static constexpr float kDefaultFxLastLevel = 0.35f;

    std::atomic<int> page { 0 };        // index into the instrument's page list (Vintage: 0 Sound … 6 Browser)
    std::atomic<int> part { 0 };        // edited part: 0 Lower, 1 Upper
    std::atomic<int> arpView { 0 };     // view shown while arp.mode is Off: 0 Arpeggiator, 1 Sequencer
    std::atomic<int> sizePercent { 0 }; // 75 … 200; 0 = not chosen yet (fit to screen)
    std::atomic<float> fxLastLevel[2] { kDefaultFxLastLevel, kDefaultFxLastLevel }; // delay, reverb level restored by the power switch

    void resetToDefaults() noexcept
    {
        page.store (0, std::memory_order_relaxed);
        part.store (0, std::memory_order_relaxed);
        arpView.store (0, std::memory_order_relaxed);
        sizePercent.store (0, std::memory_order_relaxed);
        fxLastLevel[0].store (kDefaultFxLastLevel, std::memory_order_relaxed);
        fxLastLevel[1].store (kDefaultFxLastLevel, std::memory_order_relaxed);
    }
};

/** The `UiState` child's attribute text, one string per attribute (any thread; plain copies). */
struct EditorStateText
{
    std::string page, part, arpView, size, delayLast, reverbLast;
};

namespace editorstate
{
inline constexpr std::array<const char*, 2> kPartNames { "lower", "upper" };
inline constexpr std::array<const char*, 2> kArpViewNames { "arp", "seq" };

/** Index of `name` in `names`, or -1. */
inline int indexOf (std::string_view name, std::span<const char* const> names) noexcept
{
    for (size_t i = 0; i < names.size(); ++i)
        if (name == names[i])
            return static_cast<int> (i);
    return -1;
}

inline std::string formatFloat (float v)
{
    // Two decimals are enough for a level the user set on a slider; locale-independent.
    const int hundredths = static_cast<int> (std::lround (v * 100.0f));
    std::string s = std::to_string (hundredths / 100) + ".";
    const int frac = hundredths % 100;
    s += static_cast<char> ('0' + frac / 10);
    s += static_cast<char> ('0' + frac % 10);
    return s;
}

/** Strict number parsing: the whole text must be a number (no locale). */
inline bool parseInt (std::string_view t, int& out) noexcept
{
    if (t.empty())
        return false;
    const auto r = std::from_chars (t.data(), t.data() + t.size(), out);
    return r.ec == std::errc() && r.ptr == t.data() + t.size();
}

inline bool parseLevel (std::string_view t, float& out) noexcept
{
    // "0.35", "1", "0.5": digits, at most one dot; nothing else. (std::from_chars for float is not
    // available on every target's standard library.)
    if (t.empty() || t.size() > 12)
        return false;
    double whole = 0.0, scale = 1.0;
    bool dot = false, digits = false;
    for (const char c : t)
    {
        if (c == '.' && ! dot)
        {
            dot = true;
            continue;
        }
        if (c < '0' || c > '9')
            return false;
        digits = true;
        if (dot)
        {
            scale *= 0.1;
            whole += (c - '0') * scale;
        }
        else
            whole = whole * 10.0 + (c - '0');
    }
    if (! digits)
        return false;
    out = static_cast<float> (whole);
    return true;
}
} // namespace editorstate

/** The state as text. `pageNames`: the instrument's page list (index -> name). */
inline EditorStateText toText (const EditorState& s, std::span<const char* const> pageNames)
{
    using namespace editorstate;
    EditorStateText t;
    const int page = s.page.load (std::memory_order_relaxed);
    t.page = page >= 0 && static_cast<size_t> (page) < pageNames.size() ? pageNames[static_cast<size_t> (page)] : (pageNames.empty() ? "" : pageNames[0]);
    t.part = kPartNames[s.part.load (std::memory_order_relaxed) == 1 ? 1 : 0];
    t.arpView = kArpViewNames[s.arpView.load (std::memory_order_relaxed) == 1 ? 1 : 0];
    t.size = std::to_string (s.sizePercent.load (std::memory_order_relaxed));
    t.delayLast = formatFloat (s.fxLastLevel[0].load (std::memory_order_relaxed));
    t.reverbLast = formatFloat (s.fxLastLevel[1].load (std::memory_order_relaxed));
    return t;
}

/** Restores the state from text: an unknown or invalid attribute takes its default. */
inline void fromText (EditorState& s, const EditorStateText& t, std::span<const char* const> pageNames) noexcept
{
    using namespace editorstate;
    const int page = indexOf (t.page, pageNames);
    s.page.store (page >= 0 ? page : 0, std::memory_order_relaxed);
    const int part = indexOf (t.part, kPartNames);
    s.part.store (part >= 0 ? part : 0, std::memory_order_relaxed);
    const int view = indexOf (t.arpView, kArpViewNames);
    s.arpView.store (view >= 0 ? view : 0, std::memory_order_relaxed);
    int size = 0;
    if (! parseInt (t.size, size) || ! (size == 0 || (size >= EditorState::kMinSizePercent && size <= EditorState::kMaxSizePercent)))
        size = 0;
    s.sizePercent.store (size, std::memory_order_relaxed);
    const std::string_view levels[2] { t.delayLast, t.reverbLast };
    for (int i = 0; i < 2; ++i)
    {
        float v = 0.0f;
        if (! parseLevel (levels[i], v) || ! (v > 0.0f && v <= 1.0f))
            v = EditorState::kDefaultFxLastLevel;
        s.fxLastLevel[i].store (v, std::memory_order_relaxed);
    }
}
} // namespace vs::shell
