# Genetic: заполнение 4/6/8/10 потоков

Активная задача пользователя: genetic измерен только на 8 потоках; выполнить остальные 4/6/10 и заполнить GENETIC_COMPARISON.md и BENCHMARK_SHORT.md. Сохраняется прежнее указание не перезапускать dynamic/DP. Алгоритм остаётся тем же гибридом квантильного старта и эволюционного поиска.

Запущена последовательная цепочка (exec session 85310). Она включает 48 новых genetic и два недостающих fixed для sliding на 6/10 потоках. Остальные fixed, Balanced, DP, global и element повторно не запускаются. 8-поточные результаты полностью переиспользуются.

Порядок и команды:

```sh
python3 scripts/genetic_compare.py --profile full --threads 4 --implementations genetic --output BENCHMARK_RESULT/2026-09-22/full_4t
python3 scripts/genetic_compare.py --profile full --threads 4 --implementations genetic --scenarios sliding_variable_range --rebuild-ms 10000 --output BENCHMARK_RESULT/2026-09-22/sliding_genetic_10s_4t
python3 scripts/genetic_compare.py --profile full --threads 6 --implementations genetic --output BENCHMARK_RESULT/2026-09-22/full_6t
python3 scripts/genetic_compare.py --profile full --threads 6 --implementations genetic --scenarios sliding_variable_range --rebuild-ms 10000 --output BENCHMARK_RESULT/2026-09-22/sliding_genetic_10s_6t
python3 scripts/genetic_compare.py --profile full --threads 10 --implementations genetic --output BENCHMARK_RESULT/2026-09-22/full_10t
python3 scripts/genetic_compare.py --profile full --threads 10 --implementations genetic --scenarios sliding_variable_range --rebuild-ms 10000 --output BENCHMARK_RESULT/2026-09-22/sliding_genetic_10s_10t
python3 scripts/genetic_compare.py --profile full --threads 6 --implementations fixed --scenarios sliding_variable_range --rebuild-ms 10000 --output BENCHMARK_RESULT/2026-09-22/sliding_fixed_10s_6t
python3 scripts/genetic_compare.py --profile full --threads 10 --implementations fixed --scenarios sliding_variable_range --rebuild-ms 10000 --output BENCHMARK_RESULT/2026-09-22/sliding_fixed_10s_10t
```

Не запускать benchmark-процессы параллельно. Статусы внутри manifest.json соответствующих каталогов, строки CSV пишутся после завершения измерения. Исходники и бинарник перед запуском проверены: хеши совпадают с исходной проверенной серией full_8t. Пересборка/изменение алгоритма не требуется.

Объёмы — CASES в scripts/genetic_compare.py, полный профиль. Интервал 30 с в full_Nt, 10 с в дополнительных sliding. В общей BENCHMARK_SHORT.md sliding использует 10 с как в прежней таблице; отдельный genetic должен показывать оба интервала.

Базы fixed:
- 8 потоков: свежая серия fixed_full_8t.
- 4/6/10, кроме sliding: BENCHMARK_RESULT/2026-09-18/scaling_4_6_8_10/{threads}t/{scenario}.csv.
- sliding 4: BENCHMARK_RESULT/2026-09-18/sliding_10s_4_6_10/4t.csv.
- sliding 6/10: новые два контрольных прогона выше. Fixed не зависит от интервала перестройки.

Обязательно обозначить разные серии fixed; не выдавать историческое сравнение за свежий парный замер. Отсутствующие старые DP/Balanced/global/element не дополнять вымышленными числами. В отчёте genetic заполнить времена/запросы в секунду для всех 64 сочетаний (16 сценариев/интервалов × 4 числа потоков), с прямыми ссылками на источники.

После завершения: проверить статусы 50 новых прогонов, verified=true, параметры и неизменность хешей; пересобрать обе Markdown-таблицы из CSV. До получения всех результатов задачу не считать завершённой.

## Последнее состояние

**Завершено.** Все 48 новых genetic и 2 контрольных fixed прошли проверку. Вместе с 16 ранее полученными 8-поточными результатами заполнены все 64 сочетания сценария/интервала/числа потоков.

Сводка и обе таблицы пересобраны. Проверены 64 пары время/отношение, 300 значений общей таблицы, ссылки и неизменность исходников/бинарника. Аудит — validation.json. Бенчмарки больше не выполняются; повторять их не требуется.

Команда сводки: `python3 scripts/summarize_genetic_scaling.py BENCHMARK_RESULT/2026-09-22`.
