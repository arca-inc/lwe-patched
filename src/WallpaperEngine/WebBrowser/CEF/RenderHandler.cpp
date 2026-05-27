#include "RenderHandler.h"
#include "lwe_bridge.h"

using namespace WallpaperEngine::WebBrowser::CEF;

RenderHandler::RenderHandler (WallpaperEngine::Render::Wallpapers::CWeb* webdata) : m_webdata (webdata) { }

// Required by CEF
void RenderHandler::GetViewRect (CefRefPtr<CefBrowser> browser, CefRect& rect) {
    rect = CefRect (0, 0, this->m_webdata->getWidth (), this->m_webdata->getHeight ());
}

// Will be executed in CEF message loop
void RenderHandler::OnPaint (
    CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects, const void* buffer,
    const int width, const int height
) {
    // First pixel data from CEF — signal wepapered that the web wallpaper is visible.
    // lwe_signal_first_frame() fires at most once and is a no-op on subsequent calls.
    lwe_signal_first_frame ();

    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, this->texture ());
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, buffer);
    glBindTexture (GL_TEXTURE_2D, 0);
}

int RenderHandler::getWidth () const { return this->m_webdata->getWidth (); }

int RenderHandler::getHeight () const { return this->m_webdata->getHeight (); }

GLuint RenderHandler::texture () const { return this->m_webdata->getWallpaperTexture (); }