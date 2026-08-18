#ifndef __RENDER_PARTICLES_H__
#define __RENDER_PARTICLES_H__

// OT_PARTICLESYSTEM draw path.
//
// Port of d3d_DrawParticleSystem (drawparticles.cpp) + d3d_TestAndDrawPS
// (drawparticles_A.cpp). Until this existed the renderer drew NO particle
// systems at all, which is why every fire in the game was invisible and why
// C01S01's census read "ParticleSystem 1/1 NOT DRAWN by GL renderer" -- that
// one is JA_Waterfall_2, the missing half of §36's waterfall.

// ⚠️ LTVector is a TYPEDEF, not a class — it cannot be forward-declared here.
// Include this header after the engine's base types (bdefs.h), as the other
// render headers in this directory do.

// Billboard basis for this scene, in WORLD space (same values the sprite pass
// gets). Must be called before RParticle_DrawSystems.
void RParticle_SetCamera(const LTVector &vRight, const LTVector &vUp,
                          const LTVector &vForward, const LTVector &vPos);

// Draw the WORLD-SPACE particle systems. Translucent: call it with the rest of
// the translucent set, after the opaque world and the models.
void RParticle_DrawSystems();

// Draw the FLAG_REALLYCLOSE (player-view) systems — first-person muzzle and
// tool effects, e.g. the welder's flame.
// ⚠️ Call from INSIDE the player-view pass, while its projection/view are
// installed; these are in camera space and billboard off the untransformed
// axes, not the camera's world basis.
void RParticle_DrawPlayerView();

#endif // __RENDER_PARTICLES_H__
