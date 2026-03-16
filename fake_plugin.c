/**
 * fake_plugin.c - Simulates the LLVM gold plugin message() calls
 *
 * Reproduces the exact pattern from issue #1572 where the LLVM gold plugin
 * calls message() with varargs. Before the fix, these appear as:
 *   "Linker plugin message: LLVM gold plugin: %s"
 * After the fix:
 *   "Linker plugin message: LLVM gold plugin: <actual text>"
 *
 * Build:  gcc -shared -fPIC -o fake_plugin.so fake_plugin.c
 * Test:   wild --plugin=./fake_plugin.so dummy.o -o /dev/null
 */

#include <stdio.h>
#include <stdint.h>

typedef enum {
    LDPL_INFO    = 0,
    LDPL_WARNING = 1,
    LDPL_ERROR   = 2,
    LDPL_FATAL   = 3,
} ld_plugin_level;

typedef int (*ld_plugin_message)(ld_plugin_level level, const char *format, ...);

typedef struct {
    uint32_t  tag;
    uintptr_t value;
} ld_plugin_tv;

#define LDPT_MESSAGE 11

void onload(ld_plugin_tv *tv) {
    ld_plugin_message message_fn = NULL;

    for (; tv->tag != 0; tv++) {
        if (tv->tag == LDPT_MESSAGE) {
            message_fn = (ld_plugin_message)(uintptr_t)tv->value;
            break;
        }
    }

    if (!message_fn) {
        fprintf(stderr, "fake_plugin: message callback not found in transfer vector\n");
        return;
    }

    /* --- Exact pattern reported in issue #1572 ---
     * LLVM gold plugin calls message() like:
     *   message(LDPL_INFO,    "LLVM gold plugin: %s", info_str)
     *   message(LDPL_WARNING, "LLVM gold plugin: %s", warn_str)
     *
     * Old output (broken):
     *   Linker plugin message: LLVM gold plugin: %s
     *   Linker plugin warning: LLVM gold plugin: %s
     *
     * New output (fixed):
     *   Linker plugin message: LLVM gold plugin: LTO module loaded successfully
     *   Linker plugin warning: LLVM gold plugin: weak symbol 'foo' has multiple definitions
     */
    message_fn(LDPL_INFO,
               "LLVM gold plugin: %s",
               "LTO module loaded successfully");

    message_fn(LDPL_WARNING,
               "LLVM gold plugin: %s",
               "weak symbol 'foo' has multiple definitions");

    message_fn(LDPL_ERROR,
               "LLVM gold plugin has failed to create LTO module: %s",
               "Invalid bitcode signature in /lib/libWebCore.so");

    message_fn(LDPL_FATAL,
               "Cannot read IR for file '%s': %s (errno %d)",
               "/tmp/bad.o", "No file descriptors available", 24);
}
