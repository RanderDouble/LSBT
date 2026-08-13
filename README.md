# PRD Cache Replacement

## External libCacheSim validation

The byte-aware native `prd` implementation is in
`libCacheSim/libCacheSim/cache/eviction/PRD.cpp`. It supports generic cache
traces with depth prior disabled by default, and optional depth buckets through
`depth-feature=<index>`. The first Twitter/Twemcache boundary experiment and
the exact commands are documented in
`results/libcachesim_twitter_prd.md`; its recorded results are in
`results/libcachesim_twitter_cluster52_prd.csv`.

From PowerShell, after building the WSL/Linux libCacheSim tree, run:

```powershell
.\scripts\run_twitter_libcachesim_prd.ps1
```

The same libCacheSim binary can replay the repository's Raw and LSBT Ethereum
workloads after deterministic path/key conversion. See
`results/libcachesim_hierarchy_pilot.md` and
`scripts/run_libcachesim_hierarchy.ps1`. This current integration is a
byte-capacity adapter: `prd-epoch` directly uses the current `PRD.hpp` state
machine and preserves its per-transaction/per-block epochs. It is distinct
from the generic byte-turnover prototype registered as `prd`.

本目录包含最终 PRD 主算法、LSBT/S3FIFO 对照、消融实验和绘图脚本。

## 文件结构

### 主算法

- `code/PRD.hpp`：唯一主算法配置。它固定使用 8C 初始 ghost、8C 初始 Hot lease、Cold bucket、Hot turnover FIFO，以及 ghost + lease 双自适应。
- `code/runner.cpp`：精简 PRD runner，只运行 PRD 并输出一行 CSV。
- `scripts/build.ps1`：编译 `runner.exe`。

运行 Block/LSBT trace：

```powershell
cd C:\Users\ROG\Desktop\CacheGhost\lrd_cache\LSBTreplacement
.\scripts\build.ps1
.\runner.exe --trace .\workload\lsbt_ethereum_2024\lsbt_2024-01-01_0w_5w.csv --capacity-ratio 0.01
```

输入 CSV 约定为 `version,depth,key`；输出字段包括 `requests,hits,misses,hit_ratio,elapsed_ms`。

### 论文对照和消融

- `code/ablation_runner.cpp`：Block trace 的论文实验入口。
- `code/ablation_path_runner.cpp`：Raw/transaction path trace 的论文实验入口。
- `code/LSBTdiscrete.hpp`：Exact/discrete LSBT 对照。
- `code/S3FifoBaseline.hpp`：标准 S3FIFO、8C ghost、S3 自适应控制器和 Main-evict 审计。
- `code/PRDAblations.hpp`：PRD 的论文消融配置；主算法不依赖它。
- `scripts/build_ablation.ps1`：编译两个消融 runner 并运行 self-test。

保留的 LSBT 对照只有 `LSBTdiscrete.hpp` 与 `LSBTconsecutive.hpp`；
S3FIFO 对照和 Main-evict 审计均在 `S3FifoBaseline.hpp`。其他历史 LSBT
候选实现已经删除，不再作为实验依赖。

编译消融代码：

```powershell
.\scripts\build_ablation.ps1
```

完整 Raw/Block 主结果和消融：

```powershell
.\scripts\run_evidence_fair_ablation_full.ps1
```

只跑较小的快速检查：

```powershell
.\scripts\run_evidence_fair_ablation_full.ps1 -Quick
```

专门运行 PRD ghost/lease 消融：

```powershell
.\scripts\run_evidence_fair_ablation_full.ps1 -AdaptiveOnly
```

专门运行 S3FIFO 控制器反事实对照：

```powershell
.\scripts\run_evidence_fair_ablation_full.ps1 -S3AdaptiveOnly
```

专门运行 Cold bucket、null-depth、pure-depth、Cold FIFO 消融：

```powershell
.\scripts\run_evidence_fair_ablation_full.ps1 -ColdAblationOnly
```

Main-evict-reaccess 审计：

```powershell
.\scripts\run_s3_main_cycle_audit.ps1
```

### 绘图

- `scripts/plot_adaptive_lease_full.py`：绘制 S3FIFO、Exact LSBT、PRD 三类结果及四算法简图。
- `scripts/plot_prd_lsbt_s3_full.py`：绘制 Raw/Block 的容量曲线。

示例：

```powershell
python .\scripts\plot_adaptive_lease_full.py
python .\scripts\plot_prd_lsbt_s3_full.py
```

绘图脚本读取 `results/*.csv`，图片也写入 `results/`。

## 结果与文档

- `results/prd_research_summary.md`：当前算法、结果、消融和研究结论总览。
- `results/future_work_plan.md`：后续实验和论文工作计划。
- `results/evidence_fair_ablation_full.csv`：固定基线与 PRD 消融全量结果。
- `results/adaptive_ghost_start8_full.csv`：PRD ghost-only、lease-only、dual-adaptive 结果。
- `results/s3_adaptive_controls_full.csv`：S3FIFO 自适应控制器反事实结果。
- `results/adaptive_cold_ablation_full.csv`：Cold 层消融结果。
- `results/s3_main_cycle_audit.csv` / `s3_main_cycle_audit.md`：S3FIFO Main eviction 循环审计。
- `results/adaptive_ghost_lease_full.png`：六算法容量曲线图。
- `results/adaptive_ghost_lease_final_four.png`：四算法简图。

主线代码与论文复现代码已经分开：生产使用 `PRD.hpp + runner.cpp`；论文实验使用 `ablation_*` runner。历史候选算法不应作为新的主结果报告。
