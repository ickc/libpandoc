// A directory of the disk, read-only, as a WASI preopen for
// @bjorn3/browser_wasi_shim in Node: what wasm-filter.mjs gives a wasm
// filter there for each directory of $LIBPANDOC_WASM_DIRS (by default
// none), as the other hosts (wasmtime) give them. Files are read whole
// when opened.
//
//   import { hostPreopens, nodeDirectory } from "./node-dir.mjs";
//   const f = await wasmFilter(bytes, { preopens: hostPreopens(["../common", "."]) });
//   const g = await wasmFilter(bytes, { preopens: [nodeDirectory("data", "/data")] });
//
// Nothing outside the directory is reached: a path leaving it through ".."
// or a symbolic link (followed, then checked against the directory's real
// path) fails with "permission denied", where a missing file fails with
// "does not exist". Nothing is written. (Not if someone else changes the
// directory meanwhile: a link swapped between the check and the read.)

import { lstatSync, readdirSync, readFileSync, realpathSync, statSync } from "node:fs";
import { isAbsolute, join, relative, resolve, sep } from "node:path";
import { Fd, File, OpenFile, wasi } from "@bjorn3/browser_wasi_shim";

const ERRNO = { ENOENT: wasi.ERRNO_NOENT, ENOTDIR: wasi.ERRNO_NOTDIR, EACCES: wasi.ERRNO_ACCES,
  ELOOP: wasi.ERRNO_LOOP, EISDIR: wasi.ERRNO_ISDIR, ENAMETOOLONG: wasi.ERRNO_NAMETOOLONG };
const errno = (e) => ERRNO[e?.code] ?? wasi.ERRNO_IO;

function filestat(st) {
  const type = st.isDirectory() ? wasi.FILETYPE_DIRECTORY
    : st.isFile() ? wasi.FILETYPE_REGULAR_FILE
    : st.isSymbolicLink() ? wasi.FILETYPE_SYMBOLIC_LINK
    : wasi.FILETYPE_UNKNOWN;
  const s = new wasi.Filestat(BigInt(st.ino), type, BigInt(st.size));
  s.atim = BigInt(Math.round(st.atimeMs * 1e6));
  s.mtim = BigInt(Math.round(st.mtimeMs * 1e6));
  s.ctim = BigInt(Math.round(st.ctimeMs * 1e6));
  return s;
}

class NodeDirectory extends Fd {
  constructor(root, path, preopenName = null) {
    super();
    this.root = root; // the real path of the preopened directory
    this.path = path; // this one's, inside it
    this.preopenName = preopenName;
  }
  // the real path `rel` names (its links followed when `follow`), or a WASI
  // errno: PERM when it leaves the directory
  #resolve(rel, follow = true) {
    if (rel.includes("\0")) return wasi.ERRNO_INVAL;
    const p = resolve(this.path, rel);
    if (!this.#inside(p)) return wasi.ERRNO_PERM;
    let real;
    try {
      real = follow ? realpathSync(p) : join(realpathSync(resolve(p, "..")), relative(resolve(p, ".."), p));
    } catch (e) {
      return errno(e);
    }
    return this.#inside(real) ? real : wasi.ERRNO_PERM;
  }
  #inside(p) {
    const r = relative(this.root, p);
    return r === "" || (!isAbsolute(r) && r.split(sep)[0] !== "..");
  }
  fd_prestat_get() {
    if (this.preopenName === null) return { ret: wasi.ERRNO_BADF, prestat: null };
    return { ret: 0, prestat: wasi.Prestat.dir(this.preopenName) };
  }
  fd_fdstat_get() {
    return { ret: 0, fdstat: new wasi.Fdstat(wasi.FILETYPE_DIRECTORY, 0) };
  }
  fd_filestat_get() {
    try {
      return { ret: 0, filestat: filestat(statSync(this.path)) };
    } catch (e) {
      return { ret: errno(e), filestat: null };
    }
  }
  fd_readdir_single(cookie) {
    let names;
    try {
      names = [".", "..", ...readdirSync(this.path)];
    } catch (e) {
      return { ret: errno(e), dirent: null };
    }
    if (cookie >= BigInt(names.length)) return { ret: 0, dirent: null };
    const name = names[Number(cookie)];
    try {
      const st = lstatSync(join(this.path, name));
      return { ret: 0, dirent: new wasi.Dirent(cookie + 1n, BigInt(st.ino), name, filestat(st).filetype) };
    } catch (e) {
      return { ret: errno(e), dirent: null };
    }
  }
  path_filestat_get(flags, rel) {
    const follow = (flags & wasi.LOOKUPFLAGS_SYMLINK_FOLLOW) !== 0;
    const p = this.#resolve(rel, follow);
    if (typeof p === "number") return { ret: p, filestat: null };
    try {
      return { ret: 0, filestat: filestat(follow ? statSync(p) : lstatSync(p)) };
    } catch (e) {
      return { ret: errno(e), filestat: null };
    }
  }
  path_open(dirflags, rel, oflags, rightsBase, rightsInheriting, fdflags) {
    const p = this.#resolve(rel);
    if (typeof p === "number") {
      // (creating what doesn't exist: refused as writing, below)
      if (p !== wasi.ERRNO_NOENT || !(oflags & wasi.OFLAGS_CREAT)) return { ret: p, fd_obj: null };
    }
    if (oflags & (wasi.OFLAGS_CREAT | wasi.OFLAGS_TRUNC | wasi.OFLAGS_EXCL)
        || (rightsBase & BigInt(wasi.RIGHTS_FD_WRITE)) !== 0n || (fdflags & wasi.FDFLAGS_APPEND))
      return { ret: wasi.ERRNO_ROFS, fd_obj: null };
    try {
      const st = statSync(p);
      if (st.isDirectory()) return { ret: 0, fd_obj: new NodeDirectory(this.root, p) };
      if (oflags & wasi.OFLAGS_DIRECTORY) return { ret: wasi.ERRNO_NOTDIR, fd_obj: null };
      return { ret: 0, fd_obj: new OpenFile(new File(readFileSync(p), { readonly: true })) };
    } catch (e) {
      return { ret: errno(e), fd_obj: null };
    }
  }
}

/** The disk's directory `path` (relative to the working directory now),
 *  read-only, as a WASI preopen named `name` (default: the same path). */
export function nodeDirectory(path, name = path) {
  const root = realpathSync(path);
  return new NodeDirectory(root, root, name);
}

/** The host's directories `dirs` (relative ones to `cwd`), read-only, as
 *  the preopens a wasm filter sees them by: each under its absolute path
 *  (the real one too, if a link leads to it; on Windows C:\x as /c/x) and
 *  under its path relative to `cwd` (".", "..", "../common", "img"; none
 *  on another Windows drive); and `cwd` as "." when it lies inside one of
 *  them. wasi-libc matches the beginning of a path against these names as
 *  written, without normalising: ../common/x and here.txt work,
 *  ./../common/x and img/../../common/x don't. Throws, naming it, for one
 *  that doesn't exist or isn't a directory. */
export function hostPreopens(dirs, cwd = process.cwd()) {
  const realCwd = realpathSync(cwd);
  const named = new Map();
  const add = (name, real) => {
    if (name !== null && !named.has(name)) named.set(name, new NodeDirectory(real, real, name));
  };
  for (const dir of dirs) {
    const abs = resolve(cwd, dir);
    let st;
    try {
      st = statSync(abs);
    } catch (e) {
      throw new Error(e.code === "ENOENT" || e.code === "ENOTDIR" ? `${dir} does not exist` : `${dir}: ${e.message}`);
    }
    if (!st.isDirectory()) throw new Error(`${dir} is not a directory`);
    const real = realpathSync(abs);
    add(wasiPath(abs), real);
    add(wasiPath(real), real);
    const r = relative(realCwd, real);
    if (!isAbsolute(r)) add(r === "" ? "." : r.split(sep).join("/"), real);
    const back = relative(real, realCwd);
    if (back !== "" && !isAbsolute(back) && back.split(sep)[0] !== "..") add(".", realCwd);
  }
  return [...named.values()];
}

/** An absolute path as a WASI one: itself, or on Windows C:\x as /c/x
 *  (the bindings' convention), null for other Windows paths (UNC). */
function wasiPath(p) {
  if (sep === "/") return p;
  const m = /^([A-Za-z]):\\(.*)$/.exec(p);
  return m ? `/${m[1].toLowerCase()}/${m[2].split("\\").join("/")}`.replace(/\/$/, "") : null;
}
