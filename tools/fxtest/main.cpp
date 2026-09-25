/**
	fxtest -- render Fax offline, and read the fax back out of it.

	Every check renders a synthetic source through the REAL plugin class in a
	headless GL context, or drives the plugin's own codec with no GL at all,
	and measures rather than eyeballs.

		fxtest --out /tmp/frame.png     a picture, on the test card
		fxtest --list                   every parameter, its kind and default
		fxtest --names                  every name 16 characters or fewer, unique
		fxtest --tables                 the MH codes round-trip every run length for
		                                both colours; the code lengths match the
		                                standard's; the code sets are prefix-free and
		                                no two codes make an EOL (no GL)
		fxtest --roundtrip              decode( encode( page ) ) is the page, bit for
		                                bit, MH and MR, on a corpus and through the plugin
		fxtest --scan                   the scanned page is the source thresholded /
		                                dithered, recomputed in double
		fxtest --print                  every output pixel is the page's pel coverage,
		                                recomputed in double; alpha is 1
		fxtest --streak                 one bit error in an MH line corrupts that line
		                                from the error onwards and nothing else
		fxtest --wedge                  one bit error in MR corrupts lines only until
		                                the next one-dimensional line: K = 2, 4, 8
		fxtest --conceal                with Repeat Line, a bad line is the line above
		fxtest --timing                 each line arrives at its bits / baud, floored at
		                                the minimum scan time; blank beats busy
		fxtest --resize                 a resize mid-page changes nothing on the page
		fxtest --negative               every check above can FAIL
		fxtest --bench                  the render cost, and the CPU half's share
		fxtest --export DIR             G3 and G4 streams for an independent decoder
		fxtest --dump-shaders DIR       the exact GLSL the plugin compiles
		fxtest --pipe                   raw frames in, raw frames out

	Every GL check drives a synthetic clock through `SetTime`. Run each at two
	rasters at least -- the one you develop at and 320x180, which is what CI
	uses. FXTEST_RENDERER=software asks for Apple's software renderer, which is
	what CI's macOS runner has. AGENTS.md has one line per check on where each
	tolerance comes from.

	`--script` is a plain text file of `frame  Parameter Name  value` lines,
	the fleet's format. Sliders ramp between cues; options, booleans, integers
	and events STEP (they hold the earlier cue's value until the next cue's
	frame). `--pipe` takes the fleet's frame format:

		ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
		  | fxtest --pipe --size 1920x1080 [--script cues.txt] \
		  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
*/

#include "Codec.h"
#include "Controls.h"
#include "Fax.h"
#include "Layout.h"
#include "Line.h"
#include "Shaders.h"
#include "T4.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace fax;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Sources. Rows are top-first in every source, the way a picture is, and
// flipped on the way into GL. The checks read back through the same flip,
// so "row 0" is the top of the picture everywhere in this file.
//---------------------------------------------------------------------------
using Image = std::vector< unsigned char >;

/// The test card: a sky gradient over a warm ground, a row of greys, six
/// colour patches, a skin tone, a deep shadow, a specular highlight, a set of
/// black bars of halving width (a resolution wedge), and a disc on a slow
/// Lissajous path so consecutive frames differ. `alphaHole` makes the right
/// third fully transparent, as Resolume's DXV clips are wherever the clip is not.
Image buildCard( int width, int height, int frame, bool alphaHole = false )
{
	Image card( static_cast< size_t >( width ) * height * 4 );

	const float w = static_cast< float >( width );
	const float h = static_cast< float >( height );
	const float t = static_cast< float >( frame );

	const float discX = 0.5f * w + 0.30f * w * std::sin( t * 0.05f );
	const float discY = 0.42f * h + 0.12f * h * std::sin( t * 0.037f + 1.1f );
	const float discR = 0.09f * h;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / w;
			const float v = ( static_cast< float >( y ) + 0.5f ) / h;

			float r, g, b;
			if( v < 0.55f )
			{
				const float k = v / 0.55f;
				r             = 0.15f + 0.55f * k;
				g             = 0.35f + 0.45f * k;
				b             = 0.95f - 0.15f * k;
			}
			else
			{
				r = 0.42f;
				g = 0.33f;
				b = 0.22f;
			}

			if( v > 0.84f )
			{
				const int step = std::min( 10, static_cast< int >( u * 11.0f ) );
				r = g = b = static_cast< float >( step ) / 10.0f;
			}
			else if( v > 0.66f && v < 0.80f )
			{
				static const float patches[ 8 ][ 3 ] = {
					{ 0.75f, 0.12f, 0.10f }, { 0.20f, 0.60f, 0.20f }, { 0.12f, 0.20f, 0.75f },
					{ 0.10f, 0.65f, 0.70f }, { 0.70f, 0.15f, 0.60f }, { 0.90f, 0.80f, 0.15f },
					{ 0.87f, 0.64f, 0.52f },//skin
					{ 0.03f, 0.03f, 0.03f },//deep shadow
				};
				const int patch = std::min( 7, static_cast< int >( u * 8.0f ) );
				r               = patches[ patch ][ 0 ];
				g               = patches[ patch ][ 1 ];
				b               = patches[ patch ][ 2 ];
			}
			else if( v > 0.08f && v < 0.30f && u > 0.05f && u < 0.45f )
			{
				//The wedge: bars whose period halves every tenth of the width.
				const float band   = ( u - 0.05f ) / 0.4f;
				const int octave   = std::min( 5, static_cast< int >( band * 6.0f ) );
				const float period = w * 0.04f / static_cast< float >( 1 << octave );
				if( std::fmod( static_cast< float >( x ), period ) < period * 0.5f )
					r = g = b = 0.05f;
			}

			const float hx = u - 0.85f, hy = v - 0.15f;
			if( hx * hx + hy * hy < 0.0015f )
				r = g = b = 1.0f;

			const float px   = static_cast< float >( x ) + 0.5f;
			const float dx   = px - discX;
			const float dy   = ( static_cast< float >( y ) + 0.5f ) - discY;
			const float dist = std::sqrt( dx * dx + dy * dy );
			if( dist < discR )
			{
				const float edge = std::min( 1.0f, ( discR - dist ) / ( discR * 0.2f ) );
				r                = r + ( 0.05f - r ) * edge;
				g                = g + ( 0.05f - g ) * edge;
				b                = b + ( 0.10f - b ) * edge;
			}

			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			card[ i + 0 ]  = static_cast< unsigned char >( std::clamp( r, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 1 ]  = static_cast< unsigned char >( std::clamp( g, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 2 ]  = static_cast< unsigned char >( std::clamp( b, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 3 ]  = ( alphaHole && u > 0.66f ) ? 0 : 255;
		}
	}

	return card;
}

Image buildFlat( int width, int height, unsigned char value )
{
	Image image( static_cast< size_t >( width ) * height * 4, value );
	for( size_t i = 3; i < image.size(); i += 4 )
		image[ i ] = 255;
	return image;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	//FXTEST_RENDERER=software asks for Apple's software renderer by id, on a
	//Mac that has a GPU. It is what a GPU-less CI runner falls back to, and it
	//is not repeatable at the last bit (repousse's CI failed by one ulp), so a
	//check that only holds on this Mac's GPU is found here before CI finds it.
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "FXTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		std::fprintf( stderr, "fxtest: FXTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

Image flipRows( const Image& image, int width, int height )
{
	Image flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ), flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Fax::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Fax& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Fax::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		else if( p.type == FF_TYPE_INTEGER )
		{
			const RangeStruct range = plugin.GetParamRange( i );
			p.low                   = range.min;
			p.high                  = range.max;
		}
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Fax& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

/// Options, booleans, integers and events STEP between cues; sliders ramp.
/// A ramp through an option would visit every option between the two cues,
/// and a ramp through a boolean would flip it at the halfway frame.
bool stepsBetweenCues( Fax& plugin, unsigned int index )
{
	const unsigned int type = plugin.GetParamType( index );
	return type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_INTEGER || type == FF_TYPE_EVENT;
}

bool applySetting( Fax& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name = assignment.substr( 0, equals );
	const int index        = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

bool set( Fax& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and the clock that drives it.
//---------------------------------------------------------------------------
struct Session
{
	Fax plugin;
	int width  = 0;
	int height = 0;
	double fps = 60.0;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputTexture = makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;

		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip or the composition changes size: hand
	/// the SAME instance a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	/// Render at `seconds` on the synthetic clock, whose unit is declared.
	bool renderAtTime( double seconds )
	{
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( seconds );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed at t=%.6f\n", seconds );
		return ok;
	}

	bool renderAt( int frame )
	{
		return renderAtTime( static_cast< double >( frame ) / fps );
	}

	void upload( const Image& pixels )
	{
		const Image flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	/// Render one frame of 8-bit `pixels` (top row first) at frame `frame`.
	bool render( int frame, const Image& pixels )
	{
		upload( pixels );
		return renderAt( frame );
	}

	/// The output, top row first, 8-bit.
	Image readBack()
	{
		Image pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

//---------------------------------------------------------------------------
// The baseline every GL check starts from: the frame as the page, standard,
// threshold 0.5, no halftone; MH, a clean line, 14400, LIVE (every frame a
// whole page, no timing); concealment off, plain paper, no header, fully in.
// Each check then moves the one or two things it is about.
//---------------------------------------------------------------------------
using Settings = std::vector< std::pair< const char*, float > >;

void baseline( Fax& p )
{
	set( p, "Fit", static_cast< float >( layout::kFitFrame ) );
	set( p, "Resolution", static_cast< float >( layout::kStandard ) );
	set( p, "Threshold", 0.5f );
	set( p, "Halftone", 0.0f );
	set( p, "Coding", static_cast< float >( codec::kMH ) );
	set( p, "Line Noise", 0.0f );
	set( p, "Bursts", 0.0f );
	set( p, "Baud", 3.0f );
	set( p, "Mode", static_cast< float >( controls::kModeLive ) );
	set( p, "Concealment", static_cast< float >( codec::kConcealOff ) );
	set( p, "Paper", static_cast< float >( controls::kPaperPlain ) );
	set( p, "Header", 0.0f );
	set( p, "Mix", 1.0f );
}

bool prepare( Session& session, const Settings& settings, int perturb, int width, int height )
{
	baseline( session.plugin );
	for( const auto& s : settings )
		if( !set( session.plugin, s.first, s.second ) )
			return false;
	session.plugin.SetPerturbForTest( perturb );
	return session.begin( width, height );
}

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAILED";
}

/// Lines of two pages that differ.
std::vector< int > differingLines( const codec::Page& a, const codec::Page& b )
{
	std::vector< int > out;
	for( int i = 0; i < std::min( a.lines, b.lines ); ++i )
		if( std::memcmp( a.Row( i ), b.Row( i ), codec::kLineBytes ) != 0 )
			out.push_back( i );
	return out;
}

/// The scan lines a pixel row's footprint touches, for a page placed at `g`
/// on an output `height` rows tall.
void linesOfRow( const layout::Geometry& g, int row, int& first, int& last )
{
	const double ky = g.lines / g.sheetH;
	const double v0 = ( row - g.sheetY ) * ky;
	const double v1 = ( row + 1 - g.sheetY ) * ky;
	first           = std::max( 0, static_cast< int >( std::floor( v0 ) ) );
	last            = std::min( g.lines - 1, static_cast< int >( std::ceil( v1 ) ) - 1 );
}

/// Pixels of `a` and `b` in `row` that differ by more than `tolerance` in a
/// colour channel.
int rowDifferences( const Image& a, const Image& b, int width, int row, int tolerance )
{
	int n = 0;
	for( int x = 0; x < width; ++x )
	{
		const unsigned char* p = a.data() + ( static_cast< size_t >( row ) * width + x ) * 4;
		const unsigned char* q = b.data() + ( static_cast< size_t >( row ) * width + x ) * 4;
		for( int c = 0; c < 3; ++c )
			if( std::abs( static_cast< int >( p[ c ] ) - static_cast< int >( q[ c ] ) ) > tolerance )
			{
				++n;
				break;
			}
	}
	return n;
}

/// Where the sheet is on this session's output.
layout::Geometry placed( Session& s, int fit )
{
	layout::Geometry g = s.plugin.GeometryForTest();
	layout::Place( g, fit, s.width, s.height );
	return g;
}

/// Every EOL in a stream, found here a bit at a time rather than by the
/// decoder's word-wise search, so --timing's predictions share nothing with
/// the receiver: the index of each 1 that ends eleven or more zeros.
std::vector< size_t > eolsOf( const codec::Bits& bits )
{
	std::vector< size_t > eols;
	size_t zeros = 0;
	for( size_t i = 0; i < bits.size(); ++i )
	{
		if( bits[ i ] == 0 )
		{
			++zeros;
			continue;
		}
		if( zeros >= 11 )
			eols.push_back( i );
		zeros = 0;
	}
	return eols;
}

/// Flipping this bit makes eleven zeros (a forged EOL) or breaks one.
bool forgesOrBreaksEol( const codec::Bits& bits, size_t at )
{
	if( bits[ at ] == 0 )
		return false;//0 -> 1 cannot make a zero run; data holds no EOL to break
	size_t left = 0, right = 0;
	while( at > left && bits[ at - left - 1 ] == 0 )
		++left;
	while( at + right + 1 < bits.size() && bits[ at + right + 1 ] == 0 )
		++right;
	return left + right + 1 >= 11;
}

//---------------------------------------------------------------------------
// --names
//---------------------------------------------------------------------------
int runNames()
{
	Fax plugin;
	std::set< std::string > seen;
	int failures = 0;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		if( p.name.size() > 16 )
		{
			std::printf( "   %s: %zu characters, over 16  FAILED\n", p.name.c_str(), p.name.size() );
			++failures;
		}
		if( !seen.insert( p.name ).second )
		{
			std::printf( "   %s: not unique  FAILED\n", p.name.c_str() );
			++failures;
		}
	}
	std::printf( "names: %zu parameters, longest %zu characters, all unique  %s\n", seen.size(),
	             [ & ] {
		             size_t m = 0;
		             for( const auto& n : seen )
			             m = std::max( m, n.size() );
		             return m;
	             }(),
	             verdict( failures == 0 ) );
	return failures;
}

//---------------------------------------------------------------------------
// --tables (no GL)
//
// 1. Every code's LENGTH against tools/fixtures/t4-code-lengths.txt, which
//    tools/check_tables.py extracted from an independent transcription of
//    the Recommendation (golang.org/x/image/ccitt). Exact.
// 2. Each code set (white runs, black runs, 2-D modes) is prefix-free, so a
//    decoder can only read one thing; its Kraft sum is reported.
// 3. The longest runs of leading and trailing zeros are the 7 and 3 T4.h
//    states, so no two codes in a row make eleven zeros: an EOL is
//    unmistakable in valid data.
// 4. Every run length 0..2560 (and on to 5000 by 7s, which needs repeated
//    2560 make-ups) of both colours round-trips through the plugin's coder
//    and decoder, consuming exactly the bits written, and the bits written
//    are exactly as many as the fixture's lengths for the make-up and
//    terminating codes the Recommendation prescribes for that run.
//---------------------------------------------------------------------------
std::map< std::pair< std::string, int >, int > loadLengths( const std::string& path, std::string& error )
{
	std::map< std::pair< std::string, int >, int > lengths;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return lengths;
	}
	std::string line;
	while( std::getline( file, line ) )
	{
		if( line.empty() || line[ 0 ] == '#' )
			continue;
		std::istringstream in( line );
		std::string table;
		int value = 0, length = 0;
		if( in >> table >> value >> length )
			lengths[ { table, value } ] = length;
	}
	return lengths;
}

int runTables( const std::string& fixture, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	auto report  = [ & ]( bool ok, const char* format, auto... args ) {
		if( !ok )
			++failures;
		if( quiet )
			return;
		std::printf( "   " );
		std::printf( format, args... );
		std::printf( "  %s\n", verdict( ok ) );
	};

	if( !quiet )
		std::printf( "== tables: T.4 Tables 2, 3a, 3b and 4 against the standard's code lengths\n" );

	std::string error;
	const auto lengths = loadLengths( fixture, error );
	if( !error.empty() || lengths.size() != 204 )
	{
		std::printf( "   %s (%zu lengths read, want 204)  FAILED\n", error.empty() ? fixture.c_str() : error.c_str(), lengths.size() );
		return 1;
	}

	//1. Lengths.
	int compared = 0, wrong = 0;
	for( int colour = 0; colour < 2; ++colour )
		for( const codec::TableEntry& e : codec::Table( colour, perturb ) )
		{
			std::string table = e.value > 1728 ? "ext-makeup" : std::string( colour ? "black" : "white" ) + ( e.makeup ? "-makeup" : "-term" );
			const auto it     = lengths.find( { table, e.value } );
			++compared;
			if( it == lengths.end() || it->second != static_cast< int >( std::strlen( e.bits ) ) )
			{
				++wrong;
				if( !quiet )
					std::printf( "      %s %d: %zu bits, the standard's is %d\n", table.c_str(), e.value, std::strlen( e.bits ),
					             it == lengths.end() ? -1 : it->second );
			}
		}
	for( const auto& m : t4::kModes )
	{
		const auto it = lengths.find( { "modes", m.value } );
		++compared;
		if( it == lengths.end() || it->second != static_cast< int >( std::strlen( m.bits ) ) )
			++wrong;
	}
	report( wrong == 0, "lengths: %d codes (64 + 27 + 13 white, the same black, 9 modes) against the fixture, %d differ", compared, wrong );

	//2. Prefix-free, and the Kraft sum.
	auto prefixFree = [ & ]( const std::vector< std::string >& codes, double& kraft ) {
		kraft = 0.0;
		for( const auto& c : codes )
			kraft += std::ldexp( 1.0, -static_cast< int >( c.size() ) );
		for( size_t i = 0; i < codes.size(); ++i )
			for( size_t j = 0; j < codes.size(); ++j )
				if( i != j && codes[ j ].compare( 0, codes[ i ].size(), codes[ i ] ) == 0 )
					return false;
		return true;
	};
	std::vector< std::string > all;
	for( int colour = 0; colour < 2; ++colour )
	{
		std::vector< std::string > codes;
		for( const codec::TableEntry& e : codec::Table( colour, perturb ) )
			codes.push_back( e.bits );
		double kraft = 0.0;
		const bool ok = prefixFree( codes, kraft );
		report( ok && kraft <= 1.0, "%s: %zu codes prefix-free, Kraft sum %.6f (<= 1)", colour ? "black" : "white", codes.size(), kraft );
		all.insert( all.end(), codes.begin(), codes.end() );
	}
	{
		std::vector< std::string > codes;
		for( const auto& m : t4::kModes )
			codes.push_back( m.bits );
		double kraft = 0.0;
		const bool ok = prefixFree( codes, kraft );
		report( ok && kraft <= 1.0, "2-D modes: 9 codes prefix-free, Kraft sum %.6f", kraft );
		all.insert( all.end(), codes.begin(), codes.end() );
	}

	//3. Zero runs.
	int leading = 0, trailing = 0, inner = 0;
	for( const auto& c : all )
	{
		int l = 0;
		while( l < static_cast< int >( c.size() ) && c[ static_cast< size_t >( l ) ] == '0' )
			++l;
		int t = 0;
		while( t < static_cast< int >( c.size() ) && c[ c.size() - 1 - static_cast< size_t >( t ) ] == '0' )
			++t;
		int run = 0;
		for( char ch : c )
		{
			run   = ch == '0' ? run + 1 : 0;
			inner = std::max( inner, run );
		}
		leading  = std::max( leading, l );
		trailing = std::max( trailing, t );
	}
	report( leading == t4::kMaxLeadingZeros && trailing == t4::kMaxTrailingZeros && leading + trailing < 11 && inner < 11,
	        "zeros: at most %d leading, %d trailing, %d anywhere in one code; two codes in a row hold at most %d < 11, so no EOL",
	        leading, trailing, inner, leading + trailing );

	//4. Every run, both colours.
	auto expectedBits = [ & ]( int colour, int run ) {
		const std::string c = colour ? "black" : "white";
		int n               = 0;
		while( run > 2560 )
		{
			n += lengths.at( { "ext-makeup", 2560 } );
			run -= 2560;
		}
		if( run >= 64 )
		{
			const int m = run / 64 * 64;
			n += m <= 1728 ? lengths.at( { c + "-makeup", m } ) : lengths.at( { "ext-makeup", m } );
			run -= m;
		}
		return n + lengths.at( { c + "-term", run } );
	};
	int runs = 0, bad = 0;
	std::vector< int > tried;
	for( int r = 0; r <= 2560; ++r )
		tried.push_back( r );
	for( int r = 2561; r <= 5000; r += 7 )
		tried.push_back( r );
	for( int colour = 0; colour < 2; ++colour )
		for( int r : tried )
		{
			codec::Bits bits;
			codec::PutRun( bits, colour, r, perturb );
			size_t pos       = 0;
			const int back   = codec::ReadRun( bits, pos, bits.size(), colour );
			const bool right = back == r && pos == bits.size()
			                   && static_cast< int >( bits.size() ) == expectedBits( colour, r );
			//Under the table-swap perturbation the harness codes with the
			//swapped strings, as a transcription slip would.
			bool swapped = true;
			if( perturb & codec::kPerturbTableSwap )
			{
				codec::Bits alt;
				const auto table = codec::Table( colour, perturb );
				int rest         = r;
				while( rest > 2560 )
				{
					codec::PutBits( alt, table[ 64 + 27 + 12 ].bits );
					rest -= 2560;
				}
				if( rest >= 64 )
				{
					const int m = rest / 64;
					codec::PutBits( alt, m <= 27 ? table[ 64 + m - 1 ].bits : table[ 64 + 27 + m - 28 ].bits );
					rest -= m * 64;
				}
				codec::PutBits( alt, table[ static_cast< size_t >( rest ) ].bits );
				swapped = static_cast< int >( alt.size() ) == expectedBits( colour, r );
			}
			++runs;
			if( !right || !swapped )
				++bad;
		}
	report( bad == 0, "runs: %d run lengths (0..2560 and 2561..5000 by 7, white and black) round-trip, exactly the standard's bits, %d wrong", runs, bad );

	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "tables: every code is the standard's" : "tables: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --roundtrip
//
// With no noise, decode( encode( page ) ) equals the page bit for bit, in MH
// and in MR at K = 2, 4, 8 -- on a corpus built to reach every code and every
// 2-D mode (all white, all black, alternating pels, single pels at both edges,
// random pages, pages whose lines drift by -3..+3 pels from the line above),
// then through the plugin on the test card at the raster, where the page the
// receiver shows must be the page the scanner made.
//
// Negative control: a coder that writes VR1 as VR2 must fail.
//---------------------------------------------------------------------------
codec::Page corpusPage( int kind, int lines, uint32_t seed )
{
	codec::Page p;
	p.Resize( lines );
	for( int y = 0; y < lines; ++y )
		for( int x = 0; x < codec::kWidth; ++x )
		{
			int bit = 0;
			switch( kind )
			{
			case 0: bit = 0; break;
			case 1: bit = 1; break;
			case 2: bit = ( x + y ) & 1; break;
			case 3: bit = ( x == 0 || x == codec::kWidth - 1 ) && ( y & 1 ); break;
			case 4: bit = static_cast< int >( line::Hash( seed ^ line::Hash( static_cast< uint32_t >( y * 4096 + x ) ) ) & 1u ); break;
			default: break;
			}
			if( bit )
				p.SetPel( y, x, 1 );
		}
	if( kind == 5 || kind == 6 )
	{
		//Edges that drift from line to line: every vertical mode, and now and
		//then a jump that needs pass or horizontal.
		std::vector< int > edges;
		for( int e = 0; e < 40; ++e )
			edges.push_back( static_cast< int >( line::Hash( seed + static_cast< uint32_t >( e ) ) % codec::kWidth ) );
		for( int y = 0; y < lines; ++y )
		{
			std::sort( edges.begin(), edges.end() );
			int colour = 0, e = 0;
			for( int x = 0; x < codec::kWidth; ++x )
			{
				while( e < static_cast< int >( edges.size() ) && edges[ static_cast< size_t >( e ) ] <= x )
				{
					colour ^= 1;
					++e;
				}
				if( colour )
					p.SetPel( y, x, 1 );
			}
			for( size_t i = 0; i < edges.size(); ++i )
			{
				const uint32_t h = line::Hash( seed ^ line::Hash( static_cast< uint32_t >( y * 64 ) + static_cast< uint32_t >( i ) ) );
				int step         = static_cast< int >( h % 7 ) - 3;
				if( kind == 6 && ( h >> 8 ) % 13 == 0 )
					step = static_cast< int >( ( h >> 12 ) % 200 ) - 100;
				edges[ i ] = std::clamp( edges[ i ] + step, 0, codec::kWidth - 1 );
			}
		}
	}
	return p;
}

int runRoundtrip( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	auto report  = [ & ]( bool ok, const char* format, auto... args ) {
		if( !ok )
			++failures;
		if( quiet )
			return;
		std::printf( "   " );
		std::printf( format, args... );
		std::printf( "  %s\n", verdict( ok ) );
	};
	if( !quiet )
		std::printf( "== roundtrip at %dx%d: with no noise the receiver's page is the sender's, bit for bit\n", width, height );

	//The corpus, no GL.
	int modes[ t4::kModeCount ] = {};
	int pages = 0, wrongPages = 0, badLines = 0;
	for( int kind = 0; kind <= 6; ++kind )
		for( int coding = 0; coding < 2; ++coding )
			for( int k : { 2, 4, 8 } )
			{
				if( coding == codec::kMH && k != 2 )
					continue;
				const codec::Page page = corpusPage( kind, 64, 1000u + static_cast< uint32_t >( kind ) );
				codec::EncodeOptions eo;
				eo.coding      = coding;
				eo.k           = k;
				eo.minLineBits = 96;
				eo.perturb     = perturb;
				eo.modeCounts  = modes;
				codec::Transmission t;
				codec::Encode( page, eo, t );
				codec::DecodeOptions dopt;
				dopt.coding      = coding;
				dopt.concealment = codec::kConcealOff;
				codec::Decoded d;
				codec::Decode( t.bits, page.lines, dopt, d );
				++pages;
				if( !differingLines( page, d.page ).empty() || d.segments != page.lines )
					++wrongPages;
				for( uint8_t b : d.bad )
					badLines += b;
			}
	int unused = 0;
	for( int m = 0; m < t4::kModeCount; ++m )
		unused += modes[ m ] == 0;
	report( wrongPages == 0 && badLines == 0, "corpus: %d pages of 64 lines (7 kinds; MH, MR at K 2 4 8): %d differ, %d lines flagged bad",
	        pages, wrongPages, badLines );
	report( unused == 0, "corpus: every 2-D mode used (P %d, H %d, V0 %d, VR1-3 %d %d %d, VL1-3 %d %d %d)", modes[ 0 ], modes[ 1 ],
	        modes[ 2 ], modes[ 3 ], modes[ 4 ], modes[ 5 ], modes[ 6 ], modes[ 7 ], modes[ 8 ] );

	//Through the plugin, on the card, both codings, standard and fine,
	//threshold and halftone, with the header.
	const Image card = buildCard( width, height, 7 );
	for( int coding = 0; coding < 2; ++coding )
		for( int resolution : { 0, 1 } )
			for( int halftone : { 0, 1 } )
			{
				Session s;
				if( !prepare( s, { { "Coding", static_cast< float >( coding ) }, { "Resolution", static_cast< float >( resolution ) },
				                   { "Halftone", static_cast< float >( halftone ) }, { "Header", 1.0f } },
				              perturb, width, height ) )
					return failures + 1;
				s.render( 0, card );
				const codec::Page& sent = s.plugin.ScannedForTest();
				const codec::Page& got  = s.plugin.DecodedForTest().page;
				const auto diff         = differingLines( sent, got );
				const auto shownDiff    = differingLines( got, s.plugin.DisplayForTest() );
				int flagged             = 0;
				for( uint8_t b : s.plugin.DecodedForTest().bad )
					flagged += b;
				report( diff.empty() && shownDiff.empty() && flagged == 0,
				        "plugin %s %-8s %s: %d lines, %zu bits sent, %zu lines differ, %d flagged bad",
				        controls::CodingName( coding ), layout::ResolutionName( resolution ), halftone ? "halftone " : "threshold",
				        sent.lines, s.plugin.SentForTest().size(), diff.size(), flagged );
				s.end();
			}

	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "roundtrip: the code is lossless, as T.4 is" : "roundtrip: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --scan
//
// The page the scan pass wrote against the same scan recomputed here in
// double from the 8-bit source: which pixels a pel averages (centres inside
// its rectangle, strided past eight a side, or the pixel under its centre),
// BT.709 luma over white by alpha, the threshold and the Bayer offset. Pels
// whose decision hangs on float rounding are left out and counted: a luma
// within 1e-4 of the threshold (float32 sums of at most 64 terms are good to
// ~4e-6), or a rectangle edge within 1e-3 px of where the pixel set changes
// (float32 positions are good to ~2.3e-4 px at 4K).
//
// Negative control: BT.601 weights in the shader must fail.
//---------------------------------------------------------------------------
int cpuPel( const Image& src, int W, int H, const layout::Geometry& g, double threshold, bool halftone, int pel, int line,
            bool& ambiguous )
{
	static const int bayer[ 64 ] = { 0,  32, 8,  40, 2,  34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4,  36, 14, 46,
		                             6,  38, 60, 28, 52, 20, 62, 30, 54, 22, 3,  35, 11, 43, 1,  33, 9,  41, 51, 19, 59, 27,
		                             49, 17, 57, 25, 15, 47, 7,  39, 13, 45, 5,  37, 63, 31, 55, 23, 61, 29, 53, 21 };
	constexpr double kPos = 1e-3;
	auto near             = [ & ]( double a, double b ) { return std::abs( a - b ) < kPos; };
	auto nearInteger      = [ & ]( double a ) { return std::abs( a - std::round( a ) ) < kPos; };

	ambiguous       = false;
	const double cx = pel + 0.5, cy = line + 0.5;
	if( near( cx, g.imgX0 ) || near( cx, g.imgX1 ) || near( cy, g.imgY0 ) || near( cy, g.imgY1 ) )
		ambiguous = true;
	if( cx < g.imgX0 || cx >= g.imgX1 || cy < g.imgY0 || cy >= g.imgY1 )
		return 0;

	const double x0 = g.srcX0 + pel * g.srcDX, x1 = g.srcX0 + ( pel + 1 ) * g.srcDX;
	const double y0 = g.srcY0 + line * g.srcDY, y1 = g.srcY0 + ( line + 1 ) * g.srcDY;
	for( double e : { x0, x1, y0, y1 } )
		if( nearInteger( e - 0.5 ) )
			ambiguous = true;
	const int px0 = std::clamp( static_cast< int >( std::ceil( x0 - 0.5 ) ), 0, W );
	const int px1 = std::clamp( static_cast< int >( std::ceil( x1 - 0.5 ) ), 0, W );
	const int py0 = std::clamp( static_cast< int >( std::ceil( y0 - 0.5 ) ), 0, H );
	const int py1 = std::clamp( static_cast< int >( std::ceil( y1 - 0.5 ) ), 0, H );

	auto luma = [ & ]( int x, int y ) {
		const unsigned char* p = src.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
		const double a         = p[ 3 ] / 255.0;
		return ( 0.2126 * p[ 0 ] / 255.0 + 0.7152 * p[ 1 ] / 255.0 + 0.0722 * p[ 2 ] / 255.0 ) * a + ( 1.0 - a );
	};

	double v = 0.0;
	if( px1 > px0 && py1 > py0 )
	{
		const int sx = std::max( 1, ( px1 - px0 + 7 ) / 8 );
		const int sy = std::max( 1, ( py1 - py0 + 7 ) / 8 );
		double sum = 0.0, n = 0.0;
		for( int y = py0; y < py1; y += sy )
			for( int x = px0; x < px1; x += sx )
			{
				sum += luma( x, y );
				n += 1.0;
			}
		v = sum / n;
	}
	else
	{
		const double fx = g.srcX0 + cx * g.srcDX, fy = g.srcY0 + cy * g.srcDY;
		if( nearInteger( fx ) || nearInteger( fy ) )
			ambiguous = true;
		const int px = std::clamp( static_cast< int >( std::floor( fx ) ), 0, W - 1 );
		const int py = std::clamp( static_cast< int >( std::floor( fy ) ), 0, H - 1 );
		v            = luma( px, py );
	}

	double t = threshold;
	if( halftone )
		t += ( bayer[ ( line & 7 ) * 8 + ( pel & 7 ) ] + 0.5 ) / 64.0 - 0.5;
	if( std::abs( v - t ) < 1e-4 )
		ambiguous = true;
	return v < t ? 1 : 0;
}

int runScan( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	if( !quiet )
		std::printf( "== scan at %dx%d: the scanned page is the source thresholded, recomputed in double\n", width, height );

	struct Case
	{
		int fit, resolution;
		float threshold;
		int halftone;
		bool alphaHole;
	};
	const Case cases[] = {
		{ layout::kFitFrame, layout::kStandard, 0.5f, 0, false },   { layout::kFitFrame, layout::kFine, 0.3f, 0, true },
		{ layout::kFitFrame, layout::kStandard, 0.7f, 1, false },   { layout::kFitA4Crop, layout::kStandard, 0.5f, 0, false },
		{ layout::kFitA4Letterbox, layout::kFine, 0.5f, 1, true },  { layout::kFitFrame, layout::kSuperfine, 0.5f, 1, false },
	};
	for( const Case& c : cases )
	{
		const Image card = buildCard( width, height, 3, c.alphaHole );
		Session s;
		if( !prepare( s,
		              { { "Fit", static_cast< float >( c.fit ) }, { "Resolution", static_cast< float >( c.resolution ) },
		                { "Threshold", c.threshold }, { "Halftone", static_cast< float >( c.halftone ) } },
		              perturb, width, height ) )
			return failures + 1;
		s.render( 0, card );
		const codec::Page& page    = s.plugin.ScannedForTest();
		const layout::Geometry& g  = s.plugin.GeometryForTest();
		long compared = 0, skipped = 0, wrong = 0, black = 0, transparentBlack = 0;
		for( int y = 0; y < page.lines; ++y )
			for( int x = 0; x < codec::kWidth; ++x )
			{
				bool ambiguous = false;
				const int want = cpuPel( card, width, height, g, c.threshold, c.halftone != 0, x, y, ambiguous );
				const int got  = page.Pel( y, x );
				black += got;
				if( ambiguous )
				{
					++skipped;
					continue;
				}
				++compared;
				if( want != got )
					++wrong;
			}
		(void)transparentBlack;
		const bool ok = wrong == 0 && compared > ( compared + skipped ) * 9 / 10;
		if( !ok )
			++failures;
		if( !quiet )
			std::printf( "   %-12s %-9s threshold %.1f %s%s: %d lines, %ld pels compared, %ld left out as float-close, %ld wrong, %.1f%% black  %s\n",
			             layout::FitName( c.fit ), layout::ResolutionName( c.resolution ), c.threshold, c.halftone ? "halftone" : "",
			             c.alphaHole ? " alpha hole" : "", page.lines, compared, skipped, wrong,
			             100.0 * static_cast< double >( black ) / ( static_cast< double >( page.lines ) * codec::kWidth ), verdict( ok ) );
		s.end();
	}
	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "scan: every pel is the source's, thresholded as stated" : "scan: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --print
//
// Every output pixel against the page it was given (the receiver's display
// page), recomputed in double: the exact area of the pixel's footprint that
// black pels cover, and paper, toner and desk mixed by area. Within one 8-bit
// step per channel: the shader sums at most a few hundred float32 products
// (error ~1e-5 of a pixel, far under half a step), so the only honest
// disagreement is a rounding tie, which is one step. Alpha must be exactly
// 255 everywhere -- paper is opaque -- even over a source whose right third is
// transparent.
//
// Negative control: the nearest pel instead of the coverage must fail.
//---------------------------------------------------------------------------
int runPrint( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	if( !quiet )
		std::printf( "== print at %dx%d: every pixel is the page's pel coverage, alpha 1\n", width, height );

	struct Case
	{
		int fit, resolution, paper, halftone;
	};
	const Case cases[] = {
		{ layout::kFitFrame, layout::kStandard, controls::kPaperPlain, 0 },
		{ layout::kFitFrame, layout::kSuperfine, controls::kPaperThermal, 1 },
		{ layout::kFitA4Crop, layout::kFine, controls::kPaperThermal, 0 },
		{ layout::kFitA4Letterbox, layout::kStandard, controls::kPaperPlain, 1 },
	};
	for( const Case& c : cases )
	{
		const Image card = buildCard( width, height, 5, true );
		Session s;
		if( !prepare( s,
		              { { "Fit", static_cast< float >( c.fit ) }, { "Resolution", static_cast< float >( c.resolution ) },
		                { "Paper", static_cast< float >( c.paper ) }, { "Halftone", static_cast< float >( c.halftone ) } },
		              perturb, width, height ) )
			return failures + 1;
		s.render( 0, card );
		const Image out             = s.readBack();
		const codec::Page& page     = s.plugin.DisplayForTest();
		const layout::Geometry g    = placed( s, c.fit );
		const controls::Colours col = controls::PaperColours( c.paper );
		long wrong = 0, alphaWrong = 0, grey = 0, worst = 0;
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width; ++x )
			{
				const double kx = codec::kWidth / g.sheetW, ky = page.lines / g.sheetH;
				const double u0 = ( x - g.sheetX ) * kx, u1 = ( x + 1 - g.sheetX ) * kx;
				const double v0 = ( y - g.sheetY ) * ky, v1 = ( y + 1 - g.sheetY ) * ky;
				const double area = ( u1 - u0 ) * ( v1 - v0 );
				const double cu0 = std::clamp( u0, 0.0, 1728.0 ), cu1 = std::clamp( u1, 0.0, 1728.0 );
				const double cv0 = std::clamp( v0, 0.0, static_cast< double >( page.lines ) );
				const double cv1 = std::clamp( v1, 0.0, static_cast< double >( page.lines ) );
				const double inPage = std::max( cu1 - cu0, 0.0 ) * std::max( cv1 - cv0, 0.0 ) / area;
				double black        = 0.0;
				if( inPage > 0.0 )
				{
					for( int j = static_cast< int >( std::floor( cv0 ) ); j < std::min( static_cast< int >( std::ceil( cv1 ) ), page.lines ); ++j )
					{
						const double wy = std::min( cv1, j + 1.0 ) - std::max( cv0, static_cast< double >( j ) );
						for( int i = static_cast< int >( std::floor( cu0 ) ); i < std::min( static_cast< int >( std::ceil( cu1 ) ), 1728 ); ++i )
							if( page.Pel( j, i ) )
								black += ( std::min( cu1, i + 1.0 ) - std::max( cu0, static_cast< double >( i ) ) ) * wy;
					}
					black /= area;
				}
				if( black > 1e-6 && black < inPage - 1e-6 )
					++grey;
				const unsigned char* p = out.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
				for( int ch = 0; ch < 3; ++ch )
				{
					const double v = controls::kDesk[ ch ] * ( 1.0 - inPage ) + col.paper[ ch ] * ( inPage - black ) + col.ink[ ch ] * black;
					const long want = std::lround( std::clamp( v, 0.0, 1.0 ) * 255.0 );
					const long diff = std::labs( want - static_cast< long >( p[ ch ] ) );
					worst           = std::max( worst, diff );
					if( diff > 1 )
					{
						++wrong;
						break;
					}
				}
				if( p[ 3 ] != 255 )
					++alphaWrong;
			}
		const bool ok = wrong == 0 && alphaWrong == 0;
		if( !ok )
			++failures;
		if( !quiet )
			std::printf( "   %-12s %-9s %-7s %s: %ld pixels, %ld partly covered, %ld off by more than one step (worst %ld), %ld not opaque  %s\n",
			             layout::FitName( c.fit ), layout::ResolutionName( c.resolution ), controls::PaperName( c.paper ),
			             c.halftone ? "halftone " : "threshold", static_cast< long >( width ) * height, grey, wrong, worst, alphaWrong,
			             verdict( ok ) );
		s.end();
	}
	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "print: the picture is the page, by area; paper is opaque" : "print: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// Forced single-bit errors against a clean page, shared by --streak and
// --wedge. Live mode, a clean line, the card: each frame is a whole new page
// with one forced bit flipped, compared with the clean page and the clean
// picture.
//---------------------------------------------------------------------------
struct ErrorOutcome
{
	int line;
	size_t offset;
	std::vector< int > damaged;///< lines of the page that differ from the clean page
	int firstPel;              ///< first differing pel on the error's line, -1 if none
	int pelAtError;            ///< where the decoder stood when the code with the error began
	int outsideChanged;        ///< pixels changed by > 1 step in rows touching no damaged line
	int insideChanged;         ///< pixels changed by > 1 step in rows touching the damage
	bool flagged;              ///< the decoder flagged the error's line bad
};

struct ErrorRun
{
	int lines = 0;
	int tried = 0;
	int skipped = 0;///< errors that would forge an EOL
	std::vector< ErrorOutcome > outcomes;
};

bool runErrors( int width, int height, const Settings& settings, int perturb, const std::vector< int >& lines, int perClass,
                ErrorRun& run )
{
	Session s;
	if( !prepare( s, settings, perturb, width, height ) )
		return false;
	const Image card = buildCard( width, height, 11 );
	s.render( 0, card );
	const codec::Page clean                  = s.plugin.DecodedForTest().page;
	const codec::Bits cleanBits              = s.plugin.SentForTest();
	const std::vector< codec::LineRecord > r = s.plugin.ReceivedForTest().lines;
	const Image cleanImage                   = s.readBack();
	const layout::Geometry g                 = placed( s, layout::kFitFrame );
	const bool mr                            = s.plugin.GetFloatParameter( Fax::PT_CODING ) > 0.5f;
	run.lines                                = clean.lines;

	int frame = 1;
	for( int line : lines )
	{
		if( line < 0 || line >= clean.lines )
			continue;
		const codec::LineRecord& rec = r[ static_cast< size_t >( line ) ];
		//The decoder's position at every bit of the clean line.
		std::vector< int > pelAtBit;
		std::vector< uint8_t > scratch( codec::kLineBytes );
		codec::DecodeLine( cleanBits, rec.dataStart, rec.dataStart + rec.dataBits + rec.fillBits, mr && !rec.oneD,
		                   line > 0 ? clean.Row( line - 1 ) : nullptr, scratch.data(), &pelAtBit );
		for( int k = 0; k < perClass; ++k )
		{
			const size_t offset = ( static_cast< size_t >( 2 * k + 1 ) * rec.dataBits ) / static_cast< size_t >( 2 * perClass );
			++run.tried;
			if( forgesOrBreaksEol( cleanBits, rec.dataStart + offset ) )
			{
				++run.skipped;
				continue;
			}
			s.plugin.SetForcedErrorsForTest( { { line, offset } } );
			s.render( frame++, card );
			const codec::Page& got = s.plugin.DecodedForTest().page;
			ErrorOutcome o;
			o.line       = line;
			o.offset     = offset;
			o.damaged    = differingLines( clean, got );
			o.firstPel   = -1;
			o.pelAtError = pelAtBit.empty() ? 0 : pelAtBit[ offset ];
			o.flagged    = s.plugin.DecodedForTest().bad[ static_cast< size_t >( line ) ] != 0;
			for( int x = 0; x < codec::kWidth; ++x )
				if( clean.Pel( line, x ) != got.Pel( line, x ) )
				{
					o.firstPel = x;
					break;
				}
			const Image image = s.readBack();
			o.outsideChanged = o.insideChanged = 0;
			const std::set< int > damage( o.damaged.begin(), o.damaged.end() );
			for( int y = 0; y < height; ++y )
			{
				int first, last;
				linesOfRow( g, y, first, last );
				bool touches = false;
				for( int l = first; l <= last; ++l )
					touches = touches || damage.count( l ) != 0;
				const int changed = rowDifferences( cleanImage, image, width, y, 1 );
				( touches ? o.insideChanged : o.outsideChanged ) += changed;
			}
			run.outcomes.push_back( o );
		}
	}
	s.plugin.SetForcedErrorsForTest( {} );
	s.end();
	return true;
}

//---------------------------------------------------------------------------
// --streak
//
// MH, concealment off. One bit flipped in a line's data (48 places: 8 along
// each of 6 lines) must damage exactly that line of the received page, and
// no pel of it before where the decoder stood when it began the code that
// held the error; in the picture, no pixel whose footprint touches no damaged
// line may move by more than one 8-bit step (the software renderer is not
// repeatable at the last bit), and the damaged rows must show it. Flips that
// would forge an EOL -- a 1 between two runs of zeros totalling ten -- are
// left out and counted: they split the line, which is a different claim.
//
// Negative control: a decoder that never resynchronises on EOL must fail.
//---------------------------------------------------------------------------
int runStreak( int width, int height, int perturb = 0, bool quiet = false )
{
	if( !quiet )
		std::printf( "== streak at %dx%d: one bit error in MH damages its line from the error on, and nothing else\n", width, height );
	ErrorRun run;
	const Settings settings = { { "Coding", static_cast< float >( codec::kMH ) } };
	//The page has 466 lines at 16:9 standard; six lines spread down it.
	if( !runErrors( width, height, settings, perturb, { 4, 93, 186, 233, 330, 460 }, 8, run ) )
		return 1;

	int contained = 0, damagedOwn = 0, fromError = 0, felt = 0, flagged = 0, outside = 0, visible = 0;
	for( const ErrorOutcome& o : run.outcomes )
	{
		const bool only  = o.damaged.empty() || ( o.damaged.size() == 1 && o.damaged[ 0 ] == o.line );
		const bool after = o.damaged.empty() || o.firstPel >= o.pelAtError;
		contained += only;
		damagedOwn += !o.damaged.empty() && only;
		fromError += after;
		felt += !o.damaged.empty() || o.flagged;
		flagged += o.flagged;
		outside += o.outsideChanged;
		visible += o.insideChanged > 0;
		if( !quiet && ( !only || !after || o.outsideChanged > 0 ) )
			std::printf( "      line %d bit %zu: %zu lines damaged (first %d, last %d), first pel %d, decoder at %d, %d pixels outside\n",
			             o.line, o.offset, o.damaged.size(), o.damaged.empty() ? -1 : o.damaged.front(),
			             o.damaged.empty() ? -1 : o.damaged.back(), o.firstPel, o.pelAtError, o.outsideChanged );
	}
	const int n   = static_cast< int >( run.outcomes.size() );
	const bool ok = n >= 30 && contained == n && fromError == n && felt == n && outside == 0 && damagedOwn > 0;
	if( !quiet )
	{
		std::printf( "   %d errors tried on a %d-line page, %d left out as EOL forgeries, %d measured\n", run.tried, run.lines, run.skipped, n );
		std::printf( "   no line but its own damaged: %d of %d; no pel damaged before the decoder's position at the error: %d of %d\n",
		             contained, n, fromError, n );
		std::printf( "   every error felt (its line damaged or flagged bad): %d of %d; damaged %d, the rest a streak in the line's own colour, flagged %d\n",
		             felt, n, damagedOwn, flagged );
		std::printf( "   pixels moved by more than a step outside the damaged line's rows: %d; streaks visible in the picture: %d  %s\n",
		             outside, visible, verdict( ok ) );
		std::printf( "%s\n", ok ? "streak: an error runs to the end of its line and stops at the EOL" : "streak: FAILURES" );
	}
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// --wedge
//
// MR, concealment off, at standard (K = 2), fine (K = 4) and superfine
// (K = 8). One bit flipped in a line must damage that line and only lines
// below it before the next one-dimensional line: the damage is at most K
// tall, and an error in a one-dimensional line is measured for how tall it
// actually runs. The picture outside the damaged rows may not move by more
// than one step. Then the same with Repeat Line, which conceals the lines
// that follow instead of decoding them against a wrong reference: still
// contained.
//
// Negative control: MR with no one-dimensional refresh must fail.
//---------------------------------------------------------------------------
int runWedge( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	if( !quiet )
		std::printf( "== wedge at %dx%d: one bit error in MR runs down only until the next 1-D line\n", width, height );
	for( int resolution : { 0, 1, 2 } )
		for( int conceal : { 0, 1 } )
		{
			const int k = layout::K( resolution );
			Settings settings = { { "Coding", static_cast< float >( codec::kMR ) }, { "Resolution", static_cast< float >( resolution ) },
				                  { "Concealment", static_cast< float >( conceal ) } };
			//Lines at the start of a K-group (1-D) and in the middle of one.
			const int pageLines = layout::Compute( layout::kFitFrame, resolution, width, height ).lines;
			std::vector< int > lines;
			for( int f : { 1, 3, 5, 7 } )
			{
				const int group = ( pageLines * f / 9 ) / k * k;
				lines.push_back( group );
				lines.push_back( group + k / 2 );
			}
			ErrorRun run;
			if( !runErrors( width, height, settings, perturb, lines, 4, run ) )
				return failures + 1;
			int contained = 0, oneD = 0, exactlyK = 0, tallest = 0, outside = 0, empty = 0;
			std::map< int, int > heights;
			for( const ErrorOutcome& o : run.outcomes )
			{
				const int next = ( o.line / k + 1 ) * k;
				bool inside    = true;
				for( int d : o.damaged )
					inside = inside && d >= o.line && d < next;
				if( o.damaged.empty() )
					++empty;
				contained += inside;
				outside += o.outsideChanged;
				const int tall = o.damaged.empty() ? 0 : o.damaged.back() - o.line + 1;
				tallest        = std::max( tallest, tall );
				if( o.line % k == 0 )
				{
					++oneD;
					++heights[ tall ];
					exactlyK += tall == k;
				}
				if( !quiet && !inside )
					std::printf( "      line %d bit %zu: damage %d..%d, next 1-D line %d\n", o.line, o.offset,
					             o.damaged.empty() ? -1 : o.damaged.front(), o.damaged.empty() ? -1 : o.damaged.back(), next );
			}
			const int n   = static_cast< int >( run.outcomes.size() );
			const bool ok = n >= 16 && contained == n && outside == 0 && tallest <= k;
			if( !ok )
				++failures;
			if( !quiet )
			{
				std::string hist;
				for( const auto& h : heights )
					hist += " " + std::to_string( h.first ) + ":" + std::to_string( h.second );
				std::printf( "   K = %d (%s), %s: %d errors (%d skipped as EOL forgeries), %d contained before the next 1-D line, tallest %d; "
				             "from a 1-D line the damage ran exactly K in %d of %d (heights%s); %d pixels outside  %s\n",
				             k, layout::ResolutionName( resolution ), conceal ? "Repeat Line" : "concealment off", n, run.skipped,
				             contained, tallest, exactlyK, oneD, hist.c_str(), outside, verdict( ok ) );
			}
		}
	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "wedge: MR damage is a wedge at most K lines tall" : "wedge: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --conceal
//
// Repeat Line, on a noisy line (Line Noise 0.45, a bit error rate of 6.3e-5,
// and a forced error), MH and MR, on the card in Halftone -- where the Bayer
// pattern makes neighbouring lines differ, so a copy of the wrong line cannot
// pass for the right one: every line the decoder concealed must equal the
// line above it on the received page, bit for bit (line 0 against blank
// paper), and there must be some. On the same page with concealment Off, at
// least one bad line must NOT equal the line above -- or the check would pass
// on a decoder that conceals nothing.
//
// Negative control: concealing with the line two above must fail.
//---------------------------------------------------------------------------
int runConceal( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	if( !quiet )
		std::printf( "== conceal at %dx%d: with Repeat Line a bad line is the line above, bit for bit\n", width, height );
	const Image card = buildCard( width, height, 13 );
	for( int coding = 0; coding < 2; ++coding )
	{
		int concealed = 0, equal = 0, badOff = 0, differOff = 0, lines = 0;
		for( int conceal : { 1, 0 } )
		{
			Session s;
			if( !prepare( s,
			              { { "Coding", static_cast< float >( coding ) }, { "Line Noise", 0.45f }, { "Halftone", 1.0f },
			                { "Concealment", static_cast< float >( conceal ) } },
			              perturb, width, height ) )
				return failures + 1;
			s.plugin.SetForcedErrorsForTest( { { 200, 5 } } );
			s.render( 0, card );
			const codec::Decoded& d = s.plugin.DecodedForTest();
			const codec::Page& page = s.plugin.DisplayForTest();
			static const uint8_t white[ codec::kLineBytes ] = {};
			lines = page.lines;
			for( int i = 0; i < page.lines; ++i )
			{
				const uint8_t* above = i > 0 ? page.Row( i - 1 ) : white;
				const bool same      = std::memcmp( page.Row( i ), above, codec::kLineBytes ) == 0;
				if( conceal && d.shown[ static_cast< size_t >( i ) ] == codec::kShownConcealed )
				{
					++concealed;
					equal += same;
				}
				if( !conceal && d.bad[ static_cast< size_t >( i ) ] )
				{
					++badOff;
					differOff += !same;
				}
			}
			s.end();
		}
		const bool ok = concealed > 0 && equal == concealed && differOff > 0;
		if( !ok )
			++failures;
		if( !quiet )
			std::printf( "   %s: %d lines; Repeat Line concealed %d, %d of them equal to the line above; with Off, %d bad lines, %d unlike the line above  %s\n",
			             controls::CodingName( coding ), lines, concealed, equal, badOff, differOff, verdict( ok ) );
	}
	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "conceal: a bad line is the line above it" : "conceal: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --timing
//
// Page mode on a clean line. The display is first filled with a page no test
// line can equal (one Live frame of mid grey, dithered: a Bayer pattern on
// every line), then Mode goes to Page with the source under test, so every
// arriving line visibly replaces the pattern. The clock
// then steps by 2 ms and after every step the harness reads which lines of
// the displayed page have changed: line i's arrival is bracketed to one step.
//
// Independently, the transmitted stream is parsed for its EOLs: line i's bits
// are the distance between the EOL before it and the EOL after it, and its
// predicted arrival is ( the stream up to and including its EOL and tag ) /
// baud. Every line's bracket must contain its prediction (tolerance: one clock
// step, 2 ms, a fifth of the 10 ms floor), every line's bits must be at least
// the floor ( ceil( 10 ms x baud ) ) and a blank line's exactly the floor, and
// the blank page must finish before the busy one.
//
// Negative controls: no fill (no floor) and a flat per-line time must fail.
//---------------------------------------------------------------------------
int runTiming( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	if( !quiet )
		std::printf( "== timing at %dx%d: each line takes its bits / baud, floored at 10 ms; blank beats busy\n", width, height );

	struct Case
	{
		const char* name;
		int coding, baud;
		bool blank;
	};
	const Case cases[] = {
		{ "blank", codec::kMH, 3, true },
		{ "busy", codec::kMH, 3, false },
		{ "blank", codec::kMR, 2, true },
		{ "busy", codec::kMR, 2, false },
	};
	double pageTime[ 4 ] = {};
	int index            = 0;
	for( const Case& c : cases )
	{
		Session s;
		if( !prepare( s, { { "Coding", static_cast< float >( c.coding ) }, { "Baud", static_cast< float >( c.baud ) } }, perturb, width,
		              height ) )
			return failures + 1;
		const int baud = controls::BaudRate( c.baud );
		const int floorBits = static_cast< int >( std::ceil( controls::kMinScanLineSeconds * baud - 1e-9 ) );
		const bool mr       = c.coding == codec::kMR;

		//First a page no line of the test can equal, in Live: mid grey,
		//dithered, so every line of it is a Bayer pattern.
		set( s.plugin, "Halftone", 1.0f );
		s.render( 0, buildFlat( width, height, 128 ) );
		const codec::Page black = s.plugin.DisplayForTest();
		set( s.plugin, "Halftone", 0.0f );

		//Then the page under test, in Page mode, from t0.
		const double t0 = 1.0;
		set( s.plugin, "Mode", static_cast< float >( controls::kModePage ) );
		s.upload( c.blank ? buildFlat( width, height, 255 ) : buildCard( width, height, 17 ) );
		s.renderAtTime( t0 );
		const codec::Page sent   = s.plugin.DecodedForTest().page;
		const codec::Bits stream = s.plugin.SentForTest();
		const int L              = sent.lines;

		//Every line must be distinguishable from the old page to be seen arriving.
		int unseeable = 0;
		for( int i = 0; i < L; ++i )
			unseeable += std::memcmp( sent.Row( i ), black.Row( i ), codec::kLineBytes ) == 0;

		//Predictions from the stream alone.
		const std::vector< size_t > eols = eolsOf( stream );
		std::vector< double > predicted( static_cast< size_t >( L ), -1.0 );
		std::vector< long > bits( static_cast< size_t >( L ), 0 );
		for( int i = 0; i < L && static_cast< size_t >( i + 1 ) < eols.size(); ++i )
		{
			bits[ static_cast< size_t >( i ) ]      = static_cast< long >( eols[ static_cast< size_t >( i + 1 ) ] - eols[ static_cast< size_t >( i ) ] );
			predicted[ static_cast< size_t >( i ) ] = static_cast< double >( eols[ static_cast< size_t >( i + 1 ) ] + 1 + ( mr ? 1 : 0 ) ) / baud;
		}

		//Step the clock and watch the page arrive.
		constexpr double kStep = 0.002;
		std::vector< double > arrivedBy( static_cast< size_t >( L ), -1.0 ), arrivedAfter( static_cast< size_t >( L ), -1.0 );
		int seen          = 0;
		double previous   = 0.0;
		const double stop = s.plugin.TimingForTest().duration - 1e-9;
		for( long k = 1; seen < L; ++k )
		{
			const double t = static_cast< double >( k ) * kStep;
			if( t > stop + 1.0 )
				break;
			s.renderAtTime( t0 + t );
			const codec::Page& shown = s.plugin.DisplayForTest();
			while( seen < L && std::memcmp( shown.Row( seen ), black.Row( seen ), codec::kLineBytes ) != 0 )
			{
				arrivedAfter[ static_cast< size_t >( seen ) ] = previous;
				arrivedBy[ static_cast< size_t >( seen ) ]    = t;
				++seen;
			}
			//A line identical to the old page cannot be seen (there are none:
			//`unseeable` must be 0); step past it with the next one that can.
			while( seen < L && std::memcmp( sent.Row( seen ), black.Row( seen ), codec::kLineBytes ) == 0 && s.plugin.ArrivedForTest() > seen )
				++seen;
			previous = t;
		}

		int outside = 0, underFloor = 0, notFloored = 0, aboveFloor = 0;
		double worst = 0.0;
		for( int i = 0; i < L; ++i )
		{
			const size_t u = static_cast< size_t >( i );
			if( bits[ u ] < floorBits )
				++underFloor;
			if( c.blank && bits[ u ] != floorBits )
				++notFloored;
			aboveFloor += bits[ u ] > floorBits;
			if( arrivedBy[ u ] < 0.0 )
				continue;
			const double p = predicted[ u ];
			if( !( p > arrivedAfter[ u ] - 1e-9 && p <= arrivedBy[ u ] + 1e-9 ) )
			{
				++outside;
				worst = std::max( worst, std::max( arrivedAfter[ u ] - p, p - arrivedBy[ u ] ) );
			}
		}
		pageTime[ index++ ] = arrivedBy[ static_cast< size_t >( L - 1 ) ];
		//A busy page must hold lines past the floor, or it tests only the floor.
		const bool ok = outside == 0 && underFloor == 0 && notFloored == 0 && unseeable == 0 && ( c.blank || aboveFloor > 0 );
		if( !ok )
			++failures;
		if( !quiet )
		{
			long total = 0;
			for( long b : bits )
				total += b;
			std::printf( "   %-5s %s at %d: %d lines, %ld bits, the last line in by %.3f s (predicted %.4f); "
			             "%d arrivals outside their 2 ms bracket (worst %.4f s), %d lines under the %d-bit floor, %d above it%s  %s\n",
			             c.name, controls::CodingName( c.coding ), baud, L, total, arrivedBy[ static_cast< size_t >( L - 1 ) ],
			             predicted[ static_cast< size_t >( L - 1 ) ], outside, worst, underFloor, floorBits, aboveFloor,
			             c.blank ? ( notFloored == 0 ? ", every line exactly the floor" : ", NOT every line the floor" ) : "",
			             verdict( ok ) );
		}
		s.end();
	}
	for( int i = 0; i < 4; i += 2 )
	{
		const bool ok = pageTime[ i ] > 0.0 && pageTime[ i ] < pageTime[ i + 1 ];
		if( !ok )
			++failures;
		if( !quiet )
			std::printf( "   %s: the blank page took %.3f s, the busy page %.3f s  %s\n", controls::CodingName( cases[ i ].coding ),
			             pageTime[ i ], pageTime[ i + 1 ], verdict( ok ) );
	}
	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "timing: the page arrives at the line's rate, line by line" : "timing: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --resize
//
// Page mode on the default (noisy) line: a run that resizes the output to
// 1.5W+1 x 0.75H+1 (another aspect) a third of the way through a page must
// keep the SAME page -- the same page count, the same lines arrived at every
// frame, the same received page, byte for byte -- as a run that never
// resizes. The page memory is on the CPU and is never reallocated by a
// resize (photofinish's trap: a reallocation that clears the buffer holding
// the previous frame). The picture after the resize is judged by the print
// comparator at the new raster.
//---------------------------------------------------------------------------
int runResize( int width, int height, int perturb = 0, bool quiet = false )
{
	if( !quiet )
		std::printf( "== resize at %dx%d: a resize mid-page changes nothing on the page\n", width, height );
	const int w2 = width * 3 / 2 + 1, h2 = height * 3 / 4 + 1;
	Session a, b;
	const Settings settings = { { "Mode", static_cast< float >( controls::kModePage ) }, { "Line Noise", 0.35f },
		                        { "Coding", static_cast< float >( codec::kMR ) }, { "Header", 1.0f } };
	if( !prepare( a, settings, perturb, width, height ) || !prepare( b, settings, perturb, width, height ) )
		return 1;
	const Image card = buildCard( width, height, 2 );
	int mismatches = 0, frames = 0;
	for( int f = 0; f < 120; ++f )
	{
		if( f == 40 )
			b.resize( w2, h2 );
		a.render( f, card );
		b.render( f, f >= 40 ? buildCard( w2, h2, 2 ) : card );
		++frames;
		if( a.plugin.PagesForTest() != b.plugin.PagesForTest() || a.plugin.ArrivedForTest() != b.plugin.ArrivedForTest()
		    || a.plugin.DisplayForTest().bytes != b.plugin.DisplayForTest().bytes )
			++mismatches;
	}
	const int arrived = b.plugin.ArrivedForTest();
	const int lines   = b.plugin.DisplayForTest().lines;
	a.end();
	b.end();
	const bool ok = mismatches == 0 && arrived > 0;
	if( !quiet )
		std::printf( "   %d frames, resized to %dx%d at frame 40: %d frames where the page differs from an unresized run; %d of %d lines in at the end  %s\n",
		             frames, w2, h2, mismatches, arrived, lines, verdict( ok ) );
	int failures = ok ? 0 : 1;
	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "resize: the page in flight survives a resize" : "resize: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --negative
//
// A check that cannot fail is not a check. Each of these perturbs the MODEL
// -- through a hook the shipped plugin carries at zero -- and asserts that the
// check catches it.
//---------------------------------------------------------------------------
int runNegative( int width, int height, const std::string& fixture )
{
	struct Control
	{
		const char* name;
		int failuresSeen;
	};
	const Control controls[] = {
		{ "tables with white 5 and white 8 exchanged      ", runTables( fixture, codec::kPerturbTableSwap, true ) },
		{ "roundtrip with VR1 written as VR2               ", runRoundtrip( width, height, codec::kPerturbVerticalSkew, true ) },
		{ "scan with BT.601 luma weights                   ", runScan( width, height, codec::kPerturbScanBt601, true ) },
		{ "print with the nearest pel, not its coverage    ", runPrint( width, height, codec::kPerturbPrintNearest, true ) },
		{ "streak with no resynchronisation on EOL         ", runStreak( width, height, codec::kPerturbNoResync, true ) },
		{ "wedge with no one-dimensional refresh           ", runWedge( width, height, codec::kPerturbNoRefresh, true ) },
		{ "conceal with the line two above                 ", runConceal( width, height, codec::kPerturbConcealWrong, true ) },
		{ "timing with no fill (no minimum scan time)      ", runTiming( width, height, codec::kPerturbNoFill, true ) },
		{ "timing with every line the page's mean time     ", runTiming( width, height, codec::kPerturbFlatTiming, true ) },
	};

	int failures = 0;
	for( const Control& c : controls )
	{
		const bool ok = c.failuresSeen > 0;
		std::printf( "negative %s: %s  %s\n", c.name, ok ? "it failed" : "it PASSED", verdict( ok ) );
		if( !ok )
			++failures;
	}
	std::printf( "%s\n", failures == 0 ? "negative: every perturbed model is caught" : "negative: FAILURES -- a check cannot fail" );
	return failures;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
struct BenchResult
{
	double ms = -1.0;
	Fax::CpuCost cpu;
	int pages = 0;
};

BenchResult benchAt( const Settings& settings, int width, int height, int frames )
{
	BenchResult result;
	Session session;
	for( const auto& s : settings )
		set( session.plugin, s.first, s.second );
	if( !session.begin( width, height ) )
		return result;

	//Eight frames of the card uploaded ONCE and cycled by handle, the way a
	//host hands over a texture it already has: the upload is not in the figure.
	constexpr int kCards = 8;
	GLuint textures[ kCards ];
	for( int i = 0; i < kCards; ++i )
	{
		const Image flipped = flipRows( buildCard( width, height, i * 5 ), width, height );
		textures[ i ]       = makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, flipped.data() );
	}
	auto renderCard = [ & ]( int frame ) {
		session.inputStruct.Handle = textures[ frame % kCards ];
		session.renderAt( frame );
	};

	const int warmup = 10;
	for( int frame = 0; frame < warmup; ++frame )
		renderCard( frame );
	glFinish();

	//The best of three runs: this machine's GPU and CPU are shared with other
	//builds, and the minimum is the run nothing else interrupted.
	double best = 1e9;
	for( int run = 0; run < 3; ++run )
	{
		const int pagesBefore = session.plugin.PagesForTest();
		Fax::CpuCost sum;
		const auto start = std::chrono::steady_clock::now();
		for( int frame = 0; frame < frames; ++frame )
		{
			const int before = session.plugin.PagesForTest();
			renderCard( warmup + run * frames + frame );
			if( session.plugin.PagesForTest() != before )
			{
				const Fax::CpuCost c = session.plugin.LastCpuCostForTest();
				sum.readback += c.readback;
				sum.code += c.code;
				sum.channel += c.channel;
				sum.decode += c.decode;
			}
		}
		glFinish();
		const double seconds = std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count();
		const double ms      = seconds * 1000.0 / static_cast< double >( frames );
		if( ms < best )
		{
			best          = ms;
			result.pages  = session.plugin.PagesForTest() - pagesBefore;
			const double n = std::max( 1, result.pages );
			result.cpu    = { sum.readback / n, sum.code / n, sum.channel / n, sum.decode / n };
		}
	}

	session.inputStruct.Handle = session.sourceTexture;
	glDeleteTextures( kCards, textures );
	session.end();
	result.ms = best;
	return result;
}

int runBench( int frames )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "3840x2160 ", 3840, 2160 },
	};
	std::printf( "%d frames each, best of three runs, after a 10-frame warm-up, glFinish both sides.\n", frames );
	std::printf( "Per page: the scan read-back, the sender (header + T.4), the line, the receiver, in ms.\n\n" );
	std::printf( "mode                      resolution   ms/frame  %% of 60fps   pages   readback   code   line   decode\n" );

	struct Mode
	{
		const char* name;
		Settings settings;
	};
	const Mode modes[] = {
		{ "Page (defaults)         ", {} },
		{ "Live, standard          ", { { "Mode", 1.0f } } },
		{ "Live, superfine halftone", { { "Mode", 1.0f }, { "Resolution", 2.0f }, { "Halftone", 1.0f } } },
	};
	for( const Mode& m : modes )
		for( const Size& size : sizes )
		{
			const BenchResult r = benchAt( m.settings, size.width, size.height, frames );
			std::printf( "%s  %s  %8.3f    %5.1f%%   %5d   %8.3f %6.3f %6.3f %8.3f\n", m.name, size.name, r.ms, r.ms / 16.667 * 100.0,
			             r.pages, r.cpu.readback, r.cpu.code, r.cpu.channel, r.cpu.decode );
		}
	std::printf( "\nPage mode starts a page only when the last has arrived, so its CPU half runs\n"
	             "once every few seconds and the frame figure is almost all print pass; Live\n"
	             "runs the whole CPU half every frame.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --export DIR: the test card's page, scanned by the plugin, coded as a G3
// MH stream (no fill, an EOL before every line, RTC) and as a G4 stream
// (T.6: every line two-dimensional against the one above, no EOLs, EOFB),
// each packed MSB first, with the page as a PBM. tools/gocheck decodes both
// with golang.org/x/image/ccitt, which shares no code with this repo.
// `--perturb 32` (VR1 written as VR2) exports a wrong G4 stream, which
// gocheck must reject: verify.sh runs both.
//---------------------------------------------------------------------------
std::vector< unsigned char > packMsbFirst( const codec::Bits& bits )
{
	std::vector< unsigned char > out( ( bits.size() + 7 ) / 8, 0 );
	for( size_t i = 0; i < bits.size(); ++i )
		if( bits[ i ] )
			out[ i / 8 ] = static_cast< unsigned char >( out[ i / 8 ] | ( 0x80 >> ( i % 8 ) ) );
	return out;
}

int runExport( const std::string& dir, int width, int height, int perturb )
{
	CGLContextObj ctx = CGLGetCurrentContext();
	(void)ctx;
	int written = 0;
	for( int halftone : { 0, 1 } )
	{
		Session s;
		if( !prepare( s, { { "Halftone", static_cast< float >( halftone ) }, { "Header", 1.0f } }, 0, width, height ) )
			return 1;
		s.render( 0, buildCard( width, height, 9 ) );
		const codec::Page page = s.plugin.ScannedForTest();
		s.end();

		codec::Transmission g3;
		codec::EncodeOptions eo;
		eo.coding  = codec::kMH;
		eo.perturb = perturb;
		codec::Encode( page, eo, g3 );

		codec::Bits g4;
		for( int y = 0; y < page.lines; ++y )
			codec::EncodeLine2D( page.Row( y ), y > 0 ? page.Row( y - 1 ) : nullptr, g4, perturb );
		codec::PutBits( g4, t4::kEol );
		codec::PutBits( g4, t4::kEol );

		const std::string stem = dir + "/card" + std::to_string( halftone );
		auto save              = [ & ]( const std::string& path, const std::vector< unsigned char >& data, const std::string& header ) {
			std::ofstream out( path, std::ios::binary );
			out << header;
			out.write( reinterpret_cast< const char* >( data.data() ), static_cast< std::streamsize >( data.size() ) );
			return static_cast< bool >( out );
		};
		std::vector< unsigned char > pbm;
		for( int y = 0; y < page.lines; ++y )
			for( int b = 0; b < codec::kLineBytes; ++b )
			{
				unsigned char v = 0;
				for( int i = 0; i < 8; ++i )
					if( page.Pel( y, b * 8 + i ) )
						v = static_cast< unsigned char >( v | ( 0x80 >> i ) );
				pbm.push_back( v );
			}
		if( !save( stem + ".g3", packMsbFirst( g3.bits ), "" ) || !save( stem + ".g4", packMsbFirst( g4 ), "" )
		    || !save( stem + ".pbm", pbm, "P4\n1728 " + std::to_string( page.lines ) + "\n" ) )
		{
			std::fprintf( stderr, "cannot write %s.*\n", stem.c_str() );
			return 1;
		}
		written += 3;
	}
	std::printf( "wrote %d files to %s\n", written, dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	const std::pair< const char*, std::string > files[] = {
		{ "vertex.vert", shaders::Vertex() },
		{ "scan.frag", shaders::Scan() },
		{ "print.frag", shaders::Print() },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line. Same format as the rest
// of the fleet, so one filming script drives any of them.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;

		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(),
		                  []( const std::pair< int, float >& a, const std::pair< int, float >& b ) { return a.first < b.first; } );
	return tracks;
}

/// The value at `frame`: a slider ramps linearly between cues; a stepped
/// parameter holds the last cue at or before the frame.
float valueAt( const Track& track, int frame, bool stepped )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame < track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			if( stepped )
				return a.second;
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"fxtest -- render and measure the Fax effect\n"
		"\n"
		"  --out PATH          render the test card through the plugin (default /tmp/fax.png)\n"
		"  --size WxH          raster (default 1280x720)\n"
		"  --frames N          frames to render before reading back (default 40)\n"
		"  --fps N             synthetic frame rate driving the clock (default 60)\n"
		"  --source S          card (default), white, black\n"
		"  --set \"Name=V\"      set a parameter by its display name. Repeatable.\n"
		"  --list              every parameter, its kind, default and range\n"
		"  --names             every name at most 16 characters and unique\n"
		"  --tables            the T.4 codes: lengths, prefix-free, no EOL, every run (no GL)\n"
		"  --roundtrip         decode( encode( page ) ) == page, MH and MR\n"
		"  --scan              the scanned page is the source thresholded, in double\n"
		"  --print             every pixel is the page's pel coverage; alpha is 1\n"
		"  --streak            one MH error damages its line from the error on, nothing else\n"
		"  --wedge             one MR error damages lines only until the next 1-D line\n"
		"  --conceal           with Repeat Line a bad line is the line above\n"
		"  --timing            each line takes its bits / baud, floored; blank beats busy\n"
		"  --resize            a resize mid-page changes nothing on the page\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks against a perturbed model (Codec.h), verbosely\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K, with the CPU half's share\n"
		"  --export DIR        G3 / G4 streams and the page, for tools/gocheck\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       cues for --pipe: 'frame Name Value'; sliders ramp, options and booleans step\n"
		"  --fail-render-at N  (--pipe) fail the render of frame N, to test the stream's error path\n"
		"  --fixtures PATH     the code-length fixture (default: the source tree's)\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/fax.png";
	std::string scriptPath;
	std::string sourceName = "card";
	std::string dumpDir, exportDir;
	std::string fixture = std::string( FAX_SOURCE_DIR ) + "/tools/fixtures/t4-code-lengths.txt";
	int width        = 1280;
	int height       = 720;
	int frames       = 40;
	int perturb      = 0;
	int failRenderAt = -1;
	double fps       = 60.0;
	bool wantList    = false;
	bool wantBench   = false;
	bool wantPipe    = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--source" && hasNext )
			sourceName = argv[ ++i ];
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--fail-render-at" && hasNext )
			failRenderAt = std::atoi( argv[ ++i ] );
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--export" && hasNext )
			exportDir = argv[ ++i ];
		else if( argument == "--fixtures" && hasNext )
			fixture = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--names" || argument == "--tables" || argument == "--roundtrip" || argument == "--scan"
		         || argument == "--print" || argument == "--streak" || argument == "--wedge" || argument == "--conceal"
		         || argument == "--timing" || argument == "--resize" || argument == "--negative" )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed, so it is answered before a context is made -- which also
		//means it works on a machine where creating one fails, and in CI.
		Fax plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	//The checks that need no context run without one.
	bool needsGL = false;
	for( const std::string& c : checks )
		needsGL = needsGL || ( c != "--names" && c != "--tables" );
	if( !checks.empty() && !needsGL )
	{
		int failures = 0;
		for( const std::string& c : checks )
		{
			failures += c == "--names" ? runNames() : runTables( fixture, perturb );
			std::printf( "\n" );
		}
		return failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( !exportDir.empty() )
		return finish( runExport( exportDir, width, height, perturb ) );

	if( !checks.empty() )
	{
		int failures = 0;
		for( const std::string& check : checks )
		{
			int result = 0;
			if( check == "--names" )
				result = runNames();
			else if( check == "--tables" )
				result = runTables( fixture, perturb );
			else if( check == "--roundtrip" )
				result = runRoundtrip( width, height, perturb );
			else if( check == "--scan" )
				result = runScan( width, height, perturb );
			else if( check == "--print" )
				result = runPrint( width, height, perturb );
			else if( check == "--streak" )
				result = runStreak( width, height, perturb );
			else if( check == "--wedge" )
				result = runWedge( width, height, perturb );
			else if( check == "--conceal" )
				result = runConceal( width, height, perturb );
			else if( check == "--timing" )
				result = runTiming( width, height, perturb );
			else if( check == "--resize" )
				result = runResize( width, height, perturb );
			else if( check == "--negative" )
				result = runNegative( width, height, fixture );
			failures += result;
			std::printf( "\n" );
		}
		return finish( failures == 0 ? 0 : 1 );
	}

	if( wantBench )
		return finish( runBench( frames ) );

	Session session;
	session.fps = fps;

	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}

	if( !session.begin( width, height ) )
		return finish( 1 );

	if( wantPipe )
	{
		//A reader that hangs up must end the take with exit 1 and a message,
		//not SIGPIPE's silent 141: write() then fails and the loop says so.
		std::signal( SIGPIPE, SIG_IGN );
		struct Automation
		{
			Track track;
			bool stepped;
		};
		std::map< unsigned int, Automation > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation[ static_cast< unsigned int >( index ) ] = { entry.second,
					                                                   stepsBetweenCues( session.plugin, static_cast< unsigned int >( index ) ) };
			}
		}

		Image frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame at the end of a pipe is the end of the stream,
			//not a frame to render: the stream ends cleanly, and only whole
			//frames ever come out.
			if( got < frame.size() )
				break;

			//Through the plugin's own setter, so a cue moves the same thing an
			//operator's slider would.
			for( const auto& a : automation )
				session.plugin.SetFloatParameter( a.first, valueAt( a.second.track, index, a.second.stepped ) );

			if( index == failRenderAt || !session.render( index, frame ) )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const Image out = session.readBack();
			size_t written  = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone. Rendering on into a closed pipe is work
			//nobody will see, and a short frame on stdout is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}

		session.end();
		return finish( status );
	}

	for( int frame = 0; frame < frames; ++frame )
	{
		Image pixels;
		if( sourceName == "white" )
			pixels = buildFlat( width, height, 255 );
		else if( sourceName == "black" )
			pixels = buildFlat( width, height, 0 );
		else
			pixels = buildCard( width, height, frame );
		if( !session.render( frame, pixels ) )
			return finish( 1 );
	}

	const Image image = session.readBack();
	session.end();

	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}

	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
