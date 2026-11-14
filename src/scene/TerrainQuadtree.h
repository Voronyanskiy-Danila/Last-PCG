#pragma once

#include "../math/BoundingBox.h"
#include "../math/Frustum.h"

#include <DirectXMath.h>
#include <cstdint>
#include <vector>

// Visible terrain patch selected by quadtree LOD + frustum culling.
struct TerrainPatch
{
	Aabb Bounds{};
	DirectX::XMFLOAT2 UvOrigin{0.f, 0.f}; // region in heightmap [0,1]
	DirectX::XMFLOAT2 UvSize{1.f, 1.f};
	int Level = 0;
};

// Hierarchical LOD over XZ: subdivide by camera distance, cull with Frustum::IntersectsAabb.
class TerrainQuadtree
{
public:
	void Build(
		const DirectX::XMFLOAT3& originMin,
		float worldSizeXZ,
		float heightScale,
		int maxDepth,
		const float* heightSamples01,
		int heightRes);

	void Select(
		const Frustum& frustum,
		const DirectX::XMFLOAT3& cameraPos,
		float lodFactor,
		std::vector<TerrainPatch>& outPatches) const;

	const Aabb& WorldBounds() const { return mWorldBounds; }
	int MaxDepth() const { return mMaxDepth; }
	uint32_t NodeCount() const { return static_cast<uint32_t>(mNodes.size()); }

private:
	struct Node
	{
		Aabb Bounds{};
		DirectX::XMFLOAT2 UvOrigin{0.f, 0.f};
		DirectX::XMFLOAT2 UvSize{1.f, 1.f};
		int Level = 0;
		int Children[4] = {-1, -1, -1, -1};

		bool IsLeaf() const { return Children[0] < 0; }
	};

	std::vector<Node> mNodes;
	Aabb mWorldBounds{};
	float mHeightScale = 1.f;
	float mBaseY = 0.f;
	int mMaxDepth = 0;
	int mRoot = -1;

	const float* mHeightSamples = nullptr;
	int mHeightRes = 0;

	int AllocNode();
	void Subdivide(int nodeIndex);
	void SampleHeightRange(const DirectX::XMFLOAT2& uvOrigin, const DirectX::XMFLOAT2& uvSize,
		float& outMin01, float& outMax01) const;
	void SelectNode(
		int nodeIndex,
		const Frustum& frustum,
		const DirectX::XMFLOAT3& cameraPos,
		float lodFactor,
		std::vector<TerrainPatch>& outPatches) const;
};
