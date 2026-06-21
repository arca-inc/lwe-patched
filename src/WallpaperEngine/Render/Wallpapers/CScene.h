#pragma once

#include "WallpaperEngine/Render/Camera.h"

#include "WallpaperEngine/Data/Model/ScriptedDynamicValue.h"
#include "WallpaperEngine/Render/CWallpaper.h"

namespace WallpaperEngine::Render {
class Camera;
class CObject;
}

namespace WallpaperEngine::Render::Wallpapers {
using namespace WallpaperEngine::Data::Model;

class CScene final : public CWallpaper {
public:
    CScene (
	const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
	const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
    );

    ~CScene () override;

    [[nodiscard]] Camera& getCamera () const;

    // Real output viewport size in pixels (the scene FBO is scene-resolution and
    // gets scaled to this). Used by CText to size glyphs in output pixels.
    [[nodiscard]] glm::ivec2 getOutputSize () const;
    void setOutputSize (const glm::ivec2& size) { this->m_outputSize = size; }

    // While a compose layer renders its children into its own buffer, this is the layer's
    // object id. resolveTransform stops the parent walk here so children sit in the buffer's
    // local space (the layer's own origin/scale is applied when the buffer is drawn, not
    // baked into every child). -1 when not composing.
    [[nodiscard]] int getComposeStopId () const { return this->m_composeStopId; }
    void setComposeStopId (const int id) { this->m_composeStopId = id; }

    [[nodiscard]] const Scene& getScene () const;

    [[nodiscard]] int getWidth () const override;
    [[nodiscard]] int getHeight () const override;

    // Time accessors used by dynamic text layers (CText + ScriptEngine).
    // Read from the application-wide g_Time/g_TimeLast globals that other
    // renderers already consume via extern (e.g. CParticle).
    [[nodiscard]] float getTime () const;
    [[nodiscard]] float getDeltaTime () const;
    [[nodiscard]] float getFps () const;

    const glm::vec2* getMousePosition () const;
    const glm::vec2* getMousePositionLast () const;
    const glm::vec2* getMousePositionNormalized () const;
    const glm::vec2* getParallaxDisplacement () const;

    [[nodiscard]] const std::vector<CObject*>& getObjectsByRenderOrder () const;
    [[nodiscard]] const CObject* getObject (int id) const;

protected:
    void renderFrame (const glm::ivec4& viewport) override;
    void updateMouse (const glm::ivec4& viewport);

    friend class CWallpaper;

private:
    Render::CObject* createObject (const Object& object);
    Render::CObject* dispatchObjectType (const Object& object);
    void addObjectToRenderOrder (const Object& object);
    // Moves each compose layer's descendants out of the flat render order and into the
    // owning layer, so the layer renders them into its own buffer (see CComposeLayer).
    void buildComposeGroups ();
    void collectScriptedValues ();
    void registerScriptedValue (const UserSettingUniquePtr& setting);
    void updateScriptedValues ();

    std::unique_ptr<Camera> m_camera;
    glm::ivec2 m_outputSize = {0, 0};
    int m_composeStopId = -1;
    ObjectUniquePtr m_bloomObjectData;
    CObject* m_bloomObject = nullptr;
    std::map<int, CObject*> m_objects = {};
    std::vector<CObject*> m_objectsByRenderOrder = {};
    std::vector<ScriptedDynamicValue*> m_scriptedValues = {};
    glm::vec2 m_mousePosition = {};
    glm::vec2 m_mousePositionLast = {};
    glm::vec2 m_mousePositionNormalized = {};
    glm::vec2 m_parallaxDisplacement = {};
    std::shared_ptr<const CFBO> _rt_4FrameBuffer = nullptr;
    std::shared_ptr<const CFBO> _rt_8FrameBuffer = nullptr;
    std::shared_ptr<const CFBO> _rt_Bloom = nullptr;
    std::shared_ptr<const CFBO> _rt_shadowAtlas = nullptr;
};
} // namespace WallpaperEngine::Render::Wallpaper
