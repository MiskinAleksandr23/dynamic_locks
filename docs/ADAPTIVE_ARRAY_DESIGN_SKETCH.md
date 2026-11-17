# Адаптивный immutable-массив: проверяемая конструкция

Это проектная гипотеза и sketch корректности, не реализованный алгоритм,
не завершённое доказательство lock-freedom и не утверждение новизны.
Продолжает ADAPTIVE_IMMUTABLE_CHUNKS_RELATED_WORK.md.

## Почему fixed array — содержательное ограничение

Ordered map можно использовать для значений по индексам [0,N), однако плотное
фиксированное пространство позволяет не хранить ключ каждого элемента и
пользоваться offset addressing внутри chunk. Insert/delete и сдвиг рангов
не нужны. Цена: фиксированная область, неэффективность для очень разреженных
keys, необходимость directory при переменных границах.
Массив не обязан быть одной физически непрерывной аллокацией.

Первый контракт: read(i), write(i,v), readRange(l,r), writeRange(l,r,values).
Следующий: repeatable bounded pure read-modify-write над диапазоном.
Batch заданных put/remove из Jiffy сам по себе не гарантирует такой read-set
validation; сравнивать первый и второй контракты отдельно.

## Представление

- Atomic directory root D ссылается на immutable ordered list cuts/handles.
- Каждый live chunk handle H имеет неизменяемые bounds [a,b).
- Атомарное H.state — либо Active(V), либо Retired.
- V ссылается на immutable массив длины b-a. Каждая изменяющая публикация
  получает уникальный объект версии; возврат к старому V запрещён.
- Новая версия сохраняет H и bounds. Новые bounds требуют новых handles.
- Все разделяемые root/state обращения используют MCAS-aware read protocol.
- Lifetime protection предотвращает освобождение/reuse видимых readers объектов.

Первый directory — sorted cuts, lookup O(log K), обход соседних descriptors
после первого поиска. Это простой linear representation без дерева ключей.
Dense coarse lookup table — возможная последующая оптимизация с отдельной
оценкой памяти, а не обещание бесплатной прямой адресации при произвольных cuts.

## Обновление при фиксированном разбиении

1. Прочитать D; найти затронутые handles.
2. Получить Active(V) для каждого. При Retired начать заново с D.
3. Подготовить новые V для изменяемых chunks вне shared state.
4. Выполнить MCAS по H.state ожидаемым версиям и новым Active(V').
5. Для read-dependent update включить read-only chunks в validation commit.

Root D не участвует в обычном commit. Это не один глобальный CAS на каждую
запись; updates непересекающихся handles имеют непересекающиеся write sets.
Они всё равно могут конкурировать за allocator/cache/reclamation metadata.

Если snapshot/read-dependent candidate был собран несогласованно, successful
MCAS всех зависимостей подтверждает наличие ожидаемого набора версий в момент
commit. Один CAS изменяемого chunk этого не подтверждает.
Пользовательский callback не исполняется на живых внешних данных и не может
производить необратимые побочные эффекты до commit.

## Перемещение границы между двумя chunks

Старые handles A:[a,b), B:[b,c), ожидаемые state SA,SB.
Выбрали x. Создать A':[a,x), B':[x,c) с теми же логическими значениями.
Подготовить новый directory D', заменив A,B на A',B', прочие handles переиспользовать.

В одном MCAS:

    A.state : SA -> Retired
    B.state : SB -> Retired
    directoryRoot : D -> D'

Active-state новых handles полностью инициализирован до публикации.
Другие границы аналогично меняются посредством включения всех old handles,
чьё ownership меняется. Совместный m-chunk rebuild касается m+1 MCAS words,
но подготовка/копирование может быть намного дороже этой публикации.

Почему это согласуется с обычными записями:

- Update успел первым: ожидаемое SA/SB больше не совпадает; rebuild должен
  обновить candidate, иначе он теряет запись. Подготовленные copies не публикуются.
- Rebuild успел первым: old handle Retired; update на старую версию не проходит.
- Update использует старый D, но затрагивает только неизменённые handles:
  он может завершиться. Глобальная смена D не должна напрасно отменять его.
- Старый directory, пересекающий изменившуюся область, содержит retired handles;
  операция обязана проверить их, а не читать старые данные как текущие.

Последний пункт запрещает обычный pointer-only scan старого D без validation.
Также нельзя раздельно сначала publish D', а позднее retired old handles.
MCAS logical read должен интерпретировать pending descriptor и при необходимости
помогать, иначе atomic visibility этих трёх изменений потеряется.

Root сериализует публикации rebuilds, но не обычные writes. Для редкой адаптации
это приемлемый первый вариант; частые parallel rebuilds потребуют иной directory.

## Согласованное range read

Концептуально: используя один D, собрать Active(V) нужных handles и затем
повторно собрать те же state. Если векторы совпадают и Active, immutable roots
дают согласованный снимок на границе между двумя scans. Требуются уникальные
versions/no ABA, корректные atomic reads, проверенное покрытие и lifetime.
После проверки можно обходить immutable data без дальнейшей validation.

Это double-collect с retry, не wait-free алгоритм. При фиксированном partition
неудачи означают concurrent updates; при миграциях нужно отдельно анализировать
прогресс клиентских операций, а не только факт прогресса фонового rebuild.
Альтернатива — MCAS validation/fallback с helping либо versioned snapshots.
Сильная read-only оптимизация не должна ослаблять обещанный progress guarantee.

## Что переносится из dynamic, а что меняется

Переносится: bins, sampling, фиксированный бюджет K, редкая оптимизация cuts,
ограничение частоты перестроек и включение их цены в итоговые измерения.

Не переносится автоматически: coverage -> sum(group load)^2 как objective.
Новая стоимость включает число commit/read participants, copied bytes, wasted
copies при retry, true/false write conflicts, snapshot/reclamation overhead,
directory lookup и миграцию. Не каждый point-hot read требует мелкого chunk.

Candidate migration имеет реальные расходы: в отличие от lock-only проекта,
границы уже являются границами физического представления данных.

## Сравнение и приложения

Сначала сравнить на одном substrate: best fixed equal chunks, fixed weighted
cuts, локальная split/join политика и оптимизация K cuts. Затем внешний baseline
Jiffy на плотных ключах и immutable-root вариант; LFCA только на общем API.
Проверить, остаётся ли выигрыш от выбора границ после изоляции эффекта offset
addressing и отсутствия key/routing overhead.

БД: особенно естественны column segment по внутреннему row ID, массив payloads
за primary/secondary index, validity/visibility bitmaps. Это кандидаты применения,
не заявление, что такие движки уже используют предлагаемый протокол.
Диапазон внешних keys не равен диапазону row IDs, если данные не кластеризованы
по этому ключу. Согласованность одной колонки не равна транзакции по всей БД.

Пример реального array representation — DuckDB execution vectors:
https://duckdb.org/internals/vector
Это подтверждает практичность массивов, но не потребность DuckDB в данном
протоколе concurrent updates. Vector execution и storage transaction protocol
нельзя смешивать.

Перед implementation нужны invariants и model checking пересечений write,
range read, local reshape; затем stress/linearizability tests с остановкой
потока на этапах публикации и безопасным memory reclamation.
