#include "Shaders.h"

namespace fax::shaders
{
namespace
{
const char* const kVersion = "#version 410 core\n";

//---------------------------------------------------------------------------
// The vertex shader both passes share.
//---------------------------------------------------------------------------
const char* const kVertexBody = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// Pass 1: scan. gl_FragCoord.x is the 32-pel group, gl_FragCoord.y the scan
// line; texture row 0 is line 0, the top of the page.
//---------------------------------------------------------------------------
const char* const kScanBody = R"(
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
)";

//---------------------------------------------------------------------------
// Pass 2: print. The page texture is 54 x Lines RGBA8 in the scan pass's
// layout; row 0 is line 0.
//---------------------------------------------------------------------------
const char* const kPrintBody = R"(
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
)";

std::string assemble( const char* body )
{
	return std::string( kVersion ) + body;
}
} // namespace

std::string Vertex()
{
	return assemble( kVertexBody );
}

std::string Scan()
{
	return assemble( kScanBody );
}

std::string Print()
{
	return assemble( kPrintBody );
}

} // namespace fax::shaders
