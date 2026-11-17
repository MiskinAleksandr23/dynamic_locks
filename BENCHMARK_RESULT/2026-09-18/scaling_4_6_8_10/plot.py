#!/usr/bin/env python3
"""Render 14 throughput-vs-workers figures using new 4/6/10 and prior 8-worker runs."""
import csv
import hashlib
import json
from pathlib import Path
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator

import run

ROOT, OUT, BASE = run.ROOT, run.OUT, run.BASE
sys.path.insert(0, str(ROOT / "scripts"))
import plot_throughput as validated

COUNTS = (4, 6, 8, 10)
TITLES = {
    "islands_range": "Четыре островка двумя парами",
    "gradient_range": "Плотность растёт к середине окна",
    "unequal": "Четыре окна: 70/10/10/10",
    "unequal_widths_range": "Окна разных размеров: 40/30/20/10",
    "clusters_2": "Два окна, точечные запросы",
    "steady_range": "Постоянное окно, длинные запросы",
    "moving_variable_range": "Переезд окна, случайные длины запросов",
    "exchange_range": "Два окна меняются долями 90/10 → 10/90",
    "shift": "Переезд окна, точечные запросы",
    "clusters_8": "Восемь окон, точечные запросы",
    "clusters_2_range": "Два окна, длинные запросы",
    "clusters_4_range": "Четыре окна, длинные запросы",
    "uniform": "Равномерные точечные запросы",
    "hot_with_spike": "Горячее окно и 30% запросов в одну ячейку",
}
DESCRIPTIONS = {
    "islands_range": "Четыре островка шириной H/16, равные доли трафика; две пары занимают два блока fixed.",
    "gradient_range": "Одно окно шириной H, треугольная плотность начала диапазона с максимумом в середине.",
    "unequal": "Четыре разнесённых окна шириной H, доли запросов 70/10/10/10.",
    "unequal_widths_range": "Четыре окна шириной H/8, H/4, H/2, H; доли запросов 40/30/20/10.",
    "clusters_2": "Два окна шириной H на концах массива; доли 50/50, два разных блока fixed.",
    "steady_range": "Все запросы в постоянное окно шириной H=1/16 массива, занимающее четыре блока fixed.",
    "moving_variable_range": "Окно шириной H переезжает слева направо после половины запросов; длина равномерно от 1 до 10 000.",
    "exchange_range": "Два неподвижных окна шириной H; после половины запросов доли меняются с 90/10 на 10/90.",
    "shift": "Окно шириной H переезжает слева направо после половины запросов.",
    "clusters_8": "Восемь разнесённых окон шириной H, равные доли трафика; восемь разных блоков fixed.",
    "clusters_2_range": "Два окна шириной H, трафик 50/50; заняты восемь блоков fixed.",
    "clusters_4_range": "Четыре окна шириной H, равные доли трафика; заняты 16 блоков fixed.",
    "uniform": "Точечные запросы равномерно по всему массиву; горячих окон нет.",
    "hot_with_spike": "70% точечных запросов равномерно внутри окна шириной H, 30% в его центральную ячейку.",
}


def main():
    metadata = json.loads((OUT / "metadata.json").read_text())
    records = json.loads((OUT / "runs.json").read_text())
    if len(records) != 42 or any(r["status"] != "complete" for r in records):
        raise SystemExit("Wait until all 42 matched sets have completed")
    baseline_metadata = json.loads((BASE / "metadata.json").read_text())
    if metadata["source_sha256"] != baseline_metadata["source_sha256"]:
        raise SystemExit("Reference source/binary mismatch")
    for filename, digest in metadata["reference_csv_sha256"].items():
        if hashlib.sha256((BASE / filename).read_bytes()).hexdigest() != digest:
            raise SystemExit("Reference CSV changed")
    scenarios = list(dict.fromkeys(r["scenario"] for r in records))
    plots = OUT / "plots"
    plots.mkdir(exist_ok=True)
    all_rows, gallery = [], []
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11, "svg.fonttype": "none"})
    for index, scenario in enumerate(scenarios, 1):
        paths = {n: (BASE if n == 8 else OUT / f"{n}t") / f"{scenario}.csv" for n in COUNTS}
        groups = validated.read_inputs([f"{n}t={path}" for n, path in paths.items()])
        if len(groups) != 4 or any(len(g["matched"]) != 1 for g in groups):
            raise ValueError("Expected four single-repetition matched sets")
        by_count = {g["rows"][0]["threads"]: g["matched"][1] for g in groups}
        if set(by_count) != set(COUNTS):
            raise ValueError("Missing or duplicate worker counts")
        common = tuple(k for k in validated.MATCH if k not in ("threads", "unique_queries"))
        if len({tuple(r[k] for k in common) for g in groups for r in g["rows"]}) != 1:
            raise ValueError("Settings differ across worker counts")
        base = by_count[8]["fixed"]
        length = "1…10 000" if scenario == "moving_variable_range" else (
            f"{base['range_length']:,}" if scenario.endswith("_range") else "1")
        hot = "весь массив" if scenario == "uniform" else f"2^{base['hot_width'].bit_length()-1}"
        setup = (f"N=2^{base['array_size'].bit_length()-1}; H={hot}; L={length}; "
                 f"{base['operations']/1e6:g} млн запросов на каждую точку")
        fig, ax = plt.subplots(figsize=(11.5, 7.8))
        for implementation, label, color in zip(validated.IMPL, validated.NAMES, validated.COLORS):
            values = [by_count[n][implementation]["qps"] / 1e6 for n in COUNTS]
            ax.plot(COUNTS, values, "o-", color=color, linewidth=2.2, markersize=7, label=label)
        ax.set_xticks(COUNTS)
        ax.set_xlim(3.6, 10.4)
        ax.set_ylim(bottom=0)
        ax.yaxis.set_major_locator(MaxNLocator(nbins=6))
        ax.set_xlabel("Рабочие потоки")
        ax.set_ylabel("Млн запросов / с за весь запуск")
        ax.set_title(f"{TITLES[scenario]}\n{scenario}\n{setup}", fontsize=12, pad=18)
        ax.grid(color="#e2e8f0", linewidth=.8)
        ax.set_axisbelow(True)
        ax.spines[["top", "right"]].set_visible(False)
        ax.legend(frameon=False, loc="best")
        rebuild_lines = []
        for implementation, label in (("balanced", "balanced"), ("dynamic_dp", "DP")):
            rebuild_lines.append(label + ": " + " / ".join(str(by_count[n][implementation]["rebuilds"]) for n in COUNTS))
        fig.text(.5, .125, "Перестройки при 4 / 6 / 8 / 10 потоках — " + "; ".join(rebuild_lines),
                 ha="center", fontsize=10)
        fig.text(.5, .035,
                 "64 блокировки; выборка 1/64; проверка перестройки раз в 30 с. Холодный старт и перестройки включены.\n"
                 "Один повтор на точку. 8 потоков — предыдущая серия; 4, 6, 10 — новая. Линии соединяют измерения.\n"
                 "Объём работы фиксирован; длительность и доля времени до первой перестройки различаются.",
                 ha="center", fontsize=9.5)
        fig.tight_layout(rect=(0, .18, 1, 1))
        stem = f"{index:02d}_{scenario}"
        for extension in ("png", "svg"):
            fig.savefig(plots / f"{stem}.{extension}", dpi=180, facecolor="white")
        plt.close(fig)
        gallery += [f"![{TITLES[scenario]}](plots/{stem}.png)", "", DESCRIPTIONS[scenario], ""]
        for n in COUNTS:
            for implementation in validated.IMPL:
                row = by_count[n][implementation]
                all_rows.append({"scenario": scenario, "threads": n, "implementation": implementation,
                                 "qps": row["qps"], "speedup": row["qps"]/by_count[n]["fixed"]["qps"],
                                 "operations": row["operations"], "wall_s": row["wall_s"],
                                 "cpu_s": row["cpu_s"], "rebuilds": row["rebuilds"],
                                 "rebuild_ms": row["rebuild_ms"], "source": str(paths[n].relative_to(ROOT))})
    if len(all_rows) != 168:
        raise ValueError("Expected 168 verified measurements")
    with (OUT / "summary.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(all_rows[0]))
        writer.writeheader()
        writer.writerows(all_rows)
    (OUT / "GRAPHS.md").write_text("\n".join(gallery))
    (OUT / "README.md").write_text(
        "# 14 сценариев на 4, 6, 8 и 10 потоках\n\n"
        "[Все графики с подписями](GRAPHS.md). По одному графику на сценарий: запросы/с в зависимости от числа рабочих потоков. "
        "Три линии — fixed, balanced и dynamic DP. Все оси скорости начинаются с нуля.\n\n"
        "126 новых измерений на 4/6/10 потоках и 42 измерения из предыдущей серии на 8 потоках. "
        "Один повтор на каждую точку. Все итоговые массивы проверены. Алгоритмы, бинарник, геометрия нагрузки, "
        "число запросов, интервалы перестройки и остальные настройки сохранены. "
        "Внутри каждого сравнения трёх реализаций запросы одинаковые; между числами потоков "
        "меняются разделение работы и независимые генераторы входных трасс. "
        "Хранимые трассы повторяются циклически; бюджет 2²⁰ запросов на фазу суммарно по потокам. "
        "Подготовка входных данных, обнуление массива и проверка результата находятся вне таймера одинаково для всех вариантов.\n\n"
        "Время включает холодное начало, статистику, перестройки и завершение. Фиксировано число запросов, а не длительность. "
        "При разных числах потоков меняются длительность и доля первых 30 секунд. "
        "В shift, exchange_range и moving_variable_range по одной смене нагрузки после половины операций. "
        "8-поточные результаты получены в предыдущей серии. Одиночные точки не доказывают статистическую значимость "
        "небольших различий или превосходство одной адаптивной стратегии над другой.\n\n"
        "[Сырые численные данные](summary.csv), [команды и статусы](runs.json), [методика и хеши](metadata.json).\n\n"
        "Графики воспроизводятся командой: `MPLCONFIGDIR=build/mpl-cache XDG_CACHE_HOME=build/xdg-cache "
        "build/plot-venv/bin/python BENCHMARK_RESULT/2026-09-18/scaling_4_6_8_10/plot.py`.\n"
    )
    print(f"Rendered {len(scenarios)} figures from {len(all_rows)} verified measurements: {OUT / 'GRAPHS.md'}")


if __name__ == "__main__":
    main()
