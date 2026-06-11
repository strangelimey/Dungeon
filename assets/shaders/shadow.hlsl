// Depth-distance pass for point-light cube shadow maps. One render per cube
// face: geometry is transformed by the face's 90-degree view-projection and
// the pixel shader writes the light->fragment distance normalized by the
// light radius (R16_FLOAT target). The scene pass compares against these
// values in ShadowFactor (scene.hlsl).
//
// The cbuffers are prefixes of the scene pass layouts (same buffers bound),
// so the declarations below must stay in sync with scene.hlsl / Renderer.cpp.

#define MAX_SKIN_JOINTS 128

cbuffer FrameConstants : register(b0) {
	float4x4 gViewProj;   // the cube face's view-projection
	float4 gCameraPos;
	float4 gAmbient;
	float4 gDirDirection;
	float4 gDirColor;
	uint gPointLightCount;
	uint3 _pad0;
	float4 gFogGrid;
	float4 gHazeColor;
	float4 gShadowLight;  // xyz = light position, w = 1 / light radius
};

cbuffer ObjectConstants : register(b1) {
	float4x4 gWorld;
	float4 gBaseColor;
	uint gUseTexture;
	uint gSkinned;
	uint gUseNormalMap;
	float gHeightScale;
	float gSpecStrength;
	float gSpecPower;
	float2 _pad1;
};

cbuffer SkinConstants : register(b2) {
	float4x4 gJoints[MAX_SKIN_JOINTS];
};

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
};

PSInput VSMain(VSInput input) {
	float4 localPos = float4(input.position, 1.0);

	if (gSkinned != 0 && input.weights.x > 0.0) {
		float4 skinnedPos = 0;
		[unroll]
		for (int i = 0; i < 4; ++i) {
			const float w = input.weights[i];
			if (w > 0.0) skinnedPos += w * mul(gJoints[input.joints[i]], localPos);
		}
		localPos = skinnedPos;
	}

	PSInput output;
	const float4 worldPos = mul(gWorld, localPos);
	output.worldPos = worldPos.xyz;
	output.position = mul(gViewProj, worldPos);
	return output;
}

float PSMain(PSInput input) : SV_TARGET {
	return length(input.worldPos - gShadowLight.xyz) * gShadowLight.w;
}
