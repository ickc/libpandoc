// A WASI directory for libpandoc.wasm that is a directory of an Emscripten
// filesystem: Pyodide's (`pyodide.FS`). Python and pandoc then see the same
// files: a filter file, a bibliography, an image, the output file.
//
//   import { emscriptenDirectory } from "./emscripten-fs.mjs";
//   const pandoc = await load(url, { preopens: [emscriptenDirectory(py.FS, "/home/pyodide")] });
//
// Emscripten's errno numbers are WASI's, so its errors pass through as they
// are. No links (Emscripten has symbolic ones only), no timestamps.

import { Fd, wasi } from "@bjorn3/browser_wasi_shim";

// a path relative to a directory, "." and ".." resolved; null if it leaves it
function join(dir, rel) {
  const parts = dir.split("/").filter(Boolean);
  const depth = parts.length;
  for (const p of rel.split("/")) {
    if (p === "" || p === ".") continue;
    if (p === "..") {
      if (parts.length <= depth) return null;
      parts.pop();
    } else parts.push(p);
  }
  return "/" + parts.join("/");
}

// Emscripten's errno (WASI's numbering), or rethrow
function errno(e) {
  if (e && typeof e.errno === "number") return e.errno;
  throw e;
}

function filestat(FS, st) {
  const type = FS.isDir(st.mode) ? wasi.FILETYPE_DIRECTORY
    : FS.isFile(st.mode) ? wasi.FILETYPE_REGULAR_FILE
    : FS.isLink(st.mode) ? wasi.FILETYPE_SYMBOLIC_LINK
    : wasi.FILETYPE_UNKNOWN;
  const s = new wasi.Filestat(BigInt(st.ino), type, BigInt(st.size));
  const ns = (t) => BigInt(Math.round((t instanceof Date ? t.getTime() : Number(t ?? 0)) * 1e6));
  s.atim = ns(st.atime);
  s.mtim = ns(st.mtime);
  s.ctim = ns(st.ctime);
  return s;
}

/** An open file of the Emscripten filesystem. */
class EmscriptenFile extends Fd {
  constructor(FS, stream) {
    super();
    this.FS = FS;
    this.stream = stream;
  }
  fd_fdstat_get() {
    return { ret: 0, fdstat: new wasi.Fdstat(wasi.FILETYPE_REGULAR_FILE, 0) };
  }
  fd_filestat_get() {
    try {
      return { ret: 0, filestat: filestat(this.FS, this.FS.stat(this.stream.path)) };
    } catch (e) {
      return { ret: errno(e), filestat: null };
    }
  }
  fd_filestat_set_size(size) {
    try {
      this.FS.ftruncate(this.stream.fd, Number(size));
      return 0;
    } catch (e) {
      return errno(e);
    }
  }
  fd_read(size) {
    try {
      const buf = new Uint8Array(size);
      const n = this.FS.read(this.stream, buf, 0, size);
      return { ret: 0, data: buf.subarray(0, n) };
    } catch (e) {
      return { ret: errno(e), data: new Uint8Array() };
    }
  }
  fd_pread(size, offset) {
    try {
      const buf = new Uint8Array(size);
      const n = this.FS.read(this.stream, buf, 0, size, Number(offset));
      return { ret: 0, data: buf.subarray(0, n) };
    } catch (e) {
      return { ret: errno(e), data: new Uint8Array() };
    }
  }
  fd_write(data) {
    try {
      return { ret: 0, nwritten: this.FS.write(this.stream, data, 0, data.byteLength) };
    } catch (e) {
      return { ret: errno(e), nwritten: 0 };
    }
  }
  fd_pwrite(data, offset) {
    try {
      return { ret: 0, nwritten: this.FS.write(this.stream, data, 0, data.byteLength, Number(offset)) };
    } catch (e) {
      return { ret: errno(e), nwritten: 0 };
    }
  }
  fd_seek(offset, whence) {
    try {
      return { ret: 0, offset: BigInt(this.FS.llseek(this.stream, Number(offset), whence)) };
    } catch (e) {
      return { ret: errno(e), offset: 0n };
    }
  }
  fd_tell() {
    return { ret: 0, offset: BigInt(this.stream.position) };
  }
  fd_close() {
    try {
      this.FS.close(this.stream);
      return 0;
    } catch (e) {
      return errno(e);
    }
  }
}

/** A directory of the Emscripten filesystem, open (or preopened, with a name). */
class EmscriptenDirectory extends Fd {
  constructor(FS, path, preopenName = null) {
    super();
    this.FS = FS;
    this.path = path;
    this.preopenName = preopenName;
  }
  #resolve(rel) {
    return join(this.path, rel);
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
      return { ret: 0, filestat: filestat(this.FS, this.FS.stat(this.path)) };
    } catch (e) {
      return { ret: errno(e), filestat: null };
    }
  }
  fd_readdir_single(cookie) {
    let names;
    try {
      names = this.FS.readdir(this.path); // with "." and ".."
    } catch (e) {
      return { ret: errno(e), dirent: null };
    }
    if (cookie >= BigInt(names.length)) return { ret: 0, dirent: null };
    const name = names[Number(cookie)];
    let st;
    try {
      st = this.FS.lstat(join(this.path, name) ?? this.path);
    } catch (e) {
      return { ret: errno(e), dirent: null };
    }
    return { ret: 0, dirent: new wasi.Dirent(cookie + 1n, BigInt(st.ino), name, filestat(this.FS, st).filetype) };
  }
  path_filestat_get(flags, rel) {
    const p = this.#resolve(rel);
    if (p === null) return { ret: wasi.ERRNO_PERM, filestat: null };
    try {
      const follow = (flags & wasi.LOOKUPFLAGS_SYMLINK_FOLLOW) !== 0;
      return { ret: 0, filestat: filestat(this.FS, follow ? this.FS.stat(p) : this.FS.lstat(p)) };
    } catch (e) {
      return { ret: errno(e), filestat: null };
    }
  }
  path_open(dirflags, rel, oflags, rightsBase, rightsInheriting, fdflags) {
    const p = this.#resolve(rel);
    if (p === null) return { ret: wasi.ERRNO_PERM, fd_obj: null };
    const FS = this.FS;
    try {
      let exists = true;
      let st;
      try {
        st = FS.stat(p);
      } catch (e) {
        if (errno(e) !== wasi.ERRNO_NOENT) throw e;
        exists = false;
      }
      if (exists && (oflags & wasi.OFLAGS_EXCL)) return { ret: wasi.ERRNO_EXIST, fd_obj: null };
      if (oflags & wasi.OFLAGS_DIRECTORY) {
        if (!exists) {
          if (!(oflags & wasi.OFLAGS_CREAT)) return { ret: wasi.ERRNO_NOENT, fd_obj: null };
          FS.mkdir(p);
        } else if (!FS.isDir(st.mode)) return { ret: wasi.ERRNO_NOTDIR, fd_obj: null };
        return { ret: 0, fd_obj: new EmscriptenDirectory(FS, p) };
      }
      if (exists && FS.isDir(st.mode)) return { ret: 0, fd_obj: new EmscriptenDirectory(FS, p) };
      if (!exists && !(oflags & wasi.OFLAGS_CREAT)) return { ret: wasi.ERRNO_NOENT, fd_obj: null };
      const write = (rightsBase & BigInt(wasi.RIGHTS_FD_WRITE)) !== 0n;
      let flags = write ? "r+" : "r";
      if (!exists) flags = "w+";
      else if (oflags & wasi.OFLAGS_TRUNC) flags = "w+";
      const stream = FS.open(p, flags);
      if (fdflags & wasi.FDFLAGS_APPEND) FS.llseek(stream, 0, 2);
      return { ret: 0, fd_obj: new EmscriptenFile(FS, stream) };
    } catch (e) {
      return { ret: errno(e), fd_obj: null };
    }
  }
  path_create_directory(rel) {
    const p = this.#resolve(rel);
    if (p === null) return wasi.ERRNO_PERM;
    try {
      this.FS.mkdir(p);
      return 0;
    } catch (e) {
      return errno(e);
    }
  }
  path_unlink_file(rel) {
    const p = this.#resolve(rel);
    if (p === null) return wasi.ERRNO_PERM;
    try {
      this.FS.unlink(p);
      return 0;
    } catch (e) {
      return errno(e);
    }
  }
  path_remove_directory(rel) {
    const p = this.#resolve(rel);
    if (p === null) return wasi.ERRNO_PERM;
    try {
      this.FS.rmdir(p);
      return 0;
    } catch (e) {
      return errno(e);
    }
  }
  // The shim renames by path_unlink here then path_link there: so unlink
  // only names the file, and link (as a rename) moves it.
  path_unlink(rel) {
    const p = this.#resolve(rel);
    if (p === null) return { ret: wasi.ERRNO_PERM, inode_obj: null };
    try {
      this.FS.lstat(p);
      return { ret: 0, inode_obj: { emscriptenPath: p } };
    } catch (e) {
      return { ret: errno(e), inode_obj: null };
    }
  }
  path_link(rel, inode, allowDir) {
    const p = this.#resolve(rel);
    if (p === null) return wasi.ERRNO_PERM;
    if (!allowDir || !inode?.emscriptenPath) return wasi.ERRNO_NOTSUP; // a hard link
    try {
      if (inode.emscriptenPath !== p) this.FS.rename(inode.emscriptenPath, p);
      return 0;
    } catch (e) {
      return errno(e);
    }
  }
  path_readlink(rel) {
    const p = this.#resolve(rel);
    if (p === null) return { ret: wasi.ERRNO_PERM, data: null };
    try {
      return { ret: 0, data: this.FS.readlink(p) };
    } catch (e) {
      return { ret: errno(e), data: null };
    }
  }
}

/** The Emscripten directory `path` as a WASI preopen named `name` (default: the same path). */
export function emscriptenDirectory(FS, path, name = path) {
  return new EmscriptenDirectory(FS, path, name);
}
