#pragma once

#include <cmath>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// everything here is Z-up, the 3DWS loader converts from Y-up
struct Vec3
{
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;

	Vec3 operator+( const Vec3& o ) const {
		return { x + o.x, y + o.y, z + o.z };
	}

	Vec3 operator-( const Vec3& o ) const {
		return { x - o.x, y - o.y, z - o.z };
	}

	Vec3 operator-() const {
		return { -x, -y, -z };
	}

	Vec3 operator*( float s ) const {
		return { x * s, y * s, z * s };
	}

	float dot( const Vec3& o ) const {
		return x * o.x + y * o.y + z * o.z;
	}

	Vec3 cross( const Vec3& o ) const {
		return { y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x };
	}

	float length() const {
		return std::sqrt( dot( *this ) );
	}

	Vec3 normalized() const {
		float l = length();
		return l > 1e-8f ? ( *this * ( 1.0f / l ) ) : Vec3{};
	}
};

struct Face
{
	int id = 0;
	std::string material = "white";
	Vec3 normal;
	float dist = 0.0f;
	Vec3 uaxis{ 1, 0, 0 };
	Vec3 vaxis{ 0, 1, 0 };
	float uShift = 0.0f;
	float vShift = 0.0f;
	float uScale = 1.0f;
	float vScale = 1.0f;
	float lmScale = 16.0f;
	int smoothing = 0;
	std::vector<Vec3> vertices;
};

struct Solid
{
	int id = 0;
	int visgroupId = 0;
	int color[ 3 ] = { 255, 255, 255 };
	std::vector<Face> faces;
};

struct Visgroup
{
	int id = 0;
	std::string name;
	int color[ 3 ] = { 255, 255, 255 };
};

struct Entity
{
	int id = 0;
	std::string classname;
	Vec3 origin;
	std::map<std::string, std::string> keys;
	int visgroupId = 0;
	std::vector<Solid> brushSolids;
};

struct Map
{
	std::string name;
	std::vector<Solid> solids;
	std::vector<Entity> entities;
	std::vector<Visgroup> visgroups;
	std::map<std::string, std::string> worldKeys; // worldspawn properties
};

std::string lower( std::string s );
Vec3 parseVec3( const std::string& s, Vec3 fallback = {} );
void orientSolidFaces( Solid& solid );
