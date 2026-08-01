# Rebuild the paper from measurement data.
#
#   make DATA=<dir>        regenerate tables + figures, then compile
#   make paper.pdf         compile only (uses whatever is in tables/ figures/)
#
# DATA must point at the directory the experiment harnesses wrote their CSVs
# into. Every number in the paper comes from there; nothing is hand-entered,
# so a stale DATA produces a visibly stale paper rather than a silently wrong
# one.

DATA    ?= data
PDFLATEX ?= pdflatex
PYTHON  ?= python3

.PHONY: all data clean

all: data paper.pdf

data:
	$(PYTHON) scripts/tables.py $(DATA) tables
	$(PYTHON) scripts/plots.py  $(DATA) figures

# Twice: the second pass resolves \ref/\label forward references.
paper.pdf: paper.tex tables/*.tex figures/*.pdf
	$(PDFLATEX) -interaction=nonstopmode -halt-on-error paper.tex
	$(PDFLATEX) -interaction=nonstopmode -halt-on-error paper.tex

clean:
	rm -f paper.aux paper.log paper.out paper.pdf
