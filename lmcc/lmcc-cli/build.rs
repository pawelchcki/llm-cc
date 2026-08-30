use std::fs;
use std::path::PathBuf;

fn main() {
    let version_file = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../VERSION");
    println!("cargo:rerun-if-changed={}", version_file.display());
    let version = fs::read_to_string(&version_file)
        .unwrap_or_else(|error| panic!("failed to read {}: {error}", version_file.display()));
    let version = version.trim();
    assert!(!version.is_empty(), "VERSION must not be empty");
    assert_eq!(
        version,
        env!("CARGO_PKG_VERSION"),
        "Cargo metadata is out of sync; run tools/sync_version.sh from the repository root"
    );
    println!("cargo:rustc-env=RETHINK_VERSION={version}");
}
