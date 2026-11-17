#!/usr/bin/env python3
"""Plot verified matched benchmark runs; pip install matplotlib.

Example: python scripts/plot_throughput.py --input 'Окно=data.csv' --output-dir plots
Every input stays separate. Mixed scenarios/settings become separate panels.
"""
import argparse
import csv
import math
from pathlib import Path
import statistics as stats
import textwrap

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator

IMPL = ("fixed", "balanced", "dynamic_dp")
NAMES = ("fixed", "balanced", "dynamic DP")
COLORS = ("#64748b", "#0891b2", "#d97706")
CONFIG = ("threads", "operations", "trace_size", "array_size", "hot_width",
          "range_length", "sample_rate", "rebuild_interval_ms")
MATCH = CONFIG + ("unique_queries", "seed")


def positive(value, label):
    value = float(value)
    if not math.isfinite(value) or value <= 0:
        raise ValueError(f"{label}: expected a finite positive value")
    return value


def read_inputs(inputs):
    groups = {}
    seen_labels = set()
    for spec in inputs:
        label, separator, filename = spec.partition("=")
        if not separator or not label or label in seen_labels:
            raise ValueError("--input must be UNIQUE_LABEL=PATH")
        seen_labels.add(label)
        with open(filename, newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream)
            required = set(MATCH) | {"scenario", "repetition", "implementation",
                                    "wall_s", "phase_wall_s", "verified", "rebuilds"}
            if not required.issubset(reader.fieldnames or []):
                raise ValueError(f"{filename}: missing columns {required - set(reader.fieldnames or [])}")
            row_count = 0
            for row_count, row in enumerate(reader, 1):
                where = f"{filename}:{row_count + 1}"
                if row["verified"] != "true" or row["implementation"] not in IMPL:
                    raise ValueError(f"{where}: unverified or unknown implementation")
                for key in MATCH + ("repetition", "rebuilds"):
                    row[key] = int(row[key])
                    if row[key] < (0 if key in ("seed", "rebuilds") else 1):
                        raise ValueError(f"{where}: invalid {key}")
                row["wall_s"] = positive(row["wall_s"], where)
                row["phases"] = [positive(v, where) for v in row["phase_wall_s"].split(";")]
                if sum(row["phases"]) > row["wall_s"] * 1.00001:
                    raise ValueError(f"{where}: phases exceed total wall time")
                row["qps"] = row["operations"] / row["wall_s"]
                key = (label, row["scenario"], *(row[k] for k in CONFIG))
                group = groups.setdefault(key, {"label": label, "source": str(Path(filename).resolve()),
                                                "rows": [], "matched": {}})
                pair = group["matched"].setdefault(row["repetition"], {})
                if row["implementation"] in pair:
                    raise ValueError(f"{where}: duplicate repetition/implementation")
                pair[row["implementation"]] = row
                group["rows"].append(row)
            if not row_count:
                raise ValueError(f"{filename}: no measurements")
    for group in groups.values():
        phase_counts = {len(r["phases"]) for r in group["rows"]}
        if len(phase_counts) != 1:
            raise ValueError(f"{group['label']}: mismatched phase counts")
        for repetition, pair in group["matched"].items():
            if set(pair) != set(IMPL):
                raise ValueError(f"{group['label']}: incomplete repetition {repetition}")
            if len({tuple(r[k] for k in MATCH) for r in pair.values()}) != 1:
                raise ValueError(f"{group['label']}: unmatched metadata/seed in repetition {repetition}")
    return list(groups.values())


def phase_operations(row):
    # Exact benchmark Share(Share(total, thread, threads), phase, phases).
    count = len(row["phases"])
    result = [0] * count
    for thread in range(row["threads"]):
        per_thread = row["operations"] // row["threads"] + (thread < row["operations"] % row["threads"])
        for phase in range(count):
            result[phase] += per_thread // count + (phase < per_thread % count)
    return result


def caption(group):
    row = group["rows"][0]
    scenario = row["scenario"]
    length = f"1…{row['range_length']:,}" if "variable_range" in scenario else (
        f"{row['range_length']:,}" if scenario.endswith("_range") else "1")
    def size(value):
        if value & (value - 1) == 0:
            return "2" + str(value.bit_length() - 1).translate(str.maketrans("0123456789", "⁰¹²³⁴⁵⁶⁷⁸⁹"))
        return f"{value:,}"
    width = size(row["hot_width"])
    if scenario.startswith("uniform"):
        width = "весь массив"
    elif scenario == "true_contention":
        width = "1"
    elif scenario == "islands_range":
        width = size(row["hot_width"] // 16)
    elif scenario == "unequal_widths_range":
        width = "/".join(size(row["hot_width"] // divisor) for divisor in (8, 4, 2, 1))
    title = "\n".join(textwrap.wrap(group["label"], 34))
    return (f"{title}\n{scenario}\nN={size(row['array_size'])}; L={length}; W={width}\n"
            f"{row['threads']} потоков; {row['operations'] / 1e6:g} млн запросов; "
            f"r={len(group['matched'])}; интервал {row['rebuild_interval_ms'] / 1000:g} с")


def style(ax):
    ax.set_ylim(bottom=0)
    ax.set_ylabel("Млн запросов / с")
    ax.yaxis.set_major_locator(MaxNLocator(nbins=5))
    ax.grid(axis="y", color="#e2e8f0", linewidth=0.8)
    ax.set_axisbelow(True)
    ax.spines[["top", "right"]].set_visible(False)


def spread(ax, x, values, color):
    middle = stats.median(values)
    ax.errorbar(x, middle, yerr=[[middle - min(values)], [max(values) - middle]],
                fmt="none", ecolor="#0f172a", capsize=5, linewidth=1.2, zorder=4)
    offsets = [0] if len(values) == 1 else [0.16 * (i / (len(values) - 1) - .5) for i in range(len(values))]
    ax.scatter([x + o for o in offsets], values, s=23, color=color,
               edgecolors="white", linewidths=.6, zorder=5)
    return middle


def save(fig, output, name):
    for extension in ("svg", "png"):
        fig.savefig(output / f"{name}.{extension}", dpi=180, facecolor="white")
    plt.close(fig)


def plot(groups, output):
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10,
                         "axes.titleweight": "normal", "svg.fonttype": "none"})
    for page in range(0, len(groups), 6):
        batch = groups[page:page + 6]
        columns = 2 if len(batch) == 4 else min(3, len(batch))
        rows = math.ceil(len(batch) / columns)
        # A single panel still needs room for the common title and method note.
        fig, axes = plt.subplots(rows, columns, figsize=(max(10.4, 5.2 * columns), 5.1 * rows + 1.5), squeeze=False)
        for ax, group in zip(axes.flat, batch):
            for i, implementation in enumerate(IMPL):
                values = [r["qps"] / 1e6 for r in group["rows"] if r["implementation"] == implementation]
                middle = stats.median(values)
                ax.bar(i, middle, width=.62, color=COLORS[i], alpha=.86)
                spread(ax, i, values, "#0f172a")
                ax.annotate(f"{middle:.3g}", (i, max(values)), xytext=(0, 9),
                            textcoords="offset points", ha="center", fontweight="bold")
            ax.set_xticks(range(3), NAMES)
            ax.set_title(caption(group), fontsize=10, pad=14)
            style(ax)
            ax.set_ylim(top=ax.get_ylim()[1] * 1.14)
        for ax in list(axes.flat)[len(batch):]:
            ax.set_visible(False)
        fig.suptitle("Пропускная способность за весь запуск", fontsize=18, y=.985)
        fig.text(.5, .025, "operations / wall_s: холодное начало, статистика, перестройки и завершение включены.\n"
                 "Столбец — медиана; точки — все повторы; усы — min–max, не доверительный интервал.\n"
                 "Шкалы панелей различаются, все начинаются с нуля. W — ширина одного горячего окна, L — длина запроса.",
                 ha="center", fontsize=10)
        fig.tight_layout(rect=(0, .12 if rows == 1 else .085, 1, .94))
        save(fig, output, f"throughput_{page // 6 + 1:02d}")
    for index, group in enumerate(groups, 1):
        count = len(group["rows"][0]["phases"])
        if count < 2:
            continue
        fig, ax = plt.subplots(figsize=(10, 6.5))
        for i, implementation in enumerate(IMPL):
            values = [[n / seconds / 1e6 for n, seconds in zip(phase_operations(r), r["phases"])]
                      for r in group["rows"] if r["implementation"] == implementation]
            medians = [stats.median(v[p] for v in values) for p in range(count)]
            ax.plot(range(1, count + 1), medians, "o-", color=COLORS[i], label=NAMES[i], linewidth=2)
            for phase in range(count):
                spread(ax, phase + 1 + (i - 1) * .07, [v[phase] for v in values], COLORS[i])
        ax.set_xticks(range(1, count + 1))
        ax.set_xlabel("Номер фазы (порядок запросов одинаков; длительности различаются)")
        if group["rows"][0]["scenario"] == "moving_variable_range":
            ax.axvline(2.5, color="#475569", linestyle="--", linewidth=1.2)
            ax.text(2.52, .97, "Перенос окна\nслева направо", transform=ax.get_xaxis_transform(),
                    va="top", fontsize=9, color="#475569")
        elif group["rows"][0]["scenario"] == "exchange_range":
            ax.axvline(1.5, color="#475569", linestyle="--", linewidth=1.2)
            ax.text(1.52, .97, "Доли трафика\n90/10 → 10/90", transform=ax.get_xaxis_transform(),
                    va="top", fontsize=9, color="#475569")
        ax.set_title(caption(group), fontsize=11, pad=14)
        ax.legend(frameon=False)
        style(ax)
        fig.text(.5, .025, "Запросы фазы / phase_wall_s; холодное начало и перестройки внутри фаз учтены.\n"
                 "Создание и завершение потоков/структуры учитывает только график за весь запуск.\n"
                 "Линии — медианы фаз, точки — повторы, усы — min–max. Это не график мгновенной скорости.",
                 ha="center", fontsize=10)
        fig.tight_layout(rect=(0, .14, 1, 1))
        save(fig, output, f"phases_{index:02d}_{group['rows'][0]['scenario']}")


def summary(groups, output):
    fields = ("label", "source", "scenario", *CONFIG, "implementation", "repetitions",
              "median_qps", "min_qps", "max_qps", "median_paired_speedup", "min_paired_speedup",
              "max_paired_speedup", "min_rebuilds", "max_rebuilds")
    with (output / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for group in groups:
            for implementation in IMPL:
                runs = [r for r in group["rows"] if r["implementation"] == implementation]
                qps = [r["qps"] for r in runs]
                ratios = [p[implementation]["qps"] / p["fixed"]["qps"] for p in group["matched"].values()]
                writer.writerow({"label": group["label"], "source": group["source"],
                    "scenario": runs[0]["scenario"], **{k: runs[0][k] for k in CONFIG},
                    "implementation": implementation, "repetitions": len(runs),
                    "median_qps": stats.median(qps), "min_qps": min(qps), "max_qps": max(qps),
                    "median_paired_speedup": stats.median(ratios), "min_paired_speedup": min(ratios),
                    "max_paired_speedup": max(ratios), "min_rebuilds": min(r["rebuilds"] for r in runs),
                    "max_rebuilds": max(r["rebuilds"] for r in runs)})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", action="append", required=True, metavar="LABEL=CSV")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    try:
        groups = read_inputs(args.input)
        args.output_dir.mkdir(parents=True, exist_ok=True)
        plot(groups, args.output_dir)
        summary(groups, args.output_dir)
    except (ValueError, OSError, TypeError, KeyError) as error:
        parser.error(str(error))
    print(f"Rendered {len(groups)} scenarios to {args.output_dir}")


if __name__ == "__main__":
    main()
