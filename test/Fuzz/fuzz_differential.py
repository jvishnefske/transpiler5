#!/usr/bin/env python3
"""Adversarial differential fuzzing driver for emitrust-cc.

Generates one deterministic C program per seed (genprog.py), runs each
through the native-vs-transpiled differ (differ.py) on a thread pool,
and reports PASS / UNSUPPORTED / MISCOMPILE counts plus per-template
accept-rate statistics.  A template whose PASS+MISCOMPILE rate is ~0%%
is testing nothing (everything it touches gets rejected) and is
surfaced in the report.

Any HARNESS_BUG (native leg failed to compile or run) is a generator
defect and fails the run loudly regardless of flags.  MISCOMPILEs save
artifacts (seed, prog.c, both rc/stdout pairs, crate-dir pointer) under
--artifacts and fail the run when --fail-on-miscompile is given.

Cargo target-dir strategy: the driver probes once whether emitrust-cc's
cargo child inherits CARGO_TARGET_DIR (binary lands at
``$CARGO_TARGET_DIR/release/<crate>``).  If so, all builds share one
target dir under the workdir; otherwise it falls back to per-crate
target dirs.  differ.py handles both layouts.

Example campaign:
    nix develop -c python3 test/Fuzz/fuzz_differential.py \\
        --emitrust-cc build/bin/emitrust-cc \\
        --start 0 --count 5000 --jobs 8 --artifacts /tmp/fuzz-out
"""

import argparse
import os
import shutil
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import differ  # noqa: E402
import genprog  # noqa: E402
from run_c_testsuite import TRANSPILE_BUILD_TIMEOUT, run_command  # noqa: E402


def parse_args(argv):
    """Parse command-line arguments into an argparse Namespace."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emitrust-cc", dest="emitrust_cc", required=True)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--start", type=int, default=0, help="First seed (inclusive).")
    parser.add_argument("--count", type=int, default=100, help="Number of seeds.")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--artifacts", default=None, help="Directory for MISCOMPILE artifacts.")
    parser.add_argument(
        "--fail-on-miscompile",
        action="store_true",
        help="Exit nonzero if any seed classifies MISCOMPILE.",
    )
    parser.add_argument(
        "--workdir",
        default=None,
        help="Scratch directory (default: a fresh temp dir, removed on success).",
    )
    parser.add_argument(
        "--cross-fraction",
        type=float,
        default=genprog.CROSS_FRACTION,
        help="Fraction of seeds forced to combine >=2 pointer-provenance templates.",
    )
    parser.add_argument(
        "--keep-work",
        action="store_true",
        help="Keep per-seed work directories even on PASS/UNSUPPORTED.",
    )
    return parser.parse_args(argv)


def resolve_tool(value):
    """Resolve a tool argument to an absolute executable path (PATH fallback)."""
    if os.sep in value:
        candidate = os.path.abspath(value)
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
        raise SystemExit("error: not executable: %s" % candidate)
    found = shutil.which(value)
    if found is None:
        raise SystemExit("error: cannot find %r on PATH" % value)
    return found


def probe_cargo_target_dir(emitrust_cc, workdir):
    """Probe whether emitrust-cc's cargo child inherits CARGO_TARGET_DIR.

    Sets ``CARGO_TARGET_DIR`` in os.environ to a shared dir under the
    workdir, transpiles a one-liner, and checks the binary landed at
    ``$CARGO_TARGET_DIR/release/<crate>``.  On failure the variable is
    removed again and every crate falls back to its own target dir.
    Returns True when the shared layout is active.
    """
    shared = os.path.join(workdir, "shared-target")
    probe_dir = os.path.join(workdir, "cargo-probe")
    os.makedirs(probe_dir, exist_ok=True)
    source = os.path.join(probe_dir, "probe_cargo.c")
    with open(source, "w", encoding="utf-8") as handle:
        handle.write(
            "int printf(const char *, ...);\n"
            'int main(void) { printf("probe\\n"); return 0; }\n'
        )
    os.environ["CARGO_TARGET_DIR"] = shared
    rc, _stdout, _stderr, timed_out = run_command(
        [emitrust_cc, "--emit=crate", source, "-o", os.path.join(probe_dir, "crate"),
         "--crate-name", "probe_cargo", "--build"],
        TRANSPILE_BUILD_TIMEOUT,
    )
    landed = os.path.isfile(os.path.join(shared, "release", "probe_cargo"))
    if timed_out or rc != 0 or not landed:
        del os.environ["CARGO_TARGET_DIR"]
        return False
    return True


def run_seed(emitrust_cc, clang, seed, seeds_dir, cross_fraction, keep_work):
    """Generate, differentially run, and classify one seed.

    Returns (seed, program, PairResult, source_path).  Work products of
    non-MISCOMPILE seeds are cleaned up unless ``keep_work``.
    """
    program = genprog.generate_program(seed, cross_fraction)
    seed_dir = os.path.join(seeds_dir, "fuzz_%d" % seed)
    os.makedirs(seed_dir, exist_ok=True)
    source_path = os.path.join(seed_dir, "fuzz_%d.c" % seed)
    with open(source_path, "w", encoding="utf-8") as handle:
        handle.write(program.source)
    result = differ.run_pair(emitrust_cc, clang, source_path, seed_dir)
    if result.status != differ.MISCOMPILE and not keep_work:
        # Drop the bulky build products; keep the source for reference.
        for name in ("native", "crate"):
            path = os.path.join(seed_dir, name)
            if os.path.isdir(path):
                shutil.rmtree(path, ignore_errors=True)
            elif os.path.isfile(path):
                os.unlink(path)
        binary = os.path.join(
            os.environ.get("CARGO_TARGET_DIR", ""), "release", "fuzz_%d" % seed
        )
        if os.environ.get("CARGO_TARGET_DIR") and os.path.isfile(binary):
            os.unlink(binary)
    return seed, program, result, source_path


def save_artifacts(artifacts_dir, seed, program, result, source_path):
    """Persist a MISCOMPILE's repro bundle under ``artifacts_dir``."""
    bundle = os.path.join(artifacts_dir, "seed-%d" % seed)
    os.makedirs(bundle, exist_ok=True)
    shutil.copy(source_path, os.path.join(bundle, "prog.c"))
    with open(os.path.join(bundle, "meta.txt"), "w", encoding="utf-8") as handle:
        handle.write("seed=%d\n" % seed)
        handle.write("generator_version=%s\n" % genprog.GENERATOR_VERSION)
        handle.write("templates=%s\n" % ",".join(program.template_names))
        handle.write("detail=%s\n" % result.detail)
        handle.write("native_rc=%s\nrust_rc=%s\n" % (result.native_rc, result.rust_rc))
        handle.write("crate_dir=%s\n" % result.crate_dir)
    with open(os.path.join(bundle, "native.out"), "wb") as handle:
        handle.write(result.native_out)
    with open(os.path.join(bundle, "rust.out"), "wb") as handle:
        handle.write(result.rust_out)
    return bundle


def main(argv):
    """Entry point: fan seeds across a thread pool and enforce the verdicts."""
    args = parse_args(argv)
    emitrust_cc = resolve_tool(args.emitrust_cc)
    clang = resolve_tool(args.clang)

    workdir = args.workdir or tempfile.mkdtemp(prefix="fuzzdiff-")
    os.makedirs(workdir, exist_ok=True)
    seeds_dir = os.path.join(workdir, "seeds")
    os.makedirs(seeds_dir, exist_ok=True)

    shared = probe_cargo_target_dir(emitrust_cc, workdir)
    print(
        "cargo target dir: %s"
        % ("shared (%s)" % os.environ["CARGO_TARGET_DIR"] if shared else "per-crate fallback")
    )

    seeds = list(range(args.start, args.start + args.count))
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(
            pool.map(
                lambda seed: run_seed(
                    emitrust_cc, clang, seed, seeds_dir, args.cross_fraction, args.keep_work
                ),
                seeds,
            )
        )

    counts = {differ.PASS: 0, differ.UNSUPPORTED: 0, differ.MISCOMPILE: 0, differ.HARNESS_BUG: 0}
    template_stats = {spec.name: {"seeds": 0, "exercised": 0} for spec in genprog.TEMPLATES}
    miscompiles = []
    harness_bugs = []
    for seed, program, result, source_path in results:
        counts[result.status] += 1
        for name in set(program.template_names):
            template_stats[name]["seeds"] += 1
            if result.status in (differ.PASS, differ.MISCOMPILE):
                template_stats[name]["exercised"] += 1
        if result.status == differ.MISCOMPILE:
            miscompiles.append((seed, program, result, source_path))
        elif result.status == differ.HARNESS_BUG:
            harness_bugs.append((seed, result))

    for seed, result in harness_bugs:
        print("HARNESS_BUG seed=%d: %s" % (seed, result.detail))
    for seed, program, result, _source in miscompiles:
        print("MISCOMPILE seed=%d templates=%s" % (seed, ",".join(program.template_names)))
        print("  " + result.detail.replace("\n", "\n  "))
        if args.artifacts:
            bundle = save_artifacts(args.artifacts, seed, program, result, _source)
            print("  artifacts: %s" % bundle)

    print("\nper-template accept rates (exercised = PASS or MISCOMPILE):")
    dead_templates = []
    for name in sorted(template_stats):
        stat = template_stats[name]
        if stat["seeds"] == 0:
            print("  %-12s   (not drawn in this seed range)" % name)
            continue
        rate = 100.0 * stat["exercised"] / stat["seeds"]
        print("  %-12s %4d seeds, %5.1f%% exercised" % (name, stat["seeds"], rate))
        if stat["exercised"] == 0:
            dead_templates.append(name)
    if dead_templates:
        print(
            "WARNING: template(s) with 0%% accept rate are testing nothing: %s"
            % ", ".join(dead_templates)
        )

    print(
        "\nsummary: seeds=%d pass=%d unsupported=%d miscompile=%d harness_bug=%d"
        " (generator v%s, cross-fraction %.2f)"
        % (
            len(seeds),
            counts[differ.PASS],
            counts[differ.UNSUPPORTED],
            counts[differ.MISCOMPILE],
            counts[differ.HARNESS_BUG],
            genprog.GENERATOR_VERSION,
            args.cross_fraction,
        )
    )

    if harness_bugs:
        print("FAIL: %d HARNESS_BUG(s) -- generator defect, fix genprog.py" % len(harness_bugs))
        return 2
    if miscompiles and args.fail_on_miscompile:
        print(
            "FAIL: %d MISCOMPILE(s); minimize with:"
            " python3 test/Fuzz/minimize.py --seed <seed> ..." % len(miscompiles)
        )
        return 1
    if not args.workdir and not args.keep_work and not miscompiles:
        shutil.rmtree(workdir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
