#pragma once

#include "Codec.h"

#include <string>

/**
	The header the SENDING machine prints across the top of every page (the
	transmitting terminal identification a real terminal adds): the time, who
	it is from, and the page number. It is printed into the scanned page BEFORE
	coding, so it is coded, sent and broken on the line like the picture.

	The font is graticule's 5x7, each dot 4 pels wide and half a millimetre of
	lines tall (2 / 4 / 8 lines at standard / fine / superfine), on a blank band
	across the top of the page. The time is the composition's running time at
	the page's start, not the wall clock: two renders of one frame must match.
	No real sender's name or number appears.
*/
namespace fax::header
{

/// Lines the header band takes at this line density.
int BandLines( double linesPerMillimetre );

/// The text for page `page` (1-based) sent `seconds` into the composition.
std::string Text( int page, double seconds );

/// Print `text` into the top of `page`.
void Print( codec::Page& page, double linesPerMillimetre, const std::string& text );

} // namespace fax::header
