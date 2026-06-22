#pragma once

#include "WallpaperEngine/Render/CObject.h"

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render::Objects {
/**
 * Wallpaper Engine "compose layer" (model models/util/composelayer.json).
 *
 * In WE a compose layer is a *passthrough* effect: it re-samples the scene framebuffer
 * (_rt_FullFrameBuffer) through a projective transform — that is how the cafe's floor/glass
 * reflections are produced (see assets/shaders/composelayer.{vert,frag}). It does NOT own or
 * composite its children: the children render normally into the scene and inherit the
 * layer's groupScale/groupAngle through the parent chain (CObject::resolveTransform handles
 * the compose-layer case generically, for any wallpaper).
 *
 * LWE has no projective-passthrough implementation, so it drew that framebuffer re-sample as
 * vertical streaks (the visible "reflection bug"). We therefore render the compose layer as a
 * no-op: the streaks disappear and the children keep rendering correctly. This is generic —
 * it keys only off the composelayer.json model, never off a specific wallpaper/object id.
 */
class CComposeLayer final : public CObject {
public:
    CComposeLayer (Wallpapers::CScene& scene, const Object& object);

    void render () override;
};
} // namespace WallpaperEngine::Render::Objects
