/**
 * Fax — the CPU half of the plugin, PORTED to JavaScript.
 *
 * This file is a hand translation, function for function, of the plugin's
 * C++: `source/T4.h` (the code tables), `source/Codec.cpp` (the MH / MR coder,
 * EOL segmentation, the decoder, bad-line detection and concealment),
 * `source/Line.cpp` (the PCG-hash bit errors, bursts and the page timing),
 * `source/Header.cpp` + `source/Font.cpp` (the sending machine's header line),
 * `source/Layout.cpp` (Fit and Resolution) and `source/Controls.cpp` (the
 * option names and the conversions). It has no DOM and no GL, so Node can load
 * it: `demo/tools/check_port.mjs` runs it against the plugin's own C++ source
 * compiled unchanged (see that file for exactly what it compares).
 *
 * What is NOT ported: the `Perturb` test hooks (always 0 in the plugin), the
 * negative-control decoder that never resynchronises, `pelAtBit`, the trie
 * decoder the harness uses, and the forced-error hook. None of them is part of
 * what the plugin does in a host.
 *
 * Differences of implementation that are not differences of result: the
 * decoder's stream is packed into 32-bit words rather than 64-bit ones (a JS
 * number has no fast 64-bit integer), and the EOL search walks the unpacked
 * stream instead of counting leading zeros. Both find the same codes and the
 * same EOLs; the check says whether they do on the pages it tries.
 */

export const WIDTH = 1728;
export const LINE_BYTES = WIDTH / 8;

export const WHITE = 0;
export const BLACK = 1;
export const MH = 0;
export const MR = 1;
export const CONCEAL_OFF = 0;
export const CONCEAL_REPEAT = 1;

export const SHOWN_DECODED = 0;
export const SHOWN_CONCEALED = 1;
export const SHOWN_MISSING = 2;

/** std::lround, for the non-negative values it is used on here and the rest. */
const lround = (x) => (x < 0 ? -Math.round(-x) : Math.round(x));
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

//===========================================================================
// T4.h — the Recommendation's code tables, as the plugin transcribed them.
// Written here by script from T4.h's own text; check_port.mjs compares every
// string with T4.h again and every length with tools/fixtures.
//===========================================================================

/// Table 2/T.4, white terminating codes, 0..63.
export const WHITE_TERMINATING = [
  [0, '00110101'], [1, '000111'], [2, '0111'], [3, '1000'], [4, '1011'], [5, '1100'], [6, '1110'],
  [7, '1111'], [8, '10011'], [9, '10100'], [10, '00111'], [11, '01000'], [12, '001000'],
  [13, '000011'], [14, '110100'], [15, '110101'], [16, '101010'], [17, '101011'], [18, '0100111'],
  [19, '0001100'], [20, '0001000'], [21, '0010111'], [22, '0000011'], [23, '0000100'],
  [24, '0101000'], [25, '0101011'], [26, '0010011'], [27, '0100100'], [28, '0011000'],
  [29, '00000010'], [30, '00000011'], [31, '00011010'], [32, '00011011'], [33, '00010010'],
  [34, '00010011'], [35, '00010100'], [36, '00010101'], [37, '00010110'], [38, '00010111'],
  [39, '00101000'], [40, '00101001'], [41, '00101010'], [42, '00101011'], [43, '00101100'],
  [44, '00101101'], [45, '00000100'], [46, '00000101'], [47, '00001010'], [48, '00001011'],
  [49, '01010010'], [50, '01010011'], [51, '01010100'], [52, '01010101'], [53, '00100100'],
  [54, '00100101'], [55, '01011000'], [56, '01011001'], [57, '01011010'], [58, '01011011'],
  [59, '01001010'], [60, '01001011'], [61, '00110010'], [62, '00110011'], [63, '00110100'],
];

/// Table 2/T.4, black terminating codes, 0..63.
export const BLACK_TERMINATING = [
  [0, '0000110111'], [1, '010'], [2, '11'], [3, '10'], [4, '011'], [5, '0011'], [6, '0010'],
  [7, '00011'], [8, '000101'], [9, '000100'], [10, '0000100'], [11, '0000101'], [12, '0000111'],
  [13, '00000100'], [14, '00000111'], [15, '000011000'], [16, '0000010111'], [17, '0000011000'],
  [18, '0000001000'], [19, '00001100111'], [20, '00001101000'], [21, '00001101100'],
  [22, '00000110111'], [23, '00000101000'], [24, '00000010111'], [25, '00000011000'],
  [26, '000011001010'], [27, '000011001011'], [28, '000011001100'], [29, '000011001101'],
  [30, '000001101000'], [31, '000001101001'], [32, '000001101010'], [33, '000001101011'],
  [34, '000011010010'], [35, '000011010011'], [36, '000011010100'], [37, '000011010101'],
  [38, '000011010110'], [39, '000011010111'], [40, '000001101100'], [41, '000001101101'],
  [42, '000011011010'], [43, '000011011011'], [44, '000001010100'], [45, '000001010101'],
  [46, '000001010110'], [47, '000001010111'], [48, '000001100100'], [49, '000001100101'],
  [50, '000001010010'], [51, '000001010011'], [52, '000000100100'], [53, '000000110111'],
  [54, '000000111000'], [55, '000000100111'], [56, '000000101000'], [57, '000001011000'],
  [58, '000001011001'], [59, '000000101011'], [60, '000000101100'], [61, '000001011010'],
  [62, '000001100110'], [63, '000001100111'],
];

/// Table 3a/T.4, white make-up codes, 64..1728.
export const WHITE_MAKEUP = [
  [64, '11011'], [128, '10010'], [192, '010111'], [256, '0110111'], [320, '00110110'],
  [384, '00110111'], [448, '01100100'], [512, '01100101'], [576, '01101000'], [640, '01100111'],
  [704, '011001100'], [768, '011001101'], [832, '011010010'], [896, '011010011'],
  [960, '011010100'], [1024, '011010101'], [1088, '011010110'], [1152, '011010111'],
  [1216, '011011000'], [1280, '011011001'], [1344, '011011010'], [1408, '011011011'],
  [1472, '010011000'], [1536, '010011001'], [1600, '010011010'], [1664, '011000'],
  [1728, '010011011'],
];

/// Table 3a/T.4, black make-up codes, 64..1728.
export const BLACK_MAKEUP = [
  [64, '0000001111'], [128, '000011001000'], [192, '000011001001'], [256, '000001011011'],
  [320, '000000110011'], [384, '000000110100'], [448, '000000110101'], [512, '0000001101100'],
  [576, '0000001101101'], [640, '0000001001010'], [704, '0000001001011'], [768, '0000001001100'],
  [832, '0000001001101'], [896, '0000001110010'], [960, '0000001110011'], [1024, '0000001110100'],
  [1088, '0000001110101'], [1152, '0000001110110'], [1216, '0000001110111'],
  [1280, '0000001010010'], [1344, '0000001010011'], [1408, '0000001010100'],
  [1472, '0000001010101'], [1536, '0000001011010'], [1600, '0000001011011'],
  [1664, '0000001100100'], [1728, '0000001100101'],
];

/// Table 3b/T.4, extended make-up codes, both colours.
export const EXTENDED_MAKEUP = [
  [1792, '00000001000'], [1856, '00000001100'], [1920, '00000001101'], [1984, '000000010010'],
  [2048, '000000010011'], [2112, '000000010100'], [2176, '000000010101'], [2240, '000000010110'],
  [2304, '000000010111'], [2368, '000000011100'], [2432, '000000011101'], [2496, '000000011110'],
  [2560, '000000011111'],
];

/// Table 4/T.4, the two-dimensional modes, in T4.h's Mode order.
export const PASS = 0;
export const HORIZONTAL = 1;
export const V0 = 2;
export const VR1 = 3;
export const VR2 = 4;
export const VR3 = 5;
export const VL1 = 6;
export const VL2 = 7;
export const VL3 = 8;
export const MODES = [
  [PASS, '0001'], [HORIZONTAL, '001'], [V0, '1'],
  [VR1, '011'], [VR2, '000011'], [VR3, '0000011'],
  [VL1, '010'], [VL2, '000010'], [VL3, '0000010'],
];

export const EOL = '000000000001';
export const EOL_BITS = 12;
export const RTC_EOLS = 6;
export const LINE_MILLIMETRES = 215.0;
export const A4_MILLIMETRES = 297.0;

//===========================================================================
// Codec.h — the page and the bit stream.
//===========================================================================

/** 1728 pels a line, one bit a pel, 1 = black, pel 8i + b in bit b of byte i. */
export class Page {
  constructor(lines = 0) {
    this.lines = 0;
    this.bytes = new Uint8Array(0);
    if (lines > 0) this.resize(lines);
  }

  resize(lines) {
    this.lines = lines;
    this.bytes = new Uint8Array(lines * LINE_BYTES);
  }

  clear() {
    this.bytes.fill(0);
  }

  rowOffset(line) {
    return line * LINE_BYTES;
  }

  row(line) {
    return this.bytes.subarray(line * LINE_BYTES, (line + 1) * LINE_BYTES);
  }

  pel(line, x) {
    return (this.bytes[line * LINE_BYTES + (x >> 3)] >> (x & 7)) & 1;
  }

  setPel(line, x, colour) {
    const i = line * LINE_BYTES + (x >> 3);
    if (colour) this.bytes[i] |= 1 << (x & 7);
    else this.bytes[i] &= ~(1 << (x & 7));
  }
}

/** A bit stream, one byte a bit, first transmitted bit first (Codec.h's Bits). */
export class Bits {
  constructor(capacity = 1 << 16) {
    this.data = new Uint8Array(capacity);
    this.length = 0;
  }

  clear() {
    this.length = 0;
  }

  reserve(n) {
    if (n <= this.data.length) return;
    let size = this.data.length || 1;
    while (size < n) size *= 2;
    const next = new Uint8Array(size);
    next.set(this.data.subarray(0, this.length));
    this.data = next;
  }

  push(bit) {
    if (this.length === this.data.length) this.reserve(this.length + 1);
    this.data[this.length++] = bit;
  }

  /** A compiled code: a Uint8Array of 0 / 1. */
  append(code) {
    const n = code.length;
    if (this.length + n > this.data.length) this.reserve(this.length + n);
    this.data.set(code, this.length);
    this.length += n;
  }

  zeros(n) {
    if (n <= 0) return;
    if (this.length + n > this.data.length) this.reserve(this.length + n);
    this.data.fill(0, this.length, this.length + n);
    this.length += n;
  }

  view() {
    return this.data.subarray(0, this.length);
  }
}

//---------------------------------------------------------------------------
// The codes compiled once, as Codec.cpp's `Compiled` and `Luts` do.
//---------------------------------------------------------------------------
const compile = (s) => Uint8Array.from(s, (c) => (c === '1' ? 1 : 0));

const CODES = (() => {
  const term = [WHITE_TERMINATING.map(([, b]) => compile(b)), BLACK_TERMINATING.map(([, b]) => compile(b))];
  const makeup = [WHITE_MAKEUP.map(([, b]) => compile(b)), BLACK_MAKEUP.map(([, b]) => compile(b))];
  const ext = EXTENDED_MAKEUP.map(([, b]) => compile(b));
  const modes = MODES.map(([, b]) => compile(b));
  return { term, makeup, ext, modes, eol: compile(EOL) };
})();

/** A lookup table over the next `width` bits: value, length (0: no code), make-up. */
class Lut {
  constructor(width) {
    this.width = width;
    const size = 1 << width;
    this.value = new Int16Array(size);
    this.length = new Uint8Array(size);
    this.makeup = new Uint8Array(size);
  }

  add(code, value, makeup) {
    const n = code.length;
    let prefix = 0;
    for (let i = 0; i < n; i += 1) prefix = (prefix << 1) | (code[i] === '1' ? 1 : 0);
    const first = prefix << (this.width - n);
    const count = 1 << (this.width - n);
    for (let i = 0; i < count; i += 1) {
      this.value[first + i] = value;
      this.length[first + i] = n;
      this.makeup[first + i] = makeup;
    }
  }
}

const LUTS = (() => {
  const colour = [new Lut(13), new Lut(13)];
  const modes = new Lut(7);
  for (const [v, b] of WHITE_TERMINATING) colour[WHITE].add(b, v, 0);
  for (const [v, b] of WHITE_MAKEUP) colour[WHITE].add(b, v, 1);
  for (const [v, b] of BLACK_TERMINATING) colour[BLACK].add(b, v, 0);
  for (const [v, b] of BLACK_MAKEUP) colour[BLACK].add(b, v, 1);
  for (const [v, b] of EXTENDED_MAKEUP) {
    colour[WHITE].add(b, v, 1);
    colour[BLACK].add(b, v, 1);
  }
  for (const [v, b] of MODES) modes.add(b, v, 0);
  return { colour, modes };
})();

/** The stream packed first-bit-most-significant into 32-bit words (Codec.cpp's Packed, in 32 bits). */
class Packed {
  constructor(bits, size) {
    this.bits = bits;
    this.size = size;
    this.words = new Uint32Array((size >>> 5) + 2);
    const words = this.words;
    for (let i = 0; i < size; i += 1) {
      if (bits[i]) words[i >>> 5] |= 0x80000000 >>> (i & 31);
    }
  }

  /** The next k bits (k <= 16) at pos, zeros past the end. */
  peek(pos, k) {
    const w = pos >>> 5;
    const off = pos & 31;
    let v = this.words[w] << off;
    if (off !== 0) v |= this.words[w + 1] >>> (32 - off);
    return (v >>> (32 - k)) >>> 0;
  }

  /** Any 1 in [begin, end)? */
  any(begin, end) {
    const bits = this.bits;
    for (let i = begin; i < end; i += 1) if (bits[i]) return true;
    return false;
  }

  /** Every EOL: the index of the 1 that ends eleven or more zeros. */
  eols() {
    const out = [];
    const bits = this.bits;
    let zeros = 0;
    for (let i = 0; i < this.size; i += 1) {
      if (bits[i] === 0) {
        zeros += 1;
      } else {
        if (zeros >= 11) out.push(i);
        zeros = 0;
      }
    }
    return out;
  }
}

/** Lut::Read: the code at `pos`, or -1 if none or if it would run past `end`. Advances state.pos. */
function lutRead(lut, packed, state, end) {
  const e = packed.peek(state.pos, lut.width);
  const n = lut.length[e];
  if (n === 0 || state.pos + n > end) return -1;
  state.pos += n;
  return e;
}

function readRunFast(packed, state, end, colour) {
  const lut = LUTS.colour[colour];
  let run = 0;
  for (;;) {
    const e = lutRead(lut, packed, state, end);
    if (e < 0) return -1;
    run += lut.value[e];
    if (!lut.makeup[e]) return run;
  }
}

//---------------------------------------------------------------------------
// Changing elements (4.2.1.3.1), into a reusable Int32Array. Returns the count
// including the two sentinels at WIDTH.
//---------------------------------------------------------------------------
function changesOf(bytes, offset, out) {
  let n = 0;
  let previous = WHITE;
  for (let byte = 0; byte < LINE_BYTES; byte += 1) {
    const value = bytes[offset + byte];
    if (value === (previous ? 0xff : 0x00)) continue;
    for (let b = 0; b < 8; b += 1) {
      const pel = (value >> b) & 1;
      if (pel !== previous) {
        out[n++] = byte * 8 + b;
        previous = pel;
      }
    }
  }
  out[n++] = WIDTH;
  out[n++] = WIDTH;
  return n;
}

const colourAfter = (k) => ((k & 1) === 0 ? BLACK : WHITE);

function fillPels(out, offset, from, to, colour) {
  from = clamp(from, 0, WIDTH);
  to = clamp(to, 0, WIDTH);
  for (let x = from; x < to; x += 1) {
    const i = offset + (x >> 3);
    if (colour) out[i] |= 1 << (x & 7);
    else out[i] &= ~(1 << (x & 7));
  }
}

//---------------------------------------------------------------------------
// The coder.
//---------------------------------------------------------------------------

export function putBits(out, s) {
  out.append(compile(s));
}

/** One run of `colour`, as make-up codes and one terminating code (4.1.1). */
export function putRun(out, colour, run) {
  while (run > 2560) {
    out.append(CODES.ext[12]);
    run -= 2560;
  }
  if (run >= 64) {
    const m = Math.floor(run / 64);
    if (m <= 27) out.append(CODES.makeup[colour][m - 1]);
    else out.append(CODES.ext[m - 28]);
    run -= m * 64;
  }
  out.append(CODES.term[colour][run]);
}

/** 4.2.1.3: the two-dimensional coding procedure over changing elements c (cn) and r (rn). */
function encode2D(c, cn, r, rn, out) {
  const cLast = cn - 2;
  const rLast = rn - 2;
  let ka = 0;
  let kb = 0;
  let a0 = -1;
  let colour = WHITE;
  while (a0 < WIDTH) {
    while (ka < cLast && c[ka] <= a0) ka += 1;
    const a1 = c[ka];
    const a2 = c[ka + 1];
    while (kb < rLast && r[kb] <= a0) kb += 1;
    let kb1 = kb;
    if (kb1 < rLast && colourAfter(kb1) === colour) kb1 += 1;
    const b1 = r[kb1];
    const b2 = r[kb1 + 1 < rn ? kb1 + 1 : kb1];

    if (b2 < a1) {
      out.append(CODES.modes[PASS]);
      a0 = b2;
      continue;
    }
    const d = a1 - b1;
    if (d >= -3 && d <= 3) {
      const mode = d === 0 ? V0 : d > 0 ? VR1 + d - 1 : VL1 - d - 1;
      out.append(CODES.modes[mode]);
      a0 = a1;
      colour = 1 - colour;
      continue;
    }
    out.append(CODES.modes[HORIZONTAL]);
    const start = Math.max(a0, 0);
    putRun(out, colour, a1 - start);
    putRun(out, 1 - colour, a2 - a1);
    a0 = a2;
  }
}

function encode1D(changes, out) {
  let a0 = 0;
  let colour = WHITE;
  for (let k = 0; ; k += 1) {
    const a1 = changes[k];
    putRun(out, colour, a1 - a0);
    if (a1 >= WIDTH) break;
    a0 = a1;
    colour = 1 - colour;
  }
}

const WHITE_LINE = new Uint8Array(LINE_BYTES);
const scratchC = new Int32Array(WIDTH + 4);
const scratchR = new Int32Array(WIDTH + 4);

/** One line coded against `reference` (null: the imaginary white line). For the check. */
export function encodeLine2D(bytes, offset, refBytes, refOffset, out) {
  const cn = changesOf(bytes, offset, scratchC);
  const rn = refBytes ? changesOf(refBytes, refOffset, scratchR) : changesOf(WHITE_LINE, 0, scratchR);
  encode2D(scratchC, cn, scratchR, rn, out);
}

/**
 * Code a page (Codec.cpp's Encode). Returns { bits, preambleBits, lines, rtcStart };
 * `bits` is a Bits.
 */
export function encode(page, { coding = MH, k = 2, minLineBits = 0 } = {}, out = null) {
  const t = out ?? { bits: new Bits(page.lines * 64), preambleBits: 0, lines: [], rtcStart: 0 };
  t.bits.clear();
  t.lines = [];
  const mr = coding === MR;
  const kk = Math.max(1, k);
  t.bits.reserve(page.lines * 64);

  let changes = new Int32Array(WIDTH + 4);
  let reference = new Int32Array(WIDTH + 4);
  let cn = changesOf(WHITE_LINE, 0, changes);
  let rn = 0;

  const oneDimensional = (line) => (!mr ? true : line % kk === 0);

  putBits(t.bits, EOL);
  if (mr) t.bits.push(oneDimensional(0) ? 1 : 0);
  t.preambleBits = t.bits.length;

  const tail = EOL_BITS + (mr ? 1 : 0);
  for (let line = 0; line < page.lines; line += 1) {
    const dataStart = t.bits.length;
    const oneD = oneDimensional(line);
    // This line's changing elements, and the one above's (its reference).
    const swap = reference;
    reference = changes;
    rn = cn;
    changes = swap;
    cn = changesOf(page.bytes, line * LINE_BYTES, changes);
    if (oneD) encode1D(changes, t.bits);
    else encode2D(changes, cn, reference, rn, t.bits);
    const dataBits = t.bits.length - dataStart;

    const nextOneD = line + 1 < page.lines ? oneDimensional(line + 1) : true;
    let fill = 0;
    if (dataBits + tail < minLineBits) fill = minLineBits - dataBits - tail;
    t.bits.zeros(fill);
    t.bits.append(CODES.eol);
    if (mr) t.bits.push(nextOneD ? 1 : 0);

    t.lines.push({ dataStart, dataBits, fillBits: fill, totalBits: dataBits + fill + tail, oneD });
  }

  t.rtcStart = t.bits.length;
  for (let i = 0; i < RTC_EOLS; i += 1) {
    t.bits.append(CODES.eol);
    if (mr) t.bits.push(1);
  }
  return t;
}

//---------------------------------------------------------------------------
// The decoder.
//---------------------------------------------------------------------------

const refChanges = new Int32Array(WIDTH + 4);

/** Codec.cpp's decodeLine: one line's stretch [begin, end) into out at outOffset. True if good. */
function decodeLine(packed, begin, end, twoD, refBytes, refOffset, out, outOffset) {
  out.fill(0, outOffset, outOffset + LINE_BYTES);
  const state = { pos: begin };
  let colour = WHITE;
  let a0 = twoD ? -1 : 0;
  let bad = false;

  if (!twoD) {
    const luts = LUTS.colour;
    while (a0 < WIDTH) {
      let run = 0;
      let ok = false;
      for (;;) {
        const lut = luts[colour];
        const e = lutRead(lut, packed, state, end);
        if (e < 0) break;
        run += lut.value[e];
        if (!lut.makeup[e]) {
          ok = true;
          break;
        }
      }
      if (!ok) {
        bad = true;
        break;
      }
      if (a0 + run > WIDTH) {
        fillPels(out, outOffset, a0, WIDTH, colour);
        a0 = WIDTH;
        bad = true;
        break;
      }
      fillPels(out, outOffset, a0, a0 + run, colour);
      a0 += run;
      if (a0 < WIDTH) colour = 1 - colour;
    }
  } else {
    const r = refChanges;
    const rn = refBytes ? changesOf(refBytes, refOffset, r) : changesOf(WHITE_LINE, 0, r);
    const rLast = rn - 2;
    let kb = 0;
    const modes = LUTS.modes;
    while (a0 < WIDTH) {
      const e = lutRead(modes, packed, state, end);
      if (e < 0) {
        bad = true;
        break;
      }
      while (kb < rLast && r[kb] <= a0) kb += 1;
      let kb1 = kb;
      if (kb1 < rLast && colourAfter(kb1) === colour) kb1 += 1;
      const b1 = r[kb1];
      const b2 = r[kb1 + 1 < rn ? kb1 + 1 : kb1];
      const start = Math.max(a0, 0);
      const mode = modes.value[e];

      if (mode === PASS) {
        fillPels(out, outOffset, start, b2, colour);
        a0 = b2;
        continue;
      }
      if (mode === HORIZONTAL) {
        const run1 = readRunFast(packed, state, end, colour);
        const run2 = run1 < 0 ? -1 : readRunFast(packed, state, end, 1 - colour);
        if (run1 < 0 || run2 < 0) {
          bad = true;
          break;
        }
        if (start + run1 + run2 > WIDTH) {
          fillPels(out, outOffset, start, start + run1, colour);
          fillPels(out, outOffset, start + run1, WIDTH, 1 - colour);
          a0 = WIDTH;
          colour = 1 - colour;
          bad = true;
          break;
        }
        fillPels(out, outOffset, start, start + run1, colour);
        fillPels(out, outOffset, start + run1, start + run1 + run2, 1 - colour);
        a0 = start + run1 + run2;
        continue;
      }
      const d = mode === V0 ? 0 : mode <= VR3 ? mode - VR1 + 1 : -(mode - VL1 + 1);
      const a1 = b1 + d;
      if (a1 < 0 || a1 > WIDTH || (a0 >= 0 && a1 <= a0)) {
        bad = true;
        break;
      }
      fillPels(out, outOffset, start, a1, colour);
      a0 = a1;
      colour = 1 - colour;
    }
  }

  if (bad) {
    // Show the garbage: the colour it was in at the failure, to the edge.
    fillPels(out, outOffset, Math.max(a0, 0), WIDTH, colour);
    return false;
  }
  // Past the 1728th pel only fill may follow.
  const bits = packed.bits;
  for (let i = state.pos; i < end; i += 1) if (bits[i] !== 0) return false;
  return true;
}

/**
 * Decode a stream of `size` bits into `lines` lines (Codec.cpp's Decode, the
 * resynchronising path the plugin runs). Returns { page, bad, shown, segments }.
 */
export function decode(bits, size, lines, { coding = MH, concealment = CONCEAL_REPEAT } = {}, out = null) {
  const result = out ?? { page: new Page(), bad: null, shown: null, segments: 0 };
  if (result.page.lines !== lines) result.page.resize(lines);
  else result.page.clear();
  result.bad = new Uint8Array(lines);
  result.shown = new Uint8Array(lines).fill(SHOWN_MISSING);
  result.segments = 0;

  const mr = coding === MR;
  const scratch = new Uint8Array(LINE_BYTES);
  const pageBytes = result.page.bytes;
  let line = 0;
  let referenceSpoiled = false;

  const emit = (good, twoD) => {
    if (line >= lines) return;
    const row = line * LINE_BYTES;
    result.bad[line] = good ? 0 : 1;
    if (!twoD) referenceSpoiled = false;
    const conceal = concealment === CONCEAL_REPEAT && (!good || (mr && twoD && referenceSpoiled));
    if (conceal) {
      const from = line - 1;
      if (from >= 0) pageBytes.copyWithin(row, from * LINE_BYTES, from * LINE_BYTES + LINE_BYTES);
      else pageBytes.fill(0, row, row + LINE_BYTES);
      result.shown[line] = SHOWN_CONCEALED;
      if (mr) referenceSpoiled = true;
    } else {
      pageBytes.set(scratch, row);
      result.shown[line] = SHOWN_DECODED;
    }
    line += 1;
  };

  const packed = new Packed(bits, size);
  const eols = packed.eols();
  for (let e = 0; e < eols.length && line < lines; e += 1) {
    let begin = eols[e] + 1;
    let twoD = false;
    if (mr) {
      if (begin >= size) break;
      twoD = bits[begin] === 0;
      begin += 1;
    }
    const end = e + 1 < eols.length ? eols[e + 1] - 11 : size;
    if (end <= begin || !packed.any(begin, end)) continue;
    result.segments += 1;
    const good = decodeLine(packed, begin, end, twoD, line > 0 ? pageBytes : null, (line - 1) * LINE_BYTES, scratch, 0);
    emit(good, twoD);
  }
  return result;
}

//===========================================================================
// Line.cpp — the telephone line.
//===========================================================================

/** PCG output mix, exact in 32 bits. */
export function hash(value) {
  const state = (Math.imul(value >>> 0, 747796405) + 2891336453) >>> 0;
  const word = Math.imul(((state >>> ((state >>> 28) + 4)) ^ state) >>> 0, 277803737) >>> 0;
  return ((word >>> 22) ^ word) >>> 0;
}

/** The 32-bit threshold a bit's hash is compared against: Line.cpp's `cutoff`. */
export function cutoffFor(ber) {
  const scaled = Math.min(ber, 1.0) * 4294967296.0;
  return scaled >= 4294967295.0 ? 0xffffffff : Math.trunc(scaled);
}

/** Flip bits in place (Line.cpp's Corrupt, without the harness's forced errors). Returns how many flipped. */
export function corrupt(t, { ber = 0, burstBits = 1, seed = 0 } = {}) {
  let flips = 0;
  if (ber <= 0) return flips;
  const bits = t.bits.data;
  const size = t.bits.length;
  const cutoff = cutoffFor(ber);
  const burst = Math.max(1, burstBits);

  const region = (id, begin, end) => {
    const s = (seed ^ hash((id + 0x9e3779b9) >>> 0)) >>> 0;
    const rs = hash(s);
    for (let o = begin; o < end; o += 1) {
      if (hash((rs + (o - begin)) >>> 0) >= cutoff) continue;
      bits[o] ^= 1;
      flips += 1;
      for (let i = 1; i < burst && o + i < size; i += 1) {
        if (hash((rs ^ Math.imul((o - begin + i) >>> 0, 2654435761 | 0)) >>> 0) & 1) {
          bits[o + i] ^= 1;
          flips += 1;
        }
      }
      o += burst - 1;
    }
  };

  region(0xffffffff, 0, t.preambleBits);
  for (let j = 0; j < t.lines.length; j += 1) {
    const r = t.lines[j];
    region(j, r.dataStart, Math.min(size, r.dataStart + r.totalBits));
  }
  region(0xfffffffe, t.rtcStart, size);
  return flips;
}

/** Line i has arrived when its EOL has: ( preamble + lines 0..i ) / baud. */
export function schedule(t, baud) {
  const rate = Math.max(baud, 1);
  const arrival = new Float64Array(t.lines.length);
  let cumulative = t.preambleBits;
  for (let i = 0; i < t.lines.length; i += 1) {
    cumulative += t.lines[i].totalBits;
    arrival[i] = cumulative / rate;
  }
  return { arrival, duration: t.bits.length / rate };
}

/** Lines arrived `elapsed` seconds into the page: std::upper_bound. */
export function arrived(timing, elapsed) {
  const a = timing.arrival;
  let lo = 0;
  let hi = a.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    if (a[mid] <= elapsed) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

//===========================================================================
// Layout.cpp — Fit and Resolution.
//===========================================================================

export const FIT_FRAME = 0;
export const FIT_A4_CROP = 1;
export const FIT_A4_LETTERBOX = 2;
export const FIT_NAMES = ['Frame', 'A4 Crop', 'A4 Letterbox'];
export const RESOLUTION_NAMES = ['Standard', 'Fine', 'Superfine'];

export const linesPerMillimetre = (resolution) => (resolution === 1 ? 7.7 : resolution === 2 ? 15.4 : 3.85);
export const kOf = (resolution) => (resolution === 1 ? 4 : resolution === 2 ? 8 : 2);

export function place(g, fit, outW, outH) {
  const w = Math.max(outW, 1);
  const h = Math.max(outH, 1);
  if (fit === FIT_FRAME) {
    g.sheetX = 0;
    g.sheetY = 0;
    g.sheetW = w;
    g.sheetH = h;
    return g;
  }
  let sh = h;
  let sw = (h * LINE_MILLIMETRES) / A4_MILLIMETRES;
  if (sw > w) {
    sw = w;
    sh = (w * A4_MILLIMETRES) / LINE_MILLIMETRES;
  }
  g.sheetW = sw;
  g.sheetH = sh;
  g.sheetX = (w - sw) * 0.5;
  g.sheetY = (h - sh) * 0.5;
  return g;
}

export function computeLayout(fit, resolution, inW, inH) {
  const g = { lines: 0, srcX0: 0, srcDX: 1, srcY0: 0, srcDY: 1, imgX0: 0, imgX1: 0, imgY0: 0, imgY1: 0, sheetX: 0, sheetY: 0, sheetW: 0, sheetH: 0 };
  const W = Math.max(inW, 1);
  const H = Math.max(inH, 1);
  const lpm = linesPerMillimetre(resolution);
  const pelMm = LINE_MILLIMETRES / WIDTH;

  if (fit === FIT_FRAME) {
    const pageMm = (LINE_MILLIMETRES * H) / W;
    g.lines = Math.max(1, lround(pageMm * lpm));
    g.srcX0 = 0;
    g.srcDX = W / WIDTH;
    g.srcY0 = 0;
    g.srcDY = H / g.lines;
    g.imgX0 = 0;
    g.imgX1 = WIDTH;
    g.imgY0 = 0;
    g.imgY1 = g.lines;
    return place(g, fit, inW, inH);
  }

  g.lines = lround(A4_MILLIMETRES * lpm);
  const lineMm = A4_MILLIMETRES / g.lines;
  const sx = LINE_MILLIMETRES / W;
  const sy = A4_MILLIMETRES / H;
  const s = fit === FIT_A4_CROP ? Math.max(sx, sy) : Math.min(sx, sy);
  const iw = W * s;
  const ih = H * s;
  const ix = (LINE_MILLIMETRES - iw) * 0.5;
  const iy = (A4_MILLIMETRES - ih) * 0.5;

  g.srcX0 = -ix / s;
  g.srcDX = pelMm / s;
  g.srcY0 = -iy / s;
  g.srcDY = lineMm / s;
  g.imgX0 = Math.max(0, ix / pelMm);
  g.imgX1 = Math.min(WIDTH, (ix + iw) / pelMm);
  g.imgY0 = Math.max(0, iy / lineMm);
  g.imgY1 = Math.min(g.lines, (iy + ih) / lineMm);
  return place(g, fit, inW, inH);
}

//===========================================================================
// Controls.cpp — option names and conversions.
//===========================================================================

export const optionIndex = (value, count) => clamp(lround(value), 0, count - 1);
export const CODING_NAMES = ['MH', 'MR'];
export const BAUD_NAMES = ['2400', '4800', '9600', '14400'];
export const BAUD_RATES = [2400, 4800, 9600, 14400];
export const MODE_PAGE = 0;
export const MODE_LIVE = 1;
export const MODE_NAMES = ['Page', 'Live'];
export const CONCEALMENT_NAMES = ['Off', 'Repeat Line'];
export const PAPER_THERMAL = 0;
export const PAPER_PLAIN = 1;
export const PAPER_NAMES = ['Thermal', 'Plain'];

export function bitErrorRate(value) {
  const v = clamp(value, 0, 1);
  if (v <= 0) return 0;
  return Math.pow(10, -6 + 4 * v);
}

export function burstBits(value) {
  const v = clamp(value, 0, 1);
  return 1 + lround(v * 63);
}

export const threshold = (value) => clamp(value, 0, 1);

/// The minimum transmission time of a coded scan line, 10 ms (stated in the plugin).
export const MIN_SCAN_LINE_SECONDS = 0.010;

export function paperColours(paper) {
  if (paper === PAPER_PLAIN) return { paper: [0.96, 0.96, 0.95], ink: [0.06, 0.06, 0.07] };
  return { paper: [0.86, 0.85, 0.80], ink: [0.20, 0.17, 0.16] };
}

export const DESK = [0, 0, 0];

/** The fill floor in bits: Fax::startPage's ceil( 0.010 x baud - 1e-9 ). */
export const minLineBitsFor = (baud) => Math.ceil(MIN_SCAN_LINE_SECONDS * baud - 1e-9);

//===========================================================================
// Header.cpp + Font.cpp — the sending machine's header line.
//===========================================================================

/// graticule's 5x7 font, by way of teletext: Font.cpp's kGlyphs, codes 32..127, written here by script.
const FONT_GLYPHS = [
  ['.....', '.....', '.....', '.....', '.....', '.....', '.....'], // 32
  ['..#..', '..#..', '..#..', '..#..', '.....', '.....', '..#..'], // 33
  ['.#.#.', '.#.#.', '.#.#.', '.....', '.....', '.....', '.....'], // 34
  ['.#.#.', '.#.#.', '#####', '.#.#.', '#####', '.#.#.', '.#.#.'], // 35
  ['..#..', '.####', '#.#..', '.###.', '..#.#', '####.', '..#..'], // 36
  ['##..#', '##..#', '...#.', '..#..', '.#...', '#..##', '#..##'], // 37
  ['.##..', '#..#.', '#.#..', '.#...', '#.#.#', '#..#.', '.##.#'], // 38
  ['..#..', '..#..', '.#...', '.....', '.....', '.....', '.....'], // 39
  ['...#.', '..#..', '.#...', '.#...', '.#...', '..#..', '...#.'], // 40
  ['.#...', '..#..', '...#.', '...#.', '...#.', '..#..', '.#...'], // 41
  ['.....', '..#..', '#.#.#', '.###.', '#.#.#', '..#..', '.....'], // 42
  ['.....', '..#..', '..#..', '#####', '..#..', '..#..', '.....'], // 43
  ['.....', '.....', '.....', '.....', '.##..', '..#..', '.#...'], // 44
  ['.....', '.....', '.....', '#####', '.....', '.....', '.....'], // 45
  ['.....', '.....', '.....', '.....', '.....', '.##..', '.##..'], // 46
  ['.....', '....#', '...#.', '..#..', '.#...', '#....', '.....'], // 47
  ['.###.', '#...#', '#..##', '#.#.#', '##..#', '#...#', '.###.'], // 48
  ['..#..', '.##..', '..#..', '..#..', '..#..', '..#..', '.###.'], // 49
  ['.###.', '#...#', '....#', '...#.', '..#..', '.#...', '#####'], // 50
  ['#####', '...#.', '..#..', '...#.', '....#', '#...#', '.###.'], // 51
  ['...#.', '..##.', '.#.#.', '#..#.', '#####', '...#.', '...#.'], // 52
  ['#####', '#....', '####.', '....#', '....#', '#...#', '.###.'], // 53
  ['..##.', '.#...', '#....', '####.', '#...#', '#...#', '.###.'], // 54
  ['#####', '....#', '...#.', '..#..', '.#...', '.#...', '.#...'], // 55
  ['.###.', '#...#', '#...#', '.###.', '#...#', '#...#', '.###.'], // 56
  ['.###.', '#...#', '#...#', '.####', '....#', '...#.', '.##..'], // 57
  ['.....', '.##..', '.##..', '.....', '.##..', '.##..', '.....'], // 58
  ['.....', '.##..', '.##..', '.....', '.##..', '..#..', '.#...'], // 59
  ['...#.', '..#..', '.#...', '#....', '.#...', '..#..', '...#.'], // 60
  ['.....', '.....', '#####', '.....', '#####', '.....', '.....'], // 61
  ['.#...', '..#..', '...#.', '....#', '...#.', '..#..', '.#...'], // 62
  ['.###.', '#...#', '....#', '...#.', '..#..', '.....', '..#..'], // 63
  ['.###.', '#...#', '....#', '.##.#', '#.#.#', '#.#.#', '.###.'], // 64
  ['.###.', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'], // 65
  ['####.', '#...#', '#...#', '####.', '#...#', '#...#', '####.'], // 66
  ['.###.', '#...#', '#....', '#....', '#....', '#...#', '.###.'], // 67
  ['###..', '#..#.', '#...#', '#...#', '#...#', '#..#.', '###..'], // 68
  ['#####', '#....', '#....', '####.', '#....', '#....', '#####'], // 69
  ['#####', '#....', '#....', '####.', '#....', '#....', '#....'], // 70
  ['.###.', '#...#', '#....', '#.###', '#...#', '#...#', '.####'], // 71
  ['#...#', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'], // 72
  ['.###.', '..#..', '..#..', '..#..', '..#..', '..#..', '.###.'], // 73
  ['..###', '...#.', '...#.', '...#.', '...#.', '#..#.', '.##..'], // 74
  ['#...#', '#..#.', '#.#..', '##...', '#.#..', '#..#.', '#...#'], // 75
  ['#....', '#....', '#....', '#....', '#....', '#....', '#####'], // 76
  ['#...#', '##.##', '#.#.#', '#.#.#', '#...#', '#...#', '#...#'], // 77
  ['#...#', '#...#', '##..#', '#.#.#', '#..##', '#...#', '#...#'], // 78
  ['.###.', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'], // 79
  ['####.', '#...#', '#...#', '####.', '#....', '#....', '#....'], // 80
  ['.###.', '#...#', '#...#', '#...#', '#.#.#', '#..#.', '.##.#'], // 81
  ['####.', '#...#', '#...#', '####.', '#.#..', '#..#.', '#...#'], // 82
  ['.####', '#....', '#....', '.###.', '....#', '....#', '####.'], // 83
  ['#####', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'], // 84
  ['#...#', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'], // 85
  ['#...#', '#...#', '#...#', '#...#', '#...#', '.#.#.', '..#..'], // 86
  ['#...#', '#...#', '#...#', '#.#.#', '#.#.#', '#.#.#', '.#.#.'], // 87
  ['#...#', '#...#', '.#.#.', '..#..', '.#.#.', '#...#', '#...#'], // 88
  ['#...#', '#...#', '#...#', '.#.#.', '..#..', '..#..', '..#..'], // 89
  ['#####', '....#', '...#.', '..#..', '.#...', '#....', '#####'], // 90
  ['.###.', '.#...', '.#...', '.#...', '.#...', '.#...', '.###.'], // 91
  ['.....', '#....', '.#...', '..#..', '...#.', '....#', '.....'], // 92
  ['.###.', '...#.', '...#.', '...#.', '...#.', '...#.', '.###.'], // 93
  ['..#..', '.#.#.', '#...#', '.....', '.....', '.....', '.....'], // 94
  ['.....', '.....', '.....', '.....', '.....', '.....', '#####'], // 95
  ['.#...', '..#..', '...#.', '.....', '.....', '.....', '.....'], // 96
  ['.....', '.....', '.###.', '....#', '.####', '#...#', '.####'], // 97
  ['#....', '#....', '#.##.', '##..#', '#...#', '#...#', '####.'], // 98
  ['.....', '.....', '.###.', '#....', '#....', '#...#', '.###.'], // 99
  ['....#', '....#', '.##.#', '#..##', '#...#', '#...#', '.####'], // 100
  ['.....', '.....', '.###.', '#...#', '#####', '#....', '.###.'], // 101
  ['..##.', '.#..#', '.#...', '###..', '.#...', '.#...', '.#...'], // 102
  ['.....', '.....', '.####', '#...#', '.####', '....#', '.###.'], // 103
  ['#....', '#....', '#.##.', '##..#', '#...#', '#...#', '#...#'], // 104
  ['..#..', '.....', '.##..', '..#..', '..#..', '..#..', '.###.'], // 105
  ['...#.', '.....', '..##.', '...#.', '...#.', '#..#.', '.##..'], // 106
  ['#....', '#....', '#..#.', '#.#..', '##...', '#.#..', '#..#.'], // 107
  ['.##..', '..#..', '..#..', '..#..', '..#..', '..#..', '.###.'], // 108
  ['.....', '.....', '##.#.', '#.#.#', '#.#.#', '#...#', '#...#'], // 109
  ['.....', '.....', '#.##.', '##..#', '#...#', '#...#', '#...#'], // 110
  ['.....', '.....', '.###.', '#...#', '#...#', '#...#', '.###.'], // 111
  ['.....', '.....', '####.', '#...#', '####.', '#....', '#....'], // 112
  ['.....', '.....', '.####', '#...#', '.####', '....#', '....#'], // 113
  ['.....', '.....', '#.##.', '##..#', '#....', '#....', '#....'], // 114
  ['.....', '.....', '.####', '#....', '.###.', '....#', '####.'], // 115
  ['.#...', '.#...', '###..', '.#...', '.#...', '.#..#', '..##.'], // 116
  ['.....', '.....', '#...#', '#...#', '#...#', '#..##', '.##.#'], // 117
  ['.....', '.....', '#...#', '#...#', '#...#', '.#.#.', '..#..'], // 118
  ['.....', '.....', '#...#', '#...#', '#.#.#', '#.#.#', '.#.#.'], // 119
  ['.....', '.....', '#...#', '.#.#.', '..#..', '.#.#.', '#...#'], // 120
  ['.....', '.....', '#...#', '#...#', '.####', '....#', '.###.'], // 121
  ['.....', '.....', '#####', '...#.', '..#..', '.#...', '#####'], // 122
  ['...#.', '..#..', '..#..', '.#...', '..#..', '..#..', '...#.'], // 123
  ['..#..', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'], // 124
  ['.#...', '..#..', '..#..', '...#.', '..#..', '..#..', '.#...'], // 125
  ['.....', '.#...', '#.#.#', '...#.', '.....', '.....', '.....'], // 126
  ['.....', '.....', '.....', '.....', '.....', '.....', '.....'], // 127
];

const FONT_FIRST = 32;
const FONT_COUNT = 96;
const FONT_WIDTH = 5;
const FONT_HEIGHT = 7;
const FONT_ADVANCE = FONT_WIDTH + 1;

function fontBit(code, x, y) {
  if (x < 0 || x >= FONT_WIDTH || y < 0 || y >= FONT_HEIGHT) return false;
  if (code < FONT_FIRST || code >= FONT_FIRST + FONT_COUNT) return false;
  return FONT_GLYPHS[code - FONT_FIRST][y][x] === '#';
}

const DOT_PELS = 4;
const LEFT_PELS = 64;
const TOP_DOTS = 1;
const BAND_DOTS = FONT_HEIGHT + 2;

const dotLines = (lpm) => Math.max(1, lround(0.5 * lpm));

export const headerBandLines = (lpm) => BAND_DOTS * dotLines(lpm);

const pad2 = (n) => String(n).padStart(2, '0');
const pad3 = (n) => String(n).padStart(3, '0');

/** The text for page `page` (1-based) sent `seconds` into the composition. */
export function headerText(page, seconds) {
  const t = Math.max(seconds, 0);
  const s = Math.floor(t + 1e-6);
  return `${pad2(Math.floor(s / 3600) % 24)}:${pad2(Math.floor(s / 60) % 60)}:${pad2(s % 60)}   FROM: SW FAX   G3 T.4          P.${pad3(((page - 1) % 999) + 1)}`;
}

/** Print `text` into the top of `page`, over a blank band. */
export function printHeader(page, lpm, text) {
  const sy = dotLines(lpm);
  const band = Math.min(page.lines, headerBandLines(lpm));
  page.bytes.fill(0, 0, band * LINE_BYTES);

  for (let i = 0; i < text.length; i += 1) {
    const x0 = LEFT_PELS + i * FONT_ADVANCE * DOT_PELS;
    if (x0 + FONT_WIDTH * DOT_PELS > WIDTH) break;
    const code = text.charCodeAt(i);
    for (let gy = 0; gy < FONT_HEIGHT; gy += 1) {
      for (let gx = 0; gx < FONT_WIDTH; gx += 1) {
        if (!fontBit(code, gx, gy)) continue;
        for (let ly = 0; ly < sy; ly += 1) {
          const y = (TOP_DOTS + gy) * sy + ly;
          if (y >= page.lines) continue;
          for (let lx = 0; lx < DOT_PELS; lx += 1) page.setPel(y, x0 + gx * DOT_PELS + lx, BLACK);
        }
      }
    }
  }
}
