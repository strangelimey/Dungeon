// 2D UI pass: pixel-space quads with per-vertex color, alpha blended.

cbuffer ScreenConstants : register(b0) {
	float2 gScreenSize;
};

Texture2D gTexture : register(t0);
SamplerState gSampler : register(s0);

struct VSInput {
	float2 position : POSITION; // pixels, origin top-left
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
	const float2 ndc = float2(input.position.x / gScreenSize.x * 2.0 - 1.0,
							  1.0 - input.position.y / gScreenSize.y * 2.0);
	output.position = float4(ndc, 0.0, 1.0);
	output.uv = input.uv;
	output.color = input.color;
	return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
	return gTexture.Sample(gSampler, input.uv) * input.color;
}
