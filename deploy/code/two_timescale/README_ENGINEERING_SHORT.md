# Two-timescale cache v3 — engineering-frozen implementation

This package is the mechanism-preserving engineering cleanup of the current cache candidate.

## Recommended files

- `PackedLSBT.hpp`: packed LSBT scoring, timing wheel, intrusive Macro ghost, and 8-byte Micro state.
- `TwoTimescaleCacheSparse.hpp`: **recommended standalone Raw / partially batched implementation**. One hash lookup maps key -> shared slot.
- `VersionDedupCacheSparse.hpp`: Macro-only shared-slot specialization for version-deduplicated streams.
- `TwoTimescaleCachePacked.hpp`: optional dense/stable-slot fast path; use only when the host already has compact slot IDs or the key universe is small.
- `direct_macro/VersionDedupLSBT.hpp`: the previous direct single-map Macro backend. It remains a good standalone Block backend at very small capacities.

## Build

Raw Ethereum example:

```bash
g++ -O3 -DNDEBUG -std=c++20 example_eth_replay.cpp -o eth_replay
./eth_replay trace_2024-01-01_0w_5w.csv 7708
```

Add `-DTT_CACHE_DIAGNOSTICS` when peak resident assertions/counters are needed.

Twitter fixed control epoch example:

```bash
g++ -O3 -DNDEBUG -std=c++20 example_twitter_replay.cpp -o twitter_replay
./twitter_replay twitter_cluster52_prefix_raw.csv 6156
```

The example keeps the currently frozen Twitter engineering approximation `L=2C`.

## Important

The v3 rewrite changes **data structures only**, not cache decisions. Ethereum and Twitter replays were checked for exact hit/stat parity against v2.

See `ENGINEERING_FINAL.md` for CPU, RSS, state-size, parity, and backend recommendations.
