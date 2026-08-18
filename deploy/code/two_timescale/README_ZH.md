# Two-Timescale Cache：面向层次化、版本化 Workload 的双时间尺度缓存

> 当前冻结版本：**engineering v3（2026-08-18）**
>
> 本 README 用来定义：研究适用范围、Raw/Block workload 表示、当前代码入口、目录规范、算法设计、工程实现，以及后续论文的大纲。后续开发默认以本文件和 v3 代码为准。

---

# 0. 现在到底应该用哪份代码？

当前推荐主实现：

```text
Raw / immediate / partial batching
    -> TwoTimescaleCacheSparse.hpp

Fully version-deduplicated / Block
    -> direct_macro/VersionDedupLSBT.hpp
       （standalone 小容量下目前更合适）
```

公共底层实现：

```text
PackedLSBT.hpp
```

其它文件：

```text
VersionDedupCacheSparse.hpp
    Macro-only 的 shared-slot 版本；语义与 direct Macro 一致。

TwoTimescaleCachePacked.hpp
    Dense/stable-slot 快速路径；适合宿主系统已经有稳定 slot id，
    不适合作为 standalone 默认实现。
```

当前项目根目录：

```text
/mnt/data/two_timescale_cache_v3/
```

旧的 v1/v2、HVE、TTL、classifier、staged path、pairwise comparator 等实验分支均不属于当前主算法。

**需要特别注意一个当前代码事实：**

`TwoTimescaleCacheSparse::Access()` 仍保留一个可选参数：

```cpp
speculative_micro_eligible
```

核心算法默认它为 `true`；Twitter replay 也是所有请求都允许。为了逐位复现当前冻结 Ethereum v3 数字，`example_eth_replay.cpp` 和 parity benchmark 里仍使用了：

```cpp
p == 1
```

作为 first-touch 被 Macro 拒绝后能否 speculative 进入 Micro 的 integration gate。

因此：

```text
两时间尺度状态机 = 通用核心机制
p == 1 gate         = 当前 Ethereum 复现实验中的 workload-specific integration choice
```

论文若主张完全通用，后者必须单独消融、删除或明确报告，不能把它伪装成通用设计原则。

---

# 1. 适用范围

## 1.1 我们研究的不是“只适用于 LSBT 的缓存”

目标 workload 是：

```text
Hierarchical + Versioned Workloads
层次化 + 版本化 Workloads
```

它可以处于不同执行粒度：

```text
Immediate read/write / Sequential Update
                |
                | batching / coalescing 增强
                v
        Partially batched
                |
                v
       Fully deduplicated LSBT
```

也就是说，我们希望**同一个缓存机制**能够覆盖：

- 必须立即执行的读；
- 必须立即可见的写；
- sequential path update；
- 部分 batching；
- version 内完全去重的 LSBT。

LSBT 只是这条 execution-granularity spectrum 的一个端点，而不是论文唯一 workload。

---

## 1.2 为什么要求“层次化”？

层次化结构包括但不限于：

- Merkle Tree / Merkle-Patricia Trie；
- radix/prefix tree；
- hierarchical KV/index；
- path-based state/index maintenance；
- 任何对象具有 ancestor/descendant、prefix/depth 等结构属性的 workload。

Hierarchy 在当前设计中的核心作用不是永久决定 eviction 顺序，而是提供：

```text
cold-start structural prior
```

系统刚启动、Ghost 还没有形成、对象历史还很弱时，纯历史策略无法区分：

```text
shallow ancestor
vs
大量一次性 deep leaves
```

容易发生：

```text
leaf flooding
    -> first epoch 内大量叶节点先填满 cache
    -> 高频跨版本浅层节点被冲掉
    -> 后续只能依赖 ghost/history 再慢慢学回来
```

层次结构提供一个无需等待历史即可使用的 bootstrap prior。

当前实现只给 shallow node 一个**有界冷启动优势**，而不是使用永久规则：

```text
depth(u) < depth(v) => u 永远比 v 更重要
```

因此：

```text
Hierarchy -> 冷启动先验
History   -> 后续 object-specific evidence
```

---

## 1.3 为什么要求“版本化”？

Version 的主要作用**不是为了去重**。

它最重要的作用是定义 reuse evidence 的时间语义：

```text
同一个 version 内重复访问
    -> Micro / local evidence

跨多个 version 再次出现
    -> Macro / persistent evidence
```

例如：

```text
version v:
    x x x x x x x
```

只能证明 `x` 在当前 version 内很有价值，不能等价解释为：

```text
x 连续跨 7 个 version 都有价值
```

因此 version boundary 是：

```text
1. Macro 的逻辑 clock；
2. local evidence 的 cooling/reset point；
3. 区分 intra-version locality 和 cross-version persistence 的语义边界。
```

这就是为什么算法需要 version / epoch。

Version 可以来自：

- Ethereum block；
- snapshot；
- commit/checkpoint；
- batch generation；
- application version；
- 当没有合适 semantic version 时，由 cache 自己定义 control epoch。

---

## 1.4 Raw、Partial、Block 都属于适用范围

### Raw / immediate workload

保留所有原始节点请求。

共享 ancestor 可以在同一个 epoch/version 内重复出现很多次。

适用于：

- 立即读；
- 立即写；
- sequential update；
- 不能等待 batch 的路径请求。

这类 workload 存在真实的 intra-version reuse，因此 Micro 有价值。

### Fully deduplicated / Block workload

如果应用语义允许 batching/coalescing，可以在 version 内把重复 key 合并，只保留第一次访问。

此时：

```text
每个 (version, key) 最多一次
```

所以：

```text
intra-version reuse ~= 0
Micro demand        ~= 0
Macro persistence   仍然存在
```

缓存自然退化为 Macro-only。

### Partial batching

真实系统很可能介于两者之间。

部分重复请求被 coalesce，部分 immediate request 仍保留。

当前设计不需要预先声明：

```text
"我是 Raw" 或 "我是 Block"
```

实际 intra-version reuse 压力会决定 Micro 实际占多少空间。

---

# 2. Raw -> Block 的伪代码

这个转换只表示：

```text
如果 application semantics 允许 version-local coalescing，
如何从 Raw 表示得到 version-deduplicated 表示。
```

它**不是**要求生产系统必须把所有 Raw 请求变成 Block。

立即读、立即写可能根本不允许延迟和去重。

## Algorithm: Version-Local Stable Dedup

```text
Input:
    Raw = [(epoch_1, key_1, depth_1), ...]

Output:
    Block = 每个 epoch 内，每个 key 只保留第一次出现；
            保留第一次出现的原始顺序。

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

等价表示：

```text
Block_v = stable_unique_by_key(Raw_v)
```

### 什么时候可以这么转？

可以：

```text
batch update / batch recomputation
允许在 version 结束前统一合并相同节点处理
```

不一定可以：

```text
immediate read
immediate write visibility
具有强实时顺序依赖的操作
```

因此 Raw 和 Block 是两种**合法执行粒度**，而不是“原始错误版本”和“正确优化版本”。

---

# 3. Epoch 怎么定义？

## 3.1 有强 semantic version

优先使用 application semantic boundary。

当前 Ethereum：

```text
epoch = real block
```

原因是 block 能真正表达 transaction 之间属于同一个 state version 的局部 reuse。

## 3.2 没有好的 semantic version

可以使用 cache-control epoch。

当前 Twitter 冻结 engineering rule：

```text
L = 2C raw-node arrivals
```

其中 `C` 是对象数容量。

但必须明确：

```text
2C 不是普适定律。
```

现阶段更一般的 research hypothesis 是：

```text
optimal control epoch ~= cache turnover / residence timescale
L* = Theta(C)
```

所以算法核心只暴露：

```cpp
EndEpoch()
```

而不把 2C 写死在 cache implementation 中。

---

# 4. 文件夹结构

## 4.1 当前冻结代码包

```text
two_timescale_cache_v3/
|
|-- README.md
|-- README_ZH.md
|-- ENGINEERING_FINAL.md
|
|-- TwoTimescaleCacheSparse.hpp       # 当前 Raw/partial 主实现
|-- PackedLSBT.hpp                    # Macro/Micro 公共核心
|-- VersionDedupCacheSparse.hpp       # shared-slot Macro-only
|-- TwoTimescaleCachePacked.hpp       # dense/stable-slot fast path
|
|-- direct_macro/
|   |-- VersionDedupLSBT.hpp          # standalone Block 推荐 Macro backend
|   |-- LSBTElasticCore.hpp
|   |-- LSBTGhostFIFO.hpp
|   |-- LSBTState.hpp
|   `-- LSBTTimingWheel.hpp
|
|-- example_eth_replay.cpp
|-- example_twitter_replay.cpp
|
`-- benchmarks/
    |-- bench_final_eth.cpp
    |-- bench_twitter_policy.cpp
    |-- bench_block.cpp
    |-- parity_eth.csv
    |-- parity_block_eth.csv
    |-- engineering_cpu_eth.csv
    |-- engineering_cpu_twitter.csv
    |-- engineering_cpu_block_eth.csv
    `-- engineering_rss_twitter.csv
```

---

## 4.2 后续研究建议采用的完整项目结构

```text
project/
|
|-- README.md
|-- README_ZH.md
|
|-- src/
|   |-- TwoTimescaleCacheSparse.hpp
|   |-- PackedLSBT.hpp
|   |-- VersionDedupCacheSparse.hpp
|   |-- TwoTimescaleCachePacked.hpp
|   `-- direct_macro/
|
|-- examples/
|   |-- example_eth_replay.cpp
|   `-- example_twitter_replay.cpp
|
|-- workloads/
|   |-- ethereum/
|   |   |-- raw/
|   |   `-- block/
|   |
|   `-- twitter/
|       |-- raw/
|       `-- block/
|
|-- experiments/
|   |-- runners/
|   |-- analysis/
|   `-- scripts/
|
|-- results/
|   |-- main/
|   |-- temporal_robustness/
|   |-- ablations/
|   |-- oracle/
|   |-- overhead/
|   `-- parity/
|
`-- docs/
    |-- design/
    |-- audits/
    `-- paper/
```

### 代码放哪里？

正式仓库建议：

```text
src/
```

当前冻结包为了不改变 include path，header 还在 root。

当前真正需要修改的主路径只有：

```text
TwoTimescaleCacheSparse.hpp
        -> PackedLSBT.hpp
```

不要把已经证伪的实验策略重新塞回主代码。

### Workload 放哪里？

统一：

```text
workloads/<dataset>/<representation>/
```

例如：

```text
workloads/ethereum/raw/trace_2024-01-01_0w_5w.csv
workloads/ethereum/block/trace_2024-01-01_0w_5w_block.csv

workloads/twitter/raw/twitter_cluster52_prefix_raw.csv
workloads/twitter/block/twitter_cluster52_prefix_block.csv
```

大 trace 不一定进 git；可以只保存：

- 下载/生成脚本；
- manifest；
- SHA256/hash；
- schema；
- workload statistics。

### 结果放哪里？

统一：

```text
results/main/
results/temporal_robustness/
results/ablations/
results/oracle/
results/overhead/
results/parity/
```

不要把新的 CSV 继续散落在代码目录。

当前 `benchmarks/` 是冻结 v3 工程审计遗留目录，之后可以对应迁移到：

```text
results/overhead/
results/parity/
```

---

# 5. 当前算法是什么？

核心只有一句话：

```text
在一个严格总容量 C 下，
物理共享 cache capacity，
逻辑分离 Micro 和 Macro retention evidence。
```

即：

```text
Macro = cross-version persistence
Micro = intra-version locality
```

最核心 invariant：

```text
Micro hit 不产生 Macro heat。
```

因为：

```text
同一个 block 内命中 20 次
!=
连续 20 个 block 都出现一次
```

---

# 6. Structural cold-start prior

没有历史时，LSBT/历史型策略需要一个 cold-start admission prior。

当前 scoring 把 depth 压成三类：

```text
depth = 1
depth = 2
depth >= 3
```

cold-start gain：

```text
rho ^ min(depth - 1, 2)
```

默认：

```text
rho = 0.95
```

作用：

```text
没有 ghost/history 时，
给 shallow object 一个轻量 bootstrap 优势，
减少第一个 epoch 被 leaf flooding 完全冲刷。
```

但它不是永久 depth priority。

一旦 object 有 resident/ghost reuse，后续 runtime evidence 会主导。

因此不要再把当前算法理解为 DFE。

---

# 7. Macro 设计

Macro 表示：

```text
跨 version persistence
```

对于 key `x` 在 epoch `v`：

```text
只有第一次 touch 可以更新 Macro evidence。
```

后续同 epoch 对 `x` 的访问：

- 如果 Macro resident，可以直接 hit；
- 但是不调用 `Macro::Access()`；
- 所以不会 renew Macro heat。

Macro 当前包含：

```text
version/control-epoch clock
bucketized heat
one-bit renewal credit
timing wheel
long bounded ghost (8C)
```

Macro ghost 保留，因为之前实验已经证明 Block/cross-version persistence 对长 ghost history 有显著依赖。

---

# 8. Micro 设计

Micro 表示：

```text
当前 epoch 内 local reuse
```

同一个 epoch 第二次及后续访问，如果不在 Macro resident，则进入 Micro 逻辑。

Micro：

```text
fine-grained clock
例如 transaction / path unit

ghostless

epoch boundary 完全 reset
```

Micro ghost 已经做过消融：几乎没有 HR 收益，所以在 v3 被删除。

这也符合语义：

```text
local evidence 本来就不应该无限跨 epoch 延续。
```

---

# 9. 每次 Access 的核心伪代码

```text
Access(key, depth):

    if key 是当前 epoch 第一次出现:

        记录 first-touch

        根据当前 Micro resident 数量调整 Macro limit

        if Macro hit:
            return HIT

        尝试 Macro admission

        if Macro eviction 产生 victim:
            if victim 已经在本 epoch 真实复用过:
                尝试 spill victim -> Micro

        if incoming 被 Macro 拒绝
           and speculative_micro_eligible:
            尝试放入 Micro

        return MISS

    else:

        标记 reused-in-this-epoch

        if Macro resident:
            return HIT
            # 不更新 Macro heat

        if Micro hit:
            return HIT

        尝试 Micro admission

        根据 Micro 当前 resident 数量收缩 Macro，保证总容量不超过 C

        return MISS
```

---

# 10. Elastic capacity sharing

严格约束：

```text
|Macro| + |Micro| <= C
```

没有额外 resident capacity。

Micro 不提前固定占一块容量，而是 elastic growth。

当前 maximum Micro cap：

```text
MicroCap(v)
    = min(C,
          2C/3,
          0.5 * U_(v-1))
```

其中：

```text
U_(v-1) = 上一个完成 epoch 的 distinct footprint
```

第一 epoch 没有历史，使用：

```text
2C/3
```

实际 Macro limit：

```text
MacroLimit = C - current_Micro_residents
```

因此：

### Raw

intra reuse 多：

```text
Micro 会实际占用一部分 capacity
```

### Fully dedup Block

同 epoch 没重复：

```text
Micro 几乎不占空间
算法退化为 Macro-only
```

这就是为什么同一个逻辑设计能覆盖不同 batching granularity。

---

# 11. Macro -> Micro spill

当 Micro 增长导致 Macro 必须 shrink 时，并不是所有 Macro victim 都直接丢掉。

如果 victim 已经：

```text
在本 epoch 中真实复用过
```

说明它具有已经被观测到的 local value。

因此：

```text
Macro victim
    + realized intra-epoch reuse
        -> eligible for Micro spill
```

如果只出现过一次，则不 spill。

核心原则：

```text
可以保存已经证明的 local value，
但不能把 local value 伪造成 Macro heat。
```

---

# 12. Boundary selective carry

epoch 结束时 Micro 要 reset。

但是如果一个 Micro resident 在刚结束的 epoch 内确实被反复使用，直接全部清掉可能浪费仍然有潜在 persistence 的对象。

因此 boundary 时：

```text
still-resident Micro object
+ realized reuse
    -> 可以尝试 Macro admission
```

关键：

```text
不调用 Macro::Access()
```

所以：

```text
可以迁移 residency
但不会伪造一次 cross-version recurrence evidence
```

完成 carry 后：

```text
Macro.Tick()
Micro.Reset()
进入下一个 epoch
```

---

# 13. 为什么不能把 Micro/Macro 合成一个 heat？

这是整个算法最不能破坏的地方。

我们已经观察到：

```text
fast/fine clock
    -> local reuse 更好
    -> persistence 更差

slow/version clock
    -> persistence 更好
    -> local reuse 可能变差
```

所以一个 scalar heat 同时编码：

```text
local frequency + cross-version persistence
```

会产生 evidence interference。

因此：

```text
可以共享：
    hash lookup
    slot arena
    metadata memory
    queue/timing-wheel infrastructure

不能共享：
    retention evidence semantics
```

一句话：

```text
Physical implementation can be shared;
logical retention domains cannot be collapsed.
```

---

# 14. v3 工程实现为什么这样写？

## 14.1 一个 key 只做一次 hash

`TwoTimescaleCacheSparse`：

```text
external key
    -> unordered_map lookup
    -> compact slot id
```

之后所有 Macro/Micro/timing-wheel/ghost 操作全部使用 slot。

不再维护：

```text
Macro unordered_map + Micro unordered_map
```

---

## 14.2 Shared Meta

每个 tracked slot 当前 metadata：

```text
MacroState : 16 B
MicroState :  8 B
epoch_tag  :  4 B
-----------------
Meta       : 28 B
```

---

## 14.3 一个 32-bit epoch tag

替代旧实现的：

```text
seen_epoch[]
reused_epoch[]
micro_mark[]
```

一个 tag 同时保存：

```text
epoch id
listed-in-micro-touched-list bit
realized-reuse bit
```

---

## 14.4 Micro 无 ghost

MicroState 只需要：

```text
expiry + renewal
wheel next pointer
```

共 8B。

---

## 14.5 Macro ghost intrusive

ghost prev/next 直接放在 MacroState 中。

不再单独：

```text
GhostNode[]
```

---

## 14.6 Epoch reset 不重新分配

只 reset 本 epoch touched slots。

复用：

```text
hash buckets
slot arena
wheel storage
LUT
```

不再每个 epoch new/delete Micro cache。

---

# 15. 当前工程开销

详细数字见：

```text
ENGINEERING_FINAL.md
```

冻结 Ethereum policy-only 结果中：

```text
v3 相比旧 Dual v2：约 1.4x~1.8x 更快（视容量而定）
```

相对旧 single-LSBT：

```text
当前 Dual 的额外 policy CPU 大约只剩 4%~21%
```

而旧 prototype 曾经大约是：

```text
1.5x~1.8x single-LSBT
```

说明原先很大的 overhead 主要来自重复数据结构，不是双时间尺度机制天然必须支付的成本。

Twitter full replay 中 sparse v3 也显著减少 RSS。

冻结结果文件：

```text
benchmarks/engineering_cpu_eth.csv
benchmarks/engineering_cpu_twitter.csv
benchmarks/engineering_cpu_block_eth.csv
benchmarks/engineering_rss_twitter.csv
benchmarks/parity_eth.csv
benchmarks/parity_block_eth.csv
```

---

# 16. 论文建议大纲

这里只给结构，不写具体正文。

## 1. Introduction

- hierarchical + versioned caching problem；
- immediate / partial / dedup 不同执行粒度；
- single-timescale evidence interference；
- high-level design 和 contribution。

## 2. Background & Workload Model

### 2.1 Hierarchical data/state structures

### 2.2 Version / epoch semantics

### 2.3 Execution-granularity spectrum

```text
SU / Raw -> partial batching -> fully deduplicated LSBT
```

### 2.4 Raw -> Block stable-dedup definition

说明什么时候语义允许转换、什么时候不能。

## 3. Motivation & Characterization

- hierarchy cold-start / leaf flooding；
- intra-version reuse；
- cross-version persistence；
- clock/timescale conflict；
- Raw 与 dedup 两端行为；
- semantic epoch 与 capacity-scaled control epoch。

## 4. Design

### 4.1 Design goals / invariants

### 4.2 Structural bootstrap prior

### 4.3 Macro domain

### 4.4 Micro domain

### 4.5 Elastic capacity sharing

### 4.6 Macro->Micro spill

### 4.7 Boundary carry

### 4.8 Epoch selection

## 5. Implementation

- shared sparse slot；
- packed metadata；
- timing wheel；
- intrusive ghost；
- allocation-free reset；
- standalone vs host-cache integration。

## 6. Evaluation

### 6.1 Workloads / setup

### 6.2 Baselines

### 6.3 Main hit-rate comparison

### 6.4 Raw / partial / Block granularity

### 6.5 Capacity sensitivity

### 6.6 Temporal robustness

### 6.7 Mechanism ablations

### 6.8 Oracle / information-gap analysis

### 6.9 CPU / memory / end-to-end overhead

## 7. Discussion & Limitations

- 没有 meaningful epoch 的 workload；
- hierarchy 很弱的 workload；
- immediate operation 无法 batching；
- future information 与 online 信息边界；
- generalization boundary。

## 8. Related Work

- generic cache replacement；
- adaptive / learned caching；
- hierarchical cache；
- version/batch storage systems。

## 9. Conclusion

---

# 17. 后续开发必须保持的 frozen invariants

除非有新的、预注册的机制实验，否则工程重构必须保持：

```text
1. Macro / Micro retention evidence 独立。
2. Macro 使用 semantic/control epoch clock。
3. Micro 使用 finer-grained clock，并在 epoch boundary reset。
4. intra-epoch Macro hit 不 renew Macro heat。
5. |Macro| + |Micro| <= C。
6. Macro victim 只有 realized local reuse 后才可 spill Micro。
7. boundary carry 不调用 Macro::Access，不伪造 Macro evidence。
8. Macro 保留 long ghost；Micro 无 ghost。
9. depth 只作为 bounded cold-start prior。
```

另外：

```text
speculative_micro_eligible
```

目前只是一个 optional integration hook。

Ethereum frozen parity harness 的 `p == 1` 用法必须与“通用核心算法”分开报告。

---

# 18. 编译示例

Ethereum Raw，真实 block 作为 epoch：

```bash
g++ -O3 -DNDEBUG -std=c++20 example_eth_replay.cpp -o eth_replay
./eth_replay workloads/ethereum/raw/trace_2024-01-01_0w_5w.csv 7708
```

Twitter Raw，当前冻结 control epoch `L=2C`：

```bash
g++ -O3 -DNDEBUG -std=c++20 example_twitter_replay.cpp -o twitter_replay
./twitter_replay workloads/twitter/raw/twitter_cluster52_prefix_raw.csv 6156
```

打开额外 peak resident diagnostics：

```bash
-DTT_CACHE_DIAGNOSTICS
```

默认 release hot path 不执行这些 per-request diagnostic bookkeeping。

---

# 19. 一句话总结当前算法

```text
Hierarchy 提供 cold-start prior；
Version 定义 evidence cooling / Macro clock；
Micro 捕获当前 version 内 locality；
Macro 捕获跨 version persistence；
二者共享严格总容量，但不共享 retention evidence。
```

这就是当前 v3 主线。
