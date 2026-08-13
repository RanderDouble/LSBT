#!/usr/bin/env python3
"""Parallel range downloader for slow HTTP mirrors that support Range.

Resumable: ranges whose bytes are already present (non-zero) are skipped when
the output file already exists with the expected size.
"""

from __future__ import annotations

import argparse
import os
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


def fetch_range(
    url: str,
    start: int,
    end: int,
    dest: str,
    lock: threading.Lock,
    progress: dict,
    progress_lock: threading.Lock,
    retries: int = 5,
) -> int:
    size = end - start + 1
    for attempt in range(retries):
        try:
            request = Request(url, headers={"Range": f"bytes={start}-{end}"})
            with urlopen(request, timeout=60) as response:
                data = response.read()
            if len(data) != size:
                raise ValueError(f"short range {start}-{end}: got {len(data)}")
            with lock:
                with open(dest, "r+b") as handle:
                    handle.seek(start)
                    handle.write(data)
            with progress_lock:
                progress["done"] += 1
                progress["bytes"] += size
                print(
                    f"\rprogress: {progress['done']}/{progress['total']} ranges, "
                    f"{progress['bytes']/1e6:.1f} MB",
                    end="",
                    flush=True,
                )
            return size
        except (HTTPError, URLError, ValueError, OSError) as error:
            if attempt == retries - 1:
                with progress_lock:
                    print(f"\nrange {start}-{end} failed after {retries} attempts: {error}")
                raise
            time.sleep(2 * (attempt + 1))
    return 0


def is_missing(dest: str, start: int, end: int) -> bool:
    """A range counts as missing when every byte in it is still zero."""
    size = end - start + 1
    with open(dest, "rb") as handle:
        handle.seek(start)
        data = handle.read(size)
    return not any(data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--size", type=int, required=True, help="total bytes")
    parser.add_argument("--connections", type=int, default=64)
    parser.add_argument("--chunk-mb", type=float, default=0.25)
    args = parser.parse_args()

    chunk = int(args.chunk_mb * 1024 * 1024)
    ranges: list[tuple[int, int]] = []
    for start in range(0, args.size, chunk):
        ranges.append((start, min(start + chunk - 1, args.size - 1)))

    resume = os.path.exists(args.output) and os.path.getsize(args.output) == args.size
    if not resume:
        with open(args.output, "wb") as handle:
            handle.truncate(args.size)
    else:
        print("resuming existing file, scanning for missing ranges...")
        missing = []
        for start, end in ranges:
            if is_missing(args.output, start, end):
                missing.append((start, end))
        ranges = missing
        print(f"{len(ranges)} ranges still missing")

    lock = threading.Lock()
    progress_lock = threading.Lock()
    progress = {"done": 0, "bytes": 0, "total": len(ranges)}
    if not ranges:
        print("nothing to download")
        return 0
    print(f"downloading {len(ranges)} ranges")
    with ThreadPoolExecutor(max_workers=args.connections) as pool:
        futures = [
            pool.submit(
                fetch_range,
                args.url,
                start,
                end,
                args.output,
                lock,
                progress,
                progress_lock,
            )
            for start, end in ranges
        ]
        for future in futures:
            future.result()

    print()
    print(f"downloaded {os.path.getsize(args.output)} bytes -> {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
