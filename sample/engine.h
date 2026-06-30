#pragma once

namespace sample
{

// Run the mock engine for `frames` frames at the given time scale (1.0 = real
// AAA @ 60 fps budgets). Prints average frame time vs the serial baseline.
void run_sample(int frames = 20, float time_scale = 1.0f);

} // namespace sample
