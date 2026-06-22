#pragma once

#include <mutex>
#include <optional>

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

    // Serializes the scene's object graph to JSON for the debug inspector: id, name, type,
    // parent, the live transform/visibility values and effect names. Read-only; safe to
    // call from the IPC thread (it only reads data-model values the render thread updates).
    [[nodiscard]] std::string toInspectorJSON () const;

    // ── Debug inspector live controls (called from the IPC thread) ──────────────
    // Isolate a single object (only it renders); std::nullopt clears the filter.
    void debugIsolate (std::optional<int> id) const;
    // Hide / show one object (skipObjects list).
    void debugSetHidden (int id, bool hidden) const;
    // Clear isolate + all hidden flags.
    void debugClear () const;
    // Live-edit one object's transform for testing. prop ∈
    // {origin, scale, angle, alpha, visible}. vals carries 1 or 3 floats depending
    // on the prop. Returns false if the id or prop is unknown. The change sticks
    // until the wallpaper reloads, except for scripted values (clock/date text),
    // which the script overwrites each frame.
    bool debugEditObject (int id, const std::string& prop, const float* vals, int count) const;

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
    void collectScriptedValues ();
    void registerScriptedValue (const UserSettingUniquePtr& setting);
    void updateScriptedValues ();

    // Looks up a data-model object by id (nullptr if not found). Used by the debug
    // live-edit path; returns a const ref because mutation goes through the
    // DynamicValue (whose update() is non-const even via a const UserSetting).
    [[nodiscard]] const Object* findObjectData (int id) const;

    std::unique_ptr<Camera> m_camera;
    glm::ivec2 m_outputSize = {0, 0};
    // Guards the debug objectFilter/skipObjects settings against the render loop,
    // which reads them every frame while the IPC thread mutates them.
    mutable std::mutex m_debugMutex;
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
