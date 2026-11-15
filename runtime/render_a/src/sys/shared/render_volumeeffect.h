#ifndef __RENDER_VOLUMEEFFECT_H__
#define __RENDER_VOLUMEEFFECT_H__

// OT_VOLUMEEFFECT draw path — SNOWFALL, and the grass/debris of ScatterFX.
//
// Until this existed the renderer drew NO volume effects at all, which is why
// every Siberia level (C03S01..C03S04, C05S04/S05) and the snowy parts of
// C06S03/C12/C13 fell completely still: the levels author SnowVolume objects
// (C03S01 has 23 of them), the game turns each into airspaces and fills a
// vertex buffer every frame, and the renderer threw all of it away. The only
// occurrence of the words "VolumeEffect" anywhere in the compiled render path
// was a debug name string.
//
// Authority: d3d_ProcessVolumeEffect / DrawDynamicParticles
// (sys/d3d/drawvolumeeffects.cpp), queued from drawobjects.cpp:231.
//
// ⚠️ THE VERTEX LAYOUT IS AN ABI, AND IT IS DECLARED THREE TIMES.
//    The GAME writes straight into the buffer we hand it, as
//    `struct DynamicParticleVertex { float x,y,z; uint32 color; float u,v; }`
//    — 24 bytes, colour 0xAARRGGBB. That struct is NOT in a shared header: it
//    is re-declared privately in SnowFX.cpp:40, ScatterFX.cpp:32 and the
//    donor's drawvolumeeffects.cpp:57. Change our copy and snow turns to
//    confetti with no compiler error anywhere.

void RVolumeEffect_Draw();

#endif // __RENDER_VOLUMEEFFECT_H__
