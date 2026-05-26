#pragma once

#include "include/cef_client.h"
#include "include/cef_life_span_handler.h"
#include <atomic>

namespace WallpaperEngine::WebBrowser::CEF {
// *************************************************************************
//! \brief Provide access to browser-instance-specific callbacks. A single
//! CefClient instance can be shared among any number of browsers.
// *************************************************************************
class BrowserClient : public CefClient, public CefLifeSpanHandler {
public:
    explicit BrowserClient (CefRefPtr<CefRenderHandler> ptr);

    [[nodiscard]] CefRefPtr<CefRenderHandler> GetRenderHandler () override;
    [[nodiscard]] CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler () override { return this; }

    void OnAfterCreated (CefRefPtr<CefBrowser> browser) override;

    [[nodiscard]] bool IsCreated () const { return m_created.load (); }
    [[nodiscard]] CefRefPtr<CefBrowser> GetBrowser () const { return m_browser; }

    CefRefPtr<CefRenderHandler> m_renderHandler = nullptr;

    IMPLEMENT_REFCOUNTING (BrowserClient);

private:
    std::atomic<bool> m_created {false};
    CefRefPtr<CefBrowser> m_browser;
};
} // namespace WallpaperEngine::WebBrowser::CEF