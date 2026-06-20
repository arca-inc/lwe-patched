#pragma once

#include <memory>
#include <string>
#include <vector>

#include <GL/glew.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"

// Forward-declare FreeType types to avoid leaking the header into users.
struct FT_LibraryRec_;
struct FT_FaceRec_;
typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render {
class TextureProvider;
}

namespace WallpaperEngine::Data::Model {
class DynamicValue;
}

namespace WallpaperEngine::Render::Objects {
using namespace WallpaperEngine::Data::Model;

/**
 * Phase 1 text renderer.
 *
 * Renders static text objects as a single FreeType-rasterized RGBA texture
 * drawn on a textured quad with its own minimal GLSL shader. Does NOT go
 * through CRenderable / materials / passes — Phase 1 does not need effects.
 *
 * Phase 2 (scripted/dynamic text, alignment from properties, effect passes)
 * is intentionally not implemented here. When the scene provides a dynamic
 * `text: { script: "..." }` object this class captures the script source in
 * the data model but renders an empty string — the Wallpaper Engine JS
 * runtime required to evaluate it is out of scope for Phase 1.
 */
class CText final : public CObject {
public:
    CText (Wallpapers::CScene& scene, const Text& text);
    ~CText () override;

    void setup ();
    void render () override;

private:
    // Rebuilds the glyph texture (and matching quad VBO) from the given string.
    // Reuses existing GL handles if already allocated, so this is safe to call
    // every time the rendered text changes.
    void rebuildTextureFrom (const std::string& text);
    void buildShader ();
    void uploadQuadVertices ();

    // setup() helpers (kept small to keep the setup flow linear).
    bool initFreeType ();
    bool loadEmbeddedFont ();
    bool loadSystemFont ();
    unsigned int computeEffectivePixelSize () const;
    void initScriptLayer ();
    // Detects the WE "clouds" effect (the two-colour text tint) on this object and
    // caches its parameters so render() can reproduce it inline in the text shader.
    void detectCloudsEffect ();

    const Text& m_text;
    std::string m_lastRenderedText;
    Scripting::ScriptLayerHandle m_layerHandle = Scripting::kInvalidLayerHandle;

    FT_Library m_ftLibrary = nullptr;
    FT_Face m_ftFace = nullptr;
    std::vector<uint8_t> m_fontData;

    GLuint m_texture = 0;
    GLuint m_program = 0;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;

    GLint m_uMVP = -1;
    GLint m_uColor = -1;
    GLint m_uTexture = -1;

    // Inline "clouds" two-colour effect (timecolor ↔ timecolor2 cloudy blend).
    bool m_hasClouds = false;
    glm::vec3 m_cloudColor2 = {0.0f, 0.0f, 0.0f};
    // Live source of the second colour (bound to e.g. timecolor2). Read every
    // frame so colour changes pushed by the host (hot-swap --set-property) apply.
    const DynamicValue* m_cloudColor2Value = nullptr;
    glm::vec2 m_cloudSpeed = {0.0f, -0.01f};
    glm::vec2 m_cloudScale = {1.0f, 1.0f};
    float m_cloudThreshold = 0.0f;
    float m_cloudFeather = 0.5f;
    std::shared_ptr<const TextureProvider> m_cloudTexture = nullptr;
    GLint m_uColor2 = -1;
    GLint m_uClouds = -1;
    GLint m_uTime = -1;
    GLint m_uCloudSpeed = -1;
    GLint m_uCloudScale = -1;
    GLint m_uThreshold = -1;
    GLint m_uFeather = -1;
    GLint m_uHasClouds = -1;

    glm::ivec2 m_textureSize = {0, 0};
    glm::vec2 m_quadSize = {0.0f, 0.0f};

    // FreeType pixel size the glyph texture is currently rasterized at. WE sizes
    // text in *output* pixels, but the scene renders into a scene-resolution FBO
    // that is scaled down to the output; render() rasterizes at pointsize *
    // (sceneHeight / outputHeight) so glyphs land at their intended on-screen size
    // (and stay crisp instead of being upscaled). Re-rasterized when this changes.
    unsigned int m_ftPixelSize = 0;

    bool m_valid = false;
};
} // namespace WallpaperEngine::Render::Objects
