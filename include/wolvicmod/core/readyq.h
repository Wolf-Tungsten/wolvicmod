#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace wolvicmod::detail {

// Push-based ready queue for dirty-driven scheduling (§5.2 fast path).
//
// The poll form (every action compares its reads' dirty generations against
// its own last-run stamp each round) costs O(actions × reads) per round even
// when nothing changed, and N actions reading the same variable repeat the
// same check N times with different stamps — it cannot be shared. Pushing
// inverts this: a change is detected once at the write site (the == compare
// there) and notifies its dependent items once, so quiescent cones cost
// nothing and the shared-read check is merged into a single dispatch.
//
// Items are execOrder positions (an SCC group occupies one position).
// Buckets are indexed by topological level and drained in ascending level
// order: a producer always runs before its consumers within a round, each
// item runs at most once per round (pushes during the drain land at strictly
// higher levels and are picked up by the same pass), and empty levels cost
// one bitmap word each.
struct ReadyQueue {
    std::vector<std::vector<uint32_t>> buckets;  // by topo level
    std::vector<uint64_t> levelBits;             // non-empty level bitmap
    std::vector<uint32_t> levelOf;               // exec position -> topo level
    std::vector<char> queued;                    // per-position dedup flag

    void init(std::vector<uint32_t>&& levels) {
        uint32_t nLevels = 0;
        for (uint32_t l : levels) nLevels = std::max(nLevels, l + 1);
        buckets.assign(nLevels, {});
        levelBits.assign((nLevels + 63) / 64, 0);
        queued.assign(levels.size(), 0);
        levelOf = std::move(levels);
    }

    void push(uint32_t pos) {
        if (queued[pos]) return;
        queued[pos] = 1;
        const uint32_t l = levelOf[pos];
        buckets[l].push_back(pos);
        levelBits[l >> 6] |= (uint64_t{1} << (l & 63));
    }

    // Reverse-map fan-out: called from Entity::markDirty with the dependent
    // exec positions of the changed entity.
    void pushDeps(const uint32_t* deps, uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) push(deps[i]);
    }

    // Drain in ascending level order; fn(execPos) runs one item. Bits set
    // while draining belong to strictly higher levels and are consumed by
    // the same pass (the word is re-read after every bucket). The bucket
    // pass is index-based and clears the level bit only afterwards, so even
    // a same-level push (SCC self-notification — filtered from the reverse
    // map, but the drain stays correct if one slips through) is run and its
    // flag cleared instead of being wiped by clear() with the flag stuck.
    template <class F>
    void drain(F&& fn) {
        for (size_t wIdx = 0; wIdx < levelBits.size(); ++wIdx) {
            while (levelBits[wIdx] != 0) {
                const uint32_t l = static_cast<uint32_t>(wIdx * 64) +
                                   static_cast<uint32_t>(__builtin_ctzll(levelBits[wIdx]));
                auto& b = buckets[l];
                for (size_t i = 0; i < b.size(); ++i) {  // live size: same-level appends included
                    const uint32_t pos = b[i];
                    queued[pos] = 0;
                    fn(pos);
                }
                b.clear();
                levelBits[wIdx] &= levelBits[wIdx] - 1;
            }
        }
    }
};

}  // namespace wolvicmod::detail
