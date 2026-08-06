cmake_minimum_required (VERSION 3.5)
project (ltmem)
add_library (ltmem STATIC 
	ltmem.cpp
	ltmemdebug.cpp
	ltmemheap.cpp
	ltmemstats.cpp
	ltmemtrack.cpp
	stdafx.cpp)
include_directories (../../sdk/inc
	../../runtime/shared/src
	../../runtime/kernel/src
	../stdlith
	../../runtime/kernel/mem/src)
if(CMAKE_COMPILER_IS_GNUCXX)
	set_target_properties(ltmem PROPERTIES COMPILE_FLAGS "-fpermissive")
endif(CMAKE_COMPILER_IS_GNUCXX)
