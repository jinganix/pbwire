# Linux bench node: the same `make *-bench` harnesses as the host, in a
# container, so a Linux run compares row by row with the macOS tables in
# BENCHMARKS.md. Driven by scripts/bench-docker.sh (`make bench-docker`).
#
# Covered: core (C++ + protobuf), C structs, Go, Python, Rust, Node, Ruby, Lua,
# Java. Not covered on purpose: Objective-C and Swift need Apple toolchains,
# PHP needs the official C extension as its baseline (Ubuntu packages none, and
# building it from source needs a generated utf8_range amalgamation that only
# protobuf's own bazel/cmake build produces -- a pure-PHP-only baseline would
# be a ~300x different comparison), and the toolchains this image does not
# install (Dart, Zig, C#/F#, Julia, R, Erlang/Elixir, Kotlin/Scala/Clojure)
# run on the host only.
FROM ubuntu:24.04 AS bench

# Same pin as the Makefile (PROTOBUF_VERSION), so the generated baseline code
# matches the macOS tables in BENCHMARKS.md.
ARG PROTOBUF_VERSION=v36.2
ARG GO_VERSION=1.27.3
ARG PROTOC_GEN_GO_VERSION=v1.36.12
ARG PROTOBUF_CODEGEN_VERSION=3.7.2
ARG PROTOBUF_JAVA_VERSION=4.36.2

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config git curl ca-certificates \
        python3 python3-dev python3-pip \
        ruby-full \
        lua5.4 liblua5.4-dev \
        default-jdk-headless \
        nodejs npm libnode-dev \
    && rm -rf /var/lib/apt/lists/*

# protobuf from source: the baseline columns of every C++/C bench come from
# protoc, so its version has to be the pinned one, not Ubuntu's.
RUN git clone --depth 1 --branch "$PROTOBUF_VERSION" \
        https://github.com/protocolbuffers/protobuf /opt/protobuf \
    && cd /opt/protobuf && git submodule update --init --recursive \
    && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
        -Dprotobuf_BUILD_SHARED_LIBS=ON -Dprotobuf_BUILD_TESTS=OFF \
    && cmake --build build -j "$(nproc)" \
    && cmake --install build \
    && ldconfig

# Rust: the bench compares against rust-protobuf and prost, and generates the
# rust-protobuf side locally with protoc-gen-rs.
RUN curl -sSf https://sh.rustup.rs \
        | sh -s -- -y --profile minimal --default-toolchain stable \
    && /root/.cargo/bin/cargo install protobuf-codegen --version "$PROTOBUF_CODEGEN_VERSION"

# Go from go.dev, not apt: Ubuntu 24.04 ships 1.22, and the binding's go.mod
# needs 1.23+ (the go command then refuses to switch toolchains). The bench
# generates the protobuf-go side with protoc-gen-go.
RUN goarch=$(dpkg --print-architecture) \
    && curl -fsSL -o /tmp/go.tar.gz "https://go.dev/dl/go$GO_VERSION.linux-$goarch.tar.gz" \
    && tar -C /usr/local -xzf /tmp/go.tar.gz \
    && rm /tmp/go.tar.gz \
    && ln -sfn /usr/local/go/bin/go /usr/local/bin/go \
    && ln -sfn /usr/local/go/bin/gofmt /usr/local/bin/gofmt \
    && go install "google.golang.org/protobuf/cmd/protoc-gen-go@$PROTOC_GEN_GO_VERSION"

# Baseline runtimes fetched from the package managers, like the harnesses do on
# the host; PEP 668 is turned off because this image is the only user.
RUN gem install google-protobuf --no-document \
    && python3 -m pip install --break-system-packages --quiet protobuf

# Java baseline jar: baked in so the Java bench does no network I/O (it picks
# PROTOBUF_JAVA_JAR up when it exists, see scripts/bench-docker.sh).
RUN mkdir -p /opt/deps && curl -fsSL -o "/opt/deps/protobuf-java-$PROTOBUF_JAVA_VERSION.jar" \
    "https://repo1.maven.org/maven2/com/google/protobuf/protobuf-java/$PROTOBUF_JAVA_VERSION/protobuf-java-$PROTOBUF_JAVA_VERSION.jar"

ENV PATH=/usr/local/go/bin:/root/.cargo/bin:/root/go/bin:$PATH \
    LD_LIBRARY_PATH=/usr/local/lib

# Second stage: the benchmark source under /work, one layer, so a run needs no
# bind mount at all (containers that share no host paths -- Colima, remote or
# CI daemons -- would silently mount an empty directory instead). The heavy
# directories are filtered out by .dockerignore; the harnesses regenerate what
# they need, and .git stays in because bench-record.py reads the sha from it.
FROM bench AS source
COPY . /work
