// 2D UI pass: pixel-space quads with per-vertex color, alpha blended.
//
// OUTLINED TEXT rides the same pipeline: a glyph drawn through
// SpriteBatch::DrawGlyph carries a non-zero `outline` colour, and its quad is
// grown by the outline radius. Its pixels take the atlas coverage (alpha) as
// the fill and the coverage DILATED by the radius as a ring under it, so light
// text on stone keeps a dark edge whatever the stone does behind it. Every
// other sprite has outline.a == 0 and takes the plain path.

cbuffer ScreenConstants : register(b0) {
	float2 gScreenSize;
	float gTime;   // bar.hlsl's clock (SpriteBatch::SetTime); unused here
	float gUnused;
};

Texture2D gTexture : register(t0);
SamplerState gSampler : register(s0);

struct VSInput {
	float2 position : POSITION; // pixels, origin top-left
	float2 uv : TEXCOORD0;
	float4 color : COLOR;
	float4 outline : TEXCOORD1; // ring colour; a == 0 for everything but outlined text
	float2 glyph : TEXCOORD2;   // x = outline radius in px (atlas texels: glyphs draw 1:1)
};

struct PSInput {
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD0;
	float4 color : COLOR;
	nointerpolation float4 outline : TEXCOORD1;
	nointerpolation float2 glyph : TEXCOORD2;
};

PSInput VSMain(VSInput input) {
	PSInput output;
	const float2 ndc = float2(input.position.x / gScreenSize.x * 2.0 - 1.0,
							  1.0 - input.position.y / gScreenSize.y * 2.0);
	output.position = float4(ndc, 0.0, 1.0);
	output.uv = input.uv;
	output.color = input.color;
	output.outline = input.outline;
	output.glyph = input.glyph;
	return output;
}

// The most coverage within `radius` texels of uv: eight taps on the circle and
// four at half the radius (so a thin stroke cannot slip between the outer
// taps). Bilinear sampling smooths what lies between them.
float DilatedCoverage(float2 uv, float radius) {
	float2 dims;
	gTexture.GetDimensions(dims.x, dims.y);
	const float2 t = radius / dims;
	const float d = 0.70710678;
	float c = gTexture.SampleLevel(gSampler, uv, 0).a;
	c = max(c, gTexture.SampleLevel(gSampler, uv + float2( t.x, 0), 0).a);
	c = max(c, gTexture.SampleLevel(gSampler, uv + float2(-t.x, 0), 0).a);
	c = max(c, gTexture.SampleLevel(gSampler, uv + float2(0,  t.y), 0).a);
	c = max(c, gTexture.SampleLevel(gSampler, uv + float2(0, -t.y), 0).a);
	c = max(c, gTexture.SampleLevel(gSampler, uv + float2( t.x,  t.y) * d, 0).a);
	c = max(c, gTexture.SampleLevel(gSampler, uv + float2(-t.x,  t.y) * d, 0).a);
	c = max(c, gTexture.SampleLevel(gSampler, uv + float2( t.x, -t.y) * d, 0).a);
	c = max(c, gTexture.SampleLevel(gSampler, uv + float2(-t.x, -t.y) * d, 0).a);
	c = max(c, gTexture.SampleLevel(gSampler, uv + float2( t.x,  t.y) * 0.5, 0).a);
	c = max(c, gTexture.SampleLevel(gSampler, uv + float2(-t.x,  t.y) * 0.5, 0).a);
	c = max(c, gTexture.SampleLevel(gSampler, uv + float2( t.x, -t.y) * 0.5, 0).a);
	c = max(c, gTexture.SampleLevel(gSampler, uv + float2(-t.x, -t.y) * 0.5, 0).a);
	return c;
}

float4 PSMain(PSInput input) : SV_TARGET {
	const float4 tex = gTexture.Sample(gSampler, input.uv);
	if (input.outline.a <= 0.0) return tex * input.color;

	// Fill OVER ring, both straight alpha; the vertex alpha fades the pair.
	const float fillA = tex.a * input.color.a;
	const float ringA = DilatedCoverage(input.uv, input.glyph.x) * input.outline.a *
						input.color.a;
	const float outA = fillA + ringA * (1.0 - fillA);
	if (outA <= 0.0) return float4(0, 0, 0, 0);
	const float3 rgb = (input.color.rgb * fillA + input.outline.rgb * ringA * (1.0 - fillA)) /
					   outA;
	return float4(rgb, outA);
}
