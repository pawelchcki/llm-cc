#include "src/compare/cli.h"

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
#include "src/progress.h"

namespace llmcc::compare {
namespace {

constexpr std::string_view kUsage =
    "Usage:\n"
    "  llm-cc compare aggregate --output-dir DIR [--plan PLAN] "
    "[--worker FILE]...\n"
    "      [--identity FILE] [--error MESSAGE]...\n\n"
    "aggregate validates a plan and its worker artifacts and writes\n"
    "report.json, report.md, comment.md, baseline.md, baseline.json and\n"
    "publication.json. Each --error makes the report a failure; without a\n"
    "plan, --identity names the pipeline it belongs to. The exit status is 1\n"
    "for a failed report.\n";

class CompareUsageError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

struct AggregateArguments {
  std::optional<std::filesystem::path> plan;
  std::vector<std::string> workers;
  std::filesystem::path output;
  std::optional<std::filesystem::path> identity;
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
                           nullptr, arguments.errors);
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
    throw CompareUsageError("unknown compare command: " + std::string(action));
  } catch (const CompareUsageError& error) {
    std::cerr << "error: " << error.what() << "\n\n" << kUsage;
    return 2;
  }
}

}  // namespace llmcc::compare
