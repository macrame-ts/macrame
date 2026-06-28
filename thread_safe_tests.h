#pragma once

void run_thread_safe_tests();

// Runs a single fatal scenario by name (invoked in the --death subprocess).
void run_death_scenario(const char* name);
