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

#include "mmc_parent_path.h"

#include <unordered_set>
#include <utility>

namespace ock {
namespace mmc {

Result ValidateParentPaths(const std::vector<std::string> &keys, const std::vector<std::string> &parents,
                           ParentPathLayout &layout)
{
    layout = {};
    if (keys.empty() || parents.empty() || keys.size() % parents.size() != 0) {
        return MMC_INVALID_PARAM;
    }

    std::unordered_set<std::string> keySet;
    keySet.reserve(keys.size());
    for (const auto &key : keys) {
        if (key.empty() || !keySet.emplace(key).second) {
            return MMC_INVALID_PARAM;
        }
    }

    for (const auto &parent : parents) {
        if (!parent.empty() && keySet.count(parent) != 0) {
            return MMC_INVALID_PARAM;
        }
    }

    layout.pathCount = parents.size();
    layout.keysPerPath = keys.size() / parents.size();
    return MMC_OK;
}

Result BuildParentPathRetries(const std::vector<std::string> &keys, const std::vector<std::string> &parents,
                              const ParentPathLayout &layout, const std::vector<size_t> &retryIndices,
                              std::vector<ParentPathRetry> &retries)
{
    retries.clear();
    ParentPathLayout validatedLayout;
    const Result validateRet = ValidateParentPaths(keys, parents, validatedLayout);
    if (validateRet != MMC_OK || layout.pathCount != validatedLayout.pathCount ||
        layout.keysPerPath != validatedLayout.keysPerPath) {
        return MMC_INVALID_PARAM;
    }

    std::vector<bool> retryKeyMask(keys.size(), false);
    for (const size_t index : retryIndices) {
        if (index >= keys.size()) {
            return MMC_INVALID_PARAM;
        }
        retryKeyMask[index] = true;
    }

    for (size_t pathIndex = 0; pathIndex < layout.pathCount; ++pathIndex) {
        const size_t pathBegin = pathIndex * layout.keysPerPath;
        const size_t pathEnd = pathBegin + layout.keysPerPath;
        size_t keyIndex = pathBegin;
        while (keyIndex < pathEnd) {
            while (keyIndex < pathEnd && !retryKeyMask[keyIndex]) {
                ++keyIndex;
            }
            if (keyIndex == pathEnd) {
                break;
            }

            const size_t suffixBegin = keyIndex;
            while (keyIndex < pathEnd && retryKeyMask[keyIndex]) {
                ++keyIndex;
            }

            ParentPathRetry retry;
            retry.parents.push_back(suffixBegin == pathBegin ? parents[pathIndex] : keys[suffixBegin - 1]);
            retry.keys.reserve(keyIndex - suffixBegin);
            retry.sourceIndices.reserve(keyIndex - suffixBegin);
            for (size_t sourceIndex = suffixBegin; sourceIndex < keyIndex; ++sourceIndex) {
                retry.keys.push_back(keys[sourceIndex]);
                retry.sourceIndices.push_back(sourceIndex);
            }
            retries.emplace_back(std::move(retry));
        }
    }
    return MMC_OK;
}

} // namespace mmc
} // namespace ock
