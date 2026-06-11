// Forward scene pass: optional GPU skinning + Blinn-Phong with one
// directional light and up to 16 dynamic point lights.
// Matrices are uploaded row-major; all transforms use mul(matrix, vector).

#define MAX_POINT_LIGHTS 16
#define MAX_SKIN_JOINTS 128

struct PointLight {
	float4 positionRadius;  // xyz = world pos, w = radius
	float4 colorIntensity;  // rgb = color, w = intensity
	float4 shadow;          // x = shadow cube slot (-1 = unshadowed)
};

cbuffer FrameConstants : register(b0) {
	float4x4 gViewProj;
	float4 gCameraPos;
	float4 gAmbient;
	float4 gDirDirection;
	float4 gDirColor;
	uint gPointLightCount;
	uint3 _pad0;
	float4 gFogGrid;     // xy = 1 / atmosphere world extent, z = density/m, w = haze ambient
	float4 gHazeColor;   // rgb = dust albedo tint
	float4 gShadowLight; // shadow pass only (see shadow.hlsl)
	PointLight gPointLights[MAX_POINT_LIGHTS];
};

cbuffer ObjectConstants : register(b1) {
	float4x4 gWorld;
	float4 gBaseColor;
	uint gUseTexture;
	uint gSkinned;
	uint gUseNormalMap;
	float gHeightScale;
	// Per-material Blinn-Phong sheen: ~0.05/24 for dry stone, higher for
	// wet or polished surfaces (slime, jade, metal).
	float gSpecStrength;
	float gSpecPower;
	float2 _pad1;
};

cbuffer SkinConstants : register(b2) {
	float4x4 gJoints[MAX_SKIN_JOINTS];
};

Texture2D gBaseTexture : register(t0);
Texture2D gNormalMap : register(t1); // xyz = tangent-space normal, w = height
Texture2D gTurbidity : register(t2); // top-down per-cell dust density (R)
// Point-light shadow cubes (light->fragment distance / radius). Slot 0 is
// the light nearest the camera at the highest resolution; slots fall off in
// resolution with distance — detailed shadows up close, coarser far away.
TextureCube gShadowCube0 : register(t3);
TextureCube gShadowCube1 : register(t4);
TextureCube gShadowCube2 : register(t5);
TextureCube gShadowCube3 : register(t6);
SamplerState gSampler : register(s0);
SamplerState gClampSampler : register(s1);

float SampleShadowCube(int slot, float3 dir) {
	switch (slot) {
	case 0:  return gShadowCube0.SampleLevel(gClampSampler, dir, 0).r;
	case 1:  return gShadowCube1.SampleLevel(gClampSampler, dir, 0).r;
	case 2:  return gShadowCube2.SampleLevel(gClampSampler, dir, 0).r;
	default: return gShadowCube3.SampleLevel(gClampSampler, dir, 0).r;
	}
}

// 1 = fully lit, 0 = fully shadowed. The nearest light (slot 0) gets 4-tap
// PCF for soft, detailed edges; farther slots use one tap of an already
// lower-resolution cube.
float ShadowFactor(PointLight light, float3 worldPos) {
	const int slot = (int)light.shadow.x;
	if (slot < 0) return 1.0;

	const float3 toFrag = worldPos - light.positionRadius.xyz;
	const float dist = length(toFrag) / light.positionRadius.w; // normalized
	const float bias = 0.012;

	if (slot == 0) {
		// Two axes perpendicular to the lookup direction for the PCF kernel.
		const float3 absDir = abs(toFrag);
		const float3 up = (absDir.y > absDir.x && absDir.y > absDir.z)
							  ? float3(1, 0, 0)
							  : float3(0, 1, 0);
		const float3 t1 = normalize(cross(toFrag, up));
		const float3 t2 = cross(normalize(toFrag), t1);
		const float spread = 0.015 * length(toFrag);

		const float2 offsets[4] = {float2(-1, -1), float2(1, -1), float2(-1, 1),
								   float2(1, 1)};
		float lit = 0.0;
		[unroll]
		for (int i = 0; i < 4; ++i) {
			const float3 dir = toFrag + (offsets[i].x * t1 + offsets[i].y * t2) * spread;
			lit += dist - bias <= SampleShadowCube(0, dir) ? 1.0 : 0.0;
		}
		return lit * 0.25;
	}
	return dist - bias <= SampleShadowCube(slot, toFrag) ? 1.0 : 0.0;
}

struct VSInput {
	float3 position : POSITION;
	float3 normal : NORMAL;
	float2 uv : TEXCOORD;
	uint4 joints : JOINTS;
	float4 weights : WEIGHTS;
};

struct PSInput {
	float4 position : SV_POSITION;
	float3 worldPos : TEXCOORD0;
	float3 normal : TEXCOORD1;
	float2 uv : TEXCOORD2;
};

PSInput VSMain(VSInput input) {
	float4 localPos = float4(input.position, 1.0);
	float3 localNormal = input.normal;

	if (gSkinned != 0 && input.weights.x > 0.0) {
		float4 skinnedPos = 0;
		float3 skinnedNormal = 0;
		[unroll]
		for (int i = 0; i < 4; ++i) {
			const float w = input.weights[i];
			if (w > 0.0) {
				const float4x4 joint = gJoints[input.joints[i]];
				skinnedPos += w * mul(joint, localPos);
				skinnedNormal += w * mul((float3x3)joint, localNormal);
			}
		}
		localPos = skinnedPos;
		localNormal = skinnedNormal;
	}

	PSInput output;
	const float4 worldPos = mul(gWorld, localPos);
	output.worldPos = worldPos.xyz;
	output.position = mul(gViewProj, worldPos);
	output.normal = mul((float3x3)gWorld, localNormal);
	output.uv = input.uv;
	return output;
}

float3 Shade(float3 albedo, float3 normal, float3 worldPos) {
	const float3 viewDir = normalize(gCameraPos.xyz - worldPos);
	float3 color = gAmbient.rgb * albedo;

	// Directional light.
	if (any(gDirColor.rgb > 0.0)) {
		const float3 lightDir = normalize(-gDirDirection.xyz);
		const float ndl = saturate(dot(normal, lightDir));
		const float3 halfway = normalize(lightDir + viewDir);
		const float spec = pow(saturate(dot(normal, halfway)), gSpecPower) * gSpecStrength;
		color += gDirColor.rgb * (albedo * ndl + spec);
	}

	// Dynamic point lights with smooth radius falloff.
	for (uint i = 0; i < gPointLightCount; ++i) {
		const PointLight light = gPointLights[i];
		const float3 toLight = light.positionRadius.xyz - worldPos;
		const float dist = length(toLight);
		if (dist >= light.positionRadius.w) continue;

		const float3 lightDir = toLight / max(dist, 1e-4);
		const float ndl = saturate(dot(normal, lightDir));

		// Inverse-square with a smooth window to zero at the radius.
		const float window = pow(saturate(1.0 - pow(dist / light.positionRadius.w, 4.0)), 2.0);
		const float atten = window / (1.0 + dist * dist);

		// Per-material sheen (see ObjectConstants): dungeon stone stays dry
		// and matte; wet/polished materials raise gSpecStrength.
		const float3 halfway = normalize(lightDir + viewDir);
		const float spec = pow(saturate(dot(normal, halfway)), gSpecPower) * gSpecStrength;

		const float shadow = ShadowFactor(light, worldPos);
		color += light.colorIntensity.rgb * light.colorIntensity.w * atten * shadow *
				 (albedo * ndl + spec * ndl);
	}
	return color;
}

// ---------------------------------------------------------------------------
// Air turbidity. The dungeon's dust density lives in a small top-down grid
// (one texel per cell, bilinear-blended at region borders). The eye→surface
// ray is marched through it accumulating
//   * extinction: optical depth tau dims the shaded surface by exp(-tau)
//   * in-scattering: each dusty segment glows with the light that reaches it
//     (ambient + the same point torches as the surface shading), attenuated
//     by the dust between it and the eye
// so clear squares stay crisp while dusty squares haze up and catch
// torchlight as glowing motes.
// ---------------------------------------------------------------------------

float DustDensity(float3 worldPos) {
	const float2 uv = worldPos.xz * gFogGrid.xy;
	return gTurbidity.SampleLevel(gClampSampler, uv, 0).r * gFogGrid.z;
}

float3 ApplyDust(float3 surfaceColor, float3 worldPos) {
	if (gFogGrid.z <= 0.0) return surfaceColor; // atmosphere disabled

	const float3 toSurface = worldPos - gCameraPos.xyz;
	const float rayLen = max(length(toSurface), 1e-4);
	const float3 dir = toSurface / rayLen;

	const int kSteps = 12;
	const float seg = rayLen / kSteps;

	float tau = 0.0;
	float3 inscatter = 0.0;
	for (int s = 0; s < kSteps; ++s) {
		const float3 p = gCameraPos.xyz + dir * (seg * (s + 0.5));
		const float density = DustDensity(p);
		if (density <= 0.0001) continue;

		// Light arriving at this bit of dust (isotropic scattering).
		float3 dustLight = gAmbient.rgb * gFogGrid.w;
		for (uint i = 0; i < gPointLightCount; ++i) {
			const PointLight light = gPointLights[i];
			const float3 toLight = light.positionRadius.xyz - p;
			const float d = length(toLight);
			if (d >= light.positionRadius.w) continue;
			const float window =
				pow(saturate(1.0 - pow(d / light.positionRadius.w, 4.0)), 2.0);
			dustLight += light.colorIntensity.rgb * light.colorIntensity.w * window /
						 (1.0 + d * d);
		}

		const float segTau = density * seg;
		inscatter += gHazeColor.rgb * dustLight * segTau * exp(-tau);
		tau += segTau;
	}
	return surfaceColor * exp(-tau) + inscatter;
}

// Per-pixel tangent frame from screen-space derivatives (no vertex tangents
// needed). Rows are T, B, N.
float3x3 CotangentFrame(float3 N, float3 p, float2 uv) {
	const float3 dp1 = ddx(p);
	const float3 dp2 = ddy(p);
	const float2 duv1 = ddx(uv);
	const float2 duv2 = ddy(uv);
	const float3 dp2perp = cross(dp2, N);
	const float3 dp1perp = cross(N, dp1);
	float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
	float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
	const float invmax = rsqrt(max(dot(T, T), max(dot(B, B), 1e-8)));
	return float3x3(T * invmax, B * invmax, N);
}

// Steep parallax: march the view ray through the height field.
float2 Parallax(float2 uv, float3 viewTS) {
	const int kSteps = 12;
	const float layer = 1.0 / kSteps;
	// viewTS.z is the component along the normal; guard grazing angles.
	const float2 delta = viewTS.xy / max(viewTS.z, 0.25) * gHeightScale * layer;

	float depth = 0.0;
	float2 current = uv;
	// Height map stores "up" (1 = proud surface); convert to depth (0 = top).
	// SampleLevel: gradient sampling is not allowed in a divergent loop.
	float surfaceDepth = 1.0 - gNormalMap.SampleLevel(gSampler, current, 0).a;
	[unroll]
	for (int i = 0; i < kSteps; ++i) {
		if (depth >= surfaceDepth) break;
		current -= delta;
		depth += layer;
		surfaceDepth = 1.0 - gNormalMap.SampleLevel(gSampler, current, 0).a;
	}
	return current;
}

float4 PSMain(PSInput input) : SV_TARGET {
	float2 uv = input.uv;
	float3 normal = normalize(input.normal);

	if (gUseNormalMap != 0) {
		const float3x3 tbn = CotangentFrame(normal, input.worldPos, uv);
		if (gHeightScale > 0.0) {
			const float3 viewDir = normalize(gCameraPos.xyz - input.worldPos);
			const float3 viewTS = mul(tbn, viewDir); // (T.V, B.V, N.V)
			uv = Parallax(uv, viewTS);
		}
		float3 nTS = gNormalMap.Sample(gSampler, uv).xyz * 2.0 - 1.0;
		normal = normalize(mul(nTS, tbn)); // rows: nTS.x*T + nTS.y*B + nTS.z*N
	}

	float4 albedo = gBaseColor;
	if (gUseTexture != 0)
		albedo *= gBaseTexture.Sample(gSampler, uv);

	float3 color = Shade(albedo.rgb, normal, input.worldPos);
	color = ApplyDust(color, input.worldPos);

	// Simple tonemap + gamma (back buffer is UNORM).
	color = color / (color + 1.0);
	color = pow(color, 1.0 / 2.2);
	return float4(color, albedo.a);
}
