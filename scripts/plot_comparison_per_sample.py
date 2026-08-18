"""Per-sample hit-ratio comparison figures.

Reads results/expanded_ablation_full.csv and writes one figure per sample:
- results/figures/ethereum_<date>.png for each Ethereum date (slices of that
  date are request-weighted together);
- results/figures/twitter_cluster<id>.png for each Twitter cluster;
- results/figures/ethereum_all.png and results/figures/twitter_all.png for the
  request-weighted aggregate of each workload.
"""

from __future__ import annotations

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd


ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results"
FIGURES = RESULTS / "figures"

POLICIES = {
    "PRD": ("PRD", "#1b9e77", "o"),
    "LSBTSimple": ("LSBT-Simple", "#e45756", "D"),
    "LSBT-discrete": ("LSBT discrete", "#79706e", "X"),
    "LSBT-consecutive": ("LSBT consecutive", "#f58518", "s"),
    "S3FIFO-g09": ("S3FIFO (0.9C ghost)", "#9c755f", "v"),
    "S3FIFO-g8": ("S3FIFO (8C ghost)", "#4c78a8", "^"),
    "LRU": ("LRU", "#54a24b", "P"),
}


def weighted(data: pd.DataFrame) -> pd.DataFrame:
    grouped = (
        data.groupby(["sample", "algorithm", "capacity_ratio"], as_index=False)
        .agg(hits=("hits", "sum"), requests=("requests", "sum"))
    )
    grouped["hit_ratio"] = 100.0 * grouped["hits"] / grouped["requests"]
    return grouped


def plot_panel(ax, rows: pd.DataFrame, title: str) -> None:
    for algorithm, (label, color, marker) in POLICIES.items():
        cur = rows[rows["algorithm"] == algorithm].sort_values("capacity_ratio")
        if cur.empty:
            continue
        ax.plot(
            cur["capacity_ratio"] * 100,
            cur["hit_ratio"],
            marker=marker,
            color=color,
            label=label,
            linewidth=1.6,
            markersize=5,
        )
    ax.set_title(title, fontsize=10)
    ax.set_xlabel("capacity ratio (% of unique keys)")
    ax.set_ylabel("hit ratio (%)")
    ax.set_xscale("log")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=7, loc="best")


def main() -> None:
    source = RESULTS / "expanded_ablation_full.csv"
    if not source.exists():
        raise SystemExit(f"missing {source}")
    data = pd.read_csv(source)

    ethereum = data[data["workload"] == "ethereum"].copy()
    twitter = data[data["workload"] == "twitter"].copy()

    ethereum["sample"] = ethereum["trace"].str.extract(r"lsbt_(\d{4}-\d{2}-\d{2})")[0]
    twitter["sample"] = twitter["trace"].str.extract(r"twitter_cluster(\d+)")[0]
    ethereum = weighted(ethereum)
    twitter = weighted(twitter)

    FIGURES.mkdir(parents=True, exist_ok=True)

    for date, rows in ethereum.groupby("sample"):
        fig, ax = plt.subplots(figsize=(7, 5))
        plot_panel(ax, rows, f"Ethereum {date} (block traces)")
        out = FIGURES / f"ethereum_{date}.png"
        fig.tight_layout()
        fig.savefig(out, dpi=200, bbox_inches="tight")
        plt.close(fig)
        print(f"Wrote {out}")

    for cluster, rows in twitter.groupby("sample"):
        fig, ax = plt.subplots(figsize=(7, 5))
        plot_panel(ax, rows, f"Twitter cluster {cluster} (flat traces)")
        out = FIGURES / f"twitter_cluster{cluster}.png"
        fig.tight_layout()
        fig.savefig(out, dpi=200, bbox_inches="tight")
        plt.close(fig)
        print(f"Wrote {out}")

    for workload, rows, label in (
        ("ethereum", ethereum, "Ethereum block traces (all dates)"),
        ("twitter", twitter, "Twitter flat traces (all clusters)"),
    ):
        fig, ax = plt.subplots(figsize=(7, 5))
        plot_panel(ax, rows, label)
        out = FIGURES / f"{workload}_all.png"
        fig.tight_layout()
        fig.savefig(out, dpi=200, bbox_inches="tight")
        plt.close(fig)
        print(f"Wrote {out}")


if __name__ == "__main__":
    main()
