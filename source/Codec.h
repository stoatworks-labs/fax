#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/**
	The Group 3 coder and decoder of ITU-T T.4, and the page they work on.

	**The page** is 1728 pels a line, one bit a pel, 1 = black, packed eight pels
	to a byte with pel 8i + b in bit b of byte i: 216 bytes a line. That is the
	layout the scan pass writes into its RGBA8 target (54 texels x 4 channels a
	line), the layout the CPU reads back, and the layout the print pass reads,
	so nothing is repacked anywhere.

	**The coder** (4.1, 4.2) writes a page as the Recommendation's bit stream:

	    EOL [tag]  line 0 data  [fill]  EOL [tag]  line 1 data ... [fill]  RTC

	MH codes every line one-dimensionally: alternating white and black runs, the
	first white (possibly of length 0), each run as zero or more make-up codes and
	one terminating code. MR codes one line in K one-dimensionally and the rest
	against the line above with the pass / horizontal / vertical modes, and says
	which after every EOL with a tag bit (1 = one-dimensional). Fill is zeros
	before an EOL, enough that data + fill + EOL (+ tag) reaches the minimum
	scan-line time -- which is how a real terminal meets it.

	**The decoder** finds every EOL in the stream first (eleven zeros and a one --
	valid data never holds eleven zeros, see `T4.h`), and decodes each line from
	the stretch between two EOLs. That is what confines an error to its line: a
	bit error desynchronises the codes, the decoder reads the wrong lengths until
	the stretch runs out or overflows 1728 pels, and the next line starts at the
	next EOL whatever happened. A line is BAD when a code is invalid, the pels
	overflow 1728, the stretch ends short of 1728, or anything but fill follows
	the 1728th pel. With `Concealment` Repeat Line a bad line is replaced by the
	line above -- and in MR so is every two-dimensional line after it until the
	next one-dimensional line, because its reference is no longer the one the
	coder used. With Off, the decoder shows what it decoded: the pels up to the
	failure, and the colour it was in at the failure from there to the right edge.

	An error that happens to forge an EOL splits its line, and one that breaks an
	EOL merges two: the lines below move by one, as they do on paper. `fxtest
	--streak` measures only errors that do neither, and says how many it skipped.
*/
namespace fax::codec
{

constexpr int kWidth     = 1728;
constexpr int kLineBytes = kWidth / 8;

enum Colour : int
{
	kWhite = 0,
	kBlack = 1
};

enum Coding : int
{
	kMH = 0,
	kMR = 1
};

enum Concealment : int
{
	kConcealOff    = 0,
	kConcealRepeat = 1
};

/// Test hooks. Always 0 in the plugin; `fxtest --negative` sets them so every
/// check can be shown to FAIL.
enum Perturb : int
{
	kPerturbNoResync     = 1 << 0,///< decoder: count pels, never resynchronise on EOL
	kPerturbNoRefresh    = 1 << 1,///< coder: MR with no one-dimensional line after the first
	kPerturbConcealWrong = 1 << 2,///< decoder: conceal with the line TWO above
	kPerturbNoFill       = 1 << 3,///< coder: no fill, so no minimum scan-line time
	kPerturbFlatTiming   = 1 << 4,///< page timing: every line the page's mean duration
	kPerturbVerticalSkew = 1 << 5,///< coder: VR1 written as VR2 (a wrong mode, not a wrong table)
	kPerturbTableSwap    = 1 << 6,///< tables: white 5 and white 8 exchanged (a transcription slip)
	kPerturbScanBt601    = 1 << 7,///< scan shader: BT.601 luma weights, not BT.709
	kPerturbPrintNearest = 1 << 8,///< print shader: the nearest pel, not the pel coverage
};

/// A page: `lines` lines of `kLineBytes` bytes.
struct Page
{
	int lines = 0;
	std::vector< uint8_t > bytes;

	void Resize( int lineCount );
	void Clear();///< all white
	uint8_t* Row( int line )
	{
		return bytes.data() + static_cast< size_t >( line ) * kLineBytes;
	}
	const uint8_t* Row( int line ) const
	{
		return bytes.data() + static_cast< size_t >( line ) * kLineBytes;
	}
	int Pel( int line, int x ) const
	{
		return ( Row( line )[ x >> 3 ] >> ( x & 7 ) ) & 1;
	}
	void SetPel( int line, int x, int colour );
};

/// A bit stream, one byte a bit (0 or 1), first transmitted bit first.
using Bits = std::vector< uint8_t >;

/// What the coder wrote for one scan line.
struct LineRecord
{
	size_t dataStart = 0;///< first bit of the line's data
	size_t dataBits  = 0;///< the codes themselves
	size_t fillBits  = 0;///< zeros before the EOL
	size_t totalBits = 0;///< data + fill + EOL (+ tag): what the line costs the channel
	bool oneD        = true;
};

struct EncodeOptions
{
	int coding      = kMH;
	int k           = 2;///< MR: one line in k is one-dimensional
	int minLineBits = 0;///< the minimum scan-line time, in bits at the line's rate
	int perturb     = 0;
	int* modeCounts = nullptr;///< for the harness: 2-D modes used, indexed by t4::Mode
};

struct Transmission
{
	Bits bits;
	size_t preambleBits = 0;///< the EOL (+ tag) before line 0
	std::vector< LineRecord > lines;
	size_t rtcStart = 0;
};

/// Code a page. Resets `out`.
void Encode( const Page& page, const EncodeOptions& options, Transmission& out );

/// The pieces, for the harness.
void PutBits( Bits& out, const char* bits );
/// One run of `colour`, as make-up codes and one terminating code.
void PutRun( Bits& out, int colour, int run, int perturb = 0 );
void EncodeLine1D( const uint8_t* line, Bits& out, int perturb = 0 );
/// `reference` may be nullptr: the imaginary all-white line above the first.
/// `modeCounts`, if given, counts the modes used (indices of t4::Mode).
void EncodeLine2D( const uint8_t* line, const uint8_t* reference, Bits& out, int perturb = 0, int* modeCounts = nullptr );

struct DecodeOptions
{
	int coding      = kMH;
	int concealment = kConcealRepeat;
	int perturb     = 0;
};

/// What the decoder made of the stream.
struct Decoded
{
	Page page;                    ///< exactly the requested line count
	std::vector< uint8_t > bad;   ///< the line failed to decode
	std::vector< uint8_t > shown; ///< 0 decoded, 1 concealed (a copy), 2 never arrived (white)
	int segments = 0;             ///< lines found between EOLs, RTC excluded
};

enum Shown : uint8_t
{
	kShownDecoded   = 0,
	kShownConcealed = 1,
	kShownMissing   = 2
};

/// Decode a stream into `lines` lines. Resets `out`.
void Decode( const Bits& bits, int lines, const DecodeOptions& options, Decoded& out );

/// Decode one line's stretch [begin, end). `reference` nullptr means 1-D (or
/// the imaginary white line for a 2-D line). Returns true if the line is good.
/// `pelAtBit`, if given, receives for every bit in the stretch the pel position
/// (a0, clamped to 0) the decoder stood at when the code containing that bit
/// began -- what `--streak` compares the first damaged pel against.
bool DecodeLine( const Bits& bits, size_t begin, size_t end, bool twoD, const uint8_t* reference, uint8_t* out,
                 std::vector< int >* pelAtBit = nullptr );

/// Every EOL in the stream, as the index of its final 1 bit.
std::vector< size_t > FindEols( const Bits& bits );

/// The run lengths of one colour's code table, for the harness: every code as
/// (value, bits, kind 0 terminating / 1 make-up). Honours kPerturbTableSwap.
struct TableEntry
{
	int value;
	const char* bits;
	int makeup;
};
std::vector< TableEntry > Table( int colour, int perturb = 0 );

/// Decode one run of `colour` (make-ups then a terminating code) at `pos`;
/// returns the run or -1 for an invalid code. For `--tables`.
int ReadRun( const Bits& bits, size_t& pos, size_t end, int colour );

} // namespace fax::codec
