Table 1: Throughput relative to fixed (higher is better).

**(a) Four worker threads**

| Workload | Q (M) | Bal. | DP | Gen. | Global | Element |
|---|---:|---:|---:|---:|---:|---:|
| Uniform point | 5000 | 0.99 | 0.99 | 0.97 | 0.26 | 0.79 |
| Two clusters point | 2000 | 1.50 | 1.51 | 1.50 | 1.00 | 10.33 |
| Eight clusters point | 2000 | 1.03 | 1.00 | 1.03 | 0.64 | 7.00 |
| Unequal point | 2000 | 1.49 | 1.54 | 1.39 | 1.02 | 11.21 |
| Moving point | 2000 | 1.03 | 1.03 | 0.97 | 0.95 | 9.34 |
| Hot point + spike | 2000 | 0.78 | 0.79 | 0.78 | 0.67 | 0.56 |
| Steady range | 64 | 1.38 | 1.39 | 1.11 | 0.43 | — |
| Two clusters range | 64 | 1.20 | 1.20 | 0.88 | 0.36 | — |
| Four clusters range | 64 | 1.09 | 1.06 | 0.81 | 0.33 | — |
| Range islands | 96 | 2.17 | 2.12 | 1.87 | 0.61 | — |
| Range gradient | 96 | 2.01 | 1.98 | 1.48 | 0.56 | — |
| Unequal-width range | 96 | 1.75 | 1.73 | 1.51 | 0.51 | — |
| Popularity exchange | 96 | 1.35 | 1.36 | 1.32 | 0.45 | — |
| Moving variable range | 160 | 1.33 | 1.31 | 1.32 | 0.41 | — |
| Sliding variable range | 180 | 1.41 | 1.44 | 1.40 | 0.39 | — |
| Steady range (short) | 1 | 1.66 | 1.65 | 1.69 | 0.49 | 0.07 |
| Two clusters range (short) | 1 | 1.92 | 1.89 | 1.54 | 0.59 | 0.09 |
| Moving variable (short) | 1 | 1.90 | 1.82 | 1.92 | 0.54 | 0.10 |

**(b) Ten worker threads**

| Workload | Q (M) | Bal. | DP | Gen. | Global | Element |
|---|---:|---:|---:|---:|---:|---:|
| Uniform point | 5000 | 0.97 | 0.98 | 1.06 | 0.30 | 3.06 |
| Two clusters point | 2000 | 2.20 | 1.90 | 1.89 | 1.01 | 10.20 |
| Eight clusters point | 2000 | 1.54 | 1.53 | 1.56 | 0.76 | 10.08 |
| Unequal point | 2000 | 2.27 | 2.32 | 2.99 | 1.61 | 19.16 |
| Moving point | 2000 | 1.45 | 1.48 | 1.41 | 1.08 | 9.86 |
| Hot point + spike | 2000 | 0.78 | 0.80 | 0.78 | 0.61 | 0.67 |
| Steady range | 64 | 1.53 | 1.40 | 1.45 | 0.49 | — |
| Two clusters range | 64 | 1.19 | 1.22 | 1.11 | 0.38 | — |
| Four clusters range | 64 | 1.07 | 1.06 | 1.00 | 0.32 | — |
| Range islands | 96 | 2.51 | 2.46 | 2.37 | 0.69 | — |
| Range gradient | 96 | 2.06 | 2.11 | 1.90 | 0.62 | — |
| Unequal-width range | 96 | 1.82 | 1.98 | 1.73 | 0.55 | — |
| Popularity exchange | 96 | 1.39 | 1.41 | 1.42 | 0.48 | — |
| Moving variable range | 160 | 1.47 | 1.48 | 1.45 | 0.50 | — |
| Sliding variable range | 180 | 1.52 | 1.56 | 1.72 | 0.38 | — |
| Steady range (short) | 1 | 2.52 | 2.40 | 2.19 | 0.42 | 0.11 |
| Two clusters range (short) | 1 | 1.50 | 1.51 | 1.49 | 0.27 | 0.08 |
| Moving variable (short) | 1 | 1.73 | 1.44 | 1.58 | 0.43 | 0.14 |

