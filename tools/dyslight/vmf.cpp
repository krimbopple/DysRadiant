#include "vmf.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>

struct VNode
{
	std::string name;
	std::vector<std::pair<std::string, std::string>> props;
	std::vector<VNode> children;

	const std::string* get( const std::string& key ) const {
		for ( const auto& p : props )
			if ( p.first == key )
				return &p.second;
		return nullptr;
	}
};

struct VToken
{
	std::string text;
	bool quoted = false;
};

struct VParser
{
	std::string src;
	size_t pos = 0;

	explicit VParser( std::string s ) : src( std::move( s ) ) {}

	void skip() {
		while ( pos < src.size() ) {
			unsigned char c = (unsigned char)src[ pos ];
			if ( std::isspace( c ) ) {
				++pos;
			} else if ( c == '/' && pos + 1 < src.size() && src[ pos + 1 ] == '/' ) {
				while ( pos < src.size() && src[ pos ] != '\n' )
					++pos;
			} else
				break;
		}
	}

	VToken next() {
		skip();
		if ( pos >= src.size() )
			return {};
		if ( src[ pos ] == '{' || src[ pos ] == '}' )
			return { std::string( 1, src[ pos++ ] ), false };
		if ( src[ pos ] == '"' ) {
			++pos;
			std::string out;
			while ( pos < src.size() ) {
				char c = src[ pos++ ];
				if ( c == '"' )
					break;
				if ( c == '\\' && pos < src.size() ) {
					char n = src[ pos++ ];
					switch ( n ) {
					case 'n':
						out.push_back( '\n' );
						break;
					case 't':
						out.push_back( '\t' );
						break;
					default:
						out.push_back( n );
						break;
					}
				} else
					out.push_back( c );
			}
			return { out, true };
		}
		size_t start = pos;
		while ( pos < src.size() ) {
			char c = src[ pos ];
			if ( std::isspace( (unsigned char)c ) || c == '{' || c == '}' || c == '"' )
				break;
			++pos;
		}
		return { src.substr( start, pos - start ), false };
	}

	VToken peek() {
		size_t old = pos;
		VToken t = next();
		pos = old;
		return t;
	}

	std::optional<VNode> parseNode( const std::string& name ) {
		// a node is either a key/value pair or a named child block
		VNode node;
		node.name = name;
		VToken open = next();
		if ( open.text != "{" )
			return std::nullopt;
		while ( true ) {
			VToken key = next();
			if ( key.text.empty() )
				return std::nullopt;
			if ( key.text == "}" )
				break;
			VToken value = peek();
			if ( value.text == "{" ) {
				auto child = parseNode( key.text );
				if ( !child )
					return std::nullopt;
				node.children.push_back( std::move( *child ) );
			} else {
				value = next();
				node.props.push_back( { key.text, value.text } );
			}
		}
		return node;
	}

	std::vector<VNode> parseRoot() {
		std::vector<VNode> out;
		while ( true ) {
			VToken name = next();
			if ( name.text.empty() )
				break;
			if ( name.quoted || name.text == "{" || name.text == "}" )
				continue;
			auto node = parseNode( name.text );
			if ( node )
				out.push_back( std::move( *node ) );
		}
		return out;
	}
};

// VMF numbers sit inside punctuation, so skip it and let strtof parse the number
static bool parseFloats( const std::string& s, std::vector<float>& out, size_t maxCount = 0 ) {
	out.clear();
	const char* p = s.c_str();
	while ( *p ) {
		while ( *p && !std::isdigit( (unsigned char)*p ) && *p != '-' && *p != '+' && *p != '.' )
			++p;
		if ( !*p )
			break;
		char* end = nullptr;
		float v = std::strtof( p, &end );
		if ( end == p )
			break;
		out.push_back( v );
		if ( maxCount && out.size() >= maxCount )
			break;
		p = end;
	}
	return !out.empty();
}

static bool parsePlane( const std::string& s, std::vector<Vec3>& out ) {
	std::vector<float> f;
	if ( !parseFloats( s, f ) )
		return false;
	out.clear();
	for ( size_t i = 0; i + 2 < f.size() && out.size() < 3; i += 3 )
		out.push_back( { f[ i ], f[ i + 1 ], f[ i + 2 ] } );
	return out.size() == 3;
}

static bool parseAxis( const std::string& s, Vec3& axis, float& shift, float& scale ) {
	std::vector<float> f;
	if ( !parseFloats( s, f ) )
		return false;
	if ( f.size() >= 4 ) {
		axis = Vec3{ f[ 0 ], f[ 1 ], f[ 2 ] }.normalized();
		shift = f[ 3 ];
		scale = f.size() >= 5 ? f[ 4 ] : 1.0f;
		return true;
	}
	return false;
}

// use the side vertex list when present, plane is the fallback
static void parseSolidNode( const VNode& node, Map& map, int& nextSolid, int& nextFace ) {
	Solid solid;
	solid.id = nextSolid++;
	if ( const std::string* id = node.get( "id" ) )
		solid.id = std::atoi( id->c_str() );
	if ( const std::string* ed = node.get( "editor" ) )
		(void)ed;
	for ( const VNode& side : node.children ) {
		if ( side.name != "side" )
			continue;
		Face face;
		face.id = nextFace++;
		if ( const std::string* id = side.get( "id" ) )
			face.id = std::atoi( id->c_str() );
		if ( const std::string* mat = side.get( "material" ) )
			face.material = *mat;
		if ( const std::string* u = side.get( "uaxis" ) )
			parseAxis( *u, face.uaxis, face.uShift, face.uScale );
		if ( const std::string* v = side.get( "vaxis" ) )
			parseAxis( *v, face.vaxis, face.vShift, face.vScale );
		if ( const std::string* lm = side.get( "lightmapscale" ) )
			face.lmScale = (float)std::atof( lm->c_str() );
		if ( face.lmScale <= 0.0f )
			face.lmScale = 16.0f;
		if ( const std::string* sg = side.get( "smoothing_groups" ) )
			face.smoothing = std::atoi( sg->c_str() );

		if ( const std::string* plane = side.get( "plane" ) ) {
			std::vector<Vec3> pts;
			if ( parsePlane( *plane, pts ) ) {
				face.normal = ( pts[ 1 ] - pts[ 0 ] ).cross( pts[ 2 ] - pts[ 0 ] ).normalized();
				face.dist = face.normal.dot( pts[ 0 ] );
			}
		}
		for ( const VNode& child : side.children ) {
			if ( child.name == "vertex" ) {
				int count = 0;
				if ( const std::string* c = child.get( "count" ) )
					count = std::atoi( c->c_str() );
				for ( int i = 0; i < count; ++i ) {
					std::string key = "vertex" + std::to_string( i );
					if ( const std::string* v = child.get( key ) ) {
						Vec3 p = parseVec3( *v );
						face.vertices.push_back( p );
					}
				}
			}
		}
		if ( face.vertices.size() < 3 ) {
			if ( const std::string* plane = side.get( "plane" ) ) {
				std::vector<Vec3> pts;
				if ( parsePlane( *plane, pts ) ) {
					face.vertices = pts;
					if ( face.normal.length() < 0.001f ) {
						face.normal = ( pts[ 1 ] - pts[ 0 ] ).cross( pts[ 2 ] - pts[ 0 ] ).normalized();
						face.dist = face.normal.dot( pts[ 0 ] );
					}
				}
			}
		}
		if ( face.vertices.size() >= 3 )
			solid.faces.push_back( face );
	}
	if ( !solid.faces.empty() ) {
		orientSolidFaces( solid );
		map.solids.push_back( std::move( solid ) );
	}
}

bool parseVmf( const fs::path& path, Map& map, std::string& error ) {
	std::ifstream f( path, std::ios::binary );
	if ( !f ) {
		error = "could not read .vmf";
		return false;
	}
	std::ostringstream ss;
	ss << f.rdbuf();
	VParser parser( ss.str() );
	std::vector<VNode> root = parser.parseRoot();
	if ( root.empty() ) {
		error = "could not parse .vmf";
		return false;
	}

	int nextSolid = 1;
	int nextFace = 1;
	int nextEntity = 1;

	for ( const VNode& node : root ) {
		if ( node.name == "visgroups" || node.name == "visgroup" ) {
		} else if ( node.name == "world" ) {
			for ( const VNode& child : node.children ) {
				if ( child.name == "solid" )
					parseSolidNode( child, map, nextSolid, nextFace );
			}
		} else if ( node.name == "entity" ) {
			Entity ent;
			ent.id = nextEntity++;
			if ( const std::string* id = node.get( "id" ) )
				ent.id = std::atoi( id->c_str() );
			if ( const std::string* cn = node.get( "classname" ) )
				ent.classname = *cn;
			if ( const std::string* o = node.get( "origin" ) )
				ent.origin = parseVec3( *o );
			else if ( const std::string* p = node.get( "position" ) )
				ent.origin = parseVec3( *p );
			for ( const auto& p : node.props ) {
				if ( p.first != "id" && p.first != "classname" && p.first != "origin" && p.first != "editor" )
					ent.keys[ p.first ] = p.second;
			}
			for ( const VNode& child : node.children ) {
				if ( child.name == "solid" ) {
					if ( lower( ent.classname ) == "triggerbox" ) {
						Map tmp;
						int tmpSolid = nextSolid;
						int tmpFace = nextFace;
						parseSolidNode( child, tmp, tmpSolid, tmpFace );
						if ( !tmp.solids.empty() )
							ent.brushSolids.push_back( std::move( tmp.solids.back() ) );
					} else {
						parseSolidNode( child, map, nextSolid, nextFace );
					}
				}
			}
			if ( !ent.classname.empty() )
				map.entities.push_back( std::move( ent ) );
		}
	}

	map.name = path.stem().string();
	return true;
}

static std::string esc( const std::string& s ) {
	std::string out;
	for ( char c : s ) {
		if ( c == '\\' || c == '"' )
			out.push_back( '\\' );
		out.push_back( c );
	}
	return out;
}

static void writeNum( std::ostream& os, float v ) {
	os << v;
}

static void writeSolid( std::ostream& os, const Solid& solid, int solidId, int& nextId ) {
	os << "\tsolid\n\t{\n";
	os << "\t\t\"id\" \"" << solidId << "\"\n";
	for ( const Face& face : solid.faces ) {
		if ( face.vertices.size() < 3 )
			continue;
		Vec3 p1 = face.vertices[ 0 ];
		Vec3 p2 = face.vertices[ 1 ];
		Vec3 p3 = face.vertices[ 2 ];
		Vec3 n = ( p2 - p1 ).cross( p3 - p1 ).normalized();
		if ( n.dot( face.normal ) < 0.0f )
			std::swap( p2, p3 );
		os << "\t\tside\n\t\t{\n";
		os << "\t\t\t\"id\" \"" << ( nextId++ ) << "\"\n";
		os << "\t\t\t\"plane\" \"(";
		writeNum( os, p1.x );
		os << " ";
		writeNum( os, p1.y );
		os << " ";
		writeNum( os, p1.z );
		os << ") (";
		writeNum( os, p2.x );
		os << " ";
		writeNum( os, p2.y );
		os << " ";
		writeNum( os, p2.z );
		os << ") (";
		writeNum( os, p3.x );
		os << " ";
		writeNum( os, p3.y );
		os << " ";
		writeNum( os, p3.z );
		os << ")\"\n";
		os << "\t\t\t\"material\" \"" << esc( face.material ) << "\"\n";
		os << "\t\t\t\"uaxis\" \"[";
		writeNum( os, face.uaxis.x );
		os << " ";
		writeNum( os, face.uaxis.y );
		os << " ";
		writeNum( os, face.uaxis.z );
		os << " ";
		writeNum( os, face.uShift );
		os << "] ";
		writeNum( os, face.uScale );
		os << "\"\n";
		os << "\t\t\t\"vaxis\" \"[";
		writeNum( os, face.vaxis.x );
		os << " ";
		writeNum( os, face.vaxis.y );
		os << " ";
		writeNum( os, face.vaxis.z );
		os << " ";
		writeNum( os, face.vShift );
		os << "] ";
		writeNum( os, face.vScale );
		os << "\"\n";
		os << "\t\t\t\"rotation\" \"0\"\n";
		os << "\t\t\t\"lightmapscale\" \"" << face.lmScale << "\"\n";
		os << "\t\t\t\"smoothing_groups\" \"" << face.smoothing << "\"\n";
		os << "\t\t\tvertex\n\t\t\t{\n";
		os << "\t\t\t\t\"count\" \"" << face.vertices.size() << "\"\n";
		for ( size_t i = 0; i < face.vertices.size(); ++i ) {
			os << "\t\t\t\t\"vertex" << i << "\" \"";
			writeNum( os, face.vertices[ i ].x );
			os << " ";
			writeNum( os, face.vertices[ i ].y );
			os << " ";
			writeNum( os, face.vertices[ i ].z );
			os << "\"\n";
		}
		os << "\t\t\t}\n";
		os << "\t\t}\n";
	}
	if ( solid.visgroupId > 0 ) {
		os << "\t\teditor\n\t\t{\n";
		os << "\t\t\t\"visgroupid\" \"" << solid.visgroupId << "\"\n";
		os << "\t\t}\n";
	}
	os << "\t}\n";
}

static void writeEntity( std::ostream& os, const Entity& ent, int entityId ) {
	os << "entity\n{\n";
	os << "\t\"id\" \"" << entityId << "\"\n";
	os << "\t\"classname\" \"" << esc( ent.classname ) << "\"\n";
	if ( ent.origin.length() > 0.0f ) {
		os << "\t\"origin\" \"";
		writeNum( os, ent.origin.x );
		os << " ";
		writeNum( os, ent.origin.y );
		os << " ";
		writeNum( os, ent.origin.z );
		os << "\"\n";
	}
	for ( const auto& kv : ent.keys ) {
		if ( kv.first == "id" || kv.first == "classname" || kv.first == "origin" )
			continue;
		os << "\t\"" << esc( kv.first ) << "\" \"" << esc( kv.second ) << "\"\n";
	}
	os << "\teditor\n\t{\n";
	os << "\t\t\"color\" \"255 0 255\"\n";
	if ( ent.visgroupId > 0 )
		os << "\t\t\"visgroupid\" \"" << ent.visgroupId << "\"\n";
	os << "\t\t\"visgroupshown\" \"1\"\n";
	os << "\t\t\"visgroupautoshown\" \"1\"\n";
	os << "\t\t\"parentid\" \"1\"\n";
	os << "\t}\n";
	os << "}\n";
}

// renumber ids since VMF shares them across solids, sides and entities
bool writeVmf( const fs::path& path, const Map& map, std::string& error ) {
	std::ofstream os( path );
	if ( !os ) {
		error = "could not write .vmf";
		return false;
	}
	os << "versioninfo\n{\n";
	os << "\t\"editorversion\" \"400\"\n";
	os << "\t\"editorbuild\" \"8860\"\n";
	os << "\t\"mapversion\" \"1\"\n";
	os << "\t\"formatversion\" \"100\"\n";
	os << "\t\"prefab\" \"0\"\n";
	os << "}\n";
	if ( !map.visgroups.empty() ) {
		os << "visgroups\n{\n";
		for ( const Visgroup& vg : map.visgroups ) {
			os << "\tvisgroup\n\t{\n";
			os << "\t\t\"name\" \"" << esc( vg.name ) << "\"\n";
			os << "\t\t\"visgroupid\" \"" << vg.id << "\"\n";
			os << "\t\t\"color\" \"" << vg.color[ 0 ] << " " << vg.color[ 1 ] << " " << vg.color[ 2 ] << "\"\n";
			os << "\t}\n";
		}
		os << "}\n";
	}
	os << "world\n{\n";
	os << "\t\"id\" \"1\"\n";
	os << "\t\"classname\" \"worldspawn\"\n";
	os << "\t\"mapversion\" \"1\"\n";
	os << "\t\"skyname\" \"\"\n";
	int nextId = 2;
	for ( const Solid& solid : map.solids )
		writeSolid( os, solid, nextId++, nextId );
	os << "}\n";
	for ( const Entity& ent : map.entities )
		writeEntity( os, ent, nextId++ );
	return true;
}
