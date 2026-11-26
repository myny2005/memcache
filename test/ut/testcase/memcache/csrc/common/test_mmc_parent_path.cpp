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

#include "gtest/gtest.h"
#include "mmc_parent_path.h"

using namespace ock::mmc;

namespace {

constexpr size_t kComponentCount = 2U;
constexpr size_t kExpectedRetryPathCount = 2U;
constexpr size_t kKeysPerComponent = 2U;
constexpr size_t kFirstNestedKeyIndex = 1U;
constexpr size_t kSecondComponentFirstKeyIndex = 2U;
constexpr size_t kInvalidRetryKeyIndex = 4U;
constexpr size_t kThirdKeyIndex = 2U;

TEST(MmcParentPathTest, ValidatesComponentMajorPhysicalPaths)
{
    const std::vector<std::string> keys{"B_k", "C_k", "B_v", "C_v"};
    const std::vector<std::string> parents{"A_k", "A_v"};
    ParentPathLayout layout;

    ASSERT_EQ(ValidateParentPaths(keys, parents, layout), MMC_OK);
    EXPECT_EQ(layout.pathCount, kComponentCount);
    EXPECT_EQ(layout.keysPerPath, kKeysPerComponent);
}

TEST(MmcParentPathTest, AcceptsEmptyParentForRootPath)
{
    const std::vector<std::string> keys{"A_k"};
    const std::vector<std::string> parents{""};
    ParentPathLayout layout;

    EXPECT_EQ(ValidateParentPaths(keys, parents, layout), MMC_OK);
}

TEST(MmcParentPathTest, RejectsMalformedPaths)
{
    ParentPathLayout layout;

    EXPECT_EQ(ValidateParentPaths({}, {"A_k"}, layout), MMC_INVALID_PARAM);
    EXPECT_EQ(ValidateParentPaths({"A_k"}, {}, layout), MMC_INVALID_PARAM);
    EXPECT_EQ(ValidateParentPaths({"A_k", "B_k", "A_v"}, {"root_k", "root_v"}, layout), MMC_INVALID_PARAM);
    EXPECT_EQ(ValidateParentPaths({"A_k", "A_k"}, {"root_k"}, layout), MMC_INVALID_PARAM);
    EXPECT_EQ(ValidateParentPaths({"A_k", ""}, {"root_k"}, layout), MMC_INVALID_PARAM);
    EXPECT_EQ(ValidateParentPaths({"A_k", "B_k"}, {"B_k"}, layout), MMC_INVALID_PARAM);
}

TEST(MmcParentPathTest, BuildsRetryPathsWithImmediateOriginalParents)
{
    const std::vector<std::string> keys{"B_k", "C_k", "B_v", "C_v"};
    const std::vector<std::string> parents{"A_k", "A_v"};
    ParentPathLayout layout;
    ASSERT_EQ(ValidateParentPaths(keys, parents, layout), MMC_OK);

    std::vector<ParentPathRetry> retries;
    ASSERT_EQ(BuildParentPathRetries(keys, parents, layout, {kFirstNestedKeyIndex, kSecondComponentFirstKeyIndex},
                                     retries),
              MMC_OK);

    ASSERT_EQ(retries.size(), kExpectedRetryPathCount);
    EXPECT_EQ(retries[0].keys, (std::vector<std::string>{"C_k"}));
    EXPECT_EQ(retries[0].parents, (std::vector<std::string>{"B_k"}));
    EXPECT_EQ(retries[1].keys, (std::vector<std::string>{"B_v"}));
    EXPECT_EQ(retries[1].parents, (std::vector<std::string>{"A_v"}));
}

TEST(MmcParentPathTest, KeepsAdjacentFailuresInOnePath)
{
    const std::vector<std::string> keys{"B_k", "C_k", "B_v", "C_v"};
    const std::vector<std::string> parents{"A_k", "A_v"};
    ParentPathLayout layout;
    ASSERT_EQ(ValidateParentPaths(keys, parents, layout), MMC_OK);

    std::vector<ParentPathRetry> retries;
    ASSERT_EQ(BuildParentPathRetries(keys, parents, layout, {0U, kFirstNestedKeyIndex}, retries), MMC_OK);
    ASSERT_EQ(retries.size(), 1U);
    EXPECT_EQ(retries[0].keys, (std::vector<std::string>{"B_k", "C_k"}));
    EXPECT_EQ(retries[0].parents, (std::vector<std::string>{"A_k"}));
}

TEST(MmcParentPathTest, SplitsDisjointFailuresIntoValidSuffixPaths)
{
    const std::vector<std::string> keys{"B_k", "C_k", "D_k"};
    const std::vector<std::string> parents{"A_k"};
    ParentPathLayout layout;
    ASSERT_EQ(ValidateParentPaths(keys, parents, layout), MMC_OK);

    std::vector<ParentPathRetry> retries;
    ASSERT_EQ(BuildParentPathRetries(keys, parents, layout, {0U, kThirdKeyIndex}, retries), MMC_OK);
    ASSERT_EQ(retries.size(), kExpectedRetryPathCount);
    EXPECT_EQ(retries[0].keys, (std::vector<std::string>{"B_k"}));
    EXPECT_EQ(retries[0].parents, (std::vector<std::string>{"A_k"}));
    EXPECT_EQ(retries[1].keys, (std::vector<std::string>{"D_k"}));
    EXPECT_EQ(retries[1].parents, (std::vector<std::string>{"C_k"}));
}

TEST(MmcParentPathTest, RejectsRetryIndexOutsideOriginalPath)
{
    const std::vector<std::string> keys{"B_k", "C_k", "B_v", "C_v"};
    const std::vector<std::string> parents{"A_k", "A_v"};
    ParentPathLayout layout;
    ASSERT_EQ(ValidateParentPaths(keys, parents, layout), MMC_OK);

    std::vector<ParentPathRetry> retries(1U);
    EXPECT_EQ(BuildParentPathRetries(keys, parents, layout, {kInvalidRetryKeyIndex}, retries), MMC_INVALID_PARAM);
    EXPECT_TRUE(retries.empty());
}

} // namespace
