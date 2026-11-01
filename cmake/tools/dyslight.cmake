if(NOT RADIANT_BUILD_DYSLIGHT)
	return()
endif()

radiant_add_tool(dyslight
	SOURCES
		${PROJECT_SOURCE_DIR}/tools/dyslight/main.cpp
		${PROJECT_SOURCE_DIR}/tools/dyslight/map.cpp
		${PROJECT_SOURCE_DIR}/tools/dyslight/map3dw.cpp
		${PROJECT_SOURCE_DIR}/tools/dyslight/vmf.cpp
		${PROJECT_SOURCE_DIR}/tools/dyslight/texture.cpp
		${PROJECT_SOURCE_DIR}/tools/dyslight/bake.cpp
		${PROJECT_SOURCE_DIR}/tools/dyslight/rmesh.cpp
	DEPENDENCIES
		dysradiant-library-stb
	INCLUDE_DIRECTORIES
		${stb_SOURCE_DIR}
)
