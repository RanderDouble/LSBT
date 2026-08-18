#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path

import polars as pl


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Convert raw Ethereum trace CSV into LSBT access sequence CSV."
    )
    parser.add_argument("--input", required=True, help="Input raw trace CSV path.")
    parser.add_argument("--output", required=True, help="Output LSBT CSV path.")
    parser.add_argument(
        "--max-blocks",
        type=int,
        default=None,
        help="Optional limit on the number of distinct blocks to keep.",
    )
    return parser


def addr_to_prefixes(addr: str) -> list[tuple[str, int]]:
    if not addr or addr == "0x":
        return []
    hex_part = addr[2:].lower()
    prefixes: list[tuple[str, int]] = []
    for i in range(2, len(hex_part) + 1, 2):
        prefixes.append((hex_part[:i], i // 2))
    return prefixes


def generate_lsbt_sequence(
    input_path: Path, output_path: Path, max_blocks: int | None = None
) -> None:
    frame = pl.read_csv(input_path)

    if max_blocks is not None:
        unique_blocks = frame["block_number"].unique().sort().head(max_blocks).to_list()
        frame = frame.filter(pl.col("block_number").is_in(unique_blocks))

    block_addresses: dict[int, set[str]] = defaultdict(set)
    for row in frame.iter_rows(named=True):
        block_number = row["block_number"]
        from_address = row["from_address"]
        to_address = row["to_address"]
        if from_address:
            block_addresses[block_number].add(from_address)
        if to_address:
            block_addresses[block_number].add(to_address)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    total_rows = 0
    with output_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["block_number", "keylen", "key"])

        for block_number in sorted(block_addresses):
            prefixes: set[tuple[int, str]] = set()
            for addr in block_addresses[block_number]:
                for key, keylen in addr_to_prefixes(addr):
                    prefixes.add((keylen, key))
            for keylen, key in sorted(prefixes, key=lambda item: (-item[0], item[1])):
                writer.writerow([block_number, keylen, key])
                total_rows += 1

    print(f"input={input_path}")
    print(f"output={output_path}")
    print(f"blocks={len(block_addresses)}")
    print(f"lsbt_rows={total_rows}")


def main() -> None:
    args = build_parser().parse_args()
    generate_lsbt_sequence(
        input_path=Path(args.input),
        output_path=Path(args.output),
        max_blocks=args.max_blocks,
    )


if __name__ == "__main__":
    main()
