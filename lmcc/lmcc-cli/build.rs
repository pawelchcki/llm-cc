use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    for relative in ["../../VERSION", "../../tools/version.sh"] {
        println!(
            "cargo:rerun-if-changed={}",
            manifest_dir.join(relative).display()
        );
    }
    // `git rev-parse --git-path` resolves these through worktrees and submodules,
    // where `.git` is a file rather than a directory.
    for name in ["HEAD", "index"] {
        let Some(path) = git_path(&manifest_dir, name) else {
            continue;
        };
        if path.exists() {
            println!("cargo:rerun-if-changed={}", path.display());
        }
    }

    let version_script = manifest_dir.join("../../tools/version.sh");
    let version = Command::new(&version_script)
        .output()
        .ok()
        .filter(|output| output.status.success())
        .and_then(|output| String::from_utf8(output.stdout).ok())
        .map(|version| version.trim().to_owned())
        .unwrap_or_else(|| env!("CARGO_PKG_VERSION").to_owned());
    println!("cargo:rustc-env=LMCC_VERSION={version}");
}

fn git_path(repo: &Path, name: &str) -> Option<PathBuf> {
    let output = Command::new("git")
        .args(["-C"])
        .arg(repo)
        .args(["rev-parse", "--git-path", name])
        .output()
        .ok()
        .filter(|output| output.status.success())?;
    let path = PathBuf::from(String::from_utf8(output.stdout).ok()?.trim());
    Some(if path.is_absolute() {
        path
    } else {
        repo.join(path)
    })
}
