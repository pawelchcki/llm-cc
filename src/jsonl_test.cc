#include "src/jsonl.h"

#include <nlohmann/json.hpp>
#include <string>

#include "src/test_util.h"

int main() {  // NOLINT(bugprone-exception-escape)
  const std::string input =
      "{\"position\":0,\"bytes_hex\":\"61\",\"entropy\":null}\n"
      "{\"position\":1,\"bytes_hex\":\"62\",\"entropy\":0.5}\n";
  const auto records = llmcc::ParseEntropyJsonl(input);
  const auto tokens = llmcc::AlignTokens("ab", records);
  llmcc::test::ExpectEq(tokens.size(), std::size_t{2}, "two aligned tokens");
  llmcc::test::ExpectEq(tokens[1].entropy, std::optional<double>{0.5},
                        "entropy retained");

  bool mismatch = false;
  try {
    static_cast<void>(llmcc::AlignTokens("ac", records));
  } catch (const std::invalid_argument&) {
    mismatch = true;
  }
  llmcc::test::Expect(mismatch, "byte mismatch rejected");

  llmcc::Analysis analysis = llmcc::Analyze(
      tokens, {}, {},
      {.kind = llmcc::TauRule::Kind::kPercentile, .value = 67.0}, 0.8);
  const nlohmann::json output = llmcc::AnalysisJson(analysis);
  llmcc::test::Expect(output.contains("llm_cc"), "renamed JSON score field");
  llmcc::test::Expect(
      output.contains("token_count") &&
          output.contains("high_entropy_tokens") &&
          output.contains("lmcc_per_token") && output.contains("density") &&
          output.contains("mean_entropy") && output["tau_rule"] == "percentile",
      "analysis JSON contains normalized metrics and tau rule");

  const nlohmann::json empty = llmcc::AnalysisJson(llmcc::Analyze({}, {}));
  llmcc::test::Expect(
      empty["token_count"] == 0 && empty["lmcc_per_token"].is_null() &&
          empty["density"].is_null() && empty["mean_entropy"].is_null(),
      "empty normalized metrics are null");
  return 0;
}
