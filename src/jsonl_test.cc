#include "src/jsonl.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

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

  const std::vector<llmcc::EntropyRecord> prefixed = {
      {.position = 0, .bytes = " ab", .entropy = 1.0},
      {.position = 1, .bytes = " c", .entropy = 0.5}};
  const auto prefixed_tokens = llmcc::AlignTokens("ab c", prefixed);
  llmcc::test::Expect(
      prefixed_tokens.size() == 2 && prefixed_tokens[0].start_byte == 0 &&
          prefixed_tokens[0].end_byte == 2 && prefixed_tokens[1].end_byte == 4,
      "first-piece dummy-prefix space is not source text");
  const std::vector<llmcc::EntropyRecord> lone_prefix = {
      {.position = 0, .bytes = " ", .entropy = 1.0},
      {.position = 1, .bytes = "ab", .entropy = 0.5}};
  const auto lone_tokens = llmcc::AlignTokens("ab", lone_prefix);
  llmcc::test::Expect(lone_tokens.size() == 1 && lone_tokens[0].start_byte == 0,
                      "a lone dummy-prefix piece is dropped");
  bool later_space_rejected = false;
  try {
    static_cast<void>(llmcc::AlignTokens(
        "ab", std::vector<llmcc::EntropyRecord>{
                  {.position = 0, .bytes = "a", .entropy = std::nullopt},
                  {.position = 1, .bytes = " b", .entropy = 0.5}}));
  } catch (const std::invalid_argument&) {
    later_space_rejected = true;
  }
  llmcc::test::Expect(later_space_rejected,
                      "only the first piece may carry a dummy prefix");

  std::vector<llmcc::EntropyRecord> publishable = {{0, " ab", 1.0},
                                                   {1, "c", 0.5}};
  llmcc::test::Expect(
      llmcc::NormalizeDummyPrefix("abc", publishable) &&
          publishable.size() == 2 && publishable[0].bytes == "ab" &&
          publishable[0].position == 0 && publishable[1].position == 1,
      "normalized records concatenate to the source");
  std::vector<llmcc::EntropyRecord> lone = {{0, " ", std::nullopt},
                                            {1, "ab", 1.0}};
  llmcc::test::Expect(!llmcc::NormalizeDummyPrefix("ab", lone) &&
                          lone.size() == 2 && lone.front().bytes == " ",
                      "a standalone dummy-prefix piece is left unpublished");
  std::vector<llmcc::EntropyRecord> plain = {{0, "ab", 1.0}};
  llmcc::test::Expect(!llmcc::NormalizeDummyPrefix("ab", plain) &&
                          plain.size() == 1 && plain.front().bytes == "ab",
                      "records without a dummy prefix are left alone");

  llmcc::OffsetMap compact_map;
  compact_map.AppendRun(5, 2);
  compact_map.Append(20);
  llmcc::Analysis mapped{.llm_cc = 0,
                         .total_branch = 0,
                         .total_comp_level = 0,
                         .alpha = 0.8,
                         .tau = 0.67,
                         .metrics = {},
                         .tau_rule = {},
                         .units = {{.start_byte = 0,
                                    .end_byte = 2,
                                    .level = 0,
                                    .branching = 0,
                                    .children = {{.start_byte = 1,
                                                  .end_byte = 2,
                                                  .level = 0,
                                                  .branching = 0,
                                                  .children = {}}}}}};
  llmcc::MapAnalysisOffsets(mapped, compact_map);
  llmcc::test::ExpectEq(mapped.units.front().start_byte, std::size_t{5},
                        "compact map remaps unit starts");
  llmcc::test::ExpectEq(mapped.units.front().end_byte, std::size_t{20},
                        "compact map remaps unit ends");
  llmcc::test::ExpectEq(mapped.units.front().children.front().start_byte,
                        std::size_t{6},
                        "compact map remaps nested units iteratively");

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
