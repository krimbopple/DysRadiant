#include "map3dw.h"

#include <cstring>
#include <fstream>
#include <stdexcept>

static bool readFile( const fs::path& path, std::vector<uint8_t>& out ) {
	std::ifstream f( path, std::ios::binary );
	if ( !f )
		return false;
	f.seekg( 0, std::ios::end );
	std::streamoff size = f.tellg();
	if ( size < 0 )
		return false;
	f.seekg( 0, std::ios::beg );
	out.resize( (size_t)size );
	if ( size > 0 )
		f.read( (char*)out.data(), size );
	return true;
}

struct Reader
{
	const std::vector<uint8_t>& data;
	size_t pos = 0;

	Reader( const std::vector<uint8_t>& d ) : data( d ) {}

	bool need( size_t n ) const {
		return pos + n <= data.size();
	}

	bool seek( size_t p ) {
		if ( p > data.size() )
			return false;
		pos = p;
		return true;
	}

	uint8_t u8() {
		if ( !need( 1 ) )
			throw std::runtime_error( "unexpected end of 3dw" );
		return data[ pos++ ];
	}

	uint16_t u16() {
		if ( !need( 2 ) )
			throw std::runtime_error( "unexpected end of 3dw" );
		uint16_t v = (uint16_t)data[ pos ] | ( (uint16_t)data[ pos + 1 ] << 8 );
		pos += 2;
		return v;
	}

	uint32_t u32() {
		if ( !need( 4 ) )
			throw std::runtime_error( "unexpected end of 3dw" );
		uint32_t v = (uint32_t)data[ pos ] | ( (uint32_t)data[ pos + 1 ] << 8 ) | ( (uint32_t)data[ pos + 2 ] << 16 ) | ( (uint32_t)data[ pos + 3 ] << 24 );
		pos += 4;
		return v;
	}

	int32_t i32() {
		return (int32_t)u32();
	}

	float f32() {
		uint32_t v = u32();
		float f;
		std::memcpy( &f, &v, sizeof( f ) );
		return f;
	}

	std::string zstr() {
		size_t start = pos;
		while ( pos < data.size() && data[ pos ] != 0 )
			++pos;
		std::string s( (const char*)data.data() + start, pos - start );
		if ( pos < data.size() )
			++pos;
		return s;
	}
};

struct Object3
{
	std::string type;
	size_t dataOffset = 0;
	size_t size = 0;
};

bool parse3dw( const fs::path& path, Map& map, std::string& error ) {
	std::vector<uint8_t> data;
	if ( !readFile( path, data ) ) {
		error = "could not read .3dw";
		return false;
	}

	try {
		Reader r( data );
		// header has version, flags, then name and object table offsets
		uint16_t version = r.u16();
		if ( version < 9 || version > 14 ) {
			error = "unsupported .3dw version " + std::to_string( version );
			return false;
		}
		// bit 0 means the payload is compressed
		uint8_t flags = version >= 12 ? r.u8() : (uint8_t)( r.u16() & 0xFF );
		if ( flags & 1 ) {
			error = "compressed .3dw files are not supported yet";
			return false;
		}

		uint32_t nameCount = r.u32();
		uint32_t nameOffset = r.u32();
		uint32_t objectCount = r.u32();
		uint32_t objectOffset = r.u32();

		// names are null-terminated and shared by all objects
		std::vector<std::string> names;
		r.seek( nameOffset );
		for ( uint32_t i = 0; i < nameCount; ++i )
			names.push_back( r.zstr() );

		auto nameAt = [ & ]( int32_t index ) -> std::string {
			if ( index > 0 && (size_t)index <= names.size() )
				return names[ (size_t)index - 1 ];
			return {};
		};

		// object type is a 1-based name index, the size lets us skip unknowns!
		std::vector<Object3> objects;
		r.seek( objectOffset );
		for ( uint32_t i = 0; i < objectCount; ++i ) {
			int32_t typeIndex = r.i32();
			uint32_t size = r.u32();
			objects.push_back( { nameAt( typeIndex ), r.pos, size } );
			r.pos += size;
		}

		// read materials and visgroups first so later shit can point at them
		std::vector<std::string> materialByObject( objects.size() );
		std::vector<std::string> meshReferenceByObject( objects.size() );
		std::vector<int> visgroupIdByObject( objects.size(), 0 );
		int nextVisgroup = 1;

		for ( size_t i = 0; i < objects.size(); ++i ) {
			const Object3& obj = objects[ i ];
			if ( obj.type == "material" ) {
				r.seek( obj.dataOffset );
				uint8_t matFlags = r.u8();
				int32_t cat = r.i32();
				int32_t name = r.i32();
				(void)cat;
				materialByObject[ i ] = nameAt( name );
				if ( matFlags & 2 )
					r.i32();
			} else if ( obj.type == "visgroup" ) {
				r.seek( obj.dataOffset );
				r.u8();
				int32_t nameIndex = r.i32();
				uint8_t cr = r.u8();
				uint8_t cg = r.u8();
				uint8_t cb = r.u8();
				Visgroup vg;
				vg.id = nextVisgroup++;
				vg.name = nameAt( nameIndex );
				vg.color[ 0 ] = cr;
				vg.color[ 1 ] = cg;
				vg.color[ 2 ] = cb;
				map.visgroups.push_back( vg );
				visgroupIdByObject[ i ] = vg.id;
			} else if ( obj.type == "meshreference" ) {
				r.seek( obj.dataOffset );
				r.u8();
				r.i32();
				int32_t objectName = r.i32();
				r.u8();
				meshReferenceByObject[ i ] = nameAt( objectName );
			}
		}

		auto resolveVisgroup = [ & ]( int32_t ref ) -> int {
			if ( ref > 0 && (size_t)ref <= visgroupIdByObject.size() )
				return visgroupIdByObject[ (size_t)ref - 1 ];
			return 0;
		};
		auto resolveMaterial = [ & ]( int32_t ref ) -> std::string {
			if ( ref > 0 && (size_t)ref <= materialByObject.size() && !materialByObject[ (size_t)ref - 1 ].empty() )
				return materialByObject[ (size_t)ref - 1 ];
			return "white";
		};

		int nextSolid = 1;
		int nextEntity = 1;

		for ( size_t i = 0; i < objects.size(); ++i ) {
			const Object3& obj = objects[ i ];
			if ( obj.type == "brush" ) {
				r.seek( obj.dataOffset );

				// brush header has flags, key/value count, group and visgroup refs
				uint8_t brushFlags = r.u8();
				uint32_t propCount = r.u32();
				bool invisibleCollision = false;
				for ( uint32_t p = 0; p < propCount; ++p ) {
					int32_t key = r.i32();
					int32_t value = r.i32();
					if ( lower( nameAt( key ) ) == "classname" && lower( nameAt( value ) ) == "field_hit" )
						invisibleCollision = true;
				}
				int32_t groupRef = r.i32();
				int32_t visgroupRef = r.i32();
				(void)groupRef;

				Solid solid;
				solid.id = nextSolid++;
				solid.visgroupId = resolveVisgroup( visgroupRef );
				solid.color[ 0 ] = r.u8();
				solid.color[ 1 ] = r.u8();
				solid.color[ 2 ] = r.u8();
				uint8_t vertexCount = r.u8();
				std::vector<Vec3> vertices;
				vertices.reserve( vertexCount );

				// 3DWS is Y-up, blehhhhh
				for ( uint8_t v = 0; v < vertexCount; ++v ) {
					float x = r.f32();
					float y = r.f32();
					float z = r.f32();
					vertices.push_back( { x, z, y } );
				}

				uint8_t faceCount = r.u8();
				for ( uint8_t f = 0; f < faceCount; ++f ) {
					Face face;
					face.id = f + 1;
					uint8_t faceFlags = r.u8();

					// face data has plane, texture origin/scale/rotation, UV basis and luxel size
					float n0 = r.f32();
					float n1 = r.f32();
					float n2 = r.f32();
					float n3 = r.f32();
					float texPosX = r.f32();
					float texPosY = r.f32();
					float texScaleX = r.f32();
					float texScaleY = r.f32();
					float texRotX = r.f32();
					float texRotY = r.f32();
					(void)texRotY;
					float u0 = r.f32();
					float u1 = r.f32();
					float u2 = r.f32();
					float u3 = r.f32();
					float v0 = r.f32();
					float v1 = r.f32();
					float v2 = r.f32();
					float v3 = r.f32();
					(void)u3;
					(void)v3;
					float luxelSize = r.f32();
					int32_t smoothGroup = r.i32();
					int32_t materialRef = r.i32();

					// flag 0x10 adds a lightmap object and lightmap UVs
					if ( faceFlags & 16 )
						r.i32();
					uint8_t indexCount = r.u8();
					std::vector<uint8_t> indices;
					indices.reserve( indexCount );

					// vertices are index & diffuse UV, plus lightmap UV when present
					for ( uint8_t vi = 0; vi < indexCount; ++vi ) {
						uint8_t index = r.u8();
						indices.push_back( index );
						r.f32();
						r.f32();
						if ( faceFlags & 16 ) {
							r.f32();
							r.f32();
						}
					}

					face.normal = Vec3{ n0, n2, n1 }.normalized();
					face.dist = n3;
					face.uaxis = Vec3{ u0, u2, u1 }.normalized();
					face.vaxis = Vec3{ v0, v2, v1 }.normalized();
					face.uShift = texScaleX != 0.0f ? ( -texPosX * 2.0f / texScaleX ) : 0.0f;
					face.vShift = texScaleY != 0.0f ? ( texPosY * 2.0f / texScaleY ) : 0.0f;
					face.uScale = texScaleX != 0.0f ? ( texScaleX / 2.0f ) : 0.5f;
					face.vScale = texScaleY != 0.0f ? ( texScaleY / 2.0f ) : 0.5f;
					face.lmScale = luxelSize > 0.0f ? luxelSize : 16.0f;
					face.smoothing = smoothGroup;
					if ( faceFlags & 4 )
						face.material = "tooltextures/remove_face";
					else if ( invisibleCollision )
						face.material = "tooltextures/invisible_collision";
					else
						face.material = resolveMaterial( materialRef );

					for ( uint8_t index : indices ) {
						if ( index < vertices.size() )
							face.vertices.push_back( vertices[ index ] );
					}
					if ( face.vertices.size() >= 3 )
						solid.faces.push_back( face );
				}
				if ( !solid.faces.empty() ) {
					orientSolidFaces( solid );
					map.solids.push_back( solid );
				}
				(void)brushFlags;
			} else if ( obj.type == "entity" ) {
				r.seek( obj.dataOffset );

				// entity has position, key/value pairs, group and visgroup refs
				r.u8();
				float x = r.f32();
				float y = r.f32();
				float z = r.f32();
				uint32_t keyCount = r.u32();
				Entity ent;
				ent.id = nextEntity++;
				ent.origin = { x, z, y };
				for ( uint32_t k = 0; k < keyCount; ++k ) {
					int32_t key = r.i32();
					int32_t value = r.i32();
					std::string keyName = nameAt( key );
					std::string keyValue = nameAt( value );
					if ( lower( keyName ) == "classname" )
						ent.classname = keyValue;
					else
						ent.keys[ keyName ] = keyValue;
				}
				r.i32();
				int32_t visgroupRef = r.i32();
				ent.visgroupId = resolveVisgroup( visgroupRef );
				if ( ent.classname.empty() )
					ent.classname = "info_null";
				map.entities.push_back( ent );
			} else if ( obj.type == "mesh" ) {
				r.seek( obj.dataOffset );

				// mesh entities have their own transform and (possibly) scale
				uint8_t meshFlags = r.u8();
				uint32_t keyCount = r.u32();
				Entity ent;
				ent.id = nextEntity++;
				ent.classname = "model";
				for ( uint32_t k = 0; k < keyCount; ++k ) {
					int32_t key = r.i32();
					int32_t value = r.i32();
					std::string keyName = nameAt( key );
					std::string keyValue = nameAt( value );
					if ( lower( keyName ) != "classname" )
						ent.keys[ keyName ] = keyValue;
				}
				r.i32();
				int32_t visgroupRef = r.i32();
				ent.visgroupId = resolveVisgroup( visgroupRef );
				r.u8();
				r.u8();
				r.u8();
				int32_t meshRef = r.i32();
				float x = r.f32();
				float y = r.f32();
				float z = r.f32();
				ent.origin = { x, z, y };
				if ( meshRef >= 0 && (size_t)meshRef < meshReferenceByObject.size() && !meshReferenceByObject[ (size_t)meshRef ].empty() )
					ent.keys[ "file" ] = meshReferenceByObject[ (size_t)meshRef ];
				float pitch = r.f32();
				float yaw = r.f32();
				float roll = r.f32();
				ent.keys[ "angles" ] = std::to_string( pitch ) + " " + std::to_string( yaw ) + " " + std::to_string( roll );
				if ( !( meshFlags & 1 ) ) {
					float sx = r.f32();
					float sy = r.f32();
					float sz = r.f32();
					ent.keys[ "scale" ] = std::to_string( sx ) + " " + std::to_string( sy ) + " " + std::to_string( sz );
				}
				if ( !ent.keys.count( "file" ) )
					ent.keys[ "file" ] = "unknown";
				map.entities.push_back( ent );
			}
		}
	} catch ( const std::exception& e ) {
		error = e.what();
		return false;
	}

	map.name = path.stem().string();
	return true;
}
