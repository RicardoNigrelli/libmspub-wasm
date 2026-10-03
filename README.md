# libmspub-wasm

**Read Microsoft Publisher (`.pub`) files in the browser.** [libmspub](https://git.libreoffice.org/libmspub) 0.1.5 and [librevenge](https://sourceforge.net/projects/libwpd/files/librevenge/) 0.0.5 compiled to WebAssembly with Emscripten, with a JSON drawing generator so applications can rebuild the document in their own model.

> **Status: experimental.** Built for [OpenImprenta](#context), a free desktop publishing app being developed after Microsoft retired Publisher from Microsoft 365 (1 October 2026).

- `isSupported(bytes)`, `toSVG(bytes)` (debug only) and **`toJSON(bytes)`**: pages, shapes, text boxes with paragraphs and styled spans, and images (bitmap fills).
- Runs in a Web Worker or in Node. About 1.2 MB of wasm (`-Oz`), 445 KB with brotli.
- Build inside Docker (`./build.sh`) with a pinned Emscripten image and sha256-verified source tarballs; nothing is installed on the host. **Not bit-for-bit reproducible yet:** a local build and the CI build of the same commit differed by 2 bytes in the `.wasm` (an embedded build path or marker). The canonical binaries are the CI artifacts, with their `SHA256SUMS`.
- **Two patches to libmspub** (`patches/`): a broken metadata stream no longer rejects the whole document, and Escher lengths are clamped so damaged files cannot loop on 32-bit `long` (wasm32). Both are intended for upstream.

### What is lost (known limitations)
Linked text box chains (the full story is repeated in each box), shape types (everything arrives as polygons), hyperlink targets, page-number fields, CMYK/spot colours and colour-scheme indices, WordArt text, master pages (flattened), and picture **crop** (Escher `cropFromTop/Bottom/Left/Right` are not read by libmspub). WMF/EMF pictures are passed through but browsers cannot draw them. Details below (in Spanish).

### Build and test
```bash
./build.sh                                   # -> dist/Oz, dist/O3
node test/run-corpus.mjs path/to/pub-files   # parse every .pub, report pages, timings, errors
node test/fuzz-smoke.mjs path/to/pub-files   # mutation smoke test (crashes / hangs)
```
CI builds the module and runs it over the public Apache POI test files (Apache-2.0).

### License
The code in this repository (`src/`, `js/`, `web/`, `test/`, build scripts) and the patches are under the **Mozilla Public License 2.0** (`LICENSE`), like libmspub and librevenge. The wasm build also contains ICU (Unicode license) and Emscripten runtime code (MIT/University of Illinois); see each project for its terms.

Microsoft and Publisher are trademarks of Microsoft Corporation. This project is not affiliated with or endorsed by Microsoft. It reads `.pub` files only for interoperability, through libmspub, an independent implementation by the Document Liberation Project / LibreOffice.

### Context
OpenImprenta is a free (GPL-2.0-or-later) desktop publishing web app for non-professional users and small print shops. Its repository is private until the first version is stable; this reader is published now because it is the part other projects can reuse and because the MPL requires its source to be available wherever the compiled module is distributed.

---

**En español:** lector de archivos `.pub` de Microsoft Publisher para el navegador: libmspub compilado a WebAssembly, con un generador JSON propio. Experimental. Licencia MPL-2.0. Debajo sigue el informe técnico completo del prototipo, con resultados, el esquema del JSON y lo que se pierde.

---

# Informe técnico: libmspub → WebAssembly

Prototipo descartable para leer archivos de Microsoft Publisher (`.pub`) dentro del navegador, compilando **libmspub 0.1.5** y **librevenge 0.0.5** a WebAssembly con Emscripten. El objetivo es saber si sirve como importador para OpenImprenta y qué información se pierde en el camino.

**Resumen (1 de octubre de 2026)**

- Compila y funciona. Las 21 piezas legibles del corpus de Apache POI se abren en el navegador y en Node en **0,5 a 25 ms** por documento. Los 3 `clusterfuzz-*` malformados se rechazan o fallan sin tumbar nada.
- El `.wasm` pesa **1,18 MB (-Oz), 445 KB con brotli**. Casi la mitad de eso son los datos de ICU que agregué para que no se pierda texto (ver más abajo). Sin ellos pesa 805 KB (260 KB con brotli).
- **Tal cual, libmspub no sirve para producción.** Encontré y parcheé dos fallas:
  1. **Rechaza documentos enteros por un metadato roto.** Pasa con el folleto y el boletín del corpus, que son justamente los más ricos. Parche 0001.
  2. **Se cuelga en wasm32 con archivos dañados.** En x86-64 el mismo archivo termina en 0,2 s. La causa es `long` de 32 bits en `seek()`. Parche 0002.

  Con los dos parches: 6000 archivos mutados, 0 crashes y 0 cuelgues.
- El SVG de librevenge sirve solo para depurar: el texto sale invisible y las imágenes en mosaico. **El camino es el JSON propio** (`toJSON`).
- Lo que más se pierde: la cadena de cuadros de texto enlazados (**la historia completa se repite en cada cuadro**), los tipos de forma (todo llega como polígono), los hipervínculos, los campos de número de página, CMYK y colores de esquema, y WordArt. Además, 9 de cada 10 imágenes del boletín son WMF, que el navegador no muestra.

---

## Contenido de la carpeta

| Ruta | Qué es |
|---|---|
| `Dockerfile` | Build completo y reproducible. Etapas: `deps` (ports de Emscripten, librevenge y libmspub), `icudata` (datos ICU filtrados), `wasm` (binding) y `export`. |
| `build.sh` | `docker build --target export --output dist .` Deja `dist/O3`, `dist/Oz` y `dist/Oz-stubdata`. |
| `patches/*.patch` | Parches a libmspub 0.1.5. Se aplican por defecto (`--build-arg APPLY_PATCHES=0` para compilar sin ellos). |
| `icu/make-icudata.sh` | Arma el paquete de datos ICU con solo los conversores que pide libmspub. |
| `src/JSONDrawingGenerator.{h,cpp}` | **Generador propio**: implementa `librevenge::RVNGDrawingInterface` y serializa a JSON. |
| `src/binding.cpp`, `src/compile.sh` | Funciones exportadas con embind y enlace (`-O3`, `-Oz` y la variante de comparación sin datos ICU). |
| `js/libmspub.mjs` | Envoltorio ES module: `loadLibmspub()` → `isSupported` / `toSVG` / `toJSON`. Reinstancia el módulo si el wasm hace *trap*. |
| `web/` | Página de prueba. |
| `test/run-corpus.mjs` (+ `worker.mjs`) | Procesa una carpeta de `.pub`, un worker por archivo con timeout. Escribe `out/<build>/results.{json,md}`, los SVG y el JSON de cada documento. |
| `test/measure.mjs` | Tamaños (crudo, gzip, brotli) y tiempo de carga. |
| `test/fuzz-smoke.mjs` | Prueba de robustez con mutaciones deterministas (no es un fuzzer de verdad). |
| `test/inspect.mjs` | Resume un `document.json`: historias repetidas, colores, fuentes, tipos de imagen. |
| `tools/native/Dockerfile` | libmspub **nativo** (x86-64) con `pub2xhtml`, solo para diagnóstico: distingue fallas de libmspub de fallas del port. |

`dist/` y `out/` están ignoradas por el `.gitignore` de la raíz. `out/` contiene texto de los archivos del corpus, que tampoco se versiona.

## Cómo construir

Solo hace falta Docker. No se instala nada en el host.

```bash
cd spikes/libmspub-wasm
./build.sh                      # o: docker build --target export --output type=local,dest=dist .
# sin los parches, para comparar:
docker build --target export --build-arg APPLY_PATCHES=0 --output type=local,dest=dist-vanilla .
```

Versiones fijadas:

| Componente | Versión | Cómo se fija |
|---|---|---|
| Emscripten | `emscripten/emsdk:6.0.10` | versión **y** digest `sha256:e077d54e…af7aebf65` en el `Dockerfile` |
| libmspub | 0.1.5 (tag `libmspub-0.1.5`, commit `ee7e4de6`) | tarball oficial `dev-www.libreoffice.org/src/libmspub/` + sha256 |
| librevenge | 0.0.5 | tarball oficial `dev-www.libreoffice.org/src/` + sha256 |
| ICU | 68.2 (port de Emscripten `-sUSE_ICU=1`) | fijada por emsdk 6.0.10 |
| boost (solo headers) | 1.83 (port `-sUSE_BOOST_HEADERS=1`) | fijada por emsdk 6.0.10 |
| zlib | 1.3.2 (port `-sUSE_ZLIB=1`) | fijada por emsdk 6.0.10 |
| icupkg (solo para reempaquetar datos) | `icu-devtools=74.2-1ubuntu3.1` | versión de apt fijada |

Nota: en el mirror también existe **librevenge 0.0.6** (5 de julio de 2026). No la probé porque el pedido fijaba 0.0.5.

Flags importantes:

- `-fwasm-exceptions` en **todas** las bibliotecas. libmspub usa excepciones C++ para el flujo normal (`EndOfStreamException` al llegar al final de un stream), así que no se pueden desactivar. Uso las excepciones nativas de wasm, que son más chicas y rápidas que las emuladas en JS. Las funciones exportadas atrapan todo en C++ y devuelven `{ok, error}`, de modo que a JS nunca le llega una `WebAssembly.Exception`.
- `-sSTACK_SIZE=1MB`, porque libmspub recursa por los grupos de formas. También `-sALLOW_MEMORY_GROWTH=1`, `-sMODULARIZE -sEXPORT_ES6`, `-sENVIRONMENT=web,worker,node` y `-sFILESYSTEM=0`.

### Problemas del build y cómo los resolví

1. **`config.sub` viejo.** `./configure --host=wasm32-unknown-emscripten` falla con `Invalid configuration 'wasm32-unknown-emscripten': system 'emscripten' not recognized`, porque los `config.sub` de los tarballs son de 2014. Se resuelve sacando `--host`: `emconfigure` ya configura el compilador cruzado.
2. **No hay `pkg-config` en la imagen.** Lo resolví pasando `ZLIB_CFLAGS/LIBS`, `ICU_CFLAGS/LIBS` y `REVENGE_CFLAGS/LIBS` a `configure`, que así salta `PKG_CHECK_MODULES`. Detalle menor: el port de ICU escribe su `.pc` como `ici.pc`, con un error de tipeo.
3. **boost:** alcanza con los headers, tanto para libmspub (`optional`, `numeric_cast`, `multi_array`) como para librevenge (`spirit`, `algorithm`, `archive/iterators`).
4. **ICU compila, pero sin datos.** El port de Emscripten enlaza `libicu_stubdata`. La sonda `icuProbe()` del binding muestra que `UTF-8` y `UTF-16LE` abren (son algorítmicos), pero **todas** las páginas de códigos `windows-125x` fallan con `U_FILE_ACCESS_ERROR`. libmspub las usa en dos lugares:
   - **Metadatos OLE.** `MSPUBMetaData` siempre usa `windows-1252`. Con stubdata, el autor desaparece en silencio en todos los archivos. Lo comprobé: `Sample.pub` pierde `dc:creator: "Nick Burch"`.
   - **Texto de Publisher 97** (`MSPUBParser97`). La codificación se adivina con `ucsdet` y después se convierte con `ucnv`. Sin datos, **todo el texto** de un archivo de Publisher 97 desaparecería sin error. Ningún archivo del corpus usa esta ruta (98 y 2000 ya guardan UTF-16), así que **no lo pude verificar con un archivo real**: lo deduzco del código.

   **Solución aplicada:** `icu/make-icudata.sh` extrae del `icudt68l.dat` que trae el propio zip de ICU 68.2 solo `cnvalias.icu` y los conversores que libmspub puede pedir (windows-1250, 1251, 1252, 1256, 932, 936 y 950, más las dos tablas base de las que dependen 936 y 950). Después los reempaqueta con `icupkg`, los embebe como arreglo C y los registra con `udata_setCommonData()`. Con eso abren todos; `U_AMBIGUOUS_ALIAS_WARNING` es una advertencia normal de ICU, no un error. Costo: +387 KB crudos.

   | Ítem ICU | Bytes |
   |---|---:|
   | cnvalias.icu | 63 984 |
   | windows-1250/1251/1252 (ibm-5346/5347/5348) | 3 200 c/u |
   | windows-1256 (ibm-9448) | 4 336 |
   | windows-932 (ibm-943_P15A) | 90 992 |
   | windows-936 (+ base ibm-1386) | 6 448 + 116 704 |
   | windows-950 (+ base ibm-1373) | 11 872 + 124 336 |

   Casi todo el peso es de los conversores CJK. Con solo los de un byte serían unos 78 KB crudos (no lo compilé).

## Cómo correr

```bash
# página de prueba: servir la carpeta del spike (necesita dist/, o sea ./build.sh primero)
python -m http.server 8000          # o: npx serve .
# abrir http://localhost:8000/web/

# corpus completo (por defecto ../../corpus/poi, build Oz)
node test/run-corpus.mjs
node test/run-corpus.mjs ../../corpus/poi --variant O3 --repeats 5 --timeout 30000

# tamaños y tiempo de carga
node test/measure.mjs --runs 20

# robustez: N mutantes deterministas (--dump <dir> guarda los que fallan)
node test/fuzz-smoke.mjs --n 5000 --seed 3

# comparar con libmspub nativo
docker build -t libmspub-native -f tools/native/Dockerfile .           # DEBUG=1 para la traza
docker run --rm -v "$PWD/../../corpus/poi:/in:ro" libmspub-native pub2xhtml /in/Sample.pub
```

La página acepta un `.pub` por input o arrastrándolo, permite elegir el build `-Oz` o `-O3`, y muestra el tiempo de carga del módulo, de `isSupported`, de `toSVG` y de `toJSON`. Las páginas SVG se muestran como `<img>` (así nunca se ejecuta un script que viniera en el SVG) y el JSON se ve con el base64 acortado, con un botón para descargarlo completo. La verifiqué en el panel del navegador de Claude (Chrome 152) con SampleNewsletter, Sample98 y un `clusterfuzz`.

API JS:

```js
import { loadLibmspub, LibmspubCrash } from './js/libmspub.mjs';
const pub = await loadLibmspub({ variant: 'Oz' });
pub.isSupported(bytes);   // boolean
pub.toSVG(bytes);         // string[]: una página SVG por elemento
pub.toJSON(bytes);        // objeto (esquema más abajo); lanza Error si no es un .pub
// si el wasm hace trap: lanza LibmspubCrash; `await pub.ensure()` crea una instancia nueva
```

## Resultados por archivo del corpus

Build `-Oz` con los parches 0001 y 0002 y los datos ICU, en Node 22.23.2 (Windows 11, x86-64). Los tiempos son la mediana de 5 parseos sobre la misma instancia. "Formas" son elementos de geometría (polígonos y paths), e "Imágenes" son rellenos bitmap.

| Archivo | KB | ¿Soportado? | Páginas | JSON (ms) | SVG (ms) | JSON (KB) | SVG (KB) | Texto (cajas / caracteres) | Formas | Imágenes | Tablas | Resultado |
|---|---:|:---:|---:|---:|---:|---:|---:|---|---:|---:|---:|---|
| 51318.pub | 86.5 | sí | 1 | 2.5 | 2.1 | 59.7 | 55.2 | 2 / 45 | 12 | 1 (png) | 0 | ok |
| 60685.pub | 116.5 | sí | 1 | 5.0 | 3.5 | 35.9 | 13.2 | 4 / 1574 | 53 | 0 | 0 | ok |
| LinkAt0And10.pub | 63.0 | sí | 1 | 0.6 | 0.4 | 2.2 | 0.6 | 1 / 15 | 0 | 0 | 0 | ok (se pierde la URL) |
| LinkAt10.pub | 63.0 | sí | 1 | 0.5 | 0.4 | 1.8 | 0.5 | 1 / 14 | 0 | 0 | 0 | ok (se pierde la URL) |
| LinkAt10And20And30.pub | 63.0 | sí | 1 | 0.7 | 0.5 | 3.0 | 0.9 | 1 / 35 | 0 | 0 | 0 | ok (se pierde la URL) |
| LinkAt10And20And30And40.pub | 63.0 | sí | 1 | 0.7 | 0.5 | 3.7 | 1.1 | 1 / 45 | 0 | 0 | 0 | ok (se pierde la URL) |
| LinkAt10Longer.pub | 63.0 | sí | 1 | 0.5 | 0.4 | 1.8 | 0.5 | 1 / 14 | 0 | 0 | 0 | ok (se pierde la URL) |
| LinkAt20.pub | 63.0 | sí | 1 | 0.5 | 0.4 | 1.8 | 0.5 | 1 / 24 | 0 | 0 | 0 | ok (se pierde la URL) |
| Sample.pub | 70.5 | sí | 2 | 2.2 | 1.3 | 14.5 | 4.1 | 5 / 387 | 1 | 0 | 1 | ok |
| Sample2.pub | 71.0 | sí | 2 | 2.2 | 1.4 | 16.3 | 4.6 | 5 / 469 | 1 | 0 | 1 | ok |
| Sample2000.pub | 60.0 | sí | 3 | 1.3 | 0.9 | 10.8 | 3.2 | 5 / 323 | 1 | 0 | **0** | parcial: sin tabla ni nombres de fuente |
| Sample2_2010.pub | 71.5 | sí | 2 | 2.0 | 1.4 | 16.3 | 4.6 | 5 / 469 | 1 | 0 | 1 | ok |
| Sample3.pub | 70.5 | sí | 2 | 2.0 | 1.3 | 14.6 | 4.1 | 5 / 395 | 1 | 0 | 1 | ok |
| Sample3_2010.pub | 71.0 | sí | 2 | 1.9 | 1.3 | 14.6 | 4.1 | 5 / 395 | 1 | 0 | 1 | ok |
| Sample4.pub | 70.5 | sí | 2 | 1.9 | 1.3 | 14.6 | 4.1 | 5 / 389 | 1 | 0 | 1 | ok |
| Sample4_2010.pub | 71.0 | sí | 2 | 2.0 | 1.3 | 14.6 | 4.1 | 5 / 389 | 1 | 0 | 1 | ok |
| Sample98.pub | 60.0 | sí | 3 | 1.3 | 0.9 | 11.6 | 3.5 | 5 / 323 | 4 | 0 | **0** | parcial: sin tabla ni nombres de fuente |
| SampleBrochure.pub | 158.0 | sí | 2 | 9.7 | 7.8 | 176.9 | 134.0 | 20 / 3096 | 48 | 8 (5 wmf, 2 png, 1 jpeg) | 0 | ok **solo con el parche 0001** |
| SampleNewsletter.pub | 285.0 | sí | 4 | 24.4 | 18.4 | 494.3 | 376.7 | 63 / 19258 | 86 | 10 (9 wmf, 1 jpeg) | 0 | ok **solo con el parche 0001** |
| Sample_2010.pub | 71.0 | sí | 2 | 1.9 | 1.2 | 14.5 | 4.1 | 5 / 387 | 1 | 0 | 1 | ok |
| Simple.pub | 64.0 | sí | 1 | 0.8 | 0.6 | 4.3 | 1.2 | 2 / 164 | 0 | 0 | 0 | ok |
| clusterfuzz-…-4701121678278656.pub | 323.7 | sí | 0 | 0.4 | – | 0.2 | – | – | – | – | – | falla limpia: `parse()` = false |
| clusterfuzz-…-4918886059278336.pub | 32.1 | sí | 0 | 0.4 | – | 0.2 | – | – | – | – | – | falla limpia: `parse()` = false |
| clusterfuzz-…-6325615354773504.pub | 2.0 | no | – | – | – | – | – | – | – | – | – | rechazado por `isSupported` |

Notas:

- **Sin el parche 0001**, `SampleBrochure.pub` y `SampleNewsletter.pub` devuelven `parse() = false` y 0 páginas. Lo mismo pasa con libmspub **nativo**: 0.1.4 de Ubuntu y 0.1.5 compilado por mí. O sea que es un bug de libmspub, no del port (detalle en "Parches").
- **Wasm y nativo coinciden** en el número de páginas de los 24 archivos (`out/native-corpus.txt`). Con los parches, la salida JSON del corpus es **byte a byte idéntica** antes y después del parche 0002.
- **Ningún archivo crashea ni cuelga** el proceso: cada uno corre en su propio worker con timeout, y además los errores de C++ se atrapan en el binding.
- Sample98 y Sample2000 (Publisher 98 y 2000): los nombres de fuente se pierden (todo sale como "Times New Roman" donde la versión 2002+ del mismo documento dice Arial) y **la tabla no se reconoce**. Salen solo sus bordes como polilíneas y el texto de las celdas se pierde: 323 caracteres contra 387, y los 64 que faltan son justo el texto de la tabla. Los metadatos salen vacíos.

## Tamaño del wasm y tiempos

| Build | Archivo | Bytes | gzip -9 | brotli q11 |
|---|---|---:|---:|---:|
| **-Oz** (recomendado) | libmspub.wasm | 1 211 500 (1 183 KB) | 608 KB | **445 KB** |
| -Oz | libmspub.js (glue) | 32 441 | 9,5 KB | 8,6 KB |
| -O3 | libmspub.wasm | 1 312 995 (1 282 KB) | 631 KB | 459 KB |
| -Oz sin datos ICU (stubdata, solo para comparar) | libmspub.wasm | 824 435 (805 KB) | 324 KB | 260 KB |

Antes de agregar los datos ICU y la sonda, el build era de 919 KB (-O3) y 819 KB (-Oz).

**Carga (descarga + compilación + instanciación):**

- Node 22.23.2, instancia nueva cada vez, 20 corridas: -Oz **5,3 ms** de mediana (3,4 a 8,0), -O3 4,9 ms. Dentro de un worker, incluyendo el `import()` del glue, la mediana es ~10 ms.
- Navegador (panel de Claude, Chrome 152, servido sin comprimir desde `127.0.0.1`): **79 ms** en total, de los cuales 52 son de instanciación, con la pestaña visible. Otra medición con la pestaña oculta dio 39/27 ms. Son mediciones sueltas que solo sirven para el orden de magnitud. No medí sobre una red real: ahí manda la descarga de los ~445 KB brotli.

**Parseo:** el documento más grande (SampleNewsletter, 285 KB y 4 páginas) tarda 24 ms con `toJSON` y 18 ms con `toSVG` en -Oz, y 20 / 17 ms en -O3. Entre -O3 y -Oz no veo una diferencia confiable. Sumando todo el corpus, -O3 fue 15 % más rápido en una corrida y más lento en otra (n chico, ruido entre corridas). **Recomiendo -Oz**, que pesa un 8 % menos.

**Memoria:** el módulo arranca con 32 MB (`INITIAL_MEMORY`) y puede crecer. No medí el pico con el corpus sano. Sin el parche 0002, un mutante llevó el proceso de Node a 1,3 GB de RSS.

## Esquema del JSON (`toJSON`)

Todas las longitudes están en **puntos** (1/72 pulgada). El origen es la esquina superior izquierda de la página. Cada nodo trae, además de los campos curados, un `props` con **todas** las propiedades crudas de librevenge, convertidas así: longitudes a pt, porcentajes como cadena (`"50%"`), booleanos como booleanos y binarios omitidos. Los campos curados que no existen valen `null`.

```jsonc
{
  "format": "openimprenta-libmspub-json", "version": 1, "units": "pt",
  "metadata": { "dc:creator": "…", "meta:creation-date": "…" },     // OLE SummaryInformation
  "embeddedFonts": [ { "name", "mimeType", "data": "<base64>", "props" } ],
  "pages": [ {
    "index": 0, "width": 595.2756, "height": 841.8898, "props": {…},
    "elements": [ /* en orden de pintado (z-order) */ ]
  } ],
  "stats": { "calls": { "drawPolygon": 85, … },   // cuántas veces se llamó cada método de librevenge
             "orphanText": 0, "unclosedFrames": 0 },
  // agregados por js/libmspub.mjs:
  "parseOk": true, "jsonBytes": 494322
}
```

Elementos (`type`):

| `type` | Campos |
|---|---|
| `polygon`, `polyline`, `path`, `rect`, `ellipse`, `connector` | `geometry` (props de la llamada: `svg:points` = `[{svg:x, svg:y}]`, `svg:d` = `[{librevenge:path-action: "M"\|"L"\|"C"\|"Q"\|"A"\|"Z", svg:x, svg:y, svg:x1, …}]`, `svg:x/y/width/height`, `svg:rx/ry`), `fill`, `stroke`, `shadow` (si hay), `style` (estilo gráfico crudo vigente) |
| `fill` | `{kind: "none"\|"solid"\|"gradient"\|"bitmap", color: "#rrggbb", opacity, angle, style, stops[], mimeType, repeat: "stretch"\|…, data: "<base64>"}`. **Las imágenes de libmspub llegan así**: un polígono con `fill.kind = "bitmap"` y la imagen en `fill.data`. |
| `stroke` | `{kind: "none"\|"solid"\|"dash", color, width, opacity, linecap, linejoin}` |
| `image` | `x, y, width, height, rotate, mimeType, data (base64), props`. Corresponde a `drawGraphicObject`, que libmspub 0.1.5 no usa con este corpus. |
| `text` | `x, y, width, height, rotate, padding {top,right,bottom,left}, verticalAlign, columns, columnGap, props, paragraphs[]` |
| ↳ párrafo | `align, lineHeight, marginTop/Bottom/Left/Right, textIndent, list? {level, ordered}, props, spans[]` |
| ↳ span | `text` (con `\t` y `\n` para tabulación y salto de línea; se quita la marca de párrafo `\r` que libmspub deja al final), `font, size (pt), bold, italic, underline ("single"…), color, link?, fields?, props` |
| `table` | `x, y, width, height, columns [{style:column-width}], props, rows [{height, props, cells [{colSpan, rowSpan, props, paragraphs[]} \| {covered: true}]}]` |
| `layer` | `props` (p. ej. `svg:clip-path` para recortes) + `elements[]` |
| `group` | `props` + `elements[]`. libmspub 0.1.5 nunca llama a `openGroup`. |

Ejemplo real (Sample.pub, página 2, recortado):

```json
{"type":"text","x":65.1969,"y":73.7008,"width":422.3622,"height":121.8898,"rotate":null,
 "padding":{"top":2.88,"right":2.88,"bottom":2.88,"left":2.88},"verticalAlign":null,"columns":null,"columnGap":5.6693,
 "paragraphs":[{"align":"left","spans":[{"text":"This is the second page","font":"Times New Roman","size":10,
   "bold":false,"italic":false,"underline":null,"color":"#000000",
   "props":{"fo:country":"GB","fo:language":"en", "…":"…"}}]}]}
```

**Qué no hay en el JSON y conviene saber:** no hay posiciones de línea ni de glifo. libmspub entrega la caja y los párrafos, y **el layout del texto (cortes de línea, desborde) lo tiene que hacer OpenImprenta**.

## Qué información se pierde

Separo lo **observado** en el corpus de lo **deducido del código** de libmspub 0.1.5.

1. **Cuadros de texto enlazados: no llega la cadena y la historia se duplica (observado).** libmspub manda la historia **completa** a **cada** cuadro de la cadena. En SampleNewsletter hay 10 historias repetidas en 2 o 3 cuadros (las columnas de un mismo artículo, por ejemplo tres cajas de 102×68 pt en la misma fila con los mismos 407 caracteres). En SampleBrochure hay 3. No llega ni el orden de la cadena, ni el identificador de la historia, ni dónde corta cada cuadro. Si se dibuja tal cual, el texto sale 2 o 3 veces. Ojo: los `LinkAt*.pub` del corpus **no son** texto enlazado sino pruebas de **hipervínculos** de Apache POI (ver el punto 2).
2. **Hipervínculos (observado).** El texto del vínculo llega con su estilo (azul `#0066ff`, subrayado), pero **la URL se pierde**: libmspub nunca llama a `openLink`. En los `Sample*.pub` aparece además un `#` suelto delante del primer vínculo (`"#This is a link to Apache POI"`).
3. **Campos de número de página (observado, interpretación probable).** Las cajas de encabezado de las páginas interiores del boletín dicen literalmente `"Page #"`. libmspub nunca llama a `insertField`, así que el campo automático llega, con toda probabilidad, como un `#` literal.
4. **Páginas maestras: llegan aplanadas (deducido del código).** `MSPUBCollector::writePage()` dibuja el fondo y las formas de la maestra **dentro** de cada página que la usa, y `startMasterPage` no se llama nunca (0 llamadas en los 21 archivos legibles). En el corpus no encontré ningún elemento idéntico en todas las páginas, así que no lo puedo mostrar con un archivo. Una vez aplanadas, no hay forma de distinguir lo que venía de la maestra.
5. **Colores: solo RGB directo (observado + código).** Todos los colores llegan como `#rrggbb`. libmspub resuelve las referencias a la paleta y al esquema de colores del documento, y aplica los tintes y sombras (`CHANGE_INTENSITY`) a RGB fijo. **No llegan CMYK, ni tintas planas, ni el índice del esquema de colores**, de modo que cambiar el esquema después no recolorea nada. Las imágenes JPEG CMYK se reconocen como tipo de blip; no tuve un archivo para ver cómo salen.
6. **WordArt (deducido del código, sin archivo para probar).** libmspub no lee las propiedades `gtext*` de Escher, que guardan el texto y la fuente del WordArt (no están en `EscherFieldIds.h`). Existen las geometrías de las formas `TEXT_*`, así que en el mejor caso llega el contorno sin el texto. No sé si algún archivo del corpus tiene WordArt.
7. **Tablas.** En Publisher 2002 y posteriores **llegan bien** (observado en Sample, Sample2-4 y \_2010: 3 filas × 2 columnas, anchos de columna, alto de fila y texto por celda; soporta celdas combinadas con `insertCoveredTableCell`). En **98/2000 se pierden**: solo quedan los bordes como líneas y desaparece el texto de las celdas.
8. **Tipos de forma (observado).** Todo llega como `drawPolygon` (85 en el boletín, 48 en el folleto) o algún `drawPath`. Nunca llegan `drawRectangle` ni `drawEllipse`. Rectángulos, óvalos y autoformas se convierten en polígonos con la rotación ya aplicada, así que se pierde la forma editable ("este es un rectángulo redondeado de 20 % de radio").
9. **Imágenes (observado).** Llegan como relleno bitmap de un polígono, y el recorte, como `svg:clip-path` de una capa. **9 de 10 imágenes del boletín y 5 de 8 del folleto son WMF**, que ningún navegador muestra. Hace falta un conversor de WMF/EMF a SVG o PNG. libmspub además reconoce DIB (que reconstruye como BMP), PICT y TIFF, que tampoco se ven en el navegador.
10. **Fuentes y texto (observado).** Algunos spans llegan sin tamaño (`size: null`, por ejemplo "Contoso Art Gallery" en el folleto), así que hay que suponer el tamaño por defecto. En 98/2000 los nombres de fuente se pierden. No hay fuentes embebidas en el corpus, aunque `defineEmbeddedFont` está soportado.
11. **Grupos (observado).** `openGroup` no se llama nunca: los grupos llegan aplanados.
12. **Metadatos.** Sin datos ICU se pierden en silencio el autor y demás cadenas en windows-1252 (corregido en este build). En 98/2000 salen vacíos.

### El SVG de librevenge 0.0.5 solo sirve para depurar

Rendericé las páginas con Chrome headless y revisé el SVG crudo:

- **El texto sale invisible:** `font-size` se escribe en pulgadas (`0.1181`) dentro de un `viewBox` en puntos. Además va en una sola línea, sin ajustarse a la caja, y anclado al borde inferior.
- **Las imágenes salen en mosaico:** el relleno bitmap se emite como un `<pattern>` fijo de 100×100, aunque la propiedad diga `style:repeat = stretch`.
- Los WMF van como `data:image/wmf`, que el navegador muestra como imagen rota.

Por eso el camino para OpenImprenta es `toJSON` más nuestro propio render o layout.

## Robustez

- **Corpus:** los 3 `clusterfuzz-*` fallan limpio (`isSupported` = false, o `parse()` = false en menos de 1 ms). No hay traps, cuelgues ni excepciones que lleguen a JS.
- **`test/fuzz-smoke.mjs`** muta 5 archivos semilla con bytes cambiados (en su mayoría después del encabezado OLE, para que la mutación llegue a libmspub) y truncados:

  | Build | Mutantes | Rechazados | OK | Parciales | **Traps** | **Cuelgues (>10 s)** |
  |---|---:|---:|---:|---:|---:|---:|
  | Parche 0001 solo | 500 (semilla 1) | 95 | 322 | 77 | 0 | **6** |
  | Parches 0001 + 0002 | 500 (semilla 1) | 95 | 329 | 76 | 0 | 0 |
  | Parches 0001 + 0002 | 500 (semilla 2) | 81 | 337 | 82 | 0 | 0 |
  | Parches 0001 + 0002 | 5000 (semilla 3) | 749 | 3469 | 782 | 0 | 0 |

  Esto no reemplaza un fuzzer guiado por cobertura: es una prueba de humo.
- **Los 6 cuelgues eran específicos de wasm.** En wasm, 4 tardaban 18–24 s por parseo (con 1,3 GB de RSS) y 2 no terminaban en 120 s. Con libmspub nativo x86-64, los mismos 6 archivos terminan en 120–245 ms (y 2 incluso producen salida). Con el perfil de CPU y la traza de depuración encontré la causa (ver el parche 0002).
- Igual conviene ejecutar el parser **en un Web Worker con timeout** en producción: un trap o un bucle infinito que no vimos no debería congelar la pestaña. `test/worker.mjs` muestra el patrón.

## Parches

### 0001: un metadato roto no debe tirar el documento entero

`MSPUBParser::parse()` llama a `parseMetaData()` con el comentario *"No check: metadata are not important enough to fail if they can't be parsed"*. Pero una `EndOfStreamException` leyendo `\005SummaryInformation` se escapa de `parse()` y `MSPUBDocument::parse()` devuelve false. Con la traza nativa se ve `Something bad happened here! Tell: 152` y nada más. El parche envuelve la llamada en `try/catch`. Recupera **SampleBrochure (2 páginas) y SampleNewsletter (4 páginas)**, que sin él no abren ni en nativo.

### 0002: largos falsos y `long` de 32 bits (cuelgues en wasm)

libmspub hace `input->seek(offset + largoLeídoDelArchivo, SET)`, y `RVNGInputStream::seek()` recibe un `long`. En wasm32, `long` es de 32 bits: un largo falso vuelve negativo el offset y `RVNGStringStream::seek()` **rebobina a 0** (`if (d->offset < 0) d->offset = 0`). El bucle que estaba leyendo vuelve a empezar desde el principio:

- en `parseEscherDelay` reinfla las mismas imágenes una y otra vez (el perfil muestra 12 s en `inflate` y 4 s en `RVNGBinaryData::append`);
- en los parsers de bloques, `skipBlock()` vuelve a leer los mismos bloques (la traza muestra los mismos offsets 7876 veces en 15 s).

En x86-64 ese offset es positivo, cae fuera del stream y el bucle termina. El parche acota `contentsLength` (en `parseEscherContainer`) y `dataLength` (en los bloques de largo variable de `parseBlock`) a lo que queda del stream. En archivos válidos no cambia nada: la salida del corpus es idéntica byte a byte.

Ambos parches son chicos y razonables para mandar upstream (Gerrit de LibreOffice, componente libmspub).

## Recomendación

**Sirve como base, no tal cual.** La lectura es rápida (menos de 25 ms para el documento más grande del corpus), el tamaño es aceptable (445 KB brotli, o ~260 KB sin los datos CJK de ICU) y el JSON es un punto de integración limpio. Para producción:

**Imprescindible (ya hecho en este spike):**

1. Parche 0001 (metadatos que tiran el documento).
2. Parche 0002 (cuelgues por `long` de 32 bits). Conviene revisar los demás `seek(a + b)` de libmspub con el mismo criterio: los acotados cubren todo lo que mostró el fuzz, pero hay más sitios con el mismo patrón (`chunk.offset + length`, `getEscherElementTailLength`).
3. Datos ICU para las páginas de códigos (o reemplazar `ucnv` por decodificadores propios de windows-125x; ver abajo).
4. Ejecutar en un Web Worker con timeout.

**Parches a libmspub que hacen falta para un importador digno, en orden de valor:**

1. **Cuadros enlazados.** Lo mínimo es emitir en `startTextObject` un identificador de historia (el `textId` que `MSPUBCollector` ya tiene, como `libmspub:story-id`), para no duplicar el texto y poder hacerlo fluir en nuestro motor. Lo ideal es además el orden de la cadena, que libmspub no lee hoy y requeriría ingeniería inversa del formato.
2. **Tipo de forma original.** Emitir el `ShapeType` y los ajustes como propiedades (por ejemplo `libmspub:shape-type`) además del polígono, para reconstruir rectángulos, óvalos y autoformas editables.
3. **Hipervínculos** (`openLink`/`closeLink`) y **campos** (`insertField` para número de página).
4. **Maestras sin aplanar:** emitirlas con `startMasterPage` y que cada página diga qué maestra usa.
5. **Colores:** emitir el índice del esquema y el valor original (CMYK o tinta plana) como propiedades adicionales.
6. **WordArt:** leer las propiedades `gtext*` de Escher.
7. **Publisher 98/2000:** tablas y nombres de fuente.

**Fuera de libmspub:** hace falta un conversor de **WMF/EMF** a SVG o PNG en el navegador, porque son la mayoría de las imágenes de las plantillas de Publisher.

**Tamaño:** usar -Oz. Embeber solo los conversores ICU de un byte (~78 KB) y cargar los CJK bajo demanda, o directamente parchear `appendCharacters` para no depender de `ucnv` (son tablas de 256 entradas). Esto último quizá permitiría sacar ICU entero, salvo `ucsdet` y `uloc`. No lo medí.

**Corpus:** POI tiene pocos documentos reales. Antes de decidir, conviene probar con `.pub` de usuarios (sobre todo de 2003, 2007 y 2010 con plantillas) y con al menos un archivo de Publisher 97, para verificar la ruta de codificación.

## Licencias

- **libmspub**: MPL-2.0. **librevenge**: MPL-2.0 / LGPL-2.1+ (doble licencia; usamos MPL-2.0). Los parches de `patches/` modifican archivos MPL-2.0, así que se distribuyen bajo MPL-2.0 y su código fuente tiene que estar disponible para quien reciba el binario. Este spike lo cumple: los parches y el `Dockerfile` reproducen el build exacto.
- **ICU 68.2** (código y datos embebidos): licencia de Unicode, Inc. ("ICU 58 and later"). **zlib 1.3.2**: licencia zlib. **boost 1.83** (solo headers): BSL-1.0. **Runtime de Emscripten**: MIT / University of Illinois NCSA. Todas piden conservar los avisos de copyright al distribuir.
- El código propio del spike (`src/`, `js/`, `web/`, `test/`) lleva `SPDX-License-Identifier: MPL-2.0`, para poder proponer el generador JSON upstream. Es compatible con la GPL-2.0-or-later de OpenImprenta (MPL-2.0 §3.3). Si se prefiere GPL para el código propio, hay que cambiar esas cabeceras.
- Los archivos del corpus (Apache POI, Apache-2.0) no se versionan (`corpus/` está en `.gitignore`) y tampoco las salidas derivadas (`out/`).
