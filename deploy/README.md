# Deploy package: fetch + test + plot

自包含部署包：下载数据（以太坊 + Twitter）、转成统一 block CSV、跑 8 策略对比、出图。
不依赖本仓库的其他目录，整包拷到服务器即可运行。

## 依赖

- Linux，`g++`（C++17）、`zstd`、`python3`（≥3.10）
- Python 包：`polars`、`pandas`、`matplotlib`（`setup.sh` 会自动安装）
- 网络：AWS 公共区块链数据集（以太坊）、CMU FTP（Twitter）

## 快速开始

```bash
./setup.sh                          # 装依赖 + 编译 runner
./fetch_ethereum.sh                 # 抓取以太坊 2024 切片并转 block CSV
./fetch_twitter.sh                  # 下载 5 个 Twitter 集群并转 block CSV
./run_ablation.sh                   # 跑 8 策略对比（约 1 小时，可 --quick）
./.venv/bin/python plot.py          # 每样例一张图 + 汇总图（setup 后）
```

> 若系统 Python 是 PEP 668 外部管理（Debian/Ubuntu 常见），`setup.sh` 会自动创建
> `.venv` 虚拟环境并安装依赖；绘图请用 `./.venv/bin/python plot.py`。若虚拟环境
> 创建失败，`setup.sh` 会回退到 `pip --break-system-packages`（此时绘图用
> `python3 plot.py`）。

## 配置

每个脚本顶部有配置区：

- `fetch_ethereum.sh`：`DATES`（日期）、`WINDOWS`（窗口起点，0/5/10/15/20）、
  `ROWS_PER_WINDOW`（每窗口原始行数，默认 50000）。可传日期参数：
  `./fetch_ethereum.sh 2024-05-23 2024-09-18`。
- `fetch_twitter.sh`：`CLUSTERS`、`MAX_REQUESTS`（每集群保留请求数，默认 1000 万）、
  `CONNECTIONS`（并行下载连接数）。可传参数：
  `./fetch_twitter.sh --max-requests 5000000 12 52`。
- `run_ablation.sh`：`ETH_RATIOS` / `TW_RATIOS`（容量比）、`CASES`（策略列表）。

## 输出

- `workloads/ethereum/block/`：`lsbt_<date>_<w>w_<w+5>w.csv`（`version,depth,key`）
- `workloads/twitter/csv/`：`twitter_cluster<id>.csv`（10M 请求，`depth=1`）
- `results/expanded_ablation_full.csv`：每 (trace, 容量比, 策略) 一行
- `results/figures/`：每日期/每集群一张命中率图 + `ethereum_all.png`、`twitter_all.png`

## 说明

- 以太坊 trace 已按 (block, key) 去重（Block 表示）；Twitter 按时间戳分版本。
- 容量 `C = round(ratio × unique_keys)`，所有策略使用同一容量。
- `TwoTimescale` 适配器对 block 表示关闭 first-touch Micro 准入（与 LSBTSimple
  等价），保留版本内重复时的 Micro 路径。
- 大文件（Twitter 原始 zst/解压、CSV）留在本地，不随脚本提交。
