#!/usr/bin/env python3
"""Write THIRD-PARTY-NOTICES for a build of libpandoc, from cabal's plan.

Every Haskell package linked into the artifact (the component's dependency
closure in plan.json), with its version, its licence (the installed
package's) and the licence files it ships (cabal's store, or GHC's own
documentation for GHC's packages); then the C libraries those packages
link (their extra-libraries, and the runtime's libffi), how this artifact
has each (inside it, or the system's), and their texts from licenses/. A
package or C library whose licence can't be found fails the build: the
file is complete or there is none.

    notices.py --plan dist-newstyle/cache/plan.json --component flib:pandoc \\
        --store STORE --ghc-libdir LIBDIR --target linux --prefix dist --out FILE
"""
import argparse
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TEXTS = os.path.join(HERE, "..", "licenses")

# What libpandoc knows of the C libraries Haskell packages link, by the name
# they link (per target where it differs): what it is, its licence, its
# texts in licenses/. GPL-2.0 is COPYING.md, beside this file.
C_LIBRARIES = {
    "gmp": ("GMP (GHC's integers)", "LGPL-3.0-or-later OR GPL-2.0-or-later",
            ["gmp.txt", "LGPL-3.0.txt", "GPL-3.0.txt"]),
    "ffi": ("libffi (GHC's runtime)", "MIT", ["libffi.txt"]),
    "ffi@wasm": ("libffi-wasm (GHC's runtime)", "MIT", ["libffi-wasm.txt"]),
    "z": ("zlib", "Zlib", ["zlib.txt"]),
    "stdc++": ("libstdc++ and libgcc (GCC; for simdutf, in text)",
               "GPL-3.0-or-later WITH GCC-exception-3.1", ["GCC-exception-3.1.txt", "GPL-3.0.txt"]),
    "c++": ("libc++ and libc++abi (LLVM)", "Apache-2.0 WITH LLVM-exception", ["LLVM.txt"]),
    "wasi-libc": ("wasi-libc (wasi-sdk), with wasi-emulated-*, setjmp",
                  "MIT (of Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT)", ["wasi-libc.txt"]),
    "compiler-rt": ("compiler-rt's builtins (LLVM)", "Apache-2.0 WITH LLVM-exception", ["LLVM.txt"]),
    "mingw-w64": ("the MinGW-w64 runtime and winpthreads", "ZPL-2.1 and others (its notice)",
                  ["mingw-w64-runtime.txt"]),
}
# names that are another's
ALIASES = {"Cffi": "ffi", "c++abi": "c++", "gcc_s": "stdc++", "stdc++fs": "stdc++",
           "mingw32": "mingw-w64", "mingwex": "mingw-w64", "pthread@windows": "mingw-w64",
           "gcc@windows": "compiler-rt", "unwind": "c++", "setjmp": "wasi-libc",
           "wasi-emulated-process-clocks": "wasi-libc", "wasi-emulated-signal": "wasi-libc",
           "wasi-emulated-getpid": "wasi-libc", "wasi-emulated-mman": "wasi-libc",
           "c@wasm": "wasi-libc", "m@wasm": "wasi-libc", "dl@wasm": "wasi-libc", "rt@wasm": "wasi-libc"}
# the operating system's own (its C library, Windows' DLLs): not ours to list
SYSTEM = {"c", "m", "rt", "dl", "pthread", "util", "atomic", "iconv", "charset", "resolv"}
WINDOWS_SYSTEM = {"kernel32", "user32", "gdi32", "shell32", "advapi32", "ws2_32", "wsock32", "shlwapi",
                  "ole32", "oleaut32", "rpcrt4", "ntdll", "ucrt", "ucrtbase", "msvcrt", "dbghelp", "psapi",
                  "winmm", "version", "bcrypt", "crypt32", "iphlpapi", "secur32", "userenv", "uuid",
                  "mswsock", "synchronization", "api-ms-win-core-synch-l1-2-0", "mincore", "dnsapi",
                  "ncrypt", "comctl32", "comdlg32", "imm32", "setupapi", "pathcch", "moldname", "m", "c"}


def conf_fields(path):
    """An installed package's .conf: its fields, continuation lines joined."""
    fields, key = {}, None
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line[:1].isspace() and key:
                fields[key] += " " + line.strip()
            elif ":" in line:
                key, value = line.split(":", 1)
                fields[key] = value.strip()
    return fields


def closure(plan, component):
    units = {u["id"]: u for u in plan["install-plan"]}
    roots = [i for i, u in units.items()
             if u["pkg-name"] == "libpandoc" and u.get("component-name") == component]
    if not roots:
        sys.exit(f"notices: no {component} in the plan")
    seen, todo = set(), list(roots)
    while todo:
        i = todo.pop()
        if i in seen:
            continue
        seen.add(i)
        u = units[i]
        todo += u.get("depends", [])
        for name, c in u.get("components", {}).items():
            if name.startswith("lib"):
                todo += c.get("depends", [])
    return [units[i] for i in sorted(seen) if units[i]["pkg-name"] != "libpandoc"]


def doc_index(ghc_libdir):
    """GHC's own packages' documentation directories (name-version: path),
    where their licence files are: share/doc/ARCH-ghc-VER/ (ghcup) or
    doc/ARCH-ghc-VER/ (the wasm toolchain), somewhere above its libdir."""
    index, roots = {}, []
    d = os.path.abspath(ghc_libdir)
    for _ in range(4):
        roots += [os.path.join(d, "share", "doc"), os.path.join(d, "doc")]
        d = os.path.dirname(d)
    for root in roots:
        for top in glob.glob(os.path.join(root, "*")):
            for p in glob.glob(os.path.join(top, "*")) + [top]:
                if os.path.isdir(p) and licence_files(p):
                    index.setdefault(os.path.basename(p), p)
    return index


def licence_files(path):
    return sorted(p for p in glob.glob(os.path.join(path, "*"))
                  if os.path.isfile(p) and re.match(r"(?i)(licen[cs]e|copying|copyright|notice)",
                                                    os.path.basename(p)))


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().rstrip() + "\n"


def main():
    a = argparse.ArgumentParser()
    a.add_argument("--plan", required=True)
    a.add_argument("--component", required=True)
    a.add_argument("--store", required=True)
    a.add_argument("--ghc-libdir", required=True)
    a.add_argument("--target", required=True, choices=["linux", "darwin", "windows", "wasm"])
    a.add_argument("--prefix", help="the staged prefix: what of the C libraries it has")
    a.add_argument("--artifact", required=True, help="what this file is for, in a sentence")
    a.add_argument("--out", required=True)
    o = a.parse_args()
    with open(o.plan, encoding="utf-8") as f:
        plan = json.load(f)
    docs = doc_index(o.ghc_libdir)
    ghc = plan["compiler-id"]
    packages, missing, linked = [], [], set()
    for u in closure(plan, o.component):
        name, version, uid = u["pkg-name"], u["pkg-version"], u["id"]
        confs = glob.glob(os.path.join(o.store, "*", "package.db", uid + ".conf")) + \
            glob.glob(os.path.join(o.ghc_libdir, "package.conf.d", uid + ".conf"))
        fields = conf_fields(confs[0]) if confs else {}
        for lib in fields.get("extra-libraries", "").split() + \
                [h for h in fields.get("hs-libraries", "").split() if h == "Cffi"]:
            linked.add(lib)
        if name == "system-cxx-std-lib":  # the C++ library, below
            continue
        if u["type"] == "pre-existing":
            # GHC's own; the runtime's licence is GHC's
            where = docs.get(ghc if name == "rts" else f"{name}-{version}")
        else:
            where = next(iter(glob.glob(os.path.join(o.store, "*", uid, "share", "doc"))), None)
        files = licence_files(where) if where else []
        if not fields.get("license") or (not files and u["type"] == "pre-existing"):
            missing.append(f"{name}-{version} ({uid})")
            continue
        if not files:
            # (a package that ships no licence file: what its description says)
            print(f"notices: {name}-{version} ships no licence file", file=sys.stderr)
            files = [{k: fields[k] for k in ("license", "copyright", "author") if fields.get(k)}]
        source = " (from git, patched: cabal-wasm.project)" if u.get("pkg-src", {}).get("type") == "source-repo" else ""
        if not any(p[:2] == (name, version) for p in packages):  # (a package's sublibraries: once)
            packages.append((name, version, fields["license"], source, files))
    # the C libraries
    clibs = {}
    implied = {"wasm": ["wasi-libc", "compiler-rt"], "windows": ["mingw-w64"]}.get(o.target, [])
    for lib in sorted(linked) + implied:
        key = ALIASES.get(f"{lib}@{o.target}", ALIASES.get(lib, lib))
        key = f"{key}@{o.target}" if f"{key}@{o.target}" in C_LIBRARIES else key
        if key in C_LIBRARIES:
            clibs[key] = C_LIBRARIES[key]
        elif lib in SYSTEM or (o.target == "windows" and lib.lower() in WINDOWS_SYSTEM):
            continue
        else:
            missing.append(f"the C library {lib} (add it to scripts/notices.py and licenses/)")
    if missing:
        sys.exit("notices: no licence found for:\n  " + "\n  ".join(missing))

    def how(key):
        if o.target in ("windows", "wasm"):
            return "linked in (statically)"
        stem = {"stdc++": "libstdc++", "c++": "libc++"}.get(key, "lib" + key.split("@")[0])
        found = glob.glob(os.path.join(o.prefix or "", "lib", "libpandoc", stem + "[.-]*"))
        if found:
            return "included: lib/libpandoc/" + os.path.basename(sorted(found)[0])
        return "the system's, linked dynamically: not in this archive (a binding that bundles it carries this notice)"

    out = []
    w = out.append
    w("Third-party notices\n===================\n\n")
    w(f"{o.artifact} It is libpandoc's own code, GPL-2.0-or-later (COPYING.md), "
      "linked with what is listed below, each under its own licence.\n\n")
    only = [p[0] for p in packages if p[2] == "GPL-2.0-only"]
    w("This file is written by the build (scripts/notices.py) from cabal's build plan: "
      f"{len(packages)} Haskell packages built by {ghc}, and the C libraries they link."
      + (f" Of them {', '.join(only)} {'is' if len(only) == 1 else 'are'} GPL-2.0-only, so the "
         "whole is distributed under the GPL, version 2, as pandoc's own binaries are." if only else "")
      + "\n\n")
    w("The complete corresponding source is that of https://github.com/ickc/libpandoc at the "
      "commit the release was built from (its build provenance attestation names it), whose "
      "pins.env, cabal.project and cabal-wasm.project fix every other input: pandoc's version, "
      "Hackage's index state (every package below, at the version listed, from Hackage then), "
      "GHC's and cabal's versions, ghc-wasm-meta's commit, and the git packages by commit with "
      "the patches in wasm/patches.\n\n")
    w("C libraries\n-----------\n\n")
    for key, (what, lic, _) in sorted(clibs.items()):
        w(f"- {what}: {lic}; {how(key)}\n")
    w("\nHaskell packages\n----------------\n\n")
    for name, version, lic, source, _ in packages:
        w(f"- {name} {version}: {lic}{source}\n")
    for name, version, lic, source, files in packages:
        w(f"\n\n{'=' * 72}\n{name} {version} ({lic})\n{'=' * 72}\n")
        for p in files:
            if isinstance(p, dict):
                w("\n(It ships no licence file. Its package description says:)\n\n"
                  + "".join(f"{k}: {v}\n" for k, v in p.items()))
            else:
                w(f"\n--- {os.path.basename(p)}\n\n{read(p)}")
    texts = []
    for _, (_, _, names) in sorted(clibs.items()):
        texts += [n for n in names if n not in texts]
    for n in texts:
        w(f"\n\n{'=' * 72}\n{n[:-4]} (licenses/{n})\n{'=' * 72}\n\n{read(os.path.join(TEXTS, n))}")
    with open(o.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(out))
    print(f"notices: {len(packages)} Haskell packages, {len(clibs)} C libraries: {o.out}")


if __name__ == "__main__":
    main()
