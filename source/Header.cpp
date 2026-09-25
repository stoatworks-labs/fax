#include "Header.h"

#include "Font.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fax::header
{
namespace
{
constexpr int kDotPels   = 4; ///< a font dot is 4 pels (0.5 mm) wide
constexpr int kLeftPels  = 64;///< 8 mm margin
constexpr int kTopDots   = 1; ///< one dot of paper above the text
constexpr int kBandDots  = font::kHeight + 2;

int dotLines( double lpm )
{
	return std::max( 1, static_cast< int >( std::lround( 0.5 * lpm ) ) );
}
} // namespace

int BandLines( double linesPerMillimetre )
{
	return kBandDots * dotLines( linesPerMillimetre );
}

std::string Text( int page, double seconds )
{
	const double t    = std::max( seconds, 0.0 );
	const long long s = static_cast< long long >( std::floor( t + 1e-6 ) );
	char text[ 96 ];
	std::snprintf( text, sizeof( text ), "%02d:%02d:%02d   FROM: SW FAX   G3 T.4          P.%03d",
	               static_cast< int >( ( s / 3600 ) % 24 ), static_cast< int >( ( s / 60 ) % 60 ), static_cast< int >( s % 60 ),
	               ( ( page - 1 ) % 999 ) + 1 );
	return text;
}

void Print( codec::Page& page, double linesPerMillimetre, const std::string& text )
{
	const int sy   = dotLines( linesPerMillimetre );
	const int band = std::min( page.lines, BandLines( linesPerMillimetre ) );

	//The band is blank paper first; the sending machine prints over nothing.
	for( int y = 0; y < band; ++y )
		std::fill( page.Row( y ), page.Row( y ) + codec::kLineBytes, 0 );

	for( size_t i = 0; i < text.size(); ++i )
	{
		const int x0 = kLeftPels + static_cast< int >( i ) * font::kAdvance * kDotPels;
		if( x0 + font::kWidth * kDotPels > codec::kWidth )
			break;
		for( int gy = 0; gy < font::kHeight; ++gy )
			for( int gx = 0; gx < font::kWidth; ++gx )
			{
				if( !font::Bit( static_cast< unsigned char >( text[ i ] ), gx, gy ) )
					continue;
				for( int ly = 0; ly < sy; ++ly )
				{
					const int y = ( kTopDots + gy ) * sy + ly;
					if( y >= page.lines )
						continue;
					for( int lx = 0; lx < kDotPels; ++lx )
						page.SetPel( y, x0 + gx * kDotPels + lx, codec::kBlack );
				}
			}
	}
}

} // namespace fax::header
