#include "bake.h"
#include "map3dw.h"
#include "rmesh.h"
#include "texture.h"
#include "vmf.h"

#include <cstdlib>
#include <iostream>
#include <vector>

static void printUsage() {
	std::cout << "dyslight - CBRE-EX if it was good\n";
	std::cout << "  1. Bake with CBRE-EX-style lightmaps\n";
	std::cout << "  2. Bake with 3DWS-style lightmaps (default)\n";
	std::cout << "  --atlas <px> lightmap sheet size\n";
	std::cout << "  --blur <n> blur radius per face lightmap\n";
	std::cout << "  --border <n> padding texels per face lightmap\n";
	std::cout << "  --texels <units> map units per lightmap texel\n";
	std::cout << "  --ambient <r g b> ambient light colour, 0-255\n";
	std::cout << "  --noshadows\n";
}

static void modeDefaults( int mode, int& atlas, int& blur, int& border ) {
	const bool cbreSheet = mode == 1;
	atlas = cbreSheet ? 1024 : 512;
	blur = cbreSheet ? 0 : 1;
	border = cbreSheet ? 0 : 2;
}

static int askInt( const char* label, int fallback ) {
	std::cout << label << " [" << fallback << "]: ";
	std::string line;
	if ( !std::getline( std::cin, line ) || line.empty() )
		return fallback;
	return std::atoi( line.c_str() );
}

int main( int argc, char** argv ) {
	int mode = 0;
	bool convertOnly = false;
	BakeSettings settings;
	std::string inputLine;
	std::string outputLine;

	std::vector<std::string> flags;
	for ( int i = 1; i < argc; ++i )
		flags.push_back( argv[ i ] );

	std::vector<std::string> positional;
	for ( size_t i = 0; i < flags.size(); ++i ) {
		const std::string flag = flags[ i ];
		auto value = [ & ]() { return i + 1 < flags.size() ? flags[ ++i ] : std::string(); };
		if ( flag == "--noshadows" ) {
			settings.shadows = false;
		} else if ( flag == "--atlas" ) {
			settings.atlasSize = std::atoi( value().c_str() );
		} else if ( flag == "--blur" ) {
			settings.blurRadius = std::atoi( value().c_str() );
		} else if ( flag == "--border" ) {
			settings.border = std::atoi( value().c_str() );
		} else if ( flag == "--texels" ) {
			settings.texelSize = (float)std::atof( value().c_str() );
		} else if ( flag == "--ambient" ) {
			const std::string r = value();
			const std::string g = value();
			const std::string b = value();
			settings.ambient = { (float)std::atof( r.c_str() ), (float)std::atof( g.c_str() ), (float)std::atof( b.c_str() ) };
			settings.hasAmbient = true;
		} else {
			positional.push_back( flag );
		}
	}

	std::string program = argv[ 0 ];
	std::vector<char*> args;
	args.push_back( &program[ 0 ] );
	for ( std::string& item : positional )
		args.push_back( &item[ 0 ] );
	argc = (int)args.size();
	argv = args.data();

	if ( argc >= 2 && std::string( argv[ 1 ] ) == "--convert" ) {
		if ( argc < 3 ) {
			printUsage();
			return 1;
		}
		convertOnly = true;
		inputLine = argv[ 2 ];
		if ( argc >= 4 )
			outputLine = argv[ 3 ];
	} else if ( argc >= 3 ) {
		mode = std::atoi( argv[ 1 ] );
		inputLine = argv[ 2 ];
	} else {
		printUsage();

		while ( mode < 1 || mode > 3 ) {
			std::cout << "Select mode [1/2/3] (default 2 / 3DWS): ";
			std::string line;
			if ( !std::getline( std::cin, line ) )
				return 1;
			mode = line.empty() ? 2 : std::atoi( line.c_str() );
		}

		int atlas = 0, blur = 0, border = 0;
		modeDefaults( mode, atlas, blur, border );
		std::cout << "Lightmap settings\n";
		if ( settings.atlasSize == 0 )
			settings.atlasSize = askInt( "  sheet size (px)", atlas );
		if ( settings.blurRadius < 0 )
			settings.blurRadius = askInt( "  blur radius", blur );
		if ( settings.border < 0 )
			settings.border = askInt( "  border (texels)", border );
		if ( settings.texelSize <= 0.0f ) {
			std::cout << "  map units per texel [0]: ";
			std::string line;
			if ( std::getline( std::cin, line ) && !line.empty() )
				settings.texelSize = (float)std::atof( line.c_str() );
		}

		std::cout << "Input .vmf or .3dw: ";
		if ( !std::getline( std::cin, inputLine ) || inputLine.empty() ) {
			std::cerr << "No input path given.\n";
			return 1;
		}
	}

	if ( !convertOnly && ( mode < 1 || mode > 3 ) ) {
		printUsage();
		return 1;
	}

	fs::path input = fs::path( inputLine );
	if ( !fs::exists( input ) ) {
		std::cerr << "Input does not exist: " << input.string() << "\n";
		return 1;
	}

	const fs::path base = input.parent_path() / input.stem();

	Map map;
	std::string error;
	std::string ext = lower( input.extension().string() );
	bool converted = false;

	if ( ext == ".3dw" ) {
		if ( !parse3dw( input, map, error ) ) {
			std::cerr << "3DW error: " << error << "\n";
			return 1;
		}
		fs::path vmfPath;
		if ( outputLine.empty() ) {
			vmfPath = base;
			vmfPath += ".vmf";
		} else {
			vmfPath = fs::path( outputLine );
		}
		if ( !writeVmf( vmfPath, map, error ) ) {
			std::cerr << "VMF error: " << error << "\n";
			return 1;
		}
		std::cout << "Converted: " << vmfPath.string() << "\n";
		converted = true;
	} else if ( ext == ".vmf" ) {
		if ( !parseVmf( input, map, error ) ) {
			std::cerr << "VMF error: " << error << "\n";
			return 1;
		}
	} else {
		std::cerr << "Unsupported input extension: " << ext << "\n";
		return 1;
	}

	if ( convertOnly )
		return 0;

	std::cout << "Map: " << map.name << "\n";
	std::cout << "Solids: " << map.solids.size() << "\n";
	std::cout << "Entities: " << map.entities.size() << "\n";

	if ( converted )
		std::cout << "Conversion done. Continuing to bake.\n";

	BakeMode bakeMode = static_cast<BakeMode>( mode );
	bakeSettingsApplyMode( bakeMode, settings );
	BakeResult baked = bakeMap( map, bakeMode, base.parent_path(), settings );
	if ( baked.pages.empty() ) {
		std::cerr << "No bakeable faces found.\n";
		return 1;
	}
	if ( !writeBake( base, baked, bakeMode, settings, error ) ) {
		std::cerr << "Bake error: " << error << "\n";
		return 1;
	}

	std::cout << "Baked " << baked.faces.size() << " faces.\n";
	std::cout << "Atlas pages: " << baked.pages.size() << " (" << bakeModeName( bakeMode ) << ").\n";
	std::cout << "Settings: " << baked.atlasSize << "px sheets, blur " << settings.blurRadius << ", border " << settings.border << ", ";
	if ( settings.texelSize > 0.0f )
		std::cout << settings.texelSize << " units/texel";
	else
		std::cout << "per-face lightmapscale";
	if ( settings.hasAmbient )
		std::cout << ", ambient " << settings.ambient.x << " " << settings.ambient.y << " " << settings.ambient.z;
	else
		std::cout << ", ambient from worldspawn";
	std::cout << ", shadows " << ( settings.shadows ? "on" : "off" ) << "\n";

	if ( !writeRmesh( base, map, baked, error ) ) {
		std::cerr << "RMesh error: " << error << "\n";
		return 1;
	}

	fs::path rmeshPath = base;
	rmeshPath += ".rmesh";
	std::cout << "Exported: " << rmeshPath.string() << "\n";
	return 0;
}
