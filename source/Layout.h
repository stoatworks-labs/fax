#pragma once

/**
	Where the page is: how the source lands on the scanned page, and how the
	received page lands on the output.

	A T.4 scan line is 1728 pels across 215 mm (2.1/T.4). The line density is
	the Resolution: 3.85 lines/mm standard, 7.7 fine, 15.4 superfine (2.2/T.4),
	so a pel is 0.124 mm wide and a standard line 0.26 mm tall -- a standard fax
	is squashed and doubled vertically, and that is not a style choice here.

	`Fit`:
	  Frame         the page IS the frame: 1728 pels across its width and as many
	                lines as its height takes at the Resolution (466 standard lines
	                for 16:9). The received page fills the output. The default.
	  A4 Crop       an A4 sheet (215 x 297 mm, 1143 / 2287 / 4574 lines), the frame
	                scaled to COVER it and centred; the sheet is shown whole, centred
	                on the output, on a black desk.
	  A4 Letterbox  the same sheet, the frame scaled to FIT across it, blank paper
	                above and below.

	Everything here is double; the shaders receive it as float, and the harness
	recomputes it in double to judge them.
*/
namespace fax::layout
{

enum Fit : int
{
	kFitFrame = 0,
	kFitA4Crop,
	kFitA4Letterbox,
	kFitCount
};
const char* FitName( int index );

enum Resolution : int
{
	kStandard = 0,
	kFine,
	kSuperfine,
	kResolutionCount
};
const char* ResolutionName( int index );

/// Lines per millimetre (2.2/T.4).
double LinesPerMillimetre( int resolution );

/// The MR parameter K: one line in K is one-dimensional. 4.2.1.1/T.4 (07/2003)
/// gives 2 at standard, 4 at 200 and 8 at 400 lines/25.4 mm, and the note to
/// its Table 2 treats 7.7 and 15.4 lines/mm as equivalent to 200 and 400
/// (see AGENTS.md); `--wedge` measures whatever K this returns.
int K( int resolution );

struct Geometry
{
	int lines = 0;

	/// Source pixel (x right, y DOWN) of the top-left corner of pel ( 0, 0 ),
	/// and the source pixels a pel and a line span.
	double srcX0 = 0.0, srcDX = 1.0;
	double srcY0 = 0.0, srcDY = 1.0;

	/// Where the picture is on the page, in pels and lines (fractional). A pel
	/// whose centre is outside is blank paper.
	double imgX0 = 0.0, imgX1 = 0.0, imgY0 = 0.0, imgY1 = 0.0;

	/// Where the sheet is on the output, pixels, y DOWN.
	double sheetX = 0.0, sheetY = 0.0, sheetW = 0.0, sheetH = 0.0;
};

/// The page for a `fit` and `resolution`, a source of `inW` x `inH` pixels.
/// The sheet is placed on an output of the same size; `Place` re-places it.
Geometry Compute( int fit, int resolution, int inW, int inH );

/// Put an existing page's sheet on an output of `outW` x `outH`.
void Place( Geometry& g, int fit, int outW, int outH );

} // namespace fax::layout
