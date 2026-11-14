// One-shot heightmap tile generator (no D3D). Build+run:
//   cl /EHsc /O2 /Fe:tools\gen_terrain_heightmap.exe tools\gen_terrain_heightmap.cpp
//   tools\gen_terrain_heightmap.exe

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <cstdio>

namespace fs = std::filesystem;

static float Hash01(int x, int y, int seed)
{
	uint32_t n = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u +
		static_cast<uint32_t>(seed) * 1274126177u;
	n = (n ^ (n >> 13)) * 1274126177u;
	n = n ^ (n >> 16);
	return static_cast<float>(n & 0xFFFFu) / 65535.f;
}

static float ValueNoise(float x, float y, int seed)
{
	const int x0 = static_cast<int>(std::floor(x));
	const int y0 = static_cast<int>(std::floor(y));
	float fx = x - static_cast<float>(x0);
	float fy = y - static_cast<float>(y0);
	fx = fx * fx * (3.f - 2.f * fx);
	fy = fy * fy * (3.f - 2.f * fy);
	const float a = Hash01(x0, y0, seed) * (1.f - fx) + Hash01(x0 + 1, y0, seed) * fx;
	const float b = Hash01(x0, y0 + 1, seed) * (1.f - fx) + Hash01(x0 + 1, y0 + 1, seed) * fx;
	return a * (1.f - fy) + b * fy;
}

static void WriteBmpBgr(const fs::path& path, const uint8_t* bgr, int w, int h)
{
	const int rowStride = ((w * 3 + 3) / 4) * 4;
	const uint32_t pixelBytes = static_cast<uint32_t>(rowStride * h);
	const uint32_t fileSize = 54u + pixelBytes;
	std::ofstream out(path, std::ios::binary);
	if (!out)
		throw std::runtime_error("cannot create " + path.string());
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
	std::vector<uint8_t> row(static_cast<size_t>(rowStride), 0);
	for (int y = h - 1; y >= 0; --y)
	{
		for (int x = 0; x < w; ++x)
		{
			const size_t src = static_cast<size_t>(y * w + x) * 3u;
			const size_t dst = static_cast<size_t>(x * 3);
			row[dst] = bgr[src];
			row[dst + 1] = bgr[src + 1];
			row[dst + 2] = bgr[src + 2];
		}
		out.write(reinterpret_cast<const char*>(row.data()), rowStride);
	}
	if (!out)
		throw std::runtime_error("write failed " + path.string());
}

int main()
{
	try
	{
		const fs::path root = fs::path(__FILE__).parent_path().parent_path();
		const fs::path terrainDir = root / "content" / "terrain";
		const fs::path tileDir = terrainDir / "tiles";
		fs::create_directories(tileDir);

		constexpr int kSize = 512;
		constexpr int kTile = 256;
		std::vector<uint8_t> gray(static_cast<size_t>(kSize * kSize));
		std::vector<uint8_t> rgb(static_cast<size_t>(kSize * kSize * 3));

		for (int y = 0; y < kSize; ++y)
		{
			for (int x = 0; x < kSize; ++x)
			{
				const float nx = static_cast<float>(x) / static_cast<float>(kSize);
				const float ny = static_cast<float>(y) / static_cast<float>(kSize);
				float h = 0.f, amp = 1.f, freq = 2.f, norm = 0.f;
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
				const uint8_t v = static_cast<uint8_t>(std::clamp(h * 255.f, 0.f, 255.f));
				gray[static_cast<size_t>(y * kSize + x)] = v;
				const size_t pi = static_cast<size_t>(y * kSize + x) * 3u;
				rgb[pi] = static_cast<uint8_t>(22 + h * 40);
				rgb[pi + 1] = static_cast<uint8_t>(48 + h * 110);
				rgb[pi + 2] = static_cast<uint8_t>(28 + h * 70);
			}
		}

		std::vector<uint8_t> grayBgr(static_cast<size_t>(kSize * kSize * 3));
		for (int i = 0; i < kSize * kSize; ++i)
		{
			grayBgr[static_cast<size_t>(i) * 3] = gray[static_cast<size_t>(i)];
			grayBgr[static_cast<size_t>(i) * 3 + 1] = gray[static_cast<size_t>(i)];
			grayBgr[static_cast<size_t>(i) * 3 + 2] = gray[static_cast<size_t>(i)];
		}
		WriteBmpBgr(terrainDir / "heightmap.bmp", grayBgr.data(), kSize, kSize);
		WriteBmpBgr(terrainDir / "albedo.bmp", rgb.data(), kSize, kSize);

		const int n = kSize / kTile;
		for (int tz = 0; tz < n; ++tz)
		{
			for (int tx = 0; tx < n; ++tx)
			{
				std::vector<uint8_t> tile(static_cast<size_t>(kTile * kTile * 3));
				for (int y = 0; y < kTile; ++y)
				{
					for (int x = 0; x < kTile; ++x)
					{
						const uint8_t v = gray[static_cast<size_t>((tz * kTile + y) * kSize + (tx * kTile + x))];
						const size_t di = static_cast<size_t>(y * kTile + x) * 3u;
						tile[di] = tile[di + 1] = tile[di + 2] = v;
					}
				}
				char name[64];
				snprintf(name, sizeof(name), "tile_%d_%d.bmp", tx, tz);
				WriteBmpBgr(tileDir / name, tile.data(), kTile, kTile);
			}
		}
		std::cout << "Wrote terrain assets to " << terrainDir << "\n";
		return 0;
	}
	catch (const std::exception& ex)
	{
		std::cerr << ex.what() << "\n";
		return 1;
	}
}
