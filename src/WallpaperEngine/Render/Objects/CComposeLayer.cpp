#include "CComposeLayer.h"

using namespace WallpaperEngine::Render::Objects;

CComposeLayer::CComposeLayer (Wallpapers::CScene& scene, const Object& object) : CObject (scene, object) {}

void CComposeLayer::render () {
    // No-op — see CComposeLayer.h. WE's compose layer is a projective framebuffer-resample
    // (reflection) that LWE renders as streaks; we skip it. The children render themselves in
    // normal scene order and inherit this layer's groupScale/groupAngle via resolveTransform.
}
