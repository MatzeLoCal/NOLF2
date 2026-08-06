cmake_minimum_required (VERSION 3.5)
project (DLL_dx8)

if(CMAKE_CL_64)
	link_directories("$ENV{DXSDK_DIR}Lib/x64")
else(CMAKE_CL_64)
	link_directories("$ENV{DXSDK_DIR}Lib/x86")
endif(CMAKE_CL_64)

add_library (DLL_dx8 SHARED
	../../../../kernel/src/sys/win/counter.cpp
	parsewavefile.cpp
	s_dx8.cpp
	StdAfx.cpp)
include_directories (../..
	../../../../../sdk/inc 
	../../../../kernel/src
	../../../../shared/src/sys/win)
set_target_properties(DLL_dx8
	PROPERTIES OUTPUT_NAME SndDrv)
target_link_libraries(DLL_dx8
	dxguid
	winmm
	msacm32
	ltmem)
	
if(NOT CMAKE_CL_64)
	target_link_libraries(DLL_dx8 "${CMAKE_CURRENT_SOURCE_DIR}/eaxguid.lib")
endif(NOT CMAKE_CL_64)
	