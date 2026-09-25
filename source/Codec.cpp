#include "Codec.h"

#include "T4.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace fax::codec
{
namespace
{
//---------------------------------------------------------------------------
// Decoding tries, built once from T4.h. Node 0 is the root; a child index of
// 0 means "no such code".
//---------------------------------------------------------------------------
struct Trie
{
	struct Node
	{
		int child[ 2 ] = { 0, 0 };
		int value      = -1;///< the leaf's value, -1 for a branch
		int makeup     = 0;
	};
	std::vector< Node > nodes;

	Trie()
	{
		nodes.emplace_back();
	}

	void Add( const char* bits, int value, int makeup )
	{
		int n = 0;
		for( const char* c = bits; *c; ++c )
		{
			const int b = *c == '1' ? 1 : 0;
			if( nodes[ n ].child[ b ] == 0 )
			{
				nodes[ n ].child[ b ] = static_cast< int >( nodes.size() );
				nodes.emplace_back();
			}
			n = nodes[ n ].child[ b ];
		}
		nodes[ n ].value  = value;
		nodes[ n ].makeup = makeup;
	}

	/// Read one code at `pos` (not past `end`). Returns the leaf or nullptr.
	const Node* Read( const Bits& bits, size_t& pos, size_t end ) const
	{
		int n = 0;
		while( pos < end )
		{
			n = nodes[ n ].child[ bits[ pos++ ] ];
			if( n == 0 )
				return nullptr;
			if( nodes[ n ].value >= 0 )
				return &nodes[ n ];
		}
		return nullptr;
	}
};

struct Tries
{
	Trie colour[ 2 ];
	Trie modes;

	Tries()
	{
		for( const auto& c : t4::kWhiteTerminating )
			colour[ kWhite ].Add( c.bits, c.value, 0 );
		for( const auto& c : t4::kWhiteMakeup )
			colour[ kWhite ].Add( c.bits, c.value, 1 );
		for( const auto& c : t4::kBlackTerminating )
			colour[ kBlack ].Add( c.bits, c.value, 0 );
		for( const auto& c : t4::kBlackMakeup )
			colour[ kBlack ].Add( c.bits, c.value, 1 );
		for( const auto& c : t4::kExtendedMakeup )
		{
			colour[ kWhite ].Add( c.bits, c.value, 1 );
			colour[ kBlack ].Add( c.bits, c.value, 1 );
		}
		for( const auto& c : t4::kModes )
			modes.Add( c.bits, c.value, 0 );
	}
};

const Tries& tries()
{
	static const Tries t;
	return t;
}

//---------------------------------------------------------------------------
// The same codes compiled for speed: as bit arrays for the coder, and as
// lookup tables over a peek of the next 13 (or 7) bits for the decoder. A
// halftone page is two million bits a frame in Live mode, and the tries and
// the string parsing cost more than the rest of the plugin together.
//---------------------------------------------------------------------------
struct CodeBits
{
	uint8_t b[ 16 ];
	int n;
};

CodeBits compile( const char* s )
{
	CodeBits c = {};
	for( ; s[ c.n ]; ++c.n )
		c.b[ c.n ] = s[ c.n ] == '1' ? 1 : 0;
	return c;
}

struct Compiled
{
	CodeBits term[ 2 ][ 64 ];
	CodeBits makeup[ 2 ][ 27 ];
	CodeBits ext[ 13 ];
	CodeBits modes[ t4::kModeCount ];
	CodeBits eol;

	Compiled()
	{
		for( int i = 0; i < 64; ++i )
		{
			term[ kWhite ][ i ] = compile( t4::kWhiteTerminating[ i ].bits );
			term[ kBlack ][ i ] = compile( t4::kBlackTerminating[ i ].bits );
		}
		for( int i = 0; i < 27; ++i )
		{
			makeup[ kWhite ][ i ] = compile( t4::kWhiteMakeup[ i ].bits );
			makeup[ kBlack ][ i ] = compile( t4::kBlackMakeup[ i ].bits );
		}
		for( int i = 0; i < 13; ++i )
			ext[ i ] = compile( t4::kExtendedMakeup[ i ].bits );
		for( int i = 0; i < t4::kModeCount; ++i )
			modes[ i ] = compile( t4::kModes[ i ].bits );
		eol = compile( t4::kEol );
	}
};

const Compiled& compiled()
{
	static const Compiled c;
	return c;
}

inline void append( Bits& out, const CodeBits& c )
{
	out.insert( out.end(), c.b, c.b + c.n );
}

/// The stream packed first-bit-most-significant into 64-bit words, so the
/// next k bits are two shifts.
struct Packed
{
	std::vector< uint64_t > words;

	size_t size = 0;

	explicit Packed( const Bits& bits )
	{
		size = bits.size();
		words.assign( size / 64 + 2, 0 );
		//Eight 0/1 bytes to one byte, first bit most significant: the
		//multiply lands byte i's bit at bit 63 - i and nothing else in the top
		//byte (the other products fall below bit 56 or past bit 63).
		size_t i = 0;
		for( ; i + 8 <= size; i += 8 )
		{
			uint64_t v;
			std::memcpy( &v, bits.data() + i, 8 );
			const uint64_t byte = ( v * 0x8040201008040201ull ) >> 56;
			words[ i >> 6 ] |= byte << ( 56 - ( i & 63 ) );
		}
		for( ; i < size; ++i )
			words[ i >> 6 ] |= static_cast< uint64_t >( bits[ i ] ) << ( 63 - ( i & 63 ) );
	}

	int Bit( size_t pos ) const
	{
		return static_cast< int >( ( words[ pos >> 6 ] >> ( 63 - ( pos & 63 ) ) ) & 1u );
	}

	/// Any 1 in [ begin, end )?
	bool Any( size_t begin, size_t end ) const
	{
		while( begin < end && ( begin & 63 ) != 0 )
			if( Bit( begin++ ) )
				return true;
		while( begin + 64 <= end )
		{
			if( words[ begin >> 6 ] != 0 )
				return true;
			begin += 64;
		}
		while( begin < end )
			if( Bit( begin++ ) )
				return true;
		return false;
	}

	/// Every EOL: the index of the 1 that ends eleven or more zeros.
	std::vector< size_t > Eols() const
	{
		std::vector< size_t > eols;
		size_t zeros = 0;
		for( size_t w = 0; w * 64 < size; ++w )
		{
			uint64_t word   = words[ w ];
			const size_t base = w * 64;
			const int valid = static_cast< int >( std::min< size_t >( 64, size - base ) );
			int at          = 0;//bits of this word consumed
			while( at < valid )
			{
				if( word == 0 )
				{
					zeros += static_cast< size_t >( valid - at );
					break;
				}
				const int lead = __builtin_clzll( word );//zeros before the next 1
				if( at + lead >= valid )
				{
					zeros += static_cast< size_t >( valid - at );
					break;
				}
				zeros += static_cast< size_t >( lead );
				if( zeros >= 11 )
					eols.push_back( base + static_cast< size_t >( at + lead ) );
				zeros = 0;
				at += lead + 1;
				word = lead + 1 >= 64 ? 0 : word << ( lead + 1 );
			}
		}
		return eols;
	}

	uint32_t Peek( size_t pos, int k ) const
	{
		const size_t w = pos >> 6;
		const int off  = static_cast< int >( pos & 63 );
		uint64_t v     = words[ w ] << off;
		if( off != 0 )
			v |= words[ w + 1 ] >> ( 64 - off );
		return static_cast< uint32_t >( v >> ( 64 - k ) );
	}
};

struct Lut
{
	struct Entry
	{
		int16_t value;
		uint8_t length;///< 0: no code starts with these bits
		uint8_t makeup;
	};
	int width = 0;
	std::vector< Entry > entries;

	explicit Lut( int bits ) : width( bits ), entries( static_cast< size_t >( 1 ) << bits, Entry{ 0, 0, 0 } ) {}

	void Add( const char* code, int value, int makeup )
	{
		const int n = static_cast< int >( std::strlen( code ) );
		uint32_t prefix = 0;
		for( int i = 0; i < n; ++i )
			prefix = ( prefix << 1 ) | ( code[ i ] == '1' ? 1u : 0u );
		const uint32_t first = prefix << ( width - n );
		const uint32_t count = 1u << ( width - n );
		for( uint32_t i = 0; i < count; ++i )
			entries[ first + i ] = Entry{ static_cast< int16_t >( value ), static_cast< uint8_t >( n ), static_cast< uint8_t >( makeup ) };
	}

	/// The code at `pos`, or nullptr if none, or if it would run past `end`.
	const Entry* Read( const Packed& p, size_t& pos, size_t end ) const
	{
		const Entry& e = entries[ p.Peek( pos, width ) ];
		if( e.length == 0 || pos + e.length > end )
			return nullptr;
		pos += e.length;
		return &e;
	}
};

struct Luts
{
	Lut colour[ 2 ] = { Lut( 13 ), Lut( 13 ) };
	Lut modes       = Lut( 7 );

	Luts()
	{
		for( const auto& c : t4::kWhiteTerminating )
			colour[ kWhite ].Add( c.bits, c.value, 0 );
		for( const auto& c : t4::kWhiteMakeup )
			colour[ kWhite ].Add( c.bits, c.value, 1 );
		for( const auto& c : t4::kBlackTerminating )
			colour[ kBlack ].Add( c.bits, c.value, 0 );
		for( const auto& c : t4::kBlackMakeup )
			colour[ kBlack ].Add( c.bits, c.value, 1 );
		for( const auto& c : t4::kExtendedMakeup )
		{
			colour[ kWhite ].Add( c.bits, c.value, 1 );
			colour[ kBlack ].Add( c.bits, c.value, 1 );
		}
		for( const auto& c : t4::kModes )
			modes.Add( c.bits, c.value, 0 );
	}
};

const Luts& luts()
{
	static const Luts l;
	return l;
}

/// One run at `pos` from the lookup tables: make-ups then a terminating
/// code. -1 for an invalid code.
int readRunFast( const Packed& p, size_t& pos, size_t end, int colour )
{
	const Lut& lut = luts().colour[ colour ];
	int run        = 0;
	for( ;; )
	{
		const Lut::Entry* e = lut.Read( p, pos, end );
		if( e == nullptr )
			return -1;
		run += e->value;
		if( !e->makeup )
			return run;
	}
}

//---------------------------------------------------------------------------
// Changing elements (4.2.1.3.1): the positions where a pel differs from the
// one before it, the pel before the first being an imaginary white one. The
// new colour at change k is black for even k and white for odd k. Two
// sentinels at kWidth close the list.
//---------------------------------------------------------------------------
void changesOf( const uint8_t* line, std::vector< int >& out )
{
	out.clear();
	int previous = kWhite;
	for( int byte = 0; byte < kLineBytes; ++byte )
	{
		const int value = line[ byte ];
		//A whole byte of the previous colour holds no change.
		if( value == ( previous ? 0xFF : 0x00 ) )
			continue;
		for( int b = 0; b < 8; ++b )
		{
			const int pel = ( value >> b ) & 1;
			if( pel != previous )
			{
				out.push_back( byte * 8 + b );
				previous = pel;
			}
		}
	}
	out.push_back( kWidth );
	out.push_back( kWidth );
}

/// The new colour at change index k.
inline int colourAfter( size_t k )
{
	return ( k & 1 ) == 0 ? kBlack : kWhite;
}

void fillPels( uint8_t* out, int from, int to, int colour )
{
	from = std::clamp( from, 0, kWidth );
	to   = std::clamp( to, 0, kWidth );
	auto one = [ & ]( int x ) {
		if( colour )
			out[ x >> 3 ] = static_cast< uint8_t >( out[ x >> 3 ] | ( 1 << ( x & 7 ) ) );
		else
			out[ x >> 3 ] = static_cast< uint8_t >( out[ x >> 3 ] & ~( 1 << ( x & 7 ) ) );
	};
	while( from < to && ( from & 7 ) != 0 )
		one( from++ );
	const int bytes = ( to - from ) >> 3;
	if( bytes > 0 )
	{
		std::memset( out + ( from >> 3 ), colour ? 0xFF : 0x00, static_cast< size_t >( bytes ) );
		from += bytes * 8;
	}
	while( from < to )
		one( from++ );
}

/// Does an EOL (at least eleven zeros, then a one) start at `pos`? If so,
/// `after` is the index just past its one.
bool eolAt( const Bits& bits, size_t pos, size_t& after )
{
	size_t zeros = 0;
	while( pos + zeros < bits.size() && bits[ pos + zeros ] == 0 )
		++zeros;
	if( zeros < 11 || pos + zeros >= bits.size() )
		return false;
	after = pos + zeros + 1;
	return true;
}
} // namespace

//---------------------------------------------------------------------------
void Page::Resize( int lineCount )
{
	lines = lineCount;
	bytes.assign( static_cast< size_t >( lineCount ) * kLineBytes, 0 );
}

void Page::Clear()
{
	std::fill( bytes.begin(), bytes.end(), 0 );
}

void Page::SetPel( int line, int x, int colour )
{
	uint8_t& b = Row( line )[ x >> 3 ];
	if( colour )
		b = static_cast< uint8_t >( b | ( 1 << ( x & 7 ) ) );
	else
		b = static_cast< uint8_t >( b & ~( 1 << ( x & 7 ) ) );
}

//---------------------------------------------------------------------------
std::vector< TableEntry > Table( int colour, int perturb )
{
	std::vector< TableEntry > out;
	const t4::Code* term   = colour == kWhite ? t4::kWhiteTerminating : t4::kBlackTerminating;
	const t4::Code* makeup = colour == kWhite ? t4::kWhiteMakeup : t4::kBlackMakeup;
	for( int i = 0; i < 64; ++i )
		out.push_back( { term[ i ].value, term[ i ].bits, 0 } );
	for( int i = 0; i < 27; ++i )
		out.push_back( { makeup[ i ].value, makeup[ i ].bits, 1 } );
	for( const auto& c : t4::kExtendedMakeup )
		out.push_back( { c.value, c.bits, 1 } );
	if( ( perturb & kPerturbTableSwap ) != 0 && colour == kWhite )
		std::swap( out[ 5 ].bits, out[ 8 ].bits );
	return out;
}

void PutBits( Bits& out, const char* bits )
{
	for( const char* c = bits; *c; ++c )
		out.push_back( *c == '1' ? 1 : 0 );
}

void PutRun( Bits& out, int colour, int run, int perturb )
{
	(void)perturb;
	const Compiled& c = compiled();
	//4.1.1: runs past 2560 take 2560 make-ups until the rest fits.
	while( run > 2560 )
	{
		append( out, c.ext[ 12 ] );
		run -= 2560;
	}
	if( run >= 64 )
	{
		const int m = run / 64;//1..40
		if( m <= 27 )
			append( out, c.makeup[ colour ][ m - 1 ] );
		else
			append( out, c.ext[ m - 28 ] );
		run -= m * 64;
	}
	append( out, c.term[ colour ][ run ] );
}

int ReadRun( const Bits& bits, size_t& pos, size_t end, int colour )
{
	const Trie& trie = tries().colour[ colour ];
	int run          = 0;
	for( ;; )
	{
		const Trie::Node* node = trie.Read( bits, pos, end );
		if( node == nullptr )
			return -1;
		run += node->value;
		if( !node->makeup )
			return run;
	}
}


//4.2.1.3: the two-dimensional coding procedure.
namespace
{
/// 4.2.1.3: the two-dimensional coding procedure, over the changing elements
/// of the coding line `c` and the reference line `r`. a0 only ever moves
/// right, so the indices into both lists only move forward.
void encode2D( const std::vector< int >& c, const std::vector< int >& r, Bits& out, int perturb, int* modeCounts )
{
	const Compiled& code = compiled();
	const size_t cLast   = c.size() - 2;//index of the first sentinel
	const size_t rLast   = r.size() - 2;
	size_t ka            = 0;
	size_t kb            = 0;
	int a0               = -1;//the imaginary white pel before the line
	int colour           = kWhite;
	while( a0 < kWidth )
	{
		//a1: the next change on the coding line right of a0 (its new colour is
		//the opposite of a0's by construction). a2: the one after it.
		while( ka < cLast && c[ ka ] <= a0 )
			++ka;
		const int a1 = c[ ka ];
		const int a2 = c[ ka + 1 ];
		//b1: the first change on the reference line right of a0 to the colour
		//opposite a0's; b2: the next change after b1.
		while( kb < rLast && r[ kb ] <= a0 )
			++kb;
		size_t kb1 = kb;
		if( kb1 < rLast && colourAfter( kb1 ) == colour )
			++kb1;
		const int b1 = r[ kb1 ];
		const int b2 = r[ kb1 + 1 < r.size() ? kb1 + 1 : kb1 ];

		if( b2 < a1 )
		{
			append( out, code.modes[ t4::kPass ] );
			if( modeCounts )
				++modeCounts[ t4::kPass ];
			a0 = b2;
			continue;
		}
		const int d = a1 - b1;
		if( d >= -3 && d <= 3 )
		{
			int mode = d == 0 ? t4::kV0 : ( d > 0 ? t4::kVR1 + d - 1 : t4::kVL1 - d - 1 );
			if( ( perturb & kPerturbVerticalSkew ) != 0 && mode == t4::kVR1 )
				mode = t4::kVR2;
			append( out, code.modes[ mode ] );
			if( modeCounts )
				++modeCounts[ mode ];
			a0     = a1;
			colour = 1 - colour;
			continue;
		}
		append( out, code.modes[ t4::kHorizontal ] );
		if( modeCounts )
			++modeCounts[ t4::kHorizontal ];
		const int start = std::max( a0, 0 );
		PutRun( out, colour, a1 - start, perturb );
		PutRun( out, 1 - colour, a2 - a1, perturb );
		a0 = a2;
	}
}

void encode1D( const std::vector< int >& changes, Bits& out, int perturb )
{
	int a0     = 0;
	int colour = kWhite;
	for( size_t k = 0;; ++k )
	{
		const int a1 = changes[ k ];
		PutRun( out, colour, a1 - a0, perturb );
		if( a1 >= kWidth )
			break;
		a0     = a1;
		colour = 1 - colour;
	}
}
} // namespace

void EncodeLine2D( const uint8_t* line, const uint8_t* reference, Bits& out, int perturb, int* modeCounts )
{
	static const uint8_t white[ kLineBytes ] = {};
	std::vector< int > c, r;
	changesOf( line, c );
	changesOf( reference ? reference : white, r );
	encode2D( c, r, out, perturb, modeCounts );
}

void EncodeLine1D( const uint8_t* line, Bits& out, int perturb )
{
	std::vector< int > changes;
	changesOf( line, changes );
	encode1D( changes, out, perturb );
}

void Encode( const Page& page, const EncodeOptions& options, Transmission& out )
{
	out.bits.clear();
	out.lines.clear();
	const bool mr = options.coding == kMR;
	const int k   = std::max( 1, options.k );
	out.bits.reserve( std::max( out.bits.capacity(), static_cast< size_t >( page.lines ) * 64 ) );
	static const uint8_t white[ kLineBytes ] = {};
	std::vector< int > changes, reference;
	changesOf( white, changes );//above line 0: the imaginary white line

	auto oneDimensional = [ & ]( int line ) {
		if( !mr )
			return true;
		if( ( options.perturb & kPerturbNoRefresh ) != 0 )
			return line == 0;
		return line % k == 0;
	};

	//The page opens with an EOL, and in MR its tag says how line 0 is coded.
	PutBits( out.bits, t4::kEol );
	if( mr )
		out.bits.push_back( oneDimensional( 0 ) ? 1 : 0 );
	out.preambleBits = out.bits.size();

	for( int line = 0; line < page.lines; ++line )
	{
		LineRecord record;
		record.dataStart = out.bits.size();
		record.oneD      = oneDimensional( line );
		//The changing elements of this line, and of the one above (this
		//line's reference) from the last iteration.
		std::swap( reference, changes );
		changesOf( page.Row( line ), changes );
		if( record.oneD )
			encode1D( changes, out.bits, options.perturb );
		else
			encode2D( changes, reference, out.bits, options.perturb, options.modeCounts );
		record.dataBits = out.bits.size() - record.dataStart;

		//The next line's tag rides on this line's EOL.
		const bool nextOneD = line + 1 < page.lines ? oneDimensional( line + 1 ) : true;
		const size_t tail   = t4::kEolBits + ( mr ? 1 : 0 );
		size_t fill         = 0;
		if( ( options.perturb & kPerturbNoFill ) == 0 && record.dataBits + tail < static_cast< size_t >( options.minLineBits ) )
			fill = static_cast< size_t >( options.minLineBits ) - record.dataBits - tail;
		out.bits.insert( out.bits.end(), fill, 0 );
		PutBits( out.bits, t4::kEol );
		if( mr )
			out.bits.push_back( nextOneD ? 1 : 0 );

		record.fillBits  = fill;
		record.totalBits = record.dataBits + fill + tail;
		out.lines.push_back( record );
	}

	//RTC: the line after the last one is six EOLs. The last line's own EOL is
	//the first of them in spirit, but T.4 sends six after it; so do we.
	out.rtcStart = out.bits.size();
	for( int i = 0; i < t4::kRtcEols; ++i )
	{
		PutBits( out.bits, t4::kEol );
		if( mr )
			out.bits.push_back( 1 );
	}
}

//---------------------------------------------------------------------------
std::vector< size_t > FindEols( const Bits& bits )
{
	return Packed( bits ).Eols();
}


namespace
{
bool decodeLine( const Packed& bits, size_t begin, size_t end, bool twoD, const uint8_t* reference, uint8_t* out,
                 std::vector< int >* pelAtBit, std::vector< int >& r )
{
	static const uint8_t white[ kLineBytes ] = {};
	std::memset( out, 0, kLineBytes );
	if( pelAtBit )
		pelAtBit->assign( end > begin ? end - begin : 0, -1 );

	const Luts& t = luts();
	size_t pos     = begin;
	int colour     = kWhite;
	int a0         = twoD ? -1 : 0;
	bool bad       = false;

	auto mark = [ & ]( size_t from ) {
		if( !pelAtBit )
			return;
		for( size_t i = from; i < pos && i < end; ++i )
			( *pelAtBit )[ i - begin ] = std::max( a0, 0 );
	};

	if( !twoD )
	{
		while( a0 < kWidth )
		{
			const size_t codeStart = pos;
			int run                = 0;
			bool ok                = false;
			for( ;; )
			{
				const Lut::Entry* node = t.colour[ colour ].Read( bits, pos, end );
				if( node == nullptr )
					break;
				run += node->value;
				if( !node->makeup )
				{
					ok = true;
					break;
				}
			}
			mark( codeStart );
			if( !ok )
			{
				bad = true;
				break;
			}
			if( a0 + run > kWidth )
			{
				fillPels( out, a0, kWidth, colour );
				a0  = kWidth;
				bad = true;
				break;
			}
			fillPels( out, a0, a0 + run, colour );
			a0 += run;
			if( a0 < kWidth )
				colour = 1 - colour;
		}
	}
	else
	{
		changesOf( reference ? reference : white, r );
		const size_t rLast = r.size() - 2;
		size_t kb          = 0;
		while( a0 < kWidth )
		{
			const size_t codeStart = pos;
			const Lut::Entry* node = t.modes.Read( bits, pos, end );
			if( node == nullptr )
			{
				mark( codeStart );
				bad = true;
				break;
			}
			//b1, b2 as the coder finds them; a0 only moves right.
			while( kb < rLast && r[ kb ] <= a0 )
				++kb;
			size_t kb1 = kb;
			if( kb1 < rLast && colourAfter( kb1 ) == colour )
				++kb1;
			const int b1     = r[ kb1 ];
			const int b2     = r[ kb1 + 1 < r.size() ? kb1 + 1 : kb1 ];
			const int start  = std::max( a0, 0 );
			const int mode   = node->value;

			if( mode == t4::kPass )
			{
				mark( codeStart );
				fillPels( out, start, b2, colour );
				a0 = b2;
				continue;
			}
			if( mode == t4::kHorizontal )
			{
				const int run1 = readRunFast( bits, pos, end, colour );
				const int run2 = run1 < 0 ? -1 : readRunFast( bits, pos, end, 1 - colour );
				mark( codeStart );
				if( run1 < 0 || run2 < 0 )
				{
					bad = true;
					break;
				}
				if( start + run1 + run2 > kWidth )
				{
					fillPels( out, start, start + run1, colour );
					fillPels( out, start + run1, kWidth, 1 - colour );
					a0     = kWidth;
					colour = 1 - colour;
					bad    = true;
					break;
				}
				fillPels( out, start, start + run1, colour );
				fillPels( out, start + run1, start + run1 + run2, 1 - colour );
				a0 = start + run1 + run2;
				continue;
			}
			//Vertical: a1 is b1 moved by the mode's offset.
			const int d  = mode == t4::kV0 ? 0 : ( mode <= t4::kVR3 ? mode - t4::kVR1 + 1 : -( mode - t4::kVL1 + 1 ) );
			const int a1 = b1 + d;
			mark( codeStart );
			if( a1 < 0 || a1 > kWidth || ( a0 >= 0 && a1 <= a0 ) )
			{
				bad = true;
				break;
			}
			fillPels( out, start, a1, colour );
			a0     = a1;
			colour = 1 - colour;
		}
	}

	if( bad )
	{
		//Show the garbage: the colour it was in at the failure, to the edge.
		fillPels( out, std::max( a0, 0 ), kWidth, colour );
		return false;
	}
	//Past the 1728th pel only fill may follow.
	for( size_t i = pos; i < end; ++i )
		if( bits.Peek( i, 1 ) != 0 )
			return false;
	return true;
}
} // namespace

bool DecodeLine( const Bits& bits, size_t begin, size_t end, bool twoD, const uint8_t* reference, uint8_t* out,
                 std::vector< int >* pelAtBit )
{
	std::vector< int > r;
	return decodeLine( Packed( bits ), begin, end, twoD, reference, out, pelAtBit, r );
}


//---------------------------------------------------------------------------
void Decode( const Bits& bits, int lines, const DecodeOptions& options, Decoded& out )
{
	out.page.Resize( lines );
	out.bad.assign( static_cast< size_t >( lines ), 0 );
	out.shown.assign( static_cast< size_t >( lines ), kShownMissing );
	out.segments = 0;

	const bool mr = options.coding == kMR;
	std::vector< uint8_t > scratch( kLineBytes );
	int line               = 0;
	bool referenceSpoiled  = false;//MR + Repeat Line: 2-D lines are concealed until a 1-D one

	auto emit = [ & ]( bool good, bool twoD, const uint8_t* decoded ) {
		if( line >= lines )
			return;
		uint8_t* row = out.page.Row( line );
		out.bad[ static_cast< size_t >( line ) ] = good ? 0 : 1;
		if( !twoD )
			referenceSpoiled = false;
		const bool conceal = options.concealment == kConcealRepeat && ( !good || ( mr && twoD && referenceSpoiled ) );
		if( conceal )
		{
			const int from = ( options.perturb & kPerturbConcealWrong ) != 0 ? line - 2 : line - 1;
			if( from >= 0 )
				std::memcpy( row, out.page.Row( from ), kLineBytes );
			else
				std::memset( row, 0, kLineBytes );
			out.shown[ static_cast< size_t >( line ) ] = kShownConcealed;
			if( mr )
				referenceSpoiled = true;
		}
		else
		{
			std::memcpy( row, decoded, kLineBytes );
			out.shown[ static_cast< size_t >( line ) ] = kShownDecoded;
		}
		++line;
	};

	if( ( options.perturb & kPerturbNoResync ) == 0 )
	{
		//Every EOL first; each line is the stretch between two of them.
		const Packed packed( bits );
		const std::vector< size_t > eols = packed.Eols();
		std::vector< int > changes;
		for( size_t e = 0; e < eols.size() && line < lines; ++e )
		{
			size_t begin = eols[ e ] + 1;
			bool twoD    = false;
			if( mr )
			{
				if( begin >= bits.size() )
					break;
				twoD = bits[ begin ] == 0;
				++begin;
			}
			const size_t end = e + 1 < eols.size() ? eols[ e + 1 ] - 11 : bits.size();
			//A stretch of nothing but zeros is fill, or RTC: no line.
			if( end <= begin || !packed.Any( begin, end ) )
				continue;
			++out.segments;
			const uint8_t* reference = line > 0 ? out.page.Row( line - 1 ) : nullptr;
			const bool good          = decodeLine( packed, begin, end, twoD, reference, scratch.data(), nullptr, changes );
			emit( good, twoD, scratch.data() );
		}
		return;
	}

	//The negative control: a decoder that counts pels and never looks for an
	//EOL except exactly where it expects one.
	size_t pos = 0;
	size_t after;
	bool twoD = false;
	if( eolAt( bits, pos, after ) )
	{
		pos = after;
		if( mr && pos < bits.size() )
			twoD = bits[ pos++ ] == 0;
	}
	while( line < lines && pos < bits.size() )
	{
		//Decode greedily from pos: invalid codes skip a bit, an EOL mid-line is
		//stepped over, and the line ends at 1728 pels whatever came before.
		std::memset( scratch.data(), 0, kLineBytes );
		int a0     = 0;
		int colour = kWhite;
		bool good  = true;
		const Tries& t = tries();
		while( a0 < kWidth && pos < bits.size() )
		{
			if( eolAt( bits, pos, after ) )
			{
				pos  = after + ( mr ? 1 : 0 );
				good = false;
				continue;
			}
			size_t p = pos;
			int run  = 0;
			bool ok  = false;
			for( ;; )
			{
				const Trie::Node* node = t.colour[ colour ].Read( bits, p, bits.size() );
				if( node == nullptr )
					break;
				run += node->value;
				if( !node->makeup )
				{
					ok = true;
					break;
				}
			}
			if( !ok )
			{
				++pos;
				good = false;
				continue;
			}
			pos = p;
			fillPels( scratch.data(), a0, a0 + run, colour );
			a0 = std::min( kWidth, a0 + run );
			colour = 1 - colour;
		}
		(void)twoD;
		emit( good, false, scratch.data() );
		if( eolAt( bits, pos, after ) )
		{
			pos = after;
			if( mr && pos < bits.size() )
				twoD = bits[ pos++ ] == 0;
		}
	}
}

} // namespace fax::codec
