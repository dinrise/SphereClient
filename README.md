# Sphere NewClient

This directory contains the C++ client rewrite and reverse-engineering tools.
It is intentionally isolated from `SphereEmu`.

## Targets

- `sphere_client.exe` - minimal Win32 client shell that reads the original client configs and opens a client window.
- `mbc_dump.exe` - parser for Sphere `MBL script v4.0` `.mbc` and `.adb` files.
- `pe_imports.exe` - PE import-table dumper for the original `sphereclient_patched.exe`.
- `scripts\export_mbc_lua.py` - exports the original `.mbc` project into Lua module stubs under `TestClient\lua`.

## Build

The Win32 client embeds Lua through the vendored source package in
`J:\dev\sphereonline\NewClient\third_party\lua-5.5.0`:

- `lua55.lib` is built by CMake with the same MSVC runtime settings as the client.
- `lua.exe` and `luac.exe` are built into `build-vs18-win32\bin\<Config>` for local script checks.

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S J:\dev\sphereonline\NewClient -B J:\dev\sphereonline\NewClient\build-vs18-win32 -G "Visual Studio 18 2026" -A Win32
& "C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build J:\dev\sphereonline\NewClient\build-vs18-win32 --config Release
```

The original client is a 32-bit PE and uses Direct3D 9 era dependencies, so the
main client target should stay Win32 while compatibility is being matched.

## Useful Commands

```powershell
.\build-vs18-win32\bin\Release\pe_imports.exe J:\dev\sphereonline\Sph_newyear\sphereclient_patched.exe
.\build-vs18-win32\bin\Release\mbc_dump.exe J:\dev\sphereonline\Sph_newyear\mbc\_main.mbc --adb J:\dev\sphereonline\Sph_newyear\mbc\_main.adb --limit 20
.\build-vs18-win32\bin\Release\sphere_client.exe --root J:\dev\sphereonline\Sph_newyear
python J:\dev\sphereonline\NewClient\scripts\export_mbc_lua.py --mbc-dir J:\dev\sphereonline\TestClient\mbc --out-dir J:\dev\sphereonline\TestClient\lua --decompiler-dir J:\dev\sphereonline\MBCdecompiler
Get-ChildItem J:\dev\sphereonline\TestClient\lua -Filter *.lua | ForEach-Object { J:\dev\sphereonline\NewClient\build-vs18-win32\bin\Release\luac.exe -p $_.FullName }
```

For original-client folder tests:

```powershell
Copy-Item J:\dev\sphereonline\NewClient\build-vs18-win32\bin\Release\sphere_client.exe J:\dev\sphereonline\TestClient\sphere_newclient.exe -Force
J:\dev\sphereonline\TestClient\sphere_newclient.exe
```

When `--root` is omitted, `sphere_client.exe` uses the directory containing the
exe. This matches the original client-style deployment where the exe is placed
next to `config.cfg`, `connect.cfg`, `lua`, `params`, and other client assets.

## Current Scope

This is not yet a playable replacement. The first layer is a native C++ exe and
repeatable reverse-engineering tooling. The original MBC project can now be
mirrored into valid Lua modules, and `sphere_client.exe` initializes a Lua VM
from `lua\_main.lua` at startup. The generated modules still keep decompiled
pseudo-source behind script stubs, so gameplay behavior has to be ported into
real Lua code. The client runtime should depend on Lua only; MBC stays in
migration tools. The next layers are Lua script semantics, network protocol,
resource loading, Direct3D 9 renderer, input, and audio.
