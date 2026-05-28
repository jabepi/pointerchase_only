## Pointer-Chase Only Benchmark

This folder contains a standalone pointer-chase benchmark with no STREAM logic.

### Build

```bash
make
```

Binary:

- `pointer_chase_only.x`

### Run

```bash
./pointer_chase_only.x -c 819200 -x 320000 -n 5 -w /home/gem5/mess_omp_versions/array.dat -P 10000 -m
```

### CLI options

- `-c <chase_elems>`: pointer-chase nodes (cache-line nodes), must be `> 0`
- `-x <chase_total_loads>`: dependent loads per kernel call, must be `> 0`
- `-n <iterations>`: number of kernel calls, must be `> 0`
- `-w <walk_file>`: pointer walk file to load/save
- `-P <period_ticks>`: periodic dump period for gem5 stats when `-m` is used
- `-m`: enable gem5 m5 calls
- `-d`: enable debug logs
- `-h`: print usage

### Notes

- On AArch64, the pointer-chase kernel uses inline assembly (`ldr` dependency chain).
- On non-AArch64, a C fallback loop is used.
- Output is a single summary line:
  - `Pointer-chase-only: sink=... total_cycles=... total_loads=... avg_latency_cycles=... avg_latency_ns=... total_time_ns=...`
