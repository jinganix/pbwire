// Generates the prost baseline (the fastest widely used Rust runtime) from the
// same matrix3.proto the rust-protobuf baseline uses. Runs at build time, so
// bench.sh needs nothing beyond protoc and crates.io.
fn main() {
    prost_build::compile_protos(
        &["../../c/tests/data/matrix3.proto"],
        &["../../c/tests/data/"],
    )
    .expect("prost_build");
}
