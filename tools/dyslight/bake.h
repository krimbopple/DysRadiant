#pragma once

#include "map.h"

#include <cstdint>

struct Light
{
	Vec3 origin;
	Vec3 color{ 1, 1, 1 };
	float intensity = 1.0f;
	float range = 512.0f;
	bool linearFalloff = true;
	bool isSpot = false;
	bool isDirectional = false;
	bool exportSprite = false;
	Vec3 direction{ 0, 0, -1 };
	float innerCone = 35.0f;
	float outerCone = 45.0f;
};

struct LmFace
{
	int faceIndex = 0;
	int solidIndex = 0;
	int faceInSolid = 0;
	int page = 0;
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;
	int border = 0;
	Vec3 ref;
	Vec3 uaxis{ 1, 0, 0 };
	Vec3 vaxis{ 0, 1, 0 };
	float minU = 0.0f;
	float minV = 0.0f;
	float stepU = 1.0f;
	float stepV = 1.0f;
};

struct PreviewVertex
{
	Vec3 position;
	float u = 0.0f;
	float v = 0.0f;
	int page = 0;
};

struct BakeResult
{
	int atlasSize = 1024;
	Vec3 ambient{ 64, 64, 64 };
	std::vector<std::vector<uint8_t>> pages;
	std::vector<LmFace> faces;

	// world space mesh with lightmap UVs for the editor preview
	std::vector<PreviewVertex> previewVertices;
	std::vector<uint32_t> previewIndices;
};

enum class BakeMode
{
	CbreEx = 1,
	ThreeDws = 2,
	ThreeDwsCbreMath = 3,
};

const char* bakeModeName( BakeMode mode );

struct BakeSettings
{
	int atlasSize = 0;
	int blurRadius = -1;
	int border = -1;
	float texelSize = 0;
	bool hasAmbient = false;
	Vec3 ambient{ 0, 0, 0 };
	bool shadows = true;
};

void bakeSettingsApplyMode( BakeMode mode, BakeSettings& settings );

std::vector<Light> collectLights( const Map& map );
BakeResult bakeMap( const Map& map, BakeMode mode, const fs::path& mapDir, const BakeSettings& settings );
bool writeBake( const fs::path& base, const BakeResult& result, BakeMode mode, const BakeSettings& settings, std::string& error );
