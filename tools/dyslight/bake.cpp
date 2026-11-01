#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "bake.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>

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
		light.range = (float)std::atof( get( "range", get( "_range", "10000" ) ).c_str() );
		if ( light.range < 0.0f )
			light.range = 0.0f;
		light.linearFalloff = std::atoi( get( "linearfalloff", "1" ).c_str() ) != 0;
		light.isSpot = cls.find( "spot" ) != std::string::npos;
		light.isDirectional = cls.find( "sun" ) != std::string::npos || cls.find( "environment" ) != std::string::npos;
		if ( light.isDirectional ) {
			light.direction = { 0, 0, -1 };
		} else if ( light.isSpot ) {
			Vec3 ang = parseVec3( get( "angles", "0 0 0" ) );
			float pitch = ang.x * 3.14159265f / 180.0f;
			float yaw = ang.y * 3.14159265f / 180.0f;
			float roll = ang.z * 3.14159265f / 180.0f;
			(void)roll;
			light.direction = Vec3{ std::cos( pitch ) * std::cos( yaw ), std::cos( pitch ) * std::sin( yaw ), std::sin( pitch ) }.normalized();
			light.innerCone = (float)std::atof( get( "innerconeangle", "35" ).c_str() );
			light.outerCone = (float)std::atof( get( "outerconeangle", "45" ).c_str() );
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

BakeResult bakeMap( const Map& map, bool cbreStyle ) {
	BakeResult result;
	result.atlasSize = cbreStyle ? 1024 : 512;
	const int atlasSize = result.atlasSize;
	const int maxFaceDim = cbreStyle ? 256 : 128;
	std::vector<Light> lights = collectLights( map );

	struct Pending
	{
		int faceIndex = 0;
		int solidIndex = 0;
		int faceInSolid = 0;
		int w = 0;
		int h = 0;
		float minU = 0.0f;
		float minV = 0.0f;
		float stepU = 1.0f;
		float stepV = 1.0f;
	};

	std::vector<Pending> pending;
	int globalFace = 0;
	for ( size_t si = 0; si < map.solids.size(); ++si ) {
		for ( size_t fi = 0; fi < map.solids[ si ].faces.size(); ++fi ) {
			Face face = map.solids[ si ].faces[ fi ];
			ensureBasis( face );

			float lmScale = face.lmScale > 0.0f ? face.lmScale : 16.0f;
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
			int w = std::max( 2, (int)std::ceil( width / lmScale ) + 1 );
			int h = std::max( 2, (int)std::ceil( height / lmScale ) + 1 );
			w = std::min( w, maxFaceDim );
			h = std::min( h, maxFaceDim );

			Pending p;
			p.faceIndex = globalFace++;
			p.solidIndex = (int)si;
			p.faceInSolid = (int)fi;
			p.w = w;
			p.h = h;
			p.minU = minU;
			p.minV = minV;
			p.stepU = width > 0.0f ? width / (float)( w - 1 ) : lmScale;
			p.stepV = height > 0.0f ? height / (float)( h - 1 ) : lmScale;
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

	result.pages.resize( pages.size() );
	for ( auto& page : result.pages )
		page.assign( (size_t)atlasSize * atlasSize * 4, 0 );
	for ( size_t pi = 0; pi < result.pages.size(); ++pi ) {
		for ( size_t k = 3; k < result.pages[ pi ].size(); k += 4 )
			result.pages[ pi ][ k ] = 255;
	}

	for ( int idx = 0; idx < (int)pending.size(); ++idx ) {
		const Pending& p = pending[ idx ];
		const Face& face = map.solids[ p.solidIndex ].faces[ p.faceInSolid ];
		Vec3 ref = face.vertices[ 0 ];
		Vec3 normal = face.normal.normalized();
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
				float u = p.minU + (float)x * p.stepU;
				float v = p.minV + (float)y * p.stepV;
				Vec3 pos = ref + face.uaxis * u + face.vaxis * v + normal * 0.25f;
				Vec3 color{ 0, 0, 0 };
				for ( const Light& light : lights ) {
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
					float ndl = std::max( 0.0f, normal.dot( dir ) );
					if ( cbreStyle ) {
						ndl = ndl;
					} else {
						ndl *= ndl;
					}
					if ( light.isSpot ) {
						float cosAngle = light.direction.dot( -dir );
						float outer = std::cos( light.outerCone * 3.14159265f / 180.0f );
						float inner = std::cos( light.innerCone * 3.14159265f / 180.0f );
						if ( cosAngle < outer )
							continue;
						if ( cosAngle < inner )
							att *= ( cosAngle - outer ) / std::max( 0.0001f, inner - outer );
					}
					float scale = light.intensity * att * ndl;
					color = color + Vec3{ light.color.x * scale, light.color.y * scale, light.color.z * scale };
				}
				size_t dst = ( (size_t)( rect.y + y ) * atlasSize + (size_t)( rect.x + x ) ) * 4;
				result.pages[ page ][ dst + 0 ] = (uint8_t)std::clamp( (int)( color.x * 255.0f + 0.5f ), 0, 255 );
				result.pages[ page ][ dst + 1 ] = (uint8_t)std::clamp( (int)( color.y * 255.0f + 0.5f ), 0, 255 );
				result.pages[ page ][ dst + 2 ] = (uint8_t)std::clamp( (int)( color.z * 255.0f + 0.5f ), 0, 255 );
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

bool writeBake( const fs::path& base, const BakeResult& result, bool cbreStyle, std::string& error ) {
	for ( size_t i = 0; i < result.pages.size(); ++i ) {
		std::string name = base.filename().string() + "_lm" + ( result.pages.size() > 1 ? "_" + std::to_string( i ) : "" ) + ".png";
		fs::path out = base.parent_path() / name;
		if ( !stbi_write_png( out.string().c_str(), result.atlasSize, result.atlasSize, 4, result.pages[ i ].data(), result.atlasSize * 4 ) ) {
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
	os << "  \"style\": \"" << ( cbreStyle ? "cbre" : "3dws" ) << "\",\n";
	os << "  \"atlas_size\": " << result.atlasSize << ",\n";
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
		os << "    {\"face\": " << f.faceIndex << ", \"page\": " << f.page << ", \"x\": " << f.x << ", \"y\": " << f.y << ", \"w\": " << f.w << ", \"h\": " << f.h << ", \"min_u\": " << f.minU << ", \"min_v\": " << f.minV << ", \"step_u\": " << f.stepU << ", \"step_v\": " << f.stepV << "}";
		if ( i + 1 < result.faces.size() )
			os << ",";
		os << "\n";
	}
	os << "  ]\n}\n";
	return true;
}
