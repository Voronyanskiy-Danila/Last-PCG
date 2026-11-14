#include "TerrainQuadtree.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

int TerrainQuadtree::AllocNode()
{
	mNodes.push_back(Node{});
	return static_cast<int>(mNodes.size()) - 1;
}

void TerrainQuadtree::SampleHeightRange(
	const XMFLOAT2& uvOrigin,
	const XMFLOAT2& uvSize,
	float& outMin01,
	float& outMax01) const
{
	outMin01 = 0.f;
	outMax01 = 1.f;
	if (!mHeightSamples || mHeightRes <= 1)
		return;

	const int res = mHeightRes;
	const int x0 = (std::clamp)(static_cast<int>(uvOrigin.x * (res - 1)), 0, res - 1);
	const int y0 = (std::clamp)(static_cast<int>(uvOrigin.y * (res - 1)), 0, res - 1);
	const int x1 = (std::clamp)(static_cast<int>((uvOrigin.x + uvSize.x) * (res - 1)), 0, res - 1);
	const int y1 = (std::clamp)(static_cast<int>((uvOrigin.y + uvSize.y) * (res - 1)), 0, res - 1);

	// Coarse grid sample (~16x16) across the UV rect for min/max.
	constexpr int kSteps = 16;
	float hMin = 1.f;
	float hMax = 0.f;
	for (int j = 0; j <= kSteps; ++j)
	{
		const int y = y0 + (y1 - y0) * j / kSteps;
		for (int i = 0; i <= kSteps; ++i)
		{
			const int x = x0 + (x1 - x0) * i / kSteps;
			const float h = mHeightSamples[static_cast<size_t>(y * res + x)];
			hMin = (std::min)(hMin, h);
			hMax = (std::max)(hMax, h);
		}
	}
	outMin01 = hMin;
	outMax01 = hMax;
}

void TerrainQuadtree::Build(
	const XMFLOAT3& originMin,
	float worldSizeXZ,
	float heightScale,
	int maxDepth,
	const float* heightSamples01,
	int heightRes)
{
	mNodes.clear();
	mHeightScale = heightScale;
	mBaseY = originMin.y;
	mMaxDepth = (std::max)(0, maxDepth);
	mHeightSamples = heightSamples01;
	mHeightRes = heightRes;

	float rootMin01 = 0.f;
	float rootMax01 = 1.f;
	SampleHeightRange({0.f, 0.f}, {1.f, 1.f}, rootMin01, rootMax01);

	mWorldBounds.Min = {originMin.x, mBaseY + rootMin01 * heightScale, originMin.z};
	mWorldBounds.Max = {
		originMin.x + worldSizeXZ,
		mBaseY + rootMax01 * heightScale,
		originMin.z + worldSizeXZ};

	mRoot = AllocNode();
	Node& root = mNodes[static_cast<size_t>(mRoot)];
	root.Bounds = mWorldBounds;
	root.UvOrigin = {0.f, 0.f};
	root.UvSize = {1.f, 1.f};
	root.Level = 0;

	Subdivide(mRoot);
}

void TerrainQuadtree::Subdivide(int nodeIndex)
{
	const int level = mNodes[static_cast<size_t>(nodeIndex)].Level;
	if (level >= mMaxDepth)
		return;

	const Aabb parentBounds = mNodes[static_cast<size_t>(nodeIndex)].Bounds;
	const XMFLOAT2 uvOrigin = mNodes[static_cast<size_t>(nodeIndex)].UvOrigin;
	const XMFLOAT2 uvSize = mNodes[static_cast<size_t>(nodeIndex)].UvSize;

	const float midX = 0.5f * (parentBounds.Min.x + parentBounds.Max.x);
	const float midZ = 0.5f * (parentBounds.Min.z + parentBounds.Max.z);
	const float midU = uvOrigin.x + 0.5f * uvSize.x;
	const float midV = uvOrigin.y + 0.5f * uvSize.y;
	const float halfU = 0.5f * uvSize.x;
	const float halfV = 0.5f * uvSize.y;

	const float xs0[2] = {parentBounds.Min.x, midX};
	const float xs1[2] = {midX, parentBounds.Max.x};
	const float zs0[2] = {parentBounds.Min.z, midZ};
	const float zs1[2] = {midZ, parentBounds.Max.z};
	const float us0[2] = {uvOrigin.x, midU};
	const float vs0[2] = {uvOrigin.y, midV};

	for (int iz = 0; iz < 2; ++iz)
	{
		for (int ix = 0; ix < 2; ++ix)
		{
			const int child = AllocNode();
			mNodes[static_cast<size_t>(nodeIndex)].Children[iz * 2 + ix] = child;

			Node& c = mNodes[static_cast<size_t>(child)];
			c.UvOrigin = {us0[ix], vs0[iz]};
			c.UvSize = {halfU, halfV};
			c.Level = level + 1;

			float hMin01 = 0.f;
			float hMax01 = 1.f;
			SampleHeightRange(c.UvOrigin, c.UvSize, hMin01, hMax01);
			c.Bounds.Min = {xs0[ix], mBaseY + hMin01 * mHeightScale, zs0[iz]};
			c.Bounds.Max = {xs1[ix], mBaseY + hMax01 * mHeightScale, zs1[iz]};

			Subdivide(child);
		}
	}
}

void TerrainQuadtree::Select(
	const Frustum& frustum,
	const XMFLOAT3& cameraPos,
	float lodFactor,
	std::vector<TerrainPatch>& outPatches) const
{
	outPatches.clear();
	if (mRoot < 0)
		return;
	SelectNode(mRoot, frustum, cameraPos, lodFactor, outPatches);
}

void TerrainQuadtree::SelectNode(
	int nodeIndex,
	const Frustum& frustum,
	const XMFLOAT3& cameraPos,
	float lodFactor,
	std::vector<TerrainPatch>& outPatches) const
{
	const Node& node = mNodes[static_cast<size_t>(nodeIndex)];
	if (!frustum.IntersectsAabb(node.Bounds))
		return;

	const float sizeXZ = node.Bounds.Max.x - node.Bounds.Min.x;
	const float dist = DistancePointToAabb(cameraPos, node.Bounds);
	const bool wantSplit = !node.IsLeaf() && (dist < sizeXZ * lodFactor);

	if (!wantSplit)
	{
		TerrainPatch p{};
		p.Bounds = node.Bounds;
		p.UvOrigin = node.UvOrigin;
		p.UvSize = node.UvSize;
		p.Level = node.Level;
		outPatches.push_back(p);
		return;
	}

	for (int i = 0; i < 4; ++i)
	{
		const int child = node.Children[i];
		if (child >= 0)
			SelectNode(child, frustum, cameraPos, lodFactor, outPatches);
	}
}
