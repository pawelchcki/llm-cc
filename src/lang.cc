#include "src/lang.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>

extern "C" {
const TSLanguage* tree_sitter_rust();
const TSLanguage* tree_sitter_c();
const TSLanguage* tree_sitter_cpp();
const TSLanguage* tree_sitter_java();
const TSLanguage* tree_sitter_python();
const TSLanguage* tree_sitter_go();
const TSLanguage* tree_sitter_javascript();
const TSLanguage* tree_sitter_c_sharp();
}

namespace llmcc {
namespace {

constexpr auto kRustComments =
    std::to_array<std::string_view>({"line_comment", "block_comment"});
constexpr auto kRustStructural = std::to_array<std::string_view>(
    {"function_item", "loop_expression", "while_expression", "for_expression",
     "if_expression", "match_expression", "match_block", "block",
     "declaration_list", "field_declaration_list",
     "ordered_field_declaration_list", "enum_variant_list"});
constexpr auto kCComments = std::to_array<std::string_view>({"comment"});
constexpr auto kCStructural = std::to_array<std::string_view>(
    {"function_definition", "compound_statement", "for_statement",
     "while_statement", "do_statement", "if_statement", "switch_statement"});
constexpr auto kCppStructural = std::to_array<std::string_view>(
    {"function_definition", "compound_statement", "for_statement",
     "while_statement", "do_statement", "if_statement", "switch_statement",
     "for_range_loop", "lambda_expression", "try_statement",
     "namespace_definition", "field_declaration_list", "enumerator_list"});
constexpr auto kJavaComments =
    std::to_array<std::string_view>({"line_comment", "block_comment"});
constexpr auto kJavaStructural =
    std::to_array<std::string_view>({"method_declaration",
                                     "constructor_declaration",
                                     "compact_constructor_declaration",
                                     "lambda_expression",
                                     "block",
                                     "constructor_body",
                                     "class_body",
                                     "interface_body",
                                     "enum_body",
                                     "annotation_type_body",
                                     "if_statement",
                                     "ternary_expression",
                                     "for_statement",
                                     "enhanced_for_statement",
                                     "while_statement",
                                     "do_statement",
                                     "switch_expression",
                                     "switch_block",
                                     "switch_block_statement_group",
                                     "switch_rule",
                                     "guard",
                                     "try_statement",
                                     "try_with_resources_statement",
                                     "catch_clause",
                                     "finally_clause",
                                     "synchronized_statement"});
constexpr auto kPythonComments = std::to_array<std::string_view>({"comment"});
constexpr auto kPythonStructural = std::to_array<std::string_view>(
    {"function_definition", "lambda", "block", "class_definition",
     "if_statement", "conditional_expression", "elif_clause", "else_clause",
     "for_statement", "for_in_clause", "if_clause", "while_statement",
     "match_statement", "case_clause", "try_statement", "except_clause",
     "except_group_clause", "finally_clause", "with_statement"});
constexpr auto kPythonComprehensions = std::to_array<std::string_view>(
    {"list_comprehension", "set_comprehension", "dictionary_comprehension",
     "generator_expression"});
constexpr auto kGoComments = std::to_array<std::string_view>({"comment"});
constexpr auto kGoStructural = std::to_array<std::string_view>(
    {"function_declaration", "method_declaration", "func_literal", "block",
     "field_declaration_list", "if_statement", "for_statement",
     "expression_switch_statement", "type_switch_statement", "select_statement",
     "expression_case", "type_case", "communication_case", "default_case"});
constexpr auto kJavaScriptComments = std::to_array<std::string_view>(
    {"comment", "html_comment", "hash_bang_line"});
constexpr auto kJavaScriptStructural = std::to_array<std::string_view>(
    {"function_declaration", "generator_function_declaration",
     "method_definition",    "function_expression",
     "generator_function",   "arrow_function",
     "statement_block",      "class_body",
     "class_static_block",   "if_statement",
     "ternary_expression",   "for_statement",
     "for_in_statement",     "while_statement",
     "do_statement",         "switch_statement",
     "switch_body",          "switch_case",
     "switch_default",       "try_statement",
     "catch_clause",         "finally_clause",
     "with_statement"});
constexpr auto kCSharpComments = std::to_array<std::string_view>({"comment"});
constexpr auto kCSharpStructural =
    std::to_array<std::string_view>({"method_declaration",
                                     "constructor_declaration",
                                     "destructor_declaration",
                                     "operator_declaration",
                                     "conversion_operator_declaration",
                                     "local_function_statement",
                                     "lambda_expression",
                                     "anonymous_method_expression",
                                     "block",
                                     "arrow_expression_clause",
                                     "declaration_list",
                                     "enum_member_declaration_list",
                                     "accessor_declaration",
                                     "if_statement",
                                     "conditional_expression",
                                     "for_statement",
                                     "foreach_statement",
                                     "while_statement",
                                     "do_statement",
                                     "switch_statement",
                                     "switch_body",
                                     "switch_section",
                                     "switch_expression",
                                     "switch_expression_arm",
                                     "when_clause",
                                     "try_statement",
                                     "catch_clause",
                                     "catch_filter_clause",
                                     "finally_clause",
                                     "using_statement",
                                     "lock_statement",
                                     "checked_statement",
                                     "unsafe_statement",
                                     "fixed_statement",
                                     "from_clause",
                                     "join_clause",
                                     "join_into_clause",
                                     "let_clause",
                                     "where_clause",
                                     "order_by_clause",
                                     "select_clause",
                                     "group_clause"});

constexpr auto kRustAliases = std::to_array<std::string_view>({"rust"});
constexpr auto kCAliases = std::to_array<std::string_view>({"c"});
constexpr auto kCppAliases = std::to_array<std::string_view>({"cpp", "c++"});
constexpr auto kJavaAliases = std::to_array<std::string_view>({"java"});
constexpr auto kPythonAliases =
    std::to_array<std::string_view>({"python", "py"});
constexpr auto kGoAliases = std::to_array<std::string_view>({"go", "golang"});
constexpr auto kJavaScriptAliases = std::to_array<std::string_view>(
    {"javascript", "js", "node", "nodejs", "node.js"});
constexpr auto kCSharpAliases =
    std::to_array<std::string_view>({"csharp", "cs", "c#"});

constexpr auto kRustExtensions = std::to_array<std::string_view>({".rs"});
constexpr auto kCExtensions = std::to_array<std::string_view>({".c", ".h"});
constexpr auto kCppExtensions = std::to_array<std::string_view>(
    {".cc", ".cpp", ".cxx", ".c++", ".hpp", ".hh", ".hxx", ".h++"});
constexpr auto kJavaExtensions = std::to_array<std::string_view>({".java"});
constexpr auto kPythonExtensions =
    std::to_array<std::string_view>({".py", ".pyw", ".pyi"});
constexpr auto kGoExtensions = std::to_array<std::string_view>({".go"});
constexpr auto kJavaScriptExtensions =
    std::to_array<std::string_view>({".js", ".mjs", ".cjs"});
constexpr auto kCSharpExtensions =
    std::to_array<std::string_view>({".cs", ".csx"});

constexpr auto kRustFunctions =
    std::to_array<std::string_view>({"function_item"});
constexpr auto kRustCallables =
    std::to_array<std::string_view>({"function_item", "closure_expression"});
constexpr auto kCFunctions =
    std::to_array<std::string_view>({"function_definition"});
constexpr auto kCCallables = kCFunctions;
constexpr auto kCppCallables = std::to_array<std::string_view>(
    {"function_definition", "lambda_expression"});
constexpr auto kJavaFunctions = std::to_array<std::string_view>(
    {"method_declaration", "constructor_declaration",
     "compact_constructor_declaration"});
constexpr auto kJavaCallables = std::to_array<std::string_view>(
    {"method_declaration", "constructor_declaration",
     "compact_constructor_declaration", "lambda_expression"});
constexpr auto kPythonFunctions =
    std::to_array<std::string_view>({"function_definition"});
constexpr auto kPythonCallables =
    std::to_array<std::string_view>({"function_definition", "lambda"});
constexpr auto kGoFunctions = std::to_array<std::string_view>(
    {"function_declaration", "method_declaration"});
constexpr auto kGoCallables = std::to_array<std::string_view>(
    {"function_declaration", "method_declaration", "func_literal"});
constexpr auto kJavaScriptFunctions = std::to_array<std::string_view>(
    {"function_declaration", "generator_function_declaration",
     "method_definition"});
constexpr auto kJavaScriptCallables = std::to_array<std::string_view>(
    {"function_declaration", "generator_function_declaration",
     "method_definition", "function_expression", "generator_function",
     "arrow_function"});
constexpr auto kCSharpFunctions = std::to_array<std::string_view>(
    {"method_declaration", "constructor_declaration", "destructor_declaration",
     "operator_declaration", "conversion_operator_declaration"});
constexpr auto kCSharpCallables = std::to_array<std::string_view>(
    {"method_declaration", "constructor_declaration", "destructor_declaration",
     "operator_declaration", "conversion_operator_declaration",
     "local_function_statement", "lambda_expression",
     "anonymous_method_expression"});

using GrammarFunction = const TSLanguage* (*)();

struct LanguageMetadata {
  Language language;
  std::string_view canonical_name;
  GrammarFunction grammar;
  std::span<const std::string_view> aliases;
  std::span<const std::string_view> extensions;
  std::span<const std::string_view> comments;
  std::span<const std::string_view> structural;
  std::span<const std::string_view> functions;
  std::span<const std::string_view> callables;
};

const LanguageMetadata& Metadata(Language language) {
  static const std::array metadata = {
      LanguageMetadata{.language = Language::kRust,
                       .canonical_name = "rust",
                       .grammar = tree_sitter_rust,
                       .aliases = kRustAliases,
                       .extensions = kRustExtensions,
                       .comments = kRustComments,
                       .structural = kRustStructural,
                       .functions = kRustFunctions,
                       .callables = kRustCallables},
      LanguageMetadata{.language = Language::kC,
                       .canonical_name = "c",
                       .grammar = tree_sitter_c,
                       .aliases = kCAliases,
                       .extensions = kCExtensions,
                       .comments = kCComments,
                       .structural = kCStructural,
                       .functions = kCFunctions,
                       .callables = kCCallables},
      LanguageMetadata{.language = Language::kCpp,
                       .canonical_name = "cpp",
                       .grammar = tree_sitter_cpp,
                       .aliases = kCppAliases,
                       .extensions = kCppExtensions,
                       .comments = kCComments,
                       .structural = kCppStructural,
                       .functions = kCFunctions,
                       .callables = kCppCallables},
      LanguageMetadata{.language = Language::kJava,
                       .canonical_name = "java",
                       .grammar = tree_sitter_java,
                       .aliases = kJavaAliases,
                       .extensions = kJavaExtensions,
                       .comments = kJavaComments,
                       .structural = kJavaStructural,
                       .functions = kJavaFunctions,
                       .callables = kJavaCallables},
      LanguageMetadata{.language = Language::kPython,
                       .canonical_name = "python",
                       .grammar = tree_sitter_python,
                       .aliases = kPythonAliases,
                       .extensions = kPythonExtensions,
                       .comments = kPythonComments,
                       .structural = kPythonStructural,
                       .functions = kPythonFunctions,
                       .callables = kPythonCallables},
      LanguageMetadata{.language = Language::kGo,
                       .canonical_name = "go",
                       .grammar = tree_sitter_go,
                       .aliases = kGoAliases,
                       .extensions = kGoExtensions,
                       .comments = kGoComments,
                       .structural = kGoStructural,
                       .functions = kGoFunctions,
                       .callables = kGoCallables},
      LanguageMetadata{.language = Language::kJavaScript,
                       .canonical_name = "javascript",
                       .grammar = tree_sitter_javascript,
                       .aliases = kJavaScriptAliases,
                       .extensions = kJavaScriptExtensions,
                       .comments = kJavaScriptComments,
                       .structural = kJavaScriptStructural,
                       .functions = kJavaScriptFunctions,
                       .callables = kJavaScriptCallables},
      LanguageMetadata{.language = Language::kCSharp,
                       .canonical_name = "csharp",
                       .grammar = tree_sitter_c_sharp,
                       .aliases = kCSharpAliases,
                       .extensions = kCSharpExtensions,
                       .comments = kCSharpComments,
                       .structural = kCSharpStructural,
                       .functions = kCSharpFunctions,
                       .callables = kCSharpCallables},
  };
  const auto* const match =
      std::ranges::find(metadata, language, &LanguageMetadata::language);
  if (match == metadata.end()) {
    throw std::logic_error("unknown source language");
  }
  return *match;
}

const std::array<Language, 8> kLanguages = {
    Language::kRust,   Language::kC,  Language::kCpp,        Language::kJava,
    Language::kPython, Language::kGo, Language::kJavaScript, Language::kCSharp,
};

constexpr std::string_view kCanonicalLanguageNames =
    "rust, c, cpp, java, python, go, javascript, or csharp";

bool HasKind(std::span<const std::string_view> kinds, const char* kind) {
  return std::ranges::find(kinds, kind) != kinds.end();
}

bool IsNodeType(TSNode node, std::string_view type) {
  return !ts_node_is_null(node) && ts_node_type(node) == type;
}

using Parser = std::unique_ptr<TSParser, decltype(&ts_parser_delete)>;
using Tree = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>;

Tree Parse(std::string_view source, Language language) {
  if (source.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("source is too large for tree-sitter");
  }
  Parser parser(ts_parser_new(), ts_parser_delete);
  if (!parser ||
      !ts_parser_set_language(parser.get(), Metadata(language).grammar())) {
    throw std::runtime_error("tree-sitter language ABI is incompatible");
  }
  Tree tree(ts_parser_parse_string(parser.get(), nullptr, source.data(),
                                   static_cast<std::uint32_t>(source.size())),
            ts_tree_delete);
  if (!tree) {
    throw std::runtime_error("tree-sitter parser returned no tree");
  }
  return tree;
}

void CollectCommentRanges(
    TSNode node, std::span<const std::string_view> comment_kinds,
    std::vector<std::pair<std::size_t, std::size_t>>& ranges) {
  if (HasKind(comment_kinds, ts_node_type(node))) {
    ranges.emplace_back(ts_node_start_byte(node), ts_node_end_byte(node));
    return;
  }
  for (std::uint32_t i = 0; i < ts_node_child_count(node); ++i) {
    CollectCommentRanges(ts_node_child(node, i), comment_kinds, ranges);
  }
}

void CollectStructuralEvents(TSNode node, Language language,
                             std::size_t structural_depth,
                             std::span<const std::string_view> structural_kinds,
                             std::vector<StructuralEvent>& events) {
  const bool structural = HasKind(structural_kinds, ts_node_type(node));
  const std::size_t start = ts_node_start_byte(node);
  const std::size_t end = ts_node_end_byte(node);
  if (structural && start < end) {
    events.push_back(
        {.scope_start = start, .byte_offset = end, .depth = structural_depth});
  }
  const std::size_t child_depth = structural_depth + (structural ? 1 : 0);
  const bool python_comprehension =
      language == Language::kPython &&
      HasKind(kPythonComprehensions, ts_node_type(node));
  const bool python_comprehension_loop =
      language == Language::kPython && IsNodeType(node, "for_in_clause");
  const bool python_comprehension_filter =
      language == Language::kPython && IsNodeType(node, "if_clause");
  const TSNode comprehension_body =
      python_comprehension
          ? ts_node_child_by_field_name(node, "body", sizeof("body") - 1)
          : TSNode{};
  std::size_t comprehension_clause_count = 0;
  if (python_comprehension) {
    for (std::uint32_t i = 0; i < ts_node_child_count(node); ++i) {
      const TSNode child = ts_node_child(node, i);
      if (IsNodeType(child, "for_in_clause") ||
          IsNodeType(child, "if_clause")) {
        ++comprehension_clause_count;
      }
    }
  }
  std::size_t comprehension_depth = child_depth;
  for (std::uint32_t i = 0; i < ts_node_child_count(node); ++i) {
    const TSNode child = ts_node_child(node, i);
    std::size_t effective_depth = comprehension_depth;
    if (python_comprehension && ts_node_eq(child, comprehension_body)) {
      effective_depth = child_depth + comprehension_clause_count;
    }
    const char* field_name = ts_node_field_name_for_child(node, i);
    if (python_comprehension_loop && field_name != nullptr &&
        std::string_view(field_name) == "right") {
      effective_depth = structural_depth;
    }
    if (python_comprehension_filter) {
      effective_depth = structural_depth;
    }
    CollectStructuralEvents(child, language, effective_depth, structural_kinds,
                            events);
    if (python_comprehension && (IsNodeType(child, "for_in_clause") ||
                                 IsNodeType(child, "if_clause"))) {
      ++comprehension_depth;
    }
  }
}

std::string NodeText(TSNode node, std::string_view source) {
  if (ts_node_is_null(node)) {
    return "<anonymous>";
  }
  const std::size_t start = ts_node_start_byte(node);
  const std::size_t end = ts_node_end_byte(node);
  if (start > end || end > source.size()) {
    return "<anonymous>";
  }
  return std::string(source.substr(start, end - start));
}

std::string CFunctionName(TSNode function, std::string_view source) {
  TSNode current = ts_node_child_by_field_name(function, "declarator",
                                               sizeof("declarator") - 1);
  constexpr auto kNames = std::to_array<std::string_view>(
      {"identifier", "field_identifier", "qualified_identifier",
       "destructor_name", "operator_name", "operator_cast",
       "template_function"});
  for (std::size_t steps = 0; steps < 128 && !ts_node_is_null(current);
       ++steps) {
    if (HasKind(kNames, ts_node_type(current))) {
      return NodeText(current, source);
    }
    TSNode declarator = ts_node_child_by_field_name(current, "declarator",
                                                    sizeof("declarator") - 1);
    if (!ts_node_is_null(declarator)) {
      current = declarator;
      continue;
    }
    if (IsNodeType(current, "reference_declarator") ||
        IsNodeType(current, "parenthesized_declarator")) {
      current = ts_node_named_child(current, 0);
      continue;
    }
    break;
  }
  return "<anonymous>";
}

TSNode Field(TSNode node, std::string_view name) {
  return ts_node_child_by_field_name(node, name.data(),
                                     static_cast<std::uint32_t>(name.size()));
}

bool HasBody(TSNode function) {
  return !ts_node_is_null(Field(function, "body"));
}

bool IsDefinition(TSNode function, Language language) {
  if (language == Language::kRust || language == Language::kC ||
      language == Language::kCpp) {
    return true;
  }
  return HasBody(function);
}

std::string CallableName(TSNode function, std::string_view source,
                         Language language) {
  if (language == Language::kC || language == Language::kCpp) {
    return CFunctionName(function, source);
  }
  if (language == Language::kCSharp) {
    if (IsNodeType(function, "destructor_declaration")) {
      return "~" + NodeText(Field(function, "name"), source);
    }
    if (IsNodeType(function, "operator_declaration")) {
      return "operator " + NodeText(Field(function, "operator"), source);
    }
    if (IsNodeType(function, "conversion_operator_declaration")) {
      std::string kind = "conversion";
      for (std::uint32_t i = 0; i < ts_node_child_count(function); ++i) {
        const TSNode child = ts_node_child(function, i);
        if (IsNodeType(child, "implicit") || IsNodeType(child, "explicit")) {
          kind = NodeText(child, source);
          break;
        }
      }
      return kind + " operator " + NodeText(Field(function, "type"), source);
    }
  }
  return NodeText(Field(function, "name"), source);
}

std::pair<std::size_t, std::size_t> CallableSpan(TSNode function,
                                                 Language language) {
  if (language == Language::kPython) {
    const TSNode parent = ts_node_parent(function);
    if (IsNodeType(parent, "decorated_definition")) {
      return {ts_node_start_byte(parent), ts_node_end_byte(parent)};
    }
  }
  return {ts_node_start_byte(function), ts_node_end_byte(function)};
}

void CollectFunctions(TSNode node, std::string_view source, Language language,
                      bool inside_function,
                      std::vector<FunctionSpan>& functions) {
  const LanguageMetadata& metadata = Metadata(language);
  const bool reportable = HasKind(metadata.functions, ts_node_type(node));
  const bool callable = HasKind(metadata.callables, ts_node_type(node));
  if (reportable && !inside_function && IsDefinition(node, language)) {
    auto [start, end] = CallableSpan(node, language);
    functions.push_back({.name = CallableName(node, source, language),
                         .start_byte = start,
                         .end_byte = end});
  }
  const bool child_inside_function = inside_function || callable;
  for (std::uint32_t i = 0; i < ts_node_child_count(node); ++i) {
    CollectFunctions(ts_node_child(node, i), source, language,
                     child_inside_function, functions);
  }
}

void AppendSource(std::string_view source, std::size_t start, std::size_t end,
                  std::string& output, OffsetMap& map) {
  output.append(source.substr(start, end - start));
  for (std::size_t i = start; i < end; ++i) {
    map.push_back(i);
  }
}

bool EndsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

}  // namespace

std::pair<std::string, OffsetMap> StripComments(std::string_view source,
                                                Language language) {
  Tree tree = Parse(source, language);
  std::vector<std::pair<std::size_t, std::size_t>> ranges;
  CollectCommentRanges(ts_tree_root_node(tree.get()),
                       Metadata(language).comments, ranges);
  std::ranges::sort(ranges);

  std::string output;
  output.reserve(source.size());
  OffsetMap map;
  map.reserve(source.size() + 1);
  std::size_t cursor = 0;
  for (const auto [start, end] : ranges) {
    if (start < cursor) {
      continue;
    }
    AppendSource(source, cursor, start, output, map);
    bool preserved_newline = false;
    for (std::size_t i = start; i < end; ++i) {
      if (source[i] == '\n') {
        output.push_back('\n');
        map.push_back(i);
        preserved_newline = true;
      }
    }
    if (!preserved_newline && !output.empty() &&
        !std::isspace(static_cast<unsigned char>(output.back())) &&
        end < source.size() &&
        !std::isspace(static_cast<unsigned char>(source[end]))) {
      output.push_back(' ');
      map.push_back(start);
    }
    cursor = end;
  }
  AppendSource(source, cursor, source.size(), output, map);
  map.push_back(source.size());
  return {std::move(output), std::move(map)};
}

std::vector<std::size_t> LineStarts(std::string_view source) {
  std::vector<std::size_t> starts = {0};
  for (std::size_t i = 0; i < source.size(); ++i) {
    if (source[i] == '\n') {
      starts.push_back(i + 1);
    }
  }
  return starts;
}

std::vector<StructuralEvent> StructuralEvents(std::string_view source,
                                              Language language) {
  Tree tree = Parse(source, language);
  std::vector<StructuralEvent> events;
  CollectStructuralEvents(ts_tree_root_node(tree.get()), language, 0,
                          Metadata(language).structural, events);
  std::ranges::sort(events, {}, [](const StructuralEvent& event) {
    return std::tuple(event.byte_offset, event.depth, event.scope_start);
  });
  events.erase(std::ranges::unique(events).begin(), events.end());
  return events;
}

std::vector<FunctionSpan> Functions(std::string_view preprocessed,
                                    Language language) {
  Tree tree = Parse(preprocessed, language);
  std::vector<FunctionSpan> functions;
  CollectFunctions(ts_tree_root_node(tree.get()), preprocessed, language, false,
                   functions);
  return functions;
}

Language ParseLanguage(std::string_view name) {
  for (Language language : kLanguages) {
    const auto& aliases = Metadata(language).aliases;
    if (std::ranges::find(aliases, name) != aliases.end()) {
      return language;
    }
  }
  throw std::invalid_argument("unsupported language '" + std::string(name) +
                              "'; expected " +
                              std::string(kCanonicalLanguageNames));
}

Language InferLanguage(std::string_view path) {
  for (Language language : kLanguages) {
    for (std::string_view extension : Metadata(language).extensions) {
      if (EndsWith(path, extension)) {
        return language;
      }
    }
  }
  throw std::invalid_argument("cannot infer language from source file '" +
                              std::string(path) + "'; pass --lang " +
                              std::string(kCanonicalLanguageNames));
}

std::string_view LanguageName(Language language) {
  return Metadata(language).canonical_name;
}

bool IsHeaderPath(std::string_view path) {
  return std::ranges::any_of(
      std::initializer_list<std::string_view>{".h", ".hpp", ".hh", ".hxx",
                                              ".h++"},
      [path](std::string_view suffix) { return EndsWith(path, suffix); });
}

bool IsSourcePath(std::string_view path, bool include_headers) {
  if (IsHeaderPath(path)) {
    return include_headers;
  }
  return std::ranges::any_of(kLanguages, [path](Language language) {
    return std::ranges::any_of(Metadata(language).extensions,
                               [path](std::string_view extension) {
                                 return EndsWith(path, extension);
                               });
  });
}

}  // namespace llmcc
