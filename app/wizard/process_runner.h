// Runs a child process to completion with SDL3, with the ability to stop it. No console output is shown.
#pragma once
#include <atomic>
#include <string>
#include <vector>

namespace dw3setup {

constexpr int kProcessNotStarted = -1;
constexpr int kProcessCancelled = -2;

// Returns the exit code, kProcessNotStarted if the program cannot be started (for instance, it is not installed), or
// kProcessCancelled after `cancel` became true and the process was killed.
int run_process(const std::vector<std::string>& command, const std::atomic<bool>& cancel);

}  // namespace dw3setup
