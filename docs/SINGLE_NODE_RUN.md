# Запуск на 32 и 64 физических ядрах одной NUMA ноды

`scripts/run_single_node.py` выбирает одну NUMA-ноду и запускает конфигурации последовательно: сначала 32 рабочих потока, затем 64. Для каждого физического ядра берётся один разрешённый online logical CPU; SMT-соседи не считаются дополнительными ядрами. Память привязывается к той же ноде. Серверные измерения пока **не выполнялись**: проверены только локальные тесты разбора топологии и формирования команд.

## Исходники и требования

Нужны Linux, Python 3.9+, CMake 3.16+, подходящий GCC/Clang с C++20 (`jthread`, `barrier`, `atomic::wait`, `atomic_ref`) и `__int128`, а также `lscpu` из util-linux и `numactl`. Практичный вариант компилятора — GCC 12 или новее с соответствующей libstdc++. Нельзя переносить готовые macOS-бинарники или CMake cache на Linux.

На сервере клонируйте ветку с измеренным кодом и скриптами запуска.

```sh
git clone --single-branch --branch experiments/paper-range-locks-20261118 https://github.com/MiskinAleksandr23/dynamic_locks.git dynamic-server
cd dynamic-server
```

Для переноса без Git есть исходный пакет `snapshots/2026-11-17-server-source.tar.gz`. При использовании пакета распакуйте его в новый пустой каталог.

```sh
mkdir dynamic-server
tar -xzf /path/to/2026-11-17-server-source.tar.gz -C dynamic-server
cd dynamic-server
```

Сборка C++ benchmark и проверка корректности:

```sh
cmake -S . -B build/server-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++ \
  -DBUILD_LOCKTREE_BASELINE=OFF \
  -DBUILD_GENETIC_BENCHMARK=OFF \
  -DBUILD_BENCHMARK_V700=OFF
cmake --build build/server-release -j 8
ctest --test-dir build/server-release --output-on-failure
python3 tests/run_single_node_test.py
```

Вместо `g++` можно указать подходящий `clang++`. Эти команды собирают C++ fixed/balanced/DP/MGL. Наличие исходников CA tree в пакете не означает, что Java-реализация сравнивается этим C++ executable: для неё нужен отдельный согласованный harness и JDK.

## Проверка размещения без запуска нагрузки

Сначала посмотрите, какую топологию видит Linux:

```sh
lscpu
numactl --hardware
```

Затем выполните dry-run будущей серии. Фигурные скобки оставляются буквально, вместе с кавычками:

```sh
python3 scripts/run_single_node.py \
  --threads 32 64 --node auto --dry-run \
  --output BENCHMARK_RESULT/server-paper \
  -- python3 scripts/paper_experiments.py \
  --binary build/server-release/adaptive_lock_benchmark \
  --threads '{threads}' --output '{output}' \
  --repetitions 3 --seed 97 --bins 1024 --lambda 10000
```

Dry-run не создаёт каталог результатов и не выполняет benchmark. Он печатает выбранную ноду, точные CPU IDs и обе команды `numactl`. Набор для 32 потоков является подмножеством набора для 64. Проверяются `lscpu`, текущая CPU affinity, `Mems_allowed_list`, kernel online masks и `thread_siblings_list` из sysfs.

Для этой серии одна нода должна предоставлять **64 разрешённых физических ядра**. Если «128 ядер» означает 128 logical CPUs с SMT2, распределённых между двумя нодами, на ноде может быть только 32 физических ядра. Тогда скрипт отвергнет запрос 64: не включит второй SMT-поток и не перейдёт на другую ноду незаметно. В таком окружении можно явно запустить только `--threads 32` либо получить другую CPU allocation.

В следующих командах явно выбрана **нода 0**. Если dry-run выбрал другую ноду, замените `--node 0` во всех командах на её номер. Не предполагайте, что первые 64 числовых CPU IDs образуют одну ноду или 64 разных ядра.

## Полная серия fixed, balanced и DP

После успешного dry-run запустите ту же серию с `--execute`:

```sh
python3 scripts/run_single_node.py \
  --threads 32 64 --node 0 --execute \
  --output BENCHMARK_RESULT/server-paper \
  -- python3 scripts/paper_experiments.py \
  --binary build/server-release/adaptive_lock_benchmark \
  --threads '{threads}' --output '{output}' \
  --repetitions 3 --seed 97 --bins 1024 --lambda 10000
```

`paper_experiments.py` выполняет восемь нагрузок из своего `CASES`: `islands_range`, `uniform`, `clusters_2`, `unequal_widths_range`, `steady_range`, `sliding_variable_range`, `mixed_range`, `hot_with_spike`. Для выбора подмножества добавьте, например, `--cases steady_range islands_range` **в конец команды дочернего скрипта**.

В каждой конфигурации сравниваются только `fixed,balanced,dynamic_dp`, на одинаковых запросах и с одинаковым числом операций. Записи увеличивают все элементы диапазона. Числа запросов, длины, интервалы и read/point mix зафиксированы в скрипте; обычно интервал проверки 30 с, у sliding/mixed — 10 с. `--lambda 10000` применяется к сырым счётчикам, как в текущей модели.

**Q — суммарное число запросов всех потоков, а не число на поток.** Сохранённый объём не гарантирует ту же длительность на x86-64, что на Mac. Перед большой серией полезно выполнить одно подмножество в новом каталоге с `--repetitions 1` и проверить `wall_s`, `rebuilds` и полные логи. Если быстрые реализации заканчиваются до первой проверки, такой замер не показывает окупаемость адаптации. Любое изменение Q или интервала нужно применить одинаково ко всем реализациям соответствующего сравнения и явно сохранить как другую конфигурацию.

Результаты размещаются так:

```text
BENCHMARK_RESULT/server-paper/
  placement.json
  32t.console.log
  64t.console.log
  32t/manifest.json, *.csv, *.txt
  64t/manifest.json, *.csv, *.txt
```

После завершения:

```sh
python3 scripts/summarize_paper.py BENCHMARK_RESULT/server-paper/32t
python3 scripts/summarize_paper.py BENCHMARK_RESULT/server-paper/64t
```

Получатся `summary.json` и `summary.csv`. Summarizer проверяет совпадение seed, геометрии и фактических видов запросов внутри каждого повтора. Его текущий формат рассчитан на fixed/balanced/DP, а не на дополнительный MGL.

## Прямое сравнение с MGL на 64 и 1024 листьях

`paper_experiments.py` не добавляет MGL. Для этого используйте непосредственный вызов benchmark через тот же placement runner. Ниже обе гранулярности запускаются **последовательно**, с одинаковыми Q, seed, интервалом и выбранной нодой.

Сначала только просмотр команд:

```sh
for mgl_leaves in 64 1024; do
  python3 scripts/run_single_node.py \
    --threads 32 64 --node 0 --dry-run \
    --output "BENCHMARK_RESULT/server-mgl-${mgl_leaves}" \
    -- ./build/server-release/adaptive_lock_benchmark \
    --implementations fixed,balanced,dynamic_dp,mgl \
    --model paper --write-body range --scenario steady_range \
    --threads '{threads}' --csv '{output}.csv' \
    --operations 24000000 --repetitions 3 --seed 197 \
    --array-size 67108864 --hot-width 4194304 --range-length 10000 \
    --read-percent 0 --point-percent 0 --lock-kind mutex \
    --mgl-leaves "$mgl_leaves" --bins 1024 --lambda 10000 \
    --sample-rate 64 --rebuild-ms 10000 --min-samples 640 \
    --min-improvement 0.1 --trace-size 1048576
done
```

После проверки выбранных CPU IDs — запуск:

```sh
set -e
for mgl_leaves in 64 1024; do
  python3 scripts/run_single_node.py \
    --threads 32 64 --node 0 --execute \
    --output "BENCHMARK_RESULT/server-mgl-${mgl_leaves}" \
    -- ./build/server-release/adaptive_lock_benchmark \
    --implementations fixed,balanced,dynamic_dp,mgl \
    --model paper --write-body range --scenario steady_range \
    --threads '{threads}' --csv '{output}.csv' \
    --operations 24000000 --repetitions 3 --seed 197 \
    --array-size 67108864 --hot-width 4194304 --range-length 10000 \
    --read-percent 0 --point-percent 0 --lock-kind mutex \
    --mgl-leaves "$mgl_leaves" --bins 1024 --lambda 10000 \
    --sample-rate 64 --rebuild-ms 10000 --min-samples 640 \
    --min-improvement 0.1 --trace-size 1048576
done
```

Для островков замените `--scenario steady_range` на `--scenario islands_range` и используйте новые каталоги, например `server-islands-mgl-64`/`server-islands-mgl-1024`. В каждом каталоге будут `32t.csv`, `64t.csv`, console logs и `placement.json`.

64 листа MGL означают **127 узлов**, 1024 листа — **2047 узлов**. Это не одинаковый бюджет с K=64 mutex у fixed/balanced/DP. CSV хранит `mgl_leaves`, `hierarchy_leaves`, `hierarchy_nodes`, `hierarchy_bytes` и `effective_lock_kind`; результаты разных гранулярностей нужно показывать отдельно. Эти команды используют только пишущие запросы; `--lock-kind mutex` относится к нашим вариантам, а MGL применяет собственные атомарные IS/IX/S/X состояния.

## Отдельный эксперимент с авторским CA tree

Нужен **JDK 17+**, причём `java` и `javac` должны относиться к одной установке. Локальная проверка выполнена с JDK 21.0.8; серверного результата пока нет. Код, семантика операций, warmup и ограничения сравнения описаны в [CA tree README](../third_party/ca_tree_java/README.md).

Этот harness сравнивает авторский CA tree с `fixed_avl` в Java ordered map. Каждый замер запускается в **новой JVM**; измеряемая CA tree начинает с одного base node. Это отдельный эксперимент: его числа нельзя напрямую добавлять в таблицу C++ array-lock как CA/fixed C++.

Dry-run:

```sh
python3 scripts/run_single_node.py \
  --threads 32 64 --node auto --dry-run \
  --output BENCHMARK_RESULT/server-ca \
  -- python3 scripts/ca_tree_experiments.py \
  --threads '{threads}' --output '{output}' --repetitions 3
```

После проверки ноды, с тем же явным выбором ноды 0, что и выше:

```sh
python3 scripts/run_single_node.py \
  --threads 32 64 --node 0 --execute \
  --output BENCHMARK_RESULT/server-ca \
  -- python3 scripts/ca_tree_experiments.py \
  --threads '{threads}' --output '{output}' --repetitions 3
```

По умолчанию выполняются `steady_range`, `islands_range`, `uniform_point`. Число запросов меняется флагами `--range-operations` и `--point-operations`; для первого просмотра времени можно явно задать меньшее одинаковое число запросов и один повтор в другом новом каталоге. Скрипт сам компилирует Java sources, выполняет self-tests и сохраняет версии JDK, команды, исходники и CSV. `--java`/`--javac` позволяют выбрать конкретную установку вместо `PATH`.

JVM запускается с G1, `-Xms512m -Xmx2g`, `ActiveProcessorCount` равным числу workers и `-XX:-UseNUMA`. GC, JIT и другие helper threads используют **тот же CPU set**, что и workers; дополнительных ядер вне ноды runner не выдаёт. Heap создаётся после привязки процесса. Конфигурации 32 и 64 выполняются последовательно, результаты находятся в `server-ca/32t/` и `server-ca/64t/`, размещение — в `server-ca/placement.json`.

## Что гарантирует привязка

Перед каждым workload скрипт повторно проверяет топологию, доступные CPU, разрешённые memory nodes и хеши исполняемых файлов. Затем выполняет короткий bound preflight `numactl --show` и требует точного совпадения `physcpubind`, `policy: bind` и `membind`. При отказе `numactl` или несовпадении workload не запускается; fallback на другую ноду, SMT либо interleave отсутствует.

Привязка применяется **до запуска дочерней программы**. Массив, trace, эталон и статистика создаются и впервые трогаются уже под `--membind` выбранной ноды. Это важно: сначала инициализировать большой массив на другой ноде, а затем привязать только workers было бы другим экспериментом.

Все потоки процесса наследуют **один общий CPU set**: 32 или 64 выбранных физических ядра. Coordinator, rebuilder и другие служебные потоки используют этот же набор. Это не назначение «worker i всегда работает на core i»: ОС может перемещать потоки внутри набора. Запускаемая программа не должна сама расширять affinity или менять NUMA policy. Скрипт не резервирует CPU от чужих задач; серию следует выполнять без конкурирующих benchmark-процессов.

`placement.json` сохраняет исходную топологию, ограничения affinity/cpuset, выбранные ядра, проверенный memory policy, команды, хеши, PID и exit status. Каталоги вывода должны быть новыми; существующие результаты не перезаписываются. При ошибке серия останавливается. Ctrl-C завершает активную группу дочерних процессов и оставляет частичные файлы с пометкой interruption; автоматически продолжать такой набор как полный результат нельзя.
