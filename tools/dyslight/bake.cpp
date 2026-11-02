#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "bake.h"

#include "texture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <vector>

namespace
{
struct Tri
{
	Vec3 a, b, c;
};

// grid of solid brushes, used to test whether a light is an occluder
// 3DWS uses pointvisible() per luxel and this is the equivalent1
class ShadowGrid
{
public:
	void build( const Map& map ) {
		m_solids.clear();
		m_min = Vec3{ 1e30f, 1e30f, 1e30f };
		Vec3 max{ -1e30f, -1e30f, -1e30f };
		bool any = false;

		for ( size_t solidIndex = 0; solidIndex < map.solids.size(); ++solidIndex ) {
			const Solid& solid = map.solids[ solidIndex ];
			Entry entry;
			entry.source = (int)solidIndex;
			for ( const Face& face : solid.faces ) {
				if ( face.vertices.size() < 3 )
					continue;
				if ( lower( face.material ).rfind( "tooltextures/", 0 ) == 0 )
					continue;
				for ( size_t i = 1; i + 1 < face.vertices.size(); ++i )
					entry.tris.push_back( { face.vertices[ 0 ], face.vertices[ i ], face.vertices[ i + 1 ] } );
				for ( const Vec3& v : face.vertices ) {
					entry.min = { std::min( entry.min.x, v.x ), std::min( entry.min.y, v.y ), std::min( entry.min.z, v.z ) };
					entry.max = { std::max( entry.max.x, v.x ), std::max( entry.max.y, v.y ), std::max( entry.max.z, v.z ) };
					entry.centroid = entry.centroid + v;
					++entry.vertices;
					any = true;
				}
			}
			if ( entry.tris.empty() )
				continue;
			entry.centroid = entry.centroid * ( 1.0f / (float)std::max( 1, entry.vertices ) );
			m_solids.push_back( std::move( entry ) );
		}

		if ( !any || m_solids.empty() )
			return;

		for ( const Entry& entry : m_solids ) {
			m_min = { std::min( m_min.x, entry.min.x ), std::min( m_min.y, entry.min.y ), std::min( m_min.z, entry.min.z ) };
			max = { std::max( max.x, entry.max.x ), std::max( max.y, entry.max.y ), std::max( max.z, entry.max.z ) };
		}

		const Vec3 size{ std::max( 1.0f, max.x - m_min.x ), std::max( 1.0f, max.y - m_min.y ), std::max( 1.0f, max.z - m_min.z ) };
		const float longest = std::max( size.x, std::max( size.y, size.z ) );
		m_cell = std::max( 32.0f, longest / 64.0f );
		m_nx = std::max( 1, (int)std::ceil( size.x / m_cell ) );
		m_ny = std::max( 1, (int)std::ceil( size.y / m_cell ) );
		m_nz = std::max( 1, (int)std::ceil( size.z / m_cell ) );
		m_cells.assign( (size_t)m_nx * m_ny * m_nz, {} );

		for ( size_t i = 0; i < m_solids.size(); ++i ) {
			const Entry& entry = m_solids[ i ];
			const int x0 = cellIndex( entry.min.x, 0 ), x1 = cellIndex( entry.max.x, 0 );
			const int y0 = cellIndex( entry.min.y, 1 ), y1 = cellIndex( entry.max.y, 1 );
			const int z0 = cellIndex( entry.min.z, 2 ), z1 = cellIndex( entry.max.z, 2 );
			for ( int z = z0; z <= z1; ++z )
				for ( int y = y0; y <= y1; ++y )
					for ( int x = x0; x <= x1; ++x )
						m_cells[ ( (size_t)z * m_ny + y ) * m_nx + x ].push_back( (int)i );
		}
		m_seen.assign( m_solids.size(), 0 );
	}

	// solids that enclose a point do not occlude it, as mappers routinely bury lights because they are retarded
	std::vector<uint8_t> ignoreMask( const Vec3& point ) const {
		std::vector<uint8_t> mask( m_solids.size(), 0 );
		for ( size_t i = 0; i < m_solids.size(); ++i )
			if ( containsPoint( m_solids[ i ], point ) )
				mask[ i ] = 1;
		return mask;
	}

	bool blocked( const Vec3& from, const Vec3& to, int ignoreSolid, const std::vector<uint8_t>& mask ) {
		if ( m_solids.empty() )
			return false;
		const Vec3 seg = to - from;
		const float length = seg.length();
		if ( length < 1e-3f )
			return false;

		const float skin = 2.0f;
		const float tMin = skin / length;
		const float tMaxHit = 1.0f - skin / length;
		if ( tMin >= tMaxHit )
			return false;

		++m_epoch;
		int cell[ 3 ] = { cellIndex( from.x, 0 ), cellIndex( from.y, 1 ), cellIndex( from.z, 2 ) };
		const int step[ 3 ] = { seg.x > 0.0f ? 1 : -1, seg.y > 0.0f ? 1 : -1, seg.z > 0.0f ? 1 : -1 };
		const float dir[ 3 ] = { seg.x / length, seg.y / length, seg.z / length };
		const float origin[ 3 ] = { from.x, from.y, from.z };
		const float boundMin[ 3 ] = { m_min.x, m_min.y, m_min.z };
		float tMax[ 3 ];
		float tDelta[ 3 ];
		for ( int axis = 0; axis < 3; ++axis ) {
			const float cellSize = m_cell;
			if ( std::abs( dir[ axis ] ) < 1e-8f ) {
				tMax[ axis ] = 1e30f;
				tDelta[ axis ] = 1e30f;
				continue;
			}
			const float next = boundMin[ axis ] + (float)( cell[ axis ] + ( step[ axis ] > 0 ? 1 : 0 ) ) * cellSize;
			tMax[ axis ] = ( next - origin[ axis ] ) / dir[ axis ];
			tDelta[ axis ] = cellSize / std::abs( dir[ axis ] );
		}

		float travelled = 0.0f;
		while ( travelled <= length ) {
			const size_t index = ( (size_t)cell[ 2 ] * m_ny + cell[ 1 ] ) * m_nx + cell[ 0 ];
			for ( int solid : m_cells[ index ] ) {
				if ( m_seen[ solid ] == m_epoch )
					continue;
				m_seen[ solid ] = m_epoch;
				const Entry& entry = m_solids[ solid ];
				if ( entry.source == ignoreSolid || ( !mask.empty() && mask[ solid ] ) )
					continue;
				if ( !segmentHitsSolid( entry, from, seg, tMin, tMaxHit ) )
					continue;
				if ( containsPoint( entry, from ) )
					continue;
				return true;
			}

			int axis = 0;
			if ( tMax[ 1 ] < tMax[ axis ] )
				axis = 1;
			if ( tMax[ 2 ] < tMax[ axis ] )
				axis = 2;
			travelled = tMax[ axis ];
			if ( travelled > length )
				break;
			cell[ axis ] += step[ axis ];
			if ( cell[ axis ] < 0 || cell[ axis ] >= ( axis == 0 ? m_nx : axis == 1 ? m_ny : m_nz ) )
				break;
			tMax[ axis ] += tDelta[ axis ];
		}
		return false;
	}

private:
	struct Entry
	{
		int source = -1;
		Vec3 min{ 1e30f, 1e30f, 1e30f };
		Vec3 max{ -1e30f, -1e30f, -1e30f };
		Vec3 centroid;
		int vertices = 0;
		std::vector<Tri> tris;
	};

	int cellIndex( float value, int axis ) const {
		const float base = axis == 0 ? m_min.x : axis == 1 ? m_min.y : m_min.z;
		const int count = axis == 0 ? m_nx : axis == 1 ? m_ny : m_nz;
		const int index = (int)std::floor( ( value - base ) / m_cell );
		return std::clamp( index, 0, count - 1 );
	}

	static bool segmentHitsAabb( const Entry& entry, const Vec3& from, const Vec3& seg ) {
		float t0 = 0.0f, t1 = 1.0f;
		const float origin[ 3 ] = { from.x, from.y, from.z };
		const float delta[ 3 ] = { seg.x, seg.y, seg.z };
		const float lo[ 3 ] = { entry.min.x, entry.min.y, entry.min.z };
		const float hi[ 3 ] = { entry.max.x, entry.max.y, entry.max.z };
		for ( int axis = 0; axis < 3; ++axis ) {
			if ( std::abs( delta[ axis ] ) < 1e-8f ) {
				if ( origin[ axis ] < lo[ axis ] || origin[ axis ] > hi[ axis ] )
					return false;
				continue;
			}
			float near = ( lo[ axis ] - origin[ axis ] ) / delta[ axis ];
			float far = ( hi[ axis ] - origin[ axis ] ) / delta[ axis ];
			if ( near > far )
				std::swap( near, far );
			t0 = std::max( t0, near );
			t1 = std::min( t1, far );
			if ( t0 > t1 )
				return false;
		}
		return true;
	}

	static bool segmentHitsTriangle( const Tri& tri, const Vec3& from, const Vec3& seg, float tMin, float tMax ) {
		// moller trumbore https://stackoverflow.com/questions/42740765/intersection-between-line-and-triangle-in-3d
		const Vec3 edge1 = tri.b - tri.a;
		const Vec3 edge2 = tri.c - tri.a;
		const Vec3 h = seg.cross( edge2 );
		const float det = edge1.dot( h );
		if ( std::abs( det ) < 1e-9f )
			return false;
		const float invDet = 1.0f / det;
		const Vec3 s = from - tri.a;
		const float u = s.dot( h ) * invDet;
		if ( u < 0.0f || u > 1.0f )
			return false;
		const Vec3 q = s.cross( edge1 );
		const float v = seg.dot( q ) * invDet;
		if ( v < 0.0f || u + v > 1.0f )
			return false;
		const float t = edge2.dot( q ) * invDet;
		return t > tMin && t < tMax;
	}

	static bool segmentHitsSolid( const Entry& entry, const Vec3& from, const Vec3& seg, float tMin, float tMax ) {
		if ( !segmentHitsAabb( entry, from, seg ) )
			return false;
		for ( const Tri& tri : entry.tris )
			if ( segmentHitsTriangle( tri, from, seg, tMin, tMax ) )
				return true;
		return false;
	}

	static bool containsPoint( const Entry& entry, const Vec3& p ) {
		if ( p.x < entry.min.x || p.x > entry.max.x || p.y < entry.min.y || p.y > entry.max.y || p.z < entry.min.z || p.z > entry.max.z )
			return false;

		for ( const Tri& tri : entry.tris ) {
			Vec3 n = ( tri.b - tri.a ).cross( tri.c - tri.a );
			if ( n.dot( entry.centroid - tri.a ) > 0.0f )
				n = n * -1.0f;
			if ( n.dot( p - tri.a ) > 0.01f )
				return false;
		}
		return true;
	}

	std::vector<Entry> m_solids;
	std::vector<std::vector<int>> m_cells;
	std::vector<uint32_t> m_seen;
	uint32_t m_epoch = 0;
	Vec3 m_min;
	float m_cell = 128.0f;
	int m_nx = 1, m_ny = 1, m_nz = 1;
};
}

std::vector<Light> collectLights( const Map& map ) {
	std::vector<Light> lights;
	for ( const Entity& ent : map.entities ) {
		std::string cls = lower( ent.classname );
		if ( cls.find( "light" ) == std::string::npos && cls != "spotlight" )
			continue;
		Light light;
		light.origin = ent.origin;
		auto get = [ & ]( const std::string& key, const std::string& fallback = {} ) -> std::string {
			auto it = ent.keys.find( key );
			return it != ent.keys.end() ? it->second : fallback;
		};
		std::string sprite = lower( get( "hassprite", "true" ) );
		light.exportSprite = ( cls == "light" || cls == "spotlight" ) && sprite != "no" && sprite != "false" && sprite != "0";
		Vec3 color = parseVec3( get( "color", get( "_light", "255 255 255" ) ), { 255, 255, 255 } );
		light.color = { color.x / 255.0f, color.y / 255.0f, color.z / 255.0f };
		light.intensity = (float)std::atof( get( "intensity", get( "_lightscale", "1" ) ).c_str() );
		light.range = (float)std::atof( get( "range", get( "_range", get( "_distance", "512" ) ) ).c_str() );
		if ( light.range < 0.0f )
			light.range = 0.0f;
		light.linearFalloff = std::atoi( get( "linearfalloff", "1" ).c_str() ) != 0;
		light.isSpot = cls.find( "spot" ) != std::string::npos;
		light.isDirectional = cls.find( "sun" ) != std::string::npos || cls.find( "environment" ) != std::string::npos;
		if ( light.isDirectional || light.isSpot ) {
			Vec3 ang = parseVec3( get( "angles", "0 0 0" ) );
			float pitch = ang.x * 3.14159265f / 180.0f;
			float yaw = ang.y * 3.14159265f / 180.0f;
			light.direction = Vec3{ std::sin( yaw ) * std::cos( pitch ), std::cos( yaw ) * std::cos( pitch ), -std::sin( pitch ) }.normalized();
			if ( light.isSpot ) {
				light.innerCone = (float)std::atof( get( "innerconeangle", "35" ).c_str() );
				light.outerCone = (float)std::atof( get( "outerconeangle", "45" ).c_str() );
			}
		}
		lights.push_back( light );
	}
	return lights;
}

static void ensureBasis( Face& face ) {
	Vec3 n = face.normal.normalized();
	if ( n.length() < 0.5f && face.vertices.size() >= 3 )
		n = ( face.vertices[ 1 ] - face.vertices[ 0 ] ).cross( face.vertices[ 2 ] - face.vertices[ 0 ] ).normalized();
	face.normal = n;
	face.uaxis = face.uaxis.normalized();
	face.vaxis = face.vaxis.normalized();
	if ( face.uaxis.length() < 0.5f || std::abs( face.uaxis.dot( n ) ) > 0.99f ) {
		Vec3 axis = std::abs( n.z ) < 0.9f ? Vec3{ 0, 0, 1 } : Vec3{ 1, 0, 0 };
		face.uaxis = n.cross( axis ).normalized();
	}
	face.vaxis = n.cross( face.uaxis ).normalized();
	if ( face.vaxis.length() < 0.5f )
		face.vaxis = n.cross( face.uaxis ).normalized();
}

const char* bakeModeName( BakeMode mode ) {
	switch ( mode ) {
	case BakeMode::CbreEx:
		return "cbre";
	case BakeMode::ThreeDws:
		return "3dws";
	case BakeMode::ThreeDwsCbreMath:
		return "3dws-cbre";
	}
	return "cbre";
}

void bakeSettingsApplyMode( BakeMode mode, BakeSettings& settings ) {
	const bool cbreSheet = mode == BakeMode::CbreEx;
	if ( settings.atlasSize <= 0 )
		settings.atlasSize = cbreSheet ? 1024 : 512;
	if ( settings.blurRadius < 0 )
		settings.blurRadius = cbreSheet ? 0 : 1;
	if ( settings.border < 0 )
		settings.border = cbreSheet ? 0 : 2;
}

BakeResult bakeMap( const Map& map, BakeMode mode, const fs::path& mapDir, const BakeSettings& settings ) {
	BakeResult result;
	const bool cbreSheet = mode == BakeMode::CbreEx;
	const bool squareNdl = mode == BakeMode::ThreeDws;
	const bool halfLight = mode == BakeMode::ThreeDws;
	result.atlasSize = settings.atlasSize > 0 ? settings.atlasSize : ( cbreSheet ? 1024 : 512 );
	const int atlasSize = result.atlasSize;
	const int maxFaceDim = cbreSheet ? 256 : 128;
	const int border = settings.border >= 0 ? settings.border : ( cbreSheet ? 0 : 2 );
	const int blurRadius = settings.blurRadius >= 0 ? settings.blurRadius : ( cbreSheet ? 0 : 1 );
	const bool shadows = settings.shadows;
	std::vector<Light> lights = collectLights( map );

	// 3DWS starts every lightmap at ambientlight / 2 and defaults ambientlight to 0x40
	Vec3 ambient{ 64.0f / 510.0f, 64.0f / 510.0f, 64.0f / 510.0f };
	if ( settings.hasAmbient ) {
		ambient = { settings.ambient.x / 510.0f, settings.ambient.y / 510.0f, settings.ambient.z / 510.0f };
	} else {
		for ( const char* key : { "ambient", "_ambient", "ambient_light" } ) {
			auto it = map.worldKeys.find( key );
			if ( it == map.worldKeys.end() )
				continue;
			const Vec3 value = parseVec3( it->second );
			ambient = { value.x / 510.0f, value.y / 510.0f, value.z / 510.0f };
			break;
		}
	}
	result.ambient = { ambient.x * 510.0f, ambient.y * 510.0f, ambient.z * 510.0f };

	ShadowGrid shadow;
	if ( shadows && !lights.empty() )
		shadow.build( map );

	// a light buried inside something must not shadow the whole room...
	std::vector<std::vector<uint8_t>> lightIgnore;
	if ( shadows ) {
		lightIgnore.reserve( lights.size() );
		for ( const Light& light : lights )
			lightIgnore.push_back( shadow.ignoreMask( light.origin ) );
	}

	struct Pending
	{
		int faceIndex = 0;
		int solidIndex = 0;
		int faceInSolid = 0;
		int w = 0;
		int h = 0;
		int border = 0;
		Vec3 ref;
		Vec3 uaxis{ 1, 0, 0 };
		Vec3 vaxis{ 0, 1, 0 };
		Vec3 normal{ 0, 0, 1 };
		float minU = 0.0f;
		float minV = 0.0f;
		float stepU = 1.0f;
		float stepV = 1.0f;
	};

	std::vector<Pending> pending;
	int globalFace = 0;
	for ( size_t si = 0; si < map.solids.size(); ++si ) {
		for ( size_t fi = 0; fi < map.solids[ si ].faces.size(); ++fi ) {
			const Face& mapFace = map.solids[ si ].faces[ fi ];
			if ( mapFace.vertices.size() < 3 )
				continue;
			if ( lower( mapFace.material ).rfind( "tooltextures/", 0 ) == 0 )
				continue;
			if ( textureHasAlpha( mapFace.material, mapDir ) )
				continue;

			Face face = mapFace;
			ensureBasis( face );

			float lmScale = settings.texelSize > 0.0f ? settings.texelSize : ( face.lmScale > 0.0f ? face.lmScale : 16.0f );
			Vec3 ref = face.vertices[ 0 ];
			float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;

			for ( const Vec3& v : face.vertices ) {
				Vec3 d = v - ref;
				float u = d.dot( face.uaxis );
				float vv = d.dot( face.vaxis );
				minU = std::min( minU, u );
				maxU = std::max( maxU, u );
				minV = std::min( minV, vv );
				maxV = std::max( maxV, vv );
			}

			float width = std::max( 0.0f, maxU - minU );
			float height = std::max( 0.0f, maxV - minV );
			const int faceBorder = std::min( border, std::max( 0, ( maxFaceDim - 2 ) / 2 ) );
			int faceW = std::max( 2, (int)std::ceil( width / lmScale ) + 1 );
			int faceH = std::max( 2, (int)std::ceil( height / lmScale ) + 1 );
			faceW = std::min( faceW, std::max( 2, maxFaceDim - faceBorder * 2 ) );
			faceH = std::min( faceH, std::max( 2, maxFaceDim - faceBorder * 2 ) );
			const int w = faceW + faceBorder * 2;
			const int h = faceH + faceBorder * 2;

			Pending p;
			p.faceIndex = globalFace++;
			p.solidIndex = (int)si;
			p.faceInSolid = (int)fi;
			p.w = w;
			p.h = h;
			p.border = faceBorder;
			p.ref = ref;
			p.uaxis = face.uaxis;
			p.vaxis = face.vaxis;
			p.normal = face.normal.normalized();
			p.minU = minU;
			p.minV = minV;
			p.stepU = width > 0.0f ? width / (float)( faceW - 1 ) : lmScale;
			p.stepV = height > 0.0f ? height / (float)( faceH - 1 ) : lmScale;
			pending.push_back( p );
		}
	}
	if ( pending.empty() )
		return result;

	// give every face a small lightmap, then pack them into one atlas
	std::vector<int> order( pending.size() );
	for ( size_t i = 0; i < order.size(); ++i )
		order[ i ] = (int)i;
	std::sort( order.begin(), order.end(), [ & ]( int a, int b ) { return pending[ a ].h > pending[ b ].h; } );

	struct PageState
	{
		int x = 0;
		int y = 0;
		int rowH = 0;
	};

	std::vector<PageState> pages;
	pages.push_back( {} );
	std::vector<int> pageFor( pending.size(), -1 );
	for ( int idx : order ) {
		const Pending& p = pending[ idx ];
		int chosen = -1;
		for ( size_t pi = 0; pi < pages.size(); ++pi ) {
			PageState& ps = pages[ pi ];
			if ( ps.x + p.w <= atlasSize && ps.y + p.h <= atlasSize ) {
				chosen = (int)pi;
				break;
			}
			if ( ps.x > 0 && ps.y + ps.rowH + p.h <= atlasSize ) {
				ps.y += ps.rowH;
				ps.x = 0;
				ps.rowH = 0;
				if ( p.w <= atlasSize && ps.y + p.h <= atlasSize ) {
					chosen = (int)pi;
					break;
				}
			}
		}

		if ( chosen < 0 ) {
			pages.push_back( {} );
			chosen = (int)pages.size() - 1;
		}

		PageState& ps = pages[ chosen ];
		LmFace lf;
		lf.faceIndex = p.faceIndex;
		lf.solidIndex = p.solidIndex;
		lf.faceInSolid = p.faceInSolid;
		lf.page = chosen;
		lf.x = ps.x;
		lf.y = ps.y;
		lf.w = p.w;
		lf.h = p.h;
		lf.border = p.border;
		lf.ref = p.ref;
		lf.uaxis = p.uaxis;
		lf.vaxis = p.vaxis;
		lf.minU = p.minU;
		lf.minV = p.minV;
		lf.stepU = p.stepU;
		lf.stepV = p.stepV;
		result.faces.push_back( lf );
		pageFor[ idx ] = chosen;

		ps.x += p.w;
		ps.rowH = std::max( ps.rowH, p.h );
		if ( ps.x >= atlasSize ) {
			ps.x = 0;
			ps.y += ps.rowH;
			ps.rowH = 0;
		}
	}

	for ( const LmFace& lf : result.faces ) {
		const Face& face = map.solids[ lf.solidIndex ].faces[ lf.faceInSolid ];
		const uint32_t base = (uint32_t)result.previewVertices.size();
		for ( const Vec3& vertex : face.vertices ) {
			const float localU = ( vertex - lf.ref ).dot( lf.uaxis );
			const float localV = ( vertex - lf.ref ).dot( lf.vaxis );
			const float u = ( (float)lf.x + (float)lf.border + 0.5f + ( localU - lf.minU ) / std::max( 0.0001f, lf.stepU ) ) / (float)atlasSize;
			const float v = ( (float)lf.y + (float)lf.border + 0.5f + ( localV - lf.minV ) / std::max( 0.0001f, lf.stepV ) ) / (float)atlasSize;
			result.previewVertices.push_back( { vertex, u, v, lf.page } );
		}
		for ( size_t i = 1; i + 1 < face.vertices.size(); ++i ) {
			result.previewIndices.push_back( base );
			result.previewIndices.push_back( base + (uint32_t)i );
			result.previewIndices.push_back( base + (uint32_t)i + 1 );
		}
	}

	result.pages.resize( pages.size() );
	for ( auto& page : result.pages )
		page.assign( (size_t)atlasSize * atlasSize * 4, 0 );
	for ( size_t pi = 0; pi < result.pages.size(); ++pi ) {
		for ( size_t k = 3; k < result.pages[ pi ].size(); k += 4 )
			result.pages[ pi ][ k ] = 255;
	}

	for ( int idx = 0; idx < (int)pending.size(); ++idx ) {
		const Pending& p = pending[ idx ];
		int page = pageFor[ idx ];
		int rectIndex = -1;
		for ( size_t i = 0; i < result.faces.size(); ++i ) {
			if ( result.faces[ i ].faceIndex == p.faceIndex ) {
				rectIndex = (int)i;
				break;
			}
		}
		if ( rectIndex < 0 )
			continue;
		const LmFace& rect = result.faces[ rectIndex ];
		for ( int y = 0; y < p.h; ++y ) {
			for ( int x = 0; x < p.w; ++x ) {
				float u = p.minU + (float)( x - p.border ) * p.stepU;
				float v = p.minV + (float)( y - p.border ) * p.stepV;
				Vec3 pos = p.ref + p.uaxis * u + p.vaxis * v + p.normal * 0.25f;
				Vec3 color = ambient;
				for ( size_t li = 0; li < lights.size(); ++li ) {
					const Light& light = lights[ li ];
					Vec3 toLight = light.origin - pos;
					float dist = toLight.length();
					Vec3 dir;
					float att = 1.0f;
					if ( light.isDirectional ) {
						dir = -light.direction.normalized();
						dist = 1.0f;
					} else {
						if ( dist < 0.001f )
							continue;
						att = light.range > 0.0f ? std::max( 0.0f, 1.0f - dist / light.range ) : 1.0f;
						if ( att <= 0.0f )
							continue;
						if ( !light.linearFalloff )
							att = std::sqrt( att );
						dir = toLight * ( 1.0f / dist );
					}
					float ndl = std::max( 0.0f, p.normal.dot( dir ) );
					if ( squareNdl )
						ndl *= ndl;
					if ( light.isSpot ) {
						float cosAngle = light.direction.dot( -dir );
						float outer = std::cos( light.outerCone * 3.14159265f / 180.0f );
						float inner = std::cos( light.innerCone * 3.14159265f / 180.0f );
						if ( cosAngle < outer )
							continue;
						if ( cosAngle < inner )
							att *= ( cosAngle - outer ) / std::max( 0.0001f, inner - outer );
					}
					if ( shadows && !light.isDirectional && shadow.blocked( pos, light.origin, p.solidIndex, lightIgnore[ li ] ) )
						continue;
					float scale = light.intensity * att * ndl;
					if ( halfLight )
						scale *= 0.5f;
					color = color + Vec3{ light.color.x * scale, light.color.y * scale, light.color.z * scale };
				}
				size_t dst = ( (size_t)( rect.y + y ) * atlasSize + (size_t)( rect.x + x ) ) * 4;
				result.pages[ page ][ dst + 0 ] = (uint8_t)std::clamp( (int)( color.x * 255.0f + 0.5f ), 0, 255 );
				result.pages[ page ][ dst + 1 ] = (uint8_t)std::clamp( (int)( color.y * 255.0f + 0.5f ), 0, 255 );
				result.pages[ page ][ dst + 2 ] = (uint8_t)std::clamp( (int)( color.z * 255.0f + 0.5f ), 0, 255 );
			}
		}
	}

	// 3DWS blurs every face lightmap so luxel steps do not show
	if ( blurRadius > 0 ) {
		for ( const LmFace& f : result.faces ) {
			std::vector<uint8_t>& page = result.pages[ f.page ];
			const int rw = f.w, rh = f.h;
			if ( rw <= 0 || rh <= 0 )
				continue;
			std::vector<uint8_t> src( (size_t)rw * (size_t)rh * 4 );
			for ( int y = 0; y < rh; ++y )
				std::memcpy( &src[ (size_t)y * (size_t)rw * 4 ], &page[ ( (size_t)( f.y + y ) * (size_t)atlasSize + (size_t)f.x ) * 4 ], (size_t)rw * 4 );
			for ( int y = 0; y < rh; ++y ) {
				for ( int x = 0; x < rw; ++x ) {
					int r = 0, g = 0, b = 0, n = 0;
					for ( int dy = -blurRadius; dy <= blurRadius; ++dy ) {
						const int sy = y + dy;
						if ( sy < 0 || sy >= rh )
							continue;
						for ( int dx = -blurRadius; dx <= blurRadius; ++dx ) {
							const int sx = x + dx;
							if ( sx < 0 || sx >= rw )
								continue;
							const size_t s = ( (size_t)sy * (size_t)rw + (size_t)sx ) * 4;
							r += src[ s ];
							g += src[ s + 1 ];
							b += src[ s + 2 ];
							++n;
						}
					}
					const size_t d = ( (size_t)( f.y + y ) * (size_t)atlasSize + (size_t)( f.x + x ) ) * 4;
					page[ d ] = (uint8_t)( r / n );
					page[ d + 1 ] = (uint8_t)( g / n );
					page[ d + 2 ] = (uint8_t)( b / n );
				}
			}
		}
	}
	return result;
}

static std::string jsonEscape( const std::string& s ) {
	std::string out;
	for ( char c : s ) {
		if ( c == '\\' || c == '"' )
			out.push_back( '\\' );
		out.push_back( c );
	}
	return out;
}

bool writeBake( const fs::path& base, const BakeResult& result, BakeMode mode, const BakeSettings& settings, std::string& error ) {
	for ( size_t i = 0; i < result.pages.size(); ++i ) {
		std::string name = base.filename().string() + "_lm" + ( result.pages.size() > 1 ? "_" + std::to_string( i ) : "" ) + ".png";
		fs::path out = base.parent_path() / name;
		const size_t pixels = (size_t)result.atlasSize * (size_t)result.atlasSize;
		std::vector<uint8_t> rgb( pixels * 3 );
		for ( size_t p = 0; p < pixels; ++p ) {
			rgb[ p * 3 + 0 ] = result.pages[ i ][ p * 4 + 0 ];
			rgb[ p * 3 + 1 ] = result.pages[ i ][ p * 4 + 1 ];
			rgb[ p * 3 + 2 ] = result.pages[ i ][ p * 4 + 2 ];
		}
		if ( !stbi_write_png( out.string().c_str(), result.atlasSize, result.atlasSize, 3, rgb.data(), result.atlasSize * 3 ) ) {
			error = "could not write " + out.string();
			return false;
		}
	}

	fs::path manifest = base.parent_path() / ( base.filename().string() + "_lm.json" );
	std::ofstream os( manifest );
	if ( !os ) {
		error = "could not write " + manifest.string();
		return false;
	}
	os << "{\n";
	os << "  \"format\": \"dyslight-v1\",\n";
	os << "  \"style\": \"" << bakeModeName( mode ) << "\",\n";
	os << "  \"atlas_size\": " << result.atlasSize << ",\n";
	os << "  \"blur_radius\": " << settings.blurRadius << ",\n";
	os << "  \"border\": " << settings.border << ",\n";
	os << "  \"texel_size\": " << settings.texelSize << ",\n";
	os << "  \"ambient\": [" << result.ambient.x << ", " << result.ambient.y << ", " << result.ambient.z << "],\n";
	os << "  \"shadows\": " << ( settings.shadows ? "true" : "false" ) << ",\n";
	os << "  \"pages\": [";
	for ( size_t i = 0; i < result.pages.size(); ++i ) {
		if ( i )
			os << ", ";
		os << "\"" << jsonEscape( base.filename().string() + "_lm" + ( result.pages.size() > 1 ? "_" + std::to_string( i ) : "" ) + ".png" ) << "\"";
	}
	os << "],\n";
	os << "  \"faces\": [\n";
	for ( size_t i = 0; i < result.faces.size(); ++i ) {
		const LmFace& f = result.faces[ i ];
		os << "    {\"face\": " << f.faceIndex << ", \"page\": " << f.page << ", \"x\": " << f.x << ", \"y\": " << f.y << ", \"w\": " << f.w << ", \"h\": " << f.h << ", \"border\": " << f.border << ", \"min_u\": " << f.minU << ", \"min_v\": " << f.minV << ", \"step_u\": " << f.stepU << ", \"step_v\": " << f.stepV << "}";
		if ( i + 1 < result.faces.size() )
			os << ",";
		os << "\n";
	}
	os << "  ]\n}\n";

	fs::path preview = base.parent_path() / ( base.filename().string() + "_lm_preview.txt" );
	std::ofstream ps( preview );
	if ( ps ) {
		ps << std::setprecision( 9 );
		ps << "DYSLIGHT_PREVIEW 1\n";
		ps << "atlas " << result.atlasSize << "\n";
		for ( size_t i = 0; i < result.pages.size(); ++i ) {
			ps << "page " << i << " " << base.filename().string() << "_lm" << ( result.pages.size() > 1 ? "_" + std::to_string( i ) : "" ) << ".png\n";
		}
		for ( const PreviewVertex& v : result.previewVertices )
			ps << "v " << v.position.x << " " << v.position.y << " " << v.position.z << " " << v.u << " " << v.v << " " << v.page << "\n";
		for ( size_t i = 0; i + 2 < result.previewIndices.size(); i += 3 )
			ps << "i " << result.previewIndices[ i ] << " " << result.previewIndices[ i + 1 ] << " " << result.previewIndices[ i + 2 ] << "\n";
	}
	return true;
}
