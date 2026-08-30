use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{Context, Result, bail, ensure};
use clap::Parser;
use lmcc_core::{Analysis, Token, Unit, analyze};
use lmcc_lang::{CFrontend, CppFrontend, LanguageFrontend, OffsetMap, RustFrontend};
use serde_json::Value;
use tempfile::NamedTempFile;

const DEFAULT_MODEL_PATH: &str = "models/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf";
const DEFAULT_MODEL_DOWNLOAD: &str = "mkdir -p models && curl -L --fail -o models/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf https://huggingface.co/bartowski/DeepSeek-Coder-V2-Lite-Base-GGUF/resolve/main/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf";

#[derive(Debug, Parser)]
#[command(
    name = "lmcc",
    about = "Compute entropy-guided language-model code complexity"
)]
struct Arguments {
    /// Source file to analyze.
    source: PathBuf,

    /// Source language (rust, c, cpp, or c++); inferred from the extension if omitted.
    #[arg(long)]
    lang: Option<String>,

    /// Read scorer JSONL instead of running model inference.
    #[arg(long, value_name = "PATH")]
    entropy_jsonl: Option<PathBuf>,

    /// GGUF model passed to rethink-cc.
    #[arg(long, value_name = "GGUF")]
    model: Option<PathBuf>,

    /// Path or command name for the C++ scorer.
    #[arg(long, default_value = "rethink-cc")]
    scorer_bin: PathBuf,

    /// Transformer layers to offload to the GPU when running the scorer.
    #[arg(long, value_name = "N")]
    gpu_layers: Option<u32>,

    /// Maximum scorer context size in tokens.
    #[arg(long, value_name = "N")]
    context: Option<u32>,

    /// Entropy percentile used as tau.
    #[arg(long, default_value_t = 67.0)]
    tau_percentile: f64,

    /// Branching weight in the LM-CC sum.
    #[arg(long, default_value_t = 0.8)]
    alpha: f64,
}

#[derive(Debug)]
struct EntropyRecord {
    position: usize,
    bytes: Vec<u8>,
    entropy: Option<f64>,
}

fn main() {
    if let Err(error) = run(Arguments::parse()) {
        eprintln!("error: {error:#}");
        std::process::exit(1);
    }
}

fn run(mut arguments: Arguments) -> Result<()> {
    let current_dir = std::env::current_dir().context("failed to determine current directory")?;
    arguments.model = resolve_model(
        arguments.model,
        arguments.entropy_jsonl.as_deref(),
        &current_dir,
    )?;
    ensure!(
        arguments.entropy_jsonl.is_none()
            || (arguments.gpu_layers.is_none() && arguments.context.is_none()),
        "--gpu-layers and --context cannot be used with --entropy-jsonl"
    );

    let language = selected_language(arguments.lang.as_deref(), &arguments.source)?;
    let frontend: Box<dyn LanguageFrontend> = match language {
        "rust" => Box::new(RustFrontend),
        "c" => Box::new(CFrontend),
        "cpp" | "c++" => Box::new(CppFrontend),
        _ => bail!("unsupported language '{language}'; expected rust, c, cpp, or c++"),
    };
    let source = fs::read_to_string(&arguments.source)
        .with_context(|| format!("failed to read source file {}", arguments.source.display()))?;
    let (preprocessed, offset_map) = frontend.strip_comments(&source);
    let structural_events = frontend.structural_events(&preprocessed);

    let jsonl = if let Some(path) = &arguments.entropy_jsonl {
        fs::read(path)
            .with_context(|| format!("failed to read entropy JSONL {}", path.display()))?
    } else {
        run_scorer(
            &arguments.scorer_bin,
            arguments.model.as_deref().expect("model checked above"),
            &preprocessed,
            arguments.gpu_layers,
            arguments.context,
        )?
    };
    let records = parse_entropy_jsonl(&jsonl)?;
    let tokens = align_tokens(&preprocessed, &records)?;
    let mut analysis = analyze(
        &tokens,
        &structural_events,
        arguments.tau_percentile,
        arguments.alpha,
    )?;
    map_analysis_offsets(&mut analysis, &offset_map)?;

    serde_json::to_writer_pretty(io::stdout().lock(), &analysis)
        .context("failed to write JSON output")?;
    println!();
    Ok(())
}

fn resolve_model(
    model: Option<PathBuf>,
    entropy_jsonl: Option<&Path>,
    current_dir: &Path,
) -> Result<Option<PathBuf>> {
    ensure!(
        !(model.is_some() && entropy_jsonl.is_some()),
        "provide exactly one of --entropy-jsonl or --model"
    );
    if entropy_jsonl.is_some() {
        return Ok(None);
    }
    if model.is_some() {
        return Ok(model);
    }

    let default_model = current_dir.join(DEFAULT_MODEL_PATH);
    ensure!(
        default_model.exists(),
        "default model {} does not exist; download it with: {DEFAULT_MODEL_DOWNLOAD}",
        default_model.display()
    );
    Ok(Some(default_model))
}

fn selected_language<'a>(explicit: Option<&'a str>, source: &Path) -> Result<&'a str> {
    if let Some(language) = explicit {
        return Ok(language);
    }

    match source.extension().and_then(|extension| extension.to_str()) {
        Some("rs") => Ok("rust"),
        Some("c" | "h") => Ok("c"),
        Some("cc" | "cpp" | "cxx" | "hpp" | "hh") => Ok("cpp"),
        _ => bail!(
            "cannot infer language from source file '{}'; pass --lang rust, c, cpp, or c++",
            source.display()
        ),
    }
}

fn run_scorer(
    scorer_bin: &Path,
    model: &Path,
    preprocessed: &str,
    gpu_layers: Option<u32>,
    context: Option<u32>,
) -> Result<Vec<u8>> {
    let mut source_file = NamedTempFile::new().context("failed to create preprocessed input")?;
    source_file
        .write_all(preprocessed.as_bytes())
        .context("failed to write preprocessed input")?;
    source_file
        .flush()
        .context("failed to flush preprocessed input")?;

    let mut command = Command::new(scorer_bin);
    command
        .arg("--entropy")
        .arg("--model")
        .arg(model)
        .arg("--file")
        .arg(source_file.path());
    if let Some(gpu_layers) = gpu_layers {
        command.arg("--gpu-layers").arg(gpu_layers.to_string());
    }
    if let Some(context) = context {
        command.arg("--context-size").arg(context.to_string());
    }

    let output = command
        .output()
        .with_context(|| format!("failed to run scorer {}", scorer_bin.display()))?;
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        bail!("scorer exited with {}: {}", output.status, stderr.trim());
    }
    Ok(output.stdout)
}

fn parse_entropy_jsonl(input: &[u8]) -> Result<Vec<EntropyRecord>> {
    let text = std::str::from_utf8(input).context("entropy JSONL is not UTF-8")?;
    let mut records = Vec::new();
    for (line_index, line) in text.lines().enumerate() {
        if line.trim().is_empty() {
            continue;
        }
        let value: Value = serde_json::from_str(line)
            .with_context(|| format!("invalid JSON on entropy line {}", line_index + 1))?;
        let object = value
            .as_object()
            .with_context(|| format!("entropy line {} is not an object", line_index + 1))?;
        let position_u64 = object
            .get("position")
            .and_then(Value::as_u64)
            .with_context(|| format!("entropy line {} has no valid position", line_index + 1))?;
        let position =
            usize::try_from(position_u64).context("entropy position does not fit this platform")?;
        ensure!(
            position == records.len(),
            "entropy positions must be contiguous from zero; expected {}, got {}",
            records.len(),
            position
        );
        let bytes_hex = object
            .get("bytes_hex")
            .and_then(Value::as_str)
            .with_context(|| format!("entropy line {} has no bytes_hex string", line_index + 1))?;
        let entropy_value = object
            .get("entropy")
            .with_context(|| format!("entropy line {} has no entropy field", line_index + 1))?;
        let entropy = if entropy_value.is_null() {
            None
        } else {
            let value = entropy_value
                .as_f64()
                .with_context(|| format!("entropy line {} is not numeric", line_index + 1))?;
            ensure!(
                value.is_finite() && value >= 0.0,
                "entropy line {} must be finite and non-negative",
                line_index + 1
            );
            Some(value)
        };
        ensure!(
            entropy.is_some() || position == 0,
            "only the first token may have null entropy"
        );
        records.push(EntropyRecord {
            position,
            bytes: decode_hex(bytes_hex)
                .with_context(|| format!("invalid bytes_hex on entropy line {}", line_index + 1))?,
            entropy,
        });
    }
    Ok(records)
}

fn decode_hex(value: &str) -> Result<Vec<u8>> {
    ensure!(value.len().is_multiple_of(2), "hex string has odd length");
    value
        .as_bytes()
        .chunks_exact(2)
        .map(|pair| {
            let high = hex_digit(pair[0])?;
            let low = hex_digit(pair[1])?;
            Ok(high << 4 | low)
        })
        .collect()
}

fn hex_digit(byte: u8) -> Result<u8> {
    match byte {
        b'0'..=b'9' => Ok(byte - b'0'),
        b'a'..=b'f' => Ok(byte - b'a' + 10),
        b'A'..=b'F' => Ok(byte - b'A' + 10),
        _ => bail!("invalid hex digit"),
    }
}

fn align_tokens(source: &str, records: &[EntropyRecord]) -> Result<Vec<Token>> {
    let source = source.as_bytes();
    let mut offset: usize = 0;
    let mut tokens = Vec::with_capacity(records.len());
    for record in records {
        ensure!(
            !record.bytes.is_empty(),
            "token {} has empty bytes_hex",
            record.position
        );
        let end = offset
            .checked_add(record.bytes.len())
            .context("token byte offset overflow")?;
        ensure!(
            source.get(offset..end) == Some(record.bytes.as_slice()),
            "token {} bytes do not match preprocessed source at byte {}",
            record.position,
            offset
        );
        tokens.push(Token {
            start_byte: offset,
            end_byte: end,
            entropy: record.entropy,
        });
        offset = end;
    }
    ensure!(
        offset == source.len(),
        "token bytes cover {} source bytes, expected {}",
        offset,
        source.len()
    );
    Ok(tokens)
}

fn map_analysis_offsets(analysis: &mut Analysis, map: &OffsetMap) -> Result<()> {
    fn map_unit(unit: &mut Unit, map: &OffsetMap) -> Result<()> {
        unit.start_byte = *map
            .get(unit.start_byte)
            .context("unit start is outside the preprocessing offset map")?;
        unit.end_byte = *map
            .get(unit.end_byte)
            .context("unit end is outside the preprocessing offset map")?;
        for child in &mut unit.children {
            map_unit(child, map)?;
        }
        Ok(())
    }

    for unit in &mut analysis.units {
        map_unit(unit, map)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_jsonl_byte_mismatch() {
        let input = br#"{"position":0,"bytes_hex":"616264","entropy":null}"#;
        let records = parse_entropy_jsonl(input).unwrap();
        assert!(align_tokens("abc", &records).is_err());
    }

    #[test]
    fn rejects_gaps_in_entropy_positions() {
        let input = br#"{"position":0,"bytes_hex":"61","entropy":null}
{"position":2,"bytes_hex":"62","entropy":0.5}"#;
        let error = parse_entropy_jsonl(input).unwrap_err().to_string();
        assert!(error.contains("expected 1, got 2"));
    }

    #[test]
    fn rejects_invalid_hex_bytes() {
        let input = br#"{"position":0,"bytes_hex":"0g","entropy":null}"#;
        let error = parse_entropy_jsonl(input).unwrap_err().to_string();
        assert!(error.contains("invalid bytes_hex"));
    }

    #[test]
    fn requires_the_entropy_field() {
        let input = br#"{"position":0,"bytes_hex":"61"}"#;
        assert!(parse_entropy_jsonl(input).is_err());
    }

    #[test]
    fn infers_supported_source_extensions() {
        for (path, expected) in [
            ("source.rs", "rust"),
            ("source.c", "c"),
            ("source.h", "c"),
            ("source.cc", "cpp"),
            ("source.cpp", "cpp"),
            ("source.cxx", "cpp"),
            ("source.hpp", "cpp"),
            ("source.hh", "cpp"),
        ] {
            assert_eq!(selected_language(None, Path::new(path)).unwrap(), expected);
        }
    }

    #[test]
    fn explicit_language_overrides_extension() {
        assert_eq!(
            selected_language(Some("c++"), Path::new("source.unknown")).unwrap(),
            "c++"
        );
    }

    #[test]
    fn rejects_an_unknown_extension_without_lang() {
        let error = selected_language(None, Path::new("source.txt"))
            .unwrap_err()
            .to_string();
        assert!(error.contains("cannot infer language"));
    }

    #[test]
    fn rejects_scorer_options_with_precomputed_entropy() {
        for option in ["--gpu-layers", "--context"] {
            let arguments = Arguments::try_parse_from([
                "lmcc",
                "source.rs",
                "--entropy-jsonl",
                "entropy.jsonl",
                option,
                "8",
            ])
            .unwrap();
            let error = run(arguments).unwrap_err().to_string();
            assert!(error.contains("cannot be used with --entropy-jsonl"));
        }
    }

    #[test]
    fn defaults_to_deepseek_coder_v2_model_in_current_directory() {
        let directory = tempfile::tempdir().unwrap();
        let model = directory.path().join(DEFAULT_MODEL_PATH);
        fs::create_dir_all(model.parent().unwrap()).unwrap();
        fs::write(&model, []).unwrap();

        assert_eq!(
            resolve_model(None, None, directory.path()).unwrap(),
            Some(model)
        );
    }

    #[test]
    fn missing_default_model_error_includes_download_command() {
        let directory = tempfile::tempdir().unwrap();
        let error = resolve_model(None, None, directory.path())
            .unwrap_err()
            .to_string();

        assert!(error.contains(DEFAULT_MODEL_PATH));
        assert!(error.contains(DEFAULT_MODEL_DOWNLOAD));
    }

    #[test]
    fn explicit_model_and_entropy_jsonl_remain_mutually_exclusive() {
        let error = resolve_model(
            Some(PathBuf::from("model.gguf")),
            Some(Path::new("entropy.jsonl")),
            Path::new("."),
        )
        .unwrap_err()
        .to_string();

        assert!(error.contains("provide exactly one"));
    }
}
