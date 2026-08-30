use std::path::PathBuf;
use std::process::Command;

use serde_json::Value;

#[test]
fn analyzes_checked_in_entropy_fixture_without_a_model() {
    let fixtures = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures");
    let output = Command::new(env!("CARGO_BIN_EXE_lmcc"))
        .arg(fixtures.join("sample.rs"))
        .arg("--lang")
        .arg("rust")
        .arg("--entropy-jsonl")
        .arg(fixtures.join("sample.jsonl"))
        .output()
        .expect("lmcc should run");
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let result: Value = serde_json::from_slice(&output.stdout).expect("valid output JSON");
    assert!((result["tau"].as_f64().unwrap() - 0.736).abs() < 1e-12);
    assert!((result["lmcc"].as_f64().unwrap() - 1.4).abs() < 1e-12);
    assert_eq!(result["total_branch"], 1);
    assert_eq!(result["total_comp_level"], 3);
    assert_eq!(result["alpha"], 0.8);
    assert_eq!(result["units"][0]["level"], 1);
    assert_eq!(result["units"][0]["branching"], 1);
    assert_eq!(result["units"][0]["children"][0]["level"], 2);
}

#[test]
fn realistic_rust_fixture_matches_full_golden_json() {
    let fixtures = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures");
    let output = Command::new(env!("CARGO_BIN_EXE_lmcc"))
        .arg(fixtures.join("realistic.rs"))
        .arg("--entropy-jsonl")
        .arg(fixtures.join("realistic.jsonl"))
        .output()
        .expect("lmcc should run");
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );

    let actual: Value = serde_json::from_slice(&output.stdout).expect("valid output JSON");
    let expected: Value = serde_json::from_str(include_str!("fixtures/realistic.expected.json"))
        .expect("valid golden JSON");
    assert_eq!(actual, expected);
}

#[test]
fn infers_cpp_and_analyzes_checked_in_entropy_fixture() {
    let fixtures = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures");
    let output = Command::new(env!("CARGO_BIN_EXE_lmcc"))
        .arg(fixtures.join("sample.cpp"))
        .arg("--entropy-jsonl")
        .arg(fixtures.join("sample_cpp.jsonl"))
        .output()
        .expect("lmcc should run");
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );

    let result: Value = serde_json::from_slice(&output.stdout).expect("valid output JSON");
    assert!((result["tau"].as_f64().unwrap() - 0.535).abs() < 1e-12);
    assert_eq!(result["alpha"], 0.8);
    assert!(
        result["units"]
            .as_array()
            .is_some_and(|units| !units.is_empty())
    );
}

#[test]
fn models_list_reports_only_cached_gguf_files() {
    let cache = tempfile::tempdir().unwrap();
    std::fs::write(cache.path().join("alpha.gguf"), vec![0_u8; 1024]).unwrap();
    std::fs::write(cache.path().join("beta.gguf"), vec![0_u8; 2048]).unwrap();
    std::fs::write(cache.path().join("ignored.txt"), b"not a model").unwrap();
    std::fs::write(
        cache.path().join("models.json"),
        br#"{
            "alpha.gguf": {"downloaded_at": 0, "last_used_at": 86400}
        }"#,
    )
    .unwrap();

    let output = Command::new(env!("CARGO_BIN_EXE_lmcc"))
        .args(["models", "list"])
        .env("LMCC_CACHE_DIR", cache.path())
        .output()
        .expect("lmcc models list should run");

    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let stdout = String::from_utf8(output.stdout).unwrap();
    assert!(
        stdout.contains("alpha.gguf\t0.00 GiB\t1970-01-01 00:00:00 UTC\t1970-01-02 00:00:00 UTC")
    );
    assert!(stdout.contains("beta.gguf\t0.00 GiB\tnever\tnever"));
    assert!(!stdout.contains("ignored.txt"));
}

#[test]
fn models_remove_deletes_model_partial_and_manifest_entry() {
    let cache = tempfile::tempdir().unwrap();
    std::fs::write(cache.path().join("remove.gguf"), b"model").unwrap();
    std::fs::write(cache.path().join("remove.gguf.partial"), b"partial").unwrap();
    std::fs::write(cache.path().join("keep.gguf"), b"keep").unwrap();
    std::fs::write(
        cache.path().join("models.json"),
        br#"{
            "remove.gguf": {"downloaded_at": 1, "last_used_at": 2},
            "keep.gguf": {"downloaded_at": 3}
        }"#,
    )
    .unwrap();

    let output = Command::new(env!("CARGO_BIN_EXE_lmcc"))
        .args(["models", "remove", "remove.gguf"])
        .env("LMCC_CACHE_DIR", cache.path())
        .output()
        .expect("lmcc models remove should run");

    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(!cache.path().join("remove.gguf").exists());
    assert!(!cache.path().join("remove.gguf.partial").exists());
    assert!(cache.path().join("keep.gguf").exists());
    let manifest: Value =
        serde_json::from_slice(&std::fs::read(cache.path().join("models.json")).unwrap()).unwrap();
    assert!(manifest.get("remove.gguf").is_none());
    assert!(manifest.get("keep.gguf").is_some());
}

#[test]
fn models_remove_rejects_paths() {
    let cache = tempfile::tempdir().unwrap();
    let output = Command::new(env!("CARGO_BIN_EXE_lmcc"))
        .args(["models", "remove", "../model.gguf"])
        .env("LMCC_CACHE_DIR", cache.path())
        .output()
        .expect("lmcc models remove should run");

    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("bare file name"));
}

#[test]
fn models_path_prints_the_environment_override() {
    let cache = tempfile::tempdir().unwrap();
    let output = Command::new(env!("CARGO_BIN_EXE_lmcc"))
        .args(["models", "path"])
        .env("LMCC_CACHE_DIR", cache.path())
        .output()
        .expect("lmcc models path should run");

    assert!(output.status.success());
    assert_eq!(
        String::from_utf8(output.stdout).unwrap().trim(),
        cache.path().to_string_lossy()
    );
}
