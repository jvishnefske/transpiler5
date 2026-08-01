#!/usr/bin/env python3
"""Generate the data-driven LaTeX tables from the measurement CSVs.

Every number in the paper's result tables is produced here from a CSV; none
is typed by hand. A missing input yields a table that says so rather than a
stale one.
"""
import csv
import collections
import os
import sys

DATA = sys.argv[1] if len(sys.argv) > 1 else "."
OUT = sys.argv[2] if len(sys.argv) > 2 else "tables"

LABEL = {
    "cpp-realworld": r"C\texttt{++} projects",
    "c-realworld": "C programs",
    "endtoend": "EndToEnd",
    "c-testsuite": "c-testsuite",
}
ORDER = ["cpp-realworld", "c-realworld", "endtoend", "c-testsuite"]


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
        return default


def write(name, text):
    with open(os.path.join(OUT, name), "w") as handle:
        handle.write(text)
    print("  wrote " + name)


def missing(name, why):
    write(name, "\\begin{table}[t]\\centering\\footnotesize\n"
                "\\textit{Pending: %s.}\n\\end{table}\n" % why)


def e1():
    rows = read("e1_summary.csv")
    if not rows:
        return missing("e1.tex", "E1 data not collected")
    agg = collections.defaultdict(collections.Counter)
    keys = ["items", "green", "yellow", "red", "ported", "stubbed",
            "dropped", "missing", "declared", "false_green", "false_red"]
    for row in rows:
        bucket = agg[row["corpus"]]
        bucket["projects"] += 1
        for key in keys:
            bucket[key] += num(row.get(key))

    total = collections.Counter()
    body = []
    for corpus in ORDER:
        if corpus not in agg:
            continue
        v = agg[corpus]
        total.update(v)
        body.append(
            "%s & %d & %d & %d & %d & %d & %d & %d & \\textbf{%d} \\\\"
            % (LABEL[corpus], v["items"], v["green"], v["yellow"], v["red"],
               v["ported"], v["dropped"], v["false_green"], v["false_red"]))
    body.append("\\midrule")
    body.append(
        "total (%d proj.) & %d & %d & %d & %d & %d & %d & %d & \\textbf{%d} \\\\"
        % (total["projects"], total["items"], total["green"], total["yellow"],
           total["red"], total["ported"], total["dropped"],
           total["false_green"], total["false_red"]))

    write("e1.tex", r"""% Nine columns do not fit a single column at 10pt, so this one spans
% both. Kept as generated output rather than hand-tuned in the source.
\begin{table*}[t]
\centering
\footnotesize
\caption{E1: predicted colour against realised status. \emph{FP} is a false
positive (predicted admissible, then dropped): recoverable, costing one
probe. \emph{FN} is a false negative (predicted \red{red}, actually ported):
unrecoverable, and the quantity \S\ref{sec:directional} argues must be zero.}
\label{tab:e1}
\begin{tabular}{@{}lrrrrrrrr@{}}
\toprule
 & & \multicolumn{3}{c}{\textbf{predicted}}
 & \multicolumn{2}{c}{\textbf{realised}}
 & \multicolumn{2}{c}{\textbf{errors}} \\
\cmidrule(lr){3-5}\cmidrule(lr){6-7}\cmidrule(lr){8-9}
\textbf{corpus} & \textbf{items} & \green{G} & \amber{Y} & \red{R}
 & port. & drop. & FP & FN \\
\midrule
""" + "\n".join(body) + r"""
\bottomrule
\end{tabular}

\vspace{2pt}
\raggedright\scriptsize
Statuses not shown (\texttt{stubbed}, \texttt{missing}, \texttt{declared})
account for the remainder and are neither error kind. \texttt{declared} items
are prototypes with no definition in the project and are excluded from the
translated-fraction denominator. The \amber{Y} column is zero throughout:
see \S\ref{sec:eval-precision}.
\end{table*}
""")


def e2():
    rows = read("e2_yield.csv")
    if not rows:
        return missing("e2.tex", "E2 data not collected")
    agg = collections.defaultdict(collections.Counter)
    for row in rows:
        bucket = agg[row["corpus"]]
        bucket["projects"] += 1
        bucket["strict_ok"] += num(row.get("strict_ok"))
        bucket["strict_builds"] += num(row.get("strict_builds"))
        bucket["incr_emitted"] += num(row.get("incr_emitted"))
        bucket["incr_builds"] += num(row.get("incr_builds"))
        bucket["graph_items"] += num(row.get("graph_items"))
        bucket["ported"] += num(row.get("ported"))
        bucket["stubbed"] += num(row.get("stubbed"))

    body = []
    total = collections.Counter()
    # Known corpora first in a fixed order, then anything else the harness
    # produced -- a corpus missing from ORDER must still reach the totals.
    corpora = [c for c in ORDER if c in agg]
    corpora += [c for c in sorted(agg) if c not in corpora]
    for corpus in corpora:
        v = agg[corpus]
        total.update(v)
        pct = 100.0 * v["ported"] / v["graph_items"] if v["graph_items"] else 0
        body.append(
            "%s & %d & %d & \\textbf{%d} & %d/%d & %.0f\\%% \\\\"
            % (LABEL.get(corpus, corpus), v["projects"], v["strict_builds"],
               v["incr_builds"], v["ported"], v["graph_items"], pct))
    body.append("\\midrule")
    pct = 100.0 * total["ported"] / total["graph_items"] if total["graph_items"] else 0
    body.append("total & %d & %d & \\textbf{%d} & %d/%d & %.0f\\%% \\\\"
                % (total["projects"], total["strict_builds"],
                   total["incr_builds"], total["ported"],
                   total["graph_items"], pct))

    write("e2.tex", r"""\begin{table}[t]
\centering
\footnotesize
\caption{E2: projects yielding a \emph{compiling} Rust crate. ``strict'' is
the conventional all-or-nothing path; ``partial'' is the mode described in
this paper. Both columns count crates that \texttt{cargo build} accepts, not
crates that were merely emitted.}
\label{tab:e2}
\begin{tabular}{@{}lrrrrr@{}}
\toprule
\textbf{corpus} & \textbf{proj.} & \textbf{strict} & \textbf{partial}
 & \textbf{items} & \textbf{\%} \\
\midrule
""" + "\n".join(body) + r"""
\bottomrule
\end{tabular}
\end{table}
""")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    print("tables from " + os.path.abspath(DATA))
    e1()
    e2()
