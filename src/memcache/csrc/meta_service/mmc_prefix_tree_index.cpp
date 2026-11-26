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

#include "mmc_prefix_tree_index.h"

#include <functional>

namespace ock {
namespace mmc {

PrefixTreeIndex::ScopedShardPairLock::ScopedShardPairLock(Shard &first, size_t firstIndex, Shard &second,
                                                           size_t secondIndex)
{
    if (firstIndex == secondIndex) {
        firstLock_ = std::unique_lock<std::mutex>(first.nodeMutex);
        return;
    }
    if (firstIndex < secondIndex) {
        firstLock_ = std::unique_lock<std::mutex>(first.nodeMutex);
        secondLock_ = std::unique_lock<std::mutex>(second.nodeMutex);
        return;
    }
    firstLock_ = std::unique_lock<std::mutex>(second.nodeMutex);
    secondLock_ = std::unique_lock<std::mutex>(first.nodeMutex);
}

Result PrefixTreeIndex::RegisterPending(const std::string &key, const std::string &parentKey,
                                        PrefixTreeRegistration &registration)
{
    registration = {};
    if (key.empty() || key == parentKey) {
        return MMC_INVALID_PARAM;
    }

    const size_t keyShardIndex = GetShardIndex(key);
    const size_t parentShardIndex = parentKey.empty() ? keyShardIndex : GetShardIndex(parentKey);
    Shard &keyShard = shards_[keyShardIndex];
    Shard &parentShard = shards_[parentShardIndex];
    ScopedShardPairLock lock(keyShard, keyShardIndex, parentShard, parentShardIndex);

    const auto existing = keyShard.nodes.find(key);
    if (existing != keyShard.nodes.end()) {
        registration.generation = existing->second.generation;
        if (existing->second.parentKey == parentKey) {
            registration.status = PrefixTreeRegistrationStatus::EXISTING;
        } else {
            registration.status = PrefixTreeRegistrationStatus::PARENT_CONFLICT;
            parentConflictCount_.fetch_add(1, std::memory_order_relaxed);
        }
        return MMC_OK;
    }

    PrefixTreeNode node;
    node.parentKey = parentKey;
    node.generation = NextGeneration();
    node.lastAccessSequence = NextAccessSequence();
    const auto detachedChildren = keyShard.detachedChildCounts.find(key);
    if (detachedChildren != keyShard.detachedChildCounts.end()) {
        node.childCountHint = detachedChildren->second;
        keyShard.detachedChildCounts.erase(detachedChildren);
    }
    keyShard.nodes.emplace(key, std::move(node));
    nodeCount_.fetch_add(1, std::memory_order_relaxed);

    if (!parentKey.empty()) {
        const auto parent = parentShard.nodes.find(parentKey);
        if (parent != parentShard.nodes.end()) {
            ++parent->second.childCountHint;
        } else {
            ++parentShard.detachedChildCounts[parentKey];
        }
    }

    registration.generation = keyShard.nodes.at(key).generation;
    registration.status = PrefixTreeRegistrationStatus::INSERTED;
    return MMC_OK;
}

Result PrefixTreeIndex::MarkReady(const std::string &key, uint64_t &generation)
{
    const size_t shardIndex = GetShardIndex(key);
    Shard &shard = shards_[shardIndex];
    uint64_t accessSequence = 0;
    {
        std::lock_guard<std::mutex> lock(shard.nodeMutex);
        const auto node = shard.nodes.find(key);
        if (node == shard.nodes.end()) {
            return MMC_OBJECT_NOT_EXISTS;
        }
        node->second.ready = true;
        node->second.lastAccessSequence = NextAccessSequence();
        generation = node->second.generation;
        accessSequence = node->second.lastAccessSequence;
    }
    PushHeapEntry(key, generation, accessSequence);
    return MMC_OK;
}

Result PrefixTreeIndex::Touch(const std::string &key)
{
    const size_t shardIndex = GetShardIndex(key);
    Shard &shard = shards_[shardIndex];
    uint64_t generation = 0;
    uint64_t accessSequence = 0;
    {
        std::lock_guard<std::mutex> lock(shard.nodeMutex);
        const auto node = shard.nodes.find(key);
        if (node == shard.nodes.end()) {
            return MMC_OBJECT_NOT_EXISTS;
        }
        if (!node->second.ready) {
            return MMC_UNMATCHED_STATE;
        }
        node->second.lastAccessSequence = NextAccessSequence();
        generation = node->second.generation;
        accessSequence = node->second.lastAccessSequence;
    }
    PushHeapEntry(key, generation, accessSequence);
    return MMC_OK;
}

Result PrefixTreeIndex::GetSnapshot(const std::string &key, PrefixTreeNodeSnapshot &snapshot) const
{
    const Shard &shard = shards_[GetShardIndex(key)];
    std::lock_guard<std::mutex> lock(shard.nodeMutex);
    const auto node = shard.nodes.find(key);
    if (node == shard.nodes.end()) {
        return MMC_OBJECT_NOT_EXISTS;
    }
    snapshot.parentKey = node->second.parentKey;
    snapshot.generation = node->second.generation;
    snapshot.lastAccessSequence = node->second.lastAccessSequence;
    snapshot.childCountHint = node->second.childCountHint;
    snapshot.ready = node->second.ready;
    return MMC_OK;
}

Result PrefixTreeIndex::RemoveIfGeneration(const std::string &key, uint64_t generation)
{
    const size_t keyShardIndex = GetShardIndex(key);
    Shard &keyShard = shards_[keyShardIndex];
    std::string parentKey;
    {
        std::lock_guard<std::mutex> lock(keyShard.nodeMutex);
        const auto node = keyShard.nodes.find(key);
        if (node == keyShard.nodes.end()) {
            return MMC_OBJECT_NOT_EXISTS;
        }
        if (node->second.generation != generation) {
            return MMC_UNMATCHED_STATE;
        }
        parentKey = node->second.parentKey;
    }

    const size_t parentShardIndex = parentKey.empty() ? keyShardIndex : GetShardIndex(parentKey);
    Shard &parentShard = shards_[parentShardIndex];
    ScopedShardPairLock lock(keyShard, keyShardIndex, parentShard, parentShardIndex);
    const auto node = keyShard.nodes.find(key);
    if (node == keyShard.nodes.end()) {
        return MMC_OBJECT_NOT_EXISTS;
    }
    if (node->second.generation != generation || node->second.parentKey != parentKey) {
        return MMC_UNMATCHED_STATE;
    }

    if (node->second.childCountHint != 0U) {
        parentShard.detachedChildCounts[key] += node->second.childCountHint;
    }
    if (!parentKey.empty()) {
        const auto parent = parentShard.nodes.find(parentKey);
        if (parent != parentShard.nodes.end()) {
            if (parent->second.childCountHint != 0U) {
                --parent->second.childCountHint;
            }
        } else {
            const auto detachedChildren = parentShard.detachedChildCounts.find(parentKey);
            if (detachedChildren != parentShard.detachedChildCounts.end()) {
                if (detachedChildren->second > 1U) {
                    --detachedChildren->second;
                } else {
                    parentShard.detachedChildCounts.erase(detachedChildren);
                }
            }
        }
    }
    keyShard.nodes.erase(node);
    nodeCount_.fetch_sub(1, std::memory_order_relaxed);
    return MMC_OK;
}

bool PrefixTreeIndex::PopLeafCandidate(PrefixTreeCandidate &candidate)
{
    candidate = {};
    const size_t firstShard = nextPopShard_.fetch_add(1, std::memory_order_relaxed) % kShardCount;
    for (size_t offset = 0; offset < kShardCount; ++offset) {
        const size_t shardIndex = (firstShard + offset) % kShardCount;
        Shard &shard = shards_[shardIndex];
        while (true) {
            HeapEntry entry;
            {
                std::lock_guard<std::mutex> lock(shard.heapMutex);
                if (shard.heap.empty()) {
                    break;
                }
                entry = shard.heap.top();
                shard.heap.pop();
            }

            std::lock_guard<std::mutex> lock(shard.nodeMutex);
            const auto node = shard.nodes.find(entry.key);
            if (node == shard.nodes.end() || node->second.generation != entry.generation ||
                node->second.lastAccessSequence != entry.accessSequence || !node->second.ready ||
                node->second.childCountHint != 0U) {
                staleHeapPopCount_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            candidate.key = entry.key;
            candidate.generation = entry.generation;
            candidate.accessSequence = entry.accessSequence;
            leafCandidateCount_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

PrefixTreeIndexStats PrefixTreeIndex::GetStats() const
{
    PrefixTreeIndexStats stats;
    stats.nodeCount = nodeCount_.load(std::memory_order_relaxed);
    std::vector<std::string> parentKeys;
    parentKeys.reserve(stats.nodeCount);
    for (const auto &shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.nodeMutex);
        for (const auto &[key, node] : shard.nodes) {
            (void)key;
            if (!node.parentKey.empty()) {
                parentKeys.push_back(node.parentKey);
            }
        }
    }
    for (const auto &parentKey : parentKeys) {
        PrefixTreeNodeSnapshot parentSnapshot;
        if (GetSnapshot(parentKey, parentSnapshot) != MMC_OK) {
            ++stats.orphanCount;
        }
    }
    stats.parentConflictCount = parentConflictCount_.load(std::memory_order_relaxed);
    stats.staleHeapPopCount = staleHeapPopCount_.load(std::memory_order_relaxed);
    stats.leafCandidateCount = leafCandidateCount_.load(std::memory_order_relaxed);
    return stats;
}

size_t PrefixTreeIndex::GetShardIndex(const std::string &key) const
{
    return std::hash<std::string>{}(key) % kShardCount;
}

uint64_t PrefixTreeIndex::NextGeneration()
{
    return nextGeneration_.fetch_add(1, std::memory_order_relaxed) + 1U;
}

uint64_t PrefixTreeIndex::NextAccessSequence()
{
    return nextAccessSequence_.fetch_add(1, std::memory_order_relaxed) + 1U;
}

void PrefixTreeIndex::PushHeapEntry(const std::string &key, uint64_t generation, uint64_t accessSequence)
{
    Shard &shard = shards_[GetShardIndex(key)];
    std::lock_guard<std::mutex> lock(shard.heapMutex);
    shard.heap.push({key, generation, accessSequence});
}

} // namespace mmc
} // namespace ock
