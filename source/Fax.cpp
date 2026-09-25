#include "Fax.h"

#include "Controls.h"
#include "Diag.h"
#include "Header.h"
#include "Shaders.h"

#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace fax;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Fax >,                                        // Create method
	"FX01",                                                      // Plugin unique ID of maximum length 4.
	"SW Fax",                                                    // Plugin name
	2,                                                           // API major version number
	1,                                                           // API minor version number
	0,                                                           // Plugin major version number
	1,                                                           // Plugin minor version number
	FF_EFFECT,                                                   // Plugin type
	"The picture sent as a Group 3 fax over a noisy line.\n\nEach frame is scanned to 1728 pels a line, thresholded or dithered to black and white, coded with the real ITU-T T.4 run-length codes (MH, or MR against the line above), sent through a line that flips bits, and decoded by a real decoder. A bit error streaks the rest of its line, MR carries it down to the next one-dimensional line, a bad line is repeated from the one above, and a busy page takes longer to arrive than a blank one.",// Plugin description
	"Fax FFGL effect"                                            // About
);

namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Wall clock, for hosts that never call SetTime. Steady rather than system,
/// so nothing here moves when the machine's clock is corrected.
double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

double millisecondsSince( std::chrono::steady_clock::time_point t )
{
	return std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t ).count();
}

/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

constexpr int kGroups = codec::kWidth / 32;///< 54 texels a scan line
} // namespace

//---------------------------------------------------------------------------
Fax::Fax()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The page arrives at the line's rate, and that is a function of the
	//host's clock: a re-render of the same composition must arrive the same.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. The frame as the page, standard resolution, a mid threshold,
	// no halftone; MR at 14400 bit/s with a line that drops a few bits a
	// page, one page at a time; the receiver repeating bad lines, on thermal
	// paper, with the sender's header. See AGENTS.md for how these were
	// chosen against Resolume's demo clips.
	//---------------------------------------------------------------------
	params[ PT_FIT ]        = static_cast< float >( layout::kFitFrame );
	params[ PT_RESOLUTION ] = static_cast< float >( layout::kStandard );
	params[ PT_THRESHOLD ]  = 0.5f;
	params[ PT_HALFTONE ]   = 0.0f;

	params[ PT_CODING ]     = static_cast< float >( codec::kMR );
	params[ PT_LINE_NOISE ] = 0.35f;
	params[ PT_BURSTS ]     = 0.0f;
	params[ PT_BAUD ]       = 3.0f;//14400
	params[ PT_MODE ]       = static_cast< float >( controls::kModePage );

	params[ PT_CONCEALMENT ] = static_cast< float >( codec::kConcealRepeat );
	params[ PT_PAPER ]       = static_cast< float >( controls::kPaperThermal );
	params[ PT_HEADER ]      = 1.0f;
	params[ PT_MIX ]         = 1.0f;

	//---------------------------------------------------------------------
	// Declaration. Options are mapped by index in Controls.cpp because an
	// option's range reads back 0..1 whatever its element count.
	//---------------------------------------------------------------------
	auto declareOptions = [ this ]( unsigned int id, const char* name, int count, const char* ( *nameAt )( int ) ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), nameAt( i ), static_cast< float >( i ) );
	};

	declareOptions( PT_FIT, "Fit", layout::kFitCount, layout::FitName );
	declareOptions( PT_RESOLUTION, "Resolution", layout::kResolutionCount, layout::ResolutionName );
	SetParamInfof( PT_THRESHOLD, "Threshold", FF_TYPE_STANDARD );
	SetParamInfo( PT_HALFTONE, "Halftone", FF_TYPE_BOOLEAN, false );

	declareOptions( PT_CODING, "Coding", controls::kCodingCount, controls::CodingName );
	SetParamInfof( PT_LINE_NOISE, "Line Noise", FF_TYPE_STANDARD );
	SetParamInfof( PT_BURSTS, "Bursts", FF_TYPE_STANDARD );
	declareOptions( PT_BAUD, "Baud", controls::kBaudCount, controls::BaudName );
	declareOptions( PT_MODE, "Mode", controls::kModeCount, controls::ModeName );

	declareOptions( PT_CONCEALMENT, "Concealment", controls::kConcealmentCount, controls::ConcealmentName );
	declareOptions( PT_PAPER, "Paper", controls::kPaperCount, controls::PaperName );
	SetParamInfo( PT_HEADER, "Header", FF_TYPE_BOOLEAN, true );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	for( FFUInt32 i = PT_FIT; i <= PT_HALFTONE; ++i )
		SetParamGroup( i, "Scan" );
	for( FFUInt32 i = PT_CODING; i <= PT_MODE; ++i )
		SetParamGroup( i, "Line" );
	for( FFUInt32 i = PT_CONCEALMENT; i <= PT_MIX; ++i )
		SetParamGroup( i, "Receiver" );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Fax effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Fax::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string vertex = shaders::Vertex();
	struct
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	} const stages[] = {
		{ &scanShader, shaders::Scan(), "scan" },
		{ &printShader, shaders::Print(), "print" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( vertex, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Fax: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Fax: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	glGenTextures( 1, &pageTexture );
	pageTextureLines = 0;
	pageActive       = false;
	diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Fax::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

//The unit voting is readout's, unchanged: the ratio of the host's clock
//delta to a steady clock's names the unit outright, and nothing plausible
//sits between 1 and 1000.
double Fax::nowSeconds()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	if( !hostTimeSeen || hostTime < 0.0 )
		return wallNow - wallStart;

	const double raw = hostTime;

	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		//A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	lastRawTime  = raw;
	lastWallTime = wallNow;

	//Until the unit is settled, run on the real clock rather than assume one:
	//wrong in origin but right in rate, where assuming seconds would be a
	//thousand times fast on Resolume.
	return clockScale != 0.0 ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
bool Fax::startPage( const FFGLTextureStruct& input, double now )
{
	const int width  = static_cast< int >( input.Width );
	const int height = static_cast< int >( input.Height );

	const int fit        = controls::OptionIndex( params[ PT_FIT ], layout::kFitCount );
	const int resolution = controls::OptionIndex( params[ PT_RESOLUTION ], layout::kResolutionCount );
	const int coding     = controls::OptionIndex( params[ PT_CODING ], controls::kCodingCount );
	const int baud       = controls::BaudRate( controls::OptionIndex( params[ PT_BAUD ], controls::kBaudCount ) );
	const int conceal    = controls::OptionIndex( params[ PT_CONCEALMENT ], controls::kConcealmentCount );
	const double lpm     = layout::LinesPerMillimetre( resolution );

	geometry = layout::Compute( fit, resolution, width, height );
	const int lines = geometry.lines;

	//---------------------------------------------------------------------
	// Allocation first, before anything binds a texture: allocating leaves the
	// active unit bound to nothing, and the symptom of the wrong order is
	// correct on every frame except the one that allocates. The page memory
	// lives on the CPU; the buffer holds one scan and nothing else.
	//---------------------------------------------------------------------
	if( !scanBuffer.Ensure( kGroups, lines, GL_RGBA8, PassBuffer::Sampling::Nearest ) )
	{
		diag::error( "could not allocate the scan buffer" );
		return false;
	}

	//---------------------------------------------------------------------
	// 1. Scan.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( scanBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		scanBuffer.ResizeViewPort();
		ScopedShaderBinding shader( scanShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );

		scanShader.Set( "InputTexture", 0 );
		glUniform2i( scanShader.FindUniform( "InputSize" ), width, height );
		scanShader.Set( "SrcOrigin", static_cast< float >( geometry.srcX0 ), static_cast< float >( geometry.srcY0 ) );
		scanShader.Set( "SrcStep", static_cast< float >( geometry.srcDX ), static_cast< float >( geometry.srcDY ) );
		scanShader.Set( "ImageRect", static_cast< float >( geometry.imgX0 ), static_cast< float >( geometry.imgX1 ),
		                static_cast< float >( geometry.imgY0 ), static_cast< float >( geometry.imgY1 ) );
		scanShader.Set( "Threshold", static_cast< float >( controls::Threshold( params[ PT_THRESHOLD ] ) ) );
		scanShader.Set( "Halftone", params[ PT_HALFTONE ] > 0.5f ? 1 : 0 );
		scanShader.Set( "Perturb", perturb );
		quad.Draw();
	}

	//Read it back: 216 bytes a line, 100 KB for a standard 16:9 page. The one
	//stall in the plugin, and the price of a coder that is the real one.
	auto t0 = std::chrono::steady_clock::now();
	scanned.Resize( lines );
	glBindTexture( GL_TEXTURE_2D, scanBuffer.TextureID() );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, scanned.bytes.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );
	cpuCost.readback = millisecondsSince( t0 );

	//---------------------------------------------------------------------
	// 2. The sending machine: its header, then T.4.
	//---------------------------------------------------------------------
	t0 = std::chrono::steady_clock::now();
	if( params[ PT_HEADER ] > 0.5f )
		header::Print( scanned, lpm, header::Text( pageIndex + 1, now ) );

	codec::EncodeOptions encode;
	encode.coding      = coding;
	encode.k           = layout::K( resolution );
	encode.minLineBits = static_cast< int >( std::ceil( controls::kMinScanLineSeconds * baud - 1e-9 ) );
	encode.perturb     = perturb;
	codec::Encode( scanned, encode, transmission );
	sentBits     = transmission.bits;
	cpuCost.code = millisecondsSince( t0 );

	//---------------------------------------------------------------------
	// 3. The line.
	//---------------------------------------------------------------------
	t0 = std::chrono::steady_clock::now();
	line::Noise noise;
	noise.ber       = controls::BitErrorRate( params[ PT_LINE_NOISE ] );
	noise.burstBits = params[ PT_BURSTS ] > 0.0f ? controls::BurstBits( params[ PT_BURSTS ] ) : 1;
	noise.seed      = line::Hash( static_cast< uint32_t >( pageIndex ) * 2654435761u + 12345u );
	line::Corrupt( transmission, noise, forced );
	line::Schedule( transmission, baud, perturb, timing );
	cpuCost.channel = millisecondsSince( t0 );

	//---------------------------------------------------------------------
	// 4. The receiving machine.
	//---------------------------------------------------------------------
	t0 = std::chrono::steady_clock::now();
	codec::DecodeOptions decode;
	decode.coding      = coding;
	decode.concealment = conceal;
	decode.perturb     = perturb;
	codec::Decode( transmission.bits, lines, decode, decoded );
	cpuCost.decode = millisecondsSince( t0 );

	//The paper under the arriving page is the last page -- blank on the first,
	//and blank again when the page changes shape.
	if( display.lines != lines )
	{
		display.Resize( lines );
		uploadDisplay( 0, lines );
	}

	pageStart      = now;
	pageFit        = fit;
	pageResolution = resolution;
	pageActive     = true;
	shownLines     = 0;
	++pageIndex;
	return true;
}

void Fax::uploadDisplay( int fromLine, int toLine )
{
	glBindTexture( GL_TEXTURE_2D, pageTexture );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	if( pageTextureLines != display.lines )
	{
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, kGroups, display.lines, 0, GL_RGBA, GL_UNSIGNED_BYTE, display.bytes.data() );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		pageTextureLines = display.lines;
	}
	else if( toLine > fromLine )
	{
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, fromLine, kGroups, toLine - fromLine, GL_RGBA, GL_UNSIGNED_BYTE,
		                 display.Row( fromLine ) );
	}
	glBindTexture( GL_TEXTURE_2D, 0 );
}

//---------------------------------------------------------------------------
FFResult Fax::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];
	if( input.Width == 0 || input.Height == 0 )
		return FF_FAIL;

	const int width  = static_cast< int >( input.Width );
	const int height = static_cast< int >( input.Height );

	//The host's viewport, read before anything of ours changes it.
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// The clock. One thing reads it: how far into the page the line is.
	// Reduced in double here; nothing absolute crosses into GLSL.
	//---------------------------------------------------------------------
	const double now = nowSeconds();
	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );

	const int mode       = controls::OptionIndex( params[ PT_MODE ], controls::kModeCount );
	const int fit        = controls::OptionIndex( params[ PT_FIT ], layout::kFitCount );
	const int resolution = controls::OptionIndex( params[ PT_RESOLUTION ], layout::kResolutionCount );

	//---------------------------------------------------------------------
	// A new page: every frame in Live; in Page when the last one has ended,
	// when the page would change shape, when the mode changes, or when the
	// clock has gone backwards past the page's start (a scrub).
	//---------------------------------------------------------------------
	const bool newPage = !pageActive || mode == controls::kModeLive || mode != pageMode || fit != pageFit
	                     || resolution != pageResolution || now < pageStart || now - pageStart >= timing.duration;
	pageMode = mode;
	if( newPage && !startPage( input, now ) )
		return FF_FAIL;

	//---------------------------------------------------------------------
	// The lines that have arrived, over the last page.
	//---------------------------------------------------------------------
	const int arrived = mode == controls::kModeLive ? geometry.lines : line::Arrived( timing, now - pageStart );
	if( arrived > shownLines )
	{
		std::copy( decoded.page.Row( shownLines ), decoded.page.Row( arrived ), display.Row( shownLines ) );
		uploadDisplay( shownLines, arrived );
		shownLines = arrived;
	}

	//---------------------------------------------------------------------
	// Print, straight to the host.
	//---------------------------------------------------------------------
	layout::Geometry shown = geometry;
	layout::Place( shown, pageFit, width, height );
	const controls::Colours colours = controls::PaperColours( controls::OptionIndex( params[ PT_PAPER ], controls::kPaperCount ) );
	const FFGLTexCoords maxCoords   = GetMaxGLTexCoords( input );
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( printShader.GetGLID() );
		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding pageBinding( pageTexture );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding sourceBinding( input.Handle );

		printShader.Set( "PageBits", 0 );
		printShader.Set( "Source", 1 );
		printShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		glUniform2i( printShader.FindUniform( "OutSize" ), width, height );
		printShader.Set( "Sheet", static_cast< float >( shown.sheetX ), static_cast< float >( shown.sheetY ),
		                 static_cast< float >( shown.sheetW ), static_cast< float >( shown.sheetH ) );
		printShader.Set( "Lines", display.lines );
		printShader.Set( "PaperColour", colours.paper[ 0 ], colours.paper[ 1 ], colours.paper[ 2 ] );
		printShader.Set( "InkColour", colours.ink[ 0 ], colours.ink[ 1 ], colours.ink[ 2 ] );
		printShader.Set( "DeskColour", controls::kDesk[ 0 ], controls::kDesk[ 1 ], controls::kDesk[ 2 ] );
		printShader.Set( "MixAmount", std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) );
		printShader.Set( "Perturb", perturb );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Fax::DeInitGL()
{
	scanShader.FreeGLResources();
	printShader.FreeGLResources();
	quad.Release();
	scanBuffer.Destroy();
	if( pageTexture != 0 )
	{
		glDeleteTextures( 1, &pageTexture );
		pageTexture = 0;
	}
	pageTextureLines = 0;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Fax::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Fax::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Fax::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Fax::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

//---------------------------------------------------------------------------
void Fax::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void Fax::SetPerturbForTest( int bits )
{
	perturb = bits;
}

void Fax::SetForcedErrorsForTest( const std::vector< line::ForcedError >& errors )
{
	forced = errors;
}
