# RocksDB PerconaFT LockTree baseline

Подготовлен внешний userspace baseline с API `ReadQuery(left,right,callback)` и `WriteQuery(left,right,callback)`. **Сборка и проверки корректности прошли на локальном Mac, включая ASan/UBSan.** Начатое сравнение остановлено по просьбе пользователя после обнаружения большого overhead; частичные результаты сохранены отдельно и не используются в таблице MGL.

## Происхождение

- Репозиторий: [facebook/rocksdb](https://github.com/facebook/rocksdb).
- Фиксированный commit: `f16beed54628fdc0f2d8bba779f7fb0c7611fcf2`.
- [Исходная библиотека LockTree](https://github.com/facebook/rocksdb/tree/f16beed54628fdc0f2d8bba779f7fb0c7611fcf2/utilities/transactions/lock/range/range_tree/lib) — часть PerconaFT, выделенная авторами RocksDB для использования без остальной БД. [PR 7753](https://github.com/facebook/rocksdb/pull/7753) описывает импорт, [PR 7506](https://github.com/facebook/rocksdb/pull/7506) — диапазонный интерфейс RocksDB.
- В `upstream/` сохранены 44 неизменённых файла; Git blob SHA-1 и SHA-256 каждого находятся в [source.json](source.json). Выбран разрешённый upstream вариант лицензирования Apache-2.0; оригинальные уведомления и лицензии сохранены.

Это **RocksDB/PerconaFT LockTree**, а не авторский artifact Kogan–Dice–Issa. В [Citron, FAST 2023](https://www.usenix.org/system/files/fast23-gao.pdf), раздел 5, обсуждается range tree в RocksDB и цитируется PerconaFT; это не означает, что LockTree входил в измеренные baselines Citron. Открытый авторский userspace-код Kogan–Dice–Issa в проведённом поиске не найден. Найденный FreeBSD-порт зависит от механизмов ядра и сюда не включён.

## Соответствие нашему API

`LockTreeRangeLock` объявлен в [common/locktree_lock.hpp](../../common/locktree_lock.hpp), реализация обёртки — в [.cpp](../../common/locktree_lock.cpp).

- Диапазоны **включительные** `[left,right]`, как у наших классов и у LockTree. Концы ограничиваются массивом и упорядочиваются; пустой массив не вызывает callback.
- Весь callback выполняется под одним логическим range lock. Native дерево может использовать несколько внутренних mutex: число внутренних захватов не подменяется единицей.
- Каждому запросу соответствует отдельный живой идентификатор владельца. Между запросами диапазоны не сохраняются. После callback, включая исключение, обёртка освобождает диапазон и вызывает native retry для ожидающих запросов.
- Запрос на чтение использует native `READ`, запись — `WRITE`. **Эта версия разделяет read-lock только при совпадающих границах. Разные частично пересекающиеся read-диапазоны могут сериализоваться.** Для основного сравнения разумно сначала использовать пишущие запросы; в RW-результатах обязательно указать это ограничение.
- Обёртка не создаёт БД и не выполняет дисковый ввод-вывод. Ключи — числовые индексы `uint64_t`; сравнение не зависит от endian или выравнивания памяти.
- Выделение native узлов, ключей, release buffer и condition variable учитывается в запросе. Callback передаётся без дополнительного heap allocation через синхронный trampoline. Native `lock_request` создаётся и уничтожается на каждый запрос, как в API-адаптере RocksDB; библиотека также допускает повторное использование request, поэтому этот lifecycle нельзя выдавать за минимально возможные накладные расходы LockTree.

Все обращения к защищаемому массиву должны использовать один объект блокировки. Нельзя уничтожать его при активных callback или ждать внутри callback завершения конфликтующего запроса. Обёртка обеспечивает освобождение при исключении из callback; полная гарантия восстановления после OOM внутри native библиотеки не заявляется.

## Локальный portability слой

`compat/` предоставляет только используемые библиотекой интерфейсы mutex/condition-variable factory, реализованные через pthread mutex/condvar. Это не полная RocksDB API/ABI. Дерево, поиск конфликтов, shared-owner logic, STO, ожидание/повтор запросов и освобождение native памяти остаются upstream-кодом. Стандартный лимит памяти менеджера — 64 MiB, без отключения его встроенных оптимизаций.

Native заголовки изолированы в `.cpp`, поскольку содержат глобальные portability-макросы. CMake-цели: `locktree_standalone` для библиотеки и `locktree_baseline` для обёртки. Подготовлено для macOS/Linux с GCC/Clang и pthreads; сборка и тесты выполнены на macOS; запуск на Linux пока не проверялся.

## Проверка сборки

Команды для повторной проверки в отдельной сборке:

```sh
cmake -S third_party/rocksdb_locktree -B build/locktree-check \
  -DCMAKE_BUILD_TYPE=Debug -DLOCKTREE_BUILD_TESTS=ON
cmake --build build/locktree-check -j 4
ctest --test-dir build/locktree-check --output-on-failure
```

На Mac при необходимости добавьте `-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++`. Аналогично следует проверить отдельную сборку с `-DSANITIZER=address,undefined`. Тесты покрывают границы, исключения callback, параллельные идентичные читатели, непересекающиеся писатели и атомарность диапазонных обновлений. Timeout теста — 30 секунд.

## Подключение к benchmark позднее

Подключение к основному benchmark уже применено. [Исходный патч](../../integrations/locktree_benchmark.patch) сохранён как история изменения; **повторно применять его не нужно**. Включение библиотеки остаётся опциональным:

```sh
cmake -S . -B build/with-locktree -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_LOCKTREE_BASELINE=ON -DLOCKTREE_BUILD_TESTS=ON
cmake --build build/with-locktree -j 4
ctest --test-dir build/with-locktree --output-on-failure
```

Пример короткого сравнения:

```sh
./build/with-locktree/adaptive_lock_benchmark \
  --model paper --write-body range --scenario steady_range \
  --implementations fixed,balanced,dynamic_dp,locktree --lock-kind mutex \
  --threads 8 --array-size 67108864 --hot-width 4194304 --range-length 1000 \
  --operations 1000000 --read-percent 0 --bins 1024 --lambda 10000 \
  --sample-rate 64 --rebuild-ms 100 --repetitions 1 --seed 97 \
  --csv /tmp/paper-vs-locktree.csv
```

Этот короткий пример проверяет проводку стенда; для итогового вывода нужны длительные matched runs с перестройками и повторами. `--lock-kind` выбирает примитив для наших реализаций; LockTree всегда использует свой native механизм. CSV содержит `effective_lock_kind=locktree_native` в его CSV-строку. Старый summarizer поддерживает только fixed/balanced/DP; таблицу с новым baseline нужно строить отдельно, сверяя seed, число операций, read/write mix и геометрию, а не смешивая результаты разных запусков.

Начатая серия: [команды, статусы и сохранённые результаты](../../BENCHMARK_RESULT/2026-11-17/locktree_comparison/manifest.json). Она остановлена пользователем; продолжать её автоматически не следует.
