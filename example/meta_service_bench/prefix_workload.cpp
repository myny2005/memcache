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

#include "prefix_workload.h"

#include <algorithm>
#include <utility>

namespace ock::mmc::bench {
namespace {
constexpr uint32_t kKeyComponentCount = 2;
constexpr uint32_t kKeyComponentK = 0;
constexpr uint32_t kKeyComponentV = 1;
constexpr uint32_t kPercentBase = 100;
constexpr uint64_t kMixConstantOne = 0x9e3779b97f4a7c15ULL;
constexpr uint64_t kMixConstantTwo = 0xbf58476d1ce4e5b9ULL;
constexpr uint64_t kMixConstantThree = 0x94d049bb133111ebULL;

uint64_t Mix(uint64_t value)
{
    value += kMixConstantOne;
    value = (value ^ (value >> 30U)) * kMixConstantTwo;
    value = (value ^ (value >> 27U)) * kMixConstantThree;
    return value ^ (value >> 31U);
}
} // namespace

PrefixWorkload::PrefixWorkload(PrefixWorkloadConfig config) : config_(std::move(config)) {}

bool PrefixWorkload::IsValid(std::string &error) const
{
    if (config_.rootCount == 0 || config_.maxDepth == 0 || config_.fanout == 0 || config_.componentCount == 0) {
        error = "rootCount, maxDepth, fanout and componentCount must be greater than zero";
        return false;
    }
    if (config_.sharedPrefixPercent > kPercentBase) {
        error = "sharedPrefixPercent must be in [0, 100]";
        return false;
    }
    if (config_.requestPages == 0 || config_.requestPages > config_.maxDepth) {
        error = "requestPages must be in [1, maxDepth]";
        return false;
    }
    if (config_.sharedPrefixPages > config_.requestPages) {
        error = "sharedPrefixPages must not exceed requestPages";
        return false;
    }
    return true;
}

uint64_t PrefixWorkload::Hash(uint64_t requestId, uint32_t depth) const
{
    return Mix(config_.seed ^ Mix(requestId) ^ (static_cast<uint64_t>(depth) * kMixConstantOne));
}

PrefixRequest PrefixWorkload::MakeRequest(uint64_t requestId) const
{
    PrefixRequest request;
    request.requestId = requestId;
    request.pages.reserve(config_.requestPages);

    const bool useSharedPrefix = (Hash(requestId, 0) % kPercentBase) < config_.sharedPrefixPercent;
    const uint32_t rootIndex = useSharedPrefix ? 0 : static_cast<uint32_t>(Hash(requestId, 1) % config_.rootCount);
    std::string parentKey;
    std::string path = "prefix_root_" + std::to_string(rootIndex);
    for (uint32_t depth = 0; depth < config_.requestPages; ++depth) {
        const bool isSharedPage = useSharedPrefix && depth < config_.sharedPrefixPages;
        const uint32_t branch = isSharedPage ? 0 : static_cast<uint32_t>(Hash(requestId, depth + 2) % config_.fanout);
        path += "/d" + std::to_string(depth) + "_b" + std::to_string(branch);
        request.pages.push_back({path, parentKey});
        parentKey = path;
    }
    return request;
}

std::string PrefixWorkload::MakeComponentKey(const std::string &pageKey, uint32_t componentIndex) const
{
    if (componentIndex == kKeyComponentK) {
        return pageKey + "_k";
    }
    if (componentIndex == kKeyComponentV) {
        return pageKey + "_v";
    }
    return pageKey + "_component_" + std::to_string(componentIndex);
}

std::vector<std::string> PrefixWorkload::MakePageMajorPhysicalKeys(const PrefixRequest &request) const
{
    std::vector<std::string> keys;
    keys.reserve(request.pages.size() * config_.componentCount);
    for (const auto &page : request.pages) {
        for (uint32_t component = 0; component < config_.componentCount; ++component) {
            keys.push_back(MakeComponentKey(page.key, component));
        }
    }
    return keys;
}

PhysicalWritePlan PrefixWorkload::MakeWritePlan(const PrefixRequest &request, size_t cachedPageCount) const
{
    PhysicalWritePlan plan;
    if (cachedPageCount >= request.pages.size()) {
        return plan;
    }

    const size_t missingPageCount = request.pages.size() - cachedPageCount;
    plan.keys.reserve(missingPageCount * config_.componentCount);
    plan.parents.reserve(config_.componentCount);
    for (uint32_t component = 0; component < config_.componentCount; ++component) {
        const std::string parentKey = cachedPageCount == 0
                                          ? ""
                                          : MakeComponentKey(request.pages[cachedPageCount - 1].key, component);
        plan.parents.push_back(parentKey);
        for (size_t pageIndex = cachedPageCount; pageIndex < request.pages.size(); ++pageIndex) {
            plan.keys.push_back(MakeComponentKey(request.pages[pageIndex].key, component));
        }
    }
    return plan;
}

bool ValidatePrefixWorkload(std::string &error)
{
    PrefixWorkloadConfig config;
    config.rootCount = 1;
    config.maxDepth = 4;
    config.fanout = 16;
    config.sharedPrefixPercent = kPercentBase;
    config.sharedPrefixPages = 3;
    config.requestPages = 4;
    config.componentCount = kKeyComponentCount;
    PrefixWorkload workload(config);
    if (!workload.IsValid(error)) {
        return false;
    }

    const PrefixRequest first = workload.MakeRequest(1);
    const PrefixRequest second = workload.MakeRequest(2);
    if (first.pages.size() != config.requestPages || second.pages.size() != config.requestPages) {
        error = "unexpected generated page count";
        return false;
    }
    for (size_t index = 0; index < config.sharedPrefixPages; ++index) {
        if (first.pages[index].key != second.pages[index].key) {
            error = "shared request pages do not share the configured prefix";
            return false;
        }
        const std::string expectedParent = index == 0 ? "" : first.pages[index - 1].key;
        if (first.pages[index].parentKey != expectedParent) {
            error = "generated page has an invalid parent";
            return false;
        }
    }

    const std::vector<std::string> pageMajorKeys = workload.MakePageMajorPhysicalKeys(first);
    if (pageMajorKeys.size() != config.requestPages * config.componentCount ||
        pageMajorKeys[0].find("_k") == std::string::npos || pageMajorKeys[1].find("_v") == std::string::npos) {
        error = "invalid physical K/V key generation";
        return false;
    }

    const PhysicalWritePlan plan = workload.MakeWritePlan(first, config.sharedPrefixPages);
    if (plan.parents.size() != config.componentCount || plan.keys.size() != config.componentCount ||
        plan.parents[0] != first.pages[config.sharedPrefixPages - 1].key + "_k" ||
        plan.parents[1] != first.pages[config.sharedPrefixPages - 1].key + "_v") {
        error = "invalid component-major parent write plan";
        return false;
    }
    return true;
}

} // namespace ock::mmc::bench
