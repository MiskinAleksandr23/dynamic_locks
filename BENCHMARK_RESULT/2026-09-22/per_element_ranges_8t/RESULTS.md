**8 потоков, по 1 млн запросов, один повтор.** Массив 2²⁶, окно 2²², длина 10000; в moving_variable_range — 1…10000.

| Тест | Fixed, с | Element, с | Element, запросов/с | Element / fixed |
|---|---:|---:|---:|---:|
| `steady_range` | 1,21 | 14,18 | 70534 | 0,09 |
| `clusters_2_range` | 0,94 | 13,87 | 72091 | 0,07 |
| `moving_variable_range` | 0,97 | 8,80 | 113585 | 0,11 |

Проверки итоговых массивов: **OK**.

[CSV](summary.csv) · [Команды](manifest.json)
