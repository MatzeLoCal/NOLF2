cmake_minimum_required (VERSION 3.5)
project (RegMgr)
add_library (RegMgr STATIC 
	regmgr.cpp)
include_directories (../lith) 