"""Plot request-averaged hit ratios for the expanded Ethereum and Twitter runs."""

from __future__ import annotations

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd


ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results"

POLICIES = {
    "S3FIFO-g09": ("S3FIFO (0.9C ghost)", "#9c755f", "v"),
    "S3FIFO-g8": ("S3FIFO (8C ghost)", "#4c78a8", "^"),
    "LSBT-discrete": ("LSBT discrete", "#79706e", "X"),
    "LSBT-consecutive": ("LSBT consecutive", "#f58518", "s"),
    "PRD": ("PRD", "#1b9e77", "o"),
}


def weighted(data: pd.DataFrame) -> pd.DataFrame:
    grouped = (
        data.groupby(["workload", "algorithm", "capacity_ratio"], as_index=False)
        .agg(hits=("hits", "sum"), requests=("requests", "sum"))
    )
    grouped["hit_ratio"] = 100.0 * grouped["hits"] / grouped["requests"]
    return grouped


def plot_workload(ax, data: pd.DataFrame, workload: str) -> None:
    subset = data[data["workload"] == workload]
    for algorithm, (label, color, marker) in POLICIES.items():
        rows = subset[subset["algorithm"] == algorithm].sort_values("capacity_ratio")
        if rows.empty:
            continue
        ax.plot(
            rows["capacity_ratio"] * 100,
            rows["hit_ratio"],
            marker=marker,
            color=color,
            label=label,
            linewidth=1.6,
        )
    ax.set_title("Ethereum block traces" if workload == "ethereum" else "Twitter flat traces")
    ax.set_xlabel("capacity ratio (% of unique keys)")
    ax.set_ylabel("hit ratio (%)")
    ax.set_xscale("log")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=8)


def main() -> None:
    source = RESULTS / "expanded_ablation_full.csv"
    if not source.exists():
        source = RESULTS / "expanded_ablation_quick.csv"
    data = pd.read_csv(source)
    data = weighted(data)

    fig, axes = plt.subplots(1, 2, figsize=(13, 5))
    plot_workload(axes[0], data, "ethereum")
    plot_workload(axes[1], data, "twitter")
    fig.suptitle("Expanded dataset: request-weighted hit ratio by policy")
    fig.tight_layout()

    out = RESULTS / "expanded_hit_ratio.png"
    fig.savefig(out, dpi=220, bbox_inches="tight")
    print(f"Wrote {out}")


if __name__ == "__main__":
    main()
