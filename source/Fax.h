#pragma once

#include "Codec.h"
#include "Layout.h"
#include "Line.h"
#include "PassBuffer.h"
#include "StoatworksAboutParams.h"

#include <FFGLSDK.h>

#include <string>
#include <vector>

/**
	Fax -- the picture sent as a Group 3 fax over a noisy line, as an FFGL
	effect.

	**The one idea.** A Group 3 fax (ITU-T T.4) does not send pixels. It
	thresholds each scan line to black and white and sends RUN LENGTHS as
	variable-length codes, a line at a time, each ended by an EOL; in MR most
	lines are sent as differences from the line above. So the clip goes through
	a real coder, a real noisy line and a real decoder, and the look of a fax
	falls out of the code: a bit error desynchronises the rest of its line into
	a streak to the right edge; in MR the damage runs down until the next
	one-dimensional line; a decoder that finds a bad line repeats the one above;
	and a busy page takes longer to arrive than a blank one, because white runs
	are cheap.

	One GPU pass scans the frame to 1728 pels a line, packed eight to a byte,
	and it is read back; the coder (`Codec.h`), the line (`Line.h`) and the
	decoder run on the CPU; the received page is uploaded and a second pass
	prints it. See AGENTS.md.
*/
class Fax : public CFFGLPlugin
{
public:
	Fax();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//--- test hooks: the harness's view of the receiver ----------------------
	/// The harness DECLARES the clock's unit rather than leaving the voting
	/// to infer one.
	void SetClockScaleForTest( double scale );
	/// A bitmask of `codec::Perturb`. Always 0 in the plugin.
	void SetPerturbForTest( int bits );
	/// Bits to flip on every page, whatever the noise says.
	void SetForcedErrorsForTest( const std::vector< fax::line::ForcedError >& errors );

	const fax::codec::Page& ScannedForTest() const { return scanned; }
	const fax::codec::Bits& SentForTest() const { return sentBits; }
	const fax::codec::Transmission& ReceivedForTest() const { return transmission; }
	const fax::codec::Decoded& DecodedForTest() const { return decoded; }
	const fax::codec::Page& DisplayForTest() const { return display; }
	const fax::line::Timing& TimingForTest() const { return timing; }
	const fax::layout::Geometry& GeometryForTest() const { return geometry; }
	int ArrivedForTest() const { return shownLines; }
	int PagesForTest() const { return pageIndex; }
	double PageStartForTest() const { return pageStart; }

	/// What the CPU half cost on the last page start, in milliseconds.
	struct CpuCost
	{
		double readback = 0.0, code = 0.0, channel = 0.0, decode = 0.0;
	};
	CpuCost LastCpuCostForTest() const { return cpuCost; }

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Scan
		PT_FIT,
		PT_RESOLUTION,
		PT_THRESHOLD,
		PT_HALFTONE,

		//Line
		PT_CODING,
		PT_LINE_NOISE,
		PT_BURSTS,
		PT_BAUD,
		PT_MODE,

		//Receiver
		PT_CONCEALMENT,
		PT_PAPER,
		PT_HEADER,
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	/// The host's clock in seconds, whatever unit it arrived in.
	double nowSeconds();

	/// Scan, code, send and decode a new page. False if GL failed.
	bool startPage( const FFGLTextureStruct& input, double now );
	void uploadDisplay( int fromLine, int toLine );

	ffglex::FFGLShader scanShader;
	ffglex::FFGLShader printShader;
	ffglex::FFGLScreenQuad quad;

	fax::PassBuffer scanBuffer;///< 54 x lines, RGBA8: the scanned page's bits
	GLuint pageTexture = 0;    ///< 54 x lines, RGBA8: the page as the receiver shows it
	int pageTextureLines = 0;

	//--- the page in flight ---------------------------------------------------
	fax::layout::Geometry geometry;
	fax::codec::Page scanned;             ///< what the sender coded (header included)
	fax::codec::Bits sentBits;            ///< the stream before the line
	fax::codec::Transmission transmission;///< the stream after it
	fax::codec::Decoded decoded;          ///< what the receiver made of it
	fax::codec::Page display;             ///< the arriving page over the last one
	fax::line::Timing timing;
	bool pageActive  = false;
	double pageStart = 0.0;
	int pageIndex    = 0;
	int shownLines   = 0;
	int pageFit      = -1;
	int pageResolution = -1;
	int pageMode     = -1;
	CpuCost cpuCost;

	//--- the clock (readout's unit voting, by way of teletext) ---------------
	bool hostTimeSeen   = false;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double wallStart    = -1.0;
	double lastWallTime = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	int clockFrames     = 0;

	int perturb = 0;
	std::vector< fax::line::ForcedError > forced;

	/// Zero-initialised: the About block's ids are never stored to, so
	/// without this GetFloatParameter hands the host whatever was on the
	/// stack for them.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
