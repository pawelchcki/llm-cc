use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{Context, Result, bail, ensure};
use clap::Parser;
use lmcc_core::{Analysis, Token, Unit, analyze};
use lmcc_lang::{LanguageFrontend, OffsetMap, RustFrontend};
use serde_json::Value;
use tempfile::NamedTempFile;

#[derive(Debug, Parser)]
#[command(
    name = "lmcc",
    about = "Compute entropy-guided language-model code complexity"
)]
struct Arguments {
    /// Source file to analyze.
    source: PathBuf,

    /// Source language (currently only rust).
    #[arg(long, default_value = "rust")]
    lang: String,

    /// Read scorer JSONL instead of running model inference.
    #[arg(long, value_name = "PATH")]
    entropy_jsonl: Option<PathBuf>,

    /// GGUF model passed to rethink-cc.
    #[arg(long, value_name = "GGUF")]
    model: Option<PathBuf>,

    /// Path or command name for the C++ scorer.
    #[arg(long, default_value = "rethink-cc")]
    scorer_bin: PathBuf,

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

fn run(arguments: Arguments) -> Result<()> {
    ensure!(
        arguments.lang == "rust",
        "unsupported language '{}'; expected rust",
        arguments.lang
    );
    ensure!(
        arguments.entropy_jsonl.is_some() ^ arguments.model.is_some(),
        "provide exactly one of --entropy-jsonl or --model"
    );

    let source = fs::read_to_string(&arguments.source)
        .with_context(|| format!("failed to read source file {}", arguments.source.display()))?;
    let frontend = RustFrontend;
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

fn run_scorer(scorer_bin: &Path, model: &Path, preprocessed: &str) -> Result<Vec<u8>> {
    let mut source_file = NamedTempFile::new().context("failed to create preprocessed input")?;
    source_file
        .write_all(preprocessed.as_bytes())
        .context("failed to write preprocessed input")?;
    source_file
        .flush()
        .context("failed to flush preprocessed input")?;

    let output = Command::new(scorer_bin)
        .arg("--entropy")
        .arg("--model")
        .arg(model)
        .arg("--file")
        .arg(source_file.path())
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
}
