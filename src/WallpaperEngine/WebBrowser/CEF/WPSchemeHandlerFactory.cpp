#include "WPSchemeHandlerFactory.h"
#include "WPSchemeHandler.h"
#include "WallpaperEngine/WebBrowser/WebBrowserContext.h"
#include "include/wrapper/cef_helpers.h"

using namespace WallpaperEngine::WebBrowser::CEF;

WPSchemeHandlerFactory::WPSchemeHandlerFactory (const Project& project) : m_project (project) { }

CefRefPtr<CefResourceHandler> WPSchemeHandlerFactory::Create (
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString& scheme_name,
    CefRefPtr<CefRequest> request
) {
    CEF_REQUIRE_IO_THREAD ();
    return new WPSchemeHandler (this->m_project);
}

std::string WPSchemeHandlerFactory::generateSchemeName (const std::string& workshopId) {
    // Fixed scheme — workshopId no longer embedded so the same "wp://" scheme
    // name works across hot-swaps; CEF custom schemes can only be registered once
    // at startup and would otherwise show ERR_UNKNOWN_URL_SCHEME on wallpaper change.
    (void) workshopId;
    return std::string (WPENGINE_SCHEME);
}