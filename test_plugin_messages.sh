#!/bin/bash
set -e

cargo build --features plugins --release -q

gcc -shared -fPIC -o fake_plugin.so fake_plugin.c

cat > /tmp/wild_test_main.c << 'EOF'
int main(void) { return 0; }
EOF
# -flto=thin produces an LLVM bitcode object, which triggers kind.is_compiler_ir()
# and causes wild to pass the file to the plugin, loading it and calling onload.
clang -flto=thin -c /tmp/wild_test_main.c -o /tmp/wild_test_main.o

./target/release/wild --plugin=./fake_plugin.so /tmp/wild_test_main.o -o /dev/null 2>&1 || true
