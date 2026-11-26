# Prefix-Aware MemCache Eviction Plan

## Objective and Requirements

Implement prefix-aware cache eviction for SGLang HiCache on top of MemCache physical keys.

The desired model is based on physical parent-key paths, not logical KV-page groups. A logical cache page may have
multiple physical components, most commonly K and V:

```text
A_k -> B_k -> C_k
A_v -> B_v -> C_v
```

The system must never treat a partial physical page as a logical cache hit. A K/V page is usable only when every
required physical component is readable.

Primary requirements:

- Do not merge work into `master` or `main` while developing or reviewing.
- Preserve the parent-key API direction introduced by MemCache PR #8 and SGLang PR #1.
- Keep parent/child metadata separate from `MmcMemObjMeta` and `MmcMemBlob` metadata.
- Keep existing MetaService metadata, blob states, leases, and LRU as the correctness authority.
- Treat the prefix tree/index as a best-effort eviction policy. It may temporarily form a forest or contain stale
  candidates, but it must never make data reads unsafe.
- Existing LRU must remain the fallback for eviction.
- Avoid expensive parent-to-child vectors and global locks on normal Put/Get paths.
- Use the synthetic `example/meta_service_bench` workload before SGLang/NPU end-to-end testing.

## Current Implementation Status

### Branches

- `review/hms-memcache-pr-8` at `03354fcb`: baseline parent-aware MemCache API. It transports `parents`, but does not
  persist a tree or change eviction.
- `feature/prefix-tree-workload-simulator` at `9a0a1793`: separate benchmark branch. It adds the deterministic
  prefix-tree MetaService benchmark.
- `feature/prefix-parent-api-contract`: active branch for parent-path validation, retry correctness, prefix-index
  lifecycle, and opt-in prefix-aware eviction. It starts from `review/hms-memcache-pr-8` and intentionally does not
  contain the benchmark commit.

### SGLang

The reviewed SGLang direction derives physical parent keys from prefix keys, keeps K and V as separate physical paths,
reorders page-major keys into component-major paths, and calls the MemCache parent-aware write API.

The intended request shape is:

```text
keys    = [B_k, C_k, B_v, C_v]
parents = [A_k, A_v]
```

which means:

```text
A_k -> B_k -> C_k
A_v -> B_v -> C_v
```

### MemCache parent-aware API

`BatchAllocWithParentsRequest` already carries:

```text
keys
parents
options
flags
rankId
operateId
leaseTtlMs
```

The current MetaService path still allocates each key independently. It does not yet record parent relations, enforce
parent availability, or make eviction parent-aware.

### API contract and retry changes

The API-contract stage was committed as `8a207239` (`fix/validate parent paths and preserve them on retries`):

- New common helper:
  - `src/memcache/csrc/common/mmc_parent_path.h`
  - `src/memcache/csrc/common/mmc_parent_path.cpp`
- Parent-aware requests are validated on both client and MetaService paths.
- Parent-aware write retry no longer filters `keys` while retaining stale `parents`.
- Tests and API documentation were added/updated.

### PrefixTreeIndex and MetaService integration in the active working tree

The current uncommitted work implements the remaining in-repository feature stages:

- sharded, best-effort `PrefixTreeIndex` with lazy per-shard heaps, generations, detached child-count hints, roots,
  orphans, conflict preservation, and stale-entry discard;
- registration after successful `BatchAllocWithParents` allocations only;
- ready state after successful `WRITE_OK`, touch after a normal readable Get, and generation-aware cleanup on full
  metadata deletion;
- default-disabled `ock.mmc.meta_service.prefix_eviction.enabled` feature flag;
- normal watermark eviction first tries ready prefix leaves, validates the key and medium in the existing MetaContainer,
  then completes the target through normal LRU;
- PrefixTree and prefix-eviction metrics exposed through MetaService metrics.

The repository test script was run repeatedly for the API, index, lifecycle, and eviction stages. CMake stops before
compilation because this environment has no Python `pybind11` package/configuration directory. `git diff --check` and
new-line length checks pass. Formatting could not be checked locally because the installed clang-format is 14 while
the repository configuration requires clang-format 18 syntax.

## Relevant Files, Classes, and Functions

### Parent-aware API and client write flow

| Path | Responsibility |
|---|---|
| `src/memcache/include/mmc_client.h` | Public C API declaration of `mmcc_batch_put_with_parents`. |
| `src/memcache/include/cpp/mmcache.h` | Public C++ `ObjectStore::BatchPutFromWithParents` declaration. |
| `src/memcache/csrc/mmcache_store.cpp` | C++ store wrapper: `MmcacheStore::BatchPutFromWithParents`. |
| `src/memcache/csrc/mmc_client.cpp` | C API bridge: `mmcc_batch_put_with_parents`. |
| `src/memcache/csrc/client/mmc_client_default.h` | `MmcClientDefault` parent-aware Put declarations. |
| `src/memcache/csrc/client/mmc_client_default.cpp` | `BatchPutWithParents` and `DoBatchPutWithParents`; allocation, copy, `WRITE_OK`/`WRITE_FAIL`, retry. |
| `src/memcache/csrc/proto/mmc_msg_client_meta.h` | `BatchAllocWithParentsRequest` protocol definition. |

### MetaService allocation and lifecycle

| Path | Responsibility |
|---|---|
| `src/memcache/csrc/meta_service/mmc_meta_net_server.cpp` | RPC handler `MetaNetServer::HandleBatchAllocWithParents`. It applies allocation strategy and dispatches to proxy. |
| `src/memcache/csrc/meta_service/mmc_meta_mgr_proxy.{h,cpp}` | `MmcMetaMgrProxy::BatchAllocWithParents`; currently performs batch allocation and is the correct place to validate the parent-path shape and later register pending prefix nodes. |
| `src/memcache/csrc/meta_service/mmc_meta_manager.{h,cpp}` | Core metadata lifecycle: `Alloc`, `Get`, `GetByRank`, `UpdateState`, `Remove`, unmount/dead-rank cleanup, and eviction. |
| `src/memcache/csrc/meta_service/mmc_meta_container.h` | Abstract metadata/LRU container interface. |
| `src/memcache/csrc/meta_service/mmc_meta_container_lru.cpp` | Standard LRU implementation. |
| `src/memcache/csrc/meta_service/mmc_meta_container_sharded_lru.cpp` | Sharded LRU implementation. |
| `src/memcache/csrc/meta_service/mmc_global_allocator.h` | Physical blob allocation/freeing. It should not own prefix-tree semantics in the first implementation. |

### Existing lifecycle facts that matter

- `MmcMetaManager::Alloc` allocates blobs in `ALLOCATED` state, creates metadata, and inserts it into the container.
- `MmcMetaManager::UpdateState` receives `MMC_WRITE_OK` or `MMC_WRITE_FAIL`.
  - `WRITE_OK` transitions blob state to readable.
  - `WRITE_FAIL` calls `Remove(key)`.
- `MmcMetaManager::Get` and `GetByRank` use real blob states and leases. A pending/allocated-only blob is not readable.
- `MmcMetaManager::CheckAndEvict` delegates normal watermark eviction to
  `MmcMetaContainer::MultiLevelElimination`.
- `MmcMetaManager::EvictCallBackFunction` checks leases and returns `REMOVE`, `MOVE_DOWN`, or `FAIL`.
- `MOVE_DOWN` can retain a key in a lower tier such as SSD. It is not equivalent to deleting the metadata key.

### Benchmark

| Path | Responsibility |
|---|---|
| `example/meta_service_bench/bench_meta_service.cpp` | In-process MetaService + mock buffer-manager benchmark. |
| `example/meta_service_bench/prefix_workload.{h,cpp}` | Deterministic shared-prefix request generator on the benchmark branch. |
| `example/meta_service_bench/README.md` | Benchmark options and usage. |

The benchmark is metadata/control-plane simulation only. It does not replace SGLang/NPU data-plane end-to-end testing.

## Architectural Decisions and Rationale

### Physical component trees, not logical KV-page groups

Use physical keys as tree nodes. K and V remain separate paths, rather than storing a synthetic logical KV group in
MetaService.

Rationale:

- Parent-key API from SGLang and MemCache already speaks physical keys.
- Physical key metadata maps naturally to current MetaService storage metadata.
- The design supports future components beyond K and V.
- It avoids maintaining a second logical-group mapping whose layout must exactly match SGLang.

Consequence: logical page hit validation must continue to require all components. A present `_k` with missing `_v` is a
miss, not a partial success.

### Best-effort prefix index

The planned prefix index is not a transactional ownership graph and is not a source of truth for data validity.

Rationale:

- Fully consistent parent-to-child vectors create costly synchronization on concurrent Put/Remove paths.
- A cache can safely degrade to a miss or ordinary LRU eviction when tree metadata is stale.
- Existing object/blob metadata, states, and leases already enforce safe reads and removals.

Allowed temporary states:

- orphan node whose parent was evicted or failed to register;
- parent node whose child hint is stale;
- stale heap candidate;
- independent forest roots.

Not allowed:

- returning a partial K/V page as a hit;
- using a stale prefix entry to bypass leases, blob states, or MetaContainer checks;
- deleting data directly from the prefix index without normal MetaService eviction/removal validation.

### No parent-to-children vectors in version 1

The index will use a lightweight `childCountHint`, not a list of children. It may also keep a count associated with a
parent key that is not currently represented by a node, so a later parent registration can observe its known children.

`childCountHint == 0` means only "candidate leaf", not a correctness guarantee. Version 1 must not perform cascade
eviction based solely on this hint.

### Lazy heap and generation numbers

The future index should use a lazy min-heap of entries similar to:

```text
{key, generation, accessSequence}
```

`generation` prevents stale-entry/ABA bugs:

```text
old B_k is removed
new B_k is created
an old heap entry for B_k is popped
```

The old heap entry must not evict or remove the new node. Heap entries are validated when popped; stale entries are
discarded cheaply.

### Keep the global allocator unchanged initially

`MmcGlobalAllocator` should continue to allocate and free real blobs. Prefix awareness belongs in victim selection in
MetaService, not in physical allocator behavior.

Normal watermark eviction can later prefer candidates from the prefix index, then fall back to the existing LRU. Initial
prefix work should not modify forced fragmentation eviction or defragmentation eviction.

### Parent-aware request contract

For `N = keys.size()` and `C = parents.size()`, `N % C == 0`. Each of the `C` paths contains `N / C` consecutive keys.

Example:

```text
keys    = [B_k, C_k, B_v, C_v]
parents = [A_k, A_v]
```

Root paths use empty-string parents:

```text
keys    = [A_k, A_v]
parents = ["", ""]
```

Keys must be unique in a request. A non-empty parent cannot be a member of that request's keys. This prevents a cycle or
cross-component parent relation inside one request.

### Retry semantics

Automatic client retry is currently only for `SMEM_BM_GVA_NOT_FOUND`, generally indicating missing or unmounted GVA
registration during data copy. It retries up to `MAX_PUT_RETRY_TIMES` while clearing preferred local-service IDs.

Before the active change, retry collected failed keys and buffers but reused the original full `parents` vector. That
could make a malformed path after filtering keys.

The active implementation builds valid retry suffix runs:

```text
original: A_k -> B_k -> C_k
failed:   C_k
retry:    keys=[C_k], parents=[B_k]
```

For adjacent failures:

```text
original: A_k -> B_k -> C_k
failed:   B_k, C_k
retry:    keys=[B_k, C_k], parents=[A_k]
```

For disjoint failures, it creates separate retry requests so each request remains a valid path and successful keys are
not rewritten.

## Completed Tasks

### Existing branches / reviewed work

- Existing logical-group work was created and pushed previously, but is not the intended final design.
- SGLang PR #1 and MemCache PR #8 were fetched as review branches without merging into main/master.
- Parent-aware SGLang-to-MemCache API direction was reviewed.
- Prefix-tree synthetic benchmark was created and pushed separately on `feature/prefix-tree-workload-simulator`.

### Completed API-contract stage

- Added `ValidateParentPaths` for parent-aware request shape validation.
- Added `BuildParentPathRetries` for safe retry suffix construction.
- Added client-side validation in the C API and `MmcClientDefault` path.
- Added MetaService-side validation in `MmcMetaMgrProxy::BatchAllocWithParents`.
- Added API comments and C/C++/Python/REST documentation updates.
- Added unit tests for path validation and retry construction.
- Added a MetaService proxy test for malformed parent paths.

### PrefixTreeIndex, lifecycle, and normal-eviction stage, implemented but not compiled locally

- Added `mmc_prefix_tree_index.{h,cpp}` and focused unit tests.
- Added parent-aware allocation registration, write readiness, readable-access touch, and full-key removal cleanup.
- Added generation/object identity matching for removal to avoid key recreation (ABA) deleting a newer index node.
- Added opt-in preferred candidate eviction support to both LRU container implementations.
- Added the default-disabled MetaService config option, Python binding, REST metrics, and unit tests for preferred LRU
  candidate validation.

## Remaining Tasks in Implementation Order

### 1. Validate the API-contract change

1. Run the parent-path and MetaManager tests on a machine with `pybind11` installed.
2. Push only this feature branch; do not merge into main/master.

### 2. Validate standalone `PrefixTreeIndex`

Implemented in:

```text
src/memcache/csrc/meta_service/mmc_prefix_tree_index.h
src/memcache/csrc/meta_service/mmc_prefix_tree_index.cpp
test/ut/testcase/memcache/csrc/meta_service/mmc_prefix_tree_index_test.cpp
```

Suggested node state:

```text
key
parentKey
generation
pending / observed-readable state
lastAccessSequence
childCountHint
```

Suggested operations:

```text
RegisterPending(key, parent)
MarkReady(key)
Touch(key)
RemoveIfGeneration(key, generation)
PopLeafCandidate()
```

Required initial tests:

- independent K/V paths;
- root and orphan registration;
- duplicate/idempotent registration;
- conflicting parents: preserve original edge and report conflict;
- generation/ABA safety;
- stale heap entry discard;
- child-hint increment/decrement;
- concurrent register/remove/touch.

Do not add cascade eviction in this stage.

### 3. Validate MetaService lifecycle integration

Implemented. Validate the added `PrefixTreeLifecycleTracksSuccessfulParentAwareAllocations` test and existing removal,
unmount, and eviction tests on a build-capable machine. `MOVE_DOWN` intentionally retains the prefix node.

### 4. Validate observability and controlled rollout

Implemented in `mmc_meta_metric_manager.{h,cpp}` and the REST metrics facade:

- node count;
- orphan count;
- parent conflicts;
- stale heap pops;
- leaf candidates returned/rejected;
- prefix-selected eviction attempts/successes;
- LRU fallback count.

The default-disabled feature flag is `ock.mmc.meta_service.prefix_eviction.enabled`. Validate config loading, Python
binding, and metrics output before enabling it in a production-like deployment.

### 5. Validate prefix candidates with normal eviction

Implemented for normal watermark eviction only:

```text
MmcMetaManager::CheckAndEvict
  -> MmcMetaContainer::MultiLevelElimination
  -> MmcMetaManager::EvictCallBackFunction
```

The container validates each preferred key while holding its normal identity/LRU locks. The prefix index releases its
own lock before MetaContainer lookup. `MmcGlobalAllocator`, forced fragmentation eviction, and defragmentation eviction
remain unchanged.

Desired behavior:

```text
prefix leaf candidate
  -> validate key is still in requested memory tier
  -> existing EvictCallBackFunction checks lease/state and executes REMOVE or MOVE_DOWN
  -> fallback to ordinary LRU for remaining required eviction
```

Likely files:

```text
src/memcache/csrc/meta_service/mmc_meta_container.h
src/memcache/csrc/meta_service/mmc_meta_container_lru.cpp
src/memcache/csrc/meta_service/mmc_meta_container_sharded_lru.cpp
src/memcache/csrc/meta_service/mmc_meta_manager.cpp
```

### 6. Run benchmark pressure and concurrency scenarios

Use `feature/prefix-tree-workload-simulator` after lifecycle and eviction integration.

Required scenarios:

- deterministic shared-prefix reuse;
- root/orphan paths;
- repeated write failures and retries;
- concurrent writes to shared prefixes;
- pressure causing eviction;
- stale heap candidates after delete/recreate;
- partial K/V page detection;
- comparison against baseline LRU behavior.

### 7. SGLang/NPU end-to-end validation

Only after MetaService unit and simulator tests are stable:

- run SGLang with `enable_parent_keys`;
- verify K/V full-page hit behavior;
- validate pressure/eviction with real NPU memory;
- verify no partial KV reuse and no lease regressions.

## Known Issues and Edge Cases

### Build and validation environment

- CMake/UT are blocked before compilation in this environment: `pybind11` is not installed and CMake resolves an
  invalid `/share/cmake/pybind11` directory.
- The local clang-format is 14, but `.clang-format` contains clang-format 18 configuration (`SortUsingDeclarations:
  Never`). Use clang-format 18 or repository CI for formatting validation.
- The active PrefixTree/lifecycle/eviction work is uncommitted and has not been compiled locally.
- Existing benchmark branch is separate from the API-contract branch; it will need rebase/cherry-pick/merge later, not
  an accidental merge into main/master.

### Concurrency and lifecycle

- `A -> B -> C` may have successful A/C writes while B fails. C can exist physically while B is absent. Future index
  logic must accept C as an orphan until B appears or cleanup occurs.
- K and V can become ready at different times. Complete-page validation is mandatory in SGLang/read logic.
- Parent eviction can race child registration. This may create a forest; it must not affect correctness.
- Parent allocation is not a prerequisite for child allocation in the best-effort model.
- Duplicate allocations must not overwrite a recorded parent edge.
- A stale heap entry may refer to a removed/recreated key. Generation validation is mandatory.
- A child count may become stale under concurrent cleanup. Treat it as a candidate hint only.
- A key moved to SSD remains a metadata key. Do not remove its prefix node merely because a higher-tier blob was evicted.
- Existing leases and `EvictCallBackFunction` are mandatory final guards for eviction.

### Locking rule

Never hold a PrefixTreeIndex lock while calling MetaContainer, `MmcMemObjMeta`, allocator, RPC, or callbacks.

Safe pattern:

```text
pop candidate under PrefixTreeIndex lock
release PrefixTreeIndex lock
ask MetaContainer to validate/evict through normal path
```

This avoids lock-order cycles between tree metadata and existing MetaService locks.

## Testing and Acceptance Criteria

### API-contract stage

Run the repository test script, which builds tests but does not require launching an external MetaService daemon:

```bash
bash script/run_ut.sh mmc_parent_path
```

If filtering does not select the common helper test due to target naming, run the broader relevant unit target/filter and
verify the new `MmcParentPathTest` cases. Also run the MetaManager test containing
`BatchAllocWithParentsRejectsMalformedPaths`.

For the current work also run filters matching:

```text
MmcPrefixTreeIndexTest
PrefixTreeLifecycleTracksSuccessfulParentAwareAllocations
PreferredCandidatesValidateMediaBeforeEviction
```

Acceptance criteria:

- malformed parent paths are rejected by C API/client and by MetaService proxy;
- valid K/V component-major path is accepted;
- root paths using `""` parent are accepted;
- retry preserves a valid parent for every retried path;
- adjacent failures are retried as one suffix;
- disjoint failures do not cause successful keys to be rewritten;
- no existing non-parent-aware Put/Get behavior regresses.

### PrefixTreeIndex stage

Acceptance criteria:

- all index unit tests pass under normal and concurrent execution;
- stale heap entries cannot act on a new generation of the same key;
- no tree operation is required for a successful normal cache read;
- index failures can degrade to LRU/miss behavior without corrupting metadata.

### Eviction stage

Acceptance criteria:

- prefix candidates are only preferences; LRU remains available and correct;
- eviction respects current blob state and active leases;
- keys moved to lower storage tiers remain tracked appropriately;
- complete K/V pages are never read from a partial physical set;
- simulator pressure/concurrency scenarios complete without metadata leaks, crashes, or invalid removals;
- SGLang/NPU end-to-end tests show expected reuse and no incorrect cache hits.
