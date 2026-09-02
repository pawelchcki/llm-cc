#include "src/models.h"

#include <set>
#include <string>

#include "src/test_util.h"

int main() {
  const llmcc::ModelSpec& default_model = llmcc::DefaultModel();
  llmcc::test::ExpectEq(default_model.name,
                        std::string_view("deepseek-coder-v2-lite-base-q6_k"),
                        "DeepSeek Coder V2 Lite is the default model");
  llmcc::test::ExpectEq(&default_model, &llmcc::kModels.front(),
                        "default model is the first registry entry");

  std::set<std::string> names;
  for (const llmcc::ModelSpec& model : llmcc::Models()) {
    llmcc::test::Expect(llmcc::FindModel(model.name) == &model,
                        "registry model can be found by name");
    llmcc::test::Expect(!model.name.empty() && !model.file.empty() &&
                            !model.url.empty() && model.approx_bytes != 0,
                        "registry model fields are populated");
    llmcc::test::Expect(names.emplace(model.name).second,
                        "registry model names are unique");
  }
  llmcc::test::Expect(llmcc::FindModel("not-a-model") == nullptr,
                      "unknown model is not found");

  llmcc::test::ExpectEq(llmcc::FormatApproxSize(14'000'000'000ULL),
                        std::string("14 GB"), "whole gigabytes are formatted");
  llmcc::test::ExpectEq(llmcc::FormatApproxSize(5'500'000'000ULL),
                        std::string("5.5 GB"),
                        "fractional gigabytes are formatted");
  llmcc::test::ExpectEq(llmcc::FormatApproxSize(400'000'000ULL),
                        std::string("400 MB"), "megabytes are formatted");
  return 0;
}
