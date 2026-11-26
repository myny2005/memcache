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

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "mmc_prefix_tree_index.h"

using namespace ock::mmc;

namespace {

constexpr size_t kConcurrentThreadCount = 8U;
constexpr size_t kKeysPerConcurrentThread = 64U;
constexpr size_t kRemainingKeysPerConcurrentThread = kKeysPerConcurrentThread / 2U;
constexpr size_t kNodeCountPerConcurrentThread = kRemainingKeysPerConcurrentThread + 1U;
constexpr size_t kExpectedConcurrentNodeCount = kConcurrentThreadCount * kNodeCountPerConcurrentThread;
constexpr size_t kExpectedOrphanCount = 1U;

PrefixTreeRegistration RegisterNode(PrefixTreeIndex &index, const std::string &key, const std::string &parentKey)
{
    PrefixTreeRegistration registration;
    EXPECT_EQ(index.RegisterPending(key, parentKey, registration), MMC_OK);
    return registration;
}

TEST(MmcPrefixTreeIndexTest, TracksIndependentKeyAndValuePaths)
{
    PrefixTreeIndex index;
    const auto keyParent = RegisterNode(index, "B_k", "A_k");
    const auto keyLeaf = RegisterNode(index, "C_k", "B_k");
    const auto valueParent = RegisterNode(index, "B_v", "A_v");
    const auto valueLeaf = RegisterNode(index, "C_v", "B_v");
    ASSERT_EQ(keyParent.status, PrefixTreeRegistrationStatus::INSERTED);
    ASSERT_EQ(keyLeaf.status, PrefixTreeRegistrationStatus::INSERTED);
    ASSERT_EQ(valueParent.status, PrefixTreeRegistrationStatus::INSERTED);
    ASSERT_EQ(valueLeaf.status, PrefixTreeRegistrationStatus::INSERTED);

    PrefixTreeNodeSnapshot keyParentSnapshot;
    PrefixTreeNodeSnapshot valueParentSnapshot;
    ASSERT_EQ(index.GetSnapshot("B_k", keyParentSnapshot), MMC_OK);
    ASSERT_EQ(index.GetSnapshot("B_v", valueParentSnapshot), MMC_OK);
    EXPECT_EQ(keyParentSnapshot.childCountHint, 1U);
    EXPECT_EQ(valueParentSnapshot.childCountHint, 1U);

    uint64_t generation = 0;
    ASSERT_EQ(index.MarkReady("B_k", generation), MMC_OK);
    ASSERT_EQ(index.MarkReady("C_k", generation), MMC_OK);
    ASSERT_EQ(index.MarkReady("B_v", generation), MMC_OK);
    ASSERT_EQ(index.MarkReady("C_v", generation), MMC_OK);

    PrefixTreeCandidate firstCandidate;
    PrefixTreeCandidate secondCandidate;
    ASSERT_TRUE(index.PopLeafCandidate(firstCandidate));
    ASSERT_TRUE(index.PopLeafCandidate(secondCandidate));
    EXPECT_NE(firstCandidate.key, secondCandidate.key);
    EXPECT_TRUE(firstCandidate.key == "C_k" || firstCandidate.key == "C_v");
    EXPECT_TRUE(secondCandidate.key == "C_k" || secondCandidate.key == "C_v");
}

TEST(MmcPrefixTreeIndexTest, PreservesDetachedChildHintUntilParentRegisters)
{
    PrefixTreeIndex index;
    const auto root = RegisterNode(index, "A_k", "");
    const auto orphan = RegisterNode(index, "C_k", "B_k");
    ASSERT_EQ(root.status, PrefixTreeRegistrationStatus::INSERTED);
    ASSERT_EQ(orphan.status, PrefixTreeRegistrationStatus::INSERTED);

    PrefixTreeNodeSnapshot orphanSnapshot;
    ASSERT_EQ(index.GetSnapshot("C_k", orphanSnapshot), MMC_OK);
    EXPECT_EQ(orphanSnapshot.parentKey, "B_k");
    EXPECT_EQ(orphanSnapshot.childCountHint, 0U);
    EXPECT_EQ(index.GetStats().orphanCount, kExpectedOrphanCount);

    const auto parent = RegisterNode(index, "B_k", "A_k");
    ASSERT_EQ(parent.status, PrefixTreeRegistrationStatus::INSERTED);
    PrefixTreeNodeSnapshot parentSnapshot;
    ASSERT_EQ(index.GetSnapshot("B_k", parentSnapshot), MMC_OK);
    EXPECT_EQ(parentSnapshot.childCountHint, 1U);
    EXPECT_EQ(index.GetStats().orphanCount, 0U);
}

TEST(MmcPrefixTreeIndexTest, KeepsFirstParentForIdempotentAndConflictingRegistration)
{
    PrefixTreeIndex index;
    const auto inserted = RegisterNode(index, "B_k", "A_k");
    const auto existing = RegisterNode(index, "B_k", "A_k");
    const auto conflict = RegisterNode(index, "B_k", "other_A_k");

    EXPECT_EQ(inserted.status, PrefixTreeRegistrationStatus::INSERTED);
    EXPECT_EQ(existing.status, PrefixTreeRegistrationStatus::EXISTING);
    EXPECT_EQ(conflict.status, PrefixTreeRegistrationStatus::PARENT_CONFLICT);
    EXPECT_EQ(existing.generation, inserted.generation);
    EXPECT_EQ(conflict.generation, inserted.generation);

    PrefixTreeNodeSnapshot snapshot;
    ASSERT_EQ(index.GetSnapshot("B_k", snapshot), MMC_OK);
    EXPECT_EQ(snapshot.parentKey, "A_k");
    EXPECT_EQ(index.GetStats().parentConflictCount, 1U);
}

TEST(MmcPrefixTreeIndexTest, IgnoresStaleGenerationAfterKeyRecreation)
{
    PrefixTreeIndex index;
    const auto first = RegisterNode(index, "B_k", "A_k");
    uint64_t generation = 0;
    ASSERT_EQ(index.MarkReady("B_k", generation), MMC_OK);
    ASSERT_EQ(index.RemoveIfGeneration("B_k", first.generation), MMC_OK);

    const auto second = RegisterNode(index, "B_k", "A_k");
    ASSERT_EQ(index.MarkReady("B_k", generation), MMC_OK);
    EXPECT_NE(first.generation, second.generation);
    EXPECT_EQ(index.RemoveIfGeneration("B_k", first.generation), MMC_UNMATCHED_STATE);

    PrefixTreeCandidate candidate;
    ASSERT_TRUE(index.PopLeafCandidate(candidate));
    EXPECT_EQ(candidate.key, "B_k");
    EXPECT_EQ(candidate.generation, second.generation);
    EXPECT_GE(index.GetStats().staleHeapPopCount, 1U);
}

TEST(MmcPrefixTreeIndexTest, DiscardsStaleAccessEntryAfterTouch)
{
    PrefixTreeIndex index;
    const auto registration = RegisterNode(index, "B_k", "A_k");
    uint64_t generation = 0;
    ASSERT_EQ(index.MarkReady("B_k", generation), MMC_OK);
    PrefixTreeNodeSnapshot beforeTouch;
    ASSERT_EQ(index.GetSnapshot("B_k", beforeTouch), MMC_OK);
    ASSERT_EQ(index.Touch("B_k"), MMC_OK);

    PrefixTreeCandidate candidate;
    ASSERT_TRUE(index.PopLeafCandidate(candidate));
    EXPECT_EQ(candidate.generation, registration.generation);
    EXPECT_GT(candidate.accessSequence, beforeTouch.lastAccessSequence);
    EXPECT_GE(index.GetStats().staleHeapPopCount, 1U);
}

TEST(MmcPrefixTreeIndexTest, UpdatesChildHintWhenChildIsRemoved)
{
    PrefixTreeIndex index;
    const auto parent = RegisterNode(index, "A_k", "");
    const auto child = RegisterNode(index, "B_k", "A_k");
    ASSERT_EQ(parent.status, PrefixTreeRegistrationStatus::INSERTED);
    ASSERT_EQ(child.status, PrefixTreeRegistrationStatus::INSERTED);

    PrefixTreeNodeSnapshot snapshot;
    ASSERT_EQ(index.GetSnapshot("A_k", snapshot), MMC_OK);
    ASSERT_EQ(snapshot.childCountHint, 1U);
    ASSERT_EQ(index.RemoveIfGeneration("B_k", child.generation), MMC_OK);
    ASSERT_EQ(index.GetSnapshot("A_k", snapshot), MMC_OK);
    EXPECT_EQ(snapshot.childCountHint, 0U);
}

TEST(MmcPrefixTreeIndexTest, HandlesConcurrentRegisterTouchAndRemove)
{
    PrefixTreeIndex index;
    std::atomic<size_t> successCount{0};
    std::vector<std::thread> threads;
    threads.reserve(kConcurrentThreadCount);
    for (size_t threadIndex = 0; threadIndex < kConcurrentThreadCount; ++threadIndex) {
        threads.emplace_back([&index, &successCount, threadIndex]() {
            const std::string parentKey = "parent_" + std::to_string(threadIndex);
            PrefixTreeRegistration parent;
            if (index.RegisterPending(parentKey, "", parent) != MMC_OK) {
                return;
            }
            for (size_t keyIndex = 0; keyIndex < kKeysPerConcurrentThread; ++keyIndex) {
                const std::string key = parentKey + "_child_" + std::to_string(keyIndex);
                PrefixTreeRegistration registration;
                if (index.RegisterPending(key, parentKey, registration) != MMC_OK) {
                    return;
                }
                uint64_t generation = 0;
                if (index.MarkReady(key, generation) != MMC_OK || index.Touch(key) != MMC_OK) {
                    return;
                }
                if (keyIndex % 2U == 0U && index.RemoveIfGeneration(key, registration.generation) != MMC_OK) {
                    return;
                }
            }
            successCount.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }

    EXPECT_EQ(successCount.load(std::memory_order_relaxed), kConcurrentThreadCount);
    EXPECT_EQ(index.GetStats().nodeCount, kExpectedConcurrentNodeCount);
    for (size_t threadIndex = 0; threadIndex < kConcurrentThreadCount; ++threadIndex) {
        PrefixTreeNodeSnapshot parentSnapshot;
        const std::string parentKey = "parent_" + std::to_string(threadIndex);
        ASSERT_EQ(index.GetSnapshot(parentKey, parentSnapshot), MMC_OK);
        EXPECT_EQ(parentSnapshot.childCountHint, kRemainingKeysPerConcurrentThread);
    }
}

} // namespace
