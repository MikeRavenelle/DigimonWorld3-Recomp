#include "process_runner.h"

#include <SDL3/SDL.h>

namespace dw3setup {

int run_process(const std::vector<std::string>& command, const std::atomic<bool>& cancel) {
    std::vector<const char*> argv;
    for (const std::string& a : command) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_Process* process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!process) return kProcessNotStarted;
    int exit_code = 0;
    while (!SDL_WaitProcess(process, false, &exit_code)) {
        if (cancel.load()) {
            SDL_KillProcess(process, true);
            SDL_WaitProcess(process, true, &exit_code);
            SDL_DestroyProcess(process);
            return kProcessCancelled;
        }
        SDL_Delay(50);
    }
    SDL_DestroyProcess(process);
    return exit_code;
}

}  // namespace dw3setup
