#include "CComposeLayer.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <sstream>
#include <utility>

#include "WallpaperEngine/Data/Assets/Texture.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Camera.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

using namespace WallpaperEngine::Render::Objects;

namespace {
const char* kVertexShader = R"glsl(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
uniform mat4 uMVP;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
}
)glsl";

const char* kFragmentShader = R"glsl(
#version 330 core
in vec2 vUV;
uniform sampler2D uTexture;
uniform float uAlpha;
out vec4 FragColor;
void main() {
    vec4 c = texture(uTexture, vUV);
    FragColor = vec4(c.rgb, c.a * uAlpha);
}
)glsl";

GLuint compileShader (GLenum type, const char* source) {
    GLuint shader = glCreateShader (type);
    glShaderSource (shader, 1, &source, nullptr);
    glCompileShader (shader);
    GLint status = GL_FALSE;
    glGetShaderiv (shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
	char log[1024];
	glGetShaderInfoLog (shader, sizeof (log), nullptr, log);
	sLog.error ("CComposeLayer shader compile failed: ", log);
	glDeleteShader (shader);
	return 0;
    }
    return shader;
}
} // namespace

CComposeLayer::CComposeLayer (Wallpapers::CScene& scene, const Object& object) : CObject (scene, object) {
    if (object.is<Image> ()) {
	this->m_nativeSize = object.as<Image> ()->size;
    }
    // Guard against a zero/degenerate size so the FBO is always valid.
    if (this->m_nativeSize.x < 1.0f) this->m_nativeSize.x = 1.0f;
    if (this->m_nativeSize.y < 1.0f) this->m_nativeSize.y = 1.0f;
}

CComposeLayer::~CComposeLayer () {
    if (this->m_vbo != 0) glDeleteBuffers (1, &this->m_vbo);
    if (this->m_vao != 0) glDeleteVertexArrays (1, &this->m_vao);
    if (this->m_program != 0) glDeleteProgram (this->m_program);
}

void CComposeLayer::setChildren (std::vector<CObject*> children) { this->m_children = std::move (children); }

void CComposeLayer::ensureGL () {
    if (this->m_glReady) {
	return;
    }
    this->m_glReady = true;

    std::ostringstream name;
    name << "_compose_" << this->getId ();
    const glm::vec2 size = this->m_nativeSize;
    this->m_fbo = this->getScene ().create (
	name.str (), TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0f, size, size
    );

    const GLuint vs = compileShader (GL_VERTEX_SHADER, kVertexShader);
    const GLuint fs = compileShader (GL_FRAGMENT_SHADER, kFragmentShader);
    if (vs == 0 || fs == 0) {
	if (vs) glDeleteShader (vs);
	if (fs) glDeleteShader (fs);
	return;
    }
    this->m_program = glCreateProgram ();
    glAttachShader (this->m_program, vs);
    glAttachShader (this->m_program, fs);
    glLinkProgram (this->m_program);
    glDeleteShader (vs);
    glDeleteShader (fs);
    this->m_uMVP = glGetUniformLocation (this->m_program, "uMVP");
    this->m_uTexture = glGetUniformLocation (this->m_program, "uTexture");
    this->m_uAlpha = glGetUniformLocation (this->m_program, "uAlpha");

    const float hx = this->m_nativeSize.x * 0.5f;
    const float hy = this->m_nativeSize.y * 0.5f;
    // The buffer is rendered with a centred ortho (row 0 = bottom), so UV.v follows gl y.
    const float verts[] = {
	-hx, -hy, 0.0f, 0.0f, hx, -hy, 1.0f, 0.0f, hx, hy, 1.0f, 1.0f,
	-hx, -hy, 0.0f, 0.0f, hx, hy,  1.0f, 1.0f, -hx, hy, 0.0f, 1.0f,
    };
    glGenVertexArrays (1, &this->m_vao);
    glGenBuffers (1, &this->m_vbo);
    glBindVertexArray (this->m_vao);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_vbo);
    glBufferData (GL_ARRAY_BUFFER, sizeof (verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray (0);
    glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof (float), reinterpret_cast<void*> (0));
    glEnableVertexAttribArray (1);
    glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof (float), reinterpret_cast<void*> (2 * sizeof (float)));
    glBindVertexArray (0);
}

void CComposeLayer::renderChildrenToBuffer () {
    auto& scene = this->getScene ();

    // Save the compose state so nested compose layers restore their parent's context.
    const int prevStopId = scene.getComposeStopId ();
    const glm::ivec2 prevOutputSize = scene.getOutputSize ();
    const std::shared_ptr<const CFBO> prevTarget = scene.getFBO ();

    GLint prevFBO = 0;
    GLint prevViewport[4] = {0, 0, 0, 0};
    glGetIntegerv (GL_FRAMEBUFFER_BINDING, &prevFBO);
    glGetIntegerv (GL_VIEWPORT, prevViewport);

    glBindFramebuffer (GL_FRAMEBUFFER, this->m_fbo->getFramebuffer ());
    glViewport (0, 0, this->m_fbo->getRealWidth (), this->m_fbo->getRealHeight ());
    glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    scene.setComposeStopId (this->getId ());
    scene.setActiveRenderTarget (this->m_fbo);
    scene.setOutputSize ({static_cast<int> (this->m_nativeSize.x), static_cast<int> (this->m_nativeSize.y)});
    scene.getCamera ().pushLocalProjection (this->m_nativeSize.x, this->m_nativeSize.y);

    for (auto* child : this->m_children) {
	if (child != nullptr) {
	    child->render ();
	}
    }

    scene.getCamera ().popLocalProjection ();
    scene.setComposeStopId (prevStopId);
    scene.setActiveRenderTarget (prevStopId == -1 ? nullptr : prevTarget);
    scene.setOutputSize (prevOutputSize);

    glBindFramebuffer (GL_FRAMEBUFFER, static_cast<GLuint> (prevFBO));
    glViewport (prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
}

void CComposeLayer::drawBuffer () {
    auto& scene = this->getScene ();
    const auto transform = this->resolveTransform (this->getObject ());

    const float scene_w = scene.getCamera ().getWidth ();
    const float scene_h = scene.getCamera ().getHeight ();
    // Same placement convention as CImage/CText: origin is WE bottom-left ortho space.
    const glm::vec3 gl_origin = {
	transform.origin.x - scene_w * 0.5f,
	scene_h * 0.5f - transform.origin.y,
	transform.origin.z,
    };

    glm::mat4 model = glm::translate (glm::mat4 (1.0f), gl_origin);
    model = glm::rotate (model, transform.angle, glm::vec3 (0.0f, 0.0f, 1.0f));
    model = glm::scale (model, transform.scale);

    const glm::mat4 mvp
	= scene.getCamera ().getProjection () * scene.getCamera ().getLookAt () * model;

    GLint prevVao = 0;
    GLint prevProgram = 0;
    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv (GL_CURRENT_PROGRAM, &prevProgram);
    const GLboolean prevDepth = glIsEnabled (GL_DEPTH_TEST);

    glDisable (GL_DEPTH_TEST);
    glEnable (GL_BLEND);
    glBlendEquation (GL_FUNC_ADD);
    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram (this->m_program);
    glUniformMatrix4fv (this->m_uMVP, 1, GL_FALSE, glm::value_ptr (mvp));
    glUniform1f (this->m_uAlpha, 1.0f);
    glActiveTexture (GL_TEXTURE0);
    glBindSampler (0, 0);
    glBindTexture (GL_TEXTURE_2D, this->m_fbo->getTextureID (0));
    glUniform1i (this->m_uTexture, 0);

    glBindVertexArray (this->m_vao);
    glDrawArrays (GL_TRIANGLES, 0, 6);

    glBindVertexArray (static_cast<GLuint> (prevVao));
    glUseProgram (static_cast<GLuint> (prevProgram));
    if (prevDepth) glEnable (GL_DEPTH_TEST);
}

void CComposeLayer::render () {
    if (!this->getObject ().is<Image> () || !this->getObject ().as<Image> ()->visible->value->getBool ()) {
	return;
    }
    this->ensureGL ();
    if (this->m_program == 0 || this->m_fbo == nullptr) {
	return;
    }
    this->renderChildrenToBuffer ();
    this->drawBuffer ();
}
