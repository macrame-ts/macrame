#pragma once

// Runs the full test suite (all groups), reporting through the harness.
void run_all_tests();

// Runs a single fatal scenario by name (invoked in the --death subprocess).
void run_death_scenario(const char* name);
