// The reference side of demo/tools/check_port.mjs.
//
// Compiled by check_port.sh against the plugin's own source/Codec.cpp,
// source/Line.cpp, source/Header.cpp, source/Font.cpp, source/Layout.cpp and
// source/Controls.cpp, UNCHANGED. It runs them in the order Fax::startPage does
// (header, Encode, Corrupt, Schedule, Decode) with the same conversions, and
// writes every intermediate so the JavaScript port can be compared with them
// byte for byte. What this is NOT: the plugin binary, a GL context, the scan
// pass or the print pass -- the order of calls below is copied from
// Fax::startPage by hand, and only a reader checks that copy.
//
//   refchain page IN.pbm OUTSTEM coding resolution baud noise bursts conceal header pageIndex seconds
//   refchain layout      one line per (fit, resolution, size)
//   refchain controls    the conversions at v = 0, 0.01 .. 1
//   refchain header      the header text for a few pages and times

#include "../../source/Codec.h"
#include "../../source/Controls.h"
#include "../../source/Header.h"
#include "../../source/Layout.h"
#include "../../source/Line.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace fax;

namespace
{
bool readPbm( const std::string& path, codec::Page& page )
{
	std::ifstream in( path, std::ios::binary );
	std::string magic;
	int w = 0, h = 0;
	in >> magic >> w >> h;
	in.get();
	if( magic != "P4" || w != codec::kWidth || h <= 0 )
		return false;
	page.Resize( h );
	std::vector< unsigned char > raw( static_cast< size_t >( codec::kLineBytes ) * h );
	in.read( reinterpret_cast< char* >( raw.data() ), static_cast< std::streamsize >( raw.size() ) );
	if( !in )
		return false;
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < codec::kWidth; ++x )
			page.SetPel( y, x, ( raw[ static_cast< size_t >( y ) * codec::kLineBytes + x / 8 ] >> ( 7 - x % 8 ) ) & 1 );
	return true;
}

void save( const std::string& path, const void* data, size_t n )
{
	std::ofstream out( path, std::ios::binary );
	out.write( static_cast< const char* >( data ), static_cast< std::streamsize >( n ) );
}

int page( int argc, char** argv )
{
	if( argc != 13 )
	{
		std::fprintf( stderr, "page wants 11 arguments\n" );
		return 2;
	}
	const std::string in   = argv[ 2 ];
	const std::string stem = argv[ 3 ];
	const int coding       = std::atoi( argv[ 4 ] );
	const int resolution   = std::atoi( argv[ 5 ] );
	const int baud         = controls::BaudRate( std::atoi( argv[ 6 ] ) );
	const float noiseV     = static_cast< float >( std::atof( argv[ 7 ] ) );
	const float burstsV    = static_cast< float >( std::atof( argv[ 8 ] ) );
	const int conceal      = std::atoi( argv[ 9 ] );
	const bool header      = std::atoi( argv[ 10 ] ) != 0;
	const int pageIndex    = std::atoi( argv[ 11 ] );
	const double seconds   = std::atof( argv[ 12 ] );
	const double lpm       = layout::LinesPerMillimetre( resolution );

	codec::Page scanned;
	if( !readPbm( in, scanned ) )
	{
		std::fprintf( stderr, "cannot read %s\n", in.c_str() );
		return 1;
	}

	// Fax::startPage, step 2: the sending machine.
	if( header )
		header::Print( scanned, lpm, header::Text( pageIndex + 1, seconds ) );
	save( stem + ".scanned", scanned.bytes.data(), scanned.bytes.size() );

	codec::EncodeOptions encode;
	encode.coding      = coding;
	encode.k           = layout::K( resolution );
	encode.minLineBits = static_cast< int >( std::ceil( controls::kMinScanLineSeconds * baud - 1e-9 ) );
	codec::Transmission transmission;
	codec::Encode( scanned, encode, transmission );
	save( stem + ".sent", transmission.bits.data(), transmission.bits.size() );
	{
		std::ofstream out( stem + ".lines" );
		out << transmission.preambleBits << ' ' << transmission.rtcStart << '\n';
		for( const auto& r : transmission.lines )
			out << r.dataStart << ' ' << r.dataBits << ' ' << r.fillBits << ' ' << r.totalBits << ' ' << ( r.oneD ? 1 : 0 ) << '\n';
	}

	// Step 3: the line.
	line::Noise noise;
	noise.ber       = controls::BitErrorRate( noiseV );
	noise.burstBits = burstsV > 0.0f ? controls::BurstBits( burstsV ) : 1;
	noise.seed      = line::Hash( static_cast< uint32_t >( pageIndex ) * 2654435761u + 12345u );
	const size_t flips = line::Corrupt( transmission, noise, {} );
	save( stem + ".recv", transmission.bits.data(), transmission.bits.size() );
	line::Timing timing;
	line::Schedule( transmission, baud, 0, timing );
	{
		std::ofstream out( stem + ".timing" );
		char buffer[ 64 ];
		std::snprintf( buffer, sizeof( buffer ), "%.17g\n", timing.duration );
		out << flips << '\n' << buffer;
		for( double a : timing.arrival )
		{
			std::snprintf( buffer, sizeof( buffer ), "%.17g\n", a );
			out << buffer;
		}
	}

	// Step 4: the receiving machine.
	codec::DecodeOptions decode;
	decode.coding      = coding;
	decode.concealment = conceal;
	codec::Decoded decoded;
	codec::Decode( transmission.bits, scanned.lines, decode, decoded );
	save( stem + ".page", decoded.page.bytes.data(), decoded.page.bytes.size() );
	save( stem + ".bad", decoded.bad.data(), decoded.bad.size() );
	save( stem + ".shown", decoded.shown.data(), decoded.shown.size() );
	{
		std::ofstream out( stem + ".segments" );
		out << decoded.segments << '\n';
	}
	return 0;
}

int layoutGrid()
{
	const int sizes[][ 2 ] = { { 1280, 720 }, { 960, 540 }, { 640, 360 }, { 1920, 1080 }, { 720, 1280 }, { 1000, 1000 }, { 1733, 211 } };
	for( int fit = 0; fit < layout::kFitCount; ++fit )
		for( int res = 0; res < layout::kResolutionCount; ++res )
			for( const auto& s : sizes )
			{
				const layout::Geometry g = layout::Compute( fit, res, s[ 0 ], s[ 1 ] );
				std::printf( "%d %d %d %d %d %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g\n", fit, res,
				             s[ 0 ], s[ 1 ], g.lines, g.srcX0, g.srcDX, g.srcY0, g.srcDY, g.imgX0, g.imgX1, g.imgY0, g.imgY1,
				             g.sheetX, g.sheetY, g.sheetW, g.sheetH );
			}
	return 0;
}

int controlsGrid()
{
	for( int i = 0; i <= 100; ++i )
	{
		const float v = static_cast< float >( i ) / 100.0f;
		std::printf( "%.9g %.17g %d %.17g\n", static_cast< double >( v ), controls::BitErrorRate( v ), controls::BurstBits( v ),
		             controls::Threshold( v ) );
	}
	for( int b = 0; b < controls::kBaudCount; ++b )
		std::printf( "baud %d %d\n", controls::BaudRate( b ),
		             static_cast< int >( std::ceil( controls::kMinScanLineSeconds * controls::BaudRate( b ) - 1e-9 ) ) );
	return 0;
}

int headerGrid()
{
	const int pages[]      = { 1, 2, 999, 1000, 1234 };
	const double seconds[] = { 0.0, 0.9999999, 59.5, 3599.0, 3600.0, 86399.0, 86400.5, 123456.7 };
	for( int p : pages )
		for( double s : seconds )
			std::printf( "%s\n", header::Text( p, s ).c_str() );
	for( double lpm : { 3.85, 7.7, 15.4 } )
		std::printf( "band %d\n", header::BandLines( lpm ) );
	return 0;
}
} // namespace

int main( int argc, char** argv )
{
	const std::string mode = argc > 1 ? argv[ 1 ] : "";
	if( mode == "page" )
		return page( argc, argv );
	if( mode == "layout" )
		return layoutGrid();
	if( mode == "controls" )
		return controlsGrid();
	if( mode == "header" )
		return headerGrid();
	std::fprintf( stderr, "usage: refchain page|layout|controls|header ...\n" );
	return 2;
}
