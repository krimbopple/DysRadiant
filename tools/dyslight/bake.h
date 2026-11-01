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
	float minU = 0.0f;
	float minV = 0.0f;
	float stepU = 1.0f;
	float stepV = 1.0f;
};

struct BakeResult
{
	int atlasSize = 1024;
	std::vector<std::vector<uint8_t>> pages;
	std::vector<LmFace> faces;
};

std::vector<Light> collectLights( const Map& map );
BakeResult bakeMap( const Map& map, bool cbreStyle );
bool writeBake( const fs::path& base, const BakeResult& result, bool cbreStyle, std::string& error );
