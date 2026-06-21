#pragma once

#include <GL/glew.h>
#include <glm/vec2.hpp>
#include <vector>

#include "WallpaperEngine/Render/CFBO.h"
#include "WallpaperEngine/Render/CObject.h"

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render::Objects {
/**
 * Wallpaper Engine "compose layer" (model models/util/composelayer.json).
 *
 * In WE a compose layer renders its child objects into an off-screen buffer at the
 * layer's native size, then draws that buffer using the layer's own transform. The
 * children are laid out in the buffer's local space, so the layer's (often non-uniform)
 * scale stretches the *composite* as a whole instead of every child individually.
 *
 * LWE previously had no compose support: it flattened children into the scene and let
 * each one inherit the layer's scale multiplicatively, which distorted nested groups
 * (e.g. the cafe chalkboard polaroid came out stretched into a wide smear). This object
 * restores the correct behaviour: it owns an FBO, renders its assigned children into it
 * with a local projection (see Camera::pushLocalProjection + CScene::setComposeStopId),
 * then draws the FBO as a single textured quad with the layer's resolved transform.
 */
class CComposeLayer final : public CObject {
public:
    CComposeLayer (Wallpapers::CScene& scene, const Object& object);
    ~CComposeLayer () override;

    // Children assigned to this layer (those whose nearest compose ancestor is this
    // layer), in scene render order. Populated by CScene after all objects are created.
    void setChildren (std::vector<CObject*> children);

    void render () override;

private:
    void ensureGL ();
    void renderChildrenToBuffer ();
    void drawBuffer ();

    std::vector<CObject*> m_children = {};
    std::shared_ptr<CFBO> m_fbo = nullptr;
    glm::vec2 m_nativeSize = {0.0f, 0.0f};

    GLuint m_program = 0;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    GLint m_uMVP = -1;
    GLint m_uTexture = -1;
    GLint m_uAlpha = -1;
    bool m_glReady = false;
};
} // namespace WallpaperEngine::Render::Objects
