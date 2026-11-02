#include "vmf.h"

#include <array>
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
			if ( name.text == "{" || name.text == "}" )
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

struct DPlane
{
	double nx = 0.0;
	double ny = 0.0;
	double nz = 0.0;
	double d = 0.0;

	double distance( double x, double y, double z ) const {
		return nx * x + ny * y + nz * z - d;
	}
};

static bool planeFromPoints( const Vec3& a, const Vec3& b, const Vec3& c, DPlane& out ) {
	double ux = (double)c.x - a.x, uy = (double)c.y - a.y, uz = (double)c.z - a.z;
	double vx = (double)b.x - a.x, vy = (double)b.y - a.y, vz = (double)b.z - a.z;
	double nx = uy * vz - uz * vy;
	double ny = uz * vx - ux * vz;
	double nz = ux * vy - uy * vx;
	double len = std::sqrt( nx * nx + ny * ny + nz * nz );
	if ( len < 1e-9 )
		return false;
	nx /= len;
	ny /= len;
	nz /= len;
	out = { nx, ny, nz, nx * a.x + ny * a.y + nz * a.z };
	return true;
}

// keep the part of the polygon behind the plane (distance <= 0)
static void clipPolygon( std::vector<std::array<double, 3>>& poly, const DPlane& plane ) {
	const double eps = 1e-3;
	std::vector<std::array<double, 3>> out;
	out.reserve( poly.size() + 1 );
	for ( size_t i = 0; i < poly.size(); ++i ) {
		const std::array<double, 3>& cur = poly[ i ];
		const std::array<double, 3>& nxt = poly[ ( i + 1 ) % poly.size() ];
		double dc = plane.distance( cur[ 0 ], cur[ 1 ], cur[ 2 ] );
		double dn = plane.distance( nxt[ 0 ], nxt[ 1 ], nxt[ 2 ] );
		bool curInside = dc <= eps;
		bool nxtInside = dn <= eps;
		if ( curInside )
			out.push_back( cur );
		if ( curInside != nxtInside ) {
			double t = dc / ( dc - dn );
			out.push_back( { cur[ 0 ] + ( nxt[ 0 ] - cur[ 0 ] ) * t,
			                 cur[ 1 ] + ( nxt[ 1 ] - cur[ 1 ] ) * t,
			                 cur[ 2 ] + ( nxt[ 2 ] - cur[ 2 ] ) * t } );
		}
	}
	poly.swap( out );
}

// a large quad on the plane then clip it against every other brush plane
static std::vector<Vec3> facePolygon( const DPlane& plane, const std::vector<DPlane>& planes, size_t self ) {
	const double size = 1.0e6;
	Vec3 n{ (float)plane.nx, (float)plane.ny, (float)plane.nz };
	// pick axis least aligned with the normal
	Vec3 axis{ 0, 0, 1 };
	if ( std::abs( n.x ) <= std::abs( n.y ) && std::abs( n.x ) <= std::abs( n.z ) )
		axis = Vec3{ 1, 0, 0 };
	else if ( std::abs( n.y ) <= std::abs( n.z ) )
		axis = Vec3{ 0, 1, 0 };

	Vec3 u = n.cross( axis ).normalized();
	Vec3 v = n.cross( u ).normalized();
	Vec3 center = n * (float)plane.d;

	std::vector<std::array<double, 3>> poly = {
		{ center.x + u.x * size + v.x * size, center.y + u.y * size + v.y * size, center.z + u.z * size + v.z * size },
		{ center.x - u.x * size + v.x * size, center.y - u.y * size + v.y * size, center.z - u.z * size + v.z * size },
		{ center.x - u.x * size - v.x * size, center.y - u.y * size - v.y * size, center.z - u.z * size - v.z * size },
		{ center.x + u.x * size - v.x * size, center.y + u.y * size - v.y * size, center.z + u.z * size - v.z * size },
	};
	for ( size_t i = 0; i < planes.size(); ++i ) {
		if ( i == self )
			continue;
		clipPolygon( poly, planes[ i ] );
		if ( poly.empty() )
			return {};
	}

	std::vector<Vec3> out;
	for ( const std::array<double, 3>& p : poly ) {
		Vec3 point{ (float)p[ 0 ], (float)p[ 1 ], (float)p[ 2 ] };
		if ( !out.empty() && ( point - out.back() ).length() < 1e-3f )
			continue;
		out.push_back( point );
	}
	if ( out.size() >= 2 && ( out.front() - out.back() ).length() < 1e-3f )
		out.pop_back();
	return out;
}

static void rebuildSolidPolygons( std::vector<Face>& faces, const std::vector<std::optional<DPlane>>& planes ) {
	bool needsRebuild = false;
	for ( const Face& face : faces )
		if ( face.vertices.size() < 3 )
			needsRebuild = true;
	if ( !needsRebuild || planes.size() < 4 )
		return;
	for ( const std::optional<DPlane>& plane : planes )
		if ( !plane )
			return;

	std::vector<DPlane> valid;
	valid.reserve( planes.size() );
	for ( const std::optional<DPlane>& plane : planes )
		valid.push_back( *plane );

	for ( size_t i = 0; i < faces.size(); ++i ) {
		if ( faces[ i ].vertices.size() >= 3 )
			continue;
		std::vector<Vec3> polygon = facePolygon( valid[ i ], valid, i );
		if ( polygon.size() >= 3 )
			faces[ i ].vertices = std::move( polygon );
	}
}

static void parseSolidNode( const VNode& node, Map& map, int& nextSolid, int& nextFace ) {
	Solid solid;
	solid.id = nextSolid++;
	if ( const std::string* id = node.get( "id" ) )
		solid.id = std::atoi( id->c_str() );

	std::vector<Face> faces;
	std::vector<std::optional<DPlane>> planes;
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

		std::optional<DPlane> plane;
		if ( const std::string* planeProp = side.get( "plane" ) ) {
			std::vector<Vec3> pts;
			if ( parsePlane( *planeProp, pts ) ) {
				face.normal = ( pts[ 1 ] - pts[ 0 ] ).cross( pts[ 2 ] - pts[ 0 ] ).normalized();
				face.dist = face.normal.dot( pts[ 0 ] );
				DPlane p;
				if ( planeFromPoints( pts[ 0 ], pts[ 1 ], pts[ 2 ], p ) )
					plane = p;
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
		faces.push_back( std::move( face ) );
		planes.push_back( plane );
	}

	rebuildSolidPolygons( faces, planes );

	for ( Face& face : faces ) {
		if ( face.vertices.size() < 3 )
			continue;
		if ( face.normal.length() < 0.001f ) {
			face.normal = ( face.vertices[ 1 ] - face.vertices[ 0 ] ).cross( face.vertices[ 2 ] - face.vertices[ 0 ] ).normalized();
			face.dist = face.normal.dot( face.vertices[ 0 ] );
		}
		solid.faces.push_back( std::move( face ) );
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
			for ( const auto& p : node.props )
				map.worldKeys[ p.first ] = p.second;
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
		if ( n.dot( face.normal ) > 0.0f )
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
