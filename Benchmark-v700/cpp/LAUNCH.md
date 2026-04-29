# LAUNCH

After setting up the project, you can launch benchmarks. Commands to launch have the following pattern:

```shell
cd setbench/microbench

LD_PRELOAD=../lib/libjemalloc.so ./bin/<data_structure_name>.debra <bench_args>
```

Example:
```shell
LD_PRELOAD=../lib/libjemalloc.so ./bin/aksenov_splaylist_64.debra -json-file json_example.txt -result-file result_example.txt 
```

## Benchmark arguments 

+ `-json-file <file_name>` — file with launch options in the json format ([BenchParameters](microbench/workloads/bench_parameters.h), [example](microbench/json_exampl/json_example.cpp))
+ `-result-file <file_name>` — file to output the results in the json format (optional)

## Troubleshooting

If something breaks after the launch, or there is such a problem:

```shell
PAPI ERROR: thread 0 unable to add event PAPI_L2_DCM: Permission level does not permit operation
```
then the following can help:

```shell
sudo sysctl kernel.perf_event_paranoid=1
```
