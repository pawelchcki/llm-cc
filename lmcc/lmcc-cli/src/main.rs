use std::collections::BTreeMap;
use std::ffi::OsString;
use std::fs::{self, OpenOptions};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use anyhow::{Context, Result, bail, ensure};
use clap::{Parser, Subcommand};
use lmcc_core::{Analysis, Token, Unit, analyze};
use lmcc_lang::{CFrontend, CppFrontend, LanguageFrontend, OffsetMap, RustFrontend};
use serde_json::{Map, Value};
use tempfile::NamedTempFile;

const DEFAULT_MODEL_PATH: &str = "models/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf";
const DEFAULT_MODEL_FILE: &str = "DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf";
const MANIFEST_FILE: &str = "models.json";
const DEFAULT_MODEL_URL: &str = "https://huggingface.co/bartowski/DeepSeek-Coder-V2-Lite-Base-GGUF/resolve/main/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf";
const PROGRESS_BYTE_INTERVAL: u64 = 256 * 1024 * 1024;
const PROGRESS_TIME_INTERVAL: Duration = Duration::from_secs(3);

#[derive(Debug, Parser)]
#[command(
    name = "lmcc",
    version = env!("RETHINK_VERSION"),
    about = "Compute entropy-guided language-model code complexity",
    args_conflicts_with_subcommands = true
)]
struct Arguments {
    /// Source file to analyze.
    source: Option<PathBuf>,

    #[command(subcommand)]
    command: Option<CommandKind>,

    /// Source language (rust, c, cpp, or c++); inferred from the extension if omitted.
    #[arg(long)]
    lang: Option<String>,

    /// Read scorer JSONL instead of running model inference.
    #[arg(long, value_name = "PATH")]
    entropy_jsonl: Option<PathBuf>,

    /// GGUF model passed to rethink-cc.
    #[arg(long, value_name = "GGUF")]
    model: Option<PathBuf>,

    /// Do not automatically download the default model if it is missing.
    #[arg(long)]
    no_download: bool,

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

#[derive(Debug, Subcommand)]
enum CommandKind {
    /// Inspect or remove cached models.
    Models {
        #[command(subcommand)]
        command: ModelsCommand,
    },
}

#[derive(Debug, Subcommand)]
enum ModelsCommand {
    /// List cached GGUF models.
    List,
    /// Remove a cached model and any partial download.
    Remove { file_name: String },
    /// Print the model cache directory.
    Path,
}

#[derive(Clone, Debug, Default)]
struct ModelTimestamps {
    downloaded_at: Option<u64>,
    last_used_at: Option<u64>,
}

type ModelManifest = BTreeMap<String, ModelTimestamps>;

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
    if let Some(command) = arguments.command {
        return run_command(command);
    }

    let source = arguments
        .source
        .as_deref()
        .context("a source file is required unless a subcommand is used")?;
    let current_dir = std::env::current_dir().context("failed to determine current directory")?;
    let cache_dir = cache_dir()?;
    arguments.model = resolve_model(
        arguments.model,
        arguments.entropy_jsonl.as_deref(),
        arguments.no_download,
        &current_dir,
        &cache_dir,
    )?;
    ensure!(
        arguments.entropy_jsonl.is_none()
            || (arguments.gpu_layers.is_none() && arguments.context.is_none()),
        "--gpu-layers and --context cannot be used with --entropy-jsonl"
    );

    let language = selected_language(arguments.lang.as_deref(), source)?;
    let frontend: Box<dyn LanguageFrontend> = match language {
        "rust" => Box::new(RustFrontend),
        "c" => Box::new(CFrontend),
        "cpp" | "c++" => Box::new(CppFrontend),
        _ => bail!("unsupported language '{language}'; expected rust, c, cpp, or c++"),
    };
    let source = fs::read_to_string(source)
        .with_context(|| format!("failed to read source file {}", source.display()))?;
    let (preprocessed, offset_map) = frontend.strip_comments(&source);
    let structural_events = frontend.structural_events(&preprocessed);

    let jsonl = if let Some(path) = &arguments.entropy_jsonl {
        fs::read(path)
            .with_context(|| format!("failed to read entropy JSONL {}", path.display()))?
    } else {
        let model = arguments.model.as_deref().expect("model checked above");
        mark_cached_model_used(&cache_dir, model);
        run_scorer(
            &arguments.scorer_bin,
            model,
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

fn run_command(command: CommandKind) -> Result<()> {
    let cache_dir = cache_dir()?;
    match command {
        CommandKind::Models { command } => match command {
            ModelsCommand::List => list_models(&cache_dir),
            ModelsCommand::Remove { file_name } => remove_model(&cache_dir, &file_name),
            ModelsCommand::Path => {
                println!("{}", cache_dir.display());
                Ok(())
            }
        },
    }
}

fn cache_dir() -> Result<PathBuf> {
    cache_dir_from(
        std::env::var_os("LMCC_CACHE_DIR"),
        std::env::var_os("XDG_CACHE_HOME"),
        std::env::var_os("HOME"),
    )
}

fn cache_dir_from(
    lmcc_cache_dir: Option<OsString>,
    xdg_cache_home: Option<OsString>,
    home: Option<OsString>,
) -> Result<PathBuf> {
    if let Some(path) = lmcc_cache_dir.filter(|path| !path.is_empty()) {
        return Ok(PathBuf::from(path));
    }
    if let Some(path) = xdg_cache_home.filter(|path| !path.is_empty()) {
        return Ok(PathBuf::from(path).join("lmcc/models"));
    }
    if let Some(path) = home.filter(|path| !path.is_empty()) {
        return Ok(PathBuf::from(path).join(".cache/lmcc/models"));
    }
    bail!("cannot determine model cache directory; set LMCC_CACHE_DIR")
}

fn resolve_model(
    model: Option<PathBuf>,
    entropy_jsonl: Option<&Path>,
    no_download: bool,
    current_dir: &Path,
    cache_dir: &Path,
) -> Result<Option<PathBuf>> {
    resolve_model_with_downloader(
        model,
        entropy_jsonl,
        no_download,
        current_dir,
        cache_dir,
        download_default_model,
    )
}

fn resolve_model_with_downloader<F>(
    model: Option<PathBuf>,
    entropy_jsonl: Option<&Path>,
    no_download: bool,
    current_dir: &Path,
    cache_dir: &Path,
    downloader: F,
) -> Result<Option<PathBuf>>
where
    F: FnOnce(&Path) -> Result<()>,
{
    ensure!(
        !(model.is_some() && entropy_jsonl.is_some()),
        "provide exactly one of --entropy-jsonl or --model"
    );
    if model.is_some() {
        return Ok(model);
    }
    if entropy_jsonl.is_some() {
        return Ok(None);
    }

    let legacy_model = current_dir.join(DEFAULT_MODEL_PATH);
    if legacy_model.exists() {
        return Ok(Some(legacy_model));
    }

    let cached_model = cache_dir.join(DEFAULT_MODEL_FILE);
    if !cached_model.exists() {
        ensure!(
            !no_download,
            "default model is not cached at {}; remove --no-download to fetch it automatically",
            cached_model.display()
        );
        eprintln!("downloading ~14 GB to {}", cached_model.display());
        downloader(&cached_model)?;
    }
    Ok(Some(cached_model))
}

fn download_default_model(target: &Path) -> Result<()> {
    if let Some(parent) = target.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("failed to create model directory {}", parent.display()))?;
    }

    let partial = partial_path(target);
    let requested_offset = match fs::metadata(&partial) {
        Ok(metadata) => metadata.len(),
        Err(error) if error.kind() == io::ErrorKind::NotFound => 0,
        Err(error) => {
            return Err(error).with_context(|| {
                format!("failed to inspect partial download {}", partial.display())
            });
        }
    };

    if requested_offset == 0 {
        eprintln!("Downloading model from {DEFAULT_MODEL_URL}");
    } else {
        eprintln!("Resuming model download at {requested_offset} bytes");
    }

    let platform_agent = ureq::Agent::config_builder()
        .tls_config(
            ureq::tls::TlsConfig::builder()
                .root_certs(ureq::tls::RootCerts::PlatformVerifier)
                .build(),
        )
        .build()
        .new_agent();
    let request = |agent: &ureq::Agent| {
        let mut request = agent.get(DEFAULT_MODEL_URL);
        if requested_offset > 0 {
            request = request.header("Range", format!("bytes={requested_offset}-"));
        }
        request.call()
    };
    let response = match request(&platform_agent) {
        Err(error) if platform_roots_are_empty(&error) => {
            let fallback_agent = ureq::Agent::config_builder()
                .tls_config(
                    ureq::tls::TlsConfig::builder()
                        .root_certs(ureq::tls::RootCerts::WebPki)
                        .build(),
                )
                .build()
                .new_agent();
            request(&fallback_agent)
        }
        response => response,
    };
    let response = match response {
        Ok(response) => response,
        Err(ureq::Error::StatusCode(status)) => {
            bail!("download failed with HTTP status {status} for {DEFAULT_MODEL_URL}")
        }
        Err(error) => {
            return Err(error).with_context(|| format!("download failed for {DEFAULT_MODEL_URL}"));
        }
    };

    let status = response.status().as_u16();
    let resume_offset = if requested_offset > 0 && status == 206 {
        let content_range = response
            .headers()
            .get("Content-Range")
            .and_then(|value| value.to_str().ok())
            .context("resume response has no valid Content-Range header")?;
        let range_start = content_range
            .strip_prefix("bytes ")
            .and_then(|range| range.split_once('-'))
            .and_then(|(start, _)| start.parse::<u64>().ok())
            .context("resume response has an invalid Content-Range header")?;
        ensure!(
            range_start == requested_offset,
            "resume response starts at byte {range_start}, expected {requested_offset}"
        );
        requested_offset
    } else {
        ensure!(
            status == 200,
            "download failed with HTTP status {status} for {DEFAULT_MODEL_URL}"
        );
        0
    };

    let response_length = response
        .headers()
        .get("Content-Length")
        .and_then(|value| value.to_str().ok())
        .and_then(|value| value.parse::<u64>().ok());
    let total_length = response_length.and_then(|length| resume_offset.checked_add(length));
    stream_download(
        response.into_body().into_reader(),
        target,
        resume_offset,
        total_length,
    )?;
    mark_model_downloaded(target);
    Ok(())
}

fn platform_roots_are_empty(error: &ureq::Error) -> bool {
    let mut current: Option<&(dyn std::error::Error + 'static)> = Some(error);
    while let Some(error) = current {
        if error
            .to_string()
            .contains("No CA certificates were loaded from the system")
        {
            return true;
        }
        current = error.source();
    }
    false
}

fn partial_path(target: &Path) -> PathBuf {
    let mut path = OsString::from(target.as_os_str());
    path.push(".partial");
    PathBuf::from(path)
}

fn stream_download<R: Read>(
    mut reader: R,
    target: &Path,
    resume_offset: u64,
    total_length: Option<u64>,
) -> Result<()> {
    if let Some(parent) = target.parent().filter(|path| !path.as_os_str().is_empty()) {
        fs::create_dir_all(parent)
            .with_context(|| format!("failed to create model directory {}", parent.display()))?;
    }

    let partial = partial_path(target);
    if resume_offset > 0 {
        let actual_length = fs::metadata(&partial)
            .with_context(|| format!("failed to inspect partial download {}", partial.display()))?
            .len();
        ensure!(
            actual_length == resume_offset,
            "partial download has {actual_length} bytes, expected {resume_offset}"
        );
    }
    let mut output = OpenOptions::new()
        .create(true)
        .write(true)
        .append(resume_offset > 0)
        .truncate(resume_offset == 0)
        .open(&partial)
        .with_context(|| format!("failed to open partial download {}", partial.display()))?;

    let mut downloaded = resume_offset;
    let mut last_reported_bytes = downloaded;
    let mut last_reported_at = Instant::now();
    let mut buffer = vec![0_u8; 1024 * 1024];
    loop {
        let count = reader
            .read(&mut buffer)
            .with_context(|| format!("failed while downloading {DEFAULT_MODEL_URL}"))?;
        if count == 0 {
            break;
        }
        output
            .write_all(&buffer[..count])
            .with_context(|| format!("failed to write partial download {}", partial.display()))?;
        downloaded = downloaded
            .checked_add(u64::try_from(count).context("download size overflow")?)
            .context("download size overflow")?;

        if downloaded.saturating_sub(last_reported_bytes) >= PROGRESS_BYTE_INTERVAL
            || last_reported_at.elapsed() >= PROGRESS_TIME_INTERVAL
        {
            print_download_progress(downloaded, total_length);
            last_reported_bytes = downloaded;
            last_reported_at = Instant::now();
        }
    }
    if let Some(total) = total_length {
        ensure!(
            downloaded == total,
            "download ended after {downloaded} bytes, expected {total}"
        );
    }
    output
        .flush()
        .with_context(|| format!("failed to flush partial download {}", partial.display()))?;
    drop(output);

    print_download_progress(downloaded, total_length);
    fs::rename(&partial, target).with_context(|| {
        format!(
            "failed to move completed download {} to {}",
            partial.display(),
            target.display()
        )
    })?;
    Ok(())
}

fn print_download_progress(downloaded: u64, total_length: Option<u64>) {
    if let Some(total) = total_length.filter(|total| *total > 0) {
        let percentage = downloaded as f64 * 100.0 / total as f64;
        eprintln!("Downloaded {downloaded} bytes ({percentage:.1}%)");
    } else {
        eprintln!("Downloaded {downloaded} bytes");
    }
}

fn manifest_path(cache_dir: &Path) -> PathBuf {
    cache_dir.join(MANIFEST_FILE)
}

fn read_manifest(cache_dir: &Path) -> ModelManifest {
    let Some(Value::Object(entries)) = fs::read(manifest_path(cache_dir))
        .ok()
        .and_then(|contents| serde_json::from_slice(&contents).ok())
    else {
        return ModelManifest::new();
    };
    entries
        .into_iter()
        .filter_map(|(file_name, value)| {
            let value = value.as_object()?;
            Some((
                file_name,
                ModelTimestamps {
                    downloaded_at: value.get("downloaded_at").and_then(Value::as_u64),
                    last_used_at: value.get("last_used_at").and_then(Value::as_u64),
                },
            ))
        })
        .collect()
}

fn write_manifest(cache_dir: &Path, manifest: &ModelManifest) {
    let result = (|| -> Result<()> {
        fs::create_dir_all(cache_dir)?;
        let entries = manifest
            .iter()
            .map(|(file_name, timestamps)| {
                let mut value = Map::new();
                if let Some(downloaded_at) = timestamps.downloaded_at {
                    value.insert("downloaded_at".to_owned(), Value::from(downloaded_at));
                }
                if let Some(last_used_at) = timestamps.last_used_at {
                    value.insert("last_used_at".to_owned(), Value::from(last_used_at));
                }
                (file_name.clone(), Value::Object(value))
            })
            .collect();
        let contents = serde_json::to_vec_pretty(&Value::Object(entries))?;
        fs::write(manifest_path(cache_dir), contents)?;
        Ok(())
    })();
    let _ = result;
}

fn epoch_seconds() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

fn mark_model_downloaded(model: &Path) {
    let (Some(cache_dir), Some(file_name)) = (
        model.parent(),
        model.file_name().and_then(|name| name.to_str()),
    ) else {
        return;
    };
    let mut manifest = read_manifest(cache_dir);
    manifest
        .entry(file_name.to_owned())
        .or_default()
        .downloaded_at = Some(epoch_seconds());
    write_manifest(cache_dir, &manifest);
}

fn mark_cached_model_used(cache_dir: &Path, model: &Path) {
    let Some(model_parent) = model.parent() else {
        return;
    };
    let is_cached = model_parent == cache_dir
        || model_parent
            .canonicalize()
            .ok()
            .zip(cache_dir.canonicalize().ok())
            .is_some_and(|(model_parent, cache_dir)| model_parent == cache_dir);
    if !is_cached {
        return;
    }
    let Some(file_name) = model.file_name().and_then(|name| name.to_str()) else {
        return;
    };
    let mut manifest = read_manifest(cache_dir);
    manifest
        .entry(file_name.to_owned())
        .or_default()
        .last_used_at = Some(epoch_seconds());
    write_manifest(cache_dir, &manifest);
}

fn list_models(cache_dir: &Path) -> Result<()> {
    let manifest = read_manifest(cache_dir);
    let entries = match fs::read_dir(cache_dir) {
        Ok(entries) => entries,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(()),
        Err(error) => {
            return Err(error)
                .with_context(|| format!("failed to read model cache {}", cache_dir.display()));
        }
    };
    let mut models = Vec::new();
    for entry in entries {
        let entry =
            entry.with_context(|| format!("failed to read model cache {}", cache_dir.display()))?;
        let path = entry.path();
        if path.extension().and_then(|extension| extension.to_str()) != Some("gguf") {
            continue;
        }
        let metadata = entry
            .metadata()
            .with_context(|| format!("failed to inspect cached model {}", path.display()))?;
        if metadata.is_file() {
            models.push((entry.file_name(), metadata.len()));
        }
    }
    models.sort_by(|left, right| left.0.cmp(&right.0));

    for (file_name, size) in models {
        let file_name = file_name.to_string_lossy();
        let timestamps = manifest
            .get(file_name.as_ref())
            .cloned()
            .unwrap_or_default();
        println!(
            "{}\t{:.2} GiB\t{}\t{}",
            file_name,
            size as f64 / 1024_f64.powi(3),
            format_timestamp(timestamps.downloaded_at),
            format_timestamp(timestamps.last_used_at),
        );
    }
    Ok(())
}

fn remove_model(cache_dir: &Path, file_name: &str) -> Result<()> {
    ensure!(
        is_bare_file_name(file_name),
        "model name must be a bare file name without path separators"
    );
    let target = cache_dir.join(file_name);
    remove_if_present(&target)?;
    remove_if_present(&partial_path(&target))?;

    let mut manifest = read_manifest(cache_dir);
    manifest.remove(file_name);
    write_manifest(cache_dir, &manifest);
    Ok(())
}

fn is_bare_file_name(file_name: &str) -> bool {
    !file_name.is_empty()
        && file_name != "."
        && file_name != ".."
        && !file_name.contains(['/', '\\'])
}

fn remove_if_present(path: &Path) -> Result<()> {
    match fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error).with_context(|| format!("failed to remove {}", path.display())),
    }
}

fn format_timestamp(timestamp: Option<u64>) -> String {
    let Some(timestamp) = timestamp else {
        return "never".to_owned();
    };
    let Ok(timestamp) = i64::try_from(timestamp) else {
        return timestamp.to_string();
    };
    let days = timestamp.div_euclid(86_400);
    let seconds = timestamp.rem_euclid(86_400);
    let (year, month, day) = civil_date(days);
    format!(
        "{year:04}-{month:02}-{day:02} {:02}:{:02}:{:02} UTC",
        seconds / 3_600,
        seconds % 3_600 / 60,
        seconds % 60
    )
}

fn civil_date(days_since_epoch: i64) -> (i64, i64, i64) {
    let days = days_since_epoch + 719_468;
    let era = if days >= 0 { days } else { days - 146_096 } / 146_097;
    let day_of_era = days - era * 146_097;
    let year_of_era =
        (day_of_era - day_of_era / 1_460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let mut year = year_of_era + era * 400;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let month_prime = (5 * day_of_year + 2) / 153;
    let day = day_of_year - (153 * month_prime + 2) / 5 + 1;
    let month = month_prime + if month_prime < 10 { 3 } else { -9 };
    year += i64::from(month <= 2);
    (year, month, day)
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
    use std::io::Cursor;

    struct InterruptedReader {
        data: Cursor<Vec<u8>>,
        reads_before_error: usize,
    }

    impl Read for InterruptedReader {
        fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
            if self.reads_before_error == 0 {
                return Err(io::Error::new(
                    io::ErrorKind::ConnectionReset,
                    "test interruption",
                ));
            }
            self.reads_before_error -= 1;
            let read_length = buffer.len().min(4);
            self.data.read(&mut buffer[..read_length])
        }
    }

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
    fn removed_download_flag_is_rejected() {
        assert!(Arguments::try_parse_from(["lmcc", "source.rs", "--download"]).is_err());
    }

    #[test]
    fn cache_directory_uses_the_documented_precedence() {
        assert_eq!(
            cache_dir_from(
                Some(OsString::from("/override")),
                Some(OsString::from("/xdg")),
                Some(OsString::from("/home/user")),
            )
            .unwrap(),
            PathBuf::from("/override")
        );
        assert_eq!(
            cache_dir_from(
                None,
                Some(OsString::from("/xdg")),
                Some(OsString::from("/home/user")),
            )
            .unwrap(),
            PathBuf::from("/xdg/lmcc/models")
        );
        assert_eq!(
            cache_dir_from(None, None, Some(OsString::from("/home/user"))).unwrap(),
            PathBuf::from("/home/user/.cache/lmcc/models")
        );
        assert!(cache_dir_from(None, None, None).is_err());
    }

    #[test]
    fn model_resolution_follows_explicit_entropy_legacy_and_cache_order() {
        let directory = tempfile::tempdir().unwrap();
        let cache = directory.path().join("cache");
        let legacy = directory.path().join(DEFAULT_MODEL_PATH);
        let cached = cache.join(DEFAULT_MODEL_FILE);
        fs::create_dir_all(legacy.parent().unwrap()).unwrap();
        fs::create_dir_all(&cache).unwrap();
        fs::write(&legacy, []).unwrap();
        fs::write(&cached, []).unwrap();

        assert_eq!(
            resolve_model(
                Some(PathBuf::from("explicit.gguf")),
                None,
                false,
                directory.path(),
                &cache,
            )
            .unwrap(),
            Some(PathBuf::from("explicit.gguf"))
        );
        assert_eq!(
            resolve_model(
                None,
                Some(Path::new("entropy.jsonl")),
                false,
                directory.path(),
                &cache,
            )
            .unwrap(),
            None
        );
        assert_eq!(
            resolve_model(None, None, false, directory.path(), &cache).unwrap(),
            Some(legacy.clone())
        );
        fs::remove_file(legacy).unwrap();
        assert_eq!(
            resolve_model(None, None, false, directory.path(), &cache).unwrap(),
            Some(cached)
        );
    }

    #[test]
    fn missing_cached_model_is_downloaded_automatically() {
        let directory = tempfile::tempdir().unwrap();
        let cache = directory.path().join("cache");
        let expected = cache.join(DEFAULT_MODEL_FILE);

        let resolved =
            resolve_model_with_downloader(None, None, false, directory.path(), &cache, |target| {
                assert_eq!(target, expected);
                fs::create_dir_all(target.parent().unwrap())?;
                fs::write(target, b"fake model")?;
                Ok(())
            })
            .unwrap();

        assert_eq!(resolved, Some(expected));
    }

    #[test]
    fn no_download_error_includes_expected_cache_path() {
        let directory = tempfile::tempdir().unwrap();
        let cache = directory.path().join("cache");
        let expected = cache.join(DEFAULT_MODEL_FILE);
        let error = resolve_model(None, None, true, directory.path(), &cache)
            .unwrap_err()
            .to_string();

        assert!(error.contains("--no-download"));
        assert!(error.contains(&expected.display().to_string()));
    }

    #[test]
    fn explicit_model_and_entropy_jsonl_remain_mutually_exclusive() {
        let error = resolve_model(
            Some(PathBuf::from("model.gguf")),
            Some(Path::new("entropy.jsonl")),
            false,
            Path::new("."),
            Path::new("cache"),
        )
        .unwrap_err()
        .to_string();

        assert!(error.contains("provide exactly one"));
    }

    #[test]
    fn manifest_updates_and_corruption_is_tolerated() {
        let directory = tempfile::tempdir().unwrap();
        let model = directory.path().join("model.gguf");
        fs::write(&model, []).unwrap();

        mark_model_downloaded(&model);
        let manifest = read_manifest(directory.path());
        assert!(manifest["model.gguf"].downloaded_at.is_some());
        assert!(manifest["model.gguf"].last_used_at.is_none());

        fs::write(manifest_path(directory.path()), b"not json").unwrap();
        assert!(read_manifest(directory.path()).is_empty());
        mark_cached_model_used(directory.path(), &model);
        let manifest = read_manifest(directory.path());
        assert!(manifest["model.gguf"].downloaded_at.is_none());
        assert!(manifest["model.gguf"].last_used_at.is_some());
    }

    #[test]
    fn successful_download_renames_partial_file() {
        let directory = tempfile::tempdir().unwrap();
        let target = directory.path().join("models/model.gguf");

        stream_download(Cursor::new(b"model data"), &target, 0, Some(10)).unwrap();

        assert_eq!(fs::read(&target).unwrap(), b"model data");
        assert!(!partial_path(&target).exists());
    }

    #[test]
    fn interrupted_download_leaves_only_partial_file() {
        let directory = tempfile::tempdir().unwrap();
        let target = directory.path().join("model.gguf");
        let reader = InterruptedReader {
            data: Cursor::new(b"unfinished".to_vec()),
            reads_before_error: 1,
        };

        let error = stream_download(reader, &target, 0, None)
            .unwrap_err()
            .to_string();

        assert!(error.contains("failed while downloading"));
        assert!(!target.exists());
        assert_eq!(fs::read(partial_path(&target)).unwrap(), b"unfi");
    }

    #[test]
    fn resumed_download_appends_at_the_existing_offset() {
        let directory = tempfile::tempdir().unwrap();
        let target = directory.path().join("model.gguf");
        let partial = partial_path(&target);
        fs::write(&partial, b"first ").unwrap();

        stream_download(Cursor::new(b"second"), &target, 6, Some(12)).unwrap();

        assert_eq!(fs::read(&target).unwrap(), b"first second");
        assert!(!partial.exists());
    }
}
