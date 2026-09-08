"""The emitrust-cc wrapper: option validation, sandboxed invocation, result shape.

TRANSPILE ONLY. This module never builds or runs the emitted Rust. `cargo
build` on attacker-supplied source would execute build scripts, and running
the emitted binary would execute attacker C semantics; neither is exposed by
the product, so neither is implemented here. The one thing the service does
with untrusted input is run a clang-based frontend over it, which is why the
option surface below is a WHITELIST -- emitrust-cc inherits LLVM's global
option registry (several hundred flags, including ones that write files), so
an allow-list is the only safe way to accept user options.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import os
import resource
import shutil
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from . import config


class OptionError(ValueError):
    """A rejected option set. The message is shown to the user verbatim."""


# --- the option whitelist ------------------------------------------------
#
# Every key is a form field the playground can send. Anything not listed is
# rejected by name. Values are validated against the enumerations below, never
# interpolated into a shell (we always exec a list, never a string).

EMIT_KINDS = {
    "rust": ["--emit=rust"],
    "crate": ["--emit=crate"],
    "mlir": ["--emit=mlir"],
    "actor-plan": ["--emit=actor-plan"],
}

ACTOR_MODES = {
    "same-thread": [],  # the default; no flag
    "threaded": ["--actor-mode=threaded"],
    "async": ["--actor-mode=async"],
}

# Boolean flags: field name -> argv to append when true.
BOOL_FLAGS = {
    "preserve_c_names": ["--preserve-c-names"],
    "recover": ["--recover"],
    "c_abi_exports": ["--c-abi-exports"],
    "incremental": ["--incremental"],
    "no_actor_lift": ["--actor-lift=false"],
}

LANGUAGES = {"c": ".c", "cpp": ".cpp"}


@dataclass(frozen=True)
class CompileOptions:
    """A validated, canonical option set. Hashable -> usable as a cache key."""

    emit: str = "rust"
    language: str = "c"
    actor_mode: str = "same-thread"
    preserve_c_names: bool = False
    recover: bool = False
    c_abi_exports: bool = False
    incremental: bool = False
    no_actor_lift: bool = False

    @staticmethod
    def parse(raw: dict[str, Any] | None) -> "CompileOptions":
        raw = dict(raw or {})
        unknown = set(raw) - {f for f in CompileOptions.__dataclass_fields__}
        if unknown:
            raise OptionError(f"unknown option(s): {', '.join(sorted(unknown))}")

        emit = str(raw.get("emit", "rust"))
        if emit not in EMIT_KINDS:
            raise OptionError(
                f"emit must be one of {', '.join(sorted(EMIT_KINDS))}"
            )
        language = str(raw.get("language", "c"))
        if language not in LANGUAGES:
            raise OptionError("language must be 'c' or 'cpp'")
        actor_mode = str(raw.get("actor_mode", "same-thread"))
        if actor_mode not in ACTOR_MODES:
            raise OptionError(
                f"actor_mode must be one of {', '.join(sorted(ACTOR_MODES))}"
            )

        bools = {}
        for name in BOOL_FLAGS:
            value = raw.get(name, False)
            if not isinstance(value, bool):
                raise OptionError(f"{name} must be a boolean")
            bools[name] = value

        opts = CompileOptions(
            emit=emit, language=language, actor_mode=actor_mode, **bools
        )
        opts.validate()
        return opts

    def validate(self) -> None:
        """Reject combinations the driver itself refuses, with a better message."""
        if self.incremental and self.emit != "crate":
            raise OptionError("incremental output requires emit=crate")
        if self.actor_mode != "same-thread" and self.no_actor_lift:
            raise OptionError(
                "actor_mode requires the actor lift; clear 'no actor lift'"
            )

    def argv(self) -> list[str]:
        args = list(EMIT_KINDS[self.emit])
        args += ACTOR_MODES[self.actor_mode]
        for name, flag in BOOL_FLAGS.items():
            if getattr(self, name):
                args += flag
        return args

    def canonical(self) -> str:
        """Stable JSON for the cache key. Field order is the dataclass order."""
        return json.dumps(
            {f: getattr(self, f) for f in self.__dataclass_fields__},
            sort_keys=True,
            separators=(",", ":"),
        )


@dataclass
class CompileResult:
    ok: bool
    output: str
    diagnostics: str
    exit_code: int
    duration_ms: int
    emit: str
    truncated: bool = False
    files: list[dict[str, str]] = field(default_factory=list)

    def to_json(self) -> dict[str, Any]:
        return {
            "ok": self.ok,
            "output": self.output,
            "diagnostics": self.diagnostics,
            "exit_code": self.exit_code,
            "duration_ms": self.duration_ms,
            "emit": self.emit,
            "truncated": self.truncated,
            "files": self.files,
        }


def cache_key(source: str, options: CompileOptions) -> str:
    """Content address for (source, options). Also the cache filename."""
    h = hashlib.sha256()
    h.update(b"emitrust-web-v1\0")
    h.update(options.canonical().encode())
    h.update(b"\0")
    h.update(source.encode("utf-8", "surrogatepass"))
    return h.hexdigest()


def _apply_rlimits() -> None:
    """Run in the child between fork and exec. Bounds one invocation."""
    resource.setrlimit(
        resource.RLIMIT_CPU,
        (config.COMPILE_CPU_SECONDS, config.COMPILE_CPU_SECONDS),
    )
    resource.setrlimit(
        resource.RLIMIT_AS,
        (config.COMPILE_ADDRESS_SPACE, config.COMPILE_ADDRESS_SPACE),
    )
    resource.setrlimit(
        resource.RLIMIT_FSIZE,
        (config.COMPILE_MAX_OUTPUT_BYTES, config.COMPILE_MAX_OUTPUT_BYTES),
    )
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    # Deliberately NO RLIMIT_NPROC. It is per-UID, not per-process, so it
    # counts every process the service user already has -- and MLIR's verifier
    # grows a thread pool sized to the machine, which then fails with
    # "LLVM ERROR: pthread_create failed" and aborts the compile. Measured:
    # a cap of 64 killed every --emit=crate and --recover run while letting
    # trivial ones through, which is the worst possible failure shape.
    # Thread/process count is bounded by TasksMax= in the systemd unit, which
    # is per-service and therefore the correct mechanism.
    os.setsid()


_semaphore: asyncio.Semaphore | None = None


def _get_semaphore() -> asyncio.Semaphore:
    global _semaphore
    if _semaphore is None:
        _semaphore = asyncio.Semaphore(config.MAX_CONCURRENT_COMPILES)
    return _semaphore


async def compile_source(source: str, options: CompileOptions) -> CompileResult:
    """Transpile `source` in a scratch directory. Never raises on user error."""
    if len(source.encode()) > config.MAX_SOURCE_BYTES:
        raise OptionError(
            f"source exceeds {config.MAX_SOURCE_BYTES // 1024} KiB"
        )

    async with _get_semaphore():
        return await asyncio.get_running_loop().run_in_executor(
            None, _compile_blocking, source, options
        )


def _compile_blocking(source: str, options: CompileOptions) -> CompileResult:
    import subprocess
    import time

    workdir = Path(tempfile.mkdtemp(prefix="emitrust-web-"))
    try:
        src = workdir / f"input{LANGUAGES[options.language]}"
        src.write_text(source)
        outdir = workdir / "out"

        argv: list[str] = []
        if config.NIX_DEVELOP:
            argv += [config.NIX_DEVELOP, "develop", "-c"]
        argv += [str(config.EMITRUST_CC)]
        argv += options.argv()
        argv += [str(src)]
        if options.emit == "crate":
            argv += ["-o", str(outdir)]

        started = time.monotonic()
        try:
            proc = subprocess.run(
                argv,
                cwd=workdir,
                capture_output=True,
                text=True,
                timeout=config.COMPILE_TIMEOUT_S,
                preexec_fn=_apply_rlimits,
                env={
                    "PATH": "/usr/bin:/bin",
                    "HOME": str(workdir),
                    "TMPDIR": str(workdir),
                    "LC_ALL": "C",
                },
            )
            stdout, stderr, code = proc.stdout, proc.stderr, proc.returncode
        except subprocess.TimeoutExpired:
            return CompileResult(
                ok=False,
                output="",
                diagnostics=(
                    f"timed out after {config.COMPILE_TIMEOUT_S:g}s.\n"
                    "The transpiler bounds its own work, so a timeout usually "
                    "means the input hit a pathological shape. If you think "
                    "that is a bug, it probably is -- please report it."
                ),
                exit_code=124,
                duration_ms=int(config.COMPILE_TIMEOUT_S * 1000),
                emit=options.emit,
            )
        duration_ms = int((time.monotonic() - started) * 1000)

        files: list[dict[str, str]] = []
        if options.emit == "crate" and outdir.is_dir():
            output, files = _collect_crate(outdir)
        else:
            output = stdout

        truncated = False
        if len(output) > config.COMPILE_MAX_OUTPUT_BYTES:
            output = output[: config.COMPILE_MAX_OUTPUT_BYTES]
            truncated = True

        return CompileResult(
            ok=(code == 0),
            output=output,
            diagnostics=stderr.strip(),
            exit_code=code,
            duration_ms=duration_ms,
            emit=options.emit,
            truncated=truncated,
            files=files,
        )
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def _collect_crate(outdir: Path) -> tuple[str, list[dict[str, str]]]:
    """Flatten an emitted crate into (primary source, [{path, content}])."""
    files: list[dict[str, str]] = []
    budget = config.COMPILE_MAX_OUTPUT_BYTES
    for path in sorted(outdir.rglob("*")):
        if not path.is_file():
            continue
        try:
            text = path.read_text()
        except (UnicodeDecodeError, OSError):
            continue
        if len(text) > budget:
            text = text[:budget]
        budget -= len(text)
        files.append({"path": str(path.relative_to(outdir)), "content": text})
        if budget <= 0:
            break

    primary = ""
    for candidate in ("src/main.rs", "src/lib.rs"):
        for f in files:
            if f["path"] == candidate:
                primary = f["content"]
                break
        if primary:
            break
    return primary, files
