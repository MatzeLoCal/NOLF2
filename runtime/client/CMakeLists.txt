cmake_minimum_required (VERSION 3.5)
project (EXE_Lithtech)

add_definitions(-DDE_LOCAL_SERVERBIND -DDE_CLIENT_COMPILE -DDSNDMGR_NO_MFC -DDIRECTENGINE_COMPILE -DMODEL_SUPPORT_ABC)
if(CMAKE_CL_64)
	link_directories("$ENV{DXSDK_DIR}Lib/x64")
else(CMAKE_CL_64)
	link_directories("$ENV{DXSDK_DIR}Lib/x86")
endif(CMAKE_CL_64)
	
add_executable (EXE_Lithtech WIN32
	../controlfilemgr/controlfilemgr.cpp
	src/client_filemgr.cpp
	../kernel/src/icommandlineargs.cpp
	src/client_iltcommon.cpp
	../server/src/server_iltcommon.cpp
	../shared/src/shared_iltcommon.cpp
	../kernel/src/sys/win/lt_cursor_impl.cpp
	src/client_iltmodel.cpp
	../shared/src/modellt_impl.cpp
	../server/src/server_iltmodel.cpp
	src/client_iltphysics.cpp
	../server/src/server_iltphysics.cpp
	../shared/src/shared_iltphysics.cpp
	../server/src/server_iltsoundmgr.cpp
	../sound/src/soundmgr.cpp
	../shared/src/transformlt_impl.cpp
	src/client_iltvideomgr.cpp
	../shared/src/interface_linkage.cpp
	../world/src/world_blind_object_data.cpp
	../world/src/world_blocker_data.cpp
	../world/src/world_blocker_math.cpp
	src/world_client_bsp.cpp
	../world/src/world_particle_blocker_data.cpp
	../server/src/world_server_bsp.cpp
	../world/src/world_shared_bsp.cpp
	../model/src/animtracker.cpp
	../shared/src/bdefs.cpp
	../kernel/src/sys/win/bindmgr.cpp
	../kernel/src/sys/win/binkvideomgrimpl.cpp
	../shared/src/classbind.cpp
	../server/src/classmgr.cpp
	../kernel/src/sys/win/client.cpp
	src/client_formatmgr.cpp
	src/client_graphmgr.cpp
	src/client_ticks.cpp
	src/clientde_impl.cpp
	src/clientmgr.cpp
	src/clientshell.cpp
	src/cmoveabstract.cpp
	src/cnet.cpp
	src/cobject.cpp
	../shared/src/collision.cpp
	../shared/src/compress.cpp
	../shared/src/concommand.cpp
	../shared/src/conparse.cpp
	src/console.cpp
	src/consolecommands.cpp
	../kernel/src/sys/linux/counter.cpp
	src/sys/win/customfontfilemgr.cpp
	src/cutil.cpp
	../model/src/sys/d3d/d3d_model_load.cpp
	# d3ddrawprim.cpp is the D3D9 DrawPrim implementation: it drives the global
	# g_Device (defined in the excluded d3d_device.cpp) and is replaced by the
	# GL DrawPrim in Phase 2. A null ILTDrawPrim stub provides the symbols.
	#../render_b/src/sys/d3d/d3ddrawprim.cpp
	../render_b/src/sys/d3d/d3dtexinterface.cpp
	../kernel/io/src/sys/linux/linuxfile.cpp
	../world/src/de_mainworld.cpp
	../kernel/mem/src/sys/linux/de_memory.cpp
	../kernel/mem/src/sys/linux/linuxundata.cpp
	../world/src/de_nodes.cpp
	../world/src/de_objects.cpp
	../shared/src/debuggeometry.cpp
	../kernel/src/debugging.cpp
	src/debuggraphmgr.cpp
	../shared/src/dhashtable.cpp
	# DirectShow video manager: Windows-only (streams.h/ddraw.h). videomgr.cpp
	# guards out the DShow path on macOS; video is a Phase 4 concern.
	#../kernel/src/sys/win/dshowvideomgrimpl.cpp
	../shared/src/sys/win/dstreamopenqueuemgr.cpp
	../kernel/src/sys/linux/linuxdsys.cpp
	../shared/src/dtxmgr.cpp
	../kernel/src/sys/win/dutil.cpp
	../shared/src/engine_vars.cpp
	src/errorlog.cpp
	../shared/src/findobj.cpp
	../shared/src/ftclient.cpp
	../shared/src/ftserv.cpp
	../world/src/fullintersectline.cpp
	../server/src/game_serialize.cpp
	../shared/src/gamemath.cpp
	../shared/src/genericprop_setup.cpp
	../shared/src/genltstream.cpp
	../shared/src/geometry.cpp
	../shared/src/geomroutines.cpp
	../shared/src/impl_common.cpp
	../kernel/src/sys/win/input.cpp
	# InputSim.cpp is a DirectInput-based debug input manager; input.cpp is
	# already a macOS null-stub and nothing references its isim_* symbols here.
	#../kernel/src/sys/win/InputSim.cpp
	../kernel/src/sys/win/interface_helpers.cpp
	../server/src/interlink.cpp
	../world/src/intersect_line.cpp
	../world/src/intersectsweptsphere.cpp
	../shared/src/leech.cpp
	../world/src/light_table.cpp
	../shared/src/lightmap_compress.cpp
	../shared/src/lightmap_planes.cpp
	src/linesystem.cpp
	../kernel/src/sys/win/load_pcx.cpp
	../kernel/net/src/localdriver.cpp
	src/ltbenchmark_impl.cpp
	../kernel/src/sys/win/ltdirectmusic_impl.cpp
	../../sdk/inc/lterror.cpp
	../kernel/src/sys/linux/lthread.cpp
	src/ltinfo_impl.cpp
	../kernel/src/sys/linux/ltlibraryloader.cpp
	../kernel/mem/src/ltmemory.cpp
	../shared/src/ltmessage.cpp
	src/ltmessage_client.cpp
	../server/src/ltmessage_server.cpp
	../../sdk/inc/ltmodule.cpp
	../../sdk/inc/ltobjref.cpp
	../../sdk/inc/ltquatbase.cpp
	../kernel/src/sys/win/ltrendermgr_impl.cpp
	src/lttexturestring.cpp
	../kernel/src/sys/linux/ltthread.cpp
	../shared/src/lttimer.cpp
	src/memorywatch.cpp
	../model/src/model.cpp
	../model/src/model_load.cpp
	../model/src/modelallocations.cpp
	../shared/src/motion.cpp
	../shared/src/moveobject.cpp
	../shared/src/moveplayer.cpp
	../kernel/src/sys/win/musicdriver.cpp
	../kernel/net/src/netmgr.cpp
	../shared/src/nexus.cpp
	#../model/src/sys/null/nullmodel_load.cpp
	../shared/src/objectmgr.cpp
	../kernel/net/src/packet.cpp
	../shared/src/parse_world_info.cpp
	src/particlesystem.cpp
	../shared/src/pixelformat.cpp
	src/polygrid.cpp
	src/predict.cpp
	../shared/src/ratetracker.cpp
	../kernel/src/sys/win/render.cpp
	../server/src/s_client.cpp
	../server/src/s_concommand.cpp
	../server/src/s_Intersect.cpp
	../server/src/s_net.cpp
	../server/src/s_object.cpp
	../server/src/server_consolestate.cpp
	../server/src/server_extradata.cpp
	../server/src/server_filemgr.cpp
	../server/src/serverde_impl.cpp
	../server/src/serverevent.cpp
	../server/src/servermgr.cpp
	src/setupobject.cpp
	src/shellnet.cpp
	src/shelltransfer.cpp
	src/shellutil.cpp
	../server/src/smoveabstract.cpp
	../sound/src/soundbuffer.cpp
	../sound/src/sounddata.cpp
	../sound/src/soundinstance.cpp
	../server/src/soundtrack.cpp
	src/sprite.cpp
	../shared/src/spritecontrolimpl.cpp
	../shared/src/stacktrace.cpp
	../shared/src/stdlterror.cpp
	../kernel/src/sys/linux/streamsim.cpp
	../kernel/src/sys/win/stringmgr.cpp
	../shared/src/strtools.cpp
	../kernel/src/sys/win/systeminfo.cpp
	../kernel/src/sys/win/text_mgr.cpp
	src/texturestring.cpp
	src/sys/win/texturestringimage.cpp
	../kernel/src/sys/linux/timemgr.cpp
	../model/src/transformmaker.cpp
	../kernel/net/src/sys/win/udpdriver.cpp
	# The Win32 udpdriver uses the portable (pthreads) net thread class
	# CLinux_LTThread; build its implementation on macOS too.
	../kernel/net/src/sys/linux/linux_ltthread.cpp
	../shared/src/version_info.cpp
	../kernel/src/sys/win/version_resource.cpp
	../kernel/src/sys/win/videomgr.cpp
	src/volumeeffect.cpp
	src/watermark.cpp
	../sound/src/wave.cpp
	src/sys/win/winclientde_impl.cpp
	src/sys/win/winconsole_impl.cpp
	../world/src/world_tree.cpp)
set_target_properties(EXE_Lithtech
	PROPERTIES OUTPUT_NAME Lithtech)
	
include_directories(src
	../../sdk/inc
	../../sdk/inc/physics
	../../libs/stdlith
	../../libs/rezmgr
	../../libs/lith
	../shared/src
	../kernel/src
	../kernel/mem/src
	../world/src
	../kernel/io/src
	../kernel/net/src
	../model/src
	../client/src
	../sound/src
	../shared/src/sys/win
	../lithtemplate
	../physics/src
	../info/src
	../render_b/src
	../ui/src
	../controlfilemgr
	../server/src)

# sys/win headers needed on all platforms (win-named sys .cpp compiled everywhere;
# render interface lives in shared/src/sys/win).
include_directories(../kernel/src/sys/win
	../shared/src/sys/win)

if(ENABLE_DIRECTX)	# FIXME: set to find DirectX path
	include_directories(../../libs/Lib_DShow
		"$ENV{DXSDK_DIR}Include")
		#if(CMAKE_CL_64)
		#	link_directories("C:\\Program Files (x86)\\Microsoft DirectX SDK (February 2006)\\Lib\\x64")
		#else(CMAKE_CL_64)
			
			# FIXME: Why is this not working??
			
		
		#endif(CMAKE_CL_64)
		target_link_libraries(EXE_Lithtech
			ddraw
			d3dx9
			d3d9
			DxErr
			dxguid
			dinput8
			strmiids)
endif(ENABLE_DIRECTX)

target_link_libraries(EXE_Lithtech
	Lib_Lith
	Lib_StdLith
	Lib_ZLib
	ltmem
	Lib_RezMgr
	Lib_Info
	Lib_ILTSound
	Lib_UI)

# Windows-only renderer/video libs; on macOS a null renderer + stubs provide
# these symbols instead.
if(WIN32)
	target_link_libraries(EXE_Lithtech Lib_DShow LIB_D3DRender)
endif(WIN32)
	
if(WIN32)
	include_directories(../kernel/src/sys/win
		../shared/src/sys/win)
	target_link_libraries(EXE_Lithtech
		winmm)
endif(WIN32)

if(ENABLE_D3D)
	include_directories(../render_a/src/sys/d3d)
endif(ENABLE_D3D)

if(MSVC)
	set_target_properties(EXE_Lithtech PROPERTIES COMPILE_FLAGS "/wd4996")
endif(MSVC)