from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]


FUNCTION_RE = re.compile(r"^function\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(", re.MULTILINE)


def lua_quote(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def lua_long_string(value: str) -> str:
    normalized = value.replace("\r\n", "\n").replace("\r", "\n")
    for depth in range(16):
        marker = "=" * depth
        closing = f"]{marker}]"
        if closing not in normalized:
            return f"[{marker}[\n{normalized}\n]{marker}]"
    raise ValueError("cannot find a safe Lua long-string delimiter")


def load_decompiler(decompiler_dir: Path) -> tuple[Any, Any]:
    sys.path.insert(0, str(decompiler_dir))
    from decompile.decompiler import decompile_to_text  # type: ignore
    from decompile.linker import MbcProjectLinker  # type: ignore
    from mbc_format.loader import MbcProject  # type: ignore

    return MbcProject, MbcProjectLinker, decompile_to_text


def make_project_linker(mbc_project_linker: Any, project: Any) -> Any:
    factory = getattr(mbc_project_linker, "from_ffprc_plan", None)
    if callable(factory):
        return factory(project.scripts)
    return mbc_project_linker(project.scripts)


def extract_functions(pseudo_source: str) -> list[str]:
    seen: set[str] = set()
    names: list[str] = []
    for match in FUNCTION_RE.finditer(pseudo_source):
        name = match.group(1)
        if name in seen:
            continue
        seen.add(name)
        names.append(name)
    return names


def render_lua_module(module_name: str, source_name: str, pseudo_source: str) -> str:
    functions = extract_functions(pseudo_source)
    lines: list[str] = [
        "-- Initial Lua migration output for a legacy Sphere script.",
        "-- The decompiler currently emits C-like pseudo-source; this file keeps",
        "-- that source and exposes matching callable stubs for the client runtime.",
        f"-- source: {source_name}",
        "",
        "local M = {}",
        "",
        f"M.__module = {lua_quote(module_name)}",
        f"M.__legacy_source = {lua_quote(source_name)}",
        f"M.__pseudo = {lua_long_string(pseudo_source)}",
        "",
        "M.__exports = {",
    ]

    for name in functions:
        lines.append(f"    {lua_quote(name)},")

    lines.extend(
        [
            "}",
            "",
            "local function dispatch(function_name, ...)",
            '    local sphere_table = rawget(_G, "sphere")',
            '    local runtime = type(sphere_table) == "table" and sphere_table.runtime or nil',
            '    local caller = type(runtime) == "table" and runtime.call_script_stub or nil',
            '    if type(caller) == "function" then',
            "        return caller(M.__module, function_name, M.__pseudo, ...)",
            "    end",
            '    error(("Lua script stub is not implemented: %s.%s"):format(M.__module, function_name), 2)',
            "end",
            "",
        ]
    )

    for name in functions:
        lines.extend(
            [
                f"M[{lua_quote(name)}] = function(...)",
                f"    return dispatch({lua_quote(name)}, ...)",
                "end",
                "",
            ]
        )

    lines.append("return M")
    lines.append("")
    return "\n".join(lines)


def export_project(mbc_dir: Path, out_dir: Path, decompiler_dir: Path, write_pseudo: bool) -> int:
    if not mbc_dir.exists():
        raise FileNotFoundError(f"MBC directory not found: {mbc_dir}")
    if not decompiler_dir.exists():
        raise FileNotFoundError(f"MBC decompiler directory not found: {decompiler_dir}")

    mbc_project, mbc_project_linker, decompile_to_text = load_decompiler(decompiler_dir)
    project = mbc_project.load_dir(mbc_dir)
    linker = make_project_linker(mbc_project_linker, project)

    out_dir.mkdir(parents=True, exist_ok=True)
    pseudo_dir = out_dir / "_pseudo"
    if write_pseudo:
        pseudo_dir.mkdir(parents=True, exist_ok=True)

    modules: list[dict[str, Any]] = []
    failed: list[dict[str, str]] = []

    scripts = sorted(project.scripts, key=lambda script: script.path.name.lower())
    for index, script in enumerate(scripts, start=1):
        source_name = f"mbc/{script.path.name}"
        lua_path = out_dir / f"{script.path.stem}.lua"
        try:
            pseudo_source = decompile_to_text(script, project_linker=linker)
            lua_text = render_lua_module(script.path.stem, source_name, pseudo_source)
            lua_path.write_text(lua_text, encoding="utf-8", newline="\n")
            if write_pseudo:
                (pseudo_dir / f"{script.path.stem}.txt").write_text(pseudo_source, encoding="utf-8", newline="\n")

            functions = extract_functions(pseudo_source)
            modules.append(
                {
                    "module": script.path.stem,
                    "source": source_name,
                    "lua": str(lua_path.relative_to(out_dir).as_posix()),
                    "pseudo": str((Path("_pseudo") / f"{script.path.stem}.txt").as_posix()) if write_pseudo else None,
                    "functions": len(functions),
                    "programs": len(getattr(script, "programs", [])),
                }
            )
            print(f"[{index}/{len(scripts)}] {source_name} -> {lua_path.relative_to(out_dir)}")
        except Exception as exc:
            failed.append({"source": source_name, "error": str(exc)})
            print(f"[{index}/{len(scripts)}] {source_name} -> ERROR: {exc}")

    manifest = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "mbc_dir": str(mbc_dir),
        "decompiler_dir": str(decompiler_dir),
        "format": "legacy-mbc-to-lua-stub-v2",
        "scripts_total": len(scripts),
        "scripts_generated": len(modules),
        "scripts_failed": len(failed),
        "modules": modules,
        "failed": failed,
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8", newline="\n")

    print(f"\nGenerated {len(modules)} Lua modules into {out_dir}")
    if failed:
        print(f"Failed scripts: {len(failed)}")
        return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Export Sphere MBC scripts into Lua module stubs.")
    parser.add_argument("--mbc-dir", type=Path, default=REPO_ROOT / "TestClient" / "mbc")
    parser.add_argument("--out-dir", type=Path, default=REPO_ROOT / "TestClient" / "lua")
    parser.add_argument("--decompiler-dir", type=Path, default=REPO_ROOT / "MBCdecompiler")
    parser.add_argument("--no-pseudo", action="store_true", help="Do not write lua/_pseudo/*.txt audit files.")
    args = parser.parse_args()

    return export_project(
        mbc_dir=args.mbc_dir.resolve(),
        out_dir=args.out_dir.resolve(),
        decompiler_dir=args.decompiler_dir.resolve(),
        write_pseudo=not args.no_pseudo,
    )


if __name__ == "__main__":
    raise SystemExit(main())
