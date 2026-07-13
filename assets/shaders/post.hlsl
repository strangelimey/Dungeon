// Post-process chain (Graphics/PostProcess.*): bufferless full-screen-triangle
// passes over the linear-HDR scene target. PSBright thresholds into a half-res
// target, PSBlur runs one direction of a separable gaussian (ping-ponged), and
// PSComposite adds the bloom and applies the ACES tonemap + gamma into the
// UNORM back buffer. The scene shader's inline tonemap is off for this path
// (scene.hlsl gTonemapInline), so every input here is linear HDR.

cbuffer PostConstants : register(b0) {
	float2 gTexel;    // 1 / source dimensions
	float2 gDir;      // blur direction: (1,0) or (0,1)
	float gThreshold; // bloom bright-pass threshold (linear HDR)
	float gKnee;      // smoothstep width above the threshold
	float gStrength;  // bloom add weight in the composite
	float gExposure;  // pre-tonemap exposure
};

Texture2D gSource : register(t0);
Texture2D gBloom : register(t1); // composite only
SamplerState gSampler : register(s0);

struct PSInput {
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD;
};

// Full-screen triangle from SV_VertexID — no vertex buffer bound.
PSInput VSMain(uint id : SV_VertexID) {
	PSInput output;
	const float2 uv = float2((id << 1) & 2, id & 2);
	output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	output.uv = uv;
	return output;
}

// ACES filmic fit (Narkowicz). Shared verbatim with scene.hlsl's inline path
// so the LDR previews grade identically to the composited scene.
float3 AcesTonemap(float3 x) {
	const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
	return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Bright pass: energy above the threshold blooms, easing in over gKnee so the
// cutoff never pops. Sampling the full-res scene into the half-res target at
// the bilinear tap IS the downsample.
float4 PSBright(PSInput input) : SV_TARGET {
	const float3 color = gSource.Sample(gSampler, input.uv).rgb;
	const float lum = max(color.r, max(color.g, color.b));
	const float t = saturate((lum - gThreshold) / max(gKnee, 1e-4));
	return float4(color * t * t * (3.0 - 2.0 * t), 1.0);
}

// One direction of a 9-tap separable gaussian (run twice, H then V).
float4 PSBlur(PSInput input) : SV_TARGET {
	static const float kWeights[5] = {0.227027, 0.1945946, 0.1216216, 0.054054,
									  0.016216};
	float3 acc = gSource.Sample(gSampler, input.uv).rgb * kWeights[0];
	[unroll]
	for (int i = 1; i < 5; ++i) {
		const float2 offset = gDir * gTexel * i;
		acc += gSource.Sample(gSampler, input.uv + offset).rgb * kWeights[i];
		acc += gSource.Sample(gSampler, input.uv - offset).rgb * kWeights[i];
	}
	return float4(acc, 1.0);
}

float4 PSComposite(PSInput input) : SV_TARGET {
	float3 color = gSource.Sample(gSampler, input.uv).rgb;
	color += gBloom.Sample(gSampler, input.uv).rgb * gStrength;
	color = AcesTonemap(color * gExposure);
	color = pow(color, 1.0 / 2.2);
	return float4(color, 1.0);
}
