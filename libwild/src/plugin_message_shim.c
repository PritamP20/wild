#include <stdio.h>
#include <stdarg.h>

// Forward declaration to Rust function (returns Status which is #[repr(C)] int)
extern int plugin_message_handler(int level, const char* formatted);

// C shim that accepts variadic arguments and formats them
int plugin_message_impl(int level, const char* format, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    // Call the Rust handler with the formatted message
    return plugin_message_handler(level, buffer);
}
