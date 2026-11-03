# Formula backend benchmark

Compiler: Apple clang version 21.0.0 (clang-2100.1.1.101)

## Compile (median of 3, compiler peak RSS)

| case | wall s | peak RSS MiB | object |
|---|---|---|---|
| dag_formula | 1.2 | 224 | 96 KiB |
| poe_formula | 8.68 | 623 | 918 KiB |
| scale_formula | 0.98 | 216 | 76 KiB |

## Init (DAG, median ns)

| backend | construct | finish | total wall | peak RSS MiB |
|---|---|---|---|---|
| init_formula | 901167 | 46792 | 952834 | 9 |

## MMIO hot path (median ns/access)

| workload | dynamic | formula |
|---|---|---|
| 4 mixed30 callbacks hot read regfw | 77.0 | 71.4 |
| 4 mixed30 callbacks hot read switch_normalized | 3.6 | 3.4 |
| 4 mixed30 callbacks hot read switch_raw | 4.0 | 3.4 |
| 4 mixed30 callbacks hot write regfw | 78.5 | 74.0 |
| 4 mixed30 callbacks hot write switch_normalized | 3.5 | 3.6 |
| 4 mixed30 callbacks hot write switch_raw | 3.6 | 3.9 |
| 4 mixed30 callbacks mapped read regfw | 143.2 | 142.6 |
| 4 mixed30 callbacks mapped read switch_normalized | 11.8 | 11.6 |
| 4 mixed30 callbacks mapped read switch_raw | 11.6 | 12.2 |
| 4 mixed30 callbacks mapped write regfw | 142.2 | 139.1 |
| 4 mixed30 callbacks mapped write switch_normalized | 14.1 | 14.0 |
| 4 mixed30 callbacks mapped write switch_raw | 14.1 | 14.4 |
| 4 mixed30 callbacks random read regfw | 92.1 | 85.3 |
| 4 mixed30 callbacks random read switch_normalized | 12.4 | 11.9 |
| 4 mixed30 callbacks random read switch_raw | 11.4 | 12.0 |
| 4 mixed30 callbacks random write regfw | 89.3 | 81.2 |
| 4 mixed30 callbacks random write switch_normalized | 12.1 | 12.2 |
| 4 mixed30 callbacks random write switch_raw | 12.3 | 12.0 |
| 4 mixed30 rw hot read regfw | 48.2 | 44.3 |
| 4 mixed30 rw hot read switch_normalized | 2.8 | 2.8 |
| 4 mixed30 rw hot read switch_raw | 2.8 | 2.9 |
| 4 mixed30 rw hot write regfw | 48.7 | 46.3 |
| 4 mixed30 rw hot write switch_normalized | 3.1 | 3.0 |
| 4 mixed30 rw hot write switch_raw | 3.4 | 3.0 |
| 4 mixed30 rw mapped read regfw | 119.4 | 113.9 |
| 4 mixed30 rw mapped read switch_normalized | 11.2 | 11.2 |
| 4 mixed30 rw mapped read switch_raw | 11.7 | 11.3 |
| 4 mixed30 rw mapped write regfw | 118.7 | 113.8 |
| 4 mixed30 rw mapped write switch_normalized | 11.2 | 15.6 |
| 4 mixed30 rw mapped write switch_raw | 12.2 | 16.0 |
| 4 mixed30 rw random read regfw | 51.8 | 48.5 |
| 4 mixed30 rw random read switch_normalized | 11.5 | 12.0 |
| 4 mixed30 rw random read switch_raw | 11.9 | 11.2 |
| 4 mixed30 rw random write regfw | 51.7 | 49.8 |
| 4 mixed30 rw random write switch_normalized | 13.8 | 14.2 |
| 4 mixed30 rw random write switch_raw | 13.1 | 13.4 |

## Scale (8 VF, 262144 mapped u512 words)

```
RESULT construct=0.000790 finish=0.000070 maxrss_kb=9142272 scalar32_w_tps=46884600 scalar32_r_tps=48401585 identity_w_tps=7974952 sink=1099477549056
```
