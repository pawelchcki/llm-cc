#include "src/lang.h"

#ifndef _WIN32
#include <sys/resource.h>
#endif

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::string Snippet(llmcc::Language language, std::size_t index) {
  const std::string id = std::to_string(index);
  switch (language) {
    case llmcc::Language::kRust:
      return "fn f" + id +
             "(x:i32)->i32{let mut y=x;for i in 0..8{if i%2==0{y+=i;}}y}\n";
    case llmcc::Language::kC:
      return "int f" + id +
             "(int x){int y=x;for(int i=0;i<8;i++){if(i%2==0)y+=i;}return "
             "y;}\n";
    case llmcc::Language::kCpp:
      return "int f" + id +
             "(int x){int y=x;for(int i=0;i<8;++i){if(i%2==0){y+=i;}}return "
             "y;}\n";
    case llmcc::Language::kJava:
      return "class C" + id +
             "{int f(int x){int y=x;for(int "
             "i=0;i<8;i++){if(i%2==0)y+=i;}return y;}}\n";
    case llmcc::Language::kPython:
      return "def f" + id +
             "(x):\n    y=x\n    for i in range(8):\n        if i%2==0:\n      "
             "      y+=i\n    return y\n";
    case llmcc::Language::kGo:
      return "func f" + id +
             "(x int)int{y:=x;for i:=0;i<8;i++{if i%2==0{y+=i}};return y}\n";
    case llmcc::Language::kJavaScript:
      return "function f" + id +
             "(x){let y=x;for(let i=0;i<8;++i){if(i%2===0){y+=i;}}return y;}\n";
    case llmcc::Language::kCSharp:
      return "class C" + id +
             "{int F(int x){int y=x;for(int "
             "i=0;i<8;i++){if(i%2==0)y+=i;}return y;}}\n";
  }
  throw std::logic_error("unknown language");
}

std::string Representative(std::size_t target, llmcc::Language language,
                           std::size_t& expected_functions) {
  std::string source;
  source.reserve(target + 256);
  if (language == llmcc::Language::kGo) {
    source = "package p\n";
  }
  while (source.size() < target) {
    source += Snippet(language, expected_functions++);
  }
  return source;
}

std::string Wide(std::size_t target) {
  std::string source = "int values[]={";
  source.reserve(target + 4);
  while (source.size() < target) {
    source += "0,";
  }
  source += "0};\n";
  return source;
}

std::string Malformed(std::size_t target) {
  std::string source;
  source.reserve(target);
  while (source.size() < target) {
    source += "int broken( { if ( value [ ;\n";
  }
  return source;
}

std::string Deep() {
  std::string source = "int f(){return ";
  source.append(4096, '(');
  source += '0';
  source.append(4096, ')');
  source += ";}\n";
  return source;
}

long long PeakRssBytes() {
#ifdef _WIN32
  return -1;
#else
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return -1;
  }
#ifdef __APPLE__
  return usage.ru_maxrss;
#else
  return static_cast<long long>(usage.ru_maxrss) * 1024;
#endif
#endif
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape)
  const std::size_t mib =
      argc > 1 ? static_cast<std::size_t>(std::stoull(argv[1])) : 10;
  if (mib > 1024) {
    throw std::invalid_argument("size must be at most 1024 MiB");
  }
  const llmcc::Language language =
      argc > 2 ? llmcc::ParseLanguage(argv[2]) : llmcc::Language::kCpp;
  const std::string_view scenario = argc > 3 ? argv[3] : "representative";
  const std::size_t target = mib * 1024 * 1024;
  std::size_t expected_functions = 0;
  std::string source;
  if (scenario == "representative") {
    source = Representative(target, language, expected_functions);
  } else if (scenario == "wide") {
    source = Wide(target);
  } else if (scenario == "malformed") {
    source = Malformed(target);
  } else if (scenario == "deep") {
    source = Deep();
  } else {
    throw std::invalid_argument(
        "scenario must be representative, wide, malformed, or deep");
  }

  const auto start = std::chrono::steady_clock::now();
  llmcc::PreparedSource prepared;
  try {
    prepared = llmcc::PrepareSource(source, language);
  } catch (const std::exception& error) {
    if (scenario == "deep" &&
        std::string_view(error.what()).find("depth limit") !=
            std::string_view::npos) {
      std::cout << "scenario=deep language=" << llmcc::LanguageName(language)
                << " source_bytes=" << source.size()
                << " rejected=1 reason=" << error.what()
                << " peak_rss_bytes=" << PeakRssBytes() << '\n';
      return 0;
    }
    std::cerr << "scenario=" << scenario
              << " language=" << llmcc::LanguageName(language)
              << " source_bytes=" << source.size()
              << " failed=1 reason=" << error.what()
              << " peak_rss_bytes=" << PeakRssBytes() << '\n';
    return 1;
  }
  if (scenario == "deep") {
    throw std::runtime_error("deep syntax unexpectedly passed depth limit");
  }
  {
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    if (prepared.original_offsets.size() != prepared.cleaned.size() + 1 ||
        prepared.original_offsets.back() != source.size()) {
      throw std::runtime_error("preprocessing offset map lacks end coverage");
    }
    if (scenario == "representative" &&
        prepared.functions.size() != expected_functions) {
      throw std::runtime_error("callable count mismatch: expected " +
                               std::to_string(expected_functions) + ", got " +
                               std::to_string(prepared.functions.size()));
    }
    std::cout << "scenario=" << scenario
              << " language=" << llmcc::LanguageName(language)
              << " source_bytes=" << source.size()
              << " cleaned_bytes=" << prepared.cleaned.size()
              << " callables=" << prepared.functions.size()
              << " map_spans=" << prepared.original_offsets.span_count()
              << " elapsed_seconds=" << seconds
              << " peak_rss_bytes=" << PeakRssBytes() << '\n';
  }
  return 0;
}
