#pragma once

#include "map.h"

bool parseVmf( const fs::path& path, Map& map, std::string& error );
bool writeVmf( const fs::path& path, const Map& map, std::string& error );
