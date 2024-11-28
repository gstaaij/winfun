#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "src/nob.h"

#include <stdbool.h>
#include <stdint.h>

#define CMD_CC_32BIT(cmd) cmd_append((cmd), "i686-w64-mingw32-gcc")
#define CMD_CC_64BIT(cmd) cmd_append((cmd), "x86_64-w64-mingw32-gcc")
#if INTPTR_MAX == INT64_MAX
    #define IS_64BIT true
#elif INTPTR_MAX == INT32_MAX
    #define IS_64BIT false
#endif
#define CMD_CFLAGS(cmd) cmd_append((cmd), "-Wall", "-Wextra", "-Wswitch-enum", "-static", "-isystem:./winver.h")
#define CMD_FILE(cmd, name) cmd_append((cmd), "-o", temp_sprintf("./build/%s", (name)), temp_sprintf("./src/%s.c", (name)))

const char* files[] = {
    "changefont",
};

void log_usage(Log_Level level, const char* program) {
    nob_log(level, "Usage: %s [options]");
}

void log_options(Log_Level level) {
    nob_log(level, "Available options:");
    nob_log(level, "  --bitness 32|64   Sets the target bitness");
}

int main(int argc, char** argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);

    const char* program = shift(argv, argc);

    bool target_64bit = IS_64BIT;
    // Parse the options
    while (argc > 0) {
        const char* option = shift(argv, argc);
        if (strcmp(option, "--bitness") == 0) {
            if (argc < 1) {
                log_usage(ERROR, program);
                nob_log(ERROR, "Missing bitness value");
                return 1;
            }

            const char* bitness = shift(argv, argc);
            if (strcmp(bitness, "64") == 0) {
                target_64bit = true;
            } else if (strcmp(bitness, "32") == 0) {
                target_64bit = false;
            } else {
                log_usage(ERROR, program);
                nob_log(ERROR, "Invalid bitness value");
                return 1;
            }
        } else if (strcmp(option, "--help") == 0) {
            log_usage(INFO, program);
            log_options(INFO);
            return 0;
        } else {
            log_usage(ERROR, program);
            log_options(ERROR);
            nob_log(ERROR, "Invalid option %s", option);
            return 1;
        }
    }

    mkdir_if_not_exists("./build");

    Cmd cmd = {0};

    for (size_t i = 0; i < ARRAY_LEN(files); ++i) {
        if (target_64bit) CMD_CC_64BIT(&cmd);
        else              CMD_CC_32BIT(&cmd);
        CMD_CFLAGS(&cmd);
        CMD_FILE(&cmd, files[i]);
        if (!cmd_run_sync_and_reset(&cmd)) return 1;
        temp_reset();
    }

    return 0;
}
