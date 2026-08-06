cmake_minimum_required (VERSION 3.5)
project (ServerDir)

if(WIN32)
	add_library (ServerDir SHARED
		CServerDir_Titan.cpp
		stdafx.cpp)
	include_directories (../../sdk/inc
		../WONAPI
		../../NOLF2/ClientRes/Shared)
	target_link_libraries(ServerDir
		WONAPI)
	if(MSVC)
		set_target_properties(ServerDir PROPERTIES COMPILE_FLAGS "/wd4996")
	endif(MSVC)
else()
	# macOS: the WON/Titan online stack is not ported. Build a stub whose factory
	# returns NULL (callers null-check it) so the game DLLs link without WONAPI.
	add_library (ServerDir STATIC
		serverdir_macos_stub.cpp)
	include_directories (../../sdk/inc)
endif()