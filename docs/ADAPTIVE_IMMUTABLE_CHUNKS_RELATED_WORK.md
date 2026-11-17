# Адаптация разбиения immutable-кусков: ближайшие работы

Дата проверки: 23 октября 2026. Продолжение LOCK_FREE_ARRAY_RESEARCH.md.
Поисковый обзор первичных источников, не полный systematic review.
Без субагентов, новых реализаций и benchmark runs.

## Исследуемая конструкция

Фиксированное индексное пространство массива разбито на непрерывные области.
Каждая область представлена ссылкой на immutable-контейнер. Операция строит
новые версии и согласованно публикует ссылки. Динамическая политика выбирает
размеры/границы областей по workload. Обновление нескольких ссылок может быть
реализовано MCAS, но MCAS — механизм, а не вся постановка.

Искали не просто динамические деревья/массивы, а изменение гранулярности
неизменяемых контейнеров по нагрузке, включая atomic batch updates.

## Главный результат поиска

Общая идея уже реализована в очень близких структурах — прежде всего Jiffy,
а для point updates + range reads также в Im-Tr-CA и LFCA. Нельзя объявлять
новизной «immutable chunks + workload-driven split/merge + lock-free».

### Jiffy — Kobus, Kokociński, Wojciechowski, PPoPP 2022

- Полный опубликованный текст: https://www.cs.put.poznan.pl/pawelw/pdf/ppopp22-full.pdf
- Краткий авторский обзор HOPC 2024: https://www.cs.put.poznan.pl/pawelw/pdf/hopc24a.pdf
- Ранняя версия: https://arxiv.org/abs/2102.01044
- Код: https://github.com/tkobus/jiffy
- Это ordered map, а не fixed array, но индексами массива можно пользоваться
  как фиксированным плотным набором ключей; само это различие ещё не новизна.
- Immutable revisions содержат группы key/value entries. Новая версия группы
  включает все изменения batch, относящиеся к её области (§3.3).
- Batch revisions ссылаются на общий descriptor. Финальная версия публикуется
  после установки необходимых revisions, обеспечивая атомарную видимость.
  Это специализированный versioning/helping protocol, не вызов generic MCAS.
- §3.5: split создаёт две половины entries, merge объединяет соседние области;
  операции согласованы с обычными и пакетными изменениями.
- §3.7: два EMA по времени чтений и updates. wr=pUpdates/(pUpdates+pReads),
  target=max(50,150-200*wr); при размере вне target/2..2*target split/merge.
  Учитывается каждый update и каждое сотое чтение. При batch время распределяется
  между созданными revisions. Решение встроено в update path.
- Это локальная политика размера, не оптимизация всех cuts при фиксированном K.
- Примерно 25..300 entries — параметры Java-реализации, не универсальные размеры
  для C++ числового массива. Поэлементные значения там представлены ссылками.

Проверены §§3.1, 3.3, 3.5, 3.7 и краткий HOPC обзор. Полное доказательство,
код и поведение на наших workloads в этом проходе не проверены.

### LFCA — Winblad, Sagonas, Jonsson, SPAA 2018

- https://winsh.me/papers/spaa2018lfcatree.pdf
- §4, Adaptations: high-contention split immutable leaf container на две базы;
  low-contention join соседних контейнеров для диапазонных чтений и малого contention.
- Информация об interference обновлений и multi-base range queries влияет на stat.
- Перестройки lock-free, со специальными node states/helping. Готового generic
  atomic range update значений в рассмотренном API нет.
- Внутри persistent treap: большая база не обязательно копируется целиком при
  point update. Для flat-array chunk это различие существенно.
- В авторском PDF есть поправка: lookup lock-free, а не wait-free.

### Im-Tr-CA — Winblad, ICCSW 2017 (proceedings опубликованы в 2018)

- https://winsh.me/papers/iccsw17paper.pdf
- §§2–4: immutable treap под базами и split/join; гранулярность адаптируется
  к contention и диапазонным чтениям, есть оптимистические чтения и locks.
- Это предшественник той же идеи, а не полностью lock-free решение.

### Контроли, которые нельзя смешивать с адаптацией по workload

- KiWi: https://iditkeidar.com/wp-content/uploads/files/ftp/KiWi-TOPC2020.pdf
  Есть chunk replacement, compaction, split/merge для хранения и заполненности.
  Chunks в обычной работе изменяемые; это не тождественно immutable revisions Jiffy.
- Leaplist: https://transact2013.cse.lehigh.edu/avni.pdf
  Есть immutable entries/fat nodes и split/merge, но STM/locking-синхронизация;
  не используется как свидетельство нашего lock-free workload partitioner.
- Общие CoW/page-locality работы и dynamic resizing vectors исключены как
  прямые ответы: другой контракт и/или нет shared-memory atomic range protocol.

## Что могло бы отличать перенос dynamic

В dynamic число областей ограничено заданным K, а границы распределяются
по всему массиву с использованием workload statistics. В Jiffy/LFCA описаны
локальные split/merge и меняющееся число областей. В прочитанных политиках нет
нашего глобального фиксированного бюджета и оптимизатора cuts по гистограмме.
Это отличие от конкретных механизмов этих работ, не доказательство новизны
во всей литературе и не доказательство преимущества global optimization.

Особенно важно не ограничиваться coverage-frequency objective прошлого проекта.
Нужная функция должна учитывать, в зависимости от операции:

- число read/commit participants, включая read set для dependent writes;
- copied bytes и повторное создание кандидатов после конфликтов;
- write-write false conflicts от общей ссылки на chunk;
- snapshot/validation/history maintenance;
- overhead directory и стоимость репартиционирования.

Read-only snapshot обычно не конфликтует с записью так же, как под mutex;
поэтому прежняя «нагрузка» не обязана предсказывать полезные границы.

Проверяемая гипотеза: при сопоставимом бюджете памяти/metadata и одинаковом
контракте адаптация cuts по геометрии операций и цене копирования даёт выигрыш
над лучшим fixed chunking и локальными Jiffy/LFCA-подобными policies.

Ближайший следующий шаг — читать/использовать Jiffy как сильный baseline:
в ней уже одновременно присутствуют immutable chunks, autoscaling и atomic batches.
Для операций «читать диапазон и условно менять по прочитанному» отдельно проверить
контракт: batch предписанных put/remove не даёт автоматически транзакционную
валидацию произвольного read set.

## Поисковые запросы и пределы

Web search, 2026-10-23, английские научные/авторские источники, без нижней даты:

- Jiffy concurrent skip list adapt revision size updates range queries immutable merge split
- adaptive immutable chunks multi word compare swap array partition workload
- lock free contention adapting immutable leaf containers split join MCAS adaptive granularity
- concurrent immutable array adaptive chunk size range transactions
- immutable chunks workload partitioning array CAS
- array adaptive granularity lock-free
- copy-on-write adaptive granularity concurrency array

Не найден точный готовый fixed-size dense array с generic MCAS и оптимизатором
cuts dynamic. Отсутствие такого результата поиска не доказывает отсутствие работы.
Большой arXiv PDF Jiffy не загрузился из-за лимита размера; вместо него прочитан
авторский полный PDF опубликованной PPoPP 2022 версии по указанной ссылке.
