"""Plot request-weighted full-run hit ratios for PRD, Exact LSBT, and S3FIFO."""

from __future__ import annotations

import csv
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt


ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results"

POLICIES = {
    "Turnover8": ("PRD (Turnover-8C)", "#1b9e77", "o"),
    "exactLSBT": ("Exact LSBT", "#d95f02", "s"),
    "S3FIFO": ("S3FIFO", "#386cb0", "^"),
}


def read_per_trace_ratios(path: Path) -> dict[float, dict[str, dict[str, float]]]:
    values: dict[float, dict[str, dict[str, float]]] = defaultdict(
        lambda: defaultdict(dict)
    )
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            algorithm = row["algorithm"]
            if algorithm not in POLICIES:
                continue
            capacity = float(row["capacity_ratio"])
            values[capacity][row["trace"]][algorithm] = 100.0 * float(row["hit_ratio"])
    return values


def capacity_label(value: float) -> str:
    percentage = value * 100
    return f"{percentage:g}%"


def plot(granularity: str) -> None:
    source = RESULTS / f"ghost_hot_turnover_full_{granularity}.csv"
    ratios = read_per_trace_ratios(source)
    capacities = sorted(ratios)
    traces = sorted(next(iter(ratios.values())))
    trace_labels = [trace.removeprefix("trace_").removesuffix(".csv") for trace in traces]

    fig, axes = plt.subplots(2, 3, figsize=(17, 8), sharex=True, sharey=True,
                             constrained_layout=True)
    for ax, capacity in zip(axes.flat, capacities):
        for algorithm, (label, color, marker) in POLICIES.items():
            ax.plot(
                range(len(traces)),
                [ratios[capacity][trace][algorithm] for trace in traces],
                label=label,
                color=color,
                marker=marker,
                markersize=3.8,
                linewidth=1.5,
            )
        ax.set_title(f"Capacity {capacity_label(capacity)}")
        ax.set_ylim(bottom=0)
        ax.grid(axis="y", color="#d9d9d9", linewidth=0.7)
        ax.set_xticks(range(len(traces)))
        ax.set_xticklabels(trace_labels, rotation=65, ha="right", fontsize=6.8)

    fig.suptitle(f"{granularity.capitalize()} traces: per-trace hit ratio at each cache capacity",
                 fontsize=16)
    fig.supxlabel("Trace")
    fig.supylabel("Hit ratio (%)")
    handles, labels = axes.flat[0].get_legend_handles_labels()
    fig.legend(handles, labels, frameon=False, loc="upper center", ncol=3,
               bbox_to_anchor=(0.5, 0.965))
    fig.savefig(RESULTS / f"prd_lsbt_s3_{granularity}_full.png", dpi=220)
    plt.close(fig)


def plot_selected_trace() -> None:
    selections = {
        "raw": "trace_2024-01-01_0w_5w.csv",
        "block": "lsbt_2024-01-01_0w_5w.csv",
    }
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.8), sharex=True, constrained_layout=True)

    for ax, (granularity, trace) in zip(axes, selections.items()):
        values = read_per_trace_ratios(RESULTS / f"ghost_hot_turnover_full_{granularity}.csv")
        for algorithm, (label, color, marker) in POLICIES.items():
            series = sorted(
                (capacity, per_trace[trace][algorithm])
                for capacity, per_trace in values.items()
            )
            ax.plot(
                [capacity * 100 for capacity, _ in series],
                [ratio for _, ratio in series],
                label=label,
                color=color,
                marker=marker,
                markersize=6,
                linewidth=2.2,
            )
        ax.set_title(f"{granularity.capitalize()}: 2024-01-01, 0w-5w")
        ax.set_xlabel("Cache capacity")
        ax.set_ylabel("Hit ratio (%)")
        ax.set_xscale("log")
        ax.set_xticks([0.5, 1, 2, 5, 10, 20])
        ax.set_xticklabels(["0.5%", "1%", "2%", "5%", "10%", "20%"]) 
        ax.set_ylim(bottom=0)
        ax.grid(axis="y", color="#d9d9d9", linewidth=0.8)

    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, frameon=False, loc="upper center", ncol=3,
               bbox_to_anchor=(0.5, 1.04))
    fig.savefig(RESULTS / "prd_lsbt_s3_selected_trace_capacity.png", dpi=220,
                bbox_inches="tight")
    plt.close(fig)


if __name__ == "__main__":
    plt.style.use("seaborn-v0_8-whitegrid")
    plot("raw")
    plot("block")
    plot_selected_trace()
