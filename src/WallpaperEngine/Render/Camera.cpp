#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;

Camera::Camera (Wallpapers::CScene& scene, const SceneData::Camera& camera) :
    m_width (0), m_height (0), m_camera (camera), m_scene (scene) {
    // get the lookat position
    // TODO: ENSURE THIS IS ONLY USED WHEN NOT DOING AN ORTOGRAPHIC CAMERA AS IT THROWS OFF POINTS
    this->m_lookat = glm::lookAt (this->getEye (), this->getCenter (), this->getUp ());
}

Camera::~Camera () = default;

const glm::vec3& Camera::getCenter () const { return this->m_camera.configuration.center; }

const glm::vec3& Camera::getEye () const { return this->m_camera.configuration.eye; }

const glm::vec3& Camera::getUp () const { return this->m_camera.configuration.up; }

const glm::mat4& Camera::getProjection () const { return this->m_projection; }

const glm::mat4& Camera::getLookAt () const { return this->m_lookat; }

bool Camera::isOrthogonal () const { return this->m_isOrthogonal; }

Wallpapers::CScene& Camera::getScene () const { return this->m_scene; }

float Camera::getWidth () const { return this->m_width; }

float Camera::getHeight () const { return this->m_height; }

float Camera::getFov () const { return this->m_camera.projection.fov; }

float Camera::getNearZ () const { return this->m_camera.projection.nearz; }

float Camera::getFarZ () const { return this->m_camera.projection.farz; }

void Camera::setOrthogonalProjection (const float width, const float height) {
    this->m_width = width;
    this->m_height = height;

    float nearz = this->m_camera.projection.nearz;
    float farz = this->m_camera.projection.farz;

    this->m_projection = glm::ortho<float> (-width / 2.0, width / 2.0, -height / 2.0, height / 2.0, nearz, farz);
    this->m_projection = glm::translate (this->m_projection, this->getEye ());
    this->m_isOrthogonal = true;
}

void Camera::pushLocalProjection (const float width, const float height) {
    this->m_projectionStack.push_back ({this->m_width, this->m_height, this->m_projection});

    const float nearz = this->m_camera.projection.nearz;
    const float farz = this->m_camera.projection.farz;
    // Compose children are positioned relative to the buffer centre, but CImage/CText still
    // compute their gl position as `origin - size/2` (the scene's bottom-left→centred
    // mapping). Feeding that through a plain centred ortho double-shifts them to the buffer
    // edge — that was the real "children render transparent" symptom (content off-buffer,
    // centre empty). ortho(-W,0, H,0) cancels the extra -W/2 / +H/2 shift exactly, so a
    // child at local (0,0) lands at the buffer centre. (NDC = 2*local/size.)
    this->m_width = width;
    this->m_height = height;
    this->m_projection = glm::ortho<float> (-width, 0.0f, height, 0.0f, nearz, farz);
}

void Camera::popLocalProjection () {
    if (this->m_projectionStack.empty ()) {
	return;
    }
    const auto& saved = this->m_projectionStack.back ();
    this->m_width = saved.width;
    this->m_height = saved.height;
    this->m_projection = saved.projection;
    this->m_projectionStack.pop_back ();
}