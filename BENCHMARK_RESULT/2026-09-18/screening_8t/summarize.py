#!/usr/bin/env python3
"""Validate all 42 measurements, rank scenarios, and render the report/gallery."""
import csv
import json
from pathlib import Path
import sys

import run

ROOT, OUT = run.ROOT, run.OUT
sys.path.insert(0, str(ROOT / "scripts"))
import plot_throughput as plotting

LABELS = {
    "islands_range": "Четыре островка двумя парами",
    "gradient_range": "Плавная концентрация к середине",
    "unequal": "Четыре окна: 70/10/10/10",
    "unequal_widths_range": "Окна разных размеров: 40/30/20/10",
    "clusters_2": "Два горячих окна, точечные запросы",
    "steady_range": "Постоянное горячее окно",
    "moving_variable_range": "Переезд окна, длины 1…10 000",
    "exchange_range": "Два окна меняются долями 90/10",
    "shift": "Переезд окна, точечные запросы",
    "clusters_8": "Восемь горячих окон, точечные запросы",
    "clusters_2_range": "Два горячих окна, длинные запросы",
    "clusters_4_range": "Четыре горячих окна, длинные запросы",
    "uniform": "Равномерная нагрузка, точечные запросы",
    "hot_with_spike": "Горячее окно и 30% в одну ячейку",
}


def main():
    manifest = json.loads((OUT / "runs.json").read_text())
    assert len(manifest) == 14 and all(r["status"] == "complete" for r in manifest)
    groups = plotting.read_inputs([
        f"{LABELS[name]}={OUT / (name + '.csv')}" for name, _, _ in run.CASES
    ])
    assert len(groups) == 14 and sum(len(g["rows"]) for g in groups) == 42
    for group in groups:
        assert len(group["matched"]) == 1
        assert all(r["threads"] == 8 for r in group["rows"])
    groups.sort(key=lambda g: min(g["matched"][1][i]["qps"] /
                g["matched"][1]["fixed"]["qps"] for i in ("balanced", "dynamic_dp")), reverse=True)
    plots = OUT / "plots"
    plots.mkdir(exist_ok=True)
    plotting.plot(groups, plots)
    plotting.summary(groups, plots)

    report = [
        "# Все 14 сценариев на 8 потоках — 18 сентября 2026",
        "",
        "Первичный отбор: один повтор каждой из трёх реализаций, 42 последовательных запуска. "
        "Ускорение = wall_s(fixed) / wall_s(варианта) при одинаковом числе запросов и одинаковых входных трассах. "
        "Это одиночные измерения, а не оценка статистической значимости. "
        "Порядок сценариев ниже — по меньшему из двух ускорений balanced и DP.",
        "",
        "8 рабочих потоков; 64 блокировки; статистика примерно на каждом 64-м запросе; "
        "проверка необходимости перестройки раз в 30 с. Адаптивная реализация дополнительно имеет фоновый поток. "
        "В wall_s входят холодный старт, сбор статистики, перестройки и завершение. "
        "Перед серией прошли 6/6 CTest; после каждого замера проверен весь итоговый массив: 42/42. "
        "Генерация трасс, обнуление данных и проверка результата вынесены за таймер одинаково для всех реализаций.",
        "",
        "Каждый запрос читает весь диапазон и изменяет одну ячейку внутри него. "
        "Лимит хранимой трассы — 2²⁰ запросов на фазу суммарно по потокам; трасса повторяется циклически. "
        "Фазы переключаются после заданного числа операций, а не по времени. "
        "В shift, exchange_range и moving_variable_range остаётся по одной реальной смене нагрузки, "
        "как в предыдущих измерениях. Несколько смен в одном запуске здесь ещё не проверяются.",
        "",
        "| Сценарий | fixed, млн/с | balanced, млн/с | DP, млн/с | balanced / fixed | DP / fixed |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for group in groups:
        p = group["matched"][1]
        report.append(f"| `{p['fixed']['scenario']}` | {p['fixed']['qps']/1e6:.3f} | "
                      f"{p['balanced']['qps']/1e6:.3f} | {p['dynamic_dp']['qps']/1e6:.3f} | "
                      f"{p['balanced']['qps']/p['fixed']['qps']:.2f}× | "
                      f"{p['dynamic_dp']['qps']/p['fixed']['qps']:.2f}× |")
    report += ["", "## Параметры и стоимость", "",
               "В каждой ячейке с тройкой порядок: fixed / balanced / DP. CPU — суммарное процессорное время. "
               "Перестройки — только опубликованные новые разбиения; проверка раз в 30 с может не привести к перестройке. "
               "rebuild_ms не заменяет wall_s и не вычитается из него.", "",
               "| Сценарий | N | Ширина H | Длина запроса | Запросов, млн | wall, с | CPU, с | Перестройки | rebuild, мс |",
               "|---|---:|---:|---:|---:|---|---|---|---|"]
    for group in groups:
        p = group["matched"][1]
        r = p["fixed"]
        name = r["scenario"]
        length = f"1…{r['range_length']}" if name == "moving_variable_range" else (
            str(r["range_length"]) if name.endswith("_range") else "1")
        triples = [" / ".join(f"{float(p[i][key]):.{precision}f}" for i in plotting.IMPL)
                   for key, precision in (("wall_s", 2), ("cpu_s", 2), ("rebuilds", 0), ("rebuild_ms", 3))]
        report.append(f"| `{name}` | {r['array_size']} | {r['hot_width']} | {length} | "
                      f"{r['operations']/1e6:g} | " + " | ".join(triples) + " |")
    report += ["", "H — базовая ширина окна: у islands_range каждый островок H/16; "
               "у unequal_widths_range ширины H/8, H/4, H/2, H; для uniform H не используется.", "",
               "Число операций сохранено из предыдущих отдельных замеров, кроме uniform (32 млн → 5 млрд, "
               "чтобы измерить длительную равномерную нагрузку) и moving_variable_range (192 → 160 млн, "
               "чтобы оставить запас до трёх минут). Число операций одинаковое внутри каждого сравнения. "
               "Это не прогоны фиксированной длительности: время определяется реализацией.", "",
               "[Галерея графиков](GRAPHS.md) · [Все численные результаты](plots/summary.csv) · "
               "[Команды и статусы](runs.json) · [Методика и хеши исходников](metadata.json)", "",
               "Повтор запуска: `python3 BENCHMARK_RESULT/2026-09-18/screening_8t/run.py` "
               "(скрипт отказывается перезаписывать существующие измерения). "
               "Повтор графиков: `MPLCONFIGDIR=build/mpl-cache XDG_CACHE_HOME=build/xdg-cache "
               "build/plot-venv/bin/python BENCHMARK_RESULT/2026-09-18/screening_8t/summarize.py`.", ""]
    (OUT / "README.md").write_text("\n".join(report))
    gallery = []
    for page in range(0, len(groups), 6):
        gallery += [f"![Запросы в секунду, страница {page//6+1}](plots/throughput_{page//6+1:02d}.png)", "",
                    "8 потоков, один повтор. Запросы/с за весь запуск, включая холодное начало и перестройки. "
                    + "; ".join(g["label"] for g in groups[page:page+6]) + ".", ""]
    for index, group in enumerate(groups, 1):
        r = group["rows"][0]
        if len(r["phases"]) > 1:
            gallery += [f"![{group['label']}](plots/phases_{index:02d}_{r['scenario']}.png)", "",
                        group["label"] + ". Запросы/с по фазам; длительности фаз у реализаций различаются. "
                        "Это средняя скорость фазы, а не мгновенная скорость по времени.", ""]
    (OUT / "GRAPHS.md").write_text("\n".join(gallery))
    print("\n".join(report))


if __name__ == "__main__":
    main()
