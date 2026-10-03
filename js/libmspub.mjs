// OpenImprenta: thin ES-module wrapper around the emscripten build.
// SPDX-License-Identifier: MPL-2.0
//
//   const pub = await loadLibmspub();            // Oz build by default
//   pub.isSupported(bytes)  -> boolean
//   pub.parse(bytes)        -> { doc, blobs }    JSON format version 2: pictures and fonts are
//                                                NOT in the JSON; `"blob": n` points to blobs[n],
//                                                a Uint8Array with its own ArrayBuffer (transferable)
//   pub.toJSON(bytes)       -> object            legacy (format version 1): parse() with the blobs
//                                                inlined back as base64 `data`. Costs memory.
//   pub.toSVG(bytes)        -> string[]          (one SVG per page, debugging only)
//   pub.heapBytes()         -> number            size of the wasm linear memory (it never shrinks)
//   pub.dispose()                                drops the instance so its memory can be collected
//
// Errors: unsupported or unparseable files throw a LibmspubError (`error.code` is 'unsupported'
// when the bytes are not a Publisher file). If the wasm traps (memory access out of bounds,
// abort, stack overflow...) the instance is no longer trustworthy: the wrapper throws a
// LibmspubCrash and `await pub.ensure()` creates a fresh instance.

const variants = {
  O3: () => import('../dist/O3/libmspub.js'),
  Oz: () => import('../dist/Oz/libmspub.js'),
  // comparison build without the filtered ICU data (see icu/make-icudata.sh)
  'Oz-stubdata': () => import('../dist/Oz-stubdata/libmspub.js'),
};

/** An unreadable or unsupported file (`code`: 'unsupported' | 'parse' | 'memory' | 'format'). */
export class LibmspubError extends Error {
  constructor(message, code) {
    super(message);
    this.name = 'LibmspubError';
    this.code = code;
  }
}

export class LibmspubCrash extends Error {
  constructor(cause) {
    super(`wasm trap: ${cause && cause.message ? cause.message : cause}`);
    this.name = 'LibmspubCrash';
    this.cause = cause;
  }
}

function asBytes(input) {
  if (input instanceof Uint8Array) return input;
  if (input instanceof ArrayBuffer) return new Uint8Array(input);
  if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
  throw new TypeError('expected Uint8Array or ArrayBuffer');
}

function base64(bytes) {
  if (typeof Buffer === 'function') return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength).toString('base64');
  let s = '';
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(s);
}

/**
 * Parses with an instantiated emscripten module (the raw embind functions). Exported so that
 * applications that load the glue themselves (e.g. from their own origin) share the protocol.
 * Returns { doc, blobs }; throws Error (code 'unsupported' | 'parse') on unreadable files.
 */
export function parseWithModule(mod, input) {
  const bytes = asBytes(input);
  if (bytes.length === 0) throw new LibmspubError('empty input', 'unsupported');
  const view = mod.inputBuffer(bytes.length);
  if (!view) throw new LibmspubError(`could not allocate ${bytes.length} bytes in the wasm heap`, 'memory');
  view.set(bytes);
  let r;
  let blobs;
  try {
    r = mod.parseInput();
    if (!r.json) throw new LibmspubError(r.error || 'parse failed', r.unsupported ? 'unsupported' : 'parse');
    // copy each picture out of the wasm heap into its own ArrayBuffer (transferable)
    blobs = [];
    for (let i = 0; i < r.blobs; i++) blobs.push(mod.blob(i).slice());
  } finally {
    mod.release();
  }
  let doc;
  try {
    doc = JSON.parse(r.json);
  } catch (e) {
    throw new LibmspubError(`the module returned invalid JSON (${e.message})`, 'format');
  }
  const declared = Array.isArray(doc.blobs) ? doc.blobs : [];
  if (declared.length !== blobs.length || declared.some((b, i) => b.size !== blobs[i].byteLength)) {
    throw new LibmspubError('blob table does not match the blobs returned by the module', 'format');
  }
  doc.parseOk = r.ok;
  doc.jsonBytes = r.json.length;
  if (!r.ok) doc.error = r.error;
  return { doc, blobs };
}

/** Format version 1 view of a version 2 result: every `"blob": n` becomes `"data": "<base64>"`. */
export function inlineBlobs(doc, blobs) {
  const inline = (o) => {
    if (o && typeof o.blob === 'number') {
      o.data = base64(blobs[o.blob]);
      delete o.blob;
    } else if (o && 'blob' in o) {
      o.data = null;
      delete o.blob;
    }
  };
  const walk = (els) => {
    for (const e of els || []) {
      if (e.type === 'image') inline(e);
      if (e.fill && e.fill.kind === 'bitmap') inline(e.fill);
      if (e.elements) walk(e.elements);
    }
  };
  for (const f of doc.embeddedFonts || []) inline(f);
  for (const p of doc.pages || []) walk(p.elements);
  delete doc.blobs;
  doc.version = 1;
  return doc;
}

export async function loadLibmspub({ variant = 'Oz', moduleOptions = {} } = {}) {
  const { default: createLibmspub } = await variants[variant]();
  const create = () => createLibmspub({ print: () => {}, printErr: () => {}, ...moduleOptions });
  const t0 = performance.now();
  let mod = await create();
  const loadMs = performance.now() - t0;

  let broken = false;
  async function ensure() {
    if (broken || !mod) {
      mod = await create();
      broken = false;
    }
  }

  function guarded(fn) {
    if (!mod) throw new Error('instance disposed; await pub.ensure() first');
    if (broken) throw new Error('instance crashed; await pub.ensure() first');
    try {
      return fn();
    } catch (e) {
      // C++ exceptions are caught in binding.cpp and come back as LibmspubError;
      // anything else is a trap (WebAssembly.RuntimeError) or an emscripten abort.
      if (e instanceof LibmspubError || e instanceof TypeError) throw e;
      broken = true;
      throw new LibmspubCrash(e);
    }
  }

  return {
    variant,
    loadMs,
    /** Re-creates the instance if the previous call trapped or after dispose(). */
    ensure,
    isSupported(input) {
      const bytes = asBytes(input);
      if (bytes.length === 0) return false;
      return guarded(() => {
        const view = mod.inputBuffer(bytes.length);
        if (!view) return false;
        view.set(bytes);
        try {
          return mod.isSupportedInput();
        } finally {
          mod.release();
        }
      });
    },
    /** { doc, blobs }: JSON format version 2, pictures as binary blobs. */
    parse(input) {
      return guarded(() => parseWithModule(mod, input));
    },
    /** Legacy (format version 1): pictures inlined as base64. Prefer parse(). */
    toJSON(input) {
      const { doc, blobs } = guarded(() => parseWithModule(mod, input));
      return inlineBlobs(doc, blobs);
    },
    toSVG(input) {
      const r = guarded(() => mod.toSVG(asBytes(input)));
      if (!r.ok && !(r.pages && r.pages.length)) throw new Error(r.error);
      return r.pages;
    },
    /** Size of the wasm linear memory in bytes (its peak so far: it never shrinks). */
    heapBytes() {
      return mod ? mod.heapSize() : 0;
    },
    /** Drops the instance; the next call needs `await pub.ensure()`. */
    dispose() {
      if (mod) mod.release();
      mod = null;
    },
    /** Debug: dumps every librevenge call to `print` (moduleOptions.print). */
    toRaw(input) {
      return guarded(() => mod.toRaw(asBytes(input)));
    },
  };
}
