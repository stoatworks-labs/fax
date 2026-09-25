#include "Layout.h"

#include "T4.h"

#include <algorithm>
#include <cmath>

namespace fax::layout
{

const char* FitName( int index )
{
	static const char* const names[ kFitCount ] = { "Frame", "A4 Crop", "A4 Letterbox" };
	return names[ std::clamp( index, 0, kFitCount - 1 ) ];
}

const char* ResolutionName( int index )
{
	static const char* const names[ kResolutionCount ] = { "Standard", "Fine", "Superfine" };
	return names[ std::clamp( index, 0, kResolutionCount - 1 ) ];
}

double LinesPerMillimetre( int resolution )
{
	switch( resolution )
	{
	case kFine: return 7.7;
	case kSuperfine: return 15.4;
	default: return 3.85;
	}
}

int K( int resolution )
{
	switch( resolution )
	{
	case kFine: return 4;
	case kSuperfine: return 8;
	default: return 2;
	}
}

void Place( Geometry& g, int fit, int outW, int outH )
{
	const double w = static_cast< double >( std::max( outW, 1 ) );
	const double h = static_cast< double >( std::max( outH, 1 ) );
	if( fit == kFitFrame )
	{
		g.sheetX = 0.0;
		g.sheetY = 0.0;
		g.sheetW = w;
		g.sheetH = h;
		return;
	}
	//A4 portrait, whole, centred; pixels are taken as square.
	double sh = h;
	double sw = h * t4::kLineMillimetres / t4::kA4Millimetres;
	if( sw > w )
	{
		sw = w;
		sh = w * t4::kA4Millimetres / t4::kLineMillimetres;
	}
	g.sheetW = sw;
	g.sheetH = sh;
	g.sheetX = ( w - sw ) * 0.5;
	g.sheetY = ( h - sh ) * 0.5;
}

Geometry Compute( int fit, int resolution, int inW, int inH )
{
	Geometry g;
	const double W   = static_cast< double >( std::max( inW, 1 ) );
	const double H   = static_cast< double >( std::max( inH, 1 ) );
	const double lpm = LinesPerMillimetre( resolution );
	const double pelMm = t4::kLineMillimetres / t4::kLinePels;

	if( fit == kFitFrame )
	{
		const double pageMm = t4::kLineMillimetres * H / W;
		g.lines             = std::max( 1, static_cast< int >( std::lround( pageMm * lpm ) ) );
		g.srcX0             = 0.0;
		g.srcDX             = W / t4::kLinePels;
		g.srcY0             = 0.0;
		g.srcDY             = H / g.lines;
		g.imgX0             = 0.0;
		g.imgX1             = t4::kLinePels;
		g.imgY0             = 0.0;
		g.imgY1             = g.lines;
		Place( g, fit, inW, inH );
		return g;
	}

	g.lines             = static_cast< int >( std::lround( t4::kA4Millimetres * lpm ) );
	const double lineMm = t4::kA4Millimetres / g.lines;

	//Millimetres of page per source pixel: cover or contain.
	const double sx = t4::kLineMillimetres / W;
	const double sy = t4::kA4Millimetres / H;
	const double s  = fit == kFitA4Crop ? std::max( sx, sy ) : std::min( sx, sy );
	const double iw = W * s;
	const double ih = H * s;
	const double ix = ( t4::kLineMillimetres - iw ) * 0.5;
	const double iy = ( t4::kA4Millimetres - ih ) * 0.5;

	g.srcX0 = -ix / s;
	g.srcDX = pelMm / s;
	g.srcY0 = -iy / s;
	g.srcDY = lineMm / s;
	g.imgX0 = std::max( 0.0, ix / pelMm );
	g.imgX1 = std::min( static_cast< double >( t4::kLinePels ), ( ix + iw ) / pelMm );
	g.imgY0 = std::max( 0.0, iy / lineMm );
	g.imgY1 = std::min( static_cast< double >( g.lines ), ( iy + ih ) / lineMm );
	Place( g, fit, inW, inH );
	return g;
}

} // namespace fax::layout
