#!/usr/bin/env python3
"""Matched write-only comparison of fixed, paper policies and the classical multi-granularity hierarchy."""
import csv
import datetime as dt
import hashlib
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
BINARY = ROOT / 'build/with-mgl/adaptive_lock_benchmark'
IMPL = {'fixed', 'balanced', 'dynamic_dp', 'mgl'}
MATCH = ('scenario','threads','operations','trace_size','unique_queries','array_size','hot_width',
         'range_length','seed','sample_rate','rebuild_interval_ms','read_percent','read_operations',
         'write_operations','model','bins','lambda','min_samples','min_improvement','write_body',
         'point_percent','point_operations','range_operations','lock_kind','mgl_leaves')


def stamp():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def main():
    if (OUT/'manifest.json').exists():raise SystemExit('Refusing to overwrite existing measurements')
    sources=['benchmarks/adaptive_lock_benchmark.cpp','dynamic/paper_lock.hpp','dynamic/paper_partitioner.hpp',
             'common/naive_lock.hpp','common/range_lock.hpp','common/multigranularity_lock.hpp']
    m={'status':'running','started_utc':stamp(),'binary_sha256':hashlib.sha256(BINARY.read_bytes()).hexdigest(),
       'source_sha256':{f:hashlib.sha256((ROOT/f).read_bytes()).hexdigest() for f in sources},
       'baseline':'Local implementation of classical IS/IX/S/X multi-granularity locking on a fixed binary hierarchy; not an author artifact',
       'mgl_leaves':1024,'mgl_nodes':2047,'mgl_node_storage_bytes':262016,
       'validation':'14/14 Release CTest passed; MGL compatibility, endpoints, partial overlaps, exception unwind and atomic ranges passed in Release and ASan/UBSan.',
       'method':['Three repetitions, rotating implementation order, sequential runs.',
                 'Identical requests, same counts and full-array validation within each matched quartet.',
                 'Whole-range increment writes only; no read-only requests.',
                 'All construction, destruction, allocation, lock waits, statistics and rebuilding inside wall time.',
                 'Input preparation, array initialization and final verification excluded equally.',
                 'Uniform is a deliberately short point-overhead control; no adaptive rebuild expected before it ends.',
                 'Range workloads use 24M total requests and 10-second rebuild checks; do not mix with old 30-second/64M-or-96M measurements.',
                 'MGL uses 1024 fixed leaves and 2047 padded atomic mode-state nodes, not a64-mutex budget.',
                 'No per-query heap allocation; parent-first intentions and maximal-covered-node S/X modes held until callback return.'],
       'jobs':[]}
    for scenario,operations in [('uniform',5_000_000),('steady_range',24_000_000),('islands_range',24_000_000)]:
        ranges=scenario.endswith('_range')
        command=[str(BINARY),'--implementations','fixed,balanced,dynamic_dp,mgl','--scenario',scenario,
                 '--model','paper','--write-body','range','--threads','8','--repetitions','3','--seed','197',
                 '--operations',str(operations),'--array-size',str(1<<(26 if ranges else 20)),
                 '--hot-width',str(1<<(22 if ranges else 14)),'--range-length','10000' if ranges else '1',
                 '--read-percent','0','--point-percent','0','--lock-kind','mutex','--bins','1024','--mgl-leaves','1024',
                 '--lambda','10000','--sample-rate','64','--rebuild-ms','10000',
                 '--min-samples','640','--min-improvement','0.1','--trace-size','1048576',
                 '--csv',str(OUT/(scenario+'.csv'))]
        m['jobs'].append({'scenario':scenario,'command':command,'status':'pending'})
    def save():
        temp=OUT/'manifest.json.tmp';temp.write_text(json.dumps(m,indent=2)+'\n');temp.replace(OUT/'manifest.json')
    save()
    for job in m['jobs']:
        job.update(status='running',started_utc=stamp());start=time.monotonic();save()
        print('START',job['scenario'],flush=True)
        with (OUT/(job['scenario']+'.txt')).open('w') as log:
            proc=subprocess.Popen(job['command'],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
            job['pid']=proc.pid;save()
            for line in proc.stdout:
                log.write(line);log.flush()
                if 'verified=true' in line or 'paired_speedup=' in line:print(line.strip(),flush=True)
            rc=proc.wait()
        job.update(returncode=rc,elapsed_s=time.monotonic()-start,finished_utc=stamp())
        try:
            if rc:raise ValueError(f'benchmark exit {rc}')
            path=OUT/(job['scenario']+'.csv')
            with path.open() as stream:rows=list(csv.DictReader(stream))
            assert len(rows)==12 and all(r['verified']=='true' for r in rows)
            for rep in ('1','2','3'):
                group=[r for r in rows if r['repetition']==rep]
                assert len(group)==4 and {r['implementation'] for r in group}==IMPL
                assert len({tuple(r[k] for k in MATCH) for r in group})==1
                assert all(r['effective_lock_kind']==('atomic_mgl' if r['implementation']=='mgl' else 'mutex') for r in group)
            job.update(status='complete',csv_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        except Exception as error:
            job.update(status='failed',error=str(error));m['status']='failed';save();raise
        save()
        print('COMPLETE',job['scenario'],flush=True)
    assert hashlib.sha256(BINARY.read_bytes()).hexdigest()==m['binary_sha256']
    for f,h in m['source_sha256'].items():assert hashlib.sha256((ROOT/f).read_bytes()).hexdigest()==h
    m.update(status='complete',finished_utc=stamp(),verified_runs=36,elapsed_s=sum(j['elapsed_s'] for j in m['jobs']));save()


if __name__=='__main__':main()
