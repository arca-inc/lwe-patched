#pragma once

#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <vector>

#include "WallpaperEngine/Data/Model/Wallpaper.h"

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render {
using namespace WallpaperEngine::Data::Model;

class Camera {
public:
    Camera (Wallpapers::CScene& scene, const SceneData::Camera& camera);
    ~Camera ();

    void setOrthogonalProjection (const float width, const float height);

    /**
     * Temporarily replaces the projection with a centred orthographic one of the given
     * size, used while rendering a compose layer's children into its own buffer (their
     * local coordinate space is the buffer, not the scene). Nestable; popLocalProjection
     * restores the previous projection. getProjection/getWidth/getHeight follow the
     * active (top-of-stack) projection so dependent code (CImage/CText) renders into the
     * buffer at native size.
     */
    void pushLocalProjection (float width, float height);
    void popLocalProjection ();

    [[nodiscard]] const glm::vec3& getCenter () const;
    [[nodiscard]] const glm::vec3& getEye () const;
    [[nodiscard]] const glm::vec3& getUp () const;
    [[nodiscard]] const glm::mat4& getProjection () const;
    [[nodiscard]] const glm::mat4& getLookAt () const;
    [[nodiscard]] Wallpapers::CScene& getScene () const;
    [[nodiscard]] bool isOrthogonal () const;
    [[nodiscard]] float getWidth () const;
    [[nodiscard]] float getHeight () const;
    [[nodiscard]] float getFov () const;
    [[nodiscard]] float getNearZ () const;
    [[nodiscard]] float getFarZ () const;

private:
    struct SavedProjection {
	float width;
	float height;
	glm::mat4 projection;
    };
    std::vector<SavedProjection> m_projectionStack = {};

    float m_width;
    float m_height;
    bool m_isOrthogonal = false;
    glm::mat4 m_projection = {};
    glm::mat4 m_lookat = {};
    const SceneData::Camera& m_camera;
    Wallpapers::CScene& m_scene;
};
} // namespace WallpaperEngine::Render
