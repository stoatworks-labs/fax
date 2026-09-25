#pragma once

/**
	Host parameters are 0..1 (sliders and booleans) or element indices
	(options); these are what they mean. An option's range reads back 0..1 from
	the SDK whatever its element count, so options are mapped by INDEX here and
	nowhere else.
*/
namespace fax::controls
{

/// An option's value to its index, rounded and clamped.
int OptionIndex( float value, int count );

/// Coding, in menu order: MH (one-dimensional), MR (two-dimensional).
constexpr int kCodingCount = 2;
const char* CodingName( int index );

/// Baud: the rates of V.27 ter, V.29 and V.17.
constexpr int kBaudCount = 4;
const char* BaudName( int index );
int BaudRate( int index );

/// Mode: Page (a page captured and sent at the line rate) or Live (every frame
/// a new page, whole, no timing).
enum Mode : int
{
	kModePage = 0,
	kModeLive = 1,
	kModeCount
};
const char* ModeName( int index );

constexpr int kConcealmentCount = 2;
const char* ConcealmentName( int index );

enum Paper : int
{
	kPaperThermal = 0,
	kPaperPlain   = 1,
	kPaperCount
};
const char* PaperName( int index );

/// Line Noise: 0 is a clean line; above it the bit error rate climbs
/// logarithmically from 1e-6 to 1e-2 at 1. The exponent is a stated choice.
double BitErrorRate( float value );

/// Bursts: 0 single-bit errors; above it each error event is a burst of up to
/// 64 bits in which each bit flips with probability one half.
int BurstBits( float value );

/// Threshold: the luma below which a pel prints black. Linear, 0..1.
double Threshold( float value );

/// The minimum transmission time of a coded scan line (4.1.3/T.4; the
/// terminals negotiate 0, 5, 10, 20 or 40 ms in T.30). 10 ms, stated.
constexpr double kMinScanLineSeconds = 0.010;

/// Paper and toner, sRGB-encoded.
struct Colours
{
	float paper[ 3 ];
	float ink[ 3 ];
};
Colours PaperColours( int paper );

/// The desk the A4 sheet lies on.
constexpr float kDesk[ 3 ] = { 0.0f, 0.0f, 0.0f };

} // namespace fax::controls
