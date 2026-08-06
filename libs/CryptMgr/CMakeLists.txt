cmake_minimum_required (VERSION 3.5)
project (CryptMgr)
add_library (CryptMgr STATIC 
	blowfish.cpp 
	cryptmgr.cpp)