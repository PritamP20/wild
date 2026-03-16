# Dockerfile for testing plugin message formatting improvements
# docker build -t wild-plugin-test . -f test_plugin_messages.Dockerfile
# docker run -it wild-plugin-test

FROM ubuntu:24.04

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    clang \
    gcc \
    g++ \
    lld \
    curl \
    git \
    pkg-config \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Install Rust
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
ENV PATH="/root/.cargo/bin:${PATH}"

# Set up working directory
WORKDIR /wild

# Copy the project
COPY . .

# Build the project with plugins
RUN cargo build --features plugins --release

# Run our comprehensive tests
CMD ["/bin/bash", "./test_plugin_messages.sh"]