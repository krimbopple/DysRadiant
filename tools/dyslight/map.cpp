#include "map.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

std::string lower( std::string s ) {
	std::transform( s.begin(), s.end(), s.begin(), []( unsigned char c ) { return (char)std::tolower( c ); } );
	return s;
}

Vec3 parseVec3( const std::string& s, Vec3 fallback ) {
	const char* p = s.c_str();
	float v[ 3 ];
	int n = 0;
	while ( *p && n < 3 ) {
		while ( *p && !std::isdigit( (unsigned char)*p ) && *p != '-' && *p != '+' && *p != '.' )
			++p;
		if ( !*p )
			break;
		char* end = nullptr;
		v[ n ] = std::strtof( p, &end );
		if ( end == p )
			break;
		++n;
		p = end;
	}
	return n == 3 ? Vec3{ v[ 0 ], v[ 1 ], v[ 2 ] } : fallback;
}

void orientSolidFaces( Solid& solid ) {
	if ( solid.faces.empty() )
		return;

	Vec3 center;
	int count = 0;
	for ( const Face& face : solid.faces ) {
		for ( const Vec3& v : face.vertices ) {
			center = center + v;
			++count;
		}
	}
	if ( count == 0 )
		return;
	center = center * ( 1.0f / (float)count );

	// source normals are not always reliable, so use the winding and flip it when needed
	for ( Face& face : solid.faces ) {
		if ( face.vertices.size() < 3 )
			continue;
		Vec3 n = ( face.vertices[ 1 ] - face.vertices[ 0 ] ).cross( face.vertices[ 2 ] - face.vertices[ 0 ] ).normalized();
		Vec3 fc;
		for ( const Vec3& v : face.vertices )
			fc = fc + v;
		fc = fc * ( 1.0f / (float)face.vertices.size() );
		if ( n.dot( fc - center ) < 0.0f )
			n = -n;
		face.normal = n;
		face.dist = n.dot( face.vertices[ 0 ] );
	}
}
