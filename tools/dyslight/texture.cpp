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

static const char* const textureExtensions[] = { ".jpeg", ".jpg", ".png" };

static std::string normalizeName( const std::string& name ) {
	std::string out = name;
	std::replace( out.begin(), out.end(), '\\', '/' );
	while ( out.rfind( "./", 0 ) == 0 )
		out.erase( 0, 2 );
	return out;
}

static bool findTextureFile( const std::string& material, const fs::path& mapDir, fs::path& out ) {
	const std::string name = normalizeName( material );
	const bool hasExtension = fs::path( name ).has_extension();
	for ( const fs::path& dir : textureDirectories( mapDir ) ) {
		std::error_code ec;
		if ( hasExtension ) {
			fs::path full = dir / name;
			if ( fs::is_regular_file( full, ec ) ) {
				out = full;
				return true;
			}
			continue;
		}
		for ( const char* ext : textureExtensions ) {
			fs::path full = dir / ( name + ext );
			if ( fs::is_regular_file( full, ec ) ) {
				out = full;
				return true;
			}
		}
	}
	return false;
}

std::string resolveTextureName( const std::string& material, const fs::path& mapDir ) {
	const std::string name = normalizeName( material );
	if ( fs::path( name ).has_extension() )
		return name;

	for ( const fs::path& dir : textureDirectories( mapDir ) ) {
		for ( const char* ext : textureExtensions ) {
			std::error_code ec;
			if ( fs::is_regular_file( dir / ( name + ext ), ec ) )
				return name + ext;
		}
	}
	return name;
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

	fs::path file;
	if ( findTextureFile( material, mapDir, file ) ) {
		int iw = 0, ih = 0, channels = 0;
		if ( stbi_info( file.string().c_str(), &iw, &ih, &channels ) && iw > 0 && ih > 0 ) {
			w = iw;
			h = ih;
		}
	}
	cache[ material ] = { w, h };
}

bool textureHasAlpha( const std::string& material, const fs::path& mapDir ) {
	static std::map<std::string, bool> cache;
	auto it = cache.find( material );
	if ( it != cache.end() )
		return it->second;

	bool transparent = false;
	fs::path file;
	if ( findTextureFile( material, mapDir, file ) ) {
		int w = 0, h = 0, channels = 0;
		if ( unsigned char* pixels = stbi_load( file.string().c_str(), &w, &h, &channels, 4 ) ) {
			for ( int x = 0; x < 4 && !transparent; ++x ) {
				for ( int y = 0; y < 4; ++y ) {
					const int px = x * ( w - 1 ) / 3;
					const int py = y * ( h - 1 ) / 3;
					if ( pixels[ ( (size_t)py * (size_t)w + (size_t)px ) * 4 + 3 ] != 255 ) {
						transparent = true;
						break;
					}
				}
			}
			stbi_image_free( pixels );
		}
	}
	cache[ material ] = transparent;
	return transparent;
}

std::string resolveModelName( const std::string& model, const fs::path& mapDir ) {
	const std::string name = normalizeName( model );
	if ( fs::path( name ).has_extension() )
		return name;

	const char* exts[] = { ".fbx", ".x", ".b3d", ".3ds", ".obj" };
	for ( const fs::path& dir : textureDirectories( mapDir ) ) {
		for ( const char* ext : exts ) {
			std::error_code ec;
			fs::path full = dir / ( name + ext );
			if ( fs::is_regular_file( full, ec ) )
				return full.filename().string();
		}
	}
	return name + ".x";
}
