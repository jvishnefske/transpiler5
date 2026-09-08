#!/usr/bin/env python3
"""Smoke test for the emitrust web service's dependency-free half.

INTENT: pins the two invariants the service's correctness actually rests on,
neither of which FastAPI can check --
  1. the OPTION WHITELIST really is a whitelist (emitrust-cc inherits LLVM's
     global option registry, so anything that leaks through becomes arbitrary
     compiler flags), and
  2. the SANDBOX limits do not break real compiles. That second one is not
     hypothetical: an RLIMIT_NPROC cap here silently killed every
     --emit=crate and --recover run while letting trivial ones pass, because
     MLIR grows a verifier thread pool and RLIMIT_NPROC is per-UID. A test
     that only compiled hello.c would have missed it.

Runs the REAL compiler -- no mocks, because the failure above only appears
against the real one. Needs the nix devshell:

    nix develop -c python3 web/tests/smoke.py
"""
import asyncio, os, sys, tempfile
from pathlib import Path
HERE = Path(__file__).resolve()
REPO = HERE.parents[2]
EXAMPLES = HERE.parents[1] / "examples"
sys.path.insert(0, str(REPO))

tmp = tempfile.mkdtemp(prefix="emitrust-smoke-")
os.environ["EMITRUST_STATE_DIR"] = tmp
os.environ["EMITRUST_CACHE_DIR"] = tmp + "/cache"
os.environ["EMITRUST_DB"] = tmp + "/trial.sqlite3"
os.environ["EMITRUST_TRIAL_DAILY"] = "3"
os.environ["EMITRUST_TRIAL_LIFETIME"] = "5"

from web.server import cache, compiler, examples, quota, config

fails = []
def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}{'  :: ' + detail if detail and not cond else ''}")
    if not cond: fails.append(name)

print("== option validation (the whitelist) ==")
o = compiler.CompileOptions.parse({"emit": "rust"})
check("default parses", o.emit == "rust" and o.language == "c")
for bad, why in [
    ({"emit": "exe"}, "unknown emit"),
    ({"nope": 1}, "unknown key"),
    ({"language": "python"}, "bad language"),
    ({"incremental": True}, "incremental without crate"),
    ({"actor_mode": "threaded", "no_actor_lift": True}, "contradictory pair"),
    ({"recover": "yes"}, "non-bool"),
]:
    try:
        compiler.CompileOptions.parse(bad); check(f"rejects {why}", False, "accepted it")
    except compiler.OptionError:
        check(f"rejects {why}", True)

print("\n== argv construction (never a shell string) ==")
a = compiler.CompileOptions.parse({"emit": "crate", "incremental": True, "recover": True}).argv()
check("crate+incremental+recover", a == ["--emit=crate", "--recover", "--incremental"], str(a))
a2 = compiler.CompileOptions.parse({"emit": "rust", "actor_mode": "threaded"}).argv()
check("threaded actor", a2 == ["--emit=rust", "--actor-mode=threaded"], str(a2))

print("\n== cache keying ==")
o1 = compiler.CompileOptions.parse({"emit": "rust"})
o2 = compiler.CompileOptions.parse({"emit": "mlir"})
k1, k2 = compiler.cache_key("int main(){}", o1), compiler.cache_key("int main(){}", o2)
check("options change the key", k1 != k2)
check("same input is stable", k1 == compiler.cache_key("int main(){}", o1))
check("source changes the key", k1 != compiler.cache_key("int main(){ }", o1))

c = cache.ResultCache()
check("miss before put", not c.has(k1))
c.put(k1, {"output": "x"})
check("hit after put", c.has(k1) and c.get(k1)["output"] == "x")

print("\n== trial meter (daily 3, lifetime 5) ==")
t = quota.TrialStore()
check("check does not spend", t.check("u1").remaining_today == 3 and t.check("u1").remaining_today == 3)
r = [t.spend("u1").remaining_today for _ in range(3)]
check("spends down to zero", r == [2, 1, 0], str(r))
check("blocks at zero", not t.spend("u1").allowed)
check("reason is user-facing", "cached results are still free" in (t.check("u1").reason or "").lower())
check("other user unaffected", t.check("u2").remaining_today == 3)
t.refund("u1")
check("refund restores one", t.check("u1").remaining_today == 1)

print("\n== real compile through the sandbox ==")
src = open(EXAMPLES / "hello.c").read()
res = asyncio.run(compiler.compile_source(src, compiler.CompileOptions.parse({"emit": "rust"})))
check("hello.c compiles", res.ok and "fn main" in res.output, f"rc={res.exit_code} {res.diagnostics[:120]}")
check("timing recorded", res.duration_ms >= 0)

rej = open(EXAMPLES / "rejected.c").read()
res2 = asyncio.run(compiler.compile_source(rej, compiler.CompileOptions.parse({"emit": "rust"})))
check("rejection is a located diagnostic", not res2.ok and "error:" in res2.diagnostics and ".c:" in res2.diagnostics,
      res2.diagnostics[:160])
res3 = asyncio.run(compiler.compile_source(rej, compiler.CompileOptions.parse({"emit": "rust", "recover": True})))
check("--recover keeps the rest", res3.ok and "tu0_checksum" in res3.output, f"rc={res3.exit_code}")

crate = asyncio.run(compiler.compile_source(open(EXAMPLES / "crate.c").read(),
                                            compiler.CompileOptions.parse({"emit": "crate"})))
paths = sorted(f["path"] for f in crate.files)
check("crate emits Cargo.toml + main.rs", paths == ["Cargo.toml", "src/main.rs"], str(paths))
check("crate primary is main.rs", "fn main" in crate.output)

print("\n== oversize source is refused before exec ==")
try:
    asyncio.run(compiler.compile_source("x" * (config.MAX_SOURCE_BYTES + 1),
                                        compiler.CompileOptions.parse({"emit": "rust"})))
    check("oversize rejected", False, "accepted")
except compiler.OptionError:
    check("oversize rejected", True)

print("\n== example catalog warms ==")
warmed, failed = asyncio.run(examples.warm_cache(c))
check(f"all {len(examples.CATALOG)} examples warm", failed == 0 and warmed == len(examples.CATALOG),
      f"warmed={warmed} failed={failed}")
cat = examples.catalog(c)
check("catalog reports every entry cached", all(e["cached"] for e in cat),
      str([e["slug"] for e in cat if not e["cached"]]))

print("\n" + ("ALL PASS" if not fails else f"{len(fails)} FAILED: {fails}"))
sys.exit(1 if fails else 0)
