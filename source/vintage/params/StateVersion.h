#pragma once

namespace vs
{
// Bump on every change to parameters or saved state, and add a migration step.
// See .claude/skills/add-parameter.
inline constexpr int kStateVersion = 1;
} // namespace vs
