# Two-timescale cache engineering simplification — frozen v3

Date: 2026-08-18

## Goal

Reduce implementation and policy overhead **without changing cache semantics**. Every optimization below was accepted only after hit-by-hit parity against the v2 paper-candidate implementation.

## Frozen semantic invariants

The v3 implementation preserves all mechanisms currently supported by experiments:

1. Macro and Micro keep independent retention evidence.
2. Macro advances on the control-epoch/version clock.
3. Micro advances on the fine-grained transaction/path clock and resets at the epoch boundary.
4. Micro hits never renew Macro heat.
5. Total resident capacity is exact: `|Macro| + |Micro| <= C`.
6. A Macro victim spills to Micro only after realized intra-epoch reuse.
7. A still-resident Micro object with realized intra-epoch reuse may be offered to Macro at the epoch boundary without calling `Macro::Access`.
8. Macro keeps the validated long ghost; Micro has no ghost.

No classifier, staged-path rule, TTL, bucket comparator, or learned selector is present.

## What was removed or packed

### 1. Two hash tables -> one shared sparse index for Raw

v2 maintained independent `unordered_map` state for Macro and Micro. v3 performs one external `key -> slot` lookup. Macro state, Micro state, and epoch flags share that slot. Timing wheels and the ghost FIFO link slot IDs directly and do not hash again.

### 2. Three per-key epoch arrays -> one 32-bit tag

v2 stored `seen_epoch`, `reused_epoch`, and `micro_mark` as three 32-bit vectors (12 B/key). v3 uses one 32-bit field:

- lower 30 bits: epoch id,
- bit 30: listed in the Micro touched list,
- bit 31: realized intra-epoch reuse.

For the standalone sparse implementation this tag exists only for currently tracked slots, not the whole key universe.

### 3. Micro state 16 B -> 8 B

Micro has no ghost. Its state is only:

- packed expiry + renewal bit: 4 B,
- timing-wheel next link: 4 B.

### 4. Ghost node array removed

v2 stored a 16 B LSBT state plus a separate 12 B `GhostNode` for each Macro ghost. v3 stores the ghost FIFO links intrusively in the 16 B Macro state. No separate ghost-node vector is allocated.

### 5. Eviction log removed

v2 accumulated Macro victims in `pending_evicted_keys` and drained the vector after each operation. v3 returns the victim directly to the two-timescale controller. No hot-path eviction-log allocation/copying remains.

### 6. Structural transition LUT collapsed

The structural cold-start weight is `rho^min(depth-1,2)`. Therefore all depths belong to only three distinct classes: depth 1, depth 2, depth >=3. v3 stores `3 * bucket_count` transitions instead of `(max_depth+1) * bucket_count`, and no longer stores a separate representative-heat vector.

### 7. Epoch reset is allocation-free

Micro state is reset only for slots touched during the finished epoch. Slot storage, hash buckets, timing-wheel storage, and LUT buffers are reused.

### 8. Diagnostics are outside the production hot path

Compile with `-DTT_CACHE_DIAGNOSTICS` to update peak-resident assertions/counters. The default release path does not execute per-request peak bookkeeping.

## Exact parity

### Ethereum Raw, semantic block boundary

v2 and v3 shared-sparse produce exactly identical hits, HR/MHR/IHR, carry/spill counts, and peak resident counts.

| C | Hits | HR | MHR | IHR | Carries | Spills |
|---:|---:|---:|---:|---:|---:|---:|
| 3,854 | 1,517,007 | 75.8503% | 11.4839% | 96.6515% | 133,649 | 27,744 |
| 7,708 | 1,603,942 | 80.1971% | 23.3265% | 98.5758% | 112,662 | 25,964 |
| 15,416 | 1,663,969 | 83.1985% | 33.3086% | 99.3212% | 64,432 | 8,175 |
| 30,832 | 1,709,411 | 85.4706% | 41.2433% | 99.7634% | 29,334 | 2,202 |

### Twitter cluster52 Raw, fixed `L=2C`

At `C=6156`, both versions produce exactly:

- HR = 0.716157000
- MHR = 0.312418303
- IHR = 0.995892901
- peak Micro = 2,484
- peak resident = 6,156

### Ethereum version-deduplicated / Block

The packed sparse Macro-only specialization matches the direct v2 Macro implementation exactly at all checked capacities; resident and ghost sizes also match.

## CPU: Raw two-timescale cache

Policy-only replay; trace is parsed once and replay timing excludes parsing. `-O3 -DNDEBUG -march=native`.

### Ethereum, 2,000,000 node requests

| C | Dual v2 | Shared-sparse v3 | Speedup | v3 / old single-LSBT |
|---:|---:|---:|---:|---:|
| 3,854 | 167.127 ms | 117.480 ms | 1.423x | 1.211x |
| 7,708 | 125.815 ms | 77.707 ms | 1.619x | 1.079x |
| 15,416 | 115.719 ms | 62.972 ms | 1.838x | 1.037x |
| 30,832 | 84.291 ms | 53.649 ms | 1.571x | 1.200x |

The old prototype's 1.5--1.8x two-tier overhead over single LSBT was therefore largely an implementation artifact. After simplification, the standalone shared-sparse Dual is only about **3.7--21.1%** above the old single-LSBT policy in these runs.

### Twitter, 8,000,000 node requests, `L=2C`

| C | Dual v2 | Shared-sparse v3 | Speedup |
|---:|---:|---:|---:|
| 3,078 | 1077.159 ms | 783.211 ms | 1.375x |
| 6,156 | 1148.351 ms | 918.362 ms | 1.250x |
| 12,312 | 1186.898 ms | 966.205 ms | 1.228x |
| 24,624 | 1255.739 ms | 951.124 ms | 1.320x |

## Memory

### Standalone initialization

At `key_count=209,035`:

- `C=3,854`: v2 ~6.3 MB RSS; shared-sparse v3 ~4.0 MB.
- `C=24,624`: v2 ~7.9 MB RSS; shared-sparse v3 ~5.4 MB.

Unlike the dense fast path, shared-sparse metadata scales with tracked objects rather than the entire key universe.

### Full Twitter replay peak RSS

| C | v2 | shared-sparse v3 | reduction | dense fast path |
|---:|---:|---:|---:|---:|
| 6,156 | 18.44 MB | 7.64 MB | 58.5% | 28.98 MB |
| 24,624 | 29.50 MB | 19.66 MB | 33.4% | 29.05 MB |

The dense version is a useful CPU upper bound when the host system already owns a compact stable slot ID, but it should not be the standalone default because it allocates policy metadata over the whole key-id space.

## Block / deduplicated specialization

Block has no Micro tier after version-local deduplication. There is therefore no duplicated lookup to eliminate. A standalone `key -> MacroState` implementation is already near-minimal.

The new shared-slot Macro-only backend is exact but is ~21--22% slower at the two smallest tested capacities (C=1,927 and 3,854), then becomes ~4--10% faster at C>=7,708. Therefore the recommended engineering choice is:

- **Raw / partial batching:** shared-sparse two-timescale implementation.
- **Fully version-deduplicated standalone stream:** direct single-map Macro LSBT remains the lowest-risk small-capacity backend.
- **Integrated system with an existing cache key->entry lookup:** attach packed Macro/Micro state to the host entry/slot; then the extra standalone hash map disappears and the packed-slot path is the relevant implementation.

This is a data-structure specialization only; the Macro retention semantics are identical.

## Remaining overhead

After removing duplicated maps/ghosts/logs, the remaining two-timescale cost is real algorithmic work:

1. one epoch-tag check per request;
2. on intra-epoch requests, checking Macro residence before Micro;
3. a second timing wheel for Micro;
4. elastic Macro shrinking when Micro grows;
5. boundary selective carry over Micro-touched slots.

On the same sparse lookup infrastructure, the current Dual costs roughly 1.15--1.39x a single Macro LSBT in the measured Ethereum capacities. This is the genuine policy overhead to optimize next; it is no longer dominated by avoidable container duplication.

## Recommended code path

Use `TwoTimescaleCacheSparse.hpp` as the standalone Raw/partial-batching implementation.

Use `VersionDedupLSBT` (direct Macro backend) for the current standalone fully-deduplicated Block evaluation when small-capacity CPU is important. `VersionDedupCacheSparse.hpp` is provided as the exact shared-slot alternative and is preferable when integrated with an existing slot/index layer.

`TwoTimescaleCachePacked.hpp` is retained only as an optional dense/stable-slot fast path and as a lower-overhead engineering bound.
