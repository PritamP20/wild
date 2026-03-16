# Fix: Allow Linker Plugins to Print Full Log Messages (Issue #1662)

---

## The Problem

When building large projects with LTO (Link Time Optimization), the LLVM gold plugin calls
back into the linker to report diagnostic messages using a printf-style variadic API:

```c
message(LDPL_ERROR, "LLVM gold plugin has failed to create LTO module: %s", error_details);
message(LDPL_INFO,  "LLVM gold plugin: %s", info_string);
```

Before this fix, Wild only printed the raw format string — all extra arguments were silently
dropped:

```
Linker plugin error: LLVM gold plugin has failed to create LTO module: %s
Linker plugin message: LLVM gold plugin: %s
```

Instead of the correct output:

```
Linker plugin error: LLVM gold plugin has failed to create LTO module: Invalid bitcode signature
Linker plugin message: LLVM gold plugin: LTO module loaded successfully
```

---

## Root Cause

### What the Plugin API looks like in C

The GNU gold plugin API defines the message callback type as:

```c
typedef int (*ld_plugin_message)(ld_plugin_level level, const char *format, ...);
```

The `...` means it is variadic — the plugin passes a format string plus any number of extra
arguments, exactly like `printf`. When the plugin calls `message(level, "Error: %s", details)`,
the C ABI pushes `level`, a pointer to the format string, and then `details` onto the stack.

### What the old Rust function looked like

```rust
extern "C" fn message(level: libc::c_int, format: *const libc::c_char) -> Status {
    let format_str = unsafe { CStr::from_ptr(format) };
    println!("Linker plugin {level}: {}", format_str.to_string_lossy());
    Status::Ok
}
```

This function only accepted two parameters — `level` and `format`. When the plugin called
`message(level, "Error: %s", details)`, the `details` value was pushed onto the stack by the
plugin's C code but the Rust function never read it. It printed `"Error: %s"` literally.

### Why Rust cannot fix this natively (on stable)

To properly handle variadic arguments from C you need to:
1. Accept `...` in your function signature
2. Capture the extra arguments using `va_list` / `VaList`
3. Pass them to `vsnprintf` to format the complete string

In Rust, implementing a function that accepts `...` requires the `c_variadic` feature which is
not stable as of early 2026. Wild must compile on stable Rust. Even on nightly, forwarding
varargs to `vsnprintf` via Rust's `VaList` has compatibility issues — mati865 tested this and
found it produced corrupted bytes instead of the formatted values. The `va_list` crate was also
evaluated but cannot forward a captured `va_list` to `vsnprintf`.

The only clean solution that works on stable Rust is a **C shim**.

---

## The Fix: 5 Files Changed

---

### FILE 1 — `libwild/src/plugin_message_shim.c` (NEW)

This is the core of the entire fix. A 17-line C file that acts as the bridge between the
plugin's variadic call and Rust.

```c
#include <stdio.h>
```
`stdio.h` provides `vsnprintf` — the standard C function that formats a string using a
`va_list` into a buffer. Without this header the compiler would not know the signature of
`vsnprintf`.

```c
#include <stdarg.h>
```
`stdarg.h` provides the four macros needed to work with variadic arguments: `va_list` (the
type), `va_start` (begin reading args), `va_end` (clean up), and `va_arg` (read one arg). We
use three of these four.

```c
extern int plugin_message_handler(int level, const char* formatted);
```
A forward declaration — it tells the C compiler "there is a function called
`plugin_message_handler` that takes an `int` and a `const char*` and returns `int`, but it is
defined somewhere else (in the Rust binary)". Without this declaration the compiler would refuse
to compile the call on line 16. The `extern` keyword is what says "defined elsewhere". The
`int` return type matches Rust's `Status` enum which is `#[repr(C)]` so on the C side it is
just an `int`.

```c
int plugin_message_impl(int level, const char* format, ...) {
```
This is the function Wild registers as the message callback in the transfer vector. Plugins call
this when they want to print a message. The `int level` is the severity (0=info, 1=warn,
2=error, 3=fatal). `const char* format` is the printf-style format string. The `...` means this
function accepts any number of additional arguments after `format` — this is standard C variadic
syntax. The function must be in C (not Rust) because C natively supports variadic functions.

```c
    char buffer[4096];
```
Allocates a 4096-byte buffer on the stack to hold the formatted output. Stack allocation means
no `malloc`/`free` needed and no risk of memory leak. 4096 bytes is large enough to hold any
realistic plugin diagnostic message. If a message exceeds this length `vsnprintf` will
truncate it safely.

```c
    va_list args;
```
Declares a variable of type `va_list`. This is a special opaque type defined by `stdarg.h` that
holds the internal state for reading variadic arguments — essentially a pointer into the stack
frame where the extra arguments live.

```c
    va_start(args, format);
```
`va_start` is a macro that initialises `args` to point at the first variadic argument. The
second parameter (`format`) must be the last named parameter before the `...` — this is how
`va_start` knows where the varargs begin on the stack. After this line, `args` is ready to be
passed to `vsnprintf`.

```c
    vsnprintf(buffer, sizeof(buffer), format, args);
```
This is the key line. `vsnprintf` is like `sprintf` (formats into a string) but:
- Takes a `va_list` instead of `...` so it can be called from variadic wrapper functions
- The second argument `sizeof(buffer)` caps the output at 4096 bytes preventing overflow
- `format` is the format string (e.g. `"LLVM gold plugin: %s"`)
- `args` is the captured variadic arguments

After this call, `buffer` contains the fully substituted string, for example:
`"LLVM gold plugin: Invalid bitcode signature in /lib/libWebCore.so"`

```c
    va_end(args);
```
Required cleanup macro. Every `va_start` must be paired with a `va_end`. It releases internal
resources and on some platforms resets the stack pointer. Omitting this is undefined behaviour.

```c
    return plugin_message_handler(level, buffer);
```
Calls back into Rust with the fully formatted string. `plugin_message_handler` is the Rust
function (declared on line 5). `level` is passed through unchanged. `buffer` is a pointer to
the stack-allocated formatted string — this is safe because `plugin_message_handler` finishes
before this function returns and `buffer` is still in scope.

---

### FILE 2 — `libwild/build.rs` (NEW)

Cargo runs `build.rs` automatically before compiling the package. This file is how we compile
the C shim into a static library that gets linked into Wild.

```rust
fn main() {
```
The entry point of the build script. Cargo runs this and uses any output (printed to stdout in
special `cargo:` format) to configure the build.

```rust
    if cfg!(feature = "plugins") {
```
`cfg!(feature = "plugins")` evaluates to `true` only when Cargo was invoked with
`--features plugins`. This is important for two reasons: (1) users who do not use plugins do
not incur the cost of compiling C code, and (2) the symbols `plugin_message_impl` and
`plugin_message_handler` are only defined in the `plugins` feature gate, so linking the C shim
without the feature would produce linker errors.

```rust
        cc::Build::new()
```
Creates a new compilation job using the `cc` crate. The `cc` crate auto-detects the C compiler
available on the system (gcc, clang, msvc, etc.) and constructs the correct compiler invocation.

```rust
            .file("src/plugin_message_shim.c")
```
Specifies the C source file to compile. The path is relative to the `libwild/` directory
(where `build.rs` lives). This is the shim file we created above.

```rust
            .compile("plugin_message_shim");
```
Compiles the C file and produces a static library named `libplugin_message_shim.a`. The `cc`
crate also emits the correct `cargo:rustc-link-lib=static=plugin_message_shim` directive so
Cargo automatically links the static library into the final Wild binary. This is what makes the
`#[link(name = "plugin_message_shim", kind = "static")]` attribute in Rust resolve successfully.

---

### FILE 3 — `libwild/src/linker_plugins.rs` (MODIFIED)

Three separate changes were made in this file.

#### Change A — FFI declaration block (lines 994–1000)

```rust
#[link(name = "plugin_message_shim", kind = "static")]
unsafe extern "C" {
    fn plugin_message_impl(level: libc::c_int, format: *const libc::c_char, ...) -> Status;
}
```

`#[link(name = "plugin_message_shim", kind = "static")]` — tells rustc to link the static
library `libplugin_message_shim.a` when compiling this file. `kind = "static"` explicitly
requests a static link so the C code is baked into the Wild binary rather than depending on
a separate `.so` at runtime. This attribute redundantly reinforces what `build.rs` already
declares but makes the dependency explicit at the source level.

`unsafe extern "C"` — declares a block of foreign function interfaces. `extern "C"` means
the functions inside use the C ABI (C calling convention, C name mangling rules). `unsafe`
is required on the block because Rust cannot verify the safety of external functions — the
programmer is asserting they know the signatures are correct.

`fn plugin_message_impl(level: libc::c_int, format: *const libc::c_char, ...) -> Status;` —
declares the C shim function so Rust code can call it. `libc::c_int` is Rust's type alias for
C's `int` (typically 32-bit signed). `*const libc::c_char` is Rust's equivalent of `const
char*` — a raw pointer to a C string. The `...` at the end makes this a variadic function
declaration — this is valid in Rust for `extern "C"` function declarations (calling variadic C
functions), even though implementing a variadic function in Rust requires nightly. `-> Status`
is the return type; `Status` is a `#[repr(C)]` Rust enum so it is ABI-compatible with `int`.

#### Change B — Rust callback function (lines 1002–1022)

```rust
#[allow(dead_code)]
```
Suppresses the Rust compiler warning "function is never called". From Rust's perspective nothing
in the Rust codebase calls `plugin_message_handler` directly — only the C shim calls it — so
the compiler wrongly infers it is unused. This attribute silences that warning.

```rust
#[unsafe(no_mangle)]
```
Prevents Rust from applying its name mangling to this function. Without this attribute Rust
would export the function with a mangled name like
`_ZN7libwild14linker_plugins22plugin_message_handler17h3a5f9b2c1d4e8f6gE` which the C shim
cannot find. With this attribute the function is exported as literally `plugin_message_handler`
— which matches the `extern int plugin_message_handler(...)` declaration in the C shim.
`unsafe(...)` syntax (as opposed to just `#[no_mangle]`) is required in Rust editions 2024+.

```rust
extern "C" fn plugin_message_handler(level: libc::c_int, formatted: *const libc::c_char) -> Status {
```
`extern "C"` makes this function use the C calling convention so the C shim can call it as a
normal C function. `level` is the severity integer. `formatted` is a raw pointer to the
null-terminated string that `vsnprintf` wrote into `buffer` in the C shim. `-> Status` is the
return type that maps to `int` on the C side.

```rust
    catch_panics(|| {
```
Wraps the entire body in Wild's panic catcher. If any Rust code inside panics, `catch_unwind`
catches it and returns `Status::Err` instead of unwinding through C stack frames (which would
be undefined behaviour because C does not know about Rust panics).

```rust
        let Some(level) = MessageLevel::from_raw(level) else {
            return Status::Err;
        };
```
Converts the raw C integer into Wild's `MessageLevel` enum. `from_raw` returns `None` if the
integer is not 0, 1, 2, or 3. If a plugin passes an unrecognised level we return an error
status rather than panicking.

```rust
        let formatted = unsafe { CStr::from_ptr(formatted) };
```
Converts the raw C string pointer into Rust's `CStr` type. `CStr::from_ptr` reads bytes from
the pointer until it finds a null terminator. The `unsafe` block is required because this
dereferences a raw pointer — Rust cannot statically verify the pointer is valid and
null-terminated. We trust the C shim to always pass a valid string (it always does because
`vsnprintf` always null-terminates its output).

```rust
        if level == MessageLevel::Error || level == MessageLevel::Fatal {
            println!("Linker plugin {level}: {}", formatted.to_string_lossy());
            ERROR_MESSAGE.replace(Some(formatted.to_string_lossy().to_string()));
        } else {
            println!("Linker plugin {level}: {}", formatted.to_string_lossy());
        }
```
`to_string_lossy()` converts the C string to a Rust `&str`, safely replacing any non-UTF-8
bytes with the `?` character. For `Error` and `Fatal` severity levels the message is also
stored in `ERROR_MESSAGE` (a thread-local `Cell`) so Wild can later surface it as the linker's
error result and propagate it to the user. For `Info` and `Warning` levels it is only printed.

#### Change C — Transfer vector registration (lines 378–384)

The transfer vector is an array of tag-value pairs that Wild passes to the plugin's `onload`
function. Each entry is an `LdPluginTv { tag: u32, value: usize }`. The plugin walks this array
at startup to discover what callbacks the linker is offering. Tag 11 (`LDPT_MESSAGE`) identifies
the message callback.

```rust
// Before — registered a plain non-variadic Rust function:
transfer_vector.push(LdPluginTv::fn_ptr2(Tag::Message, message));

// After — registers the C shim which handles varargs:
transfer_vector.push(LdPluginTv {
    tag: Tag::Message as u32,
    value: plugin_message_impl as *const extern "C" fn(libc::c_int, *const libc::c_char) as usize,
});
```

`tag: Tag::Message as u32` — `Tag::Message = 11` in the enum. Casting to `u32` gives the raw
integer `11` that goes into the struct. The plugin checks `tv->tag == 11` to identify this slot.

`value: plugin_message_impl as *const extern "C" fn(...) as usize` — gets the address of the
C shim function `plugin_message_impl` and stores it as a raw integer. The plugin later casts
this integer back to a function pointer and calls it. The intermediate cast to `*const extern
"C" fn(...)` is needed because Rust requires an explicit type-erased pointer before converting
to `usize`.

We cannot use the existing `fn_ptr2` helper because `plugin_message_impl` is variadic. Rust's
type system treats variadic and non-variadic function pointers as distinct types, so the
generic helper would not accept the variadic signature. We build the `LdPluginTv` struct
manually instead.

---

### FILE 4 — `Cargo.toml` (workspace root) (MODIFIED)

```toml
cc = "1.0"
```
Added under `[workspace.dependencies]`. This registers the `cc` crate at version `1.0` in the
workspace dependency table. Any package in the workspace can then reference it with
`{ workspace = true }` without repeating the version number. The `cc` crate is the standard
Rust tool for invoking C compilers from `build.rs`.

---

### FILE 5 — `libwild/Cargo.toml` (MODIFIED)

```toml
[build-dependencies]
cc = { workspace = true }
```
`[build-dependencies]` is a special Cargo section for dependencies that are only needed during
the build step — i.e. code used in `build.rs`. These are not compiled into the library itself.
`cc = { workspace = true }` pulls in the `cc` version defined in the workspace root.

---

## How All Pieces Connect at Runtime

```
Plugin code calls (C):
    message(LDPL_ERROR, "Failed to open: %s", "/lib/foo.so")
         |
         | plugin reads tag=11 from transfer vector, casts value to function pointer
         v
plugin_message_impl(LDPL_ERROR, "Failed to open: %s", "/lib/foo.so")
  [plugin_message_shim.c — compiled C code]
    va_start captures "/lib/foo.so"
    vsnprintf writes "Failed to open: /lib/foo.so" into buffer
    va_end cleans up
         |
         | direct C function call to Rust export
         v
plugin_message_handler(LDPL_ERROR, "Failed to open: /lib/foo.so")
  [linker_plugins.rs — Rust code]
    from_raw(2) → MessageLevel::Error
    CStr::from_ptr → &str "Failed to open: /lib/foo.so"
    println! → "Linker plugin error: Failed to open: /lib/foo.so"
    ERROR_MESSAGE.replace(Some("Failed to open: /lib/foo.so"))
```

---

## Important Detail: Lazy Plugin Loading

The plugin is NOT loaded when Wild parses `--plugin=foo.so`. Wild creates a
`Store::Unloaded` entry instead. The plugin's `onload` function is only called the **first
time Wild encounters an LTO input file** (a file where `kind.is_compiler_ir()` returns true).

This means:
- A `.o` compiled with `gcc -c` → regular ELF object → plugin never loaded → no messages
- A `.o` compiled with `clang -flto=thin -c` → LLVM bitcode object → `is_compiler_ir()` is
  true → Wild calls `process_input` → `loaded()` is called → plugin loaded → `onload` runs →
  our message callback is registered → plugin messages work

This is why the test script uses `clang -flto=thin -c` and not `gcc -c`.

---

## Before vs After

```
Plugin calls:   message(ERROR, "LLVM gold plugin: %s", "Invalid bitcode")

BEFORE fix:     Linker plugin error: LLVM gold plugin: %s
AFTER fix:      Linker plugin error: LLVM gold plugin: Invalid bitcode
```

```
Plugin calls:   message(FATAL, "Cannot read '%s': %s (errno %d)", "/tmp/a.o", "No fds", 24)

BEFORE fix:     Linker plugin fatal error: Cannot read '%s': %s (errno %d)
AFTER fix:      Linker plugin fatal error: Cannot read '/tmp/a.o': No fds (errno 24)
```
