# Twitter (OSDI'20) traces

The S3FIFO paper evaluates on the Twitter in-memory cache traces collected by
Yang et al. (OSDI'20). This directory keeps the local copies used by the
expanded experiments.

Source:

```text
https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/twitter/sample100/
```

Each file is a zstd-compressed stream of packed 24-byte records:

```text
uint32 timestamp; uint64 obj_id; uint32 obj_size; int64 next_access_vtime;
```

Workflow:

```bash
./scripts/fetch_twitter.sh            # downloads sample100 clusters 12 24 47 48 52
./scripts/prepare_twitter.sh          # decompress + convert to block CSV
./scripts/run_expanded_ablation.sh    # run policies, collect hit ratios
```

The conversion keeps the first 10 million requests per cluster by default
(`prepare_twitter.sh --max-requests N`), emits `version,depth,key` rows with
`depth=1` (flat object workload), and uses a monotonic logical version that
increments whenever the raw timestamp changes.

Raw archives and converted CSVs are large and gitignored; only this README,
the fetch/prepare scripts, and the result summaries are version-controlled.
