JavaRQBench: Java Benchmarks for Concurrent Maps with Support for Range Queries
===============================================================================

This repository contains code for benchmarking and testing Java
implementations of concurrent maps with support for range queries and
state-of-the-art implementations of such maps.

Compile and Test
----------------

**Requirements**

* The [sbt](http://www.scala-sbt.org/) build tool
* [JDK 8](http://www.oracle.com/technetwork/java/javase/downloads/jdk8-downloads-2133151.html) or greater
* [python3](https://www.python.org/) (Needed to run benchmarks)
* [matplotlib](https://matplotlib.org/) (Needed to plot benchmark results)

**Compilation**

When the requirements are installed the following command can be used
to compile everything and run the tests (the tests can take a few
minutes to complete depending on the system):

`sbt test`


Benchmarks
----------

The folder `benchmark_tools` contains scripts for running three types
of benchmarks that are briefly described below. A detailed description
of these benchmarks can be found in the [LFCA tree paper][1]
([preprint][2]).

### Mixed Operations

In the mixed operations benchmark a configurable number of threads
perform a configurable mix of operations.

More information about this benchmark can be found in
[benchmark_tools/MIX_OPS_BENCH_README.md][3].

### Separate Threads For Updates and Range Queries

In this benchmark half of the threads do update operations
(put and remove) while the other half only do range queries.

More information about this benchmark can be found in   
[benchmark_tools/SEP_THREADS_BENCH_README.md][4].

### Time Series Benchmark For the LFCA Tree

Currently, this benchmark can only be used for the LFCA tree. The
intention of this benchmark is to produce a time series showing how
the number of route nodes and the throughput of an LFCA tree changes
when the workload changes.

More information about this benchmark can be found in   
[benchmark_tools/TIME_SERIES.md][5].

Data Structures
---------------
This repository contains the following data structures.


**LFCA tee** - the lock-free contention adapting search tree

Publication:

Kjell Winblad, Konstantinos Sagonas, and Bengt Jonsson. Lock-free
contention adapting search trees. In Proceedings of the 30th ACM
Symposium on Parallelism in Algorithms and Architectures, SPAA ’18,
2018.

**Im-Tr-CA** - the contention adapting search tree with an optimization enabled by an immutable treap

Publication:

Kjell Winblad. Faster Concurrent Range Queries with Contention
Adapting Search Trees Using Immutable Data. In 2017 Imperial College
Computing Student Workshop (ICCSW 2017)

Code located in: src/main/java/se/uu/collection/ImmTreapCATreeMapSTDR.java

**AVL-CA** - the contention adapting search tree tree using an AVL tree as the sequential data structure

Publications:

Konstantinos Sagonas and Kjell Winblad. A contention adapting approach
to concurrent ordered sets. Journal of Parallel and Distributed
Computing, 2018.

Konstantinos Sagonas and Kjell Winblad. Contention adapting search
trees. In 14th International Symposium on Parallel and Distributed
Computing, ISPDC, 2015

Code located in: src/main/java/se/uu/collection/CATreeMapSTDR.java

**SL-CA** - the contention adapting search tree using a skip list with fat nodes as the sequential data structure

Publications:

Konstantinos Sagonas and Kjell Winblad. A contention adapting approach
to concurrent ordered sets. Journal of Parallel and Distributed
Computing, 2018.

Konstantinos Sagonas and Kjell Winblad. Efficient support for range
queries and range updates using contention adapting search trees. In
Languages and Compilers for Parallel Computing - 28th International
Workshop, LCPC, 2016.

Code located in: src/main/java/se/uu/collection/FatCATreeMapSTDR.java

**SnapTree** - A Practical Concurrent Binary Search Tree

Publication:

Nathan G. Bronson, Jared Casper, Hassan Chafi, and Kunle Olukotun. A
practical concurrent binary search tree. In Proceedings of the 15th ACM
SIGPLAN Symposium on Principles and Practice of Parallel Programming,
PPoPP ’10

Code located in: src/main/java/edu/stanford/ppl/concurrent/SnapTreeMap.java

**k-ary** - The lock-free k-ary search tree

Publications:

Trevor BrownJoanna Helga. Non-blocking k-ary Search Trees. In
Principles of Distributed Systems: 15th International Conference,
OPODIS 2011.

Trevor Brown and Hillel Avni. Range queries in non-blocking k-ary
search trees. In Principles of Distributed Systems: 16th International
Conference, OPODIS 2012.

Code located in: src/main/java/algorithms/published/LockFreeKSTRQ.java

**Data structures not included**

The following data structures are not included in this repository for
licensing reasons but have been used in this benchmark framework
previously ([see][1]).

**KiWi** - A lock-free data structure with range query support

Publication:

Dmitry Basin, Edward Bortnikov, Anastasia Braginsky, Guy Golan-Gueta,
Eshcar Hillel, Idit Keidar, and Moshe Sulamy. KiWi: A Key-Value
Map for Scalable Real-Time Analytics. In Proceedings of the 22Nd ACM
SIGPLAN Symposium on Principles and Practice of Parallel Programming
(PPoPP ’17).

Java source code for the KiWi is available
[here](https://github.com/Eshcar/KiWi). The KiWi's source code is not
included in this repository as it does not have any license.

**ChatterjeeSL** - A general method for creating data structures with
  range query support applied to a lock-free skip list

Publication:

Bapi Chatterjee. 2017.  Lock-free Linearizable 1-Dimensional Range
Queries.  In Proceedings of the 18th International Conference on
Distributed Computing and Networking (ICDCN ’17).

License
-------

GNU General Public License (GPL) Version 3


[1]: https://doi.org/10.1145/3210377.3210413
[2]: http://www.it.uu.se/research/group/languages/software/ca_tree/spaa2018lfcatree.pdf
[3]: benchmark_tools/MIX_OPS_BENCH_README.md
[4]: benchmark_tools/SEP_THREADS_BENCH_README.md
[5]: benchmark_tools/TIME_SERIES_README.md