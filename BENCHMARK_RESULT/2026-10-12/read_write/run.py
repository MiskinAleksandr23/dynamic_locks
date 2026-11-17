#!/usr/bin/env python3
"""Nine sequential mixed-read measurements with a 9 minute total execution cap."""
import csv
import datetime as dt
import hashlib
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent


def save(m):
    (OUT / 'manifest.json').write_text(json.dumps(m, indent=2) + '\n')


def main():
    if (OUT / 'manifest.json').exists():
        raise SystemExit('Refusing to overwrite measurements')
    files = ['common/range_lock.hpp', 'common/naive_lock.hpp', 'common/element_lock.hpp',
             'common/spinlock.hpp', 'common/spinlock.cpp', 'dynamic/dynamic_lock.hpp',
             'dynamic/partitioner.hpp', 'benchmarks/adaptive_lock_benchmark.cpp',
             'build/read-write/adaptive_lock_benchmark']
    m = dict(started_utc=dt.datetime.now(dt.timezone.utc).isoformat(), status='running',
             source_sha256={p: hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in files},
             validation='Release CTest 10/10 passed including concurrent shared reads, atomic range writes, read statistics, periodic rebuilds, exceptions and mixed-trace reference replay.',
             method='One repetition. Same input and 90% read probability for every run. Existing yield-based spinlock vs std::mutex vs std::shared_mutex. Shared reads sample with thread-local RNG and atomic_ref counter increments; all backends use this read-statistics path. Writes and rebuild snapshots remain exclusive. Whole lock lifetime timed. No warmup; all array updates verified.',
             total_execution_cap_s=540, jobs=[])
    for index, implementation in enumerate(('fixed', 'balanced', 'dynamic_dp')):
        kinds = ['mutex', 'spinlock', 'rwlock']
        for kind in kinds[index:] + kinds[:index]:
            name = f'{implementation}_{kind}'
            command = [str(ROOT/'build/read-write/adaptive_lock_benchmark'),
                       '--scenario', 'steady_range', '--threads', '8', '--operations', '24000000',
                       '--array-size', '67108864', '--hot-width', '4194304', '--range-length', '10000',
                       '--read-percent', '90', '--lock-kind', kind, '--implementations', implementation,
                       '--rebuild-ms', '10000', '--sample-rate', '64', '--seed', '1',
                       '--trace-size', '1048576', '--repetitions', '1', '--csv', str(OUT/(name+'.csv'))]
            m['jobs'].append(dict(name=name, command=command, status='pending'))
    save(m)
    start = time.monotonic()
    signature = None
    for job in m['jobs']:
        remaining = 540-(time.monotonic()-start)
        if remaining <= 0:
            m['status'] = 'time_limit'
            break
        job.update(status='running', started_utc=dt.datetime.now(dt.timezone.utc).isoformat())
        save(m)
        print('START',job['name'],flush=True)
        with (OUT/(job['name']+'.txt')).open('w') as log:
            try:
                result = subprocess.run(job['command'],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,
                                        timeout=remaining)
            except subprocess.TimeoutExpired:
                job['status'] = 'time_limit'
                m['status'] = 'time_limit'
                break
        job['returncode'] = result.returncode
        if result.returncode:
            job['status'] = 'failed'
            m['status'] = 'failed'
            break
        with (OUT/(job['name']+'.csv')).open() as f:
            rows=list(csv.DictReader(f))
        assert len(rows)==1 and rows[0]['verified']=='true'
        row=rows[0]
        keys=('scenario','threads','operations','seed','trace_size','unique_queries','array_size',
              'hot_width','range_length','sample_rate','rebuild_interval_ms','read_percent',
              'read_operations','write_operations')
        current=tuple(row[k] for k in keys)
        if signature is None: signature=current
        assert current==signature
        assert int(row['read_operations'])+int(row['write_operations'])==24000000
        job.update(status='complete',result=row)
        save(m)
        print('DONE',job['name'],f"{24000000/float(row['wall_s'])/1e6:.3f} Mqps",
              'wall_s='+row['wall_s'],'cpu_s='+row['cpu_s'],'rebuilds='+row['rebuilds'],flush=True)
    if all(j['status']=='complete' for j in m['jobs']):m['status']='complete'
    m.update(elapsed_s=time.monotonic()-start, finished_utc=dt.datetime.now(dt.timezone.utc).isoformat())
    save(m)
    if m['status']!='complete':raise SystemExit(m['status'])


if __name__=='__main__':main()
