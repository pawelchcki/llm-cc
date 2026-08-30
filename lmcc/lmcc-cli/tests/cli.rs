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
