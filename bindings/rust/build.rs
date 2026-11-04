// Links the core statically: the binding compiles `libpbwire.a` in, so there is
// no dylib to find at run time (unlike the FFI hosts, which dlopen it).
//
// The archive is copied into OUT_DIR, which holds nothing else: `make all` also
// leaves a dylib next to it, and a search path pointing there could resolve to
// the dylib and need it at run time. Search paths and `-l static` from a build
// script propagate to the binaries that depend on this crate.
//
// The library directory is PBWIRE_LIB_DIR, else `<repo>/build` found by walking
// up from this crate until the checkout's include/pbwire.h turns up.
use std::path::PathBuf;

fn main() {
    let dir = match std::env::var_os("PBWIRE_LIB_DIR") {
        Some(dir) => PathBuf::from(dir),
        None => find_build_dir(),
    };
    let archive = dir.join("libpbwire.a");
    let out = PathBuf::from(std::env::var_os("OUT_DIR").expect("OUT_DIR"));
    std::fs::copy(&archive, out.join("libpbwire.a")).unwrap_or_else(|e| {
        panic!("copy {}: {e} (run `make` in the repo root)", archive.display())
    });
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=pbwire");
    println!("cargo:rerun-if-env-changed=PBWIRE_LIB_DIR");
    println!("cargo:rerun-if-changed={}", archive.display());
    println!("cargo:rerun-if-changed=build.rs");
}

fn find_build_dir() -> PathBuf {
    let mut dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    loop {
        if dir.join("include/pbwire.h").is_file() {
            return dir.join("build");
        }
        if !dir.pop() {
            panic!(
                "no pbwire checkout above {}; set PBWIRE_LIB_DIR",
                env!("CARGO_MANIFEST_DIR")
            );
        }
    }
}
