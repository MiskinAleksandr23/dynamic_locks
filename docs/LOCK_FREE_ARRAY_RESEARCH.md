# Фиксированный массив и lock-free диапазонные операции

Дата: 23 октября 2026. Поисковый обзор, не систематический обзор и не доказательство новизны.
Работа выполнена без запуска субагентов. Код проекта не изменялся, новых benchmark runs нет.

## Вопрос и границы поиска

Фиксированный логический массив A[0..N), shared-memory многопоточность.
Нас интересуют операции с линейризуемым результатом:

1. read(i), write(i,v), CAS(i,old,new).
2. scan(l,r): весь результат соответствует одному моменту времени.
3. writeRange(l,r,values), transformRange(l,r,f): единое изменение диапазона.
4. transaction(l,r,f): чтение диапазона и изменение данных, зависящее от прочитанного.

Динамические vectors, очереди и ring buffers сами по себе не отвечают этому вопросу.
Чтение каждого элемента атомарно не делает весь scan атомарным.
Snapshot из point writes не обеспечивает atomic multi-element writes.
Lock-free означает системный прогресс, wait-free — завершение каждой операции
за конечное число собственных шагов; ни одно свойство не означает ускорение.

Искали первичные статьи, авторские тексты, официальные спецификации;
англоязычные работы без ограничения по нижней дате, включая находки до 2026 года.
Основные запросы:

| Запрос | Найденная линия |
|---|---|
| wait free atomic snapshots shared memory arrays Afek 1993 snapshot object | Atomic snapshot memory |
| partial snapshot wait-free array implementation | Partial snapshots, SPAA 2008 |
| constant time snapshots applications concurrent data structures range updates PPoPP 2021 | Versioned CAS |
| efficient multi word compare and swap 2020 | MCAS, DISC 2020 |
| lock-free range updates array | MCAS/STM и учебные segment-tree проекты |
| multi-word compare-and-swap 2024 2025 implementation | Big Atomics 2025; MCAS preprint 2026 |

## Проверенные основные источники

### Atomic Snapshots of Shared Memory — Afek et al., PODC 1990 / JACM 1993

- Авторский PDF proceedings: https://groups.csail.mit.edu/tds/papers/Shavit/PODC90.pdf
- Архив MIT: https://dspace.mit.edu/entities/publication/355717a8-388a-46a4-afc6-800d0d1522b8
- JACM DOI: 10.1145/153724.153741.
- Уже рассматривается массив регистров с update одного компонента и единым scan.
- Есть single-writer и multi-writer wait-free конструкции.
- Это доказательство существования и основа области, не готовый быстрый range-update массив.

### Partial Snapshot Objects — Attiya, Guerraoui, Ruppert, SPAA 2008

- Полный авторский PDF: https://hagit.net.technion.ac.il/files/2015/09/AGR08.pdf
- Multi-writer vector: update(i,v), scan произвольного подмножества индексов.
- Диапазон — частный случай подмножества.
- Линейризуемые wait-free реализации; announcements, active set, helping.
- §§1–4: цены помощи и сканирования зависят от конкуренции и размера запросов;
  не обещать бесплатную point write или O(k) scan во всех режимах.
- Авторы уже исследуют адаптивность к числу активных процессов. Это не то же
  самое, что перестройка физических блоков по распределению обращений.

### Efficient Partial Snapshot Implementations — Kallimanis et al., preprint 2020

- https://arxiv.org/abs/2006.06048
- Проверены abstract и описание контракта, не полный алгоритм.
- lambda-Snap: ограниченное число одновременно активных scanner processes,
  O(lambda) update, O(lambda*m) full scan, O(lambda*m) space.
- Есть partial variant. Не переносить full-scan формулу автоматически на partial scan.

### Constant-Time Snapshots with Applications to Concurrent Data Structures — PPoPP 2021

- https://www.cs.cmu.edu/~blelloch/papers/3437801.3441602.pdf
- https://arxiv.org/abs/2007.02372
- DOI 10.1145/3437801.3441602.
- Camera + versioned CAS objects: takeSnapshot возвращает handle за O(1),
  readVersion читает значение объекта на соответствующий момент.
- O(1) относится к получению handle, а не выдаче N или k элементов.
- Чтение старой версии зависит от числа последующих успешных изменений
  соответствующего объекта; нужны история и её освобождение.
- §3 применим к array of vCAS cells; это наше применение общего механизма,
  а не утверждение, что авторы измеряли именно наш массив.
- §7: эксперименты на деревьях, в том числе сравнение с LFCA; не выдавать
  результаты этих деревьев за ускорение фиксированного массива.
- Не предоставляет автоматически atomic range writes: последовательные vCAS
  остаются несколькими операциями; читатель может получить их промежуточное состояние.

### Efficient Multi-Word Compare and Swap — Guerraoui et al., DISC 2020

- https://drops.dagstuhl.de/entities/document/10.4230/LIPIcs.DISC.2020.4
- https://drops.dagstuhl.de/storage/00lipics/lipics-vol179-disc2020/LIPIcs.DISC.2020.4/LIPIcs.DISC.2020.4.pdf
- https://arxiv.org/abs/2008.02527
- Lock-free MCAS: набор (address,expected,desired) меняется атомарно.
- §4: descriptors, logical ownership/acquisition, единый status, helping.
  Читатели должны использовать специальный read, интерпретирующий descriptors.
- k+1 CAS в обычном бесконфликтном случае для k адресов; не worst-case bound
  под конкуренцией и не стоимость всей прикладной транзакции.
- Это непосредственно даёт строительный блок для записи нескольких ячеек
  либо ссылок на immutable chunks. Последний вариант — предлагаемая конструкция.
- Для read-dependent update весь read set должен участвовать в проверке commit,
  включая адреса/версии, для которых desired=expected, либо нужен эквивалентный
  транзакционный протокол. Один CAS только изменяемой ячейки недостаточен.

### Big Atomics — Anderson, Blelloch, Jayanti, preprint 2025

- https://arxiv.org/abs/2501.07503
- https://arxiv.org/html/2501.07503v1
- Load/store/CAS целого k-word объекта, lock-free конструкции и экспериментальная оценка.
- Применимо как кандидат для фиксированных крупных блоков.
- Один такой объект не равен произвольному перекрывающемуся range: атомарность
  нескольких Big Atomics требует отдельного протокола.
- Здесь проверен preprint; публикационные названия других версий не смешиваются.

### Contention-aware helping для MCAS — Unno et al., preprint July 2026

- https://arxiv.org/abs/2607.06034
- Проверен abstract, не доказательство и не весь код.
- Динамическое ограничение помощи, backoff, version embedding.
- Найден как актуальная работа для дальнейшего чтения; заявления об ABA и
  ускорении не принимаются как собственная проверка этих свойств.

### Практические примитивы

- C++ draft: https://eel.is/c++draft/atomics.lockfree
- Java: https://docs.oracle.com/javase/8/docs/api/java/util/concurrent/atomic/package-summary.html
- Array of atomic cells даёт атомарность point operations. Для C++ проверить
  is_always_lock_free/is_lock_free целевого типа: atomic<T> не универсальная
  гарантия lock-freedom для любых T.

## Что это означает для dynamic

Существующий DynamicLock защищает внешние данные и выполняет callback под locks.
Это не тот же контракт, что snapshot array. Lock-free вариант, скорее всего,
должен владеть представлением массива и предоставлять явные операции/транзакции.
Нельзя сохранить произвольные записи callback в захваченный внешний vector,
просто заменив mutex на CAS.

Варианты для проверки:

| Конструкция | Point write | Atomic scan | Atomic multi-write | Основная цена |
|---|---|---|---|---|
| Array of atomic cells | Да | Нет, если просто цикл | Нет | слабый range contract |
| Partial snapshots / vCAS | Да | Да | сам механизм не даёт | помощь либо история версий |
| Один immutable root + CAS | Да | Да | Да при публикации всей операции одним root CAS | общий hotspot, копирование |
| MCAS над ячейками | Да | отдельный согласованный read protocol | Да | число затронутых адресов, descriptors/helping |
| MCAS над immutable chunks | Да | отдельный snapshot/validated collection | Да | число chunks против объёма копирования |

Единый root может ссылаться на persistent vector/tree, а не обязательно полную
копию массива. Но любой writer всё равно публикуется через один root.
Фиксированный логический размер не требует плоского физического представления.

### Гипотеза продолжения: адаптивная гранулярность chunks

Immutable chunks покрывают фиксированное индексное пространство. Операция
получает текущие roots, готовит замену и публикует её через MCAS по необходимым
chunks. Чистые читатели получают согласованный набор roots, затем сканируют
immutable данные. Конкретный read protocol ещё надо выбрать и доказать.

Мелкие chunks уменьшают ложные конфликты и копирование при point writes;
крупные сокращают число MCAS-участников у больших multi-write/read-dependent
операций. Read-only scan не обязан писать descriptors каждого chunk: нужен
отдельный хороший baseline для чтений.

Предмет адаптации: границы/размеры chunks по footprint запросов, фактическим
CAS retries, read/write set, copied bytes и работе helpers. Не просто частоты.
Однако generic lock-free adaptive granularity уже покрывает LFCA; текущая
формулировка не устанавливает глобальную научную новизну.

Репартиционирование — отдельная операция над directory/chunks. Безопасная
публикация должна согласовать concurrent point/range updates, исключить
возврат retired chunks, сохранить покрытие индексов и lifetime старых версий.
Фоновый mutex-based rebuild, обязательный для завершения запросов, нарушит
заявление о lock-free всей структуры. Нужны helping/certified publication либо
обход необязательной незавершённой перестройки; это ещё не готовый протокол.

Callback для speculative execution должен быть повторяемым и не иметь внешних
эффектов. Для helping нужны ограниченные описания операций, а не произвольный
пользовательский код, который может ждать mutex, делать I/O или останавливаться.
Memory allocation/reclamation и удержание старых версий входят в стоимость
и в область проверки progress guarantees.

## Первый эксперимент после выбора контракта

Сначала fixed chunks, без online adaptation. Сравнить существующие locks с
одним immutable root, per-cell MCAS и chunk MCAS при одинаковой атомарности.
Отдельно point write + snapshot read и транзакционные range operations.
Важно: текущий benchmark читает весь диапазон, но меняет одну ячейку; сравнение
с point atomic increment + несогласованным scan ослабляет его контракт.

Варьировать N, threads, range sizes, read/write ratio, skew и движущийся hotspot;
измерять throughput, tail latency, copied bytes, allocations, retries и помощь.
Сначала подобрать лучший fixed chunk size по режимам. Если он существенно
меняется и разница заметна, проверять adaptive chunks с включённой ценой миграции.
Это необходимая проверка полезности, не самостоятельная заявка на новизну.

## Что пока не установлено

- Не найден и не проверен готовый промышленный контейнер с точным API dynamic.
- Не доказано преимущество lock-free над текущими mutex/spin/RW вариантами.
- Не построен и не проверен online chunk migration protocol.
- Не проведён полный поиск STM, range assignment/lazy segment-tree algorithms.
- Учебный проект https://www.andrew.cmu.edu/user/nsodhi/segtrees.html найден,
  но корректность и linearizability его range операций не проверены;
  как доказательство существования надёжного алгоритма он не используется.
