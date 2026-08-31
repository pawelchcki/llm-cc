#include "src/jsonl.h"

#include <nlohmann/json.hpp>
#include <string>

#include "src/test_util.h"

int main() {
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

  llmcc::Analysis analysis = llmcc::Analyze(tokens, {}, 67.0, 0.8);
  const nlohmann::json output = llmcc::AnalysisJson(analysis);
  llmcc::test::Expect(output.contains("llm_cc"), "renamed JSON score field");
  return 0;
}
