// OpenImprenta spike: test page for the libmspub WebAssembly build.
// SPDX-License-Identifier: MPL-2.0
import { loadLibmspub, LibmspubCrash } from '../js/libmspub.mjs';

const $ = (id) => document.getElementById(id);
const statusEl = $('status');
let pub = null;
let lastJson = null;
let lastName = 'document';
let svgUrls = [];

function setStatus(text, kind = '') {
  statusEl.textContent = text;
  statusEl.className = `status ${kind}`;
}

function wasmTransfer(variant) {
  const entry = performance.getEntriesByType('resource')
    .filter((e) => e.name.includes(`/dist/${variant}/libmspub.wasm`)).pop();
  return entry ? { encoded: entry.encodedBodySize, decoded: entry.decodedBodySize, fetchMs: entry.duration } : null;
}

async function load(variant) {
  setStatus(`Cargando el módulo ${variant}…`);
  const t0 = performance.now();
  pub = await loadLibmspub({ variant });
  const total = performance.now() - t0;
  const net = wasmTransfer(variant);
  const size = net ? ` · .wasm ${(net.decoded / 1024).toFixed(0)} KB (transferido ${(net.encoded / 1024).toFixed(0)} KB)` : '';
  setStatus(`Módulo ${variant} listo en ${total.toFixed(0)} ms (instanciar: ${pub.loadMs.toFixed(0)} ms)${size}.`, 'ok');
  return { total, net };
}

let loadInfo = await load($('variant').value);
$('variant').addEventListener('change', async (e) => {
  loadInfo = await load(e.target.value);
});

function shortenBinary(key, value) {
  if (typeof value === 'string' && value.length > 200 && (key === 'data' || key === 'office:binary-data' || key === 'draw:fill-image')) {
    return `${value.slice(0, 60)}… (${value.length} caracteres base64)`;
  }
  return value;
}

function row(label, value) {
  const tr = document.createElement('tr');
  tr.innerHTML = `<td></td><td class="num"></td>`;
  tr.children[0].textContent = label;
  tr.children[1].textContent = value;
  return tr;
}

async function processFile(file) {
  lastName = file.name.replace(/\.pub$/i, '');
  const bytes = new Uint8Array(await file.arrayBuffer());
  const tbody = $('times').querySelector('tbody');
  tbody.replaceChildren();
  svgUrls.forEach((u) => URL.revokeObjectURL(u));
  svgUrls = [];
  $('pages').replaceChildren();
  $('json').textContent = '';

  await pub.ensure();
  try {
    let t = performance.now();
    const supported = pub.isSupported(bytes);
    const tSupported = performance.now() - t;
    tbody.append(row('Archivo', `${file.name} (${(bytes.length / 1024).toFixed(1)} KB)`));
    tbody.append(row('Carga del módulo (total / instanciar)', `${loadInfo.total.toFixed(1)} / ${pub.loadMs.toFixed(1)} ms`));
    tbody.append(row('isSupported()', `${supported ? 'sí' : 'no'} · ${tSupported.toFixed(1)} ms`));
    $('times').hidden = false;
    if (!supported) {
      setStatus('libmspub no reconoce este archivo como Publisher.', 'bad');
      return;
    }

    t = performance.now();
    let pages = [];
    let svgError = null;
    try { pages = pub.toSVG(bytes); } catch (e) { svgError = e; }
    const tSvg = performance.now() - t;
    tbody.append(row('toSVG()', svgError ? `error: ${svgError.message}` : `${pages.length} páginas · ${tSvg.toFixed(1)} ms`));

    if (svgError instanceof LibmspubCrash) await pub.ensure();

    t = performance.now();
    let doc = null;
    let jsonError = null;
    try { doc = pub.toJSON(bytes); } catch (e) { jsonError = e; }
    const tJson = performance.now() - t;
    tbody.append(row('toJSON()', jsonError ? `error: ${jsonError.message}` : `${doc.pages.length} páginas · ${(doc.jsonBytes / 1024).toFixed(1)} KB · ${tJson.toFixed(1)} ms`));

    pages.forEach((svg, i) => {
      const url = URL.createObjectURL(new Blob([svg], { type: 'image/svg+xml' }));
      svgUrls.push(url);
      const fig = document.createElement('figure');
      const img = document.createElement('img');
      img.src = url;              // <img> never runs scripts embedded in the SVG
      img.alt = `Página ${i + 1}`;
      const cap = document.createElement('figcaption');
      cap.textContent = `Página ${i + 1}`;
      fig.append(img, cap);
      $('pages').append(fig);
    });

    if (doc) {
      lastJson = doc;
      $('json').textContent = JSON.stringify(doc, shortenBinary, 2);
    }
    $('tabs').hidden = false;
    showPane('svgPane');
    const crashed = svgError instanceof LibmspubCrash || jsonError instanceof LibmspubCrash;
    if (crashed) setStatus('El módulo se cayó (trap de wasm) con este archivo; se reinició la instancia.', 'bad');
    else if (svgError || jsonError || (doc && !doc.parseOk)) setStatus('libmspub no pudo leer el documento completo.', 'bad');
    else setStatus(`Listo: ${pages.length} páginas.`, 'ok');
  } catch (e) {
    setStatus(`Error: ${e.message}`, 'bad');
    if (e instanceof LibmspubCrash) await pub.ensure();
  }
}

function showPane(id) {
  for (const b of $('tabs').querySelectorAll('button')) {
    const on = b.dataset.pane === id;
    b.setAttribute('aria-selected', String(on));
    $(b.dataset.pane).hidden = !on;
  }
}
$('tabs').addEventListener('click', (e) => {
  if (e.target.dataset.pane) showPane(e.target.dataset.pane);
});

$('download').addEventListener('click', () => {
  if (!lastJson) return;
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([JSON.stringify(lastJson, null, 2)], { type: 'application/json' }));
  a.download = `${lastName}.json`;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
});

const drop = $('drop');
$('file').addEventListener('change', (e) => e.target.files[0] && processFile(e.target.files[0]));
drop.addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') $('file').click(); });
drop.addEventListener('dragover', (e) => { e.preventDefault(); drop.classList.add('over'); });
drop.addEventListener('dragleave', () => drop.classList.remove('over'));
drop.addEventListener('drop', (e) => {
  e.preventDefault();
  drop.classList.remove('over');
  if (e.dataTransfer.files[0]) processFile(e.dataTransfer.files[0]);
});
