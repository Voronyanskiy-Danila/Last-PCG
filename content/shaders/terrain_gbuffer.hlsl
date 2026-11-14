// terrain_gbuffer.hlsl — Lab terrain: heightmap displacement → G-buffer
// Unit grid in XZ [0,1]^2; World places/scales the patch. Height from t0 (R).
// Skirt verts: PosL.y < 0 → drop below surface to hide LOD cracks.

cbuffer TerrainCB : register(b0)
{
	float4x4 gWorld;
	float4x4 gWorldInvTranspose;
	float4x4 gWorldViewProj;

	float3   gEyePosW;
	float    gHeightScale;

	float2   gUvOrigin;
	float2   gUvSize;

	float3   gAlbedoTint;
	float    gRoughness;
};

Texture2D    gHeightMap : register(t0);
Texture2D    gAlbedoMap : register(t1);
SamplerState gSamLinear : register(s0);

struct VSInput
{
	float3 PosL     : POSITION;
	float3 NormalL  : NORMAL;
	float2 Tex      : TEXCOORD0;
	float4 TangentL : TANGENT;
};

struct PSInput
{
	float4 PosH    : SV_POSITION;
	float3 PosW    : POSITION;
	float3 NormalW : NORMAL;
	float2 TexC    : TEXCOORD0;
};

float SampleHeight(float2 uv)
{
	return gHeightMap.SampleLevel(gSamLinear, uv, 0).r;
}

float3 DisplacedPosL(float2 localXZ, float2 hmUv)
{
	float h = SampleHeight(hmUv);
	return float3(localXZ.x, h * gHeightScale, localXZ.y);
}

PSInput VS_Terrain(VSInput vin)
{
	PSInput vout;

	float2 hmUv = gUvOrigin + vin.Tex * gUvSize;
	float3 posL = DisplacedPosL(vin.PosL.xz, hmUv);

	// Skirt bottom: drop below patch to cover T-junction cracks between LOD levels.
	const bool isSkirt = vin.PosL.y < 0.0;
	if (isSkirt)
		posL.y -= max(gHeightScale * 0.4, 25.0);

	const float eps = 1.0 / 64.0;
	float3 pR = DisplacedPosL(vin.PosL.xz + float2(eps, 0), hmUv + float2(eps * gUvSize.x, 0));
	float3 pU = DisplacedPosL(vin.PosL.xz + float2(0, eps), hmUv + float2(0, eps * gUvSize.y));
	float3 nL = normalize(cross(pR - posL, pU - posL)); // CW winding → +Y for D3D default front face

	if (isSkirt)
		nL = float3(0, 1, 0);

	float4 posW = mul(float4(posL, 1.0), gWorld);
	vout.PosW = posW.xyz;
	vout.NormalW = normalize(mul(nL, (float3x3)gWorldInvTranspose));
	vout.PosH = mul(float4(posL, 1.0), gWorldViewProj);
	vout.TexC = hmUv;
	return vout;
}

struct GBufferOut
{
	float4 Albedo   : SV_Target0;
	float4 Normal   : SV_Target1;
	float4 Position : SV_Target2;
	float4 MatExtra : SV_Target3;
};

GBufferOut PS_Terrain(PSInput pin)
{
	GBufferOut o;
	float3 albedo = gAlbedoMap.Sample(gSamLinear, pin.TexC).rgb * gAlbedoTint;
	o.Albedo = float4(albedo, 1.0);
	o.Normal = float4(normalize(pin.NormalW), 0.0);
	o.Position = float4(pin.PosW, 1.0);
	o.MatExtra = float4(gRoughness, 0.0, 1.0, 0.0);
	return o;
}
