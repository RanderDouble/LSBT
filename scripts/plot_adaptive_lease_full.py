"""Plot full-run adaptive ghost/lease ablation against S3FIFO-8C."""

from __future__ import annotations

from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results"

POLICIES = {
    "S3FIFO-g09": ("S3FIFO (0.9C ghost)", "#9c755f", "v"),
    "S3FIFO-g8": ("S3FIFO (8C ghost)", "#4c78a8", "^"),
    "exactLSBT": ("Exact LSBT", "#79706e", "X"),
    "PRD-adaptive-ghost": ("PRD: adaptive ghost", "#f58518", "s"),
    "PRD-adaptive-lease": ("PRD: adaptive lease", "#54a24b", "D"),
    "PRD-adaptive-ghost-lease": ("PRD: adaptive ghost + lease", "#e45756", "o"),
}

FINAL_POLICIES = {
    "S3FIFO-g09": POLICIES["S3FIFO-g09"],
    "S3FIFO-g8": POLICIES["S3FIFO-g8"],
    "exactLSBT": POLICIES["exactLSBT"],
    "PRD-adaptive-ghost-lease": POLICIES["PRD-adaptive-ghost-lease"],
}


def weighted_series(data: pd.DataFrame) -> pd.DataFrame:
    grouped = data.groupby(["workload", "algorithm", "capacity_ratio"], as_index=False).agg(
        hits=("hits", "sum"), requests=("requests", "sum")
    )
    grouped["hit_ratio"] = 100 * grouped["hits"] / grouped["requests"]
    return grouped


def plot(policies: dict[str, tuple[str, str, str]], output_name: str) -> None:
    baseline = pd.read_csv(RESULTS / "evidence_fair_ablation_full.csv")
    adaptive = pd.read_csv(RESULTS / "adaptive_ghost_start8_full.csv")
    exact_lsbt = pd.concat(
        [
            pd.read_csv(RESULTS / "ghost_hot_turnover_full_raw.csv"),
            pd.read_csv(RESULTS / "ghost_hot_turnover_full_block.csv"),
        ],
        ignore_index=True,
    )
    data = pd.concat([baseline, adaptive, exact_lsbt], ignore_index=True)
    data = weighted_series(data[data["algorithm"].isin(policies)])

    fig, axes = plt.subplots(1, 2, figsize=(12, 4.8), sharex=True, constrained_layout=True)
    for axis, workload in zip(axes, ("raw", "block")):
        for algorithm, (label, color, marker) in policies.items():
            series = data[(data["workload"] == workload) & (data["algorithm"] == algorithm)]
            axis.plot(
                series["capacity_ratio"] * 100,
                series["hit_ratio"],
                label=label,
                color=color,
                marker=marker,
                markersize=6,
                linewidth=2.4,
            )
        axis.set_title(f"{workload.capitalize()} traces")
        axis.set_xlabel("Cache capacity")
        axis.set_xscale("log")
        axis.set_xticks([0.5, 1, 2, 5, 10, 20])
        axis.set_xticklabels(["0.5%", "1%", "2%", "5%", "10%", "20%"]) 
        axis.set_ylim(bottom=0)
        axis.grid(axis="y", color="#d9d9d9", linewidth=0.8)

    axes[0].set_ylabel("Request-weighted hit ratio (%)")
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, ncol=3, loc="lower center", frameon=False,
               bbox_to_anchor=(0.5, -0.16))
    fig.savefig(RESULTS / output_name, dpi=220, bbox_inches="tight")


def main() -> None:
    plot(POLICIES, "adaptive_ghost_lease_full.png")
    plot(FINAL_POLICIES, "adaptive_ghost_lease_final_four.png")


if __name__ == "__main__":
    plt.style.use("seaborn-v0_8-whitegrid")
    main()
