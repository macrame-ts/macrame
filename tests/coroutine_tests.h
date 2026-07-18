#pragma once

// Coroutine-support tests. Compiles to an empty `run_coroutine_tests()` when the
// toolchain has no coroutines (`__cpp_impl_coroutine` undefined).
void run_coroutine_tests();
