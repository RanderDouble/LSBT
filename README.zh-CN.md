# PRD 缓存替换

> English: [README.md](README.md)

本仓库包含 PRD 缓存策略、LSBT 基线、S3FIFO 基线，以及用于在基于以太坊导出的带版本 trace 上进行受控对比的各个 runner。

PRD 依据复用证据（reuse evidence）对条目进行分类。新条目通过 Cold 路径进入缓存（可选地按树深度先验排序）；一旦观测到常驻（resident）或幽灵（ghost）复用，即证明该条目的价值并将其提升为 Hot。Hot 条目持有有界的轮换租约（turnover lease），未续约的证据会变得可被淘汰。实现默认使用 8C 幽灵目录，并可独立自适应调整幽灵目录与租约。这里的 `C` 始终表示常驻缓存容量；它不是版本号或 epoch 长度。

## 仓库结构

| 路径 | 用途 |
| --- | --- |
| `code/PRD.hpp` | 仅包含主 PRD 实现。 |
| `code/runner.cpp` | 主 PRD 策略的最小 runner。 |
| `code/LSBTdiscrete.hpp` | 分桶/离散版 LSBT 基线。 |
| `code/LSBTconsecutive.hpp` | 连续版 LSBT 基线。 |
| `code/S3FifoBaseline.hpp` | S3FIFO 基线及受控扩展/审计钩子。 |
| `code/PRDAblations.hpp` | 仅用于消融实验的具名 PRD 配置。 |
| `code/ablation_runner.cpp` | Block trace 对比 runner。 |
| `code/ablation_path_runner.cpp` | 原始交易路径对比 runner。 |
| `code/lease_sweep_runner.cpp` | 租约敏感性 runner。 |
| `code/s3_audit_runner.cpp` | S3FIFO Main 淘汰/重访审计 runner。 |
| `code/synthetic_lease_runner.cpp` | 合成租约压力测试。 |
| `scripts/` | 构建、扫描、审计和绘图脚本（PowerShell 与 bash 两个版本）。 |
| `tools/hierarchy_trace_convert.cpp` | 将 Raw/LSBT/Twitter/CloudPhysics CSV 输入转换为固定 64 字节逻辑节点 CSV。 |
| `workload/` | 本地小规模 workload 目录，供 block 脚本使用；trace 数据不随仓库捆绑。 |
| `results/README.md` | 结果目录约定。生成的输出文件按 `.gitignore` 有意不入库。 |

只有源代码和脚本纳入版本控制。trace 数据、生成的 CSV/图片以及可执行文件都是本地产物，已被 `.gitignore` 排除。

## 环境要求

- Windows PowerShell 5+。
- 提供 `g++` 的 C++17 编译器（原生 runner 可用 MinGW-w64）。
- Python 3，绘图脚本需要 `pandas` 和 `matplotlib`。

默认构建脚本会将可执行文件输出到仓库根目录。

在 Linux/macOS 上，每个 PowerShell 脚本都有对应的 bash 翻译版本（`scripts/*.sh`）。shell 脚本需要 bash 4+，使用同样的 `g++` 工具链，并构建免扩展名的可执行文件到仓库根目录（`runner`、`ablation_runner`、……）。

## Shell 用法（Linux/macOS）

每个 PowerShell 脚本都有同名的 bash 版本。参数使用 `--` 而不是 `-`，二进制文件不带 `.exe` 后缀：

```bash
./scripts/build.sh                    # release；或 build.sh debug
./scripts/build_ablation.sh           # 构建两个 ablation runner 并运行自测
./scripts/build_s3_audit.sh

./runner --trace workload/lsbt_ethereum_2024/lsbt_2024-01-01_0w_5w.csv --capacity-ratio 0.01
./runner --trace workload/lsbt_ethereum_2024/lsbt_2024-01-01_0w_5w.csv --cache-size 10000

./scripts/run_evidence_fair_ablation_full.sh --quick
./scripts/run_evidence_fair_ablation_full.sh --adaptive-only
./scripts/run_evidence_fair_ablation_full.sh --s3-adaptive-only
./scripts/run_evidence_fair_ablation_full.sh --cold-ablation-only

./scripts/run_experiments.sh --workload-dir DIR --ratios 0.01,0.05,0.10,0.20 --output results/raw.csv
./scripts/run_lease_sweep.sh [TRACE.csv]
./scripts/run_granularity_sweep.sh [TRACE.csv]
./scripts/run_s3_main_cycle_audit.sh [TRACE.csv]
./scripts/run_s3_turnover_audit_full.sh --quick
./scripts/summarize.sh --input results/raw.csv --output results/summary.csv

python scripts/plot_prd_lsbt_s3_full.py
python scripts/plot_adaptive_lease_full.py
```

绘图脚本是 Python 写的，无需修改即可运行。

注意事项：

- shell 脚本与 PowerShell 版本依赖相同的外部目录布局：原始以太坊 trace 位于本仓库向上两级目录的相邻位置（`run_evidence_fair_ablation_full.sh` 和 `run_s3_turnover_audit_full.sh` 使用），单独构建的 libCacheSim 树位于向上三级目录（libcachesim 脚本使用）。
- `run_experiments.sh` 直接调用当前的 `runner` 接口。PowerShell 原版传入了 `--algorithm both`，而当前 `runner.cpp` 已不再接受该参数，因此 shell 版本省略了它。

## Trace 格式

原生 runner 消费带表头的 CSV，记录格式如下：

```text
version,depth,key
```

- `version` 定义逻辑 epoch。当该值变化时，runner 调用一次 `FinishBlock()`。
- `depth` 是正的树深度。PRD 的 Cold 先验和 LSBT 都会用到。
- `key` 是任意节点标识符。

`runner.cpp` 和 `ablation_runner.cpp` 直接使用该格式。Raw path runner 使用交易行，并在内部重建每个以太坊地址的 20 个前缀节点。

## 运行主 PRD

构建最小 runner：

```powershell
cd C:\Users\ROG\Desktop\CacheGhost\lrd_cache\LSBTreplacement
.\scripts\build.ps1
```

按容量比例运行 block/LSBT CSV：

```powershell
.\runner.exe --trace .\workload\lsbt_ethereum_2024\trace.csv --capacity-ratio 0.01
```

或直接指定常驻容量：

```powershell
.\runner.exe --trace .\workload\lsbt_ethereum_2024\trace.csv --cache-size 10000
```

输出为一行 CSV，包含请求数、命中、未命中、命中率和计时统计。可选参数为 `--max-depth` 和 `--history-retention`。

## 对比与消融实验

构建两个对比 runner 并运行自测：

```powershell
.\scripts\build_ablation.ps1
```

可用对比策略包括 `PRD`、`LSBTdiscrete`、`LSBTconsecutive`、`S3FIFO` 和 `S3FIFOGhost8`。runner 还暴露了受控的 PRD 与 S3FIFO 变体，用于 ghost、租约和 Cold 路径消融；这些是实验对照，不是独立的算法。

运行完整的 Raw/Block 扫描（脚本期望本仓库相邻位置存在本地 trace 目录）：

```powershell
.\scripts\run_evidence_fair_ablation_full.ps1
```

常用的更小规模模式：

```powershell
.\scripts\run_evidence_fair_ablation_full.ps1 -Quick
.\scripts\run_evidence_fair_ablation_full.ps1 -AdaptiveOnly
.\scripts\run_evidence_fair_ablation_full.ps1 -S3AdaptiveOnly
.\scripts\run_evidence_fair_ablation_full.ps1 -ColdAblationOnly
```

其他专项实验：

```powershell
.\scripts\run_lease_sweep.ps1
.\scripts\run_granularity_sweep.ps1
.\scripts\build_s3_audit.ps1
.\scripts\run_s3_main_cycle_audit.ps1
.\scripts\run_s3_turnover_audit_full.ps1
```

## 绘图

绘图脚本读取 `results/` 下本地生成的 CSV 文件并将图写入该目录：

```powershell
python .\scripts\plot_prd_lsbt_s3_full.py
python .\scripts\plot_adaptive_lease_full.py
```

## 可选的 libCacheSim 适配器

`tools/hierarchy_trace_convert.cpp` 将受支持的源 trace 转换为外部 libCacheSim 适配器使用的 CSV 形式。用 C++20 编译，例如在 WSL 中：

```bash
g++ -std=c++20 -O3 tools/hierarchy_trace_convert.cpp -o tools/hierarchy_trace_convert
```

脚本 `scripts/run_libcachesim_hierarchy.ps1` 假定存在一个单独构建、本地打过补丁、提供 `prd-epoch` 策略的 libCacheSim 树。该适配器和 libCacheSim 构建目录有意不纳入本仓库。`scripts/run_twitter_libcachesim_prd.ps1` 有同样的外部依赖。

## 可复现性说明

- 缓存容量 `C`、幽灵容量（例如 `8C`）、轮换租约和逻辑 epoch 是不同的量。
- 幽灵命中是带保留元数据的缓存**未命中**；它永远不会被计为常驻缓存命中。
- 生成的结果不会提交。报告任何结果时，请同时保留所用命令、trace 来源、容量定义和 epoch 定义。
