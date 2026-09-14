#include "src/lang.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>

#include "src/input_limits.h"

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
constexpr auto kCppExtensions =
    std::to_array<std::string_view>({".cc", ".cpp", ".cxx", ".c++", ".cu",
                                     ".hpp", ".hh", ".hxx", ".h++", ".cuh"});
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
  const auto match =
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

class Deadline {
 public:
  explicit Deadline(const PreprocessOptions& options)
      : budget_(options.time_budget),
        expires_(std::chrono::steady_clock::now() + options.time_budget),
        max_depth_(options.max_syntax_depth) {
    if (options.time_budget.count() < 0) {
      throw std::invalid_argument(
          "preprocessing time budget must be non-negative");
    }
    if (max_depth_ == 0) {
      throw std::invalid_argument("syntax depth limit must be positive");
    }
  }

  void Check(std::string_view stage) {
    if (std::chrono::steady_clock::now() >= expires_) {
      timed_out_ = true;
      throw std::runtime_error("source preprocessing exceeded its " +
                               std::to_string(budget_.count()) +
                               " ms time budget during " + std::string(stage));
    }
  }

  void Tick(std::string_view stage) {
    if ((++ticks_ & 4095U) == 0) {
      Check(stage);
    }
  }

  void CheckDepth(std::size_t depth) const {
    if (depth > max_depth_) {
      throw std::runtime_error(
          "source syntax tree exceeds the preprocessing depth limit of " +
          std::to_string(max_depth_));
    }
  }

  void CheckStructuralDepth(std::size_t depth) const {
    if (depth > max_depth_) {
      throw std::runtime_error(
          "source structural depth exceeds the preprocessing depth limit of " +
          std::to_string(max_depth_));
    }
  }

  static bool ParseProgress(TSParseState* state) {
    auto& deadline = *static_cast<Deadline*>(state->payload);
    if (std::chrono::steady_clock::now() >= deadline.expires_) {
      deadline.timed_out_ = true;
      return true;
    }
    return false;
  }

  [[nodiscard]] bool timed_out() const { return timed_out_; }
  [[nodiscard]] std::chrono::milliseconds budget() const { return budget_; }

 private:
  std::chrono::milliseconds budget_;
  std::chrono::steady_clock::time_point expires_;
  std::size_t max_depth_;
  std::size_t ticks_ = 0;
  bool timed_out_ = false;
};

class TreeCursor {
 public:
  explicit TreeCursor(TSNode node) : cursor_(ts_tree_cursor_new(node)) {}
  ~TreeCursor() { ts_tree_cursor_delete(&cursor_); }
  TreeCursor(const TreeCursor&) = delete;
  TreeCursor& operator=(const TreeCursor&) = delete;

  [[nodiscard]] TSNode node() const {
    return ts_tree_cursor_current_node(&cursor_);
  }
  [[nodiscard]] const char* field_name() const {
    return ts_tree_cursor_current_field_name(&cursor_);
  }
  bool FirstChild() { return ts_tree_cursor_goto_first_child(&cursor_); }
  bool NextSibling() { return ts_tree_cursor_goto_next_sibling(&cursor_); }
  bool Parent() { return ts_tree_cursor_goto_parent(&cursor_); }

 private:
  TSTreeCursor cursor_;
};

struct SourceInput {
  std::string_view source;
};

const char* ReadSource(void* payload, std::uint32_t byte_index,
                       TSPoint position, std::uint32_t* bytes_read) {
  static_cast<void>(position);
  const auto source = static_cast<SourceInput*>(payload)->source;
  if (byte_index >= source.size()) {
    *bytes_read = 0;
    return nullptr;
  }
  constexpr std::size_t kChunkBytes = std::size_t{64} * 1024;
  *bytes_read = static_cast<std::uint32_t>(
      std::min(kChunkBytes, source.size() - byte_index));
  return source.data() + byte_index;
}

Tree Parse(std::string_view source, Language language, Deadline& deadline) {
  CheckSourceSize(source.size());
  if (source.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("source is too large for tree-sitter");
  }
  deadline.Check("parser setup");
  Parser parser(ts_parser_new(), ts_parser_delete);
  if (!parser ||
      !ts_parser_set_language(parser.get(), Metadata(language).grammar())) {
    throw std::runtime_error("tree-sitter language ABI is incompatible");
  }
  SourceInput payload{.source = source};
  const TSInput input{.payload = &payload,
                      .read = ReadSource,
                      .encoding = TSInputEncodingUTF8,
                      .decode = nullptr};
  const TSParseOptions parse_options{
      .payload = &deadline, .progress_callback = Deadline::ParseProgress};
  Tree tree(
      ts_parser_parse_with_options(parser.get(), nullptr, input, parse_options),
      ts_tree_delete);
  if (!tree) {
    if (deadline.timed_out()) {
      throw std::runtime_error("source preprocessing exceeded its " +
                               std::to_string(deadline.budget().count()) +
                               " ms time budget during tree-sitter parsing");
    }
    throw std::runtime_error("tree-sitter parser returned no tree");
  }
  deadline.Check("parsing");
  return tree;
}

void CollectCommentRanges(
    TSNode node, std::span<const std::string_view> comment_kinds,
    std::vector<std::pair<std::size_t, std::size_t>>& ranges,
    Deadline& deadline) {
  TreeCursor cursor(node);
  std::size_t depth = 0;
  while (true) {
    deadline.Tick("comment collection");
    const TSNode current = cursor.node();
    const bool comment = HasKind(comment_kinds, ts_node_type(current));
    if (comment) {
      ranges.emplace_back(ts_node_start_byte(current),
                          ts_node_end_byte(current));
    }
    if (!comment && cursor.FirstChild()) {
      deadline.CheckDepth(++depth);
      continue;
    }
    while (!cursor.NextSibling()) {
      if (depth == 0) {
        return;
      }
      static_cast<void>(cursor.Parent());
      --depth;
    }
  }
}

bool IsPythonStringNode(TSNode node, std::string_view source,
                        Deadline& deadline) {
  const std::size_t start = ts_node_start_byte(node);
  const std::size_t end = ts_node_end_byte(node);
  if (start >= end || end > source.size()) {
    return false;
  }
  // Python docstrings are str constants.  Bytes and f-strings are expressions
  // but are not docstrings; raw and unicode prefixes remain valid.
  for (char byte : source.substr(start, end - start)) {
    deadline.Tick("Python docstring detection");
    if (byte == '\'' || byte == '"') {
      return true;
    }
    const char prefix =
        static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
    if (prefix == 'b' || prefix == 'f') {
      return false;
    }
    if (prefix != 'r' && prefix != 'u' &&
        !std::isspace(static_cast<unsigned char>(byte))) {
      return false;
    }
  }
  return false;
}

bool IsPythonLiteralString(TSNode node, std::string_view source,
                           Deadline& deadline) {
  struct Pending {
    TSNode node;
    std::size_t depth;
  };
  std::vector<Pending> pending = {{.node = node, .depth = 0}};
  while (!pending.empty()) {
    const Pending item = pending.back();
    pending.pop_back();
    deadline.Tick("Python docstring detection");
    deadline.CheckDepth(item.depth);
    const std::string_view type = ts_node_type(item.node);
    if (type == "string") {
      if (!IsPythonStringNode(item.node, source, deadline)) {
        return false;
      }
      continue;
    }
    const bool parenthesized = type == "parenthesized_expression";
    if (!parenthesized && type != "concatenated_string") {
      return false;
    }
    bool saw_string = false;
    TreeCursor children(item.node);
    if (children.FirstChild()) {
      do {
        const TSNode child = children.node();
        if (!ts_node_is_named(child) || IsNodeType(child, "comment")) {
          continue;
        }
        if (parenthesized && saw_string) {
          return false;
        }
        saw_string = true;
        pending.push_back({.node = child, .depth = item.depth + 1});
      } while (children.NextSibling());
    }
    if (!saw_string) {
      return false;
    }
  }
  return true;
}

void CollectPythonSuiteDocstring(
    TSNode suite, std::string_view source,
    std::vector<std::pair<std::size_t, std::size_t>>& ranges,
    Deadline& deadline) {
  TreeCursor statements(suite);
  if (!statements.FirstChild()) {
    return;
  }
  do {
    deadline.Tick("Python docstring collection");
    const TSNode statement = statements.node();
    if (!ts_node_is_named(statement)) {
      continue;
    }
    if (IsNodeType(statement, "comment")) {
      continue;
    }
    if (!IsNodeType(statement, "expression_statement")) {
      return;
    }
    TSNode expression{};
    std::size_t named_children = 0;
    TreeCursor children(statement);
    if (children.FirstChild()) {
      do {
        if (ts_node_is_named(children.node())) {
          expression = children.node();
          ++named_children;
        }
      } while (children.NextSibling());
    }
    if (named_children != 1 ||
        !IsPythonLiteralString(expression, source, deadline)) {
      return;
    }
    ranges.emplace_back(ts_node_start_byte(statement),
                        ts_node_end_byte(statement));
    return;
  } while (statements.NextSibling());
}

TSNode Field(TSNode node, std::string_view name);
bool IsDefinition(TSNode function, Language language);
std::string CallableName(TSNode function, std::string_view source,
                         Language language);
std::pair<std::size_t, std::size_t> CallableSpan(TSNode function,
                                                 Language language);

void CollectStructuralEvents(
    TSNode node, Language language, std::size_t structural_depth,
    std::span<const std::string_view> structural_kinds,
    std::vector<StructuralEvent>& events, Deadline& deadline,
    std::string_view source = {},
    std::vector<std::pair<std::size_t, std::size_t>>* removals = nullptr,
    std::vector<FunctionSpan>* functions = nullptr) {
  struct Frame {
    std::size_t structural_depth;
    std::size_t child_depth;
    std::size_t comprehension_depth;
    std::size_t comprehension_clause_count;
    TSNode comprehension_body;
    bool python_comprehension;
    bool python_comprehension_loop;
    bool python_comprehension_filter;
    bool child_inside_function;
    bool skip_children;
  };

  const LanguageMetadata& metadata = Metadata(language);
  if (removals != nullptr && language == Language::kPython) {
    CollectPythonSuiteDocstring(node, source, *removals, deadline);
  }
  auto Enter = [&](TSNode current, std::size_t depth, bool inside_function) {
    deadline.Tick("source metadata traversal");
    deadline.CheckStructuralDepth(depth);
    const bool comment = removals != nullptr &&
                         HasKind(metadata.comments, ts_node_type(current));
    if (comment) {
      removals->emplace_back(ts_node_start_byte(current),
                             ts_node_end_byte(current));
    }
    if (removals != nullptr && language == Language::kPython &&
        (IsNodeType(current, "function_definition") ||
         IsNodeType(current, "class_definition"))) {
      const TSNode body = Field(current, "body");
      if (!ts_node_is_null(body)) {
        CollectPythonSuiteDocstring(body, source, *removals, deadline);
      }
    }
    const bool callable = HasKind(metadata.callables, ts_node_type(current));
    if (functions != nullptr && !inside_function &&
        HasKind(metadata.functions, ts_node_type(current)) &&
        IsDefinition(current, language)) {
      auto [start, end] = CallableSpan(current, language);
      functions->push_back({.name = CallableName(current, source, language),
                            .start_byte = start,
                            .end_byte = end});
    }
    const bool structural = HasKind(structural_kinds, ts_node_type(current));
    const std::size_t start = ts_node_start_byte(current);
    const std::size_t end = ts_node_end_byte(current);
    if (structural && start < end) {
      events.push_back(
          {.scope_start = start, .byte_offset = end, .depth = depth});
    }
    const std::size_t child_depth = depth + (structural ? 1 : 0);
    const bool comprehension =
        language == Language::kPython &&
        HasKind(kPythonComprehensions, ts_node_type(current));
    std::size_t clause_count = 0;
    if (comprehension) {
      TreeCursor children(current);
      if (children.FirstChild()) {
        do {
          deadline.Tick("source metadata traversal");
          const TSNode child = children.node();
          if (IsNodeType(child, "for_in_clause") ||
              IsNodeType(child, "if_clause")) {
            ++clause_count;
          }
        } while (children.NextSibling());
      }
    }
    return Frame{
        .structural_depth = depth,
        .child_depth = child_depth,
        .comprehension_depth = child_depth,
        .comprehension_clause_count = clause_count,
        .comprehension_body = comprehension
                                  ? ts_node_child_by_field_name(
                                        current, "body", sizeof("body") - 1)
                                  : TSNode{},
        .python_comprehension = comprehension,
        .python_comprehension_loop = language == Language::kPython &&
                                     IsNodeType(current, "for_in_clause"),
        .python_comprehension_filter =
            language == Language::kPython && IsNodeType(current, "if_clause"),
        .child_inside_function = inside_function || callable,
        .skip_children = comment};
  };

  auto ChildDepth = [](const Frame& parent, TSNode child,
                       const char* field_name) {
    std::size_t depth = parent.comprehension_depth;
    if (parent.python_comprehension &&
        ts_node_eq(child, parent.comprehension_body)) {
      depth = parent.child_depth + parent.comprehension_clause_count;
    }
    if (parent.python_comprehension_loop && field_name != nullptr &&
        std::string_view(field_name) == "right") {
      depth = parent.structural_depth;
    }
    if (parent.python_comprehension_filter) {
      depth = parent.structural_depth;
    }
    return depth;
  };

  TreeCursor cursor(node);
  std::vector<Frame> frames;
  frames.reserve(64);
  frames.push_back(Enter(node, structural_depth, false));
  while (true) {
    if (!frames.back().skip_children && cursor.FirstChild()) {
      deadline.CheckDepth(frames.size());
      const Frame& parent = frames.back();
      const TSNode child = cursor.node();
      const char* field_name =
          parent.python_comprehension_loop ? cursor.field_name() : nullptr;
      frames.push_back(Enter(child, ChildDepth(parent, child, field_name),
                             parent.child_inside_function));
      continue;
    }
    while (true) {
      const TSNode finished = cursor.node();
      frames.pop_back();
      if (frames.empty()) {
        return;
      }
      Frame& parent = frames.back();
      if (parent.python_comprehension &&
          (IsNodeType(finished, "for_in_clause") ||
           IsNodeType(finished, "if_clause"))) {
        ++parent.comprehension_depth;
      }
      if (cursor.NextSibling()) {
        const TSNode sibling = cursor.node();
        const char* field_name =
            parent.python_comprehension_loop ? cursor.field_name() : nullptr;
        frames.push_back(Enter(sibling, ChildDepth(parent, sibling, field_name),
                               parent.child_inside_function));
        break;
      }
      static_cast<void>(cursor.Parent());
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
      TreeCursor children(function);
      if (children.FirstChild()) {
        do {
          const TSNode child = children.node();
          if (IsNodeType(child, "implicit") || IsNodeType(child, "explicit")) {
            kind = NodeText(child, source);
            break;
          }
        } while (children.NextSibling());
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
                      std::vector<FunctionSpan>& functions,
                      Deadline& deadline) {
  const LanguageMetadata& metadata = Metadata(language);
  TreeCursor cursor(node);
  std::vector<bool> inside;
  inside.reserve(64);
  inside.push_back(inside_function);
  while (true) {
    deadline.Tick("callable traversal");
    const TSNode current = cursor.node();
    const bool current_inside = inside.back();
    const bool reportable = HasKind(metadata.functions, ts_node_type(current));
    const bool callable = HasKind(metadata.callables, ts_node_type(current));
    if (reportable && !current_inside && IsDefinition(current, language)) {
      auto [start, end] = CallableSpan(current, language);
      functions.push_back({.name = CallableName(current, source, language),
                           .start_byte = start,
                           .end_byte = end});
    }
    if (cursor.FirstChild()) {
      deadline.CheckDepth(inside.size());
      inside.push_back(current_inside || callable);
      continue;
    }
    while (!cursor.NextSibling()) {
      inside.pop_back();
      if (inside.empty()) {
        return;
      }
      static_cast<void>(cursor.Parent());
    }
  }
}

void AppendSource(std::string_view source, std::size_t start, std::size_t end,
                  std::string& output, OffsetMap& map) {
  output.append(source.substr(start, end - start));
  map.AppendRun(start, end - start);
}

std::size_t RemapOffset(const OffsetMap& map, std::size_t original) {
  return map.CleanedOffset(original);
}

std::pair<std::string, OffsetMap> RemoveRanges(
    std::string_view source,
    std::vector<std::pair<std::size_t, std::size_t>> ranges,
    Deadline& deadline) {
  deadline.Check("range removal");
  std::ranges::sort(ranges);
  std::string output;
  output.reserve(source.size());
  OffsetMap map;
  std::size_t cursor = 0;
  for (const auto [start, end] : ranges) {
    deadline.Tick("range removal");
    if (start < cursor || start > end || end > source.size()) {
      continue;
    }
    AppendSource(source, cursor, start, output, map);
    bool preserved_newline = false;
    for (std::size_t i = start; i < end; ++i) {
      if (source[i] == '\n') {
        output.push_back('\n');
        map.Append(i);
        preserved_newline = true;
      }
      deadline.Tick("range removal");
    }
    if (!preserved_newline && !output.empty() &&
        !std::isspace(static_cast<unsigned char>(output.back())) &&
        end < source.size() &&
        !std::isspace(static_cast<unsigned char>(source[end]))) {
      output.push_back(' ');
      map.Append(start);
    }
    cursor = end;
  }
  AppendSource(source, cursor, source.size(), output, map);
  map.Append(source.size());
  deadline.Check("range removal");
  return {std::move(output), std::move(map)};
}

std::vector<std::size_t> CollectLineStarts(std::string_view source,
                                           Deadline* deadline) {
  std::vector<std::size_t> starts = {0};
  for (std::size_t i = 0; i < source.size(); ++i) {
    if (deadline != nullptr) {
      deadline->Tick("line indexing");
    }
    if (source[i] == '\n') {
      starts.push_back(i + 1);
    }
  }
  return starts;
}

bool EndsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

}  // namespace

std::pair<std::string, OffsetMap> StripComments(std::string_view source,
                                                Language language,
                                                PreprocessOptions options) {
  Deadline deadline(options);
  Tree tree = Parse(source, language, deadline);
  std::vector<std::pair<std::size_t, std::size_t>> ranges;
  CollectCommentRanges(ts_tree_root_node(tree.get()),
                       Metadata(language).comments, ranges, deadline);
  return RemoveRanges(source, std::move(ranges), deadline);
}

PreparedSource PrepareSource(std::string_view source, Language language,
                             PreprocessOptions options) {
  Deadline deadline(options);
  Tree tree = Parse(source, language, deadline);
  const TSNode root = ts_tree_root_node(tree.get());

  std::vector<std::pair<std::size_t, std::size_t>> removals;
  std::vector<StructuralEvent> original_events;
  std::vector<FunctionSpan> original_functions;
  CollectStructuralEvents(root, language, 0, Metadata(language).structural,
                          original_events, deadline, source, &removals,
                          &original_functions);
  // All tree-derived data now owns its bytes and offsets. Release tree-sitter's
  // comparatively large syntax tree before allocating the cleaned source and
  // line/range indices.
  tree.reset();

  auto [cleaned, map] = RemoveRanges(source, std::move(removals), deadline);
  PreparedSource prepared{.cleaned = std::move(cleaned),
                          .original_offsets = std::move(map)};
  prepared.line_starts = CollectLineStarts(prepared.cleaned, &deadline);

  for (const StructuralEvent& event : original_events) {
    deadline.Tick("offset remapping");
    const std::size_t start =
        RemapOffset(prepared.original_offsets, event.scope_start);
    const std::size_t end =
        RemapOffset(prepared.original_offsets, event.byte_offset);
    if (start < end) {
      prepared.structural_events.push_back(
          {.scope_start = start, .byte_offset = end, .depth = event.depth});
    }
  }
  std::ranges::sort(
      prepared.structural_events, {}, [](const StructuralEvent& event) {
        return std::tuple(event.scope_start, event.byte_offset, event.depth);
      });
  prepared.structural_events.erase(
      std::ranges::unique(prepared.structural_events).begin(),
      prepared.structural_events.end());

  for (FunctionSpan function : original_functions) {
    deadline.Tick("offset remapping");
    function.start_byte =
        RemapOffset(prepared.original_offsets, function.start_byte);
    function.end_byte =
        RemapOffset(prepared.original_offsets, function.end_byte);
    if (function.start_byte < function.end_byte) {
      prepared.functions.push_back(std::move(function));
    }
  }

  std::size_t line_start = 0;
  while (line_start < prepared.cleaned.size()) {
    deadline.Tick("meaningful-line detection");
    const std::size_t newline = prepared.cleaned.find('\n', line_start);
    const std::size_t line_end =
        newline == std::string::npos ? prepared.cleaned.size() : newline;
    std::size_t first = line_start;
    while (first < line_end &&
           std::isspace(static_cast<unsigned char>(prepared.cleaned[first]))) {
      deadline.Tick("meaningful-line detection");
      ++first;
    }
    std::size_t end = line_end;
    while (end > first && std::isspace(static_cast<unsigned char>(
                              prepared.cleaned[end - 1]))) {
      deadline.Tick("meaningful-line detection");
      --end;
    }
    if (first < end) {
      prepared.meaningful_ranges.push_back({first, end});
    }
    if (newline == std::string::npos) {
      break;
    }
    line_start = newline + 1;
  }
  deadline.Check("preprocessing completion");
  return prepared;
}

std::vector<std::size_t> LineStarts(std::string_view source) {
  return CollectLineStarts(source, nullptr);
}

std::vector<StructuralEvent> StructuralEvents(std::string_view source,
                                              Language language,
                                              PreprocessOptions options) {
  Deadline deadline(options);
  Tree tree = Parse(source, language, deadline);
  std::vector<StructuralEvent> events;
  CollectStructuralEvents(ts_tree_root_node(tree.get()), language, 0,
                          Metadata(language).structural, events, deadline);
  std::ranges::sort(events, {}, [](const StructuralEvent& event) {
    return std::tuple(event.byte_offset, event.depth, event.scope_start);
  });
  events.erase(std::ranges::unique(events).begin(), events.end());
  return events;
}

std::vector<FunctionSpan> Functions(std::string_view preprocessed,
                                    Language language,
                                    PreprocessOptions options) {
  Deadline deadline(options);
  Tree tree = Parse(preprocessed, language, deadline);
  std::vector<FunctionSpan> functions;
  CollectFunctions(ts_tree_root_node(tree.get()), preprocessed, language, false,
                   functions, deadline);
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
                                              ".h++", ".cuh"},
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
