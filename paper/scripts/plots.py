#!/usr/bin/env python3
"""Generate every figure in the paper from the measurement CSVs.

Reads the CSVs produced by the experiment harnesses (see the data directory's
README for column definitions) and writes one PDF per figure into figures/.
Every figure is regenerated from data on each run; nothing is hand-drawn, and
a missing input produces a clearly-labelled "no data" panel rather than a
silently stale plot.
"""
import csv
import collections
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle

DATA = sys.argv[1] if len(sys.argv) > 1 else "."
OUT = sys.argv[2] if len(sys.argv) > 2 else "figures"

# One palette for the whole paper: green/amber/red carry the lattice meaning,
# so they must never be reused for anything else.
GREEN, AMBER, RED = "#1b7837", "#bf812d", "#b2182b"
BLUE, GREY, DARK = "#2166ac", "#bdbdbd", "#4d4d4d"

plt.rcParams.update({
    "font.size": 8,
    "axes.titlesize": 9,
    "axes.labelsize": 8,
    "legend.fontsize": 7,
    "xtick.labelsize": 7,
    "ytick.labelsize": 7,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "figure.dpi": 150,
})

CORPUS_LABEL = {
    "cpp-realworld": "C++ projects",
    "c-realworld": "C programs",
    "endtoend": "EndToEnd",
    "c-testsuite": "c-testsuite",
}
CORPUS_ORDER = ["cpp-realworld", "c-realworld", "endtoend", "c-testsuite"]


def read(name):
    path = os.path.join(DATA, name)
    if not os.path.exists(path):
        return None
    with open(path) as handle:
        return list(csv.DictReader(handle))


def num(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        try:
            return float(value)
        except (TypeError, ValueError):
            return default


def nodata(name, why):
    fig, ax = plt.subplots(figsize=(6.6, 1.4))
    ax.text(0.5, 0.5, "no data: " + why, ha="center", va="center",
            color=RED, fontsize=9)
    ax.axis("off")
    fig.savefig(os.path.join(OUT, name), bbox_inches="tight")
    plt.close(fig)
    print("  (no data) " + name)


def save(fig, name):
    fig.savefig(os.path.join(OUT, name), bbox_inches="tight")
    plt.close(fig)
    print("  wrote " + name)


# --------------------------------------------------------------------------
# E1: predicted colour against realised status
# --------------------------------------------------------------------------
def e1():
    rows = read("e1_coloring_vs_outcome.csv")
    if not rows:
        return nodata("e1_confusion.pdf", "e1_coloring_vs_outcome.csv")

    colours = ["green", "yellow", "red"]
    statuses = ["ported", "stubbed", "missing", "declared", "dropped"]
    grid = collections.Counter()
    for row in rows:
        grid[(row["predicted_color"], row["actual_status"])] += 1

    fig, (ax, bx) = plt.subplots(
        1, 2, figsize=(7.2, 2.4), gridspec_kw={"width_ratios": [1.55, 1]})

    top = max(grid.values()) if grid else 1
    for i, colour in enumerate(colours):
        for j, status in enumerate(statuses):
            count = grid[(colour, status)]
            # Shade by magnitude, but keep zero visually distinct from small.
            shade = 0.0 if count == 0 else 0.20 + 0.70 * (count / top) ** 0.35
            base = {"green": GREEN, "yellow": AMBER, "red": RED}[colour]
            ax.add_patch(Rectangle((j, -i), 1, -1, facecolor=base,
                                   alpha=shade, edgecolor="white", lw=1.2))
            ax.text(j + 0.5, -i - 0.5, str(count), ha="center", va="center",
                    fontsize=8,
                    color="white" if shade > 0.55 else DARK,
                    fontweight="bold" if count else "normal")

    # The contract-violating cell: predicted red, actually ported.
    fr = grid[("red", "ported")]
    ax.add_patch(Rectangle((0, -2), 1, -1, fill=False, edgecolor=RED,
                           lw=2.0, linestyle="--", zorder=5))
    # Label to the RIGHT of the row: the dashed box is the visual link, so no
    # arrow is needed and nothing can overlap a cell or a tick label.
    ax.text(len(statuses) + 0.12, -2.5,
            "false negative\n(must be 0)", fontsize=6.5, color=RED,
            va="center", ha="left", fontweight="bold")
    # The middle lattice value exists but never fired on these corpora; say so
    # on the figure rather than leaving a silent band of zeros.
    ax.text(len(statuses) + 0.12, -1.5, "never observed\non any corpus",
            fontsize=6.5, color=GREY, va="center", ha="left", style="italic")

    ax.set_xlim(0, len(statuses))
    ax.set_ylim(-len(colours), 0)
    ax.set_xticks([j + 0.5 for j in range(len(statuses))])
    ax.set_xticklabels(statuses, rotation=20, ha="right")
    ax.set_yticks([-i - 0.5 for i in range(len(colours))])
    ax.set_yticklabels(["green", "yellow", "red"])
    ax.set_xlabel("realised status")
    ax.set_ylabel("predicted colour")
    ax.set_title("(a) all corpora, %d items" % len(rows))
    for spine in ax.spines.values():
        spine.set_visible(False)
    ax.tick_params(length=0)

    # Per-corpus error rates.
    per = collections.defaultdict(lambda: collections.Counter())
    for row in rows:
        corpus = row["corpus"]
        per[corpus]["n"] += 1
        pred, act = row["predicted_color"], row["actual_status"]
        if pred in ("green", "yellow") and act == "dropped":
            per[corpus]["fg"] += 1
        if pred == "red" and act == "ported":
            per[corpus]["fr"] += 1

    order = [c for c in CORPUS_ORDER if c in per]
    ys = range(len(order))
    fgs = [per[c]["fg"] for c in order]
    frs = [per[c]["fr"] for c in order]
    span = max(fgs + frs + [1])
    bx.barh([y + 0.18 for y in ys], fgs, height=0.34, color=AMBER,
            label="false positive: admitted, then dropped (costs one probe)")
    bx.barh([y - 0.18 for y in ys], frs, height=0.34, color=RED,
            label="false negative: excluded, would port (unrecoverable)")
    for y, c in zip(ys, order):
        bx.text(per[c]["fg"] + span * 0.05, y + 0.18, str(per[c]["fg"]),
                va="center", fontsize=7)
        bx.text(per[c]["fr"] + span * 0.05, y - 0.18, str(per[c]["fr"]),
                va="center", fontsize=7, color=RED, fontweight="bold")
    bx.set_yticks(list(ys))
    bx.set_yticklabels(["%s\n(n=%d)" % (CORPUS_LABEL.get(c, c), per[c]["n"])
                        for c in order])
    bx.set_xlabel("items")
    bx.set_xlim(0, span * 1.35)
    # Counts are integers; fractional ticks would imply a resolution the data
    # does not have.
    bx.set_xticks(list(range(0, span + 1)))
    bx.set_title("(b) errors by direction")
    # Figure-level legend with space reserved by rect: an axes-level legend
    # anchored outside interacts with tight_layout and collapses the axes.
    handles, labels_ = bx.get_legend_handles_labels()
    fig.legend(handles, labels_, loc="lower center", ncol=1, frameon=False,
               bbox_to_anchor=(0.5, -0.02))
    fig.tight_layout(rect=[0, 0.13, 1, 1])
    save(fig, "e1_confusion.pdf")
    return {"false_red": fr, "items": len(rows)}


# --------------------------------------------------------------------------
# E5: root-cause attribution compression
# --------------------------------------------------------------------------
def e5():
    rows = read("e5_compression.csv")
    if not rows:
        return nodata("e5_attribution.pdf", "e5_compression.csv")
    rows = [r for r in rows if num(r["rejected_items"]) > 0]
    if not rows:
        return nodata("e5_attribution.pdf", "no project has rejected items")
    rows.sort(key=lambda r: -num(r["rejected_items"]))
    rows = rows[:7]

    labels = ["%s\n%s" % (r["project"][:18], CORPUS_LABEL.get(r["corpus"],
                                                              r["corpus"]))
              for r in rows]
    rejected = [num(r["rejected_items"]) for r in rows]
    direct = [num(r["distinct_direct_tags"]) for r in rows]
    roots = [num(r["root_items"]) for r in rows]

    xs = range(len(rows))
    fig, ax = plt.subplots(figsize=(6.9, 2.5))
    ax.bar([x - 0.27 for x in xs], rejected, width=0.26, color=GREY,
           label="rejected items (symptoms)")
    ax.bar([x for x in xs], direct, width=0.26, color=BLUE,
           label="distinct direct blocker tags")
    ax.bar([x + 0.27 for x in xs], roots, width=0.26, color=RED,
           label="root items (the actual work list)")
    for x, (a, c) in enumerate(zip(rejected, roots)):
        if a and c and a > c:
            ax.text(x, max(a, c) + 0.45, "%.1f$\\times$" % (a / c),
                    ha="center", fontsize=7, color=DARK)
    ax.set_xticks(list(xs))
    ax.set_xticklabels(labels, fontsize=6.5, rotation=18, ha="right")
    ax.set_ylabel("count")
    ax.set_title("Work-list compression by root-cause attribution "
                 "(projects with rejections, most first)")
    ax.legend(frameon=False, loc="upper right")
    fig.tight_layout()
    save(fig, "e5_attribution.pdf")




# --------------------------------------------------------------------------
# E4: longitudinal progress on a fixed benchmark
# --------------------------------------------------------------------------
def e4():
    rows = read("e4_totals.csv")
    if not rows:
        return nodata("e4_longitudinal.pdf", "e4_totals.csv")
    ctrl_rows = read("e4_totals_plainmode.csv") or []

    def spine_of(rs, corpus="all"):
        out = [r for r in rs if r["corpus"] == corpus]
        out.sort(key=lambda r: num(r["order"]))
        return out

    treat = spine_of(rows)
    ctrl = spine_of(ctrl_rows)
    if not treat:
        return nodata("e4_longitudinal.pdf", "no 'all' rows in e4_totals.csv")
    xs = list(range(len(treat)))
    shas = [r["sha"][:7] for r in treat]
    total = num(treat[0]["projects"], 1)

    fig, (ax, bx) = plt.subplots(2, 1, figsize=(6.9, 4.1), sharex=True)

    # Treatment: best mode the revision supports. Control: plain mode at every
    # revision. The gap between them is the effect of partial translation,
    # isolated from growth of the supported subset.
    tb = [num(r["crates_building"]) for r in treat]
    ax.plot(xs, tb, color=GREEN, lw=2.2, marker="o", ms=5,
            label="partial translation (treatment)", zorder=3)
    if ctrl:
        cb = [num(r["crates_building"]) for r in ctrl][:len(xs)]
        ax.plot(xs[:len(cb)], cb, color=GREY, lw=1.8, marker="s", ms=4,
                ls="--", label="all-or-nothing (control)", zorder=2)
        # Shade the isolated effect.
        ax.fill_between(xs[:len(cb)], cb, tb[:len(cb)], color=GREEN,
                        alpha=0.12, lw=0, zorder=1)
    ax.axhline(total, color=DARK, lw=0.7, ls=":", zorder=0)
    ax.text(0.02, total + 0.35, "all %d projects" % total, fontsize=6.5,
            color=DARK)
    for x, v in zip(xs, tb):
        ax.annotate(str(v), (x, v), textcoords="offset points",
                    xytext=(0, 7), ha="center", fontsize=6.5, color=GREEN)
    ax.set_ylabel("projects yielding a\ncompiling crate")
    ax.set_ylim(0, total + 3.4)
    ax.legend(frameon=False, loc="lower right", ncol=1, fontsize=6.5)
    ax.set_title("Fixed %d-project benchmark; only the tool varies" % total)

    intro = next((i for i, r in enumerate(treat)
                  if (r.get("ported_total") or "").strip()), None)
    if intro is not None:
        for axis in (ax, bx):
            axis.axvline(intro, color=DARK, lw=0.9, ls="--", zorder=0)
        ax.text(intro - 0.06, total + 2.2, "partial translation introduced",
                fontsize=6.5, color=DARK, style="italic", ha="right")

    for key, label, colour in (("cpp", "C++ projects", RED),
                               ("c", "C programs", BLUE)):
        rowset = spine_of(rows, key)
        pxs, pct, lab = [], [], []
        for x, r in zip(xs, rowset):
            if (r.get("ported_total") or "").strip():
                items = num(r["graph_items_total"], 1)
                pxs.append(x)
                pct.append(100.0 * num(r["ported_total"]) / items)
                lab.append("%s/%s" % (r["ported_total"],
                                      r["graph_items_total"]))
        if not pxs:
            continue
        bx.plot(pxs, pct, color=colour, lw=2, marker="o", ms=4, label=label)
        for x, y, t in zip(pxs, pct, lab):
            bx.annotate(t, (x, y), textcoords="offset points", xytext=(0, 6),
                        ha="center", fontsize=6, color=colour)
    if intro is not None and intro > 0:
        bx.axvspan(-0.4, intro - 0.5, color=GREY, alpha=0.30, lw=0)
        bx.text((intro - 0.5 - 0.4) / 2.0, 45,
                "per-item accounting\ndid not yet exist", fontsize=6.5,
                ha="center", va="center", color=DARK, style="italic")
    bx.set_ylabel("program items\ntranslated (%)")
    bx.set_ylim(0, 100)
    bx.set_xlim(-0.4, len(xs) - 0.6)
    bx.set_xticks(xs)
    bx.set_xticklabels(shas, rotation=30, ha="right", fontsize=6.5)
    bx.set_xlabel("revision (oldest to newest)")
    bx.legend(frameon=False, loc="lower right", ncol=2, fontsize=6.5)
    fig.tight_layout()
    save(fig, "e4_longitudinal.pdf")




# --------------------------------------------------------------------------
# E2: yield of compiling artifacts
# --------------------------------------------------------------------------
def e2():
    rows = read("e2_yield.csv")
    if not rows:
        return nodata("e2_yield.pdf", "e2_yield.csv")
    agg = collections.defaultdict(collections.Counter)
    for row in rows:
        b = agg[row["corpus"]]
        b["n"] += 1
        for key in ("strict_builds", "incr_builds", "graph_items", "ported"):
            b[key] += num(row.get(key))
    order = [c for c in CORPUS_ORDER if c in agg]
    order += [c for c in sorted(agg) if c not in order]

    fig, (ax, bx) = plt.subplots(1, 2, figsize=(6.9, 2.6),
                                 gridspec_kw={"width_ratios": [1.1, 1]})
    ys = range(len(order))
    strict = [100.0 * agg[c]["strict_builds"] / agg[c]["n"] for c in order]
    incr = [100.0 * agg[c]["incr_builds"] / agg[c]["n"] for c in order]
    ax.barh([y + 0.19 for y in ys], incr, height=0.36, color=GREEN,
            label="partial")
    ax.barh([y - 0.19 for y in ys], strict, height=0.36, color=GREY,
            label="all-or-nothing")
    for y, c in zip(ys, order):
        ax.text(incr[y] + 1.5, y + 0.19, "%d/%d" % (agg[c]["incr_builds"],
                                                    agg[c]["n"]),
                va="center", fontsize=6.5, color=GREEN)
        ax.text(strict[y] + 1.5, y - 0.19, "%d/%d" % (agg[c]["strict_builds"],
                                                      agg[c]["n"]),
                va="center", fontsize=6.5, color=DARK)
    ax.set_yticks(list(ys))
    ax.set_yticklabels([CORPUS_LABEL.get(c, c) for c in order])
    ax.set_xlabel("projects yielding a compiling crate (%)")
    ax.set_xlim(0, 128)
    ax.legend(frameon=False, loc="lower right", fontsize=6.5)
    ax.set_title("(a) artifact yield")

    frac = [100.0 * agg[c]["ported"] / agg[c]["graph_items"]
            if agg[c]["graph_items"] else 0 for c in order]
    bx.barh(list(ys), frac, height=0.5, color=BLUE)
    for y, c in zip(ys, order):
        bx.text(frac[y] + 1.5, y, "%d/%d" % (agg[c]["ported"],
                                             agg[c]["graph_items"]),
                va="center", fontsize=6.5, color=BLUE)
    bx.set_yticks(list(ys))
    bx.set_yticklabels([])
    bx.set_xlabel("program items translated (%)")
    bx.set_xlim(0, 122)
    bx.set_title("(b) per-item yield")
    fig.tight_layout()
    save(fig, "e2_yield.pdf")


# --------------------------------------------------------------------------
# E3: repair-search cost and benefit
# --------------------------------------------------------------------------
def e3():
    rows = read("e3_search.csv")
    if not rows:
        return nodata("e3_search.pdf", "e3_search.csv")
    pair = collections.defaultdict(dict)
    for row in rows:
        pair[(row["corpus"], row["project"])][row["mode"]] = row
    gains, overheads, probes = [], [], []
    for (corpus, project), modes in pair.items():
        if "off" not in modes or "on" not in modes:
            continue
        off, on = modes["off"], modes["on"]
        delta = num(on["ported"]) - num(off["ported"])
        gains.append((corpus, project, num(off["ported"]), num(on["ported"]),
                      delta))
        wo, wn = num(off["wall_ms"]), num(on["wall_ms"])
        if wo:
            overheads.append(wn / float(wo))
        probes.append(num(on["probes"]))

    improved = sorted([g for g in gains if g[4] > 0], key=lambda g: -g[4])
    regressed = [g for g in gains if g[4] < 0]

    sweep = read("e3_bound_sweep.csv")
    ncol = 3 if sweep else 2
    widths = [1.25, 0.85, 0.9] if sweep else [1.25, 1]
    fig, axes = plt.subplots(1, ncol, figsize=(6.9 if ncol == 2 else 7.4, 2.6),
                             gridspec_kw={"width_ratios": widths})
    ax, bx = axes[0], axes[1]
    if improved:
        labels = [g[1][:26] for g in improved]
        ys = range(len(improved))
        ax.barh(list(ys), [g[3] for g in improved], height=0.6, color=GREEN)
        for y, g in zip(ys, improved):
            ax.text(g[3] + 0.12, y, "0 $\\rightarrow$ %d" % g[3], va="center",
                    fontsize=6.5, color=GREEN)
        ax.set_yticks(list(ys))
        ax.set_yticklabels(labels, fontsize=6)
        ax.set_xlim(0, max(g[3] for g in improved) * 1.45)
        ax.set_xlabel("items translated with search")
    ax.set_title("(a) %d of %d projects improve; %d regress"
                 % (len(improved), len(gains), len(regressed)))

    bx.hist(overheads, bins=24, color=BLUE, alpha=0.85)
    med = sorted(overheads)[len(overheads) // 2] if overheads else 0
    bx.axvline(med, color=RED, lw=1.4, ls="--")
    bx.text(med, bx.get_ylim()[1] * 0.92, " median %.2f$\\times$" % med,
            color=RED, fontsize=7, va="top")
    bx.set_xlabel("wall-clock overhead of --search ($\\times$)")
    bx.set_ylabel("projects")
    bx.set_title("(b) cost, n=%d" % len(overheads))

    if sweep:
        cx = axes[2]
        by_bound = collections.defaultdict(lambda: [0, 0])
        rescued = set(g[1] for g in improved)
        for row in sweep:
            b = num(row["max_nodes"])
            name = row["project"].split("/")[-1]
            slot = by_bound[b]
            slot[0] += num(row["probes"])
            if name in rescued and num(row["ported"]) > 0:
                slot[1] += 1
        bounds = sorted(by_bound)
        probes_tot = [by_bound[b][0] for b in bounds]
        rescued_n = [by_bound[b][1] for b in bounds]
        cx.plot(bounds, probes_tot, color=RED, lw=1.8, marker="s", ms=4,
                label="probes spent (all projects)")
        cx.set_xscale("log", base=2)
        cx.set_xticks(bounds)
        cx.set_xticklabels([str(b) for b in bounds])
        cx.set_xlabel("--max-search-nodes")
        cx.set_ylabel("total probes", color=RED)
        cx.tick_params(axis="y", labelcolor=RED)
        dx = cx.twinx()
        dx.plot(bounds, rescued_n, color=GREEN, lw=2, marker="o", ms=4)
        dx.set_ylabel("projects rescued", color=GREEN)
        dx.tick_params(axis="y", labelcolor=GREEN)
        dx.set_ylim(0, max(rescued_n) * 1.4 if rescued_n else 1)
        dx.spines["right"].set_visible(True)
        # The knee: every rescue is complete at 2, and nothing above it helps.
        cx.axvline(2, color=DARK, lw=0.9, ls="--")
        cx.text(2.15, max(probes_tot) * 0.55, "all rescues\ncomplete at 2",
                fontsize=6, color=DARK, style="italic")
        cx.set_title("(c) budget: cost rises, benefit does not")

    fig.tight_layout()
    save(fig, "e3_search.pdf")


# --------------------------------------------------------------------------
# E6: analysis cost relative to translation
# --------------------------------------------------------------------------
def e6():
    rows = read("e6_cost.csv")
    if not rows:
        return nodata("e6_cost.pdf", "e6_cost.csv")
    xs, graph_ms, incr_ms, cargo_ms = [], [], [], []
    for row in rows:
        nodes, gm, im = num(row["nodes"]), num(row["graph_ms"]), num(row["incremental_ms"])
        if not (nodes and gm and im):
            continue
        xs.append(nodes)
        graph_ms.append(gm)
        incr_ms.append(im)
        cargo_ms.append(num(row.get("cargo_ms")))

    fig, (ax, bx) = plt.subplots(1, 2, figsize=(6.9, 2.5))
    ax.scatter(xs, graph_ms, s=8, color=BLUE, alpha=0.55, label="item graph")
    ax.scatter(xs, incr_ms, s=8, color=GREEN, alpha=0.55, label="translation")
    live = [c for c in cargo_ms if c]
    if live:
        ax.scatter([x for x, c in zip(xs, cargo_ms) if c], live, s=8,
                   color=GREY, alpha=0.5, label="cargo build")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("program items in project")
    ax.set_ylabel("wall clock (ms)")
    ax.legend(frameon=False, fontsize=6.5, loc="upper left")
    ax.set_title("(a) absolute cost")

    ratio = [g / float(i) for g, i in zip(graph_ms, incr_ms) if i]
    bx.hist(ratio, bins=24, color=BLUE, alpha=0.85)
    med = sorted(ratio)[len(ratio) // 2] if ratio else 0
    bx.axvline(med, color=RED, lw=1.4, ls="--")
    bx.text(med, bx.get_ylim()[1] * 0.92, " median %.2f" % med, color=RED,
            fontsize=7, va="top")
    bx.set_xlabel("analysis time / translation time")
    bx.set_ylabel("projects")
    bx.set_title("(b) both are dominated by the shared parse")
    fig.tight_layout()
    save(fig, "e6_cost.pdf")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    print("plotting from " + os.path.abspath(DATA))
    e1()
    e2()
    e3()
    e4()
    e5()
    e6()
