#!/usr/bin/env python3
"""Rebuild the genetic and combined tables from verified scaling measurements."""

import argparse
import csv
import json
from pathlib import Path

from genetic_compare import CASES, ROOT

THREADS = (4, 6, 8, 10)
SLIDING = "sliding_variable_range"
OLD = ROOT / "BENCHMARK_RESULT/2026-09-18"
IDEAS = {
    "uniform": "цена адаптации без перекоса",
    "clusters_2": "разделение двух горячих областей",
    "clusters_8": "распределение блокировок между очагами",
    "unequal": "больше блокировок нагруженной области",
    "shift": "реакция на переезд нагрузки",
    "hot_with_spike": "конкуренция за один элемент",
    "steady_range": "адаптация к постоянным диапазонам",
    "clusters_2_range": "независимость двух горячих областей",
    "clusters_4_range": "балансировка нескольких групп диапазонов",
    "islands_range": "разделение независимых островков",
    "gradient_range": "подстройка под плотность запросов",
    "unequal_widths_range": "учёт размера и популярности областей",
    "exchange_range": "реакция на смену популярности",
    "moving_variable_range": "переезд диапазонов разной длины",
    "sliding_variable_range": "слежение за постепенным сдвигом",
}


def short_label(scenario, interval=None):
    suffix = " (10 с)" if interval == 10000 else ""
    return f"`{scenario}`{suffix} — {IDEAS[scenario]}"


def read_csv(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def measured(directory, scenario, implementation):
    manifest = directory / "manifest.json"
    if not manifest.exists():
        return None
    runs = json.loads(manifest.read_text())["runs"]
    if not any(r["scenario"] == scenario and r["status"] == "complete" for r in runs):
        return None
    path = directory / f"{scenario}.csv"
    rows = read_csv(path)
    if len(rows) != 1 or rows[0]["implementation"] != implementation:
        raise ValueError(f"Wrong implementation/count: {path}")
    if rows[0]["verified"] != "true" or rows[0]["repetition"] != "1":
        raise ValueError(f"Unverified measurement: {path}")
    return rows[0], path


def reference(base, scenario, threads):
    if threads == 8:
        return measured(base / "fixed_full_8t", scenario, "fixed")
    if scenario == SLIDING and threads in (6, 10):
        return measured(base / f"sliding_fixed_10s_{threads}t", scenario, "fixed")
    path = (OLD / "sliding_10s_4_6_10/4t.csv" if scenario == SLIDING else
            OLD / f"scaling_4_6_8_10/{threads}t/{scenario}.csv")
    rows = [r for r in read_csv(path) if r["implementation"] == "fixed"]
    if len(rows) != 1 or rows[0]["verified"] != "true":
        raise ValueError(f"Invalid historical fixed: {path}")
    return rows[0], path


def fmt(value):
    return "—" if value is None else f"{float(value):.2f}".replace(".", ",")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    base = args.directory.resolve()
    out = base / "scaling_4_6_8_10"
    out.mkdir(exist_ok=True)
    descriptions = {}
    for line in (ROOT / "BENCHMARK_SHORT.md").read_text().splitlines():
        if line.startswith("| `"):
            cells = [c.strip() for c in line.strip("|").split("|")]
            descriptions.setdefault(cells[0].split("`")[1], cells[1])
    data = {}
    records = []
    for scenario, operations in CASES.items():
        for interval in ((30000, 10000) if scenario == SLIDING else (30000,)):
            for threads in THREADS:
                directory = base / (f"full_{threads}t" if interval == 30000 else
                                    f"sliding_genetic_10s_{threads}t")
                result = measured(directory, scenario, "genetic")
                if result is None:
                    continue
                genetic, source = result
                assert int(genetic["operations"]) == operations
                assert int(genetic["threads"]) == threads
                assert int(genetic["rebuild_interval_ms"]) == interval
                fixed_result = reference(base, scenario, threads)
                fixed_s, fixed_source = None, None
                if fixed_result is not None:
                    fixed, fixed_source = fixed_result
                    for key in ("scenario", "threads", "repetition", "operations", "trace_size",
                                "unique_queries", "seed", "array_size", "hot_width", "sample_rate"):
                        if fixed[key] != genetic[key]:
                            raise ValueError(f"Unmatched {key}: {scenario}, {threads}t")
                    # Historical point rows say range_length=8, but the generator
                    # always uses length 1 for these scenarios. Fixed has no timer.
                    if scenario.endswith("range"):
                        assert fixed["range_length"] == genetic["range_length"]
                    assert len(fixed["phase_wall_s"].split(";")) == len(genetic["phase_wall_s"].split(";"))
                    fixed_s = float(fixed["wall_s"])
                wall_s = float(genetic["wall_s"])
                row = {"scenario": scenario, "threads": threads,
                       "rebuild_interval_ms": interval, "operations": operations,
                       "genetic_wall_s": wall_s, "genetic_qps": operations / wall_s,
                       "reference_fixed_wall_s": fixed_s,
                       "genetic_over_fixed": fixed_s / wall_s if fixed_s is not None else None,
                       "optimizations": genetic["optimizations"], "rebuilds": genetic["rebuilds"],
                       "rebuild_ms": genetic["rebuild_ms"], "verified": "true",
                       "genetic_source": str(source.relative_to(ROOT)),
                       "fixed_source": str(fixed_source.relative_to(ROOT)) if fixed_source else None}
                data[(scenario, interval, threads)] = row
                records.append(row)
    with (out / "summary.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(records[0]))
        writer.writeheader()
        writer.writerows(records)

    geometry = ("Точки: массив **2²⁰**, длина запроса **1**, окно **2¹⁴**. "
                "Диапазоны: массив **2²⁶**, длина **10 000**, окно **2²²**; исключения указаны в строках.")
    refs = ("**Базы fixed:** для 8 потоков — новая серия 22 сентября; для 4/6/10 потоков — "
            "сохранённая серия 18 сентября, кроме sliding на 6/10 потоках, где fixed измерен "
            "дополнительно 22 сентября. Исторические отношения не являются свежими парными замерами; "
            "условия разных серий могли отличаться.")
    rows = []
    for scenario, operations in CASES.items():
        for interval in ((30000, 10000) if scenario == SLIDING else (30000,)):
            label = short_label(scenario, interval)
            values = []
            for threads in THREADS:
                row = data.get((scenario, interval, threads))
                values.append("— / —" if row is None else
                              f"{fmt(row['genetic_wall_s'])} / {fmt(row['genetic_over_fixed'])}")
            rows.append(f"| {label} | {descriptions[scenario]} | {operations / 1e6:g} | " + " | ".join(values) + " |")
    root_report = [geometry, "",
                   "**4, 6, 8 и 10 потоков; 64 мьютекса. Q — всего запросов по всем потокам, млн.** "
                   "Один повтор. Полное время с холодным стартом, сбором статистики и перестройками.", "",
                   "В ячейках **время genetic, с / genetic относительно fixed по запросам/с**. "
                   "Отношение **>1 — быстрее fixed, <1 — медленнее**. «—» — полного замера нет. "
                   "Интервал проверки **30 с**, кроме отдельной строки sliding с **10 с**.", "",
                   "| Тест | Нагрузка | Q, млн | 4 потока | 6 потоков | 8 потоков | 10 потоков |",
                   "|---|---|---:|---:|---:|---:|---:|", *rows, ""]
    (ROOT / "GENETIC_COMPARISON.md").write_text("\n".join(root_report))

    legacy = {}
    for row in read_csv(OLD / "scaling_4_6_8_10/summary.csv"):
        legacy[(row["scenario"], int(row["threads"]), row["implementation"])] = float(row["speedup"])
    for path in (OLD / "sliding_10s_4_6_10/4t.csv", OLD / "sliding_10s_8t/results.csv"):
        old_rows = read_csv(path)
        fixed_s = next(float(r["wall_s"]) for r in old_rows if r["implementation"] == "fixed")
        for row in old_rows:
            legacy[(row["scenario"], int(row["threads"]), row["implementation"])] = fixed_s / float(row["wall_s"])
    baseline = {(r["scenario"], int(r["threads"]), r["implementation"]): float(r["speedup_vs_previous_fixed"])
                for r in read_csv(ROOT / "BENCHMARK_RESULT/2026-09-20/baselines/summary.csv")}
    combined = [geometry, "",
                "**Q — всего запросов по всем потокам, млн.** Fixed, balanced, DP и genetic используют "
                "64 мьютекса; global — один на весь массив, element — по одному на каждый элемент.", "",
                "В каждой ячейке пять значений: **balanced / DP / genetic / global / element**. "
                "Это отношение запросов/с к соответствующему **fixed**: **>1 быстрее, <1 медленнее**. "
                "«—» — полного замера нет.", "",
                "| Тест | Нагрузка | Q, млн | 4 потока | 6 потоков | 8 потоков | 10 потоков |",
                "|---|---|---:|---:|---:|---:|---:|"]
    for scenario, operations in CASES.items():
        values = []
        for threads in THREADS:
            row = data.get((scenario, 10000 if scenario == SLIDING else 30000, threads))
            numbers = [legacy.get((scenario, threads, kind)) for kind in ("balanced", "dynamic_dp")]
            numbers += [row["genetic_over_fixed"] if row else None]
            numbers += [baseline.get((scenario, threads, kind)) for kind in ("global", "per_element")]
            values.append(" / ".join(fmt(value) for value in numbers))
        combined.append(f"| {short_label(scenario)} | {descriptions[scenario]} | {operations / 1e6:g} | " + " | ".join(values) + " |")
    element_ranges = base / "per_element_ranges_8t/summary.csv"
    if element_ranges.exists():
        for row in read_csv(element_ranges):
            assert row["verified"] == "true" and row["threads"] == "8"
            scenario = row["scenario"]
            numbers = [row[k + "_over_fixed"] for k in ("balanced", "dynamic_dp", "genetic")]
            numbers += [None, row["element_over_fixed"]]
            values = [" / ".join(fmt(value) for value in numbers) if threads == 8 else
                      "— / — / — / — / —" for threads in THREADS]
            label = f"`{scenario}` (1 млн) — {IDEAS[scenario]}"
            workload = descriptions[scenario] + f"; интервал перестройки {row['rebuild_interval_ms']} мс"
            combined.append(f"| {label} | {workload} | {int(row['operations']) / 1e6:g} | " +
                            " | ".join(values) + " |")
    combined += [""]
    (ROOT / "BENCHMARK_SHORT.md").write_text("\n".join(combined))

    detailed = ["# Genetic: полные результаты на 4/6/8/10 потоках", "", refs, "",
                f"Готово genetic **{len(records)}/64**. Все опубликованные строки verified=true. "
                "Бинарник и параметры алгоритма общие с исходной 8-поточной серией. "
                "Полные объёмы, один повтор, 64 мьютекса, sampling 1/64, seed 1. "
                "Прогоны выполнялись последовательно, новые — только genetic и недостающий fixed sliding 6/10.", "",
                "[CSV со ссылками на источники](summary.csv) · [План/команды](PROGRESS.md) · "
                "[Короткая таблица](../../../GENETIC_COMPARISON.md)", "",
                "| Сценарий | Потоки | Интервал, с | Genetic, с | Млн запросов/с | Fixed, с | Genetic / fixed | Перестроек | Проверки, мс |",
                "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for row in records:
        detailed.append("| " + " | ".join([f"`{row['scenario']}`", str(row["threads"]),
                         str(row["rebuild_interval_ms"] // 1000), fmt(row["genetic_wall_s"]),
                         fmt(row["genetic_qps"] / 1e6), fmt(row["reference_fixed_wall_s"]),
                         fmt(row["genetic_over_fixed"]), row["rebuilds"],
                         f"{float(row['rebuild_ms']):.3f}".replace(".", ",")]) + " |")
    (out / "RESULTS.md").write_text("\n".join(detailed) + "\n")
    completed = sum(row["genetic_over_fixed"] is not None for row in records)
    print(f"Genetic measurements: {len(records)}/64; comparisons with fixed: {completed}/64")


if __name__ == "__main__":
    main()
