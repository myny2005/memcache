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

#ifndef PREFIX_WORKLOAD_H
#define PREFIX_WORKLOAD_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ock::mmc::bench {

struct PrefixWorkloadConfig {
    uint32_t rootCount{4};
    uint32_t maxDepth{8};
    uint32_t fanout{8};
    uint32_t sharedPrefixPercent{80};
    uint32_t sharedPrefixPages{3};
    uint32_t requestPages{6};
    uint32_t componentCount{2};
    uint64_t seed{42};
};

struct LogicalPage {
    std::string key;
    std::string parentKey;
};

struct PrefixRequest {
    uint64_t requestId{0};
    std::vector<LogicalPage> pages;
};

struct PhysicalWritePlan {
    std::vector<std::string> keys;
    std::vector<std::string> parents;
};

class PrefixWorkload {
public:
    explicit PrefixWorkload(PrefixWorkloadConfig config);

    bool IsValid(std::string &error) const;
    PrefixRequest MakeRequest(uint64_t requestId) const;
    std::vector<std::string> MakePageMajorPhysicalKeys(const PrefixRequest &request) const;
    PhysicalWritePlan MakeWritePlan(const PrefixRequest &request, size_t cachedPageCount) const;

private:
    std::string MakeComponentKey(const std::string &pageKey, uint32_t componentIndex) const;
    uint64_t Hash(uint64_t requestId, uint32_t depth) const;

    PrefixWorkloadConfig config_;
};

bool ValidatePrefixWorkload(std::string &error);

} // namespace ock::mmc::bench

#endif // PREFIX_WORKLOAD_H
