cmake_minimum_required (VERSION 3.5)
project (MFCStub)
add_library (MFCStub STATIC 
	mfcs_point.cpp
	mfcs_rect.cpp
	mfcs_string.cpp)
include_directories (../stdlith) 
if(MSVC)
	set_target_properties(MFCStub PROPERTIES COMPILE_FLAGS "/wd4996")
endif(MSVC)