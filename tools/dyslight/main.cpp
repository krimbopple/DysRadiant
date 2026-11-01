#include "bake.h"
#include "map3dw.h"
#include "rmesh.h"
#include "texture.h"
#include "vmf.h"

#include <cstdlib>
#include <iostream>

static void printUsage() {
	std::cout << "dyslight - CBRE-EX if it was good\n";
	std::cout << "  1. Bake with CBRE-EX-style lightmaps (default)\n";
	std::cout << "  2. Bake with 3DWS-style lightmaps\n";
	std::cout << "  --convert <input.3dw> [output.vmf]\n";
}

int main( int argc, char** argv ) {
	int mode = 0;
	bool convertOnly = false;
	std::string inputLine;
	std::string outputLine;

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

		while ( mode != 1 && mode != 2 ) {
			std::cout << "Select mode [1/2]: ";
			std::string line;
			if ( !std::getline( std::cin, line ) )
				return 1;
			mode = std::atoi( line.c_str() );
		}

		std::cout << "Input .vmf or .3dw: ";
		if ( !std::getline( std::cin, inputLine ) || inputLine.empty() ) {
			std::cerr << "No input path given.\n";
			return 1;
		}
	}

	if ( !convertOnly && mode != 1 && mode != 2 ) {
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

	bool cbreStyle = mode == 1;
	BakeResult baked = bakeMap( map, cbreStyle );
	if ( baked.pages.empty() ) {
		std::cerr << "No bakeable faces found.\n";
		return 1;
	}
	if ( !writeBake( base, baked, cbreStyle, error ) ) {
		std::cerr << "Bake error: " << error << "\n";
		return 1;
	}

	std::cout << "Baked " << baked.faces.size() << " faces.\n";
	std::cout << "Atlas pages: " << baked.pages.size() << " (" << ( cbreStyle ? "CBRE-EX" : "3DWS" ) << " style).\n";

	if ( !writeRmesh( base, map, baked, error ) ) {
		std::cerr << "RMesh error: " << error << "\n";
		return 1;
	}

	fs::path rmeshPath = base;
	rmeshPath += ".rmesh";
	std::cout << "Exported: " << rmeshPath.string() << "\n";
	return 0;
}
