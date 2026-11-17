Throughput relative to fixed (higher is better).

8 worker threads; three matched repetitions; median paired ratios. Paper balanced/DP use K=64, B=1024, lambda=10000 and 10-second rebuild checks. MGL is a local implementation of classical IS/IX/S/X hierarchical locking, with 1024 fixed leaves and 2047 atomic-state nodes (262016 bytes). Writes increment every element of each requested range. All initialization, allocation, cold start and rebuilding costs are included.

| Workload | Q (M), total | Balanced | DP | MGL |
|---|---:|---:|---:|---:|
| Uniform points (short overhead control) | 5 | 0.59 | 0.96 | 0.06 |
| steady_range | 24 | 1.76 | 1.67 | 2.02 |
| islands_range | 24 | 1.86 | 1.87 | 2.67 |

Points use N=2^20. Ranges use N=2^26, L=10000, H=2^22; islands each H/16. No read-only requests. Uniform runs of fixed/balanced/DP finish in less than a second and do not rebuild; their small timing differences should not be interpreted as adaptation results.

MGL wins over both paper policies on these two range workloads including cold start. Its hierarchy offers fine regions from the beginning, uses more lock states, and uses atomic mode counters/wait rather than std::mutex data locks; this experiment does not isolate those factors or establish a universal winner.

| Workload | Fixed wall, s | Balanced wall, s | DP wall, s | MGL wall, s |
|---|---:|---:|---:|---:|
| uniform | 0.072 | 0.109 | 0.075 | 1.116 |
| steady_range | 35.156 | 21.677 | 21.066 | 17.442 |
| islands_range | 45.850 | 24.762 | 24.511 | 17.459 |

36/36 final-array checks passed. Release14/14 CTest and the MGL ASan/UBSan test passed. The stopped LockTree experiment is separate and not combined into these ratios.

[Raw summary](summary.csv) · [Commands and source hashes](manifest.json) · [Build and method](../../../README.md)
