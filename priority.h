#pragma once

// Scheduling priority for a QUEUED task: higher pops first. A task's priority is applied
// when it is dispatched (to the scheduler, or a pipe). Bodyless / inline work carries no
// priority — it is never queued. Declaration order defines the ordering: `high` compares
// "before" `normal` before `low` (see `Task_queue_cmp` in scheduler.h). Shared by the
// scheduler and the task core so the latter can store a task's priority without depending
// on the scheduler header.
enum class Priority { high, normal, low };
