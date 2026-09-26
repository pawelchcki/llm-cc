#include "src/compare/cli.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>

#include <cstdio>
#endif

#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "src/compare/aggregate.h"
#include "src/compare/json_util.h"
#include "src/compare/store.h"
#include "src/input_limits.h"
#include "src/progress.h"

namespace llmcc::compare {
namespace {

constexpr std::string_view kUsage =
    "Usage:\n"
    "  llm-cc compare aggregate --output-dir DIR [--plan PLAN] "
    "[--worker FILE]...\n"
    "      [--identity FILE] [--fingerprint HEX] [--error MESSAGE]...\n"
    "  llm-cc compare store get KEY --cache LOCATION [--store-options FILE]\n"
    "      [--output FILE]\n"
    "  llm-cc compare store put KEY --cache LOCATION [--store-options FILE]\n"
    "      [--input FILE]\n\n"
    "aggregate validates a plan and its worker artifacts and writes\n"
    "report.json, report.md, comment.md, baseline.md, baseline.json and\n"
    "publication.json. Each --error makes the report a failure; without a\n"
    "plan, --identity and --fingerprint name the pipeline and experiment it\n"
    "belongs to. The exit status is 1 for a failed report.\n\n"
    "store reads or writes one object of a filesystem or s3://bucket/prefix\n"
    "store, using standard output and input by default. A missing object\n"
    "exits 1. S3 credentials come from AWS_ACCESS_KEY_ID,\n"
    "AWS_SECRET_ACCESS_KEY and AWS_SESSION_TOKEN.\n";

class CompareUsageError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

struct AggregateArguments {
  std::optional<std::filesystem::path> plan;
  std::vector<std::string> workers;
  std::filesystem::path output;
  std::optional<std::filesystem::path> identity;
  std::optional<std::string> fingerprint;
  std::vector<std::string> errors;
};

AggregateArguments ParseAggregate(const std::vector<std::string_view>& args) {
  AggregateArguments arguments;
  for (std::size_t index = 0; index < args.size(); ++index) {
    const std::string_view option = args[index];
    if (index + 1 >= args.size()) {
      throw CompareUsageError(std::string(option) + " requires a value");
    }
    const std::string_view value = args[++index];
    if (option == "--plan") {
      arguments.plan = std::filesystem::u8path(value);
    } else if (option == "--worker") {
      arguments.workers.emplace_back(value);
    } else if (option == "--output-dir") {
      arguments.output = std::filesystem::u8path(value);
    } else if (option == "--identity") {
      arguments.identity = std::filesystem::u8path(value);
    } else if (option == "--fingerprint") {
      if (value.size() != 64 || value.find_first_not_of("0123456789abcdef") !=
                                    std::string_view::npos) {
        throw CompareUsageError(
            "--fingerprint must be 64 lowercase hex digits");
      }
      arguments.fingerprint = std::string(value);
    } else if (option == "--error") {
      arguments.errors.emplace_back(value);
    } else {
      throw CompareUsageError("unknown aggregate option: " +
                              std::string(option));
    }
  }
  if (arguments.output.empty()) {
    throw CompareUsageError("aggregate requires --output-dir");
  }
  if (!arguments.plan.has_value() && arguments.errors.empty()) {
    throw CompareUsageError("aggregate requires --plan or --error");
  }
  return arguments;
}

// A failure report still names its pipeline when the identity is readable.
nlohmann::json ReadIdentity(const std::optional<std::filesystem::path>& path) {
  if (!path.has_value()) {
    return nullptr;
  }
  try {
    return ReadJsonFile(*path);
  } catch (const std::exception& error) {
    std::cerr << "warning: " << TerminalSafe(error.what()) << '\n';
    return nullptr;
  }
}

int RunAggregate(const std::vector<std::string_view>& args) {
  const AggregateArguments arguments = ParseAggregate(args);
  nlohmann::json report;
  const std::optional<std::filesystem::path>& plan = arguments.plan;
  if (!plan.has_value()) {
    report =
        WriteFailureReport(arguments.output, ReadIdentity(arguments.identity),
                           arguments.fingerprint.has_value()
                               ? nlohmann::json(*arguments.fingerprint)
                               : nlohmann::json(),
                           arguments.errors);
  } else if (arguments.errors.empty()) {
    report = AggregatePlan(*plan, arguments.workers, arguments.output);
  } else {
    // The run failed around a usable plan: keep the partial analysis next to
    // a failure report carrying both sets of errors.
    const nlohmann::json analysis =
        AggregatePlan(*plan, arguments.workers, arguments.output);
    WriteJsonFile(arguments.output / "analysis-report.json", analysis);
    std::vector<std::string> errors = arguments.errors;
    for (const nlohmann::json& error : analysis["errors"]) {
      errors.push_back(error.get<std::string>());
    }
    nlohmann::json identity = analysis["identity"];
    if (identity.is_null() ||
        (identity.is_object() &&
         identity.value("pipeline_id", nlohmann::json()).is_null())) {
      identity = ReadIdentity(arguments.identity);
    }
    report = WriteFailureReport(arguments.output, identity,
                                analysis["fingerprint"], errors);
  }
  const std::string status = report["status"].get<std::string>();
  std::cout << "comparison " << status << ": " << arguments.output.string()
            << '\n';
  return status == "failed" ? 1 : 0;
}

struct StoreArguments {
  std::string action;
  std::string key;
  std::string cache;
  std::optional<std::filesystem::path> store_options;
  std::optional<std::filesystem::path> file;
};

StoreArguments ParseStore(const std::vector<std::string_view>& args) {
  if (args.size() < 2 || (args[0] != "get" && args[0] != "put")) {
    throw CompareUsageError("store requires get or put and a KEY");
  }
  StoreArguments arguments{.action = std::string(args[0]),
                           .key = std::string(args[1])};
  for (std::size_t index = 2; index < args.size(); ++index) {
    const std::string_view option = args[index];
    if (index + 1 >= args.size()) {
      throw CompareUsageError(std::string(option) + " requires a value");
    }
    const std::string_view value = args[++index];
    if (option == "--cache") {
      arguments.cache = value;
    } else if (option == "--store-options") {
      arguments.store_options = std::filesystem::u8path(value);
    } else if ((option == "--output" && arguments.action == "get") ||
               (option == "--input" && arguments.action == "put")) {
      arguments.file = std::filesystem::u8path(value);
    } else {
      throw CompareUsageError("unknown store option: " + std::string(option));
    }
  }
  if (arguments.cache.empty()) {
    throw CompareUsageError("store requires --cache");
  }
  return arguments;
}

int RunStore(const std::vector<std::string_view>& args) {
  const StoreArguments arguments = ParseStore(args);
#if defined(_WIN32)
  // Objects are bytes; the console must not translate line endings.
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  const auto store =
      OpenStore(arguments.cache, ReadStoreOptions(arguments.store_options));
  if (arguments.action == "put") {
    const std::string value = arguments.file.has_value()
                                  ? ReadBoundedFile(*arguments.file)
                                  : ReadBoundedStream(std::cin);
    store->Put(arguments.key, value);
    return 0;
  }
  const std::optional<std::string> value = store->Get(arguments.key);
  if (!value.has_value()) {
    std::cerr << "not found: " << TerminalSafe(arguments.key) << '\n';
    return 1;
  }
  if (arguments.file.has_value()) {
    WriteFileAtomic(*arguments.file, *value);
  } else {
    std::cout.write(value->data(), static_cast<std::streamsize>(value->size()));
  }
  return 0;
}

}  // namespace

int RunCompareCommand(int argc, char** argv) {
  std::vector<std::string_view> arguments(argv + 1, argv + argc);
  if (arguments.empty() || arguments[0] == "-h" || arguments[0] == "--help") {
    std::cerr << kUsage;
    return arguments.empty() ? 2 : 0;
  }
  const std::string_view action = arguments.front();
  arguments.erase(arguments.begin());
  try {
    if (!arguments.empty() &&
        (arguments.front() == "-h" || arguments.front() == "--help")) {
      std::cout << kUsage;
      return 0;
    }
    if (action == "aggregate") {
      return RunAggregate(arguments);
    }
    if (action == "store") {
      return RunStore(arguments);
    }
    throw CompareUsageError("unknown compare command: " + std::string(action));
  } catch (const CompareUsageError& error) {
    std::cerr << "error: " << error.what() << "\n\n" << kUsage;
    return 2;
  } catch (const StoreError& error) {
    std::cerr << "error: " << TerminalSafe(error.what()) << '\n';
    return 2;
  }
}

}  // namespace llmcc::compare
