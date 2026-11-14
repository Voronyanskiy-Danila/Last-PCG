#pragma once

#include "../scene/TerrainQuadtree.h"
#include "../rendering/d3d12/D3d12_GpuUploadBuffer.h"
#include "../rendering/d3d12/D3d12_RenderHelpers.h"
#include "../math/MathUtils.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <vector>
#include <string>

struct TerrainConstants
{
	DirectX::XMFLOAT4X4 World = MathUtils::Identity4x4();
	DirectX::XMFLOAT4X4 WorldInvTranspose = MathUtils::Identity4x4();
	DirectX::XMFLOAT4X4 WorldViewProj = MathUtils::Identity4x4();

	DirectX::XMFLOAT3 EyePosW = {0.f, 0.f, 0.f};
	float HeightScale = 80.f;

	DirectX::XMFLOAT2 UvOrigin = {0.f, 0.f};
	DirectX::XMFLOAT2 UvSize = {1.f, 1.f};

	DirectX::XMFLOAT3 AlbedoTint = {1.f, 1.f, 1.f};
	float Roughness = 0.85f;
};
static_assert(sizeof(TerrainConstants) == 240u, "TerrainConstants must match terrain_gbuffer.hlsl");

class TerrainRenderer
{
public:
	static constexpr UINT kMaxPatches = 256u;
	static constexpr int kGridN = 65;
	static constexpr int kMaxDepth = 3; // 8x8 finest leaves
	static constexpr int kHeightRes = 512;

	void Initialize(
		ID3D12Device* device,
		ID3D12GraphicsCommandList* cmdList,
		ID3D12DescriptorHeap* srvHeap,
		UINT heightSrvIndex,
		UINT albedoSrvIndex,
		UINT descriptorIncrement,
		DXGI_FORMAT* rtvFormats,
		UINT rtvCount,
		DXGI_FORMAT dsvFormat);

	void EnsureHeightmapAssets(const std::wstring& contentRoot);
	void LoadTextures(
		ID3D12Device* device,
		ID3D12GraphicsCommandList* cmdList,
		ID3D12DescriptorHeap* srvHeap,
		UINT heightSrvIndex,
		UINT albedoSrvIndex,
		UINT descriptorIncrement,
		const std::wstring& contentRoot,
		std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& textureGpu,
		std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& textureUploads);

	void Build(
		const DirectX::XMFLOAT3& originMin,
		float worldSizeXZ,
		float heightScale);

	void Select(const Frustum& frustum, const DirectX::XMFLOAT3& cameraPos);

	void DrawGBuffer(
		ID3D12GraphicsCommandList* cmd,
		DirectX::CXMMATRIX view,
		DirectX::CXMMATRIX proj,
		const DirectX::XMFLOAT3& eyePosW,
		ID3D12DescriptorHeap* srvHeap,
		UINT heightSrvIndex,
		UINT descriptorIncrement);

	UINT VisiblePatchCount() const { return static_cast<UINT>(mVisible.size()); }
	const Aabb& WorldBounds() const { return mTree.WorldBounds(); }
	bool Ready() const { return m_pso != nullptr && m_geo != nullptr; }

private:
	void BuildGridMesh(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList);
	void BuildPipeline(ID3D12Device* device, DXGI_FORMAT* rtvFormats, UINT rtvCount, DXGI_FORMAT dsvFormat);
	void GenerateHeightmapFiles(const std::wstring& terrainDir);
	bool LoadCpuHeightFromBmp(const std::wstring& heightBmpPath);

	TerrainQuadtree mTree{};
	std::vector<TerrainPatch> mVisible;
	std::vector<float> mHeightCpu; // [0,1] samples, size kHeightRes^2
	float mHeightScale = 80.f;
	float mBaseY = 0.f;
	float mLodFactor = 2.2f;

	std::unique_ptr<MeshGeometry> m_geo;
	Microsoft::WRL::ComPtr<ID3DBlob> m_vsBc;
	Microsoft::WRL::ComPtr<ID3DBlob> m_psBc;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
	std::unique_ptr<GpuUploadBuffer<TerrainConstants>> m_cb;
	UINT m_cbElementSize = 0;
};
