#pragma once

#include <cstdint>

namespace ts
{

// Scheduling priority for a queued task: higher pops first. A task's priority is applied
// when it is dispatched (to the scheduler queue, or an object's access queue). Bodyless /
// inline work carries no priority - it is never queued. Declaration order defines the
// ordering: `high` compares "before" `normal` before `low`. Shared by the scheduler and
// the task core so the latter can store a task's priority without depending on the
// scheduler header; unsigned underlying type so it packs into a bitfield.
enum class Priority : std::uint8_t { high, normal, low };

} // namespace ts
