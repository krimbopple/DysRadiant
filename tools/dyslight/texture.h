#pragma once

#include "map.h"

std::vector<fs::path> textureDirectories( const fs::path& mapDir );
std::string resolveTextureName( const std::string& material, const fs::path& mapDir );
void textureSize( const std::string& material, const fs::path& mapDir, int& w, int& h );
std::string resolveModelName( const std::string& model, const fs::path& mapDir );
bool textureHasAlpha( const std::string& material, const fs::path& mapDir );
