/**
 * Fax — browser demo.
 *
 * The picture sent as a Group 3 fax (ITU-T T.4) over a noisy telephone line.
 * The one idea, from `source/Fax.h`: a fax does not send pixels. It thresholds
 * each scan line to black and white and sends RUN LENGTHS as variable-length
 * codes, a line at a time, each ended by an EOL; in MR most lines are sent as
 * the differences from the line above. So the clip goes through a real coder,
 * a real noisy line and a real decoder, and the streaks, the wedges, the
 * repeated lines and the page that arrives a line at a time fall out of the code.
 *
 * Like teletext, this plugin is **not only a shader**, and the two halves of
 * the page are not equally faithful:
 *
 *   The shaders are the plugin's. `VERTEX_BODY`, `SCAN_BODY` and `PRINT_BODY`
 *   below are `kVertexBody`, `kScanBody` and `kPrintBody` from
 *   `source/Shaders.cpp`, spliced in by `demo/tools/splice_shaders.py` and never
 *   edited here, assembled with the same `#version 410 core` line the plugin's
 *   `shaders::assemble` prepends. `demo/tools/check_shaders.py` compares them
 *   character for character and `tools/verify.sh` runs it.
 *
 *   The CPU half is a PORT, in `fax.js` — of T4.h, Codec.cpp, Line.cpp,
 *   Header.cpp, Font.cpp, Layout.cpp and Controls.cpp — and the frame sequence
 *   below is a port of `Fax::ProcessOpenGL` and `Fax::startPage`. Only a reader
 *   checks the frame sequence. `demo/tools/check_port.sh` (in verify.sh)
 *   compares fax.js with the plugin's own C++ source on twelve pages, bit for
 *   bit; see that file for exactly what it covers and what it does not.
 *
 * ------------------------------------------------------ the read-back
 *
 * The plugin scans each page on the GPU to 54 x Lines RGBA8 (eight pels a
 * channel), reads it back with glGetTexImage, codes / corrupts / decodes it on
 * the CPU and uploads the received page for the print pass. The page does
 * exactly that: one `readPixels` of the RGBA8 scan target as bytes, the port in
 * JavaScript, and a texSubImage2D of the lines that have arrived.
 *
 * ------------------------------------------------------- the clock
 *
 * The page's clock is the kit's `time`: seconds since the page started, paused
 * by Pause, stepped by Step, capped at 0.1 s a frame. The unit vote the plugin
 * runs against Resolume's millisecond clock never runs here, because the page
 * declares seconds, as the harness does. Restart sends the clock to 0, which
 * starts a new page, as a scrub does in the plugin.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';
import * as fax from './fax.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here: re-run
// `python3 demo/tools/splice_shaders.py`. The plugin assembles each as
// kVersion + body; so does `assemble` below.
//---------------------------------------------------------------------------

const VERSION = '#version 410 core\n';

const VERTEX_BODY = `
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
`;

const SCAN_BODY = `
uniform sampler2D InputTexture;
uniform ivec2 InputSize;   //the picture's size in texels
uniform vec2 SrcOrigin;    //source pixel (y down) at the top-left of pel ( 0, 0 )
uniform vec2 SrcStep;      //source pixels a pel spans, and a line
uniform vec4 ImageRect;    //the picture on the page: x0, x1 (pels), y0, y1 (lines)
uniform float Threshold;
uniform int Halftone;
uniform int Perturb;

in vec2 uv;
out vec4 fragColor;

//The 8 x 8 ordered-dither (Bayer) index matrix, row by row.
const int Bayer[ 64 ] = int[ 64 ](
	 0, 32,  8, 40,  2, 34, 10, 42,
	48, 16, 56, 24, 50, 18, 58, 26,
	12, 44,  4, 36, 14, 46,  6, 38,
	60, 28, 52, 20, 62, 30, 54, 22,
	 3, 35, 11, 43,  1, 33,  9, 41,
	51, 19, 59, 27, 49, 17, 57, 25,
	15, 47,  7, 39, 13, 45,  5, 37,
	63, 31, 55, 23, 61, 29, 53, 21 );

//Luma of source pixel ( x, y ), y down, over white paper by its alpha.
float lumaAt( int x, int y )
{
	vec4 t = texelFetch( InputTexture, ivec2( x, InputSize.y - 1 - y ), 0 );
	vec3 w = ( Perturb & 128 ) != 0 ? vec3( 0.299, 0.587, 0.114 ) : vec3( 0.2126, 0.7152, 0.0722 );
	return dot( t.rgb, w ) * t.a + ( 1.0 - t.a );
}

int pelBit( int pel, int line )
{
	float cx = float( pel ) + 0.5;
	float cy = float( line ) + 0.5;
	if( cx < ImageRect.x || cx >= ImageRect.y || cy < ImageRect.z || cy >= ImageRect.w )
		return 0;

	//The pel's rectangle in source pixels; the pixels whose centres fall in
	//it: p + 0.5 in [ a, b ) is p in [ ceil( a - 0.5 ), ceil( b - 0.5 ) ).
	float x0 = SrcOrigin.x + float( pel ) * SrcStep.x;
	float x1 = SrcOrigin.x + float( pel + 1 ) * SrcStep.x;
	float y0 = SrcOrigin.y + float( line ) * SrcStep.y;
	float y1 = SrcOrigin.y + float( line + 1 ) * SrcStep.y;
	int px0 = clamp( int( ceil( x0 - 0.5 ) ), 0, InputSize.x );
	int px1 = clamp( int( ceil( x1 - 0.5 ) ), 0, InputSize.x );
	int py0 = clamp( int( ceil( y0 - 0.5 ) ), 0, InputSize.y );
	int py1 = clamp( int( ceil( y1 - 0.5 ) ), 0, InputSize.y );

	float v;
	if( px1 > px0 && py1 > py0 )
	{
		//Past eight a side the taps are strided.
		int sx = max( 1, ( px1 - px0 + 7 ) / 8 );
		int sy = max( 1, ( py1 - py0 + 7 ) / 8 );
		float sum = 0.0;
		float n   = 0.0;
		for( int y = py0; y < py1; y += sy )
			for( int x = px0; x < px1; x += sx )
			{
				sum += lumaAt( x, y );
				n += 1.0;
			}
		v = sum / n;
	}
	else
	{
		//A pel smaller than a pixel: the pixel under its centre.
		int px = clamp( int( floor( SrcOrigin.x + cx * SrcStep.x ) ), 0, InputSize.x - 1 );
		int py = clamp( int( floor( SrcOrigin.y + cy * SrcStep.y ) ), 0, InputSize.y - 1 );
		v = lumaAt( px, py );
	}

	float t = Threshold;
	if( Halftone != 0 )
		t += ( float( Bayer[ ( line & 7 ) * 8 + ( pel & 7 ) ] ) + 0.5 ) / 64.0 - 0.5;
	return v < t ? 1 : 0;
}

void main()
{
	int group = int( floor( gl_FragCoord.x ) );
	int line  = int( floor( gl_FragCoord.y ) );
	vec4 bytes = vec4( 0.0 );
	for( int c = 0; c < 4; ++c )
	{
		int b = 0;
		for( int bit = 0; bit < 8; ++bit )
			b |= pelBit( group * 32 + c * 8 + bit, line ) << bit;
		bytes[ c ] = float( b ) / 255.0;
	}
	fragColor = bytes;
}
`;

const PRINT_BODY = `
uniform sampler2D PageBits;
uniform sampler2D Source;
uniform vec2 MaxUV;
uniform ivec2 OutSize;
uniform vec4 Sheet;        //the sheet on the output: x, y, w, h in pixels, y down
uniform int Lines;
uniform vec3 PaperColour;
uniform vec3 InkColour;
uniform vec3 DeskColour;
uniform float MixAmount;
uniform int Perturb;

in vec2 uv;
out vec4 fragColor;

int pelAt( int x, int line )
{
	vec4 t   = texelFetch( PageBits, ivec2( x >> 5, line ), 0 );
	int c    = ( x >> 3 ) & 3;
	float ch = c == 0 ? t.r : ( c == 1 ? t.g : ( c == 2 ? t.b : t.a ) );
	int byte = int( ch * 255.0 + 0.5 );
	return ( byte >> ( x & 7 ) ) & 1;
}

void main()
{
	vec4 source = texture( Source, uv * MaxUV );

	//This pixel's footprint, top-down, in pels and lines.
	vec2 p   = vec2( floor( gl_FragCoord.x ), float( OutSize.y ) - 1.0 - floor( gl_FragCoord.y ) );
	float kx = 1728.0 / Sheet.z;
	float ky = float( Lines ) / Sheet.w;
	float u0 = ( p.x - Sheet.x ) * kx;
	float u1 = ( p.x + 1.0 - Sheet.x ) * kx;
	float v0 = ( p.y - Sheet.y ) * ky;
	float v1 = ( p.y + 1.0 - Sheet.y ) * ky;
	float area = ( u1 - u0 ) * ( v1 - v0 );

	float cu0 = clamp( u0, 0.0, 1728.0 );
	float cu1 = clamp( u1, 0.0, 1728.0 );
	float cv0 = clamp( v0, 0.0, float( Lines ) );
	float cv1 = clamp( v1, 0.0, float( Lines ) );
	float inPage = max( cu1 - cu0, 0.0 ) * max( cv1 - cv0, 0.0 ) / area;

	float black = 0.0;
	if( inPage > 0.0 )
	{
		if( ( Perturb & 256 ) != 0 )
		{
			int x = clamp( int( floor( 0.5 * ( cu0 + cu1 ) ) ), 0, 1727 );
			int y = clamp( int( floor( 0.5 * ( cv0 + cv1 ) ) ), 0, Lines - 1 );
			black = float( pelAt( x, y ) ) * inPage;
		}
		else
		{
			//The exact area of the footprint each pel covers.
			int i0 = int( floor( cu0 ) );
			int i1 = min( int( ceil( cu1 ) ), 1728 );
			int j0 = int( floor( cv0 ) );
			int j1 = min( int( ceil( cv1 ) ), Lines );
			for( int j = j0; j < j1; ++j )
			{
				float wy = min( cv1, float( j + 1 ) ) - max( cv0, float( j ) );
				float row = 0.0;
				for( int i = i0; i < i1; ++i )
				{
					if( pelAt( i, j ) == 0 )
						continue;
					row += min( cu1, float( i + 1 ) ) - max( cu0, float( i ) );
				}
				black += row * wy;
			}
			black /= area;
		}
	}

	vec3 shade = DeskColour * ( 1.0 - inPage ) + PaperColour * ( inPage - black ) + InkColour * black;
	fragColor = mix( source, vec4( shade, 1.0 ), MixAmount );
}
`;

const assemble = (body) => VERSION + body;

//===========================================================================
// The renderer: Fax::ProcessOpenGL, in its order.
//
//   1. scan       54 x Lines, RGBA8: every pel thresholded (or dithered), packed
//   2. read-back  216 bytes a line, readPixels as bytes
//   3. sender     the header printed into the page, then T.4 (MH or MR)
//   4. line       PCG-hash bit errors and bursts; the page's timing
//   5. receiver   EOL segmentation, bad lines, concealment
//   6. arrival    Page mode: the lines whose EOL has arrived, over the last page
//   7. print      the received page's pel coverage at every output pixel
//===========================================================================

const GROUPS = fax.WIDTH / 32; // 54 texels a scan line

/// What the line under the canvas reports. Filled by the renderer.
const telemetry = {
  page: 0, mode: 1, lines: 0, arrived: 0, bits: 0, flips: 0, bad: 0, concealed: 0,
  duration: 0, readback: 0, code: 0, channel: 0, decode: 0, frames: [],
};

function createRenderer(gl, quad) {
  const scanShader = new Program(gl, assemble(VERTEX_BODY), assemble(SCAN_BODY), 'scan');
  const printShader = new Program(gl, assemble(VERTEX_BODY), assemble(PRINT_BODY), 'print');
  const scanBuffer = new PassBuffer(gl, { filter: 'nearest' });

  const pageTexture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, pageTexture);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);
  let pageTextureLines = 0;

  // The page in flight: Fax.h's members, same names.
  let geometry = null;
  const scanned = new fax.Page();
  let transmission = null;
  let decoded = null;
  const display = new fax.Page();
  let timing = { arrival: new Float64Array(0), duration: 0 };
  let pageActive = false;
  let pageStart = 0;
  let pageIndex = 0;
  let shownLines = 0;
  let pageFit = -1;
  let pageResolution = -1;
  let pageMode = -1;

  const uploadDisplay = (fromLine, toLine) => {
    gl.bindTexture(gl.TEXTURE_2D, pageTexture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    if (pageTextureLines !== display.lines) {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, GROUPS, display.lines, 0, gl.RGBA, gl.UNSIGNED_BYTE, display.bytes);
      pageTextureLines = display.lines;
    } else if (toLine > fromLine) {
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, fromLine, GROUPS, toLine - fromLine, gl.RGBA, gl.UNSIGNED_BYTE,
        display.bytes.subarray(fromLine * fax.LINE_BYTES, toLine * fax.LINE_BYTES));
    }
    gl.bindTexture(gl.TEXTURE_2D, null);
  };

  // Fax::startPage: scan, code, send and decode a new page.
  const startPage = (input, p, now) => {
    const width = input.width;
    const height = input.height;
    const fit = fax.optionIndex(p('fit'), 3);
    const resolution = fax.optionIndex(p('resolution'), 3);
    const coding = fax.optionIndex(p('coding'), 2);
    const baud = fax.BAUD_RATES[fax.optionIndex(p('baud'), 4)];
    const conceal = fax.optionIndex(p('concealment'), 2);
    const lpm = fax.linesPerMillimetre(resolution);

    geometry = fax.computeLayout(fit, resolution, width, height);
    const lines = geometry.lines;

    //------------------------------------------------------------------
    // 1. Scan.
    //------------------------------------------------------------------
    scanBuffer.ensure(GROUPS, lines, gl.RGBA8);
    scanBuffer.bind();
    gl.disable(gl.BLEND);
    scanShader.use();
    bindTexture(gl, 0, input.texture);
    scanShader.setSampler('InputTexture', 0);
    gl.uniform2i(scanShader.location('InputSize'), width, height);
    scanShader.set('SrcOrigin', geometry.srcX0, geometry.srcY0);
    scanShader.set('SrcStep', geometry.srcDX, geometry.srcDY);
    scanShader.set('ImageRect', geometry.imgX0, geometry.imgX1, geometry.imgY0, geometry.imgY1);
    gl.uniform1f(scanShader.location('Threshold'), fax.threshold(p('threshold')));
    scanShader.setInt('Halftone', p('halftone') > 0.5 ? 1 : 0);
    scanShader.setInt('Perturb', 0);
    quad.draw();

    // Read it back: 216 bytes a line. The one stall, as in the plugin.
    let t0 = performance.now();
    if (scanned.lines !== lines) scanned.resize(lines);
    gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
    gl.readPixels(0, 0, GROUPS, lines, gl.RGBA, gl.UNSIGNED_BYTE, scanned.bytes);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    telemetry.readback = performance.now() - t0;

    //------------------------------------------------------------------
    // 2. The sending machine: its header, then T.4.
    //------------------------------------------------------------------
    t0 = performance.now();
    if (p('header') > 0.5) fax.printHeader(scanned, lpm, fax.headerText(pageIndex + 1, now));
    transmission = fax.encode(scanned, { coding, k: fax.kOf(resolution), minLineBits: fax.minLineBitsFor(baud) }, transmission);
    telemetry.bits = transmission.bits.length;
    telemetry.code = performance.now() - t0;

    //------------------------------------------------------------------
    // 3. The line.
    //------------------------------------------------------------------
    t0 = performance.now();
    const bursts = p('bursts');
    telemetry.flips = fax.corrupt(transmission, {
      ber: fax.bitErrorRate(p('lineNoise')),
      burstBits: bursts > 0 ? fax.burstBits(bursts) : 1,
      seed: fax.hash((Math.imul(pageIndex, 2654435761 | 0) + 12345) >>> 0),
    });
    timing = fax.schedule(transmission, baud);
    telemetry.channel = performance.now() - t0;

    //------------------------------------------------------------------
    // 4. The receiving machine.
    //------------------------------------------------------------------
    t0 = performance.now();
    decoded = fax.decode(transmission.bits.data, transmission.bits.length, lines, { coding, concealment: conceal }, decoded);
    telemetry.decode = performance.now() - t0;

    // The paper under the arriving page is the last page -- blank on the
    // first, and blank again when the page changes shape.
    if (display.lines !== lines) {
      display.resize(lines);
      uploadDisplay(0, lines);
    }

    pageStart = now;
    pageFit = fit;
    pageResolution = resolution;
    pageActive = true;
    shownLines = 0;
    pageIndex += 1;

    let bad = 0;
    let concealed = 0;
    for (let i = 0; i < lines; i += 1) {
      bad += decoded.bad[i];
      if (decoded.shown[i] === fax.SHOWN_CONCEALED) concealed += 1;
    }
    telemetry.page = pageIndex;
    telemetry.lines = lines;
    telemetry.bad = bad;
    telemetry.concealed = concealed;
    telemetry.duration = timing.duration;
  };

  return {
    render({ input, params, width, height, time }) {
      // The host stores every parameter as a float; so does this.
      const p = (id) => Math.fround(params.get(id));
      const now = time;

      const mode = fax.optionIndex(p('mode'), 2);
      const fit = fax.optionIndex(p('fit'), 3);
      const resolution = fax.optionIndex(p('resolution'), 3);

      //------------------------------------------------------------------
      // A new page: every frame in Live; in Page when the last one has
      // ended, when the page would change shape, when the mode changes, or
      // when the clock has gone backwards past the page's start.
      //------------------------------------------------------------------
      const newPage = !pageActive || mode === fax.MODE_LIVE || mode !== pageMode || fit !== pageFit
        || resolution !== pageResolution || now < pageStart || now - pageStart >= timing.duration;
      pageMode = mode;
      if (newPage) startPage(input, p, now);

      //------------------------------------------------------------------
      // The lines that have arrived, over the last page.
      //------------------------------------------------------------------
      const arrived = mode === fax.MODE_LIVE ? geometry.lines : fax.arrived(timing, now - pageStart);
      if (arrived > shownLines) {
        display.bytes.set(decoded.page.bytes.subarray(shownLines * fax.LINE_BYTES, arrived * fax.LINE_BYTES), shownLines * fax.LINE_BYTES);
        uploadDisplay(shownLines, arrived);
        shownLines = arrived;
      }
      telemetry.mode = mode;
      telemetry.arrived = shownLines;

      //------------------------------------------------------------------
      // Print, onto the canvas. The host's viewport is the whole canvas.
      //------------------------------------------------------------------
      const shown = fax.place({ ...geometry }, pageFit, width, height);
      const colours = fax.paperColours(fax.optionIndex(p('paper'), 2));

      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      printShader.use();
      bindTexture(gl, 0, pageTexture);
      bindTexture(gl, 1, input.texture);
      printShader.setSampler('PageBits', 0);
      printShader.setSampler('Source', 1);
      printShader.set('MaxUV', 1.0, 1.0);
      gl.uniform2i(printShader.location('OutSize'), width, height);
      printShader.set('Sheet', shown.sheetX, shown.sheetY, shown.sheetW, shown.sheetH);
      printShader.setInt('Lines', display.lines);
      printShader.set('PaperColour', colours.paper[0], colours.paper[1], colours.paper[2]);
      printShader.set('InkColour', colours.ink[0], colours.ink[1], colours.ink[2]);
      printShader.set('DeskColour', fax.DESK[0], fax.DESK[1], fax.DESK[2]);
      gl.uniform1f(printShader.location('MixAmount'), Math.min(Math.max(p('mix'), 0), 1));
      printShader.setInt('Perturb', 0);
      quad.draw();

      bindTexture(gl, 1, null);
      bindTexture(gl, 0, null);
      gl.activeTexture(gl.TEXTURE0);

      const stamp = performance.now();
      telemetry.frames.push(stamp);
      while (telemetry.frames.length > 0 && stamp - telemetry.frames[0] > 2000) telemetry.frames.shift();
    },
  };
}

//===========================================================================
// The controls, read out of Fax::Fax(). Same names, same groups, same order,
// same defaults, same dropdown elements. Absent: the About block.
//===========================================================================

const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });

const demo = mountDemo({
  name: 'Fax',
  pluginId: 'FX01',
  kind: 'effect',
  tagline:
    'The picture sent as a Group 3 fax over a noisy telephone line. Each frame is scanned to 1728 pels a line, thresholded or dithered to black and white, coded with the real ITU-T T.4 run-length codes (MH, or MR against the line above), sent through a line that flips bits, and decoded by a real decoder. A bit error streaks the rest of its line, MR carries it down to the next one-dimensional line, a bad line is repeated from the one above, and in Page mode a busy page takes longer to arrive than a blank one.',
  repo: 'https://github.com/stoatworks-labs/fax',
  page: 'https://stoatworks-labs.com/software/fax/',

  blurb:
    'It is Fax’s own scan and print GLSL, ported from the repository to WebGL2, with the CPU half — the sender’s header, the T.4 MH/MR coder, the noisy line, the decoder and the page timing — ported to JavaScript by hand. On this page that port is checked by nobody but a reader; the repository compares it with the plugin’s C++ source on twelve test pages. It runs on generated clips in this page, with the plugin’s own parameters and no install.',

  params: [
    opt('fit', 'Fit', fax.FIT_NAMES, 0, 'Scan',
      'Frame: the page is the frame, 1728 pels across its width. A4 Crop and A4 Letterbox: an A4 sheet (1143 / 2287 / 4574 lines), the frame covering or fitting it, shown whole on a black desk.'),
    opt('resolution', 'Resolution', fax.RESOLUTION_NAMES, 0, 'Scan',
      '3.85, 7.7 or 15.4 lines per mm (T.4 2.2): a standard fax is squashed and doubled vertically. In MR it also sets K, one line in K coded one-dimensionally: 2, 4, 8. Superfine is four times the bits of Standard.'),
    std('threshold', 'Threshold', 0.5, 'Scan', {
      display: (v) => fax.threshold(v).toFixed(2),
      hint: 'The luma (BT.709, over white paper by alpha) below which a pel prints black. With Halftone, the centre of the 8 × 8 Bayer dither.',
    }),
    bool('halftone', 'Halftone', 1, 'Scan',
      'An 8 × 8 ordered dither on the threshold, so tones survive as dots. A halftone page is about 2.2 million bits against about 90,000 thresholded, and costs that much more to code, send and decode.'),

    opt('coding', 'Coding', fax.CODING_NAMES, 1, 'Line',
      'MH: every line one-dimensional, run lengths only. MR: one line in K one-dimensional, the rest coded against the line above — so an error runs down the page until the next one-dimensional line.'),
    std('lineNoise', 'Line Noise', 0.4, 'Line', {
      display: (v) => {
        const ber = fax.bitErrorRate(Math.fround(v));
        return ber === 0 ? 'clean line' : `BER ${ber.toExponential(1)}`;
      },
      hint: '0 is a clean line; above it the bit error rate is 10^(−6 + 4v), 1e-6 to 1e-2. The default is 4e-5: about ninety errors on a standard halftone page, three on a thresholded one. A stated curve, not a measured modem.',
    }),
    std('bursts', 'Bursts', 0.0, 'Line', {
      display: (v) => {
        const f = Math.fround(v);
        return f > 0 ? `bursts of up to ${fax.burstBits(f)} bits` : 'single-bit errors';
      },
      hint: '0: every error is one bit. Above it each error event flips its first bit and each of the next 1 + round(63v) − 1 with probability one half, like a click on the line.',
    }),
    opt('baud', 'Baud', fax.BAUD_NAMES, 3, 'Line',
      'The line rate (V.27 ter, V.29, V.17). Only Page mode shows it: a line takes its bits over the rate, floored at the 10 ms minimum scan-line time with fill.'),
    opt('mode', 'Mode', fax.MODE_NAMES, 1, 'Line',
      'Live: every frame a whole new page. Page: a page is captured and arrives line by line at the line rate over the last one — 4.7 s for a blank standard page at 14400, about two and a half minutes for a halftone one.'),

    opt('concealment', 'Concealment', fax.CONCEALMENT_NAMES, 1, 'Receiver',
      'What the receiver does with a line it can tell is bad (an invalid code, not 1728 pels, anything but fill after it). Repeat Line: the line above, and in MR every 2-D line after it until a 1-D one. Off: what was decoded, the colour at the failure to the right edge.'),
    opt('paper', 'Paper', fax.PAPER_NAMES, 0, 'Receiver',
      'Thermal: a warm grey sheet and brown-black image. Plain: white paper and toner. Colours chosen by eye in the plugin.'),
    bool('header', 'Header', 1, 'Receiver',
      'The sending machine’s header, printed into the top of the page BEFORE coding, so it breaks on the line with the rest: the time (the page’s clock, not the wall clock), FROM: SW FAX, G3 T.4 and the page number.'),
    std('mix', 'Mix', 1.0, 'Receiver'),
  ],

  // A moving picture with a full range for the halftone; the ramps show the
  // dither's levels; the grid shows the squash; bars are what a threshold does.
  sources: ['scene', 'ramp', 'grid', 'bars', 'detail', 'spot'],

  // The plugin ships no factory presets. These are the page's own, expressed
  // entirely in the plugin's parameters and reachable with the controls.
  presets: {
    'Page mode: watch it arrive (thresholded)': { mode: 0, halftone: 0 },
    'Streaks (MH, noisy)': { coding: 0, lineNoise: 0.7 },
    'Wedges (MR, no concealment)': { concealment: 0, lineNoise: 0.65 },
    'Clicks on the line': { bursts: 0.5, lineNoise: 0.55 },
    'Clean line': { lineNoise: 0 },
    'Plain paper, threshold': { paper: 1, halftone: 0 },
    'A4 on the desk': { fit: 2 },
    'Superfine (slow here)': { resolution: 2 },
  },

  differences: [
    'The CPU half of this plugin is a PORT, not the plugin’s own code. Fax reads each scanned page back to the CPU, prints the sender’s header into it, codes it with T.4 (MH or MR, with fill, EOLs, tags and RTC), flips bits with a PCG hash per bit, decodes it with EOL segmentation, bad-line detection and Repeat Line concealment, and times each line’s arrival — T4.h, Codec.cpp, Line.cpp, Header.cpp, Font.cpp, Layout.cpp, Controls.cpp and the frame sequence in Fax::ProcessOpenGL. All of that is ported here by hand. On this page nothing checks the port but a reader.',
    'What the repository does check: demo/tools/check_port.sh compiles the plugin’s own Codec, Line, Header, Font, Layout and Controls source unchanged and compares the port with it on twelve test pages — the scanned card (thresholded and halftone) and a synthetic page, across both codings, all three resolutions, all four baud rates, noise, bursts and both concealments — bit for bit in the coded and the corrupted stream, exactly in every arrival time, the decoded page and the bad-line flags; plus the code tables, the font, the layout, the header text and the conversions. The bit error rate itself may differ from the plugin’s by one unit in the last place (JavaScript’s Math.pow against C++’s std::pow); the integer cutoff the line uses is identical on all 101 values tried. It does not check the frame sequence on this page, which only a reader has compared with Fax.cpp, and it covers those pages and no others.',
    'The GPU half is not a port. The scan pass and the print pass are the plugin’s own GLSL, and demo/tools/check_shaders.py fails the repository’s verify script if a character of either shader (or the shared vertex shader) drifts. port() in the kit adds ES 3.00 precision qualifiers and nothing else.',
    'Speed: the plugin’s CPU half costs about 9 ms a frame at its defaults (Live, halftone, about 2.2 million bits a page) at 1080p on an M4 Max, and Live superfine halftone (about 9 million bits) is not real-time even there, about 31 ms. Here the same work runs in JavaScript on the page’s main thread and is slower; the line under the canvas shows what it costs in your browser. Nothing is skipped, thinned or lowered to keep up — every frame codes, corrupts and decodes the whole page — so the frame rate drops instead. The page is always 1728 pels wide, so the canvas size hardly changes that cost.',
    'The page’s clock steps at most 0.1 s a frame (the kit’s rule, so a tab in the background does not jump). If this browser runs the codec slower than ten frames a second, the clock, and with it Page-mode arrival, runs slower than real time.',
    'The page’s clock is declared as seconds, as the repository’s harness declares it. The plugin votes on Resolume’s clock unit over its first frames; that vote never runs here. Restart sends the clock to zero, which starts a new page, as a scrub does in the plugin. The header’s time is this clock.',
    'The input is one of the kit’s clips, generated by a shader in this page (a synthetic scene, ramps, a geometry card, colour bars, a detail sweep, lights on black), or your own image or video, decoded in the page. The plugin’s defaults (Halftone on, Threshold 0.5) were chosen on Resolume’s bundled clips, which are darker than these.',
    'Your own image or video is uploaded with premultiplied alpha, and the scan composites over white paper by straight alpha, so the partly transparent edges of a picture with alpha may scan a pel or two differently from the plugin. The kit’s clips are opaque.',
    'The plugin’s Perturb test hooks (the scan and print shaders’ Perturb uniform is 0, as in the plugin), its negative-control decoder and its forced-error hook are not ported: none of them is part of what the plugin does in a host. The About block is absent, as on every page in this suite. Fax has no audio path, so there is no audio caveat.',
    'The plugin’s proof — the tables against two independent transcriptions, an independent Go decoder, the streak, wedge, concealment and timing checks and their negative controls — is an offline harness in the repository. Nothing on this page measures anything; the line under the canvas reports what the ported chain did.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The line under the canvas: the ported chain's own numbers. Skipped in embed
// mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    line.id = 'fax-status';
    stage.append(line);
    const fmt = (n) => n.toLocaleString('en-GB');
    setInterval(() => {
      const t = telemetry;
      if (t.page === 0) return;
      const frames = t.frames.length > 1 ? ((t.frames.length - 1) * 1000) / (t.frames[t.frames.length - 1] - t.frames[0]) : 0;
      const cpu = t.readback + t.code + t.channel + t.decode;
      const arrival = t.mode === fax.MODE_PAGE
        ? `${fmt(t.arrived)} of ${fmt(t.lines)} lines arrived; the page takes ${t.duration.toFixed(1)} s. `
        : `${fmt(t.lines)} lines, a new page every frame. `;
      line.textContent =
        `Page ${fmt(t.page)}: ${fmt(t.bits)} bits sent, ${fmt(t.flips)} flipped, ${fmt(t.bad)} bad lines, ${fmt(t.concealed)} concealed. `
        + arrival
        + `CPU half ${cpu.toFixed(1)} ms a page in this browser (read-back ${t.readback.toFixed(1)}, sender ${t.code.toFixed(1)}, line ${t.channel.toFixed(1)}, receiver ${t.decode.toFixed(1)}); `
        + `${frames.toFixed(0)} frames a second.`;
    }, 250);
  }
}
