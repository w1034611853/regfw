# Formula backend benchmark

Compiler: g++-16 (Homebrew GCC 16.2.0) 16.2.0

## Compile (median of 3, compiler peak RSS)

| case | wall s | peak RSS MiB | object |
|---|---|---|---|
| dag_formula | 1.54 | 357 | 99 KiB |
| poe_formula | 14.77 | 1649 | 938 KiB |
| scale_formula | 1.3 | 318 | 78 KiB |

## Init (DAG, median ns)

| backend | construct | finish | total wall | peak RSS MiB |
|---|---|---|---|---|
| init_formula | 752750 | 30334 | 784000 | 10 |

## MMIO hot path (median ns/access)

| workload | dynamic | formula |
|---|---|---|
| 4 mixed30 callbacks hot read regfw | 51.9 | 51.1 |
| 4 mixed30 callbacks hot read switch_normalized | 5.1 | 5.1 |
| 4 mixed30 callbacks hot read switch_raw | 6.0 | 5.9 |
| 4 mixed30 callbacks hot write regfw | 48.0 | 48.1 |
| 4 mixed30 callbacks hot write switch_normalized | 5.1 | 5.0 |
| 4 mixed30 callbacks hot write switch_raw | 5.0 | 5.2 |
| 4 mixed30 callbacks mapped read regfw | 105.9 | 100.8 |
| 4 mixed30 callbacks mapped read switch_normalized | 24.7 | 24.4 |
| 4 mixed30 callbacks mapped read switch_raw | 24.6 | 24.4 |
| 4 mixed30 callbacks mapped write regfw | 99.2 | 99.3 |
| 4 mixed30 callbacks mapped write switch_normalized | 23.6 | 23.3 |
| 4 mixed30 callbacks mapped write switch_raw | 23.4 | 23.7 |
| 4 mixed30 callbacks random read regfw | 65.6 | 64.6 |
| 4 mixed30 callbacks random read switch_normalized | 26.9 | 25.4 |
| 4 mixed30 callbacks random read switch_raw | 25.7 | 26.3 |
| 4 mixed30 callbacks random write regfw | 58.2 | 58.1 |
| 4 mixed30 callbacks random write switch_normalized | 26.4 | 25.8 |
| 4 mixed30 callbacks random write switch_raw | 26.9 | 25.7 |
| 4 mixed30 rw hot read regfw | 28.4 | 28.1 |
| 4 mixed30 rw hot read switch_normalized | 4.5 | 4.6 |
| 4 mixed30 rw hot read switch_raw | 4.6 | 4.7 |
| 4 mixed30 rw hot write regfw | 28.6 | 28.8 |
| 4 mixed30 rw hot write switch_normalized | 5.0 | 4.7 |
| 4 mixed30 rw hot write switch_raw | 4.7 | 5.1 |
| 4 mixed30 rw mapped read regfw | 77.8 | 77.8 |
| 4 mixed30 rw mapped read switch_normalized | 24.3 | 23.5 |
| 4 mixed30 rw mapped read switch_raw | 24.1 | 24.3 |
| 4 mixed30 rw mapped write regfw | 77.1 | 75.2 |
| 4 mixed30 rw mapped write switch_normalized | 22.9 | 23.2 |
| 4 mixed30 rw mapped write switch_raw | 22.9 | 22.7 |
| 4 mixed30 rw random read regfw | 33.3 | 35.6 |
| 4 mixed30 rw random read switch_normalized | 24.8 | 24.8 |
| 4 mixed30 rw random read switch_raw | 24.9 | 25.2 |
| 4 mixed30 rw random write regfw | 32.3 | 32.2 |
| 4 mixed30 rw random write switch_normalized | 25.5 | 25.7 |
| 4 mixed30 rw random write switch_raw | 25.3 | 24.6 |

## Scale (8 VF, 262144 mapped u512 words)

```
RESULT construct=0.001037 finish=0.000020 maxrss_kb=10371072 scalar32_w_tps=104612190 scalar32_r_tps=165235257 identity_w_tps=19180917 sink=1099477549056
```
