fn main() {
    // Only compile the C shim when the "plugins" feature is enabled
    if cfg!(feature = "plugins") {
        cc::Build::new()
            .file("src/plugin_message_shim.c")
            .compile("plugin_message_shim");
    }
}
