// Billboard particle pass (flames, sparks, smoke). Vertices arrive as
// pre-built camera-facing quads in world space with PREMULTIPLIED colors:
//   additive particles (flame, spark): rgb = emission, a = 0
//   alpha particles (smoke):           rgb = tint * alpha, a = alpha
// rendered with One / InvSrcAlpha blending after the opaque scene, depth
// tested but not written. The sprite texture carries a soft radial falloff
// in its alpha channel.

cbuffer ParticleConstants : register(b0) {
	float4x4 gViewProj;
};

Texture2D gSprite : register(t0);
SamplerState gSampler : register(s0);

struct VSInput {
	float3 position : POSITION;
	float2 uv : TEXCOORD;
	float4 color : COLOR;
};

struct PSInput {
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD;
	float4 color : COLOR;
};

PSInput VSMain(VSInput input) {
	PSInput output;
	output.position = mul(gViewProj, float4(input.position, 1.0));
	output.uv = input.uv;
	output.color = input.color;
	return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
	const float falloff = gSprite.Sample(gSampler, input.uv).a;
	return float4(input.color.rgb * falloff, input.color.a * falloff);
}
