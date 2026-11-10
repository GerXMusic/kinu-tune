#pragma once

#include <algorithm>
#include <map>
#include <utility>

// D-207: per-frontier controls for the transition between two contiguous notes.
//
// widthMs       0..20 ms per side; 0 means "leave D-205 untouched".
// amountPercent -100 = cubic Bezier replacement, 0 = original curve (no-op),
//                +100 = cosine smoothing over the same window.
//
// The key is the pair of immutable source start frames of the two notes, so the
// setting follows a note that is moved in the output timeline. The store never
// touches Note, keeping the public per-note API unchanged.
struct BoundaryTransition
{
    float widthMs = 0.0f;
    float amountPercent = 0.0f;
};

class BoundaryTransitionStore
{
public:
    using Key = std::pair<int, int>;

    void set(int leftSrcStartFrame, int rightSrcStartFrame,
             float widthMs, float amountPercent)
    {
        const Key key{leftSrcStartFrame, rightSrcStartFrame};
        if (widthMs <= 0.0f || amountPercent == 0.0f)
        {
            values.erase(key);
            return;
        }
        BoundaryTransition value;
        value.widthMs = std::clamp(widthMs, 0.0f, 20.0f);
        value.amountPercent = std::clamp(amountPercent, -100.0f, 100.0f);
        values[key] = value;
    }

    bool get(int leftSrcStartFrame, int rightSrcStartFrame,
             BoundaryTransition& out) const
    {
        const auto it = values.find({leftSrcStartFrame, rightSrcStartFrame});
        if (it == values.end())
            return false;
        out = it->second;
        return true;
    }

    void remove(int leftSrcStartFrame, int rightSrcStartFrame)
    {
        values.erase({leftSrcStartFrame, rightSrcStartFrame});
    }

    void clear() { values.clear(); }
    bool empty() const { return values.empty(); }
    size_t size() const { return values.size(); }
    const std::map<Key, BoundaryTransition> &all() const { return values; }

private:
    std::map<Key, BoundaryTransition> values;
};
