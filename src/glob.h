#ifndef LLM_CC_GLOB_H_
#define LLM_CC_GLOB_H_

#include <string>
#include <string_view>
#include <vector>

namespace llmcc {

// An anchored, segment-aware path glob with gitignore-like semantics. Paths
// are '/'-separated and relative to a repository root, without a leading '/'.
//
//   *      any run of characters within one segment
//   ?      one character within one segment
//   [a-z]  one character from a set; [!a-z] or [^a-z] negates it
//   \x     the character x itself
//   **     as a whole segment: zero or more directories. A trailing /** matches
//          everything inside a directory, but not the directory itself.
//
// '*' and '?' never cross '/', '**' inside a longer segment is a plain '*',
// and matching is case-sensitive on every platform.
class Glob {
 public:
  // Throws std::invalid_argument for an empty, rooted, or malformed pattern.
  static Glob Compile(std::string_view pattern);

  [[nodiscard]] bool Matches(std::string_view path) const;
  // True when every path strictly beneath `directory` matches, so a walk can
  // skip the directory. This may conservatively answer false.
  [[nodiscard]] bool PrunesDirectory(std::string_view directory) const;
  [[nodiscard]] const std::string& pattern() const { return pattern_; }

 private:
  std::string pattern_;
  std::vector<std::string> segments_;
  // The pattern without its trailing "**" when it ends in one.
  std::vector<std::string> prune_prefix_;
  bool prunes_ = false;
};

}  // namespace llmcc

#endif  // LLM_CC_GLOB_H_
