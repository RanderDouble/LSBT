# LSBT Ethereum 2024 workload

This directory contains the 27 LSBT Ethereum trace slices used by the
experiment. Each file has the schema:

```csv
block_number,keylen,key
18915926,20,0000000000000000000000000000000000000018
```

The original runner interprets the second column (`keylen`) as LSBT depth;
the reduced runner preserves that behavior exactly. The files here are NTFS
hard links to `lrd_cache/workload/lsbt_ethereum_2024`, so they are complete
byte-identical inputs without a second 409 MB allocation. Copying or archiving
the artifact materializes ordinary files.

`manifest.csv` records size and SHA-256 for each trace.

