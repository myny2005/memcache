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

#ifndef MEM_FABRIC_MMC_PARENT_PATH_H
#define MEM_FABRIC_MMC_PARENT_PATH_H

#include <cstddef>
#include <string>
#include <vector>

#include "mmc_types.h"

namespace ock {
namespace mmc {

// A parent-aware write encodes component paths as consecutive equal-length key ranges.
// For keys=[B_k,C_k,B_v,C_v] and parents=[A_k,A_v], keysPerPath is two.
struct ParentPathLayout {
    size_t pathCount{0};
    size_t keysPerPath{0};
};

// Empty parent strings represent roots. A non-empty parent must be outside the submitted key set, which prevents
// cycles and cross-component links within one request.
Result ValidateParentPaths(const std::vector<std::string> &keys, const std::vector<std::string> &parents,
                           ParentPathLayout &layout);

struct ParentPathRetry {
    std::vector<size_t> sourceIndices;
    std::vector<std::string> keys;
    std::vector<std::string> parents;
};

// Build valid suffix runs for failed keys. Adjacent failures in one original path remain one retry path; disjoint
// failures become independent requests. This preserves parent mapping without rewriting successful keys.
Result BuildParentPathRetries(const std::vector<std::string> &keys, const std::vector<std::string> &parents,
                              const ParentPathLayout &layout, const std::vector<size_t> &retryIndices,
                              std::vector<ParentPathRetry> &retries);

} // namespace mmc
} // namespace ock

#endif // MEM_FABRIC_MMC_PARENT_PATH_H
