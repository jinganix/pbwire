// Links the core statically: the binding compiles `libpbwire.a` in, so there is
// no dylib to find at run time (unlike the FFI hosts, which dlopen it).
//
// The archive is copied into OUT_DIR, which holds nothing else: `make all` also
// leaves a dylib next to it, and a search path pointing there could resolve to
// the dylib and need it at run time. Search paths and `-l static` from a build
// script propagate to the binaries that depend on this crate.
//
// The C struct codec (`bindings/c/runtime/pbw_struct.c`) is compiled here into
// its own static archive for the same reason: the in-place arm calls
// `pbw_struct_codec_*` directly, and the core archive does not contain them. No
// new cargo dependency: `CC` (default `cc`) and `AR` (default `ar`) are used
// through `std::process::Command`.
//
// The library directory is PBWIRE_LIB_DIR, else `<repo>/build` found by walking
// up from this crate until the checkout's include/pbwire.h turns up.
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let root = find_root();
    let dir = match std::env::var_os("PBWIRE_LIB_DIR") {
        Some(dir) => PathBuf::from(dir),
        None => root.join("build"),
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
    compile_struct_codec(&root, &out);
    println!("cargo:rerun-if-changed=build.rs");
}

/// Compiles the layout codec into `libpbw_struct.a` in OUT_DIR. The archive is
/// linked before the core so its `pbw_*` references resolve from the same
/// command line, and it propagates to every binary that links this crate.
fn compile_struct_codec(root: &Path, out: &Path) {
    let include = root.join("include");
    let runtime = root.join("bindings/c/runtime");
    let src = runtime.join("pbw_struct.c");
    let obj = out.join("pbw_struct.o");
    let lib = out.join("libpbw_struct.a");
    let cc = std::env::var("CC").unwrap_or_else(|_| "cc".to_string());
    let status = Command::new(&cc)
        .arg("-O2")
        .arg("-std=c99")
        .arg("-fPIC")
        .arg("-I")
        .arg(&include)
        .arg("-I")
        .arg(&runtime)
        .arg("-c")
        .arg(&src)
        .arg("-o")
        .arg(&obj)
        .status()
        .unwrap_or_else(|e| panic!("run {cc}: {e}"));
    assert!(status.success(), "{cc} failed on {}", src.display());
    let ar = std::env::var("AR").unwrap_or_else(|_| "ar".to_string());
    let status = Command::new(&ar)
        .arg("crs")
        .arg(&lib)
        .arg(&obj)
        .status()
        .unwrap_or_else(|e| panic!("run {ar}: {e}"));
    assert!(status.success(), "{ar} failed for {}", lib.display());
    println!("cargo:rustc-link-lib=static=pbw_struct");
    println!("cargo:rerun-if-changed={}", src.display());
    println!("cargo:rerun-if-changed={}", runtime.join("pbw_struct.h").display());
    println!("cargo:rerun-if-env-changed=CC");
    println!("cargo:rerun-if-env-changed=AR");
}

fn find_root() -> PathBuf {
    let mut dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    loop {
        if dir.join("include/pbwire.h").is_file() {
            return dir;
        }
        if !dir.pop() {
            panic!(
                "no pbwire checkout above {}; set PBWIRE_LIB_DIR",
                env!("CARGO_MANIFEST_DIR")
            );
        }
    }
}
