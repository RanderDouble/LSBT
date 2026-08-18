# Two-Timescale Cache for Hierarchical, Versioned Workloads

> Current frozen implementation: **engineering v3** (2026-08-18)
>
> This README defines the project scope, workload model, code layout, algorithm, design rationale, and the recommended paper organization. It should be treated as the entry point for the current research codebase.

---

## 0. Which code is the current main implementation?

The current recommended implementation is:

- **Raw / immediate / partially batched workloads:** `TwoTimescaleCacheSparse.hpp`
- **Shared LSBT scoring + timing-wheel core:** `PackedLSBT.hpp`
- **Fully version-deduplicated Block workloads:**
  - standalone / small-capacity default: `direct_macro/VersionDedupLSBT.hpp`
  - shared-slot alternative: `VersionDedupCacheSparse.hpp`
- **Optional dense/stable-slot fast path:** `TwoTimescaleCachePacked.hpp`

The research default is therefore:

```text
Raw / partial batching  -> TwoTimescaleCacheSparse
Fully deduplicated      -> Macro-only LSBT
```

`TwoTimescaleCacheSparse.hpp` and the Macro-only implementation use the same Macro retention semantics. The difference is only whether a Micro tier is needed and which metadata backend is cheapest for the execution representation.

The older v1/v2 experimental branches, TTL experiments, learned classifiers, staged-path policies, HVE, and pairwise selectors are **not** part of the current core state machine. Note: the public `Access` API still exposes an optional `speculative_micro_eligible` integration hook. The frozen Ethereum replay currently sets this hook with `p == 1` to reproduce the audited v3 measurements, while Twitter leaves it enabled for all requests. This workload-specific hook is not a universal design principle and should be separately ablated/removed before making a fully generic paper claim.

---

# 1. Applicability and workload model

## 1.1 Target workload class

The algorithm targets **hierarchical, versioned workloads**.

Two properties matter, and they play different roles.

### Hierarchical

Objects belong to a hierarchy, prefix structure, tree, trie, or another structure with a notion of structural position, e.g.:

- Merkle / Merkle-Patricia tries;
- hierarchical prefix indexes;
- radix-tree style KV structures;
- path-based state/index maintenance;
- other workloads where requests naturally touch ancestors and descendants.

Hierarchy is useful because it provides a **cold-start structural prior** before runtime history exists. In a tree, shallow nodes cover a larger logical region and are often more likely to recur than a newly observed deep leaf. The current implementation therefore uses depth only as a bounded cold-start prior; depth is **not** a permanent global eviction order.

This distinction is important:

```text
Hierarchy -> bootstrap prior when history is weak.
History   -> object-specific evidence after the system has observed reuse.
```

The algorithm does not assume that every shallow node is always more valuable than every deep node.

### Versioned

Requests can be associated with a semantic or control epoch/version, such as:

- an Ethereum block;
- a storage snapshot/version;
- a commit/checkpoint;
- a batch generation;
- a cache-control epoch when no sufficiently useful application boundary exists.

A version boundary is **not introduced merely to deduplicate requests**. Its primary role is to define the lifetime of reuse evidence:

```text
within one version       -> Micro / local reuse evidence
across different versions -> Macro / persistent reuse evidence
```

The version boundary is therefore the natural **Macro clock and cooling boundary**. Repeating an object many times inside one version should not automatically create the same long-term retention evidence as observing the object again in several different versions.

This is the main reason the algorithm requires a version/epoch notion.

---

## 1.2 The algorithm is not limited to fully batched LSBT

The target space includes different execution granularities:

```text
Immediate reads/writes / SU
        |
        | increasing batching / coalescing
        v
Partially batched traversal
        |
        v
Fully version-deduplicated LSBT
```

These forms have different amounts of intra-version reuse.

### Raw / immediate representation

All physical/logical node observations are retained. A shared ancestor can therefore be requested many times inside one version.

This is necessary when requests cannot legally wait for a batch, for example:

- latency-sensitive reads;
- immediate writes;
- online path maintenance;
- partial batches mixed with synchronous operations.

Here the Micro tier is useful because real intra-version reuse exists.

### Fully version-deduplicated / Block representation

If the application semantics allow all relevant operations in one version to be coalesced, repeated requests to the same node can be merged so that each `(version, key)` appears once.

In that representation:

```text
intra-version reuse -> removed by construction
Micro demand        -> naturally approaches zero
Macro persistence   -> remains
```

The same design therefore degenerates naturally to a Macro-only cache.

### Partial batching

Partial batching lies between the two endpoints. Some duplicates disappear, while others remain. The algorithm does not need to be told which representation it is running: the amount of actual intra-version repetition determines how much Micro capacity is consumed.

---

## 1.3 Raw -> Block/version-deduplicated conversion

The following pseudocode defines the **representation conversion** used when version-local deduplication is semantically legal.

It must not be interpreted as a requirement that every production workload be converted to Block form. Immediate reads/writes may have to remain Raw.

```text
Algorithm: VersionLocalDedup

Input:
    Raw = [(epoch_1, key_1, depth_1), ...]

Output:
    Block = the first observation of each key in each epoch

current_epoch <- NONE
Seen <- empty set
Block <- empty sequence

for request r in Raw in original order:
    if r.epoch != current_epoch:
        current_epoch <- r.epoch
        Seen.clear()

    if r.key not in Seen:
        Block.append(r)
        Seen.insert(r.key)

return Block
```

Equivalently, for each epoch `v`:

```text
Block_v = stable_unique_by_key(Raw_v)
```

where `stable_unique` preserves first-observation order.

### Important semantic rule

`Raw -> Block` is allowed only when coalescing repeated operations does not violate application semantics.

Examples:

- batched Merkle-tree recomputation: often legal;
- mandatory immediate write visibility: may be illegal;
- online read responses: generally cannot be delayed merely to create a deduplicated batch.

Therefore Raw and Block should be treated as **different legal execution granularities**, not as “bad workload” and “optimized workload.”

---

## 1.4 Choosing the epoch

There are two supported cases.

### A. Strong semantic version exists

Prefer the application boundary when it captures real semantic reuse phases.

Example:

```text
Ethereum -> real block boundary
```

### B. No useful semantic version, or the external batching interval is poorly aligned with cache dynamics

Use a cache-control epoch. Current Twitter experiments use the frozen engineering approximation:

```text
L = 2C raw node arrivals
```

where `C` is cache capacity in objects.

This `2C` value is an engineering approximation, **not a universal constant**. The more general research hypothesis is that a useful control epoch is on the order of the cache turnover/residence timescale:

```text
L* = Theta(C)
```

The code deliberately exposes `EndEpoch()` to keep epoch selection outside the core replacement mechanism.

---

# 2. Repository / folder structure

The current frozen package is located at:

```text
/mnt/data/two_timescale_cache_v3/
```

For continuing the project, use the following logical layout.

```text
two_timescale_cache_v3/
|
|-- README.md                         # this file; project entry point
|-- ENGINEERING_FINAL.md              # frozen v3 parity / CPU / memory audit
|
|-- TwoTimescaleCacheSparse.hpp       # MAIN: Raw / partial-batching cache
|-- PackedLSBT.hpp                    # shared Macro/Micro scoring + timing wheels
|-- VersionDedupCacheSparse.hpp       # shared-slot Macro-only implementation
|-- TwoTimescaleCachePacked.hpp       # optional dense/stable-slot fast path
|
|-- direct_macro/
|   |-- VersionDedupLSBT.hpp          # recommended standalone dedup Macro backend
|   |-- LSBTElasticCore.hpp
|   |-- LSBTGhostFIFO.hpp
|   |-- LSBTState.hpp
|   `-- LSBTTimingWheel.hpp
|
|-- example_eth_replay.cpp            # semantic-block Raw replay example
|-- example_twitter_replay.cpp        # fixed-control-epoch Raw replay example
|
|-- benchmarks/
|   |-- bench_final_eth.cpp
|   |-- bench_twitter_policy.cpp
|   |-- bench_block.cpp
|   |-- parity_eth.csv
|   |-- parity_block_eth.csv
|   |-- engineering_cpu_eth.csv
|   |-- engineering_cpu_twitter.csv
|   |-- engineering_cpu_block_eth.csv
|   `-- engineering_rss_twitter.csv
|
|-- workloads/                        # RECOMMENDED location; large traces not bundled
|   |-- ethereum/
|   |   |-- raw/
|   |   `-- block/
|   `-- twitter/
|       |-- raw/
|       `-- block/
|
`-- results/                          # RECOMMENDED experiment outputs
    |-- main/                          # final baseline comparison tables
    |-- temporal_robustness/           # chunk/day/window robustness
    |-- ablations/                     # mechanism ablations
    |-- oracle/                        # offline/info-oracle gap analysis
    |-- overhead/                      # CPU/RSS/metadata/end-to-end results
    `-- parity/                        # implementation-equivalence checks
```

## 2.1 Where should code go?

Current cache implementation code stays in the project root for the frozen v3 package:

```text
TwoTimescaleCacheSparse.hpp
PackedLSBT.hpp
VersionDedupCacheSparse.hpp
TwoTimescaleCachePacked.hpp
direct_macro/
```

If the repository is later normalized into `src/`, move these together and keep the public API unchanged.

The **main code path to modify** is:

```text
TwoTimescaleCacheSparse.hpp
        -> PackedLSBT.hpp
```

Do not reintroduce dead experimental mechanisms into this path.

## 2.2 Where should workloads go?

Use:

```text
workloads/<dataset>/<representation>/
```

Recommended examples:

```text
workloads/ethereum/raw/trace_2024-01-01_0w_5w.csv
workloads/ethereum/block/trace_2024-01-01_0w_5w_block.csv

workloads/twitter/raw/twitter_cluster52_prefix_raw.csv
workloads/twitter/block/twitter_cluster52_prefix_block.csv
```

Large traces should normally stay outside source control and be referenced by manifest/hash if artifact size is a concern.

## 2.3 Where should experiment results go?

Do not mix new result CSVs with source headers.

Use:

```text
results/main/
results/temporal_robustness/
results/ablations/
results/oracle/
results/overhead/
results/parity/
```

`benchmarks/` in this frozen package contains the v3 engineering audit and can be treated as the predecessor of `results/overhead` and `results/parity`.

---

# 3. Recommended paper outline

This section only specifies the organization of the paper, not the prose.

## 1. Introduction

- Broad problem: caching in hierarchical, versioned systems.
- Different execution granularities: immediate / Raw, partial batching, fully deduplicated.
- Why one continuous cache-history timescale is problematic.
- High-level design idea and contributions.

## 2. Background and Workload Model

### 2.1 Hierarchical state/data structures

Define hierarchy, depth, path traversal, and structural locality.

### 2.2 Versioned execution

Define epoch/version semantics and distinguish semantic versions from cache-control epochs.

### 2.3 Execution-granularity spectrum

Define Raw/SU, partially batched, and fully version-deduplicated/LSBT representations.

### 2.4 Raw -> Block transformation

Give the version-local stable-dedup pseudocode and state when it is / is not semantically legal.

## 3. Motivation and Characterization

- Cold-start / leaf-flooding problem.
- Intra-version reuse versus cross-version persistence.
- Evidence that one scalar history/clock cannot simultaneously optimize both.
- Behavior across Raw and deduplicated representations.
- Capacity/epoch relationship where relevant.

## 4. Design

### 4.1 Design goals and invariants

### 4.2 Structural bootstrap prior

### 4.3 Macro tier

### 4.4 Micro tier

### 4.5 Elastic sharing of total capacity

### 4.6 Spill and boundary carry

### 4.7 Semantic epoch and capacity-scaled control epoch

## 5. Implementation

- Shared sparse slot arena.
- Packed Macro/Micro metadata.
- Timing wheel.
- Intrusive Macro ghost.
- Allocation-free epoch reset.
- Standalone versus host-cache integration.

## 6. Evaluation

### 6.1 Experimental setup and workloads

### 6.2 Baselines

### 6.3 Main hit-rate results

### 6.4 Raw / partial / deduplicated behavior

### 6.5 Capacity sensitivity and temporal robustness

### 6.6 Ablations

### 6.7 Oracle / information-gap analysis

### 6.8 CPU, memory, and end-to-end overhead

## 7. Discussion and Limitations

- Requirement for a meaningful version/control epoch.
- Cases without useful hierarchy.
- Future information versus online information.
- When full batching is impossible.
- Generality beyond the evaluated systems.

## 8. Related Work

- Generic cache replacement.
- Learned/adaptive cache policies.
- Version-aware/batched storage systems.
- Hierarchical/Merkle/trie caching.

## 9. Conclusion

---

# 4. Algorithm overview and design rationale

## 4.1 Core idea

The cache maintains two **logically separate retention domains** under one physical capacity budget `C`:

```text
Macro: cross-version persistence
Micro: within-version reuse
```

The critical invariant is:

```text
Micro reuse must not become Macro heat.
```

A burst of 20 accesses in the current version demonstrates strong local usefulness, but it is not equivalent to observing the object in 20 different versions.

At all times:

```text
|Macro residents| + |Micro residents| <= C
```

There is no hidden extra resident cache capacity.

---

## 4.2 Structural cold-start prior

Before an object has runtime history, the cache needs an admission/retention prior. The current LSBT scoring core uses three depth classes:

```text
depth = 1
depth = 2
depth >= 3
```

with a cold-start gain equivalent to:

```text
rho ^ min(depth - 1, 2)
```

where the default `rho = 0.95`.

Thus shallow objects receive a modest bootstrap advantage. Once reuse/ghost evidence exists, normal runtime evidence dominates. This avoids the cold-start failure mode in which a first wave of leaves floods the cache before history-based policies have learned which upper-level objects recur.

The structural prior is deliberately bounded. It is **not DFE** and does not impose a permanent `shallower > deeper` retention order.

---

## 4.3 Macro tier

Macro represents persistent value across versions.

For a key `x` in epoch `v`:

```text
only the first touch of x in v is allowed to update Macro evidence
```

Subsequent accesses to `x` in the same epoch can hit a Macro resident, but they do **not** call `Macro::Access` and therefore do not renew Macro heat.

Macro has:

- block/control-epoch clock;
- LSBT bucketized retention score;
- one-bit renewal credit;
- timing-wheel eviction ordering;
- long bounded ghost history (`8C` in the current frozen core).

The long ghost is retained because experiments showed that cross-version Macro behavior benefits from historical reappearance information.

---

## 4.4 Micro tier

Micro represents local reuse inside the current epoch.

An access after the first touch in the same epoch is an intra-epoch request. If the object is not already Macro-resident, Micro handles its local residency.

Micro has:

- a fine-grained clock (`EndMicroUnit()`), e.g. transaction/path time;
- the same bucketized retention mechanism;
- **no ghost**;
- complete reset at `EndEpoch()`.

Micro ghost was removed after ablation showed essentially no hit-rate benefit. Local history is intentionally not carried indefinitely because its semantics end with the epoch.

---

## 4.5 Request path

High-level pseudocode for the current Raw/partial implementation:

```text
Access(key, depth):

    if key is first-touch in current epoch:
        mark first-touch

        shrink Macro limit if Micro already occupies capacity

        if Macro hits:
            return HIT

        try Macro admission using structural/runtime Macro evidence

        if Macro evicts a victim:
            if victim already showed real intra-epoch reuse:
                spill victim to Micro

        if incoming key is rejected by Macro and is Micro-eligible:
            try Micro admission

        return MISS

    else:
        mark key as reused in this epoch

        if key is Macro-resident:
            return HIT
            // IMPORTANT: do not renew Macro heat

        if Micro hits:
            return HIT

        try Micro admission
        shrink Macro as needed so total residents <= C
        return MISS
```

---

## 4.6 Elastic capacity sharing

Micro does not reserve a fixed partition in advance.

Its current maximum capacity is:

```text
MicroCap(v) = min(C, 2C/3, 0.5 * U_(v-1))
```

where `U_(v-1)` is the distinct footprint of the previous completed epoch. For the first epoch, the structural cap `2C/3` is used.

The actual resident allocation is elastic:

```text
MacroLimit = C - current_Micro_residents
```

Thus a fully deduplicated workload with no intra-epoch repetition naturally consumes almost no Micro capacity, while Raw/partial workloads can expand Micro only when they exhibit actual local reuse pressure.

---

## 4.7 Macro -> Micro spill

When Micro growth forces Macro to shrink, a Macro victim is not automatically discarded.

If that victim has already shown **realized intra-epoch reuse**, it may be spilled into Micro:

```text
Macro victim + reused-in-current-epoch -> eligible for Micro spill
```

A first-touch-only victim is not given this protection.

This rule preserves already demonstrated local value without converting it into persistent Macro heat.

---

## 4.8 Boundary selective carry

At the end of an epoch, Micro is reset. However, immediately dropping every locally reused object can discard useful cross-version candidates.

The current design therefore considers still-resident Micro objects that actually reused during the finished epoch and offers them to Macro.

Critically, this is done through Macro admission **without calling `Macro::Access`**:

```text
realized Micro reuse -> may become Macro resident
                       but does not fabricate Macro recurrence heat
```

After carry:

```text
Macro clock advances
Micro resets
next epoch begins
```

---

## 4.9 Why the two domains must remain logically separate

The implementation may share metadata storage, hash lookup, slot IDs, and timing-wheel infrastructure, but it must not merge Micro and Macro evidence into one scalar heat.

The research evidence motivating the current design is:

```text
fast/fine clock  -> improves local reuse, hurts persistence
slow/version clock -> improves persistence, can lose local reuse
```

Therefore:

```text
physical data structures may be shared;
retention semantics must remain separate.
```

---

# 5. Engineering implementation

## 5.1 Shared sparse slot arena

`TwoTimescaleCacheSparse` performs one external lookup:

```text
external key -> slot id
```

The slot contains:

```text
MacroState : 16 B
MicroState :  8 B
epoch tag  :  4 B
------------------
Meta       : 28 B
```

Macro and Micro timing wheels use slot IDs directly, avoiding repeated hashing.

## 5.2 Packed epoch tag

One 32-bit value represents:

- current epoch id;
- whether the key is listed in the Micro-touched set;
- whether realized intra-epoch reuse occurred.

This replaces three separate per-key arrays from the old prototype.

## 5.3 Intrusive Macro ghost

Macro ghost links are stored directly in `MacroState`. There is no separate `GhostNode[]` allocation.

## 5.4 Allocation-free Micro reset

Only slots touched by the finished epoch are reset. The hash buckets, slot arena, timing-wheel buffers, and scoring LUTs are reused.

## 5.5 Current measured engineering cost

The v3 rewrite is decision-equivalent to v2 on the audited Ethereum and Twitter workloads.

Compared with the old Dual v2, v3 was approximately `1.2x-1.8x` faster in the frozen policy-only experiments, depending on capacity. Compared with the old single-LSBT policy, the remaining Dual overhead on Ethereum was approximately `4%-21%` in those runs.

See:

```text
ENGINEERING_FINAL.md
benchmarks/engineering_cpu_eth.csv
benchmarks/engineering_cpu_twitter.csv
benchmarks/engineering_rss_twitter.csv
```

for the frozen measurements and parity checks.

---

# 6. Building the examples

Ethereum Raw with semantic block epochs:

> **Reproducibility note:** the frozen Ethereum example passes `p == 1` to the optional `speculative_micro_eligible` hook in order to reproduce the audited v3 numbers. The cache core defaults this hook to `true`; this path-role gate is not required by the generic two-timescale model and should be reported as an integration choice if those exact numbers are used.

```bash
g++ -O3 -DNDEBUG -std=c++20 example_eth_replay.cpp -o eth_replay
./eth_replay workloads/ethereum/raw/trace_2024-01-01_0w_5w.csv 7708
```

Twitter Raw with the current fixed control epoch `L=2C`:

```bash
g++ -O3 -DNDEBUG -std=c++20 example_twitter_replay.cpp -o twitter_replay
./twitter_replay workloads/twitter/raw/twitter_cluster52_prefix_raw.csv 6156
```

For additional peak-resident diagnostics:

```bash
-DTT_CACHE_DIAGNOSTICS
```

The default release path leaves these diagnostics out of the per-request hot path.

---

# 7. Frozen design invariants

Any future implementation should preserve these unless a new experiment explicitly changes the algorithm:

1. `Macro` and `Micro` maintain independent retention evidence.
2. `Macro` advances on the semantic/control epoch clock.
3. `Micro` advances on a finer-grained unit clock and resets at epoch boundaries.
4. Intra-epoch Macro hits **do not renew Macro heat**.
5. Total resident capacity is strict: `|Macro| + |Micro| <= C`.
6. Macro victims spill to Micro only after realized local reuse.
7. Boundary carry does not fabricate Macro access evidence.
8. Macro keeps the validated long ghost; Micro remains ghostless.
9. Depth is a bounded cold-start prior, not a permanent eviction hierarchy.
10. No classifier, TTL, staged-path heuristic, or learned selector is part of the core state machine. The current API still exposes the optional `speculative_micro_eligible` integration hook; the Ethereum parity harness uses `p == 1` to reproduce frozen measurements. Treat this as an experimental integration choice, not as a generic workload invariant.

---

# 8. What should be modified next?

For algorithm research, modify only after defining a falsifiable mechanism-level hypothesis.

For engineering work, preferred directions are:

- integrate packed metadata into an existing host cache entry/slot to eliminate the standalone hash lookup;
- benchmark end-to-end storage latency/throughput, not only policy-only CPU;
- measure exact metadata bytes per resident/ghost/tracked object;
- test multi-thread synchronization/sharding strategies;
- preserve hit/stat parity when changing only data structures.

Do **not** simplify the implementation by collapsing Macro and Micro evidence into one heat variable; that would change the central algorithmic semantics rather than merely reduce engineering overhead.

