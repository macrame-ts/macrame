#pragma once

#include <cstdint>

namespace ts
{

// Scheduling priority for a QUEUED task: higher pops first. A task's priority is applied
// when it is dispatched (to the scheduler, or a pipe). Bodyless / inline work carries no
// priority — it is never queued. Declaration order defines the ordering: `high` compares
// "before" `normal` before `low`. Shared by the scheduler and the task core so the latter
// can store a task's priority without depending on the scheduler header. Unsigned
// underlying type so it packs into a 2-bit bitfield on the control block (see
// `Task_control_block::Flags`).
enum class Priority : std::uint8_t { high, normal, low };

} // namespace ts
