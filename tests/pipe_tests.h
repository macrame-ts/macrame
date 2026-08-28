#pragma once

// Pipe reader/writer protocol tests (see docs/internals/pipe-rebase-tests.md). Groups A (invariant),
// C (reader-group / sentinel behaviour), D (lone-reader elision), G (lifetime), H
// (worker-less), J (priority/cancel). Wave-1 (black-box) tests here pass on the current
// pipe and guard the rebase; Wave-2 (white-box) tests are added with the rewrite.
void run_pipe_tests();
