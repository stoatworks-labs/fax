#include "Line.h"

#include <algorithm>
#include <cmath>

namespace fax::line
{

uint32_t Hash( uint32_t value )
{
	const uint32_t state = value * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

size_t Corrupt( codec::Transmission& t, const Noise& noise, const std::vector< ForcedError >& forced )
{
	size_t flips = 0;
	codec::Bits& bits = t.bits;

	for( const ForcedError& f : forced )
	{
		size_t at = f.offset;
		if( f.line >= 0 )
		{
			if( static_cast< size_t >( f.line ) >= t.lines.size() )
				continue;
			at += t.lines[ static_cast< size_t >( f.line ) ].dataStart;
		}
		if( at < bits.size() )
		{
			bits[ at ] ^= 1;
			++flips;
		}
	}

	if( noise.ber <= 0.0 )
		return flips;

	const double scaled    = std::min( noise.ber, 1.0 ) * 4294967296.0;
	const uint32_t cutoff  = scaled >= 4294967295.0 ? 0xFFFFFFFFu : static_cast< uint32_t >( scaled );
	const int burst        = std::max( 1, noise.burstBits );

	//One region per line (its data, fill, EOL and tag), plus the preamble and
	//the RTC, each seeded on its own.
	auto region = [ & ]( uint32_t id, size_t begin, size_t end ) {
		const uint32_t seed = Hash( noise.seed ^ Hash( id + 0x9E3779B9u ) );
		for( size_t o = begin; o < end; ++o )
		{
			if( Hash( seed + static_cast< uint32_t >( o - begin ) ) >= cutoff )
				continue;
			bits[ o ] ^= 1;
			++flips;
			for( int i = 1; i < burst && o + static_cast< size_t >( i ) < bits.size(); ++i )
				if( Hash( seed ^ ( static_cast< uint32_t >( o - begin + static_cast< size_t >( i ) ) * 2654435761u ) ) & 1u )
				{
					bits[ o + static_cast< size_t >( i ) ] ^= 1;
					++flips;
				}
			o += static_cast< size_t >( burst - 1 );
		}
	};

	region( 0xFFFFFFFFu, 0, t.preambleBits );
	for( size_t j = 0; j < t.lines.size(); ++j )
	{
		const codec::LineRecord& r = t.lines[ j ];
		region( static_cast< uint32_t >( j ), r.dataStart, std::min( bits.size(), r.dataStart + r.totalBits ) );
	}
	region( 0xFFFFFFFEu, t.rtcStart, bits.size() );
	return flips;
}

void Schedule( const codec::Transmission& t, int baud, int perturb, Timing& out )
{
	const double rate = static_cast< double >( std::max( baud, 1 ) );
	out.arrival.resize( t.lines.size() );
	size_t cumulative = t.preambleBits;
	size_t total      = 0;
	for( const auto& r : t.lines )
		total += r.totalBits;
	for( size_t i = 0; i < t.lines.size(); ++i )
	{
		cumulative += t.lines[ i ].totalBits;
		out.arrival[ i ] = static_cast< double >( cumulative ) / rate;
		if( ( perturb & codec::kPerturbFlatTiming ) != 0 )
			out.arrival[ i ] = ( static_cast< double >( t.preambleBits )
			                     + static_cast< double >( total ) * static_cast< double >( i + 1 ) / static_cast< double >( t.lines.size() ) )
			                   / rate;
	}
	out.duration = static_cast< double >( t.bits.size() ) / rate;
}

int Arrived( const Timing& timing, double elapsed )
{
	return static_cast< int >( std::upper_bound( timing.arrival.begin(), timing.arrival.end(), elapsed ) - timing.arrival.begin() );
}

} // namespace fax::line
