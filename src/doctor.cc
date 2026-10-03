#include "src/doctor.h"

#include <gguf.h>
#include <llama.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#include "src/backend.h"
#include "src/build_info.h"
#include "src/cache.h"
#include "src/cache_io.h"
#include "src/model_identity.h"
#include "src/models.h"
#include "src/score_cmd.h"
#include "src/scoring_settings.h"

namespace llmcc {
namespace {

using json = nlohmann::json;
constexpr std::string_view kUsage =
    "Usage: llm-cc doctor [--format json|text] [--model GGUF | --model-name "
    "NAME]\n"
    "  --backend auto|cpu|cuda|rocm, --force-cpu, --gpu-layers N\n"
    "  --backend-dir DIR, --gpu-policy preserve|most-free, --device NAME\n"
    "  --gpu-min-free BYTES (single-device VRAM floor)\n"
    "Checks prepared local assets and visible devices without downloading,\n"
    "loading model weights or running inference. Exit: 0 ready, 1 unready,\n"
    "2 invalid options. Memory is an estimate, not an allocation guarantee.\n";

void Error(json& report, std::string_view component,
           const std::exception& error) {
  report["errors"].push_back(
      {{"component", component}, {"message", error.what()}});
}

std::uint64_t InspectModelShard(const std::filesystem::path& path,
                                json& model) {
  if (!std::filesystem::is_regular_file(path)) {
    throw std::runtime_error(
        "model is missing or not a regular file: " + path.string() +
        "; prepare local assets or pass --model GGUF");
  }
  const auto bytes = std::filesystem::file_size(path);
  model["path"] = cache_io::PathUtf8(path);
  model["bytes"] = bytes;
  const auto utf8_path = cache_io::PathUtf8(path);
  const std::unique_ptr<gguf_context, decltype(&gguf_free)> metadata(
      gguf_init_from_file(utf8_path.c_str(),
                          {.no_alloc = true, .ctx = nullptr}),
      gguf_free);
  if (!metadata)
    throw std::runtime_error("invalid or unreadable GGUF metadata: " +
                             path.string());
  const auto offset = gguf_get_data_offset(metadata.get());
  for (std::int64_t i = 0; i < gguf_get_n_tensors(metadata.get()); ++i) {
    const auto tensor_offset = gguf_get_tensor_offset(metadata.get(), i);
    const auto tensor_size = gguf_get_tensor_size(metadata.get(), i);
    if (offset > bytes || tensor_offset > bytes - offset ||
        tensor_size > bytes - offset - tensor_offset) {
      throw std::runtime_error("model tensor data is truncated: " +
                               path.string());
    }
  }
  if (gguf_get_n_tensors(metadata.get()) == 0) {
    throw std::runtime_error("GGUF contains no model tensors: " +
                             path.string());
  }
  return bytes;
}

std::uint64_t InspectLocalModel(const std::filesystem::path& path,
                                json& model) {
  std::uint64_t bytes = 0;
  model["files"] = json::array();
  if (!std::filesystem::is_regular_file(path)) {
    throw std::runtime_error(
        "model is missing or not a regular file: " + path.string() +
        "; prepare local assets or pass --model GGUF");
  }
  for (const auto& file : LocalModelFiles(path)) {
    json shard;
    const auto size = InspectModelShard(file, shard);
    if (size > std::numeric_limits<std::uint64_t>::max() - bytes) {
      throw std::runtime_error("model files are too large to measure");
    }
    bytes += size;
    model["files"].push_back(shard);
  }
  model["path"] = cache_io::PathUtf8(path);
  model["bytes"] = bytes;
  const auto utf8_path = cache_io::PathUtf8(path);
  auto parameters = llama_model_default_params();
  // An explicit empty device list prevents vocabulary inspection from
  // registering Metal/GPU devices before CPU execution has been configured.
  ggml_backend_dev_t no_devices[] = {nullptr};
  parameters.devices = no_devices;
  parameters.vocab_only = true;
  parameters.n_gpu_layers = 0;
  const std::unique_ptr<llama_model, decltype(&llama_model_free)> vocabulary(
      llama_model_load_from_file(utf8_path.c_str(), parameters),
      llama_model_free);
  if (!vocabulary) {
    throw std::runtime_error(
        "model architecture/tokenizer is incompatible or metadata is "
        "incomplete: " +
        path.string());
  }
  model["ready"] = true;
  model["check"] = "metadata, tensor bounds and vocabulary; weights not loaded";
  return bytes;
}

}  // namespace

int RunDoctorCommand(int argc, char** argv) {
  std::string format = "text";
  // Determine output format before parsing so option errors remain JSON too.
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string_view(argv[i]) == "--format") format = argv[i + 1];
  }
  json report = {{"schema_version", 1},
                 {"ready", false},
                 {"executable",
                  {{"ready", true},
                   {"version", build_info::Version()},
                   {"compiled_backend", CompiledBackend()}}},
                 {"model", {{"ready", false}}},
                 {"backend", {{"ready", false}, {"compatible", false}}},
                 {"visibility", json::object()},
                 {"devices", json::array()},
                 {"errors", json::array()}};
  ScoringSettings settings;
  ModelRequest model;
  int result = 1;
  try {
    if (format != "json" && format != "text")
      throw UsageError("--format expects json or text");
    for (int i = 1; i < argc; ++i) {
      const std::string_view option = argv[i];
      if (option == "--help" || option == "-h") {
        std::cout << kUsage;
        return 0;
      }
      if (option == "--no-download") continue;
      if (ParseScoringFlag(settings, option)) continue;
      if (++i >= argc)
        throw UsageError(std::string(option) + " requires a value");
      const std::string_view value = argv[i];
      if (option == "--format") continue;
      if (option == "--model")
        model.model = std::filesystem::u8path(value);
      else if (option == "--model-name")
        model.model_name = value;
      else if (option != "--backend" && option != "--backend-dir" &&
               option != "--gpu-layers" && option != "--gpu-policy" &&
               option != "--device" && option != "--gpu-min-free") {
        throw UsageError("unknown doctor option: " + std::string(option));
      } else if (!ParseScoringOption(settings, option, value)) {
        throw UsageError("unknown doctor option: " + std::string(option));
      }
    }
    if (model.model && model.model_name)
      throw UsageError("--model and --model-name are mutually exclusive");
    const auto* spec =
        model.model_name ? FindModel(*model.model_name) : &DefaultModel();
    if (!spec)
      throw UsageError(
          "unknown model name; see llm-cc models list --available");
    ValidateScoringSettings(settings);
    BackendLogCapture capture;
    std::uint64_t model_bytes = 0;
    try {
      const auto path =
          ResolveModel(model.model, *spec, true,
                       std::filesystem::current_path(), CacheDir(), {});
      model_bytes = InspectLocalModel(path, report["model"]);
    } catch (const std::exception& error) {
      Error(report, "model", error);
    }
    capture.Clear();
    report["backend"]["requested"] = BackendName(settings.backend);
    report["backend"]["gpu_layers"] = settings.gpu_layers;
    for (const char* name : {"CUDA_VISIBLE_DEVICES", "HIP_VISIBLE_DEVICES",
                             "ROCR_VISIBLE_DEVICES"}) {
      if (const char* value = std::getenv(name))
        report["visibility"][name] = value;
    }
    try {
      BackendRuntime backend(settings.backend, settings.gpu_layers,
                             build_info::Version(), settings.backend_directory,
                             true, false);
      report["backend"]["selected"] = backend.selected() == BackendKind::kAuto
                                          ? "metal"
                                          : BackendName(backend.selected());
      report["backend"]["compatible"] = true;
      const auto devices = settings.gpu_layers == 0
                               ? std::vector<GpuDeviceInfo>{}
                               : VisibleGpuDevices();
      for (const auto& device : devices) {
        report["devices"].push_back({{"name", device.name},
                                     {"description", device.description},
                                     {"free_bytes", device.free_bytes},
                                     {"total_bytes", device.total_bytes}});
      }
      if (settings.gpu_layers != 0 && devices.empty()) {
        throw std::runtime_error(
            "no usable visible GPU; check device permissions and visibility, "
            "or use --force-cpu");
      }
      const auto selected = SelectGpuDevice(
          settings.gpu_selection, settings.gpu_layers, model_bytes, devices);
      report["backend"]["device_policy"] =
          settings.gpu_selection.policy == GpuPolicy::kMostFree ? "most-free"
                                                                : "preserve";
      report["backend"]["selected_device"] =
          selected ? json(devices[*selected].name) : json(nullptr);
      if (settings.gpu_layers == -1 && !selected) {
        std::uint64_t aggregate = 0;
        for (const auto& device : devices) {
          aggregate +=
              std::min(device.free_bytes,
                       std::numeric_limits<std::uint64_t>::max() - aggregate);
        }
        if (aggregate < GpuMemoryRequirement(settings.gpu_selection,
                                             settings.gpu_layers,
                                             model_bytes)) {
          throw std::runtime_error(
              "insufficient visible GPU memory for model weights; use a "
              "smaller model or free GPU memory");
        }
      }
      report["backend"]["ready"] = true;
    } catch (const std::exception& error) {
      Error(report, "backend", error);
      if (!capture.Error().empty())
        report["backend"]["diagnostics"] = capture.Error();
    }
    report["ready"] = report["errors"].empty();
    result = report["ready"].get<bool>() ? 0 : 1;
  } catch (const std::exception& error) {
    Error(report, "options", error);
    result = 2;
  }
  if (format == "json")
    std::cout << report.dump() << '\n';
  else {
    std::cout << "llm-cc "
              << (report["ready"].get<bool>() ? "ready" : "not ready") << '\n';
    for (const auto& error : report["errors"]) {
      std::cout << error["component"].get<std::string>() << ": "
                << error["message"].get<std::string>() << '\n';
    }
    for (const auto& device : report["devices"]) {
      std::cout << device["name"].get<std::string>() << ": "
                << device["free_bytes"] << " bytes free\n";
    }
  }
  return result;
}

}  // namespace llmcc
