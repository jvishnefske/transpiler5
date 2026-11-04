# Puts the emitrust_bx package on sys.path for the pytest suite (FR-250).
# The tool is a standalone directory, not an installed package, so the
# tests import it the same way emitrust-bx.py does.
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


# --bx-shard=K/N keeps the collected items whose index % N == K (CI fix:
# test/BX/pytest-K.test runs one shard each so no lit test reaches the 600s
# per-test cap). Selection is by item over pytest's deterministic collection
# order, so the heavy property tests spread across shards; the shards
# partition the suite because every index has exactly one residue. Without
# the option the full suite runs.
def pytest_addoption(parser):
    parser.addoption("--bx-shard", default=None, metavar="K/N",
                     help="run only items whose collection index % N == K")


def pytest_collection_modifyitems(config, items):
    spec = config.getoption("--bx-shard")
    if spec is None:
        return
    k, n = (int(x) for x in spec.split("/"))
    if not 0 <= k < n:
        raise ValueError(f"--bx-shard {spec}: need 0 <= K < N")
    kept, dropped = [], []
    for i, item in enumerate(items):
        (kept if i % n == k else dropped).append(item)
    config._bx_shard_summary = f"bx-shard {k}/{n}: kept {len(kept)} of {len(items)}"
    if dropped:
        config.hook.pytest_deselected(items=dropped)
    items[:] = kept


def pytest_terminal_summary(terminalreporter, config):
    summary = getattr(config, "_bx_shard_summary", None)
    if summary:
        terminalreporter.write_line(summary)
