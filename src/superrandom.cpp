/*
 * Super Random - Multi-channel random CV generator for disting NT
 *
 * Generates up to 24 CV outputs with per-channel configuration:
 *   - Random type: Stepped (sample & hold) or Smooth (slewed)
 *   - Polarity: Bipolar (+/-) or Unipolar (0/+)
 *   - Range: configurable voltage range per channel
 *   - Loop: optional step looping with configurable length
 *   - Skip: probability of holding current value on a trigger
 *
 * A trigger on the configurable trigger input advances all channels.
 */

#include <new>
#include <distingnt/api.h>
#include <distingnt/serialisation.h>

// ─── Constants ───

static constexpr int kMaxChannels = 24;
static constexpr int kMaxBusses = kNT_lastBus;
static constexpr int kMaxLoopSteps = 64;

// ─── PRNG ───

struct Xorshift32
{
	uint32_t state;

	Xorshift32() : state( 123456789 ) {}

	void seed( uint32_t s ) { state = s ? s : 1; }

	uint32_t next()
	{
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		return state;
	}

	// Returns float in [0, 1]
	float nextUnipolar()
	{
		return (float)next() / 4294967295.0f;
	}
};

// ─── Enums ───

enum RandomType
{
	kTypeStepped,
	kTypeSmooth,
	kNumTypes
};

static char const * const typeStrings[] = { "Stepped", "Smooth" };

enum Polarity
{
	kBipolar,
	kUnipolar,
	kNumPolarities
};

static char const * const polarityStrings[] = { "Bipolar", "Unipolar" };

// ─── Specification ───

enum SpecIndex
{
	kSpecChannels = 0,
	kNumSpecs
};

static const _NT_specification specs[] = {
	{ .name = "Channels", .min = 1, .max = kMaxChannels, .def = 4, .type = kNT_typeGeneric },
};

// ─── Parameter layout ───
//
// Global params:
//   0: Trigger input
//   1: Slew rate
//
// Per-channel params:
//   0: CV output bus
//   1: CV output mode
//   2: Type (Stepped / Smooth)
//   3: Polarity (Bipolar / Unipolar)
//   4: Range (volts, scaled /10)
//   5: Loop steps (0 = off)
//   6: Skip %

enum
{
	kParamTriggerInput = 0,
	kParamSlewRate,

	kNumGlobalParams
};

enum
{
	kPerChOutput = 0,
	kPerChOutputMode,
	kPerChType,
	kPerChPolarity,
	kPerChRange,
	kPerChLoopSteps,
	kPerChSkip,

	kNumPerChParams
};

static constexpr int kMaxParams = kNumGlobalParams + kMaxChannels * kNumPerChParams;

// ─── Channel names ───

static const char* const outputNames[] = {
	"CV 1",  "CV 2",  "CV 3",  "CV 4",  "CV 5",  "CV 6",
	"CV 7",  "CV 8",  "CV 9",  "CV 10", "CV 11", "CV 12",
	"CV 13", "CV 14", "CV 15", "CV 16", "CV 17", "CV 18",
	"CV 19", "CV 20", "CV 21", "CV 22", "CV 23", "CV 24",
};

static const char* const pageNames[] = {
	"Ch 1",  "Ch 2",  "Ch 3",  "Ch 4",  "Ch 5",  "Ch 6",
	"Ch 7",  "Ch 8",  "Ch 9",  "Ch 10", "Ch 11", "Ch 12",
	"Ch 13", "Ch 14", "Ch 15", "Ch 16", "Ch 17", "Ch 18",
	"Ch 19", "Ch 20", "Ch 21", "Ch 22", "Ch 23", "Ch 24",
};

// ─── Display constants ───

// Flash counters decrement each draw() call (~60Hz refresh).
// At 60fps, 12 frames ≈ 200ms flash duration.
static constexpr int kFlashDuration = 12;

// ─── Per-channel state ───

struct ChannelState
{
	float currentValue;     // current output voltage
	float targetValue;      // target for smooth interpolation

	// Loop buffer
	float loopBuffer[kMaxLoopSteps];
	int loopPos;            // current position in loop (0-based)
	int loopFilled;         // how many steps have been recorded

	// Display state (written by step(), read by draw())
	uint8_t triggerFlash;   // countdown for trigger flash
	uint8_t skipFlash;      // countdown for skip flash
};

// ─── Algorithm struct ───

struct SuperRandom : public _NT_algorithm
{
	SuperRandom( int numCh ) : numChannels( numCh ) {}

	int numChannels;

	ChannelState channels[kMaxChannels];

	// Trigger detection
	bool triggerHigh;

	// PRNG
	Xorshift32 rng;

	// Cached slew coefficient
	float slewCoeff;

	// Dynamic parameter storage
	_NT_parameter params[kMaxParams];
	uint8_t globalPageParams[kNumGlobalParams];
	uint8_t chPageParams[kMaxChannels][kNumPerChParams];
	_NT_parameterPage pageDefs[1 + kMaxChannels];
	_NT_parameterPages pagesDef;
};

// ─── Helper ───

static inline void setParam( _NT_parameter& p, const char* name, int16_t min, int16_t max,
                              int16_t def, uint8_t unit, uint8_t scaling = 0,
                              char const * const * enumStr = nullptr )
{
	p.name = name;
	p.min = min;
	p.max = max;
	p.def = def;
	p.unit = unit;
	p.scaling = scaling;
	p.enumStrings = enumStr;
}

// ─── Factory callbacks ───

static void calculateRequirements( _NT_algorithmRequirements& req, const int32_t* specifications )
{
	int numChannels = specifications[kSpecChannels];
	req.numParameters = kNumGlobalParams + numChannels * kNumPerChParams;
	req.sram = sizeof( SuperRandom );
	req.dram = 0;
	req.dtc = 0;
	req.itc = 0;
}

static _NT_algorithm* construct( const _NT_algorithmMemoryPtrs& ptrs,
                                 const _NT_algorithmRequirements& req,
                                 const int32_t* specifications )
{
	int numChannels = specifications[kSpecChannels];
	SuperRandom* self = new (ptrs.sram) SuperRandom( numChannels );

	// Init state
	self->triggerHigh = false;
	self->slewCoeff = 0.001f;
	self->rng.seed( 42 );

	for ( int i = 0; i < kMaxChannels; ++i )
	{
		self->channels[i].currentValue = 0.0f;
		self->channels[i].targetValue = 0.0f;
		self->channels[i].loopPos = 0;
		self->channels[i].loopFilled = 0;
		self->channels[i].triggerFlash = 0;
		self->channels[i].skipFlash = 0;
		for ( int j = 0; j < kMaxLoopSteps; ++j )
			self->channels[i].loopBuffer[j] = 0.0f;
	}

	// ─── Build parameters ───

	// Global: trigger input
	setParam( self->params[kParamTriggerInput], "Trigger", 0, kMaxBusses, 0, kNT_unitCvInput );

	// Global: slew rate
	setParam( self->params[kParamSlewRate], "Slew", 1, 1000, 100, kNT_unitPercent );

	// Per-channel parameters
	for ( int ch = 0; ch < numChannels; ++ch )
	{
		int base = kNumGlobalParams + ch * kNumPerChParams;

		setParam( self->params[base + kPerChOutput], outputNames[ch], 1, kMaxBusses, 13 + ch, kNT_unitCvOutput );
		setParam( self->params[base + kPerChOutputMode], "mode", 0, 1, 1, kNT_unitOutputMode );
		setParam( self->params[base + kPerChType], "Type", 0, kNumTypes - 1, kTypeStepped, kNT_unitEnum );
		self->params[base + kPerChType].enumStrings = typeStrings;
		setParam( self->params[base + kPerChPolarity], "Polarity", 0, kNumPolarities - 1, kBipolar, kNT_unitEnum );
		self->params[base + kPerChPolarity].enumStrings = polarityStrings;
		// Range: 0.1V to 10.0V, stored as 1-100 with /10 scaling
		setParam( self->params[base + kPerChRange], "Range", 1, 100, 50, kNT_unitVolts, kNT_scaling10 );
		// Loop steps: 0 = off (free-running), 1-64 = loop length
		setParam( self->params[base + kPerChLoopSteps], "Loop", 0, kMaxLoopSteps, 0, kNT_unitNone );
		// Skip: 0-100%
		setParam( self->params[base + kPerChSkip], "Skip %", 0, 100, 0, kNT_unitPercent );
	}

	// ─── Build parameter pages ───

	int pageIdx = 0;

	// Global page
	self->globalPageParams[0] = kParamTriggerInput;
	self->globalPageParams[1] = kParamSlewRate;
	self->pageDefs[pageIdx].name = "Global";
	self->pageDefs[pageIdx].numParams = kNumGlobalParams;
	self->pageDefs[pageIdx].group = 0;
	self->pageDefs[pageIdx].unused[0] = 0;
	self->pageDefs[pageIdx].unused[1] = 0;
	self->pageDefs[pageIdx].params = self->globalPageParams;
	pageIdx++;

	// Per-channel pages
	for ( int ch = 0; ch < numChannels; ++ch )
	{
		int base = kNumGlobalParams + ch * kNumPerChParams;
		for ( int j = 0; j < kNumPerChParams; ++j )
			self->chPageParams[ch][j] = base + j;

		self->pageDefs[pageIdx].name = pageNames[ch];
		self->pageDefs[pageIdx].numParams = kNumPerChParams;
		self->pageDefs[pageIdx].group = 1;
		self->pageDefs[pageIdx].unused[0] = 0;
		self->pageDefs[pageIdx].unused[1] = 0;
		self->pageDefs[pageIdx].params = self->chPageParams[ch];
		pageIdx++;
	}

	self->pagesDef.numPages = pageIdx;
	self->pagesDef.pages = self->pageDefs;

	self->parameters = self->params;
	self->parameterPages = &self->pagesDef;

	return self;
}

static void parameterChanged( _NT_algorithm* self, int p )
{
	SuperRandom* pThis = static_cast<SuperRandom*>( self );

	if ( p == kParamSlewRate )
	{
		float pct = pThis->v[kParamSlewRate] / 1000.0f;
		pThis->slewCoeff = 0.00005f + pct * pct * 0.05f;
	}

	// When loop steps changes, reset loop state for that channel
	if ( p >= kNumGlobalParams )
	{
		int chParam = ( p - kNumGlobalParams ) % kNumPerChParams;
		if ( chParam == kPerChLoopSteps )
		{
			int ch = ( p - kNumGlobalParams ) / kNumPerChParams;
			pThis->channels[ch].loopPos = 0;
			pThis->channels[ch].loopFilled = 0;
		}
	}
}

static void step( _NT_algorithm* self, float* busFrames, int numFramesBy4 )
{
	SuperRandom* pThis = static_cast<SuperRandom*>( self );
	int numFrames = numFramesBy4 * 4;

	// Get trigger input
	int trigBus = pThis->v[kParamTriggerInput];
	const float* trigIn = ( trigBus > 0 ) ? busFrames + ( trigBus - 1 ) * numFrames : nullptr;

	// Cache per-channel params
	float* outputs[kMaxChannels];
	bool replace[kMaxChannels];
	int types[kMaxChannels];
	int polarities[kMaxChannels];
	float ranges[kMaxChannels];
	int loopLens[kMaxChannels];
	int skipPcts[kMaxChannels];

	for ( int ch = 0; ch < pThis->numChannels; ++ch )
	{
		int base = kNumGlobalParams + ch * kNumPerChParams;
		int outBus = pThis->v[base + kPerChOutput];
		outputs[ch] = ( outBus > 0 ) ? busFrames + ( outBus - 1 ) * numFrames : nullptr;
		replace[ch] = pThis->v[base + kPerChOutputMode];
		types[ch] = pThis->v[base + kPerChType];
		polarities[ch] = pThis->v[base + kPerChPolarity];
		ranges[ch] = pThis->v[base + kPerChRange] / 10.0f; // stored *10
		loopLens[ch] = pThis->v[base + kPerChLoopSteps];
		skipPcts[ch] = pThis->v[base + kPerChSkip];
	}

	float slewCoeff = pThis->slewCoeff;

	for ( int i = 0; i < numFrames; ++i )
	{
		// Trigger detection (rising edge above 1V)
		bool triggered = false;
		if ( trigIn )
		{
			bool high = trigIn[i] > 1.0f;
			if ( high && !pThis->triggerHigh )
				triggered = true;
			pThis->triggerHigh = high;
		}

		if ( triggered )
		{
			for ( int ch = 0; ch < pThis->numChannels; ++ch )
			{
				ChannelState& cs = pThis->channels[ch];
				int loopLen = loopLens[ch];
				float range = ranges[ch];
				bool unipolar = ( polarities[ch] == kUnipolar );

				// Skip check: roll a random number against skip percentage
				if ( skipPcts[ch] > 0 )
				{
					int roll = (int)( pThis->rng.nextUnipolar() * 100.0f );
					if ( roll < skipPcts[ch] )
					{
						cs.skipFlash = kFlashDuration;
						goto write_output;
					}
				}

				cs.triggerFlash = kFlashDuration;

				if ( loopLen == 0 )
				{
					// Free-running: generate fresh random value each trigger
					float raw = pThis->rng.nextUnipolar();
					float newVal;
					if ( unipolar )
						newVal = raw * range;
					else
						newVal = ( raw * 2.0f - 1.0f ) * range;

					cs.targetValue = newVal;
					if ( types[ch] == kTypeStepped )
						cs.currentValue = newVal;
				}
				else
				{
					// Looping mode
					if ( cs.loopFilled < loopLen )
					{
						// Still filling the loop buffer
						float raw = pThis->rng.nextUnipolar();
						float newVal;
						if ( unipolar )
							newVal = raw * range;
						else
							newVal = ( raw * 2.0f - 1.0f ) * range;

						cs.loopBuffer[cs.loopFilled] = newVal;
						cs.loopFilled++;
						cs.targetValue = newVal;
						if ( types[ch] == kTypeStepped )
							cs.currentValue = newVal;
					}
					else
					{
						// Loop is full, cycle through it
						float newVal = cs.loopBuffer[cs.loopPos];
						cs.loopPos = ( cs.loopPos + 1 ) % loopLen;
						cs.targetValue = newVal;
						if ( types[ch] == kTypeStepped )
							cs.currentValue = newVal;
					}
				}

				write_output: (void)0;
			}
		}

		// Update smooth channels and write outputs
		for ( int ch = 0; ch < pThis->numChannels; ++ch )
		{
			ChannelState& cs = pThis->channels[ch];

			if ( types[ch] == kTypeSmooth )
			{
				float diff = cs.targetValue - cs.currentValue;
				cs.currentValue += diff * slewCoeff;
			}

			if ( outputs[ch] )
			{
				if ( replace[ch] )
					outputs[ch][i] = cs.currentValue;
				else
					outputs[ch][i] += cs.currentValue;
			}
		}
	}
}

static int parameterUiPrefix( _NT_algorithm* self, int p, char* buff )
{
	if ( p >= kNumGlobalParams )
	{
		int ch = ( p - kNumGlobalParams ) / kNumPerChParams;
		int len = NT_intToString( buff, ch + 1 );
		buff[len++] = ':';
		buff[len] = 0;
		return len;
	}
	return 0;
}

static bool draw( _NT_algorithm* self )
{
	SuperRandom* pThis = static_cast<SuperRandom*>( self );

	int numCh = pThis->numChannels;

	// ─── Adaptive layout ───
	// Screen is 256x64. Leave margin for readability.
	// Bars occupy the vertical middle; loop dots go below.

	int barWidth = 240 / numCh;
	if ( barWidth < 2 )
		barWidth = 2;
	if ( barWidth > 40 )
		barWidth = 40;
	int gap = ( barWidth > 4 ) ? 2 : 1; // gap between bars
	int totalWidth = barWidth * numCh;
	int startX = ( 256 - totalWidth ) / 2;

	// Vertical layout
	int barTop = 4;
	int barBottom = 52;
	int barMidY = ( barTop + barBottom ) / 2;    // 28
	int maxBarHalf = barMidY - barTop;            // 24
	int loopDotsY = 58;                           // row for loop position dots

	// ─── Center line ───
	NT_drawShapeI( kNT_line, startX, barMidY, startX + totalWidth - 1, barMidY, 2 );

	for ( int ch = 0; ch < numCh; ++ch )
	{
		int base = kNumGlobalParams + ch * kNumPerChParams;
		ChannelState& cs = pThis->channels[ch];

		int x0 = startX + ch * barWidth;
		int x1 = x0 + barWidth - gap - 1;
		if ( x1 <= x0 )
			x1 = x0 + 1;
		int xMid = ( x0 + x1 ) / 2;

		float range = pThis->v[base + kPerChRange] / 10.0f;
		if ( range < 0.1f ) range = 0.1f;
		bool isSmooth = ( pThis->v[base + kPerChType] == kTypeSmooth );
		bool isUnipolar = ( pThis->v[base + kPerChPolarity] == kUnipolar );
		int loopLen = pThis->v[base + kPerChLoopSteps];

		// ─── Normalize value for display ───
		float normalized;
		if ( isUnipolar )
		{
			// Unipolar: 0 to +range → map to 0..1
			normalized = cs.currentValue / range;
			if ( normalized < 0.0f ) normalized = 0.0f;
			if ( normalized > 1.0f ) normalized = 1.0f;
		}
		else
		{
			// Bipolar: -range to +range → map to -1..1
			normalized = cs.currentValue / range;
			if ( normalized < -1.0f ) normalized = -1.0f;
			if ( normalized > 1.0f ) normalized = 1.0f;
		}

		// ─── Determine bar colour ───
		// Item 2: trigger flash = bright spike, skip flash = dim
		// Item 5: stepped = filled, smooth = outlined (hollow)
		int colour;
		if ( cs.triggerFlash > 0 )
		{
			colour = 15; // bright flash on trigger
			cs.triggerFlash--;
		}
		else if ( cs.skipFlash > 0 )
		{
			colour = 4; // dim flash on skip
			cs.skipFlash--;
		}
		else
		{
			colour = isSmooth ? 12 : 9;
		}

		// ─── Draw bar ───
		if ( isUnipolar )
		{
			// Draw upward from bottom baseline
			int baseline = barBottom;
			int barHeight = (int)( normalized * ( barBottom - barTop ) );
			if ( barHeight < 0 ) barHeight = 0;
			int barTopEdge = baseline - barHeight;

			if ( barHeight > 0 )
			{
				if ( isSmooth )
				{
					// Item 5: outlined/hollow bar for smooth
					NT_drawShapeI( kNT_box, x0, barTopEdge, x1, baseline, colour );
				}
				else
				{
					// Item 5: filled bar for stepped
					NT_drawShapeI( kNT_rectangle, x0, barTopEdge, x1, baseline, colour );
				}
			}
			else
			{
				NT_drawShapeI( kNT_line, x0, baseline, x1, baseline, colour );
			}

			// Baseline indicator
			NT_drawShapeI( kNT_line, x0, baseline, x1, baseline, 3 );
		}
		else
		{
			// Bipolar: draw from center line
			int barHeight = (int)( normalized * maxBarHalf );

			if ( barHeight > 0 )
			{
				if ( isSmooth )
					NT_drawShapeI( kNT_box, x0, barMidY - barHeight, x1, barMidY, colour );
				else
					NT_drawShapeI( kNT_rectangle, x0, barMidY - barHeight, x1, barMidY, colour );
			}
			else if ( barHeight < 0 )
			{
				if ( isSmooth )
					NT_drawShapeI( kNT_box, x0, barMidY, x1, barMidY - barHeight, colour );
				else
					NT_drawShapeI( kNT_rectangle, x0, barMidY, x1, barMidY - barHeight, colour );
			}
			else
			{
				NT_drawShapeI( kNT_line, x0, barMidY, x1, barMidY, colour );
			}
		}

		// ─── Item 1: Loop position indicator ───
		if ( loopLen > 0 )
		{
			int dotAreaWidth = x1 - x0 + 1;

			if ( numCh <= 6 && dotAreaWidth >= loopLen * 2 )
			{
				// Wide layout: draw individual dots for each loop step
				int dotSpacing = dotAreaWidth / loopLen;
				int dotStartX = x0 + ( dotAreaWidth - dotSpacing * loopLen ) / 2;
				for ( int s = 0; s < loopLen; ++s )
				{
					int dx = dotStartX + s * dotSpacing + dotSpacing / 2;
					int dotColour;
					if ( s < cs.loopFilled )
					{
						// Filled step
						dotColour = ( s == cs.loopPos && cs.loopFilled >= loopLen ) ? 15 : 5;
					}
					else
					{
						dotColour = 2; // unfilled step (still recording)
					}
					NT_drawShapeI( kNT_point, dx, loopDotsY, 0, 0, dotColour );
				}
			}
			else
			{
				// Narrow layout: draw a small progress bar
				int filledWidth;
				if ( cs.loopFilled < loopLen )
				{
					// Still recording: show fill progress
					filledWidth = ( cs.loopFilled * dotAreaWidth ) / loopLen;
					NT_drawShapeI( kNT_line, x0, loopDotsY, x1, loopDotsY, 2 );
					if ( filledWidth > 0 )
						NT_drawShapeI( kNT_line, x0, loopDotsY, x0 + filledWidth - 1, loopDotsY, 6 );
				}
				else
				{
					// Looping: show position as a bright pixel on a dim bar
					NT_drawShapeI( kNT_line, x0, loopDotsY, x1, loopDotsY, 3 );
					int posX = x0 + ( cs.loopPos * dotAreaWidth ) / loopLen;
					if ( posX > x1 ) posX = x1;
					NT_drawShapeI( kNT_point, posX, loopDotsY, 0, 0, 15 );
				}
			}
		}

		// ─── Item 2: Skip "x" marker on skip flash (wide layout only) ───
		if ( cs.skipFlash > 0 && barWidth >= 8 )
		{
			int sz = 2;
			NT_drawShapeI( kNT_line, xMid - sz, barMidY - sz, xMid + sz, barMidY + sz, 8 );
			NT_drawShapeI( kNT_line, xMid - sz, barMidY + sz, xMid + sz, barMidY - sz, 8 );
		}

		// ─── Channel number (wide layout only, item 3) ───
		if ( barWidth >= 12 )
		{
			char numBuf[4];
			NT_intToString( numBuf, ch + 1 );
			NT_drawText( xMid, 0, numBuf, 4, kNT_textCentre, kNT_textTiny );
		}
	}

	return false;
}

// ─── Serialisation ───
// Persists loop buffers, playback positions, current/target values, and PRNG state
// so that loops survive preset save/load.

static void serialise( _NT_algorithm* self, _NT_jsonStream& stream )
{
	SuperRandom* pThis = static_cast<SuperRandom*>( self );

	// Save PRNG state
	stream.addMemberName( "rng" );
	stream.addNumber( (int)pThis->rng.state );

	// Save per-channel state
	stream.addMemberName( "channels" );
	stream.openArray();
	for ( int ch = 0; ch < pThis->numChannels; ++ch )
	{
		ChannelState& cs = pThis->channels[ch];
		stream.openObject();

		stream.addMemberName( "cur" );
		stream.addNumber( cs.currentValue );

		stream.addMemberName( "tgt" );
		stream.addNumber( cs.targetValue );

		stream.addMemberName( "pos" );
		stream.addNumber( cs.loopPos );

		stream.addMemberName( "filled" );
		stream.addNumber( cs.loopFilled );

		stream.addMemberName( "buf" );
		stream.openArray();
		for ( int s = 0; s < cs.loopFilled; ++s )
			stream.addNumber( cs.loopBuffer[s] );
		stream.closeArray();

		stream.closeObject();
	}
	stream.closeArray();
}

static bool deserialise( _NT_algorithm* self, _NT_jsonParse& parse )
{
	SuperRandom* pThis = static_cast<SuperRandom*>( self );

	int numMembers;
	if ( !parse.numberOfObjectMembers( numMembers ) )
		return false;

	for ( int m = 0; m < numMembers; ++m )
	{
		if ( parse.matchName( "rng" ) )
		{
			int rngState;
			if ( !parse.number( rngState ) )
				return false;
			pThis->rng.seed( (uint32_t)rngState );
		}
		else if ( parse.matchName( "channels" ) )
		{
			int numCh;
			if ( !parse.numberOfArrayElements( numCh ) )
				return false;

			for ( int ch = 0; ch < numCh; ++ch )
			{
				if ( ch >= pThis->numChannels )
				{
					// More channels in preset than current instance — skip
					// Each channel is an object, skip it
					if ( !parse.skipMember() )
						return false;
					continue;
				}

				ChannelState& cs = pThis->channels[ch];

				int numFields;
				if ( !parse.numberOfObjectMembers( numFields ) )
					return false;

				for ( int f = 0; f < numFields; ++f )
				{
					if ( parse.matchName( "cur" ) )
					{
						if ( !parse.number( cs.currentValue ) )
							return false;
					}
					else if ( parse.matchName( "tgt" ) )
					{
						if ( !parse.number( cs.targetValue ) )
							return false;
					}
					else if ( parse.matchName( "pos" ) )
					{
						int v;
						if ( !parse.number( v ) )
							return false;
						cs.loopPos = v;
					}
					else if ( parse.matchName( "filled" ) )
					{
						int v;
						if ( !parse.number( v ) )
							return false;
						cs.loopFilled = v;
						if ( cs.loopFilled > kMaxLoopSteps )
							cs.loopFilled = kMaxLoopSteps;
					}
					else if ( parse.matchName( "buf" ) )
					{
						int numSteps;
						if ( !parse.numberOfArrayElements( numSteps ) )
							return false;
						for ( int s = 0; s < numSteps; ++s )
						{
							float val;
							if ( !parse.number( val ) )
								return false;
							if ( s < kMaxLoopSteps )
								cs.loopBuffer[s] = val;
						}
					}
					else
					{
						if ( !parse.skipMember() )
							return false;
					}
				}
			}
		}
		else
		{
			if ( !parse.skipMember() )
				return false;
		}
	}

	return true;
}

// ─── Factory ───

static const _NT_factory factory = {
	.guid = NT_MULTICHAR( 'S', 'R', 'n', 'd' ),
	.name = "Super Random",
	.description = "Multi-channel triggered random CV with loop, skip, range, and slew",
	.numSpecifications = ARRAY_SIZE( specs ),
	.specifications = specs,
	.calculateStaticRequirements = nullptr,
	.initialise = nullptr,
	.calculateRequirements = calculateRequirements,
	.construct = construct,
	.parameterChanged = parameterChanged,
	.step = step,
	.draw = draw,
	.midiRealtime = nullptr,
	.midiMessage = nullptr,
	.tags = kNT_tagUtility,
	.hasCustomUi = nullptr,
	.customUi = nullptr,
	.setupUi = nullptr,
	.serialise = serialise,
	.deserialise = deserialise,
	.midiSysEx = nullptr,
	.parameterUiPrefix = parameterUiPrefix,
};

// ─── Entry point ───

extern "C"
uintptr_t pluginEntry( _NT_selector selector, uint32_t data )
{
	switch ( selector )
	{
	case kNT_selector_version:
		return kNT_apiVersionCurrent;
	case kNT_selector_numFactories:
		return 1;
	case kNT_selector_factoryInfo:
		return ( data == 0 ) ? (uintptr_t)&factory : 0;
	}
	return 0;
}
