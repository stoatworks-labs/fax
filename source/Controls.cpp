#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace fax::controls
{

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

const char* CodingName( int index )
{
	static const char* const names[ kCodingCount ] = { "MH", "MR" };
	return names[ std::clamp( index, 0, kCodingCount - 1 ) ];
}

const char* BaudName( int index )
{
	static const char* const names[ kBaudCount ] = { "2400", "4800", "9600", "14400" };
	return names[ std::clamp( index, 0, kBaudCount - 1 ) ];
}

int BaudRate( int index )
{
	static const int rates[ kBaudCount ] = { 2400, 4800, 9600, 14400 };
	return rates[ std::clamp( index, 0, kBaudCount - 1 ) ];
}

const char* ModeName( int index )
{
	static const char* const names[ kModeCount ] = { "Page", "Live" };
	return names[ std::clamp( index, 0, kModeCount - 1 ) ];
}

const char* ConcealmentName( int index )
{
	static const char* const names[ kConcealmentCount ] = { "Off", "Repeat Line" };
	return names[ std::clamp( index, 0, kConcealmentCount - 1 ) ];
}

const char* PaperName( int index )
{
	static const char* const names[ kPaperCount ] = { "Thermal", "Plain" };
	return names[ std::clamp( index, 0, kPaperCount - 1 ) ];
}

double BitErrorRate( float value )
{
	const double v = std::clamp( static_cast< double >( value ), 0.0, 1.0 );
	if( v <= 0.0 )
		return 0.0;
	return std::pow( 10.0, -6.0 + 4.0 * v );
}

int BurstBits( float value )
{
	const double v = std::clamp( static_cast< double >( value ), 0.0, 1.0 );
	return 1 + static_cast< int >( std::lround( v * 63.0 ) );
}

double Threshold( float value )
{
	return std::clamp( static_cast< double >( value ), 0.0, 1.0 );
}

Colours PaperColours( int paper )
{
	//Thermal paper is a warm grey that never was white, and its image a
	//brown-black; plain paper is white and toner is black. Chosen by eye.
	if( paper == kPaperPlain )
		return { { 0.96f, 0.96f, 0.95f }, { 0.06f, 0.06f, 0.07f } };
	return { { 0.86f, 0.85f, 0.80f }, { 0.20f, 0.17f, 0.16f } };
}

} // namespace fax::controls
