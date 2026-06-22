#include "WallpaperEngine/Render/Objects/CComposeLayer.h"
#include "WallpaperEngine/Render/Objects/CImage.h"
#include "WallpaperEngine/Render/Objects/CParticle.h"
#include "WallpaperEngine/Render/Objects/CSound.h"
#include "WallpaperEngine/Render/Objects/CText.h"

#include "WallpaperEngine/Render/WallpaperState.h"

#include "CScene.h"
#include "WallpaperEngine/Logging/Log.h"

#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"

#include <ranges>

extern float g_Time;
extern float g_TimeLast;

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Render::Wallpapers;
using JSON = WallpaperEngine::Data::JSON::JSON;

namespace {
Render::CObject* createImageObject (CScene& scene, const Image& imageData) {
    auto* image = new Objects::CImage (scene, imageData);
    try {
	image->setup ();
    } catch (std::runtime_error& e) {
	sLog.error ("Cannot setup image ", image->getImage ().name, ": ", e.what ());
	delete image;
	return nullptr;
    }
    return image;
}

// A compose layer is an image whose model is WE's util/composelayer.json: it groups and
// composites its children into a buffer instead of drawing anything itself.
bool isComposeLayer (const Object& object) {
    if (!object.is<Image> ()) {
	return false;
    }
    const auto* image = object.as<Image> ();
    if (image->model == nullptr) {
	return false;
    }
    return image->model->filename.find ("composelayer") != std::string::npos;
}

Render::CObject* createParticleObject (CScene& scene, const Particle& particleData) {
    if (scene.getContext ().getApp ().getContext ().settings.general.disableParticles == true) {
	sLog.debug ("Ignoring particle system (disabled in settings): ", particleData.name);
	return nullptr;
    }

    auto* particle = new Objects::CParticle (scene, particleData);
    try {
	particle->setup ();
    } catch (std::runtime_error&) {
	sLog.error ("Cannot setup particle ", particle->getParticle ().name);
	delete particle;
	return nullptr;
    }
    return particle;
}
}

CScene::CScene (
    const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
    const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
) : CWallpaper (wallpaper, context, audioContext, scalingMode, clampMode) {
    // caller should check this, if not a std::bad_cast is good to throw
    auto scene = wallpaper.as<Scene> ();

    // setup the scene camera
    this->m_camera = std::make_unique<Camera> (*this, scene->camera);

    float width = scene->camera.projection.width;
    float height = scene->camera.projection.height;

    // detect size if the orthogonal project is auto
    if (scene->camera.projection.isAuto) {
	glm::vec2 maxExtent = { 0.0f, 0.0f };

	for (const auto& object : scene->objects) {
	    if (!object->is<Image> ()) {
		continue;
	    }

	    const auto* image = object->as<Image> ();
	    if (!image->origin || !image->origin->value) {
		continue;
	    }

	    const glm::vec3 origin = image->origin->value->getVec3 ();
	    const glm::vec2 halfSize = image->size / 2.0f;

	    maxExtent.x = glm::max (maxExtent.x, glm::abs (origin.x) + halfSize.x);
	    maxExtent.y = glm::max (maxExtent.y, glm::abs (origin.y) + halfSize.y);
	}

	if (maxExtent.x > 0.0f && maxExtent.y > 0.0f) {
	    width = maxExtent.x * 2.0f;
	    height = maxExtent.y * 2.0f;
	} else {
	    width = this->getContext ().getOutput ().getFullWidth ();
	    height = this->getContext ().getOutput ().getFullHeight ();
	    sLog.debug ("Auto projection: falling back to screen resolution ", width, "x", height);
	}
    }

    this->m_parallaxDisplacement = { 0, 0 };

    // TODO: CONVERSION
    this->m_camera->setOrthogonalProjection (width, height);

    // setup framebuffers here as they're required for the scene setup
    this->setupFramebuffers ();

    const uint32_t sceneWidth = this->m_camera->getWidth ();
    const uint32_t sceneHeight = this->m_camera->getHeight ();

    this->_rt_shadowAtlas = this->create (
	"_rt_shadowAtlas", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth, sceneHeight },
	{ sceneWidth, sceneHeight }
    );
    this->alias ("_alias_lightCookie", "_rt_shadowAtlas");

    // set clear color
    const glm::vec3 clearColor = scene->colors.clear->value->getVec3 ();

    glClearColor (clearColor.r, clearColor.g, clearColor.b, 1.0f);

    // create all objects based off their dependencies
    for (const auto& object : scene->objects) {
	this->createObject (*object);
    }

    // copy over objects by render order
    for (const auto& object : scene->objects) {
	this->addObjectToRenderOrder (*object);
    }

    this->collectScriptedValues ();

    // create extra framebuffers for the bloom effect
    this->_rt_4FrameBuffer = this->create (
	"_rt_4FrameBuffer", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth / 4, sceneHeight / 4 },
	{ sceneWidth / 4, sceneHeight / 4 }
    );
    this->_rt_8FrameBuffer = this->create (
	"_rt_8FrameBuffer", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth / 8, sceneHeight / 8 },
	{ sceneWidth / 8, sceneHeight / 8 }
    );
    this->_rt_Bloom = this->create (
	"_rt_Bloom", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth / 8, sceneHeight / 8 },
	{ sceneWidth / 8, sceneHeight / 8 }
    );

    //
    // Had to get a little creative with the effects to achieve the same bloom effect without any custom code
    // this custom image loads some effect files from the virtual container to achieve the same bloom effect
    // this approach requires of two extra draw calls due to the way the effect works in official WPE
    // (it renders directly to the screen, whereas here we never do that from a scene)
    //

    const auto bloomOrigin = glm::vec3 { sceneWidth / 2, sceneHeight / 2, 0.0f };
    const auto bloomSize = glm::vec2 { sceneWidth, sceneHeight };

    const JSON bloom
	= { { "image", "models/wpenginelinux.json" },
	    { "name", "bloomimagewpenginelinux" },
	    { "visible", true },
	    { "scale", "1.0 1.0 1.0" },
	    { "angles", "0.0 0.0 0.0" },
	    { "origin",
	      std::to_string (bloomOrigin.x) + " " + std::to_string (bloomOrigin.y) + " "
		  + std::to_string (bloomOrigin.z) },
	    { "size", std::to_string (bloomSize.x) + " " + std::to_string (bloomSize.y) },
	    { "id", -1 },
	    { "effects",
	      JSON::array (
		  { { { "file", "effects/wpenginelinux/bloomeffect.json" },
		      { "id", 15242000 },
		      { "name", "" },
		      { "passes",
			JSON::array (
			    { { { "constantshadervalues",
				  { { "bloomstrength", this->getScene ().camera.bloom.strength->value->getFloat () },
				    { "bloomthreshold",
				      this->getScene ().camera.bloom.threshold->value->getFloat () } } } },
			      { { "constantshadervalues",
				  { { "bloomstrength", this->getScene ().camera.bloom.strength->value->getFloat () },
				    { "bloomthreshold",
				      this->getScene ().camera.bloom.threshold->value->getFloat () } } } },
			      { { "constantshadervalues",
				  { { "bloomstrength", this->getScene ().camera.bloom.strength->value->getFloat () },
				    { "bloomthreshold",
				      this->getScene ().camera.bloom.threshold->value->getFloat () } } } } }
			) } } }
	      ) } };

    // create image for bloom passes
    if (scene->camera.bloom.enabled->value->getBool ()) {
	this->m_bloomObjectData = ObjectParser::parse (bloom, scene->project);
	this->m_bloomObject = this->createObject (*this->m_bloomObjectData);

	this->m_objectsByRenderOrder.push_back (this->m_bloomObject);
    }
}

CScene::~CScene () {
    // bloom object is in the objects list, so no need to explicitly delete it
    this->m_bloomObject = nullptr;

    for (const auto& val : this->m_objects | std::views::values) {
	delete val;
    }

    this->m_objectsByRenderOrder.clear ();
    this->m_objects.clear ();
}

Render::CObject* CScene::createObject (const Object& object) {
    Render::CObject* renderObject = nullptr;

    // ensure the item is not loaded already
    if (const auto current = this->m_objects.find (object.id); current != this->m_objects.end ()) {
	return current->second;
    }

    // check dependencies too!
    for (const auto& cur : object.dependencies) {
	// self-dependency is a possibility...
	if (cur == object.id) {
	    continue;
	}

	const auto dep
	    = std::ranges::find_if (this->getScene ().objects, [&cur] (const auto& o) { return o->id == cur; });

	if (dep != this->getScene ().objects.end ()) {
	    this->createObject (**dep);
	}
    }

    // check if the item has any parent and also create it first
    if (object.parent.has_value ()) {
	int parentId = object.parent.value ();

	const auto dep = std::ranges::find_if (this->getScene ().objects, [&parentId] (const auto& o) {
	    return o->id == parentId;
	});

	if (dep == this->getScene ().objects.end ()) {
	    sLog.exception ("Cannot find parent ", parentId, " for object ", object.id);
	}

	this->createObject (**dep);
    }

    renderObject = this->dispatchObjectType (object);

    if (renderObject != nullptr) {
	this->m_objects.emplace (renderObject->getId (), renderObject);
    }

    return renderObject;
}

Render::CObject* CScene::dispatchObjectType (const Object& object) {
    Render::CObject* renderObject = nullptr;

    if (isComposeLayer (object)) {
	renderObject = new Objects::CComposeLayer (*this, object);
    } else if (object.is<Image> ()) {
	renderObject = createImageObject (*this, *object.as<Image> ());
    } else if (object.is<Sound> ()) {
	renderObject = new Objects::CSound (*this, *object.as<Sound> ());
    } else if (object.is<Text> ()) {
	auto* text = new Objects::CText (*this, *object.as<Text> ());
	try {
	    text->setup ();
	} catch (std::runtime_error&) {
	    sLog.error ("Cannot setup text ", text->getObject ().name);
	    delete text;
	    return nullptr;
	}
	renderObject = text;
    } else if (object.is<Particle> ()) {
	renderObject = createParticleObject (*this, *object.as<Particle> ());
    } else {
	sLog.debug ("Unknown object type, creating placeholder, empty object: ", object.id);
	renderObject = new CObject (*this, object);
    }

    return renderObject;
}

void CScene::addObjectToRenderOrder (const Object& object) {
    const auto obj = this->m_objects.find (object.id);

    // ignores not created objects like particle systems
    if (obj == this->m_objects.end ()) {
	return;
    }

    // take into account any dependency first
    for (const auto& dep : object.dependencies) {
	// self-dependency is possible
	if (dep == object.id) {
	    continue;
	}

	// add the dependency to the list if it's created
	auto depIt = std::ranges::find_if (this->getScene ().objects, [&dep] (const auto& o) { return o->id == dep; });

	if (depIt != this->getScene ().objects.end ()) {
	    this->addObjectToRenderOrder (**depIt);
	} else {
	    sLog.error ("Cannot find dependency ", dep, " for object ", object.id);
	}
    }

    // ensure we're added only once to the render list
    const auto renderIt = std::ranges::find_if (this->m_objectsByRenderOrder, [&object] (const auto& o) {
	return o->getId () == object.id;
    });

    if (renderIt == this->m_objectsByRenderOrder.end ()) {
	this->m_objectsByRenderOrder.emplace_back (obj->second);
    }
}

void CScene::registerScriptedValue (const UserSettingUniquePtr& setting) {
    if (!setting || !setting->value) {
	return;
    }

    auto* scripted = dynamic_cast<ScriptedDynamicValue*> (setting->value.get ());
    if (!scripted) {
	return;
    }

    if (std::ranges::find (this->m_scriptedValues, scripted) == this->m_scriptedValues.end ()) {
	this->m_scriptedValues.emplace_back (scripted);
    }
}

void CScene::collectScriptedValues () {
    this->m_scriptedValues.clear ();

    for (const auto& object : this->getScene ().objects) {
	this->registerScriptedValue (object->origin);
	this->registerScriptedValue (object->groupScale);
	this->registerScriptedValue (object->groupAngles);
	this->registerScriptedValue (object->groupVisible);

	if (object->is<Image> ()) {
	    const auto* image = object->as<Image> ();
	    this->registerScriptedValue (image->scale);
	    this->registerScriptedValue (image->angles);
	    this->registerScriptedValue (image->visible);
	    this->registerScriptedValue (image->alpha);
	    this->registerScriptedValue (image->color);
	    this->registerScriptedValue (image->parallaxDepth);

	    for (const auto& effect : image->effects) {
		this->registerScriptedValue (effect->visible);
		for (const auto& pass : effect->passOverrides) {
		    for (const auto& constant : pass->constants | std::views::values) {
			this->registerScriptedValue (constant);
		    }
		}
	    }
	} else if (object->is<Particle> ()) {
	    const auto* particle = object->as<Particle> ();
	    this->registerScriptedValue (particle->scale);
	    this->registerScriptedValue (particle->angles);
	    this->registerScriptedValue (particle->visible);
	    this->registerScriptedValue (particle->parallaxDepth);
	} else if (object->is<Text> ()) {
	    const auto* text = object->as<Text> ();
	    this->registerScriptedValue (text->visible);
	    this->registerScriptedValue (text->color);
	    this->registerScriptedValue (text->alpha);
	    this->registerScriptedValue (text->scale);
	}
    }
}

void CScene::updateScriptedValues () {
    for (const auto& scripted : this->m_scriptedValues) {
	scripted->reevaluate (this);
    }
}

Camera& CScene::getCamera () const { return *this->m_camera; }

glm::ivec2 CScene::getOutputSize () const { return this->m_outputSize; }

std::string CScene::toInspectorJSON () const {
    const auto vec3 = [] (const UserSettingUniquePtr& s) -> JSON {
	if (s && s->value) {
	    const glm::vec3 v = s->value->getVec3 ();
	    return JSON::array ({v.x, v.y, v.z});
	}
	return JSON::array ({0.0f, 0.0f, 0.0f});
    };
    const auto boolOf = [] (const UserSettingUniquePtr& s, const bool def) -> bool {
	return (s && s->value) ? s->value->getBool () : def;
    };
    const auto floatOf = [] (const UserSettingUniquePtr& s, const float def) -> float {
	return (s && s->value) ? s->value->getFloat () : def;
    };

    JSON objects = JSON::array ();
    for (const auto& objPtr : this->getScene ().objects) {
	const Object& obj = *objPtr;
	JSON jo;
	jo["id"] = obj.id;
	jo["name"] = obj.name;
	if (obj.parent.has_value ()) {
	    jo["parent"] = obj.parent.value ();
	} else {
	    jo["parent"] = nullptr;
	}
	jo["origin"] = vec3 (obj.origin);

	if (obj.is<Image> ()) {
	    const auto* img = obj.as<Image> ();
	    const bool compose = img->model != nullptr && img->model->filename.find ("composelayer") != std::string::npos;
	    jo["type"] = compose ? "compose" : "image";
	    jo["scale"] = vec3 (img->scale);
	    // Compose layers behave as groups: their real transform lives in groupScale/
	    // groupAngles, not image->scale. Surface both so the inspector shows whether
	    // the group scale (which resolveTransform currently ignores) is non-trivial.
	    if (compose) {
		jo["groupScale"] = vec3 (obj.groupScale);
		jo["groupAngle"] = (obj.groupAngles && obj.groupAngles->value)
		    ? obj.groupAngles->value->getVec3 ().z : 0.0f;
	    }
	    jo["angle"] = (img->angles && img->angles->value) ? img->angles->value->getVec3 ().z : 0.0f;
	    jo["visible"] = boolOf (img->visible, true);
	    jo["alpha"] = floatOf (img->alpha, 1.0f);
	    jo["size"] = JSON::array ({img->size.x, img->size.y});
	    jo["model"] = img->model != nullptr ? img->model->filename : "";
	    JSON effects = JSON::array ();
	    for (const auto& e : img->effects) {
		if (e != nullptr) {
		    effects.push_back (e->name.empty () ? std::string ("(effect)") : e->name);
		}
	    }
	    jo["effects"] = effects;
	} else if (obj.is<Text> ()) {
	    const auto* txt = obj.as<Text> ();
	    jo["type"] = "text";
	    jo["scale"] = vec3 (txt->scale);
	    jo["visible"] = boolOf (txt->visible, true);
	    jo["alpha"] = floatOf (txt->alpha, 1.0f);
	    jo["size"] = JSON::array ({txt->size.x, txt->size.y});
	    std::string currentText = txt->text;
	    if (const auto* cobj = this->getObject (obj.id)) {
		if (const auto* ctxt = dynamic_cast<const Objects::CText*> (cobj)) {
		    currentText = ctxt->getLastRenderedText ();
		}
	    }
	    jo["text"] = currentText;
	    jo["scripted"] = !txt->script.empty ();
	} else if (obj.is<Sound> ()) {
	    jo["type"] = "sound";
	} else if (obj.is<Particle> ()) {
	    jo["type"] = "particle";
	    jo["scale"] = vec3 (obj.groupScale);
	    jo["visible"] = boolOf (obj.groupVisible, true);
	} else {
	    jo["type"] = "object";
	    jo["scale"] = vec3 (obj.groupScale);
	    jo["visible"] = boolOf (obj.groupVisible, true);
	}
	objects.push_back (std::move (jo));
    }

    JSON root;
    root["scene"] = { { "width", this->getWidth () }, { "height", this->getHeight () } };
    root["objects"] = std::move (objects);
    return root.dump ();
}

void CScene::debugIsolate (std::optional<int> id) const {
    std::lock_guard<std::mutex> lk (this->m_debugMutex);
    this->getContext ().getApp ().getContext ().settings.render.debug.objectFilter = id;
}

void CScene::debugSetHidden (int id, bool hidden) const {
    std::lock_guard<std::mutex> lk (this->m_debugMutex);
    auto& skip = this->getContext ().getApp ().getContext ().settings.render.debug.skipObjects;
    const auto it = std::ranges::find (skip, id);
    if (hidden && it == skip.end ()) {
	skip.emplace_back (id);
    } else if (!hidden && it != skip.end ()) {
	skip.erase (it);
    }
}

void CScene::debugClear () const {
    std::lock_guard<std::mutex> lk (this->m_debugMutex);
    auto& debug = this->getContext ().getApp ().getContext ().settings.render.debug;
    debug.objectFilter = std::nullopt;
    debug.skipObjects.clear ();
}

const Object* CScene::findObjectData (int id) const {
    for (const auto& objPtr : this->getScene ().objects) {
	if (objPtr->id == id) {
	    return objPtr.get ();
	}
    }
    return nullptr;
}

bool CScene::debugEditObject (int id, const std::string& prop, const float* vals, int count) const {
    const Object* obj = this->findObjectData (id);
    if (obj == nullptr) {
	return false;
    }

    // Resolve the UserSetting backing the requested property for this object's type.
    // const-ness doesn't propagate through unique_ptr, so update() works on a const obj.
    const auto* img = obj->is<Image> () ? obj->as<Image> () : nullptr;
    const auto* txt = obj->is<Text> () ? obj->as<Text> () : nullptr;

    const auto setVec3 = [&] (const UserSettingUniquePtr& s) -> bool {
	if (count < 3 || !s || !s->value) return false;
	s->value->update (glm::vec3 (vals[0], vals[1], vals[2]));
	return true;
    };
    const auto setFloat = [&] (const UserSettingUniquePtr& s) -> bool {
	if (count < 1 || !s || !s->value) return false;
	s->value->update (vals[0]);
	return true;
    };
    const auto setBool = [&] (const UserSettingUniquePtr& s) -> bool {
	if (count < 1 || !s || !s->value) return false;
	s->value->update (vals[0] != 0.0f);
	return true;
    };

    // A compose layer is an Image by type but its transform lives in the group fields
    // (groupScale/groupAngles), which is what its children inherit through resolveTransform.
    // Route scale/angle edits there so the inspector can tune the chalkboard group live.
    const bool compose = isComposeLayer (*obj);

    if (prop == "origin") {
	return setVec3 (obj->origin);
    }
    if (prop == "groupScale") {
	return setVec3 (obj->groupScale);
    }
    if (prop == "scale") {
	if (compose) return setVec3 (obj->groupScale);
	if (img) return setVec3 (img->scale);
	if (txt) return setVec3 (txt->scale);
	return setVec3 (obj->groupScale);
    }
    if (prop == "angle" || prop == "groupAngle") {
	// Single z-rotation: preserve x/y, override z.
	const UserSettingUniquePtr& s = (img && !compose) ? img->angles : obj->groupAngles;
	if (count < 1 || !s || !s->value) return false;
	glm::vec3 a = s->value->getVec3 ();
	a.z = vals[0];
	s->value->update (a);
	return true;
    }
    if (prop == "alpha") {
	if (img) return setFloat (img->alpha);
	if (txt) return setFloat (txt->alpha);
	return false;
    }
    if (prop == "visible") {
	if (img) return setBool (img->visible);
	if (txt) return setBool (txt->visible);
	return setBool (obj->groupVisible);
    }
    return false;
}

void CScene::renderFrame (const glm::ivec4& viewport) {
    // Remember the output viewport size. The scene renders into a scene-resolution
    // FBO (e.g. 3840x2160) that is then scaled to the real output (e.g. 1920x1080);
    // text layers need that ratio to size glyphs in output pixels (see CText).
    this->m_outputSize = {viewport.z, viewport.w};

    // ensure the virtual mouse position is up to date
    this->updateMouse (viewport);

    this->updateScriptedValues ();

    // update the parallax position if required
    if (this->getScene ().camera.parallax.enabled->value->getBool ()
	&& !this->getContext ().getApp ().getContext ().settings.mouse.disableparallax) {
	const float influence = this->getScene ().camera.parallax.mouseInfluence->value->getFloat ();
	const float amount = this->getScene ().camera.parallax.amount->value->getFloat ();
	const float delay = glm::clamp (
	    this->getScene ().camera.parallax.delay->value->getFloat () * (g_Time - g_TimeLast), 0.0f, 1.0f
	);

	const glm::vec2 centeredMouse = this->m_mousePosition - glm::vec2 (0.5f, 0.5f);
	this->m_parallaxDisplacement
	    = glm::mix (this->m_parallaxDisplacement, (centeredMouse * amount) * influence, delay);
    }

    // update main textures for images. Iterate every object (not just the flat render
    // order) so images nested inside compose layers — which are rendered by their layer,
    // not the main loop — still get their animated textures updated.
    for (const auto& cur : this->m_objects | std::views::values) {
	if (!cur->is<Objects::CImage> ()) {
	    continue;
	}

	const Objects::CImage* image = cur->as<Objects::CImage> ();

#if !NDEBUG
	const std::string message = "Updating texture " + image->getImage ().model->filename;

	glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, message.c_str ());
#endif

	image->getTexture ()->update ();

#if !NDEBUG
	glPopDebugGroup ();
#endif
    }

    // bind the vertex array
    glBindVertexArray (this->m_vaoBuffer);
    // use the scene's framebuffer by default
    glBindFramebuffer (GL_FRAMEBUFFER, this->getWallpaperFramebuffer ());
    // ensure we render over the whole framebuffer
    glViewport (0, 0, this->m_sceneFBO->getRealWidth (), this->m_sceneFBO->getRealHeight ());

    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Snapshot the debug filter/skip list once per frame under the lock, so the IPC
    // thread (inspector isolate/hide commands) can mutate them without racing the
    // per-object reads below.
    std::optional<int> objectFilter;
    std::vector<int> skipObjects;
    {
	std::lock_guard<std::mutex> lk (this->m_debugMutex);
	const auto& debug = this->getContext ().getApp ().getContext ().settings.render.debug;
	objectFilter = debug.objectFilter;
	skipObjects = debug.skipObjects;
    }

    for (const auto& cur : this->m_objectsByRenderOrder) {
	if (objectFilter.has_value () && cur->getId () != objectFilter.value ()) {
	    continue;
	}
	if (std::ranges::find (skipObjects, cur->getId ()) != skipObjects.end ()) {
	    continue;
	}

	cur->render ();
    }
}

void CScene::updateMouse (const glm::ivec4& viewport) {
    // update virtual mouse position first
    const glm::dvec2 position = this->getContext ().getInputContext ().getMouseInput ().position ();

    // rollover the position to the last
    this->m_mousePositionLast = this->m_mousePosition;

    // calculate the current position of the mouse in viewport space [0, 1]
    double mouseX = glm::clamp ((position.x - viewport.x) / viewport.z, 0.0, 1.0);
    // Normalize Y coordinate (OpenGL convention: 0=bottom, 1=top)
    // Particle code expects this convention: 0=bottom results in negative Y (down), 1=top results in positive Y (up)
    double normalizedMouseY = glm::clamp ((position.y - viewport.y) / viewport.w, 0.0, 1.0);

    // Account for UV cropping when using fill/fit scaling modes
    // The scene may be rendered larger than viewport and cropped via UVs
    const auto uvs = this->getState ().getTextureUVs ();

    // Map mouse position from viewport space to scene UV space
    // UVs define what portion of the scene texture is visible
    this->m_mousePositionNormalized.x = uvs.ustart + mouseX * (uvs.uend - uvs.ustart);
    this->m_mousePositionNormalized.y = uvs.vstart + normalizedMouseY * (uvs.vend - uvs.vstart);

    // Invert previous normalization of Y to match what the shader expects
    double mouseY = 1.0 - normalizedMouseY;

    this->m_mousePosition.x = this->m_mousePositionNormalized.x;
    this->m_mousePosition.y = uvs.vstart + mouseY * (uvs.vend - uvs.vstart);
}

const Scene& CScene::getScene () const { return *this->getWallpaperData ().as<Scene> (); }

int CScene::getWidth () const { return this->m_camera->getWidth (); }

int CScene::getHeight () const { return this->m_camera->getHeight (); }

float CScene::getTime () const { return g_Time; }

float CScene::getDeltaTime () const { return g_Time - g_TimeLast; }

float CScene::getFps () const {
    const float dt = g_Time - g_TimeLast;
    // Guard against the first frame (where g_TimeLast is 0 so dt == g_Time)
    // and division by zero on the very first call.
    if (dt <= 1e-6f) {
	return 60.0f;
    }
    return 1.0f / dt;
}

const glm::vec2* CScene::getMousePosition () const { return &this->m_mousePosition; }

const glm::vec2* CScene::getMousePositionLast () const { return &this->m_mousePositionLast; }

const glm::vec2* CScene::getMousePositionNormalized () const { return &this->m_mousePositionNormalized; }

const glm::vec2* CScene::getParallaxDisplacement () const { return &this->m_parallaxDisplacement; }

const std::vector<CObject*>& CScene::getObjectsByRenderOrder () const { return this->m_objectsByRenderOrder; }

const CObject* CScene::getObject (int id) const {
    const auto object = this->m_objects.find (id);
    return object == this->m_objects.end () ? nullptr : object->second;
}
