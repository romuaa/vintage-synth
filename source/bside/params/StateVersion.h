#pragma once

namespace bs
{
// Bump on every change to parameters or saved state, and add a migration step.
// See .claude/skills/add-parameter. Before v1.0 the version stays 1.
inline constexpr int kStateVersion = 1;
} // namespace bs
