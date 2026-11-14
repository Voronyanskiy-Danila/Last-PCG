#include "TerrainRenderer.h"

#include "../importers/Importer_Image_DirectXTex.h"
#include "../math/MathUtils.h"

#include <fstream>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{

float Hash01(int x, int y, int seed)
{
	uint32_t n = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u +
		static_cast<uint32_t>(seed) * 1274126177u;
	n = (n ^ (n >> 13)) * 1274126177u;
	n = n ^ (n >> 16);
	return static_cast<float>(n & 0xFFFFu) / 65535.f;
}

float ValueNoise(float x, float y, int seed)
{
	const int x0 = static_cast<int>(std::floor(x));
	const int y0 = static_cast<int>(std::floor(y));
	float fx = x - static_cast<float>(x0);
	float fy = y - static_cast<float>(y0);
	fx = fx * fx * (3.f - 2.f * fx);
	fy = fy * fy * (3.f - 2.f * fy);
	const float n00 = Hash01(x0, y0, seed);
	const float n10 = Hash01(x0 + 1, y0, seed);
	const float n01 = Hash01(x0, y0 + 1, seed);
	const float n11 = Hash01(x0 + 1, y0 + 1, seed);
	const float a = n00 * (1.f - fx) + n10 * fx;
	const float b = n01 * (1.f - fx) + n11 * fx;
	return a * (1.f - fy) + b * fy;
}

void WriteBmpBgr(const std::filesystem::path& path, const uint8_t* bgr, int w, int h)
{
	const int rowStride = ((w * 3 + 3) / 4) * 4;
	const uint32_t pixelBytes = static_cast<uint32_t>(rowStride * h);
	const uint32_t fileSize = 54u + pixelBytes;

	std::ofstream out(path, std::ios::binary);
	if (!out)
	{
		throw DxException(
			E_FAIL,
			L"Failed to create terrain BMP: " + path.wstring(),
			AnsiToWString(__FILE__),
			__LINE__);
	}

	uint8_t header[54]{};
	header[0] = 'B';
	header[1] = 'M';
	memcpy(header + 2, &fileSize, 4);
	header[10] = 54;
	header[14] = 40;
	memcpy(header + 18, &w, 4);
	memcpy(header + 22, &h, 4);
	header[26] = 1;
	header[28] = 24;
	memcpy(header + 34, &pixelBytes, 4);
	out.write(reinterpret_cast<const char*>(header), 54);
	if (!out)
	{
		throw DxException(
			E_FAIL,
			L"Failed writing terrain BMP header: " + path.wstring(),
			AnsiToWString(__FILE__),
			__LINE__);
	}

	std::vector<uint8_t> row(static_cast<size_t>(rowStride), 0);
	for (int y = h - 1; y >= 0; --y)
	{
		for (int x = 0; x < w; ++x)
		{
			const size_t src = static_cast<size_t>(y * w + x) * 3u;
			const size_t dst = static_cast<size_t>(x * 3);
			row[dst + 0] = bgr[src + 0];
			row[dst + 1] = bgr[src + 1];
			row[dst + 2] = bgr[src + 2];
		}
		out.write(reinterpret_cast<const char*>(row.data()), rowStride);
	}
	out.flush();
	if (!out)
	{
		throw DxException(
			E_FAIL,
			L"Failed writing terrain BMP pixels: " + path.wstring(),
			AnsiToWString(__FILE__),
			__LINE__);
	}
}

bool ReadBmpGray01(const std::filesystem::path& path, std::vector<float>& out, int expectedSize)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return false;

	uint8_t header[54]{};
	in.read(reinterpret_cast<char*>(header), 54);
	if (!in || header[0] != 'B' || header[1] != 'M')
		return false;

	int w = 0;
	int h = 0;
	memcpy(&w, header + 18, 4);
	memcpy(&h, header + 22, 4);
	const int bpp = header[28];
	if (w != expectedSize || h != expectedSize || bpp != 24)
		return false;

	uint32_t dataOffset = 54;
	memcpy(&dataOffset, header + 10, 4);
	const int rowStride = ((w * 3 + 3) / 4) * 4;
	in.seekg(dataOffset, std::ios::beg);

	out.assign(static_cast<size_t>(w * h), 0.f);
	std::vector<uint8_t> row(static_cast<size_t>(rowStride));
	for (int y = h - 1; y >= 0; --y)
	{
		in.read(reinterpret_cast<char*>(row.data()), rowStride);
		if (!in)
			return false;
		for (int x = 0; x < w; ++x)
		{
			const uint8_t b = row[static_cast<size_t>(x * 3 + 0)];
			const uint8_t g = row[static_cast<size_t>(x * 3 + 1)];
			const uint8_t r = row[static_cast<size_t>(x * 3 + 2)];
			const float v = static_cast<float>((static_cast<int>(r) + g + b) / 3) / 255.f;
			out[static_cast<size_t>(y * w + x)] = v;
		}
	}
	return true;
}

} // namespace

void TerrainRenderer::EnsureHeightmapAssets(const std::wstring& contentRoot)
{
	namespace fs = std::filesystem;
	const fs::path terrainDir = fs::path(contentRoot) / L"content" / L"terrain";
	const fs::path heightPath = terrainDir / L"heightmap.bmp";
	const fs::path albedoPath = terrainDir / L"albedo.bmp";
	const fs::path tile0 = terrainDir / L"tiles" / L"tile_0_0.bmp";
	if (fs::exists(heightPath) && fs::exists(albedoPath) && fs::exists(tile0))
	{
		if (!LoadCpuHeightFromBmp(heightPath.wstring()))
			GenerateHeightmapFiles(terrainDir.wstring());
		return;
	}
	GenerateHeightmapFiles(terrainDir.wstring());
}

bool TerrainRenderer::LoadCpuHeightFromBmp(const std::wstring& heightBmpPath)
{
	return ReadBmpGray01(heightBmpPath, mHeightCpu, kHeightRes);
}

void TerrainRenderer::GenerateHeightmapFiles(const std::wstring& terrainDirW)
{
	namespace fs = std::filesystem;
	const fs::path terrainDir(terrainDirW);
	const fs::path tileDir = terrainDir / L"tiles";
	fs::create_directories(tileDir);

	constexpr int kSize = kHeightRes;
	constexpr int kTile = 256;
	mHeightCpu.resize(static_cast<size_t>(kSize * kSize));
	std::vector<uint8_t> gray(static_cast<size_t>(kSize * kSize));
	std::vector<uint8_t> rgb(static_cast<size_t>(kSize * kSize * 3));

	for (int y = 0; y < kSize; ++y)
	{
		for (int x = 0; x < kSize; ++x)
		{
			const float nx = static_cast<float>(x) / static_cast<float>(kSize);
			const float ny = static_cast<float>(y) / static_cast<float>(kSize);
			float h = 0.f;
			float amp = 1.f;
			float freq = 2.f;
			float norm = 0.f;
			for (int o = 0; o < 5; ++o)
			{
				h += amp * ValueNoise(nx * freq * 3.f, ny * freq * 3.f, 42 + o);
				norm += amp;
				amp *= 0.5f;
				freq *= 2.f;
			}
			h /= (std::max)(norm, 1e-5f);
			const float ridge = 1.f - std::fabs(2.f * ValueNoise(nx * 2.5f, ny * 2.5f, 99) - 1.f);
			h = std::pow(h * 0.7f + ridge * 0.3f, 1.2f);
			mHeightCpu[static_cast<size_t>(y * kSize + x)] = h;

			const uint8_t v = static_cast<uint8_t>(std::clamp(h * 255.f, 0.f, 255.f));
			gray[static_cast<size_t>(y * kSize + x)] = v;
			const size_t pi = static_cast<size_t>(y * kSize + x) * 3u;
			rgb[pi + 0] = static_cast<uint8_t>(22 + h * 40);  // B
			rgb[pi + 1] = static_cast<uint8_t>(48 + h * 110); // G
			rgb[pi + 2] = static_cast<uint8_t>(28 + h * 70);  // R
		}
	}

	std::vector<uint8_t> grayBgr(static_cast<size_t>(kSize * kSize * 3));
	for (int i = 0; i < kSize * kSize; ++i)
	{
		const uint8_t v = gray[static_cast<size_t>(i)];
		grayBgr[static_cast<size_t>(i) * 3 + 0] = v;
		grayBgr[static_cast<size_t>(i) * 3 + 1] = v;
		grayBgr[static_cast<size_t>(i) * 3 + 2] = v;
	}
	WriteBmpBgr(terrainDir / L"heightmap.bmp", grayBgr.data(), kSize, kSize);
	WriteBmpBgr(terrainDir / L"albedo.bmp", rgb.data(), kSize, kSize);

	const int n = kSize / kTile;
	for (int tz = 0; tz < n; ++tz)
	{
		for (int tx = 0; tx < n; ++tx)
		{
			std::vector<uint8_t> tileBgr(static_cast<size_t>(kTile * kTile * 3));
			for (int y = 0; y < kTile; ++y)
			{
				for (int x = 0; x < kTile; ++x)
				{
					const int sx = tx * kTile + x;
					const int sy = tz * kTile + y;
					const uint8_t v = gray[static_cast<size_t>(sy * kSize + sx)];
					const size_t di = static_cast<size_t>(y * kTile + x) * 3u;
					tileBgr[di + 0] = v;
					tileBgr[di + 1] = v;
					tileBgr[di + 2] = v;
				}
			}
			wchar_t name[64];
			swprintf_s(name, L"tile_%d_%d.bmp", tx, tz);
			WriteBmpBgr(tileDir / name, tileBgr.data(), kTile, kTile);
		}
	}
}

void TerrainRenderer::LoadTextures(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* cmdList,
	ID3D12DescriptorHeap* srvHeap,
	UINT heightSrvIndex,
	UINT albedoSrvIndex,
	UINT descriptorIncrement,
	const std::wstring& contentRoot,
	std::vector<ComPtr<ID3D12Resource>>& textureGpu,
	std::vector<ComPtr<ID3D12Resource>>& textureUploads)
{
	EnsureHeightmapAssets(contentRoot);

	auto loadSlot = [&](UINT slot, const wchar_t* rel) {
		if (slot >= textureGpu.size())
		{
			textureGpu.resize(slot + 1u);
			textureUploads.resize(slot + 1u);
		}
		std::wstring path = contentRoot;
		if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
			path += L'\\';
		path += rel;
		ThrowIfFailed(LoadTextureImageFromFile12(
			device,
			cmdList,
			path.c_str(),
			textureGpu[slot],
			textureUploads[slot],
			nullptr));
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += static_cast<SIZE_T>(slot) * descriptorIncrement;
		Dx12Utils::CreateTextureSrv(device, textureGpu[slot].Get(), cpu, false);
	};

	loadSlot(heightSrvIndex, L"content\\terrain\\heightmap.bmp");
	loadSlot(albedoSrvIndex, L"content\\terrain\\albedo.bmp");
}

void TerrainRenderer::BuildGridMesh(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
{
	const int n = kGridN;

	struct V
	{
		XMFLOAT3 Pos;
		XMFLOAT3 Normal;
		XMFLOAT2 TexC;
		XMFLOAT4 Tangent;
	};

	std::vector<V> verts;
	verts.reserve(static_cast<size_t>(n * n + (n - 1) * 8));

	auto addVert = [&](float u, float yFlag, float v) {
		V vtx{};
		vtx.Pos = {u, yFlag, v};
		vtx.Normal = {0.f, 1.f, 0.f};
		vtx.TexC = {u, v};
		vtx.Tangent = {1.f, 0.f, 0.f, 1.f};
		verts.push_back(vtx);
		return static_cast<uint16_t>(verts.size() - 1u);
	};

	std::vector<uint16_t> gridIdx(static_cast<size_t>(n * n));
	for (int z = 0; z < n; ++z)
	{
		for (int x = 0; x < n; ++x)
		{
			const float u = static_cast<float>(x) / static_cast<float>(n - 1);
			const float v = static_cast<float>(z) / static_cast<float>(n - 1);
			gridIdx[static_cast<size_t>(z * n + x)] = addVert(u, 0.f, v);
		}
	}

	std::vector<uint16_t> indices;
	indices.reserve(static_cast<size_t>((n - 1) * (n - 1) * 6 + (n - 1) * 4 * 6));

	// Surface: CW winding for D3D default FrontCounterClockwise=FALSE (normal +Y).
	for (int z = 0; z < n - 1; ++z)
	{
		for (int x = 0; x < n - 1; ++x)
		{
			const uint16_t i0 = gridIdx[static_cast<size_t>(z * n + x)];
			const uint16_t i1 = gridIdx[static_cast<size_t>(z * n + x + 1)];
			const uint16_t i2 = gridIdx[static_cast<size_t>((z + 1) * n + x)];
			const uint16_t i3 = gridIdx[static_cast<size_t>((z + 1) * n + x + 1)];
			indices.push_back(i0);
			indices.push_back(i1);
			indices.push_back(i2);
			indices.push_back(i1);
			indices.push_back(i3);
			indices.push_back(i2);
		}
	}

	// Skirts: vertical quads along borders (bottom verts flagged with y=-1).
	auto addSkirtEdge = [&](int x0, int z0, int x1, int z1) {
		const float u0 = static_cast<float>(x0) / static_cast<float>(n - 1);
		const float v0 = static_cast<float>(z0) / static_cast<float>(n - 1);
		const float u1 = static_cast<float>(x1) / static_cast<float>(n - 1);
		const float v1 = static_cast<float>(z1) / static_cast<float>(n - 1);
		const uint16_t t0 = gridIdx[static_cast<size_t>(z0 * n + x0)];
		const uint16_t t1 = gridIdx[static_cast<size_t>(z1 * n + x1)];
		const uint16_t b0 = addVert(u0, -1.f, v0);
		const uint16_t b1 = addVert(u1, -1.f, v1);
		indices.push_back(t0);
		indices.push_back(t1);
		indices.push_back(b0);
		indices.push_back(t1);
		indices.push_back(b1);
		indices.push_back(b0);
	};

	for (int i = 0; i < n - 1; ++i)
	{
		addSkirtEdge(i, 0, i + 1, 0);                 // -Z
		addSkirtEdge(i + 1, n - 1, i, n - 1);         // +Z (CW outward-ish)
		addSkirtEdge(0, i + 1, 0, i);                 // -X
		addSkirtEdge(n - 1, i, n - 1, i + 1);         // +X
	}

	m_geo = std::make_unique<MeshGeometry>();
	m_geo->Name = "TerrainGrid";
	m_geo->VertexByteStride = sizeof(V);
	m_geo->VertexBufferByteSize = static_cast<UINT>(verts.size() * sizeof(V));
	m_geo->IndexFormat = DXGI_FORMAT_R16_UINT;
	m_geo->IndexBufferByteSize = static_cast<UINT>(indices.size() * sizeof(uint16_t));
	m_geo->IndexCount = static_cast<UINT>(indices.size());

	ThrowIfFailed(D3DCreateBlob(m_geo->VertexBufferByteSize, &m_geo->VertexBufferCPU));
	memcpy(m_geo->VertexBufferCPU->GetBufferPointer(), verts.data(), m_geo->VertexBufferByteSize);
	ThrowIfFailed(D3DCreateBlob(m_geo->IndexBufferByteSize, &m_geo->IndexBufferCPU));
	memcpy(m_geo->IndexBufferCPU->GetBufferPointer(), indices.data(), m_geo->IndexBufferByteSize);

	m_geo->VertexBufferGPU = Dx12Utils::CreateDefaultBuffer(
		device,
		cmdList,
		verts.data(),
		m_geo->VertexBufferByteSize,
		m_geo->VertexBufferUploader);
	m_geo->IndexBufferGPU = Dx12Utils::CreateDefaultBuffer(
		device,
		cmdList,
		indices.data(),
		m_geo->IndexBufferByteSize,
		m_geo->IndexBufferUploader);
}

void TerrainRenderer::BuildPipeline(
	ID3D12Device* device,
	DXGI_FORMAT* rtvFormats,
	UINT rtvCount,
	DXGI_FORMAT dsvFormat)
{
	m_vsBc = Dx12Utils::CompileShader(L"content/shaders/terrain_gbuffer.hlsl", nullptr, "VS_Terrain", "vs_5_0");
	m_psBc = Dx12Utils::CompileShader(L"content/shaders/terrain_gbuffer.hlsl", nullptr, "PS_Terrain", "ps_5_0");

	CD3DX12_DESCRIPTOR_RANGE srvRange{};
	srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2u, 0u);

	CD3DX12_ROOT_PARAMETER rp[2]{};
	rp[0].InitAsConstantBufferView(0u, 0u, D3D12_SHADER_VISIBILITY_ALL);
	rp[1].InitAsDescriptorTable(1u, &srvRange, D3D12_SHADER_VISIBILITY_ALL);

	CD3DX12_STATIC_SAMPLER_DESC sampler(
		0u,
		D3D12_FILTER_MIN_MAG_MIP_LINEAR,
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

	CD3DX12_ROOT_SIGNATURE_DESC rsd(
		2u,
		rp,
		1u,
		&sampler,
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	ComPtr<ID3DBlob> serialized;
	ComPtr<ID3DBlob> errors;
	ThrowIfFailed(D3D12SerializeRootSignature(
		&rsd, D3D_ROOT_SIGNATURE_VERSION_1, serialized.GetAddressOf(), errors.GetAddressOf()));
	if (errors)
		OutputDebugStringA(static_cast<char*>(errors->GetBufferPointer()));
	ThrowIfFailed(device->CreateRootSignature(
		0u,
		serialized->GetBufferPointer(),
		serialized->GetBufferSize(),
		IID_PPV_ARGS(&m_rootSig)));

	D3D12_INPUT_ELEMENT_DESC layout[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
	pso.InputLayout = {layout, _countof(layout)};
	pso.pRootSignature = m_rootSig.Get();
	pso.VS = {m_vsBc->GetBufferPointer(), m_vsBc->GetBufferSize()};
	pso.PS = {m_psBc->GetBufferPointer(), m_psBc->GetBufferSize()};
	CD3DX12_RASTERIZER_DESC rs(D3D12_DEFAULT);
	rs.CullMode = D3D12_CULL_MODE_NONE; // skirts + safe for winding
	pso.RasterizerState = rs;
	pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	pso.SampleMask = UINT_MAX;
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets = rtvCount;
	for (UINT i = 0; i < rtvCount; ++i)
		pso.RTVFormats[i] = rtvFormats[i];
	pso.DSVFormat = dsvFormat;
	pso.SampleDesc.Count = 1;
	ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));
}

void TerrainRenderer::Initialize(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* cmdList,
	ID3D12DescriptorHeap*,
	UINT,
	UINT,
	UINT,
	DXGI_FORMAT* rtvFormats,
	UINT rtvCount,
	DXGI_FORMAT dsvFormat)
{
	m_cbElementSize = Dx12Utils::CalcConstantBufferByteSize(sizeof(TerrainConstants));
	m_cb = std::make_unique<GpuUploadBuffer<TerrainConstants>>(device, kMaxPatches, true);
	BuildGridMesh(device, cmdList);
	BuildPipeline(device, rtvFormats, rtvCount, dsvFormat);
}

void TerrainRenderer::Build(const XMFLOAT3& originMin, float worldSizeXZ, float heightScale)
{
	mHeightScale = heightScale;
	mBaseY = originMin.y;
	if (mHeightCpu.size() != static_cast<size_t>(kHeightRes * kHeightRes))
	{
		mHeightCpu.assign(static_cast<size_t>(kHeightRes * kHeightRes), 0.5f);
	}
	mTree.Build(
		originMin,
		worldSizeXZ,
		heightScale,
		kMaxDepth,
		mHeightCpu.data(),
		kHeightRes);
}

void TerrainRenderer::Select(const Frustum& frustum, const XMFLOAT3& cameraPos)
{
	mTree.Select(frustum, cameraPos, mLodFactor, mVisible);
	if (mVisible.size() > kMaxPatches)
		mVisible.resize(kMaxPatches);
}

void TerrainRenderer::DrawGBuffer(
	ID3D12GraphicsCommandList* cmd,
	CXMMATRIX view,
	CXMMATRIX proj,
	const XMFLOAT3& eyePosW,
	ID3D12DescriptorHeap* srvHeap,
	UINT heightSrvIndex,
	UINT descriptorIncrement)
{
	if (!Ready() || mVisible.empty())
		return;

	cmd->SetPipelineState(m_pso.Get());
	cmd->SetGraphicsRootSignature(m_rootSig.Get());

	ID3D12DescriptorHeap* heaps[] = {srvHeap};
	cmd->SetDescriptorHeaps(1u, heaps);

	CD3DX12_GPU_DESCRIPTOR_HANDLE heightSrv(srvHeap->GetGPUDescriptorHandleForHeapStart());
	heightSrv.Offset(static_cast<INT>(heightSrvIndex), descriptorIncrement);
	cmd->SetGraphicsRootDescriptorTable(1u, heightSrv);

	const D3D12_VERTEX_BUFFER_VIEW vbv = m_geo->VertexBufferView();
	const D3D12_INDEX_BUFFER_VIEW ibv = m_geo->IndexBufferView();
	cmd->IASetVertexBuffers(0u, 1u, &vbv);
	cmd->IASetIndexBuffer(&ibv);
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	for (UINT i = 0; i < static_cast<UINT>(mVisible.size()); ++i)
	{
		const TerrainPatch& p = mVisible[i];
		const float sx = p.Bounds.Max.x - p.Bounds.Min.x;
		const float sz = p.Bounds.Max.z - p.Bounds.Min.z;
		// Local Y = h * HeightScale; world Y = mBaseY + local Y.
		const XMMATRIX world =
			XMMatrixScaling(sx, 1.f, sz) *
			XMMatrixTranslation(p.Bounds.Min.x, mBaseY, p.Bounds.Min.z);
		const XMMATRIX wvp = world * view * proj;

		TerrainConstants cb{};
		XMStoreFloat4x4(&cb.World, XMMatrixTranspose(world));
		XMStoreFloat4x4(&cb.WorldInvTranspose, XMMatrixTranspose(MathUtils::InverseTranspose(world)));
		XMStoreFloat4x4(&cb.WorldViewProj, XMMatrixTranspose(wvp));
		cb.EyePosW = eyePosW;
		cb.HeightScale = mHeightScale;
		cb.UvOrigin = p.UvOrigin;
		cb.UvSize = p.UvSize;
		cb.AlbedoTint = {1.f, 1.f, 1.f};
		cb.Roughness = 0.85f;

		m_cb->CopyData(static_cast<int>(i), cb);
		const UINT64 gpu = m_cb->Resource()->GetGPUVirtualAddress() +
			static_cast<UINT64>(i) * static_cast<UINT64>(m_cbElementSize);
		cmd->SetGraphicsRootConstantBufferView(0u, gpu);
		cmd->DrawIndexedInstanced(m_geo->IndexCount, 1u, 0u, 0, 0u);
	}
}
