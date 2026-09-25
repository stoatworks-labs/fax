#pragma once

#include <string>

/**
	The two passes.

	1. **scan** -- 54 x Lines, RGBA8. One fragment per 32 pels of a scan line:
	   for each pel, the luma (BT.709 on the encoded values, composited over
	   white paper by the source's alpha) averaged over the source pixels whose
	   centres fall in the pel's rectangle, or the pixel under its centre when
	   none do; then black below `Threshold`, or below it plus an 8 x 8 Bayer
	   offset in Halftone. Eight pels a channel, pel 8i + b in bit b -- the
	   page's own byte layout (`Codec.h`) -- written as byte / 255, which unorm
	   conversion stores exactly. Read back to the CPU, where the coder, the
	   line and the decoder run.

	2. **print** -- the host's framebuffer. The received page as the output
	   sees it: for each output pixel, the exact area of its footprint that
	   black pels cover (a box filter over pels and lines, not a sample), then
	   paper, toner and the desk mixed by area. Opaque: alpha 1, and Mix blends
	   the whole RGBA with the source.

	`fxtest --dump-shaders DIR` writes exactly these strings, which is what
	`tools/verify.sh` hands to glslc.
*/
namespace fax::shaders
{

std::string Vertex();
std::string Scan();
std::string Print();

} // namespace fax::shaders
