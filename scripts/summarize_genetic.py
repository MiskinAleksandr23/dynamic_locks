#!/usr/bin/env python3
"""Summarize the full genetic-only and fixed-only series without rerunning DP."""

import argparse
import csv
import json
from pathlib import Path

from genetic_compare import CASES


def read_run(directory, scenario, implementation):
    manifest = directory / "manifest.json"
    if not manifest.exists():
        return None
    runs = json.loads(manifest.read_text())["runs"]
    if not any(r["scenario"] == scenario and r["status"] == "complete" for r in runs):
        return None
    with (directory / f"{scenario}.csv").open() as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 1 or rows[0]["implementation"] != implementation or rows[0]["verified"] != "true":
        raise ValueError(f"Invalid completed measurement: {directory}/{scenario}")
    return rows[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    base = args.directory
    output = []
    table = []
    genetic_count = fixed_count = 0
    for scenario, operations in CASES.items():
        genetic = read_run(base / "full_8t", scenario, "genetic")
        fixed = read_run(base / "fixed_full_8t", scenario, "fixed")
        genetic_count += genetic is not None
        fixed_count += fixed is not None
        if genetic is None or fixed is None:
            fixed_time = f"{float(fixed['wall_s']):.2f}" if fixed else "—"
            genetic_time = f"{float(genetic['wall_s']):.2f}" if genetic else "—"
            rebuilds = genetic["rebuilds"] if genetic else "—"
            rebuild_ms = f"{float(genetic['rebuild_ms']):.3f}" if genetic else "—"
            table.append(f"| `{scenario}` | {operations / 1e6:g} | {fixed_time} | {genetic_time} | — | {rebuilds} | {rebuild_ms} |")
            continue
        for key in ("scenario", "repetition", "threads", "operations", "trace_size",
                    "unique_queries", "array_size", "hot_width", "range_length", "seed",
                    "sample_rate", "rebuild_interval_ms"):
            if fixed[key] != genetic[key]:
                raise ValueError(f"Unmatched {key}: {scenario}")
        fixed_s, genetic_s = float(fixed["wall_s"]), float(genetic["wall_s"])
        speedup = fixed_s / genetic_s
        output.append({"scenario": scenario, "threads": genetic["threads"],
                       "operations": operations, "fixed_wall_s": fixed_s,
                       "genetic_wall_s": genetic_s,
                       "fixed_qps": operations / fixed_s,
                       "genetic_qps": operations / genetic_s,
                       "genetic_over_fixed": speedup,
                       "optimizations": genetic["optimizations"],
                       "rebuilds": genetic["rebuilds"],
                       "rebuild_ms": genetic["rebuild_ms"],
                       "verified": "true"})
        table.append(f"| `{scenario}` | {operations / 1e6:g} | {fixed_s:.2f} | {genetic_s:.2f} | "
                     f"{speedup:.3f} | {genetic['rebuilds']} | {float(genetic['rebuild_ms']):.3f} |")
    if output:
        with (base / "full_summary.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(output[0]))
            writer.writeheader()
            writer.writerows(output)
    lines = ["# Полные тесты genetic: 8 потоков", "",
             f"Завершено: genetic **{genetic_count}/{len(CASES)}**, fixed **{fixed_count}/{len(CASES)}**; "
             f"сравнений **{len(output)}/{len(CASES)}**. Один повтор. "
             "Genetic и fixed измерены заново; dynamic/DP и balanced не перезапускались.", "",
             "64 мьютекса, интервал проверок 30 с, выборка 1/64, seed 1, trace 2²⁰ на фазу. "
             "Точки: N=2²⁰, H=2¹⁴, длина 1. Диапазоны: N=2²⁶, H=2²², длина 10000 "
             "(для variable_range — 1…10000). Объёмы сохранены из прежних полных тестов.", "",
             "Время включает холодный старт, сбор статистики, поиск и публикацию границ. "
             "Итоговые массивы проверены поэлементно. Процессы выполнялись последовательно: "
             "сначала серия genetic, затем серия fixed. Один повтор и отсутствие чередования "
             "серий не позволяют оценить статистическую значимость небольших отличий.", "",
             "Ускорение = fixed.wall_s / genetic.wall_s: >1 быстрее fixed, <1 медленнее. "
             "Последний столбец уже включён в полное время.", "",
             "| Сценарий | Q, млн | Fixed, с | Genetic, с | Genetic / fixed | Перестроек | Проверки, мс |",
             "|---|---:|---:|---:|---:|---:|---:|", *table, "",
             "[Сырые результаты genetic и команды](full_8t/manifest.json) · "
             "[Fixed и команды](fixed_full_8t/manifest.json) · "
             "[Сводный CSV с запросами/с](full_summary.csv)", "",
             "Старые результаты DP/Balanced сохранены в "
             "[BENCHMARK_SHORT.md](../../BENCHMARK_SHORT.md). Их нельзя напрямую делить на новый "
             "fixed: это разные серии измерений. Старый uniform fixed занимал 118,31 с; "
             "новый genetic без поиска — 61,84 с, поэтому fixed пришлось проверить в текущих условиях.", ""]
    sliding = read_run(base / "sliding_genetic_10s_8t", "sliding_variable_range", "genetic")
    sliding_fixed = read_run(base / "fixed_full_8t", "sliding_variable_range", "fixed")
    if sliding is not None and sliding_fixed is not None:
        # The interval has no effect on fixed: it has no background rebuilder.
        for key in ("scenario", "repetition", "threads", "operations", "trace_size",
                    "unique_queries", "array_size", "hot_width", "range_length", "seed"):
            if sliding[key] != sliding_fixed[key]:
                raise ValueError(f"Unmatched sliding control: {key}")
        lines += ["## Sliding с прежним интервалом 10 секунд", "",
                  "В прежних тестах sliding использовал интервал 10 с. Дополнительно "
                  "измерен только genetic при этом интервале; fixed тот же, поскольку "
                  "он не перестраивается. DP/Balanced не перезапускались.", "",
                  f"Genetic: **{float(sliding['wall_s']):.2f} с**, "
                  f"**{float(sliding_fixed['wall_s']) / float(sliding['wall_s']):.3f}×** к fixed, "
                  f"перестроек {sliding['rebuilds']}, проверок/поиска/публикации "
                  f"{float(sliding['rebuild_ms']):.3f} мс. Итоговый массив проверен.", "",
                  "[Команда и хеши](sliding_genetic_10s_8t/manifest.json) · "
                  "[CSV](sliding_genetic_10s_8t/sliding_variable_range.csv)", ""]
    (base / "FULL_RESULTS.md").write_text("\n".join(lines))
    print(f"Verified matched comparisons: {len(output)}/{len(CASES)}")


if __name__ == "__main__":
    main()
