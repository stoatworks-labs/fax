/**
 * The demo's CPU half (demo/fax.js) against the plugin's own C++ source.
 *
 *     demo/tools/check_port.sh          (builds refchain, exports the card, runs this)
 *     node demo/tools/check_port.mjs REFCHAIN DIR
 *
 * DIR holds card0.pbm and card1.pbm from `fxtest --export` (the test card as
 * the plugin's scan pass saw it, thresholded and halftone). REFCHAIN is
 * refchain.cpp built against source/Codec.cpp, Line.cpp, Header.cpp, Font.cpp,
 * Layout.cpp and Controls.cpp unchanged.
 *
 * ------------------------------------------------------------ what it compares
 *
 *   tables    every code string in fax.js against source/T4.h's text, and every
 *             length against tools/fixtures/t4-code-lengths.txt.
 *   font      every glyph row in fax.js against source/Font.cpp's text.
 *   pages     for each case (a page, and Coding, Resolution, Baud, Line Noise,
 *             Bursts, Concealment, Header, the page index and the time), EXACT
 *             equality of: the page after the header; the coded stream, bit for
 *             bit; each line's data / fill / total bits and its 1-D flag; the
 *             corrupted stream bit for bit and the flip count; every line's
 *             arrival time and the page duration (as doubles, exactly); the
 *             decoded page, the bad-line and shown flags, the segment count.
 *   layout    Layout::Compute's every field, for 3 fits x 3 resolutions x 7 sizes.
 *   controls  BitErrorRate, BurstBits, Threshold at v = 0, 0.01 .. 1; the fill floor per baud.
 *             Exact, except that the bit error rate is allowed ONE ulp: Math.pow
 *             and std::pow are not both correctly rounded, and they differ by an
 *             ulp at two of those values on this machine. The integer cutoff the
 *             line compares hashes against must still be identical.
 *   header    header::Text for 40 (page, time) pairs; the band height per resolution.
 *
 * ------------------------------------------------------------ what it cannot
 *
 * It compares the port with the plugin's SOURCE FUNCTIONS, called in the order
 * refchain.cpp copies from Fax::startPage by hand. It does not load the plugin,
 * does not check that order against Fax.cpp, and knows nothing about the page's
 * renderer in plugin.js (the frame sequence, the clock, the arrival bookkeeping,
 * the GL). It checks the pages listed below and no others: equality on these is
 * evidence about the port, not proof of it.
 */
import { execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, '..', '..');
const fax = await import(join(REPO, 'demo', 'fax.js'));

const [refchain, dir] = process.argv.slice(2);
if (!refchain || !dir) {
  console.error('usage: node check_port.mjs REFCHAIN DIR');
  process.exit(2);
}

let problems = 0;
const fail = (what) => {
  problems += 1;
  console.log(`FAIL  ${what}`);
};
const ok = (what) => console.log(`ok    ${what}`);

//---------------------------------------------------------------------------
// tables
//---------------------------------------------------------------------------
{
  const t4 = readFileSync(join(REPO, 'source', 'T4.h'), 'utf8');
  const pairs = (symbol) => {
    const m = new RegExp(`${symbol}\\[ \\w+ \\] = \\{([\\s\\S]*?)\\n\\};`).exec(t4);
    return [...m[1].matchAll(/\{ (\w+), "([01]+)" \}/g)].map((x) => [x[1], x[2]]);
  };
  const same = (name, cpp, js, numeric) => {
    const a = cpp.map(([v, b]) => `${numeric ? Number(v) : v}:${b}`).join(' ');
    const b = js.map(([v, bits]) => `${v}:${bits}`).join(' ');
    if (a !== b || cpp.length === 0) fail(`${name} differs from T4.h`);
    return cpp.length;
  };
  let n = 0;
  n += same('WHITE_TERMINATING', pairs('kWhiteTerminating'), fax.WHITE_TERMINATING, true);
  n += same('BLACK_TERMINATING', pairs('kBlackTerminating'), fax.BLACK_TERMINATING, true);
  n += same('WHITE_MAKEUP', pairs('kWhiteMakeup'), fax.WHITE_MAKEUP, true);
  n += same('BLACK_MAKEUP', pairs('kBlackMakeup'), fax.BLACK_MAKEUP, true);
  n += same('EXTENDED_MAKEUP', pairs('kExtendedMakeup'), fax.EXTENDED_MAKEUP, true);
  const modeNames = ['kPass', 'kHorizontal', 'kV0', 'kVR1', 'kVR2', 'kVR3', 'kVL1', 'kVL2', 'kVL3'];
  const cppModes = pairs('kModes').map(([v, b]) => [modeNames.indexOf(v), b]);
  n += same('MODES', cppModes, fax.MODES, false);
  if (!/kEol = "000000000001"/.test(t4) || fax.EOL !== '000000000001') fail('EOL differs from T4.h');

  // Lengths against the fixture golang's transcription gave.
  const fixture = readFileSync(join(REPO, 'tools', 'fixtures', 't4-code-lengths.txt'), 'utf8')
    .split('\n').filter((l) => l && !l.startsWith('#')).map((l) => l.split(' '));
  const ours = new Map();
  const put = (kind, list) => { for (const [v, b] of list) ours.set(`${kind} ${v}`, b.length); };
  put('white-term', fax.WHITE_TERMINATING);
  put('black-term', fax.BLACK_TERMINATING);
  put('white-makeup', fax.WHITE_MAKEUP);
  put('black-makeup', fax.BLACK_MAKEUP);
  put('ext-makeup', fax.EXTENDED_MAKEUP);
  put('modes', fax.MODES);
  let compared = 0;
  let unknown = 0;
  for (const [kind, value, length] of fixture) {
    const key = `${kind} ${value}`;
    if (!ours.has(key)) {
      unknown += 1;
      continue;
    }
    compared += 1;
    if (ours.get(key) !== Number(length)) fail(`${key}: fax.js ${ours.get(key)} bits, fixture ${length}`);
  }
  if (compared !== 204) fail(`only ${compared} fixture lines matched a code (${unknown} did not) -- the fixture's kinds have changed`);
  ok(`tables: ${n} codes equal to T4.h's strings; ${compared} lengths equal to the fixture${unknown ? ` (${unknown} fixture lines name no code here)` : ''}`);
}

//---------------------------------------------------------------------------
// font
//---------------------------------------------------------------------------
{
  const src = readFileSync(join(REPO, 'source', 'Font.cpp'), 'utf8');
  const rows = [...src.matchAll(/\{ ("[.#]{5}"(?:, "[.#]{5}"){6}) \}, \/\/ (\d+)/g)].map((m) => m[1].replace(/"/g, ''));
  const js = readFileSync(join(REPO, 'demo', 'fax.js'), 'utf8');
  const jsRows = [...js.matchAll(/\[('[.#]{5}'(?:, '[.#]{5}'){6})\], \/\/ (\d+)/g)].map((m) => m[1].replace(/'/g, ''));
  if (rows.length !== 96 || rows.join('|') !== jsRows.join('|')) fail(`font: ${jsRows.length} glyphs in fax.js, ${rows.length} in Font.cpp, and they differ`);
  else ok('font: all 96 glyphs equal to Font.cpp');
}

//---------------------------------------------------------------------------
// pages
//---------------------------------------------------------------------------
const scratch = mkdtempSync(join(tmpdir(), 'fax-port-'));

function readPbm(path) {
  const raw = readFileSync(path);
  const text = raw.subarray(0, 64).toString('latin1');
  const m = /^P4\n(\d+) (\d+)\n/.exec(text);
  if (!m || Number(m[1]) !== fax.WIDTH) throw new Error(`${path} is not a 1728-wide P4`);
  const lines = Number(m[2]);
  const body = raw.subarray(m[0].length);
  const page = new fax.Page(lines);
  for (let y = 0; y < lines; y += 1) {
    for (let x = 0; x < fax.WIDTH; x += 1) {
      if ((body[y * fax.LINE_BYTES + (x >> 3)] >> (7 - (x & 7))) & 1) page.setPel(y, x, 1);
    }
  }
  return page;
}

function writePbm(path, page) {
  const body = Buffer.alloc(page.lines * fax.LINE_BYTES);
  for (let y = 0; y < page.lines; y += 1) {
    for (let x = 0; x < fax.WIDTH; x += 1) {
      if (page.pel(y, x)) body[y * fax.LINE_BYTES + (x >> 3)] |= 0x80 >> (x & 7);
    }
  }
  writeFileSync(path, Buffer.concat([Buffer.from(`P4\n${fax.WIDTH} ${page.lines}\n`, 'latin1'), body]));
}

/** A page no scan would make: random runs from 1 pel to whole lines, a pel checker, solid lines. */
function synthetic() {
  const page = new fax.Page(300);
  let s = 1;
  const next = () => { s = fax.hash(s); return s; };
  for (let y = 0; y < page.lines; y += 1) {
    const kind = y % 10;
    if (kind === 3) { for (let x = 0; x < fax.WIDTH; x += 1) page.setPel(y, x, 1); continue; }
    if (kind === 5) { for (let x = 0; x < fax.WIDTH; x += 1) page.setPel(y, x, (x + y) & 1); continue; }
    if (kind === 7) continue;
    let x = 0;
    let colour = next() & 1;
    while (x < fax.WIDTH) {
      const scale = [4, 40, 400, 1800][next() & 3];
      const run = 1 + (next() % scale);
      for (let i = 0; i < run && x < fax.WIDTH; i += 1, x += 1) page.setPel(y, x, colour);
      colour = 1 - colour;
    }
  }
  return page;
}

const synthPath = join(scratch, 'synth.pbm');
writePbm(synthPath, synthetic());

const PAGES = { card0: join(dir, 'card0.pbm'), card1: join(dir, 'card1.pbm'), synth: synthPath };

// coding resolution baud noise bursts conceal header pageIndex seconds
const CASES = [
  ['card1', 1, 0, 3, 0.4, 0, 1, 1, 0, 0],          // the plugin's defaults, page 1
  ['card1', 1, 0, 3, 0.4, 0, 1, 1, 7, 12.5],       // the defaults, another page and time
  ['card0', 0, 0, 3, 0.0, 0, 1, 1, 0, 0],          // MH, clean
  ['card0', 0, 0, 0, 0.7, 0, 0, 1, 3, 61],         // MH, noisy, concealment off, 2400
  ['card0', 1, 1, 1, 0.75, 0, 1, 0, 2, 3601],      // MR K=4, noisy, Repeat Line, no header
  ['card1', 1, 2, 2, 0.6, 0, 0, 1, 5, 90000],      // MR K=8, concealment off
  ['card1', 0, 0, 3, 0.55, 0.5, 1, 1, 4, 1],       // bursts
  ['card1', 1, 0, 3, 1.0, 1.0, 0, 1, 9, 2],        // the worst line there is
  ['synth', 0, 0, 3, 0.0, 0, 1, 1, 0, 0],          // MH, clean, the synthetic page
  ['synth', 1, 0, 1, 0.0, 0, 1, 1, 0, 0],          // MR K=2, clean
  ['synth', 1, 2, 3, 0.8, 0.3, 1, 1, 11, 45],      // MR K=8, noisy
  ['synth', 1, 1, 0, 0.9, 0, 0, 0, 6, 0],          // MR K=4, very noisy, concealment off
];

const bytesEqual = (a, b) => a.length === b.length && Buffer.compare(Buffer.from(a.buffer, a.byteOffset, a.length), Buffer.from(b.buffer, b.byteOffset, b.length)) === 0;
const firstDiff = (a, b) => { const n = Math.min(a.length, b.length); for (let i = 0; i < n; i += 1) if (a[i] !== b[i]) return i; return n; };

let bitsCompared = 0;
for (const [pageName, coding, resolution, baudIndex, noiseV, burstsV, conceal, header, pageIndex, seconds] of CASES) {
  const label = `${pageName} ${fax.CODING_NAMES[coding]} ${fax.RESOLUTION_NAMES[resolution]} ${fax.BAUD_NAMES[baudIndex]} noise ${noiseV} bursts ${burstsV} ${fax.CONCEALMENT_NAMES[conceal]}${header ? ' header' : ''} p${pageIndex} t${seconds}`;
  const stem = join(scratch, 'case');
  execFileSync(refchain, ['page', PAGES[pageName], stem, coding, resolution, baudIndex, noiseV, burstsV, conceal, header, pageIndex, seconds].map(String));
  const ref = (ext) => new Uint8Array(readFileSync(`${stem}.${ext}`));
  const refText = (ext) => readFileSync(`${stem}.${ext}`, 'utf8').trim().split('\n');

  // Fax::startPage, as plugin.js runs it (the float params as the host stores them).
  const page = readPbm(PAGES[pageName]);
  const lpm = fax.linesPerMillimetre(resolution);
  const baud = fax.BAUD_RATES[baudIndex];
  if (header) fax.printHeader(page, lpm, fax.headerText(pageIndex + 1, seconds));
  const t = fax.encode(page, { coding, k: fax.kOf(resolution), minLineBits: fax.minLineBitsFor(baud) });
  const sent = t.bits.view().slice();
  const bursts = Math.fround(burstsV);
  const flips = fax.corrupt(t, {
    ber: fax.bitErrorRate(Math.fround(noiseV)),
    burstBits: bursts > 0 ? fax.burstBits(bursts) : 1,
    seed: fax.hash((Math.imul(pageIndex, 2654435761 | 0) + 12345) >>> 0),
  });
  const timing = fax.schedule(t, baud);
  const decoded = fax.decode(t.bits.data, t.bits.length, page.lines, { coding, concealment: conceal });

  const wrong = [];
  if (!bytesEqual(page.bytes, ref('scanned'))) wrong.push(`header page (byte ${firstDiff(page.bytes, ref('scanned'))})`);
  if (!bytesEqual(sent, ref('sent'))) wrong.push(`coded stream (${sent.length} vs ${ref('sent').length} bits, first difference at bit ${firstDiff(sent, ref('sent'))})`);
  const lines = refText('lines');
  const [pre, rtc] = lines[0].split(' ').map(Number);
  if (pre !== t.preambleBits || rtc !== t.rtcStart) wrong.push('preamble / RTC position');
  const ourLines = t.lines.map((r) => `${r.dataStart} ${r.dataBits} ${r.fillBits} ${r.totalBits} ${r.oneD ? 1 : 0}`);
  if (ourLines.join('\n') !== lines.slice(1).join('\n')) wrong.push('line records');
  if (!bytesEqual(t.bits.view(), ref('recv'))) wrong.push(`corrupted stream (first difference at bit ${firstDiff(t.bits.view(), ref('recv'))})`);
  const tim = refText('timing');
  if (Number(tim[0]) !== flips) wrong.push(`flips ${flips} vs ${tim[0]}`);
  if (Number(tim[1]) !== timing.duration) wrong.push(`duration ${timing.duration} vs ${tim[1]}`);
  let arrivalsWrong = 0;
  for (let i = 0; i < timing.arrival.length; i += 1) if (Number(tim[2 + i]) !== timing.arrival[i]) arrivalsWrong += 1;
  if (arrivalsWrong || tim.length - 2 !== timing.arrival.length) wrong.push(`${arrivalsWrong} arrival times`);
  if (!bytesEqual(decoded.page.bytes, ref('page'))) wrong.push(`decoded page (first difference in line ${Math.floor(firstDiff(decoded.page.bytes, ref('page')) / fax.LINE_BYTES)})`);
  if (!bytesEqual(decoded.bad, ref('bad'))) wrong.push('bad-line flags');
  if (!bytesEqual(decoded.shown, ref('shown'))) wrong.push('shown flags');
  if (Number(refText('segments')[0]) !== decoded.segments) wrong.push('segment count');

  bitsCompared += sent.length;
  const badLines = decoded.bad.reduce((a, b) => a + b, 0);
  if (wrong.length) fail(`${label}: ${wrong.join('; ')}`);
  else ok(`${label}: ${sent.length.toLocaleString('en-GB')} bits, ${flips} flipped, ${badLines} bad lines -- identical`);
}

//---------------------------------------------------------------------------
// layout, controls, header
//---------------------------------------------------------------------------
{
  const out = execFileSync(refchain, ['layout'], { encoding: 'utf8' }).trim().split('\n');
  let bad = 0;
  for (const row of out) {
    const f = row.split(' ');
    const [fit, res, w, h] = f.slice(0, 4).map(Number);
    const g = fax.computeLayout(fit, res, w, h);
    const ours = [g.lines, g.srcX0, g.srcDX, g.srcY0, g.srcDY, g.imgX0, g.imgX1, g.imgY0, g.imgY1, g.sheetX, g.sheetY, g.sheetW, g.sheetH];
    if (ours.some((v, i) => v !== Number(f[4 + i]))) {
      bad += 1;
      if (bad <= 3) fail(`layout ${row}: fax.js ${ours.join(' ')}`);
    }
  }
  if (bad) fail(`layout: ${bad} of ${out.length} differ`);
  else ok(`layout: ${out.length} (fit, resolution, size) cases, every field identical`);
}
{
  const out = execFileSync(refchain, ['controls'], { encoding: 'utf8' }).trim().split('\n');
  let bad = 0;
  let n = 0;
  let berUlps = 0;
  for (const row of out) {
    const f = row.split(' ');
    n += 1;
    if (f[0] === 'baud') {
      if (fax.minLineBitsFor(Number(f[1])) !== Number(f[2])) bad += 1;
      continue;
    }
    const i = n - 1;
    const v = Math.fround(i / 100);
    // Math.pow is not correctly rounded and std::pow need not be either: the
    // bit error rate may differ by an ulp. What the line uses is the integer
    // cutoff, and that must be exact; the rate itself within one ulp.
    const ber = fax.bitErrorRate(v);
    const theirs = Number(f[1]);
    const ulp = Math.abs(ber - theirs) <= Number.EPSILON * theirs;
    if (ber !== theirs) berUlps += 1;
    if (!ulp || fax.cutoffFor(ber) !== fax.cutoffFor(theirs) || fax.burstBits(v) !== Number(f[2]) || fax.threshold(v) !== Number(f[3])) {
      bad += 1;
      if (bad <= 3) fail(`controls at ${f[0]}: fax.js ${ber} ${fax.burstBits(v)} ${fax.threshold(v)} vs ${f.slice(1).join(' ')}`);
    }
  }
  if (bad) fail(`controls: ${bad} of ${out.length} differ`);
  else ok(`controls: ${out.length} conversions agree -- the cutoffs the line uses, the burst lengths, thresholds and fill floors exactly; ${berUlps} of 101 bit error rates differ by one ulp (Math.pow against std::pow), none changing a cutoff`);
}
{
  const out = execFileSync(refchain, ['header'], { encoding: 'utf8' }).trim().split('\n');
  const pages = [1, 2, 999, 1000, 1234];
  const secs = [0.0, 0.9999999, 59.5, 3599.0, 3600.0, 86399.0, 86400.5, 123456.7];
  const ours = [];
  for (const p of pages) for (const s of secs) ours.push(fax.headerText(p, s));
  for (const lpm of [3.85, 7.7, 15.4]) ours.push(`band ${fax.headerBandLines(lpm)}`);
  const bad = ours.filter((line, i) => line !== out[i]).length;
  if (bad || ours.length !== out.length) fail(`header: ${bad} of ${out.length} lines differ`);
  else ok(`header: ${out.length} texts and band heights identical`);
}

rmSync(scratch, { recursive: true, force: true });
console.log();
if (problems) {
  console.log(`${problems} problem(s): demo/fax.js no longer agrees with the plugin's source`);
  process.exit(1);
}
console.log(`the port agrees with the plugin's source on ${CASES.length} pages (${bitsCompared.toLocaleString('en-GB')} coded bits) and every table`);
