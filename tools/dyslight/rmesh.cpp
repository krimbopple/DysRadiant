#include "rmesh.h"

#include "texture.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <utility>

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

static uint32_t faceTriangleCount( const Face& face ) {
	return face.vertices.size() >= 3 ? (uint32_t)face.vertices.size() - 2 : 0;
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

static bool faceNeedsReversedWinding( const Face& face ) {
	if ( face.vertices.size() < 3 )
		return false;
	const Vec3 n = ( face.vertices[ 1 ] - face.vertices[ 0 ] ).cross( face.vertices[ 2 ] - face.vertices[ 0 ] );
	return n.dot( face.normal ) > 0.0f;
}

static void writeFaceIndices( std::ostream& os, const Face& face, uint32_t offset ) {
	const bool reverse = faceNeedsReversedWinding( face );
	for ( uint32_t i = 1; i + 1 < (uint32_t)face.vertices.size(); ++i ) {
		writeU32( os, offset + 0 );
		writeU32( os, offset + ( reverse ? i + 1 : i ) );
		writeU32( os, offset + ( reverse ? i : i + 1 ) );
	}
}

static void writeRmIndices( std::ostream& os, const std::vector<const Face*>& faces ) {
	uint32_t offset = 0;
	for ( const Face* face : faces ) {
		writeFaceIndices( os, *face, offset );
		offset += (uint32_t)face->vertices.size();
	}
}

// invisible collision and trigger boxes only carry positions
static void writeRmSimpleGeom( std::ostream& os, const std::vector<const Face*>& faces ) {
	uint32_t vertCount = 0;
	uint32_t triCount = 0;
	for ( const Face* face : faces ) {
		vertCount += (uint32_t)face->vertices.size();
		triCount += faceTriangleCount( *face );
	}

	writeU32( os, vertCount );
	for ( const Face* face : faces ) {
		for ( const Vec3& v : face->vertices ) {
			writeF32( os, v.x );
			writeF32( os, v.z );
			writeF32( os, v.y );
		}
	}

	writeU32( os, triCount );
	writeRmIndices( os, faces );
}

bool writeRmesh( const fs::path& base, const Map& map, const BakeResult& baked, std::string& error ) {
	const fs::path mapDir = base.parent_path();
	const fs::path out = mapDir / ( base.filename().string() + ".rmesh" );

	std::map<std::string, std::vector<const Face*>> transparentFaces;
	std::vector<std::string> transparentOrder;
	std::vector<const Face*> invisibleCollisionFaces;
	bool hasTriggerFaces = false;

	for ( const Solid& solid : map.solids ) {
		for ( const Face& face : solid.faces ) {
			if ( face.vertices.size() < 3 )
				continue;
			const std::string mat = lower( face.material );
			if ( mat == "tooltextures/invisible_collision" ) {
				invisibleCollisionFaces.push_back( &face );
				continue;
			}
			if ( mat.rfind( "tooltextures/", 0 ) == 0 ) {
				if ( mat == "tooltextures/triggerbox" )
					hasTriggerFaces = true;
				continue;
			}
			if ( textureHasAlpha( face.material, mapDir ) ) {
				if ( transparentFaces[ face.material ].empty() )
					transparentOrder.push_back( face.material );
				transparentFaces[ face.material ].push_back( &face );
			}
		}
	}

	std::vector<const Entity*> triggerBoxes;
	for ( const Entity& ent : map.entities )
		if ( lower( ent.classname ) == "triggerbox" && !ent.brushSolids.empty() )
			triggerBoxes.push_back( &ent );

	if ( triggerBoxes.empty() && hasTriggerFaces ) {
		error = "map has trigger box faces but no triggerbox entities, tie the brushes to an entity";
		return false;
	}

	std::vector<std::string> textureOrder;
	std::map<std::string, std::vector<const LmFace*>> lmFaces;
	for ( const LmFace& lf : baked.faces ) {
		if ( lf.solidIndex < 0 || (size_t)lf.solidIndex >= map.solids.size() )
			continue;
		const Solid& solid = map.solids[ lf.solidIndex ];
		if ( lf.faceInSolid < 0 || (size_t)lf.faceInSolid >= solid.faces.size() )
			continue;
		const Face& face = solid.faces[ lf.faceInSolid ];
		if ( face.vertices.size() < 3 )
			continue;
		if ( lower( face.material ).rfind( "tooltextures/", 0 ) == 0 )
			continue;
		if ( textureHasAlpha( face.material, mapDir ) )
			continue;
		auto& list = lmFaces[ face.material ];
		if ( list.empty() )
			textureOrder.push_back( face.material );
		list.push_back( &lf );
	}
	for ( const std::string& tex : transparentOrder )
		if ( lmFaces.find( tex ) == lmFaces.end() )
			textureOrder.push_back( tex );

	struct Batch
	{
		std::string texture;
		int page = 0;
		bool lightmapped = true;
		std::vector<std::pair<const Face*, const LmFace*>> faces; // LmFace is null for transparent batches
	};

	const int pageCount = std::max<int>( 1, (int)baked.pages.size() );
	std::vector<Batch> batches;
	for ( const std::string& tex : textureOrder ) {
		auto lmIt = lmFaces.find( tex );
		auto trIt = transparentFaces.find( tex );
		const std::vector<const LmFace*>* lmList = lmIt != lmFaces.end() ? &lmIt->second : nullptr;
		const std::vector<const Face*>* trList = trIt != transparentFaces.end() ? &trIt->second : nullptr;

		for ( int page = 0; page < pageCount; ++page ) {
			Batch batch;
			batch.texture = tex;
			batch.page = page;
			if ( lmList ) {
				for ( const LmFace* lf : *lmList ) {
					if ( lf->page != page )
						continue;
					const Face& face = map.solids[ lf->solidIndex ].faces[ lf->faceInSolid ];
					batch.faces.push_back( { &face, lf } );
				}
			}
			if ( !batch.faces.empty() ) {
				batch.lightmapped = true;
				batches.push_back( std::move( batch ) );
			} else if ( page == 0 && trList && !trList->empty() ) {
				batch.lightmapped = false;
				for ( const Face* face : *trList )
					batch.faces.push_back( { face, nullptr } );
				batches.push_back( std::move( batch ) );
			}
		}
	}

	std::ofstream os( out, std::ios::binary );
	if ( !os ) {
		error = "could not write " + out.string();
		return false;
	}

	writeB3DString( os, triggerBoxes.empty() ? "RoomMesh" : "RoomMesh.HasTriggerBox" );

	auto pageName = [ & ]( int page ) {
		std::string name = base.filename().string() + "_lm";
		if ( baked.pages.size() > 1 )
			name += "_" + std::to_string( page );
		return name + ".png";
	};

	writeU32( os, (uint32_t)batches.size() );

	for ( const Batch& batch : batches ) {
		const std::string texName = resolveTextureName( batch.texture, mapDir );
		int tw = 256, th = 256;
		textureSize( batch.texture, mapDir, tw, th );
		const float invW = 1.0f / (float)std::max( 1, tw );
		const float invH = 1.0f / (float)std::max( 1, th );

		uint32_t vertCount = 0;
		uint32_t triCount = 0;
		for ( const auto& [ face, lf ] : batch.faces ) {
			vertCount += (uint32_t)face->vertices.size();
			triCount += faceTriangleCount( *face );
		}
		if ( vertCount > 65535 ) {
			error = "RMesh vertex overflow in " + batch.texture;
			return false;
		}

		if ( batch.lightmapped ) {
			writeU8( os, 2 ); // TextureType.lightmap
			writeB3DString( os, pageName( batch.page ) );
			writeU8( os, 1 ); // TextureType.opaque
			writeB3DString( os, texName );
			writeU32( os, vertCount );

			for ( const auto& [ face, lf ] : batch.faces ) {
				const Vec3 ref = face->vertices[ 0 ];
				const float us = std::max( 0.0001f, std::abs( face->uScale ) );
				const float vs = std::max( 0.0001f, std::abs( face->vScale ) );
				for ( const Vec3& v : face->vertices ) {
					const float diffU = v.dot( face->uaxis ) / ( (float)tw * us ) + face->uShift * invW;
					const float diffV = v.dot( face->vaxis ) / ( (float)th * vs ) + face->vShift * invH;
					const float localU = ( v - ref ).dot( face->uaxis );
					const float localV = ( v - ref ).dot( face->vaxis );
					const float lmU = ( (float)lf->x + ( localU - lf->minU ) / std::max( 0.0001f, lf->stepU ) ) /
					                  (float)std::max( 1, baked.atlasSize );
					const float lmV = ( (float)lf->y + ( localV - lf->minV ) / std::max( 0.0001f, lf->stepV ) ) /
					                  (float)std::max( 1, baked.atlasSize );
					writeRmVertex( os, v, diffU, diffV, lmU, lmV );
				}
			}
		} else {
			writeU8( os, 0 );
			writeU8( os, 3 ); // COLOR / ALPHA
			writeB3DString( os, texName );
			writeU32( os, vertCount );

			for ( const auto& [ face, lf ] : batch.faces ) {
				const float us = std::max( 0.0001f, std::abs( face->uScale ) );
				const float vs = std::max( 0.0001f, std::abs( face->vScale ) );
				for ( const Vec3& v : face->vertices ) {
					const float diffU = v.dot( face->uaxis ) / ( (float)tw * us ) + face->uShift * invW;
					const float diffV = v.dot( face->vaxis ) / ( (float)th * vs ) + face->vShift * invH;
					writeRmVertex( os, v, diffU, diffV, 0.0f, 0.0f );
				}
			}
		}

		writeU32( os, triCount );
		uint32_t offset = 0;
		for ( const auto& [ face, lf ] : batch.faces ) {
			writeFaceIndices( os, *face, offset );
			offset += (uint32_t)face->vertices.size();
		}
	}

	if ( !invisibleCollisionFaces.empty() ) {
		writeU32( os, 1 );
		writeRmSimpleGeom( os, invisibleCollisionFaces );
	} else {
		writeU32( os, 0 );
	}

	if ( !triggerBoxes.empty() ) {
		writeU32( os, (uint32_t)triggerBoxes.size() );
		for ( const Entity* ent : triggerBoxes ) {
			std::vector<const Face*> faces;
			for ( const Solid& solid : ent->brushSolids )
				for ( const Face& face : solid.faces )
					if ( face.vertices.size() >= 3 )
						faces.push_back( &face );
			writeU32( os, 1 );
			writeRmSimpleGeom( os, faces );
			auto it = ent->keys.find( "name" );
			writeB3DString( os, it != ent->keys.end() ? it->second : "triggerbox" );
		}
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
		const std::string cls = lower( ent.classname );
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
		const int r = std::clamp( (int)( light.color.x * 255.0f + 0.5f ), 0, 255 );
		const int g = std::clamp( (int)( light.color.y * 255.0f + 0.5f ), 0, 255 );
		const int b = std::clamp( (int)( light.color.z * 255.0f + 0.5f ), 0, 255 );
		const std::string color = std::to_string( r ) + " " + std::to_string( g ) + " " + std::to_string( b );
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
		const std::string file = resolveModelName( fi != ent->keys.end() ? fi->second : "unknown", mapDir );
		writeB3DString( os, file );
		writeF32( os, ent->origin.x );
		writeF32( os, ent->origin.z );
		writeF32( os, ent->origin.y );
		const Vec3 angles = parseVec3( ent->keys.count( "angles" ) ? ent->keys.at( "angles" ) : "0 0 0" );
		const Vec3 scale = parseVec3( ent->keys.count( "scale" ) ? ent->keys.at( "scale" ) : "1 1 1", { 1, 1, 1 } );
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
		writeB3DString( os, ii != ent->keys.end() ? fs::path( ii->second ).filename().string() : "" );
	}

	// the game validates this trailer for plain RoomMesh files
	writeB3DString( os, "EOF" );

	return true;
}
