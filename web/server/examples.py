"""The prebaked example catalog, and the startup warm that makes it free.

Each example pins a specific capability worth showing, and each declares the
exact option set it should be viewed with. At boot we compile every
(source, options) pair once and store it under its content address, so the
playground's whole guided tour costs an anonymous visitor nothing.

This is also a soft self-test: if the warm fails for an example, that example
is a live shape the current build cannot handle, and the log says so.
"""

from __future__ import annotations

import asyncio
import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from . import cache as cache_mod
from . import compiler, config

log = logging.getLogger("emitrust.web.examples")


@dataclass(frozen=True)
class Example:
    slug: str
    title: str
    blurb: str
    filename: str
    options: dict[str, Any]

    def source(self) -> str:
        return (Path(config.EXAMPLES_DIR) / self.filename).read_text()


# Ordered: this is the sequence the playground offers as a tour, so it runs
# from "what does this even do" to the genuinely differentiating features.
CATALOG: list[Example] = [
    Example(
        slug="hello",
        title="Hello, Rust",
        blurb=(
            "The smallest complete translation unit. Note that printf becomes "
            "a real println! and main becomes a Rust main -- not a libc shim."
        ),
        filename="hello.c",
        options={"emit": "rust"},
    ),
    Example(
        slug="structs",
        title="Structs and arrays",
        blurb=(
            "C aggregates become plain Rust structs with idiomatic field "
            "names. A field spelled with a Rust keyword is mangled, not "
            "rejected."
        ),
        filename="structs.c",
        options={"emit": "rust"},
    ),
    Example(
        slug="pointers",
        title="Pointers become borrows",
        blurb=(
            "A pointer parameter is not emitted as a raw pointer. The "
            "importer proves which object it can name and lowers it to a "
            "slice or a borrow -- so the output is safe Rust, not unsafe."
        ),
        filename="pointers.c",
        options={"emit": "rust"},
    ),
    Example(
        slug="idiomatic-names",
        title="Idiomatic renaming, on and off",
        blurb=(
            "By default symbols are renamed to Rust convention so the crate "
            "compiles clean under the standard naming lints. Toggle "
            "'preserve C names' to see the verbatim spelling."
        ),
        filename="naming.c",
        options={"emit": "rust"},
    ),
    Example(
        slug="actors",
        title="Mutable globals become an owned actor",
        blurb=(
            "This is the part with no equivalent elsewhere. Mutable file-scope "
            "state is clustered by co-access and lifted into a struct owned by "
            "main, with the functions that touch it becoming &mut self methods. "
            "No static mut, no unsafe, no thread_local survives."
        ),
        filename="actor.c",
        options={"emit": "rust"},
    ),
    Example(
        slug="actor-threaded",
        title="The same actor, as a thread",
        blurb=(
            "Once state is owned, the runtime flavor is a flag. Each actor "
            "moves to a spawned thread behind a mailbox, and every call becomes "
            "a message with a typed reply channel -- so effect order still "
            "equals program order."
        ),
        filename="actor.c",
        options={"emit": "rust", "actor_mode": "threaded"},
    ),
    Example(
        slug="crate",
        title="A whole cargo crate",
        blurb=(
            "Not just a source file: a complete crate with its Cargo.toml, "
            "ready to build. This is the output the end-to-end byte-diff "
            "oracle runs against the clang-built native binary."
        ),
        filename="crate.c",
        options={"emit": "crate"},
    ),
    Example(
        slug="rejection",
        title="Rejection is a feature",
        blurb=(
            "Constructs outside the supported subset get a LOCATED diagnostic "
            "and no output. The project's rule is that a recovered item must "
            "never silently emit wrong code -- so this refuses instead."
        ),
        filename="rejected.c",
        options={"emit": "rust"},
    ),
    Example(
        slug="recover",
        title="...but you can ask it to carry on",
        blurb=(
            "The same source under --recover. The unsupported function becomes "
            "a stub with the right signature so its callers still compile, and "
            "the rejection is reported rather than hidden."
        ),
        filename="rejected.c",
        options={"emit": "rust", "recover": True},
    ),
    Example(
        slug="mlir",
        title="The IR underneath",
        blurb=(
            "The transpiler is an MLIR pipeline. This is the emitrust dialect "
            "the Rust emitter consumes -- useful if you want to see where a "
            "decision was actually made."
        ),
        filename="hello.c",
        options={"emit": "mlir"},
    ),
]


def catalog(cache: cache_mod.ResultCache) -> list[dict[str, Any]]:
    """The menu, annotated with whether each entry is free right now."""
    out = []
    for ex in CATALOG:
        try:
            source = ex.source()
        except OSError:
            continue
        try:
            options = compiler.CompileOptions.parse(ex.options)
        except compiler.OptionError:
            continue
        key = compiler.cache_key(source, options)
        out.append(
            {
                "slug": ex.slug,
                "title": ex.title,
                "blurb": ex.blurb,
                "source": source,
                "options": {
                    f: getattr(options, f) for f in options.__dataclass_fields__
                },
                "key": key,
                "cached": cache.has(key),
            }
        )
    return out


async def warm_cache(cache: cache_mod.ResultCache) -> tuple[int, int]:
    """Compile every catalog entry once. Returns (warmed, failed)."""
    if not config.EMITRUST_CC.exists():
        log.warning(
            "compiler not found at %s; skipping example warm",
            config.EMITRUST_CC,
        )
        return (0, len(CATALOG))

    warmed = failed = 0
    for ex in CATALOG:
        try:
            source = ex.source()
            options = compiler.CompileOptions.parse(ex.options)
        except (OSError, compiler.OptionError) as exc:
            log.warning("example %s is malformed: %s", ex.slug, exc)
            failed += 1
            continue

        key = compiler.cache_key(source, options)
        if cache.has(key):
            warmed += 1
            continue
        try:
            result = await compiler.compile_source(source, options)
            # A non-zero exit is a legitimate cached answer for the two
            # examples that exist to SHOW a rejection; only a timeout is not
            # durable.
            if result.exit_code == 124:
                log.warning("example %s timed out during warm", ex.slug)
                failed += 1
                continue
            # Inside the try on purpose: an unwritable state directory is a
            # plausible first-boot mistake, and it must degrade the service to
            # "no warm examples" rather than stop it from starting at all.
            cache.put(key, result.to_json())
        except Exception as exc:  # pragma: no cover - startup robustness
            log.warning("example %s failed to warm: %s", ex.slug, exc)
            failed += 1
            continue

        warmed += 1
        await asyncio.sleep(0)
    return (warmed, failed)
