#pragma once

#include <string>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "WallpaperEngine/Render/Helpers/ContextAware.h"

#include "WallpaperEngine/Render/Wallpapers/CScene.h"

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render {
class CObject : public Helpers::ContextAware {
public:
    template <class T> [[nodiscard]] const T* as () const {
	if (is<T> ()) {
	    return static_cast<const T*> (this);
	}

	throw std::bad_cast ();
    }

    template <class T> [[nodiscard]] T* as () {
	if (is<T> ()) {
	    return static_cast<T*> (this);
	}

	throw std::bad_cast ();
    }

    template <class T> [[nodiscard]] bool is () const { return typeid (*this) == typeid (T); }

    CObject (Wallpapers::CScene& scene, const Object& object);
    virtual ~CObject () override = default;

    virtual void render ();

    [[nodiscard]] Wallpapers::CScene& getScene () const;
    [[nodiscard]] const AssetLocator& getAssetLocator () const;
    [[nodiscard]] int getId () const;
    [[nodiscard]] const Object& getObject () const;

    // Absolute (parent-composed) transform of an object. WE objects can be parented
    // (e.g. a clock/date text nested under a chalkboard group); their own origin/scale/
    // angle are relative to the parent. Walks the parent chain and composes the
    // transforms so renderers place nested objects correctly. Shared by CImage and
    // CText so both honour parenting identically.
    struct ResolvedTransform {
	glm::vec3 origin;
	glm::vec3 scale;
	float angle;
    };
    [[nodiscard]] ResolvedTransform resolveTransform (const Object& object, int depth = 0) const;

private:
    Wallpapers::CScene& m_scene;
    const Object& m_object;
};
} // namespace WallpaperEngine::Render