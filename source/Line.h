#pragma once

#include "Codec.h"

#include <cstdint>
#include <vector>

/**
	The telephone line: bit errors on the way, and the time the bits take.

	**Errors.** Every transmitted bit -- data, fill, EOL, tag and RTC alike --
	flips when an integer hash of ( page seed, line, bit offset ) falls under the
	bit error rate. PCG's output mix (O'Neill 2014), so the same page on the same
	settings breaks the same way on any machine, and a re-render of a frame is
	the same picture. With `Bursts` above zero an error event becomes a burst:
	the first bit flips and each of the next ( burst - 1 ) flips with
	probability one half, which is roughly what a click on the line does to a
	modem's decisions. The bit error rate is then the rate of EVENTS.

	**Time.** A scan line takes its bit count over the rate: data, fill, EOL and
	tag, which T.4 floors at the minimum scan-line time with fill (4.1.3), so
	"bits / baud" already includes the floor. Line i has arrived when the
	receiver has its EOL: at ( preamble + sum of lines 0..i ) / baud after the
	page began. The page ends with its RTC.
*/
namespace fax::line
{

/// PCG output mix: exact in 32 bits, the same on every machine.
uint32_t Hash( uint32_t value );

struct Noise
{
	double ber      = 0.0;
	int burstBits   = 1;
	uint32_t seed   = 0;
};

/// A bit to flip whatever the noise says: line `line` (-1 the preamble), at
/// `offset` bits from that line's first data bit (from the start of the stream
/// for the preamble). For the harness.
struct ForcedError
{
	int line      = 0;
	size_t offset = 0;
};

/// Flip bits in place. Returns how many flipped.
size_t Corrupt( codec::Transmission& t, const Noise& noise, const std::vector< ForcedError >& forced );

struct Timing
{
	std::vector< double > arrival;///< seconds after the page began, per line
	double duration = 0.0;        ///< the whole stream, RTC included
};

void Schedule( const codec::Transmission& t, int baud, int perturb, Timing& out );

/// Lines that have arrived `elapsed` seconds into the page.
int Arrived( const Timing& timing, double elapsed );

} // namespace fax::line
