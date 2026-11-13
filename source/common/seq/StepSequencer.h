#pragma once

// Step sequencer note selection. Pure state machine (step counter); timing, gate and note-offs
// belong to the front-end.

#include "KeyState.h"
#include "Settings.h"

#include <algorithm>
#include <cstdint>

namespace vs::seq
{
class StepSequencer
{
public:
    /** The sequence starts again from step 1. */
    void restart() noexcept { position_ = 0; }

    /** The action for the next step. The note is the lowest pattern key plus the step's offset
        (transposition relative to C4 = 60, so a step offset is relative to the played root). */
    StepAction step (const KeyState& keys, const Settings& s) noexcept
    {
        StepAction a;
        const int length = std::clamp (s.seqLength, 1, kMaxSeqSteps);
        const auto cur = static_cast<std::size_t> (position_ % length);
        const auto next = static_cast<std::size_t> ((position_ + 1) % length);
        ++position_;
        const SeqStep& st = s.seqSteps[cur];
        a.tieFollows = s.seqSteps[next].type == StepType::Tie;
        const int root = keys.lowest();
        if (root < 0)
            return a; // no key: nothing to play
        switch (st.type)
        {
            case StepType::Rest:
                break;
            case StepType::Tie:
                a.kind = StepAction::Kind::Tie;
                break;
            case StepType::Note:
                a.kind = StepAction::Kind::Note;
                a.note = std::clamp (root + st.semitones, 0, 127);
                a.velocity = std::clamp (st.velocity, 1.0f / 127.0f, 1.0f);
                break;
        }
        return a;
    }

private:
    std::int64_t position_ = 0;
};
} // namespace vs::seq
