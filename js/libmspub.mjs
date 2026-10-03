// OpenImprenta spike: thin ES-module wrapper around the emscripten build.
// SPDX-License-Identifier: MPL-2.0
//
//   const pub = await loadLibmspub();            // Oz build by default
//   pub.isSupported(bytes)  -> boolean
//   pub.toSVG(bytes)        -> string[]          (one SVG per page)
//   pub.toJSON(bytes)       -> object            (see README, "Esquema del JSON")
//
// Errors: unsupported or unparseable files throw a normal Error. If the wasm
// traps (memory access out of bounds, abort, stack overflow...) the instance
// is no longer trustworthy: the wrapper throws a LibmspubCrash and creates a
// fresh instance before the next call.

const variants = {
  O3: () => import('../dist/O3/libmspub.js'),
  Oz: () => import('../dist/Oz/libmspub.js'),
  // comparison build without the filtered ICU data (see icu/make-icudata.sh)
  'Oz-stubdata': () => import('../dist/Oz-stubdata/libmspub.js'),
};

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

export async function loadLibmspub({ variant = 'Oz', moduleOptions = {} } = {}) {
  const { default: createLibmspub } = await variants[variant]();
  const t0 = performance.now();
  let mod = await createLibmspub({ print: () => {}, printErr: () => {}, ...moduleOptions });
  const loadMs = performance.now() - t0;

  let broken = false;
  async function ensure() {
    if (broken) {
      mod = await createLibmspub({ print: () => {}, printErr: () => {}, ...moduleOptions });
      broken = false;
    }
  }

  function guarded(fn) {
    try {
      return fn();
    } catch (e) {
      // C++ exceptions are caught in binding.cpp, so anything that reaches
      // here is a trap (WebAssembly.RuntimeError) or an emscripten abort.
      broken = true;
      throw new LibmspubCrash(e);
    }
  }

  return {
    variant,
    loadMs,
    /** Re-creates the instance if the previous call trapped. */
    ensure,
    isSupported(input) {
      if (broken) throw new Error('instance crashed; await pub.ensure() first');
      return guarded(() => mod.isSupported(asBytes(input)));
    },
    toSVG(input) {
      if (broken) throw new Error('instance crashed; await pub.ensure() first');
      const r = guarded(() => mod.toSVG(asBytes(input)));
      if (!r.ok && !(r.pages && r.pages.length)) throw new Error(r.error);
      return r.pages;
    },
    toJSON(input) {
      if (broken) throw new Error('instance crashed; await pub.ensure() first');
      const r = guarded(() => mod.toJSON(asBytes(input)));
      if (!r.ok && !r.json) throw new Error(r.error);
      const doc = JSON.parse(r.json);
      doc.parseOk = r.ok;
      doc.jsonBytes = r.json.length;
      if (!r.ok) doc.error = r.error;
      return doc;
    },
    /** Debug: dumps every librevenge call to `print` (moduleOptions.print). */
    toRaw(input) {
      return guarded(() => mod.toRaw(asBytes(input)));
    },
  };
}
