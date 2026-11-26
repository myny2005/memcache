/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemCache_Hybrid is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#ifndef MEM_FABRIC_MMC_PREFIX_TREE_INDEX_H
#define MEM_FABRIC_MMC_PREFIX_TREE_INDEX_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "mmc_types.h"

namespace ock {
namespace mmc {

enum class PrefixTreeRegistrationStatus : uint8_t {
    INSERTED = 0,
    EXISTING = 1,
    PARENT_CONFLICT = 2,
};

struct PrefixTreeRegistration {
    uint64_t generation{0};
    PrefixTreeRegistrationStatus status{PrefixTreeRegistrationStatus::EXISTING};
};

struct PrefixTreeNodeSnapshot {
    std::string parentKey;
    uint64_t generation{0};
    uint64_t lastAccessSequence{0};
    size_t childCountHint{0};
    bool ready{false};
};

struct PrefixTreeCandidate {
    std::string key;
    uint64_t generation{0};
    uint64_t accessSequence{0};
};

struct PrefixTreeIndexStats {
    size_t nodeCount{0};
    size_t orphanCount{0};
    uint64_t parentConflictCount{0};
    uint64_t staleHeapPopCount{0};
    uint64_t leafCandidateCount{0};
};

// A best-effort, sharded index for prefix-aware eviction. It does not own data, validate reads, or execute eviction.
// Nodes may temporarily be roots or orphans. childCountHint is a policy hint only and must never be used as a data
// correctness condition.
class PrefixTreeIndex {
public:
    PrefixTreeIndex() = default;
    PrefixTreeIndex(const PrefixTreeIndex &) = delete;
    PrefixTreeIndex &operator=(const PrefixTreeIndex &) = delete;

    // Registers a pending node. Re-registering the same edge is idempotent. A conflicting parent preserves the first
    // parent and is reported through registration.status.
    Result RegisterPending(const std::string &key, const std::string &parentKey, PrefixTreeRegistration &registration);

    // Marks an existing node readable and makes it eligible for leaf selection if it has no known children.
    Result MarkReady(const std::string &key, uint64_t &generation);

    // Updates recency only for a known ready node. Callers must invoke it only after their normal metadata read path
    // has confirmed the object is readable.
    Result Touch(const std::string &key);

    Result GetSnapshot(const std::string &key, PrefixTreeNodeSnapshot &snapshot) const;
    Result RemoveIfGeneration(const std::string &key, uint64_t generation);

    // Returns a ready candidate leaf after validating its lazy-heap generation and access sequence. No index lock is
    // held when this method returns, so callers must validate the real metadata before acting on the candidate.
    bool PopLeafCandidate(PrefixTreeCandidate &candidate);

    PrefixTreeIndexStats GetStats() const;

private:
    static constexpr size_t kShardCount = 64U;

    struct PrefixTreeNode {
        std::string parentKey;
        uint64_t generation{0};
        uint64_t lastAccessSequence{0};
        size_t childCountHint{0};
        bool ready{false};
    };

    struct HeapEntry {
        std::string key;
        uint64_t generation{0};
        uint64_t accessSequence{0};
    };

    struct HeapEntryLater {
        bool operator()(const HeapEntry &left, const HeapEntry &right) const
        {
            return left.accessSequence > right.accessSequence;
        }
    };

    struct Shard {
        mutable std::mutex nodeMutex;
        std::unordered_map<std::string, PrefixTreeNode> nodes;
        // Child counts whose parent node is not currently present. This lets a later parent registration recover its
        // known child hint without retaining parent-to-child vectors.
        std::unordered_map<std::string, size_t> detachedChildCounts;
        std::mutex heapMutex;
        std::priority_queue<HeapEntry, std::vector<HeapEntry>, HeapEntryLater> heap;
    };

    class ScopedShardPairLock {
    public:
        ScopedShardPairLock(Shard &first, size_t firstIndex, Shard &second, size_t secondIndex);

    private:
        std::unique_lock<std::mutex> firstLock_;
        std::unique_lock<std::mutex> secondLock_;
    };

    size_t GetShardIndex(const std::string &key) const;
    uint64_t NextGeneration();
    uint64_t NextAccessSequence();
    void PushHeapEntry(const std::string &key, uint64_t generation, uint64_t accessSequence);

    std::array<Shard, kShardCount> shards_;
    std::atomic<uint64_t> nextGeneration_{0};
    std::atomic<uint64_t> nextAccessSequence_{0};
    std::atomic<size_t> nextPopShard_{0};
    std::atomic<size_t> nodeCount_{0};
    std::atomic<uint64_t> parentConflictCount_{0};
    std::atomic<uint64_t> staleHeapPopCount_{0};
    std::atomic<uint64_t> leafCandidateCount_{0};
};

} // namespace mmc
} // namespace ock

#endif // MEM_FABRIC_MMC_PREFIX_TREE_INDEX_H
