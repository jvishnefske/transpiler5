#!/usr/bin/env python3
"""Grammar-level shrinker for miscompiling fuzz seeds.

Given a seed whose generated program MISCOMPILEs, delta-debugs at the
plan level -- never at the token level, so every candidate stays inside
the generator's UB-free grammar:

1. Template-instance reduction: repeatedly try dropping each template
   instance from the plan, keeping any removal that still diverges.
2. Value-pool/parameter reduction: for each surviving instance, walk
   every parameter toward the front of its domain list (domains are
   ordered simplest-first in genprog.TEMPLATES), keeping the simplest
   value that still diverges.

Each candidate plan is re-rendered and re-run through differ.run_pair;
the smallest still-diverging C program is written to --output.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import differ  # noqa: E402
import genprog  # noqa: E402


def parse_args(argv):
    """Parse command-line arguments into an argparse Namespace."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--emitrust-cc", dest="emitrust_cc", required=True)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--workdir", required=True)
    parser.add_argument("--output", required=True, help="Path for the minimized C program.")
    parser.add_argument(
        "--cross-fraction",
        type=float,
        default=genprog.CROSS_FRACTION,
        help="Must match the fuzzing campaign's value to reproduce the seed.",
    )
    parser.add_argument(
        "--max-steps",
        type=int,
        default=400,
        help="Hard ceiling on differ invocations (each is a full build+run).",
    )
    return parser.parse_args(argv)


class Shrinker:
    """Stateful driver: renders candidate plans and re-runs the differ."""

    def __init__(self, emitrust_cc, clang, workdir, seed, max_steps):
        self.emitrust_cc = emitrust_cc
        self.clang = clang
        self.workdir = workdir
        self.seed = seed
        self.max_steps = max_steps
        self.steps = 0

    def diverges(self, plan):
        """Render ``plan`` and return True if it still MISCOMPILEs."""
        if self.steps >= self.max_steps:
            return False
        self.steps += 1
        step_dir = os.path.join(self.workdir, "step-%d" % self.steps)
        os.makedirs(step_dir, exist_ok=True)
        source_path = os.path.join(step_dir, "fuzz_%d.c" % self.seed)
        with open(source_path, "w", encoding="utf-8") as handle:
            handle.write(genprog.render_plan(plan))
        result = differ.run_pair(self.emitrust_cc, self.clang, source_path, step_dir)
        if result.status == differ.HARNESS_BUG:
            raise SystemExit("error: HARNESS_BUG while shrinking: %s" % result.detail)
        return result.status == differ.MISCOMPILE


def reduce_instances(shrinker, plan):
    """Drop template instances while the program still diverges."""
    instances = list(plan.instances)
    changed = True
    while changed and len(instances) > 1:
        changed = False
        for index in range(len(instances)):
            if len(instances) <= 1:
                break
            candidate = instances[:index] + instances[index + 1:]
            trial = plan._replace(instances=candidate)
            if shrinker.diverges(trial):
                print(
                    "  dropped instance u%d (%s); %d left"
                    % (instances[index].uid, instances[index].name, len(candidate))
                )
                instances = candidate
                changed = True
                break
    return plan._replace(instances=instances)


def reduce_params(shrinker, plan):
    """Walk each parameter toward the front of its domain while diverging."""
    instances = list(plan.instances)
    for position, inst in enumerate(instances):
        spec = next(s for s in genprog.TEMPLATES if s.name == inst.name)
        for key in sorted(spec.domains):
            domain = spec.domains[key]
            current = inst.params[key]
            for candidate in domain:
                if candidate == current:
                    break  # Reached the current value; no simpler one diverges.
                new_params = dict(inst.params)
                new_params[key] = candidate
                new_inst = inst._replace(params=new_params)
                trial_instances = list(instances)
                trial_instances[position] = new_inst
                if shrinker.diverges(plan._replace(instances=trial_instances)):
                    print(
                        "  simplified u%d.%s: %r -> %r"
                        % (inst.uid, key, current, candidate)
                    )
                    inst = new_inst
                    instances[position] = new_inst
                    break
    return plan._replace(instances=instances)


def main(argv):
    """Entry point: verify divergence, shrink, and emit the minimized program."""
    args = parse_args(argv)
    os.makedirs(args.workdir, exist_ok=True)
    plan = genprog.plan_program(args.seed, args.cross_fraction)
    shrinker = Shrinker(args.emitrust_cc, args.clang, args.workdir, args.seed, args.max_steps)

    print(
        "seed %d: %d instance(s): %s"
        % (args.seed, len(plan.instances), ", ".join(i.name for i in plan.instances))
    )
    if not shrinker.diverges(plan):
        raise SystemExit(
            "error: seed %d does not MISCOMPILE (check --cross-fraction and"
            " generator version %s)" % (args.seed, genprog.GENERATOR_VERSION)
        )

    plan = reduce_instances(shrinker, plan)
    plan = reduce_params(shrinker, plan)

    source = genprog.render_plan(plan)
    with open(args.output, "w", encoding="utf-8") as handle:
        handle.write(source)
    print(
        "minimized to %d instance(s) [%s] in %d differ step(s): %s"
        % (
            len(plan.instances),
            ", ".join(i.name for i in plan.instances),
            shrinker.steps,
            args.output,
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
