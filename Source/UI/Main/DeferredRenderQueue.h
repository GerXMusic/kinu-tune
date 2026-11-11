#pragma once

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

// P1b (render al Play): coalescing queue for dirty render blocks.
//
// Every editing trigger (pitch edit, parameter change, engine change, MCP,
// undo/redo, DAW automation) only marks the blocks it changed here. Nothing
// renders on the edit itself; the render happens once per dirty block when the
// transport starts (flush), or when the document is saved.
//
// Blocks are frame ranges [start, end). Overlapping or adjacent ranges are
// merged, so two edits inside the same silence-bounded block cost one render.
// The queue never decides *how* a block is rendered: the flush callback does,
// and it is called exactly once per pending block.
class DeferredRenderQueue {
public:
  void markBlock(int startFrame, int endFrame) {
    if (endFrame <= startFrame)
      return;
    for (auto &block : blocks) {
      if (startFrame <= block.second && endFrame >= block.first) {
        // Overlap or touch: merge in place, then re-check against the others.
        block.first = std::min(block.first, startFrame);
        block.second = std::max(block.second, endFrame);
        mergeOverlapping();
        return;
      }
    }
    blocks.emplace_back(startFrame, endFrame);
  }

  void clear() { blocks.clear(); }

  bool hasDirty() const { return !blocks.empty(); }

  int dirtyBlockCount() const { return static_cast<int>(blocks.size()); }

  const std::vector<std::pair<int, int>> &pendingBlocks() const {
    return blocks;
  }

  // Renders every pending block exactly once and empties the queue.
  // Returns how many blocks were rendered. Templated so the callback is taken
  // by reference: a std::function would copy the caller's render counter and
  // the test could not see the renders.
  template <typename RenderBlock> int flush(RenderBlock &&renderBlock) {
    const auto pending = blocks;
    blocks.clear();
    for (const auto &block : pending)
      renderBlock(block.first, block.second);
    return static_cast<int>(pending.size());
  }

private:
  void mergeOverlapping() {
    bool merged = true;
    while (merged) {
      merged = false;
      for (std::size_t i = 0; i < blocks.size() && !merged; ++i)
        for (std::size_t j = i + 1; j < blocks.size(); ++j) {
          if (blocks[i].first <= blocks[j].second &&
              blocks[j].first <= blocks[i].second) {
            blocks[i].first = std::min(blocks[i].first, blocks[j].first);
            blocks[i].second = std::max(blocks[i].second, blocks[j].second);
            blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(j));
            merged = true;
            break;
          }
        }
    }
    std::sort(blocks.begin(), blocks.end());
  }

  std::vector<std::pair<int, int>> blocks;
};
