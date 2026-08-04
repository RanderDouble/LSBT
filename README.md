# LSBTdiscrete vs. LSBTconsecutive

This artifact isolates the approximation cost of the engineering cache in
`code/LSBTdiscrete.hpp`.

## Compared implementations

`LSBTdiscrete` is a byte-for-byte snapshot of the current engineering
header. It maps heat to expiry-version buckets, uses FIFO order within a
bucket, and lazily rescues entries whose actual decayed bucket is warmer than
their physical queue.

`LSBTconsecutive` preserves the same depth-dependent gain, block-version
decay, ghost history, admission path, and layer-protection installation. It
stores the exact recurrence

```text
H_v(k) = H_previous(k) * decay^(v - previous_v) + gain(depth)
```

and evicts the minimum exact heat using a lazy min-heap. Internally it orders
keys by `log(H_v / decay^v)`, which is equivalent at a fixed version and avoids
overflow on long traces. Heap generation numbers discard stale entries after
hits.

The word "oracle" is relative to heat quantization only. This implementation
does not inspect future requests and is not Belady's optimal cache.

## Replay semantics

For each block, the runner preserves contiguous depth groups. Before processing
depth `d`, if the next group is depth `d-1`, resident keys in that next group
are protected. Every request calls `Access`; a miss is immediately followed by
`Admit`; `FinishBlock` advances the decay version once after the whole block.

## Build and run

From this directory:

```powershell
.\scripts\build.ps1
.\runner.exe --self-test
.\runner.exe --algorithm both --trace .\workload\lsbt_ethereum_2024\lsbt_2024-01-01_0w_5w.csv --capacity-ratio 0.01
.\scripts\run_experiments.ps1
.\scripts\summarize.ps1
```

Defaults match the engineering runner: maximum depth 20, retention 0.95, gain
scale 51.2, ghost ratio 8, protection scan budget 16, and automatically sized
discrete buckets. The depth gain is `decay^((depth-1)/(max_depth-1))`; there is
no separate maximum-lag parameter.

## Scope

The reduced runner contains only the two cache policies and does no page-store
I/O. Therefore hit counts are directly comparable to the original read-only
policy replay, while `elapsed_ms` reports policy cost rather than end-to-end
storage latency.
