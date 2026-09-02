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

const TSLanguage* Grammar(Language language) {
  switch (language) {
    case Language::kRust:
      return tree_sitter_rust();
    case Language::kC:
      return tree_sitter_c();
    case Language::kCpp:
      return tree_sitter_cpp();
  }
  throw std::logic_error("unknown source language");
}

std::span<const std::string_view> CommentKinds(Language language) {
  if (language == Language::kRust) {
    return kRustComments;
  }
  return kCComments;
}

std::span<const std::string_view> StructuralKinds(Language language) {
  switch (language) {
    case Language::kRust:
      return kRustStructural;
    case Language::kC:
      return kCStructural;
    case Language::kCpp:
      return kCppStructural;
  }
  throw std::logic_error("unknown source language");
}

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
  if (!parser || !ts_parser_set_language(parser.get(), Grammar(language))) {
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

void CollectStructuralEvents(TSNode node, std::size_t structural_depth,
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
  for (std::uint32_t i = 0; i < ts_node_child_count(node); ++i) {
    CollectStructuralEvents(ts_node_child(node, i), child_depth,
                            structural_kinds, events);
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
       "destructor_name", "operator_name", "template_function"});
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

void CollectFunctions(TSNode node, std::string_view source, Language language,
                      bool inside_function,
                      std::vector<FunctionSpan>& functions) {
  const bool function =
      IsNodeType(node, language == Language::kRust ? "function_item"
                                                   : "function_definition");
  if (function && !inside_function) {
    std::string name;
    if (language == Language::kRust) {
      name = NodeText(
          ts_node_child_by_field_name(node, "name", sizeof("name") - 1),
          source);
    } else {
      name = CFunctionName(node, source);
    }
    functions.push_back({.name = std::move(name),
                         .start_byte = ts_node_start_byte(node),
                         .end_byte = ts_node_end_byte(node)});
  }
  const bool child_inside_function = inside_function || function;
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
  CollectCommentRanges(ts_tree_root_node(tree.get()), CommentKinds(language),
                       ranges);
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
  CollectStructuralEvents(ts_tree_root_node(tree.get()), 0,
                          StructuralKinds(language), events);
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
  if (name == "rust") {
    return Language::kRust;
  }
  if (name == "c") {
    return Language::kC;
  }
  if (name == "cpp" || name == "c++") {
    return Language::kCpp;
  }
  throw std::invalid_argument("unsupported language '" + std::string(name) +
                              "'; expected rust, c, cpp, or c++");
}

Language InferLanguage(std::string_view path) {
  if (EndsWith(path, ".rs")) {
    return Language::kRust;
  }
  if (EndsWith(path, ".c") || EndsWith(path, ".h")) {
    return Language::kC;
  }
  for (std::string_view suffix :
       {".cc", ".cpp", ".cxx", ".c++", ".hpp", ".hh", ".hxx", ".h++"}) {
    if (EndsWith(path, suffix)) {
      return Language::kCpp;
    }
  }
  throw std::invalid_argument("cannot infer language from source file '" +
                              std::string(path) +
                              "'; pass --lang rust, c, cpp, or c++");
}

std::string_view LanguageName(Language language) {
  switch (language) {
    case Language::kRust:
      return "rust";
    case Language::kC:
      return "c";
    case Language::kCpp:
      return "cpp";
  }
  throw std::logic_error("unknown source language");
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
  return std::ranges::any_of(
      std::initializer_list<std::string_view>{".rs", ".c", ".cc", ".cpp",
                                              ".cxx", ".c++"},
      [path](std::string_view suffix) { return EndsWith(path, suffix); });
}

}  // namespace llmcc
