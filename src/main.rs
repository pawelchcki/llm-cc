use std::fs;
use std::io::{self, Read, Write};
use std::num::NonZeroU32;
use std::path::PathBuf;

use anyhow::{Context, Result, anyhow, bail};
use clap::{Parser, ValueEnum};
use hf_hub::HFClientSync;
use llama_cpp_2::context::params::LlamaContextParams;
use llama_cpp_2::llama_backend::LlamaBackend;
use llama_cpp_2::llama_batch::LlamaBatch;
use llama_cpp_2::model::params::LlamaModelParams;
use llama_cpp_2::model::{AddBos, LlamaModel};
use llama_cpp_2::token::LlamaToken;
use serde_json::json;

const DEFAULT_HF_OWNER: &str = "bartowski";
const DEFAULT_HF_REPO: &str = "Qwen_Qwen3.5-9B-GGUF";
const DEFAULT_HF_FILE: &str = "Qwen3.5-9B-Q4_K_M.gguf";
const CPU_HF_REPO: &str = "Qwen_Qwen3.5-4B-GGUF";
const CPU_HF_FILE: &str = "Qwen_Qwen3.5-4B-Q4_K_M.gguf";

#[derive(Clone, Copy, Debug, ValueEnum)]
enum ModelPreset {
    /// Qwen3.5-9B Q4_K_M, about 6.2 GB.
    #[value(name = "qwen-9b")]
    Qwen9b,
    /// Qwen3.5-4B Q4_K_M, about 3.0 GB; a better fit for CPU-only use.
    #[value(name = "qwen-4b")]
    Qwen4b,
}

/// Score observed prompt tokens with a local causal language model.
#[derive(Debug, Parser)]
#[command(version, about)]
struct Args {
    /// Local GGUF file. If omitted, the default model is downloaded from Hugging Face.
    #[arg(long, value_name = "GGUF")]
    model: Option<PathBuf>,

    /// Built-in Hugging Face model to download when --model is omitted.
    #[arg(long, value_enum, default_value_t = ModelPreset::Qwen9b)]
    preset: ModelPreset,

    /// Text to score. Reads standard input when neither this nor --file is supplied.
    #[arg(long, conflicts_with = "file")]
    prompt: Option<String>,

    /// UTF-8 text file to score.
    #[arg(long, value_name = "PATH", conflicts_with = "prompt")]
    file: Option<PathBuf>,

    /// Prepend the model's BOS token. Qwen3.5 normally does not use one.
    #[arg(long)]
    add_bos: bool,

    /// Context capacity in tokens.
    #[arg(long, default_value_t = 2048)]
    context_size: u32,

    /// CPU threads used by llama.cpp. Zero keeps the llama.cpp default.
    #[arg(long, default_value_t = 0)]
    threads: i32,

    /// Layers to offload when built with cuda, rocm, metal, or vulkan.
    #[arg(long, default_value_t = 0)]
    gpu_layers: u32,
}

fn main() -> Result<()> {
    let args = Args::parse();
    let prompt = read_prompt(&args)?;
    let model_path = resolve_model(&args)?;

    let backend = LlamaBackend::init().context("failed to initialize llama.cpp")?;
    let model_params = LlamaModelParams::default().with_n_gpu_layers(args.gpu_layers);
    let model = LlamaModel::load_from_file(&backend, &model_path, &model_params)
        .with_context(|| format!("failed to load {}", model_path.display()))?;

    let add_bos = if args.add_bos {
        AddBos::Always
    } else {
        AddBos::Never
    };
    let tokens = model
        .str_to_token(&prompt, add_bos)
        .context("failed to tokenize input")?;
    if tokens.is_empty() {
        bail!("the input produced no tokens");
    }
    if tokens.len() > args.context_size as usize {
        bail!(
            "input has {} tokens but --context-size is {}",
            tokens.len(),
            args.context_size
        );
    }

    let mut context_params = LlamaContextParams::default()
        .with_n_ctx(NonZeroU32::new(args.context_size))
        .with_n_batch(1)
        .with_n_ubatch(1);
    if args.threads > 0 {
        context_params = context_params
            .with_n_threads(args.threads)
            .with_n_threads_batch(args.threads);
    }
    let mut context = model
        .new_context(&backend, context_params)
        .context("failed to create model context")?;
    let mut batch = LlamaBatch::new(1, 1);
    let stdout = io::stdout();
    let mut output = io::BufWriter::new(stdout.lock());

    if !args.add_bos {
        write_record(&mut output, &model, 0, tokens[0], None)?;
    }

    let mut total_log_probability = 0.0_f64;
    let mut scored = 0_usize;

    for source_index in 0..tokens.len().saturating_sub(1) {
        batch.clear();
        batch
            .add(
                tokens[source_index],
                i32::try_from(source_index).context("token position exceeds i32")?,
                &[0],
                true,
            )
            .context("failed to construct inference batch")?;
        context
            .decode(&mut batch)
            .with_context(|| format!("model evaluation failed at token {source_index}"))?;

        let target = tokens[source_index + 1];
        let log_probability = token_log_probability(context.get_logits_ith(0), target)?;
        let input_position = if args.add_bos {
            source_index
        } else {
            source_index + 1
        };
        write_record(
            &mut output,
            &model,
            input_position,
            target,
            Some(log_probability),
        )?;
        total_log_probability += log_probability;
        scored += 1;
    }
    output.flush()?;

    if scored == 0 {
        eprintln!("scored 0 tokens (a prefix or --add-bos is needed to score the first token)");
    } else {
        let mean_nll = -total_log_probability / scored as f64;
        eprintln!(
            "scored {scored} tokens; mean_nll={mean_nll:.6}; perplexity={:.6}",
            mean_nll.exp()
        );
    }
    Ok(())
}

fn read_prompt(args: &Args) -> Result<String> {
    if let Some(prompt) = &args.prompt {
        return Ok(prompt.clone());
    }
    if let Some(path) = &args.file {
        return fs::read_to_string(path)
            .with_context(|| format!("failed to read {}", path.display()));
    }

    let mut prompt = String::new();
    io::stdin()
        .read_to_string(&mut prompt)
        .context("failed to read standard input")?;
    Ok(prompt)
}

fn resolve_model(args: &Args) -> Result<PathBuf> {
    if let Some(path) = &args.model {
        if !path.is_file() {
            bail!("model file does not exist: {}", path.display());
        }
        return Ok(path.clone());
    }

    let (repo, file, approximate_size) = match args.preset {
        ModelPreset::Qwen9b => (DEFAULT_HF_REPO, DEFAULT_HF_FILE, "6.2 GB"),
        ModelPreset::Qwen4b => (CPU_HF_REPO, CPU_HF_FILE, "3.0 GB"),
    };
    eprintln!(
        "resolving {DEFAULT_HF_OWNER}/{repo}:{file} from Hugging Face (the first run downloads about {approximate_size})"
    );
    HFClientSync::new()
        .context("failed to create Hugging Face client")?
        .model(DEFAULT_HF_OWNER, repo)
        .download_file()
        .filename(file)
        .send()
        .with_context(|| format!("failed to download {}/{}:{}", DEFAULT_HF_OWNER, repo, file))
}

fn token_log_probability(logits: &[f32], target: LlamaToken) -> Result<f64> {
    let target_index = usize::try_from(target.0).context("negative target token id")?;
    let target_logit = *logits
        .get(target_index)
        .ok_or_else(|| anyhow!("target token {target_index} is outside the vocabulary"))?
        as f64;
    let max_logit = logits
        .iter()
        .copied()
        .map(f64::from)
        .fold(f64::NEG_INFINITY, f64::max);
    let sum_exp: f64 = logits
        .iter()
        .copied()
        .map(f64::from)
        .map(|logit| (logit - max_logit).exp())
        .sum();
    Ok(target_logit - max_logit - sum_exp.ln())
}

fn write_record(
    output: &mut impl Write,
    model: &LlamaModel,
    position: usize,
    token: LlamaToken,
    log_probability: Option<f64>,
) -> Result<()> {
    let bytes = model
        .token_to_piece_bytes(token, 8, true, None)
        .or_else(|error| match error {
            llama_cpp_2::TokenToStringError::InsufficientBufferSpace(size) => {
                model.token_to_piece_bytes(token, size.unsigned_abs() as usize, true, None)
            }
            other => Err(other),
        })
        .with_context(|| format!("failed to decode token {}", token.0))?;
    let piece = String::from_utf8_lossy(&bytes);
    let bytes_hex: String = bytes.iter().map(|byte| format!("{byte:02x}")).collect();
    let probability = log_probability.map(f64::exp);
    serde_json::to_writer(
        &mut *output,
        &json!({
            "position": position,
            "token_id": token.0,
            "piece": piece,
            "bytes_hex": bytes_hex,
            "probability": probability,
            "log_probability": log_probability,
        }),
    )?;
    writeln!(output)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn uniform_logits_have_uniform_probability() {
        let log_probability = token_log_probability(&[0.0, 0.0, 0.0, 0.0], LlamaToken(2)).unwrap();
        assert!((log_probability.exp() - 0.25).abs() < 1e-12);
    }

    #[test]
    fn log_softmax_is_stable_for_large_logits() {
        let log_probability = token_log_probability(&[10_000.0, 9_999.0], LlamaToken(0)).unwrap();
        let expected = 1.0 / (1.0 + (-1.0_f64).exp());
        assert!((log_probability.exp() - expected).abs() < 1e-12);
    }

    #[test]
    fn rejects_token_outside_vocabulary() {
        assert!(token_log_probability(&[0.0], LlamaToken(1)).is_err());
    }
}
