#!/usr/bin/env python3
"""Regenerate GitHub-rendered artifacts from benchmark outputs (stdlib only).

Reads results/*.csv (distributions) and results/*.log (throughput), writes:
  results/summary.csv          comparison table (GitHub renders tables <=512KB)
  results/latency_bars.svg     p50/p99, TCP vs gRPC, order + tick-to-trade
  results/throughput_bars.svg  sustained op/s, TCP vs gRPC
  results/tick2trade_cdf.svg   tick-to-trade CDF, TCP vs gRPC (log x-axis)

Usage: python3 scripts/plot.py [results_dir]
"""

import csv
import math
import os
import re
import sys

RES = sys.argv[1] if len(sys.argv) > 1 else "results"

INK, GRID, BG = "#1f2328", "#d0d7de", "#ffffff"
FONT = "system-ui,-apple-system,'Segoe UI',sans-serif"


def load_col(csv_name, col):
    vals = []
    with open(os.path.join(RES, csv_name), newline="") as f:
        rows = csv.reader(f)
        idx = next(rows).index(col)
        for row in rows:
            if row:
                vals.append(float(row[idx]))
    vals.sort()
    return vals


def pct(s, p):
    i = p * (len(s) - 1)
    lo, hi = math.floor(i), math.ceil(i)
    return s[lo] if lo == hi else s[lo] * (hi - i) + s[hi] * (i - lo)


def ops_from_log(log_name):
    with open(os.path.join(RES, log_name)) as f:
        m = re.search(r"throughput=([\d.]+)", f.read())
    return float(m.group(1)) if m else float("nan")


def nice_top(vmax, nticks=5):
    raw = vmax * 1.12 / nticks
    mag = 10 ** math.floor(math.log10(raw))
    step = next(m * mag for m in (1, 2, 2.5, 5, 10) if raw <= m * mag)
    return step * nticks, step


def frame(w, h, title):
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
        f'viewBox="0 0 {w} {h}" font-family="{FONT}" font-size="12" fill="{INK}">\n'
        f"<title>{title}</title>\n"
        f'<rect width="{w}" height="{h}" fill="{BG}"/>\n'
        f'<text x="24" y="28" font-size="15" font-weight="600">{title}</text>\n'
    )


def legend(series, x, y):
    out = []
    cx = x
    for name, color in series:
        out.append(
            f'<rect x="{cx}" y="{y}" width="12" height="12" fill="{color}"/>\n'
        )
        out.append(f'<text x="{cx + 17}" y="{y + 11}">{name}</text>\n')
        cx += 30 + 8 * len(name)
    return "".join(out)


def legend_x(series, w=640):
    return w - 24 - sum(30 + 8 * len(n) for n, _ in series)


def bar_chart(fname, title, ylabel, groups, series, values, tick_fmt, val_fmt):
    """series: [(name, color)]; values: {name: [one per group]}."""
    W, H = 640, 380
    ML, MR, MT, MB = 72, 24, 56, 64
    pw, ph = W - ML - MR, H - MT - MB
    top, step = nice_top(max(v for vs in values.values() for v in vs))
    out = [frame(W, H, title), legend(series, legend_x(series), 20)]
    for t in range(6):
        v = step * t
        y = MT + ph - ph * v / top
        out.append(
            f'<line x1="{ML}" y1="{y:.1f}" x2="{W - MR}" y2="{y:.1f}" '
            f'stroke="{GRID}"/>\n'
            f'<text x="{ML - 8}" y="{y + 4:.1f}" text-anchor="end">'
            f"{tick_fmt(v)}</text>\n"
        )
    out.append(
        f'<text x="18" y="{MT + ph / 2}" text-anchor="middle" '
        f'transform="rotate(-90 18 {MT + ph / 2})">{ylabel}</text>\n'
    )
    ng, ns = len(groups), len(series)
    gw = pw / ng
    bw = gw * 0.62 / ns
    for i, g in enumerate(groups):
        gx = ML + i * gw + (gw - bw * ns) / 2
        for j, (name, color) in enumerate(series):
            v = values[name][i]
            h = ph * v / top
            x, y = gx + j * bw + 1.5, MT + ph - h
            out.append(
                f'<rect x="{x:.1f}" y="{y:.1f}" width="{bw - 3:.1f}" '
                f'height="{h:.1f}" fill="{color}"/>\n'
                f'<text x="{x + (bw - 3) / 2:.1f}" y="{y - 6:.1f}" '
                f'text-anchor="middle" font-size="11">{val_fmt(v)}</text>\n'
            )
        out.append(
            f'<text x="{ML + i * gw + gw / 2:.1f}" y="{H - 36}" '
            f'text-anchor="middle">{g}</text>\n'
        )
    out.append(
        f'<line x1="{ML}" y1="{MT + ph}" x2="{W - MR}" y2="{MT + ph}" '
        f'stroke="{INK}"/>\n</svg>\n'
    )
    with open(os.path.join(RES, fname), "w") as f:
        f.write("".join(out))


def cdf_chart(fname, title, xlabel, curves):
    """curves: [(name, color, sorted_values)]."""
    W, H = 640, 380
    ML, MR, MT, MB = 72, 24, 56, 64
    pw, ph = W - ML - MR, H - MT - MB
    lo = 10 ** math.floor(math.log10(min(s[0] for _, _, s in curves)))
    hi = 10 ** math.ceil(math.log10(max(s[-1] for _, _, s in curves)))
    span = math.log10(hi) - math.log10(lo)
    out = [frame(W, H, title)]
    out.append(
        legend([(n, c) for n, c, _ in curves],
               legend_x([(n, c) for n, c, _ in curves]), 20)
    )
    p = lo
    while p <= hi:
        x = ML + pw * (math.log10(p) - math.log10(lo)) / span
        out.append(
            f'<line x1="{x:.1f}" y1="{MT}" x2="{x:.1f}" y2="{MT + ph}" '
            f'stroke="{GRID}"/>\n'
            f'<text x="{x:.1f}" y="{H - 40}" text-anchor="middle">{p:g}</text>\n'
        )
        p *= 10
    for f100 in (0, 25, 50, 75, 100):
        y = MT + ph - ph * f100 / 100
        out.append(
            f'<line x1="{ML}" y1="{y:.1f}" x2="{W - MR}" y2="{y:.1f}" '
            f'stroke="{GRID}"/>\n'
            f'<text x="{ML - 8}" y="{y + 4:.1f}" text-anchor="end">'
            f"{f100}%</text>\n"
        )
    out.append(
        f'<text x="{ML + pw / 2}" y="{H - 14}" text-anchor="middle">{xlabel}</text>\n'
    )
    for _, color, s in curves:
        stride = max(1, len(s) // 600)
        pts = []
        for k in range(0, len(s), stride):
            x = ML + pw * (math.log10(s[k]) - math.log10(lo)) / span
            y = MT + ph - ph * (k + 1) / len(s)
            pts.append(f"{x:.1f},{y:.1f}")
        x = ML + pw * (math.log10(s[-1]) - math.log10(lo)) / span
        pts.append(f"{x:.1f},{MT:.1f}")
        out.append(
            f'<polyline points="{" ".join(pts)}" fill="none" stroke="{color}" '
            f'stroke-width="2"/>\n'
        )
    out.append("</svg>\n")
    with open(os.path.join(RES, fname), "w") as f:
        f.write("".join(out))


def main():
    have_grpc = os.path.exists(os.path.join(RES, "grpc_order.csv"))
    specs = [
        ("order", "TCP", "tcp_order.csv", "rtt_us", "tcp_order.log"),
        ("throughput", "TCP", "tcp_throughput.csv", "rtt_us", "tcp_throughput.log"),
        ("tick2trade", "TCP", "tcp_tick2trade.csv", "tick2trade_us", "tcp_tick2trade.log"),
    ]
    if have_grpc:
        specs += [
            ("order", "gRPC", "grpc_order.csv", "rtt_us", "grpc_order.log"),
            ("throughput", "gRPC", "grpc_throughput.csv", "rtt_us", "grpc_throughput.log"),
            ("tick2trade", "gRPC", "grpc_tick2trade.csv", "tick2trade_us", "grpc_tick2trade.log"),
        ]
    st = {}
    for bench, transport, csv_name, col, log in specs:
        s = load_col(csv_name, col)
        st[(bench, transport)] = {
            "n": len(s),
            "ops": ops_from_log(log),
            "p50": pct(s, 0.50),
            "p99": pct(s, 0.99),
            "max": s[-1],
            "dist": s,
        }

    with open(os.path.join(RES, "summary.csv"), "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["benchmark", "transport", "n", "ops_per_s",
                    "p50_us", "p99_us", "max_us"])
        for (bench, transport), d in st.items():
            w.writerow([bench, transport, d["n"], f"{d['ops']:.0f}",
                        f"{d['p50']:.1f}", f"{d['p99']:.1f}", f"{d['max']:.1f}"])

    t = lambda b, k: st[(b, "TCP")][k]  # noqa: E731
    g = lambda b, k: st[(b, "gRPC")][k] if have_grpc else None  # noqa: E731
    lat_series = [("TCP p50", "#0969da"), ("TCP p99", "#54aeff")]
    lat_values = {
        "TCP p50": [t("order", "p50"), t("tick2trade", "p50")],
        "TCP p99": [t("order", "p99"), t("tick2trade", "p99")],
    }
    thr_series = [("TCP", "#0969da")]
    thr_values = {"TCP": [t("order", "ops"), t("throughput", "ops")]}
    curves = [("TCP", "#0969da", st[("tick2trade", "TCP")]["dist"])]
    if have_grpc:
        lat_series += [("gRPC p50", "#cf222e"), ("gRPC p99", "#ff8182")]
        lat_values["gRPC p50"] = [g("order", "p50"), g("tick2trade", "p50")]
        lat_values["gRPC p99"] = [g("order", "p99"), g("tick2trade", "p99")]
        thr_series.append(("gRPC", "#cf222e"))
        thr_values["gRPC"] = [g("order", "ops"), g("throughput", "ops")]
        curves.append(("gRPC", "#cf222e", st[("tick2trade", "gRPC")]["dist"]))

    bar_chart("latency_bars.svg", "Order and tick-to-trade latency (µs)",
              "microseconds", ["order RTT", "tick-to-trade"],
              lat_series, lat_values, lambda v: f"{v:.0f}", lambda v: f"{v:.0f}")
    bar_chart("throughput_bars.svg", "Sustained throughput (ops/sec)", "ops/sec",
              ["order mode", "throughput mode"], thr_series, thr_values,
              lambda v: f"{v / 1000:.0f}k", lambda v: f"{v / 1000:.0f}k")
    cdf_chart("tick2trade_cdf.svg", "Tick-to-trade CDF",
              "tick-to-trade (µs, log scale)", curves)
    print("wrote summary.csv latency_bars.svg throughput_bars.svg tick2trade_cdf.svg")


if __name__ == "__main__":
    main()
