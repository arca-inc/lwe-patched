#include "CObject.h"

#include <cmath>
#include <utility>

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Render::Wallpapers;
using namespace WallpaperEngine::Data::Model;

namespace {
glm::vec2 rotateVec2 (const glm::vec2& value, const float angle) {
    const float cosAngle = std::cos (angle);
    const float sinAngle = std::sin (angle);
    return { value.x * cosAngle - value.y * sinAngle, value.x * sinAngle + value.y * cosAngle };
}
} // namespace

CObject::CObject (Wallpapers::CScene& scene, const Object& object) :
    Helpers::ContextAware (scene), m_scene (scene), m_object (object) { }

void CObject::render () { }

Wallpapers::CScene& CObject::getScene () const { return this->m_scene; }

const AssetLocator& CObject::getAssetLocator () const { return this->getScene ().getAssetLocator (); }

int CObject::getId () const { return this->m_object.id; }

const Object& CObject::getObject () const { return this->m_object; }

CObject::ResolvedTransform CObject::resolveTransform (const Object& object, const int depth) const {
    constexpr int kMaxParentDepth = 32;

    // Inside a compose layer's child render, stop the parent walk at the layer: its own
    // origin/scale/angle is applied when the composited buffer is drawn, so from a child's
    // point of view the layer is the identity origin of the buffer's local space.
    if (object.id == this->getScene ().getComposeStopId ()) {
	return { glm::vec3 (0.0f), glm::vec3 (1.0f), 0.0f };
    }

    glm::vec3 origin = object.origin->value->getVec3 ();
    glm::vec3 scale = glm::vec3 (1.0f);
    float angle = 0.0f;

    // A compose layer is an Image by type but behaves as a group: its transform lives in
    // the group fields (groupScale/groupAngles), not the image scale/angles. Use those so
    // the composited buffer is placed and scaled correctly.
    const bool composeLayer = object.is<Image> () && object.as<Image> ()->model != nullptr
	&& object.as<Image> ()->model->filename.find ("composelayer") != std::string::npos;

    if (object.is<Image> () && !composeLayer) {
	const auto* image = object.as<Image> ();
	scale = image->scale->value->getVec3 ();
	angle = image->angles->value->getVec3 ().z;
    } else if (object.is<Text> ()) {
	const auto* text = object.as<Text> ();
	scale = text->scale->value->getVec3 ();
    } else {
	scale = object.groupScale->value->getVec3 ();
	angle = object.groupAngles->value->getVec3 ().z;
    }

    if (!object.parent.has_value ()) {
	return { origin, scale, angle };
    }

    if (depth >= kMaxParentDepth) {
	sLog.error ("Parent transform chain is too deep; possible cycle at object id=", object.id);
	return { origin, scale, angle };
    }

    const auto* parentObject = this->getScene ().getObject (object.parent.value ());
    if (parentObject == nullptr) {
	return { origin, scale, angle };
    }

    const auto& parent = parentObject->getObject ();
    const auto parentTransform = this->resolveTransform (parent, depth + 1);
    const glm::vec2 local = rotateVec2 ({ origin.x * parentTransform.scale.x, origin.y * parentTransform.scale.y }, parentTransform.angle);
    origin.x = parentTransform.origin.x + local.x;
    origin.y = parentTransform.origin.y + local.y;
    origin.z = parentTransform.origin.z + origin.z * parentTransform.scale.z;
    scale *= parentTransform.scale;
    angle += parentTransform.angle;

    return { origin, scale, angle };
}