#include "texture.h"

#include <algorithm>
#include <cstdlib>
#include <map>

#include <stb_image.h>

static void appendAncestors( std::vector<fs::path>& roots, fs::path start ) {
	for ( fs::path p = start; !p.empty(); p = p.parent_path() ) {
		roots.push_back( p );
		if ( p == p.parent_path() )
			break;
	}
}

static std::vector<fs::path> searchRoots( const fs::path& mapDir ) {
	std::vector<fs::path> roots;
	appendAncestors( roots, mapDir );

	std::error_code ec;
	fs::path cwd = fs::current_path( ec );
	if ( !ec )
		appendAncestors( roots, cwd );

	if ( const char* env = std::getenv( "DYSLIGHT_CONTENT_DIRS" ) ) {
		std::string value = env;
		size_t start = 0;
		while ( start <= value.size() ) {
			size_t end = value.find( ';', start );
			std::string item = value.substr( start, end == std::string::npos ? std::string::npos : end - start );
			if ( !item.empty() )
				roots.push_back( item );
			if ( end == std::string::npos )
				break;
			start = end + 1;
		}
	}

	std::sort( roots.begin(), roots.end() );
	roots.erase( std::unique( roots.begin(), roots.end() ), roots.end() );
	return roots;
}

std::vector<fs::path> textureDirectories( const fs::path& mapDir ) {
	std::vector<fs::path> dirs;
	for ( const fs::path& root : searchRoots( mapDir ) ) {
		dirs.push_back( root );
		dirs.push_back( root / "Textures" );
		dirs.push_back( root / "GFX" / "map" );
		dirs.push_back( root / "Props" );
		dirs.push_back( root / "Models" );
		dirs.push_back( root / "GFX" / "map" / "Props" );
	}
	std::sort( dirs.begin(), dirs.end() );
	dirs.erase( std::unique( dirs.begin(), dirs.end() ), dirs.end() );
	return dirs;
}

std::string resolveTextureName( const std::string& material, const fs::path& mapDir ) {
	std::string name = material;
	std::replace( name.begin(), name.end(), '\\', '/' );
	size_t slash = name.find_last_of( '/' );
	std::string base = slash == std::string::npos ? name : name.substr( slash + 1 );
	if ( base.find( '.' ) != std::string::npos )
		return base;

	const char* exts[] = { ".jpg", ".png", ".jpeg", ".bmp", ".tga" };
	for ( const fs::path& dir : textureDirectories( mapDir ) ) {
		for ( const char* ext : exts ) {
			std::error_code ec;
			fs::path full = dir / ( base + ext );
			if ( fs::is_regular_file( full, ec ) )
				return full.filename().string();
			full = dir / ( name + ext );
			if ( fs::is_regular_file( full, ec ) )
				return full.filename().string();
		}
	}
	return base + ".jpg";
}

void textureSize( const std::string& material, const fs::path& mapDir, int& w, int& h ) {
	w = 256;
	h = 256;
	static std::map<std::string, std::pair<int, int>> cache;
	auto it = cache.find( material );
	if ( it != cache.end() ) {
		w = it->second.first;
		h = it->second.second;
		return;
	}
	std::string base = material;
	std::replace( base.begin(), base.end(), '\\', '/' );
	size_t slash = base.find_last_of( '/' );
	if ( slash != std::string::npos )
		base = base.substr( slash + 1 );
	const char* exts[] = { ".jpg", ".png", ".jpeg", ".bmp", ".tga" };
	bool hasExtension = base.find( '.' ) != std::string::npos;
	for ( const fs::path& dir : textureDirectories( mapDir ) ) {
		for ( size_t e = 0; e < ( hasExtension ? 1 : 5 ); ++e ) {
			std::error_code ec;
			fs::path full = hasExtension ? ( dir / base ) : ( dir / ( base + exts[ e ] ) );
			if ( !fs::is_regular_file( full, ec ) )
				continue;
			int iw = 0, ih = 0, channels = 0;
			if ( stbi_info( full.string().c_str(), &iw, &ih, &channels ) && iw > 0 && ih > 0 ) {
				w = iw;
				h = ih;
				cache[ material ] = { w, h };
				return;
			}
		}
	}
	cache[ material ] = { w, h };
}

std::string resolveModelName( const std::string& model, const fs::path& mapDir ) {
	std::string base = model;
	std::replace( base.begin(), base.end(), '\\', '/' );
	size_t slash = base.find_last_of( '/' );
	if ( slash != std::string::npos )
		base = base.substr( slash + 1 );
	if ( base.find( '.' ) != std::string::npos )
		return base;

	const std::vector<fs::path> dirs = textureDirectories( mapDir );
	const char* exts[] = { ".b3d", ".x", ".fbx", ".3ds", ".obj" };
	for ( const fs::path& dir : dirs ) {
		for ( const char* ext : exts ) {
			std::error_code ec;
			fs::path full = dir / ( base + ext );
			if ( fs::is_regular_file( full, ec ) )
				return full.filename().string();
		}
	}
	return base + ".b3d";
}

bool isTransparentMaterial( const std::string& material ) {
	std::string m = lower( material );
	const char* patterns[] = { "glass", "window", "trans", "alpha", "grate", "fence", "tree", "leaf", "wire", "screen", "water", "ladder", "rail" };
	for ( const char* p : patterns )
		if ( m.find( p ) != std::string::npos )
			return true;
	return false;
}
