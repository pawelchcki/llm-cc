#ifndef LLM_CC_OFFSET_MAP_H_
#define LLM_CC_OFFSET_MAP_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace llmcc {

// A monotonic mapping from each byte boundary in preprocessed source back to
// the corresponding byte boundary in the original source.  Retained runs are
// represented by one small arithmetic span instead of one size_t per byte.
class OffsetMap {
 public:
  struct Span {
    std::uint32_t cleaned_start;
    std::uint32_t original_start;
    std::uint32_t length;
    std::uint32_t original_stride;
  };

  [[nodiscard]] bool empty() const { return size_ == 0; }
  [[nodiscard]] std::size_t size() const { return size_; }
  [[nodiscard]] std::size_t span_count() const { return spans_.size(); }

  [[nodiscard]] std::size_t at(std::size_t cleaned_offset) const {
    if (cleaned_offset >= size_) {
      throw std::out_of_range("preprocessing offset is outside the map");
    }
    return OriginalOffset(cleaned_offset);
  }

  [[nodiscard]] std::size_t operator[](std::size_t cleaned_offset) const {
    return OriginalOffset(cleaned_offset);
  }

  [[nodiscard]] std::size_t back() const {
    if (empty()) {
      throw std::out_of_range("preprocessing offset map is empty");
    }
    return OriginalOffset(size_ - 1);
  }

  // Maps a cleaned byte boundary to its original byte boundary.
  [[nodiscard]] std::size_t OriginalOffset(std::size_t cleaned_offset) const {
    const auto match = std::ranges::upper_bound(
        spans_, cleaned_offset, {},
        [](const Span& span) { return std::size_t{span.cleaned_start}; });
    if (match == spans_.begin()) {
      throw std::out_of_range("preprocessing offset is outside the map");
    }
    const Span& span = *std::prev(match);
    const std::size_t relative = cleaned_offset - span.cleaned_start;
    if (relative >= span.length) {
      throw std::out_of_range("preprocessing offset is outside the map");
    }
    return std::size_t{span.original_start} + (relative * span.original_stride);
  }

  // Equivalent to lower_bound on the former dense mapping: returns the first
  // cleaned boundary whose original boundary is >= original_offset.
  [[nodiscard]] std::size_t CleanedOffset(std::size_t original_offset) const {
    const auto match = std::ranges::lower_bound(
        spans_, original_offset, {}, [](const Span& span) {
          return std::size_t{span.original_start} +
                 (std::size_t{span.length - 1} * span.original_stride);
        });
    if (match == spans_.end()) {
      return size_;
    }
    if (original_offset <= match->original_start) {
      return match->cleaned_start;
    }
    if (match->original_stride == 0) {
      return match->cleaned_start;
    }
    const std::size_t distance = original_offset - match->original_start;
    return std::size_t{match->cleaned_start} +
           ((distance + match->original_stride - 1) / match->original_stride);
  }

  // Builder operation used while stripping source. Values must be monotonic
  // and are coalesced whenever they continue the previous retained run.
  void Append(std::size_t original_offset) {
    if (size_ > UINT32_MAX || original_offset > UINT32_MAX) {
      throw std::length_error("preprocessing offset does not fit tree-sitter");
    }
    if (!spans_.empty()) {
      Span& last = spans_.back();
      const std::size_t last_original =
          std::size_t{last.original_start} +
          (std::size_t{last.length - 1} * last.original_stride);
      if (original_offset < last_original) {
        throw std::logic_error("preprocessing offset map is not monotonic");
      }
      const std::size_t stride = original_offset - last_original;
      if (std::size_t{last.cleaned_start} + last.length == size_ &&
          last.length < UINT32_MAX &&
          ((last.length == 1 && stride <= UINT32_MAX) ||
           stride == last.original_stride)) {
        if (last.length == 1) {
          last.original_stride = static_cast<std::uint32_t>(stride);
        }
        ++last.length;
        ++size_;
        return;
      }
    }
    spans_.push_back(
        {.cleaned_start = static_cast<std::uint32_t>(size_),
         .original_start = static_cast<std::uint32_t>(original_offset),
         .length = 1,
         .original_stride = 1});
    ++size_;
  }

  void AppendRun(std::size_t original_offset, std::size_t length) {
    if (length == 0) {
      return;
    }
    if (size_ > UINT32_MAX || original_offset > UINT32_MAX ||
        length > UINT32_MAX || size_ + length > UINT32_MAX ||
        original_offset + length > std::size_t{UINT32_MAX} + 1) {
      throw std::length_error("preprocessing offset does not fit tree-sitter");
    }
    if (!spans_.empty()) {
      Span& last = spans_.back();
      if (last.original_stride == 1 &&
          std::size_t{last.cleaned_start} + last.length == size_ &&
          std::size_t{last.original_start} + last.length == original_offset &&
          std::size_t{last.length} + length <= UINT32_MAX) {
        last.length += static_cast<std::uint32_t>(length);
        size_ += length;
        return;
      }
      const std::size_t previous =
          std::size_t{last.original_start} +
          (std::size_t{last.length - 1} * last.original_stride);
      if (original_offset < previous) {
        throw std::logic_error("preprocessing offset map is not monotonic");
      }
    }
    spans_.push_back(
        {.cleaned_start = static_cast<std::uint32_t>(size_),
         .original_start = static_cast<std::uint32_t>(original_offset),
         .length = static_cast<std::uint32_t>(length),
         .original_stride = 1});
    size_ += length;
  }

 private:
  std::vector<Span> spans_;
  std::size_t size_ = 0;
};

}  // namespace llmcc

#endif  // LLM_CC_OFFSET_MAP_H_
