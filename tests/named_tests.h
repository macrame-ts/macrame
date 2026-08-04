#pragma once

// `ts::Named` tests: literal vs call-site identity, the display form, and -- the
// load-bearing one -- that a captured site points at USER code, not a library header.
void run_named_tests();
