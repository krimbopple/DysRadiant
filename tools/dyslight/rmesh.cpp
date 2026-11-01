#include "rmesh.h"

#include "texture.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>

static void writeU8( std::ostream& os, uint8_t v ) {
	os.write( (const char*)&v, 1 );
}

static void writeU32( std::ostream& os, uint32_t v ) {
	os.write( (const char*)&v, 4 );
}

static void writeF32( std::ostream& os, float v ) {
	os.write( (const char*)&v, 4 );
}

static void writeB3DString( std::ostream& os, const std::string& s ) {
	writeU32( os, (uint32_t)s.size() );
	os.write( s.data(), (std::streamsize)s.size() );
}

static void writeRmVertex( std::ostream& os, const Vec3& p, float diffU, float diffV, float lmU, float lmV ) {
	writeF32( os, p.x );
	writeF32( os, p.z );
	writeF32( os, p.y );
	writeF32( os, diffU );
	writeF32( os, diffV );
	writeF32( os, lmU );
	writeF32( os, lmV );
	writeU8( os, 255 );
	writeU8( os, 255 );
	writeU8( os, 255 );
}

static void writeRmSimpleGeom( std::ostream& os, const std::vector<const Face*>& faces ) {
	uint32_t vertCount = 0;
	uint32_t triCount = 0;
	for ( const Face* f : faces ) {
		vertCount += (uint32_t)f->vertices.size();
		triCount += (uint32_t)( f->vertices.size() >= 3 ? f->vertices.size() - 2 : 0 );
	}
	writeU32( os, vertCount );
	for ( const Face* f : faces ) {
		for ( const Vec3& v : f->vertices ) {
			writeF32( os, v.x );
			writeF32( os, v.z );
			writeF32( os, v.y );
		}
	}
	writeU32( os, triCount );
	uint32_t offset = 0;
	for ( const Face* f : faces ) {
		for ( uint32_t i = 1; i + 1 < (uint32_t)f->vertices.size(); ++i ) {
			writeU32( os, offset + 0 );
			writeU32( os, offset + i );
			writeU32( os, offset + i + 1 );
		}
		offset += (uint32_t)f->vertices.size();
	}
}

bool writeRmesh( const fs::path& base, const Map& map, const BakeResult& baked, std::string& error ) {
	fs::path out = base.parent_path() / ( base.filename().string() + ".rmesh" );
	std::ofstream os( out, std::ios::binary );
	if ( !os ) {
		error = "could not write " + out.string();
		return false;
	}

	struct Group
	{
		std::string texture;
		int page = 0;
		bool transparent = false;
		std::vector<const LmFace*> lmfaces;
	};

	std::map<std::string, Group> opaque;
	std::map<std::string, Group> transparent;
	std::vector<const Face*> invisible;

	for ( const Solid& solid : map.solids ) {
		for ( const Face& face : solid.faces ) {
			if ( face.vertices.size() < 3 )
				continue;
			std::string mat = lower( face.material );
			if ( mat == "tooltextures/invisible_collision" ) {
				invisible.push_back( &face );
				continue;
			}
			if ( mat.rfind( "tooltextures/", 0 ) == 0 )
				continue;
		}
	}

	for ( const LmFace& lf : baked.faces ) {
		if ( lf.solidIndex < 0 || (size_t)lf.solidIndex >= map.solids.size() )
			continue;
		const Solid& solid = map.solids[ lf.solidIndex ];
		if ( lf.faceInSolid < 0 || (size_t)lf.faceInSolid >= solid.faces.size() )
			continue;
		const Face& face = solid.faces[ lf.faceInSolid ];
		if ( face.vertices.size() < 3 )
			continue;
		std::string mat = lower( face.material );
		if ( mat.rfind( "tooltextures/", 0 ) == 0 )
			continue;
		if ( isTransparentMaterial( face.material ) ) {
			Group& g = transparent[ face.material ];
			g.texture = face.material;
			g.transparent = true;
			g.lmfaces.push_back( &lf );
		} else {
			std::string key = face.material + "\n" + std::to_string( lf.page );
			Group& g = opaque[ key ];
			g.texture = face.material;
			g.page = lf.page;
			g.lmfaces.push_back( &lf );
		}
	}

	std::vector<Group*> groups;
	for ( auto& kv : opaque )
		groups.push_back( &kv.second );
	for ( auto& kv : transparent )
		groups.push_back( &kv.second );

	writeB3DString( os, "RM" );
	writeU8( os, 1 );
	writeU32( os, (uint32_t)groups.size() );

	auto pageName = [ & ]( int page ) {
		std::string name = base.filename().string() + "_lm";
		if ( baked.pages.size() > 1 )
			name += "_" + std::to_string( page );
		return name + ".png";
	};

	for ( const Group* g : groups ) {
		std::string texName = resolveTextureName( g->texture, base.parent_path() );

		if ( g->transparent ) {
			writeU8( os, 0 );
			writeU8( os, 3 );
			writeB3DString( os, texName );
		} else {
			writeU8( os, 1 );
			writeB3DString( os, pageName( g->page ) );
			writeU8( os, 1 );
			writeB3DString( os, texName );
		}

		uint32_t vertCount = 0;
		uint32_t triCount = 0;
		for ( const LmFace* lf : g->lmfaces ) {
			const Face& face = map.solids[ lf->solidIndex ].faces[ lf->faceInSolid ];
			vertCount += (uint32_t)face.vertices.size();
			triCount += (uint32_t)( face.vertices.size() - 2 );
		}

		if ( vertCount > 65535 ) {
			error = "RMesh vertex overflow in " + g->texture;
			return false;
		}

		writeU32( os, vertCount );

		for ( const LmFace* lf : g->lmfaces ) {
			const Face& face = map.solids[ lf->solidIndex ].faces[ lf->faceInSolid ];
			int tw = 256, th = 256;
			textureSize( g->texture, base.parent_path(), tw, th );
			float us = std::max( 0.0001f, std::abs( face.uScale ) );
			float vs = std::max( 0.0001f, std::abs( face.vScale ) );
			Vec3 ref = face.vertices[ 0 ];
			for ( const Vec3& v : face.vertices ) {
				float diffU = v.dot( face.uaxis ) / ( (float)tw * us ) + face.uShift / (float)tw;
				float diffV = v.dot( face.vaxis ) / ( (float)th * vs ) + face.vShift / (float)th;
				float lmU = 0.0f;
				float lmV = 0.0f;
				if ( !g->transparent ) {
					float localU = ( v - ref ).dot( face.uaxis );
					float localV = ( v - ref ).dot( face.vaxis );
					lmU = ( (float)lf->x + ( localU - lf->minU ) / std::max( 0.0001f, lf->stepU ) ) /
						  (float)baked.atlasSize;
					lmV = ( (float)lf->y + ( localV - lf->minV ) / std::max( 0.0001f, lf->stepV ) ) /
						  (float)baked.atlasSize;
				}
				writeRmVertex( os, v, diffU, diffV, lmU, lmV );
			}
		}
		writeU32( os, triCount );
		uint32_t offset = 0;
		for ( const LmFace* lf : g->lmfaces ) {
			const Face& face = map.solids[ lf->solidIndex ].faces[ lf->faceInSolid ];
			for ( uint32_t i = 1; i + 1 < (uint32_t)face.vertices.size(); ++i ) {
				writeU32( os, offset + 0 );
				writeU32( os, offset + i );
				writeU32( os, offset + i + 1 );
			}
			offset += (uint32_t)face.vertices.size();
		}
	}

	writeU32( os, 0 );

	if ( !invisible.empty() ) {
		writeU32( os, 1 );
		writeRmSimpleGeom( os, invisible );
	} else {
		writeU32( os, 0 );
	}

	std::vector<const Entity*> triggers;
	for ( const Entity& ent : map.entities )
		if ( lower( ent.classname ) == "triggerbox" && !ent.brushSolids.empty() )
			triggers.push_back( &ent );

	writeU32( os, (uint32_t)triggers.size() );
	for ( const Entity* ent : triggers ) {
		writeU32( os, 1 );
		std::vector<const Face*> faces;
		for ( const Solid& solid : ent->brushSolids )
			for ( const Face& face : solid.faces )
				faces.push_back( &face );
		writeRmSimpleGeom( os, faces );
		auto it = ent->keys.find( "name" );
		writeB3DString( os, it != ent->keys.end() ? it->second : "triggerbox" );
	}

	std::vector<Light> lights;

	for ( const Light& light : collectLights( map ) )
		if ( light.exportSprite )
			lights.push_back( light );
	std::vector<const Entity*> waypoints;
	std::vector<const Entity*> soundEmitters;
	std::vector<const Entity*> props;
	std::vector<const Entity*> screens;
	for ( const Entity& ent : map.entities ) {
		std::string cls = lower( ent.classname );
		if ( cls == "waypoint" )
			waypoints.push_back( &ent );
		else if ( cls == "soundemitter" )
			soundEmitters.push_back( &ent );
		else if ( cls == "model" )
			props.push_back( &ent );
		else if ( cls == "screen" )
			screens.push_back( &ent );
	}

	writeU32( os, (uint32_t)( lights.size() + waypoints.size() + soundEmitters.size() + props.size() + screens.size() ) );

	for ( const Light& light : lights ) {
		writeB3DString( os, "light" );
		writeF32( os, light.origin.x );
		writeF32( os, light.origin.z );
		writeF32( os, light.origin.y );
		writeF32( os, light.range );
		int r = std::clamp( (int)( light.color.x * 255.0f + 0.5f ), 0, 255 );
		int g = std::clamp( (int)( light.color.y * 255.0f + 0.5f ), 0, 255 );
		int b = std::clamp( (int)( light.color.z * 255.0f + 0.5f ), 0, 255 );
		std::string color = std::to_string( r ) + " " + std::to_string( g ) + " " + std::to_string( b );
		writeU32( os, (uint32_t)color.size() );
		os.write( color.data(), (std::streamsize)color.size() );
		writeF32( os, light.intensity );
	}

	for ( const Entity* ent : waypoints ) {
		writeB3DString( os, "waypoint" );
		writeF32( os, ent->origin.x );
		writeF32( os, ent->origin.z );
		writeF32( os, ent->origin.y );
	}

	for ( const Entity* ent : soundEmitters ) {
		writeB3DString( os, "soundemitter" );
		writeF32( os, ent->origin.x );
		writeF32( os, ent->origin.z );
		writeF32( os, ent->origin.y );
		auto si = ent->keys.find( "sound" );
		auto ri = ent->keys.find( "range" );
		writeU32( os, (uint32_t)( si != ent->keys.end() ? std::atoi( si->second.c_str() ) : 0 ) );
		writeF32( os, (float)( ri != ent->keys.end() ? std::atof( ri->second.c_str() ) : 1000.0f ) );
	}

	for ( const Entity* ent : props ) {
		writeB3DString( os, "model" );
		auto fi = ent->keys.find( "file" );
		std::string file = fi != ent->keys.end() ? fi->second : "unknown";
		file = resolveModelName( file, base.parent_path() );
		writeB3DString( os, file );
		writeF32( os, ent->origin.x );
		writeF32( os, ent->origin.z );
		writeF32( os, ent->origin.y );
		Vec3 angles = parseVec3( ent->keys.count( "angles" ) ? ent->keys.at( "angles" ) : "0 0 0" );
		Vec3 scale = parseVec3( ent->keys.count( "scale" ) ? ent->keys.at( "scale" ) : "1 1 1", { 1, 1, 1 } );
		writeF32( os, angles.x );
		writeF32( os, angles.y );
		writeF32( os, angles.z );
		writeF32( os, scale.x );
		writeF32( os, scale.y );
		writeF32( os, scale.z );
	}

	for ( const Entity* ent : screens ) {
		writeB3DString( os, "screen" );
		writeF32( os, ent->origin.x );
		writeF32( os, ent->origin.z );
		writeF32( os, ent->origin.y );
		auto ii = ent->keys.find( "imgpath" );
		writeB3DString( os, ii != ent->keys.end() ? ii->second : "" );
	}

	return true;
}
