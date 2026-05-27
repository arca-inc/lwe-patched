#pragma once

#include "include/cef_client.h"
#include "include/cef_life_span_handler.h"
#include <atomic>

namespace WallpaperEngine::WebBrowser::CEF {
class BrowserClient : public CefClient, public CefLifeSpanHandler {
public:
    explicit BrowserClient (CefRefPtr<CefRenderHandler> ptr);

    [[nodiscard]] CefRefPtr<CefRenderHandler> GetRenderHandler () override;
    [[nodiscard]] CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler () override { return this; }

    void OnAfterCreated (CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose (CefRefPtr<CefBrowser> browser) override;

    [[nodiscard]] bool IsCreated () const { return m_created.load (); }
    [[nodiscard]] bool IsClosed () const { return m_closed.load (); }
    [[nodiscard]] CefRefPtr<CefBrowser> GetBrowser () const { return m_browser; }

    CefRefPtr<CefRenderHandler> m_renderHandler = nullptr;

    IMPLEMENT_REFCOUNTING (BrowserClient);

private:
    std::atomic<bool> m_created {false};
    std::atomic<bool> m_closed {false};
    CefRefPtr<CefBrowser> m_browser;
};
} // namespace WallpaperEngine::WebBrowser::CEF