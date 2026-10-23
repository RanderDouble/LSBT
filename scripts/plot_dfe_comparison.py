"""Plot aggregate and per-trace LSBT-Simple, DFE, S3FIFO, LRU, and LRFU curves."""

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd


POLICIES = {
    "LSBTSimple": ("LSBT-Simple", "#e45756", "D"),
    "DFE": ("DFE", "#4c78a8", "^"),
    "S3FIFO": ("S3FIFO (0.9C ghost)", "#9c755f", "v"),
    "LRU": ("LRU", "#54a24b", "P"),
    "LRFU": ("Canonical LRFU", "#b279a2", "X"),
    "EpochLRFU": ("Epoch-LRFU", "#eeca3b", "s"),
}


def plot(ax: plt.Axes, data: pd.DataFrame, title: str, weighted: bool) -> None:
    display = data.copy()
    if weighted:
        display["hit_ratio"] *= 100.0
    for policy, (label, color, marker) in POLICIES.items():
        rows = display[display["algorithm"] == policy].sort_values("capacity_ratio")
        if rows.empty:
            continue
        ax.plot(rows["capacity_ratio"] * 100, rows["hit_ratio"], label=label,
                color=color, marker=marker, linewidth=1.7, markersize=5.5)
    ax.set_title(title, fontsize=10)
    ax.set_xlabel("capacity ratio (% of unique keys)")
    ax.set_ylabel("hit ratio (%)")
    ax.set_xscale("log")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=8)


def save_plot(data: pd.DataFrame, title: str, stem: Path, weighted: bool) -> None:
    fig, ax = plt.subplots(figsize=(7.2, 4.8))
    plot(ax, data, title, weighted)
    fig.tight_layout()
    for extension in ("png", "pdf"):
        target = stem.with_suffix(f".{extension}")
        fig.savefig(target, dpi=220, bbox_inches="tight")
        print(f"Wrote {target}")
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True, help="weighted summary CSV")
    parser.add_argument("--raw-input", type=Path, help="per-trace CSV from the runner")
    args = parser.parse_args()
    source = args.input.resolve()
    if not source.exists():
        raise SystemExit(f"missing {source}")

    summary = pd.read_csv(source)
    save_plot(summary, "Ethereum traces: request-weighted aggregate",
              source.with_name(f"{source.stem}_hit_ratio"), weighted=True)

    if args.raw_input is None:
        return
    raw_source = args.raw_input.resolve()
    if not raw_source.exists():
        raise SystemExit(f"missing {raw_source}")
    figures = source.with_name(f"{source.stem}_figures")
    figures.mkdir(parents=True, exist_ok=True)
    raw = pd.read_csv(raw_source)
    raw["hit_ratio"] *= 100.0
    for trace, rows in raw.groupby("trace", sort=True):
        stem = figures / Path(trace).stem
        save_plot(rows, f"Ethereum workload: {Path(trace).stem}", stem, weighted=False)


if __name__ == "__main__":
    main()
