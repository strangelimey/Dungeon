// ============================================================================
// bar.hlsl - the procedural resource-bar fills (docs/icon-updates-plan.md).
//
// One quad per bar, covering the glass TUBE of the iron frame (assets/ui/
// bar_frame.png, drawn on top by the caller). The shader paints the whole
// tube: the fluid up to `fraction` and the empty glass past it. Each kind has
// its own look and its own motion:
//   Health  - blood that ebbs and flows, bubbles rising, and a heartbeat (a
//             lub-dub swell that rolls along the tube); the beat's PHASE comes
//             from the CPU so a change of rate never jumps it.
//   Stamina - a green glow that breathes, with motes drifting and a shimmer.
//   Mana    - blue wisps, and lightning that arcs along the tube at intervals.
//   Effort  - the HUD's stance meter, in the caller's colour: a faint drifting
//             grain and a soft sheen that slides along it now and then; once
//             over-exerted, the over-exertion burns across it from the left as
//             a slow ember. The quietest kind on purpose (Michael: "keep the
//             animation low key") - it sits under the hands, in the eye's way.
//   Solid   - a flat tint (the food / water placeholder).
// The three animated kinds are EMISSIVE and dim as they empty. Output is
// PREMULTIPLIED (SpriteBatch's bar pipeline): the tube is opaque, alpha 1.
//
// Space: `p` is measured in TUBE HEIGHTS (x = uv.x * aspect, y = uv.y), so a
// pattern keeps its shape on a long party-bar tube and a short sheet one.
// Edit + relaunch to iterate - the shader cache recompiles on change.
// ============================================================================

cbuffer ScreenConstants : register(b0) {
	float2 gScreenSize;
	float gTime;
	float gUnused;
};

struct VSInput {
	float2 position : POSITION;
	float2 uv : TEXCOORD0;
	float4 tint : COLOR;
	float4 params : TEXCOORD1; // kind, fraction, beat phase, seed
	float4 extra : TEXCOORD2;  // tube aspect (w/h), tube height in px, Effort's throb
};

struct PSInput {
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD0;
	float4 tint : COLOR;
	nointerpolation float4 params : TEXCOORD1;
	nointerpolation float4 extra : TEXCOORD2;
};

PSInput VSMain(VSInput input) {
	PSInput o;
	o.position = float4(input.position.x / gScreenSize.x * 2.0 - 1.0,
						1.0 - input.position.y / gScreenSize.y * 2.0, 0.0, 1.0);
	o.uv = input.uv;
	o.tint = input.tint;
	o.params = input.params;
	o.extra = input.extra;
	return o;
}

// --- tuning ------------------------------------------------------------------
static const float kDimFloor = 0.35;   // brightness of a nearly empty bar (full = 1)
static const float kGlassTint = 0.06;  // how much of the fluid's colour the empty glass keeps
// CALM (Michael, 2026-09-30: "too busy - they draw the eye"): the HUD is read
// in the corner of the eye, so the fills must live without asking to be looked
// at. kPace scales every drift, slosh and shimmer (NOT the heart rate - that is
// a reading, set by HeartRateTarget); kSubdue scales the contrast of the moving
// detail around each fill's mid colour. 1 / 1 was the first cut.
static const float kPace = 0.45;
static const float kSubdue = 0.55;
// The whole tube's brightness, fluid and glass alike (Michael, 2026-10-01: the
// fills read too bright, cartoonish; picked from a 100..50% side-by-side).
// 1 was the first cut.
static const float kBrightness = 0.7;

static const float3 kBloodDeep = float3(0.32, 0.01, 0.02);
static const float3 kBloodBright = float3(1.00, 0.10, 0.07);
static const float3 kBloodFoam = float3(1.00, 0.55, 0.45);
static const float3 kStaminaDeep = float3(0.02, 0.28, 0.08);
static const float3 kStaminaBright = float3(0.35, 1.00, 0.45);
static const float3 kManaDeep = float3(0.02, 0.06, 0.30);
static const float3 kManaBright = float3(0.30, 0.65, 1.00);
static const float3 kLightning = float3(0.85, 0.95, 1.00);
// GuardSlider's over-exertion palette (kOverDark / kOverHot there): the burning
// stretch starts dark and brightens to a full hot red at 100%.
static const float3 kOverDark = float3(0.40, 0.04, 0.03);
static const float3 kOverHot = float3(1.00, 0.10, 0.06);

// --- noise -------------------------------------------------------------------
// An INTEGER hash of a lattice point, 0..1. Not the usual frac(sin)/frac(dot)
// float hash: those amplify a one-ulp rounding difference into a different
// value, so two neighbouring cells computing the same shared corner (i + 1 in
// one, i in the next) disagreed - and every cell boundary showed as a vertical
// seam drifting along the bars. Integers cannot disagree.
float HashLattice(int2 c) {
	uint h = asuint(c.x) * 0x8da6b343u + asuint(c.y) * 0xd8163841u;
	h ^= h >> 16;
	h *= 0x7feb352du;
	h ^= h >> 15;
	h *= 0x846ca68bu;
	h ^= h >> 16;
	return h * (1.0 / 4294967296.0);
}

// The same, for a point already on the lattice (a cell id, a frame count).
float Hash21(float2 p) {
	return HashLattice(int2(floor(p)));
}

float ValueNoise(float2 p) {
	const float2 fl = floor(p);
	const int2 i = int2(fl);
	const float2 f = p - fl;
	const float2 u = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(HashLattice(i), HashLattice(i + int2(1, 0)), u.x),
				lerp(HashLattice(i + int2(0, 1)), HashLattice(i + int2(1, 1)), u.x), u.y);
}

float Fbm(float2 p) {
	// Each octave ROTATED as well as scaled: value noise lives on a square grid,
	// and octaves that share its axes stack their creases into straight vertical
	// seams - which a long, thin tube shows off at once.
	const float2x2 turn = float2x2(0.80, -0.60, 0.60, 0.80);
	float v = 0.0, a = 0.5;
	[unroll] for (int i = 0; i < 4; ++i) {
		v += a * ValueNoise(p);
		p = mul(turn, p) * 2.03 + float2(17.1, 9.7);
		a *= 0.5;
	}
	return v;
}

// A bump of width w around phase 0, WRAPPED: the distance is to the nearest
// whole beat, so the pulse fades in from the end of one beat to the start of
// the next instead of cutting off where the phase wraps (a hard seam).
float WrappedBump(float phase, float w) {
	const float d = phase - round(phase);
	return exp(-pow(d / w, 2.0));
}

// The heartbeat's shape over one beat (phase 0..1): a strong LUB, then a
// softer DUB a moment later, then rest.
float HeartEnvelope(float phase) {
	return WrappedBump(phase, 0.06) + 0.6 * WrappedBump(phase - 0.20, 0.06);
}

// --- the fills ----------------------------------------------------------------
// Each returns the fluid's colour at p (tube heights) and writes how far the
// leading edge has moved (`edge`, tube heights, + = further along).

// A detail value (0..1) pulled toward the middle by kSubdue: the pattern keeps
// its shape, it just stops shouting.
float Calm(float v) {
	return 0.5 + (v - 0.5) * kSubdue;
}

float3 Blood(float2 p, float seed, float beatPhase, float bright, out float edge) {
	const float t = gTime * kPace;
	// Ebb and flow: two slow layers sliding against each other.
	const float a = Fbm(p * float2(1.6, 3.0) + float2(-t * 0.35 + seed, t * 0.05));
	const float b = Fbm(p * float2(3.2, 5.0) + float2(t * 0.22 - seed, -t * 0.08) + a);
	float3 col = lerp(kBloodDeep, kBloodBright, Calm(saturate(b * 1.35 - 0.1)) + 0.1);
	// Bubbles rising: a few, faint, drifting up and along.
	const float2 cell = float2(0.45, 0.5);
	const float2 bp = p + float2(-t * 0.18, t * 0.35) + seed;
	const float2 id = floor(bp / cell);
	const float2 f = frac(bp / cell) - 0.5;
	const float r = 0.10 + 0.12 * Hash21(id + 3.1);
	const float present = step(0.70, Hash21(id));
	const float ring = smoothstep(r, r * 0.6, length(f * cell / 0.5)) *
					   smoothstep(r * 0.25, r * 0.6, length(f * cell / 0.5));
	col += kBloodFoam * ring * present * 0.15;
	// The heartbeat rolls along the tube from the left - at the heart's own
	// rate, but a swell rather than a flash.
	const float beat = HeartEnvelope(frac(beatPhase - p.x * 0.05));
	col *= 1.0 + 0.25 * beat;
	edge = 0.04 * sin(t * 0.9 + seed * 6.0) + 0.02 * sin(t * 2.1 + p.y * 7.0 + seed)
		   + 0.03 * HeartEnvelope(beatPhase);
	return col * bright;
}

float3 StaminaGlow(float2 p, float seed, float bright, out float edge) {
	const float t = gTime * kPace;
	const float breathe = 0.93 + 0.07 * sin(t * 1.4 + seed * 5.0);
	// Heat shimmer: the glow's body wavers, barely.
	const float y = p.y + 0.02 * sin(p.x * 7.0 - t * 3.0 + seed);
	const float core = exp(-pow((y - 0.5) / 0.30, 2.0));
	const float haze = Fbm(p * float2(1.2, 2.5) + float2(-t * 0.25, 0.0) + seed);
	float3 col = lerp(kStaminaDeep, kStaminaBright,
					  saturate(core * 0.75 + Calm(haze) * 0.45));
	// Motes drifting along: sparse and dim.
	const float2 mp = p * float2(2.5, 4.0) + float2(-t * 0.5, sin(t + seed) * 0.3) + seed;
	const float mote = smoothstep(0.95, 1.0, ValueNoise(mp * 3.0));
	col += kStaminaBright * mote * 0.2;
	edge = 0.025 * sin(t * 1.4 + seed * 5.0);
	return col * breathe * bright;
}

float3 ManaPulse(float2 p, float seed, float hpx, float bright, out float edge) {
	const float t = gTime * kPace;
	// Wisps: ridged noise, the kit's blue smoke.
	const float n = Fbm(p * float2(0.7, 1.6) + float2(-t * 0.3 + seed, t * 0.1));
	const float wisp = pow(1.0 - abs(n * 2.0 - 1.0), 4.0);
	float3 col = lerp(kManaDeep, kManaBright, Calm(saturate(wisp * 0.9 + n * 0.25)) + 0.1);
	// Lightning: an occasional strike (every ~4.5 s of REAL time, jittered per
	// bar), its head running along the tube and the trail fading behind it. It
	// keeps real time, unlike the drift: a slowed bolt is not calmer, just odd.
	const float tr = gTime;
	const float period = 4.5;
	const float k = floor((tr + seed * 7.0) / period);
	const float since = frac((tr + seed * 7.0) / period) * period;
	const float headX = since * 10.0;
	const float boltY = 0.5 + 0.55 * (Fbm(float2(p.x * 1.8, k * 3.7)) - 0.5);
	const float px = 1.0 / max(hpx, 1.0);
	const float d = abs(p.y - boltY);
	// The head TAPERS over half a tube height: a hard cut-off there drew the
	// glow behind the bolt as a lit rectangle with a straight vertical edge.
	const float head = smoothstep(headX, headX - 0.5, p.x);
	const float bolt = (exp(-d / (px * 1.2)) + 0.2 * exp(-d / 0.08)) *
					   head * exp(-since * 3.0) *
					   (0.8 + 0.2 * Hash21(float2(floor(tr * 18.0), k)));
	col += kLightning * bolt * 0.7;
	edge = 0.02 * sin(t * 3.0 + seed);
	return col * bright;
}

float3 EffortFill(float2 p, float seed, float aspect, float over, float throb, float3 tint,
				  out float edge) {
	const float t = gTime * kPace;
	// A faint grain drifting along, so the fill reads as a substance rather
	// than paint - kept to a tenth either way of the stance colour.
	const float grain = Calm(Fbm(p * float2(1.5, 3.0) + float2(-t * 0.2 + seed, 0.0)));
	float3 col = tint * (0.9 + 0.2 * grain);
	// A soft sheen sliding along the tube once every ~8 s of real time, jittered
	// per member so the four meters never sweep together.
	const float period = 8.0;
	const float ph = frac((gTime + seed * 11.0) / period);
	const float sweepX = ph * (aspect + 3.0) - 1.5;
	col += tint * exp(-pow((p.x - sweepX) / 0.8, 2.0)) * 0.22;
	if (over > 0.0) {
		// The over-exertion burns from the left, hotter as it climbs, and
		// breathes like an ember - slowly; it is a warning, not an alarm.
		const float3 hot = lerp(kOverDark, kOverHot, over);
		const float ember = 0.86 + 0.14 * sin(gTime * 1.5 + seed * 4.0);
		const float flick = Calm(Fbm(p * float2(2.0, 3.0) + float2(-t * 0.6, t * 0.2) + seed));
		float3 burn = hot * ember * (0.85 + 0.3 * flick);
		// FULL over-exertion THROBS (Michael): `throb` is the CPU's beat, the same
		// one swelling the tube's height, so colour and size pulse together -
		// from a dull blood red at the trough to a hot, near-orange red at the crest.
		if (throb > 0.0) {
			// GuardSlider sends 0.02 + 0.98 x the beat, so 0 can mean "none".
			const float beat = saturate((throb - 0.02) / 0.98);
			const float3 crest = float3(1.00, 0.32, 0.14);
			burn = lerp(burn * 0.7, crest * (0.9 + 0.2 * flick), beat);
		}
		const float end = over * aspect;
		col = lerp(col, burn, smoothstep(end + 0.08, end - 0.08, p.x));
	}
	edge = 0.0;
	return col;
}

float4 PSMain(PSInput input) : SV_TARGET {
	const int kind = (int)(input.params.x + 0.5);
	const float fraction = saturate(input.params.y);
	const float beatPhase = input.params.z;
	const float seed = input.params.w;
	const float aspect = max(input.extra.x, 1e-3);
	const float hpx = input.extra.y;
	const float2 p = float2(input.uv.x * aspect, input.uv.y);
	const float bright = lerp(kDimFloor, 1.0, fraction);

	float edge = 0.0;
	float3 fluid;
	float3 hue;
	if (kind == 1) {
		fluid = Blood(p, seed, beatPhase, bright, edge);
		hue = kBloodBright;
	} else if (kind == 2) {
		fluid = StaminaGlow(p, seed, bright, edge);
		hue = kStaminaBright;
	} else if (kind == 3) {
		fluid = ManaPulse(p, seed, hpx, bright, edge);
		hue = kManaBright;
	} else if (kind == 4) {
		// The stance's own colour, not dimmed by how full it is: a pulled-back
		// stance is already graded darker by its caller.
		fluid = EffortFill(p, seed, aspect, saturate(beatPhase), saturate(input.extra.z),
						   input.tint.rgb, edge);
		hue = input.tint.rgb;
	} else {
		fluid = input.tint.rgb;
		hue = input.tint.rgb;
	}

	// The leading edge: how far past (+) or short of (-) the fill's end this
	// pixel is, in tube heights, with the kind's own wobble. An empty bar has
	// no fluid at all, wobble or not.
	const float m = fraction * aspect + (kind == 0 ? 0.0 : edge) - p.x;
	const float aa = 1.5 / max(hpx, 1.0);
	const float inside = fraction > 0.0 ? smoothstep(-aa, aa, m) : 0.0;
	// A bright meniscus right at the edge (not for the flat placeholder).
	const float meniscus = kind == 0 ? 0.0 : exp(-abs(m) * 18.0) * inside * 0.35 * bright;

	// The cylinder: darker toward the top and bottom of the tube.
	const float shade = 0.55 + 0.45 * sin(3.14159 * input.uv.y);
	const float3 glass = hue * kGlassTint * shade + 0.015;
	float3 col = lerp(glass, fluid * shade, inside) + hue * meniscus;

	// Glass highlights over everything: a streak along the top, a faint lip below.
	const float streak = exp(-pow((input.uv.y - 0.20) / 0.07, 2.0)) * 0.22;
	const float lip = exp(-pow((input.uv.y - 0.88) / 0.05, 2.0)) * 0.06;
	col += streak + lip;

	return float4(col * kBrightness, 1.0);
}
