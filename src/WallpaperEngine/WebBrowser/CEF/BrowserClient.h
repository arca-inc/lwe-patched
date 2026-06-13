#pragma once

#include "include/cef_client.h"
#include "include/cef_display_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include <atomic>
#include <map>
#include <string>

namespace WallpaperEngine::WebBrowser::CEF {
class BrowserClient : public CefClient, public CefDisplayHandler, public CefLifeSpanHandler, public CefLoadHandler {
public:
    explicit BrowserClient (CefRefPtr<CefRenderHandler> ptr);

    [[nodiscard]] CefRefPtr<CefRenderHandler> GetRenderHandler () override;
    [[nodiscard]] CefRefPtr<CefDisplayHandler> GetDisplayHandler () override { return this; }
    [[nodiscard]] CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler () override { return this; }
    [[nodiscard]] CefRefPtr<CefLoadHandler>     GetLoadHandler ()     override { return this; }

    // CefDisplayHandler: surface page console.log output to LWE's log — used to
    // report the WebGL backend the page actually got (see OnLoadEnd's probe).
    bool OnConsoleMessage (CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
                           const CefString& message, const CefString& source, int line) override;

    void OnAfterCreated (CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose (CefRefPtr<CefBrowser> browser) override;

    // CefLoadHandler: inject properties after page load
    void OnLoadEnd (CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override;

    [[nodiscard]] bool IsCreated () const { return m_created.load (); }
    [[nodiscard]] bool IsClosed () const { return m_closed.load (); }
    [[nodiscard]] CefRefPtr<CefBrowser> GetBrowser () const { return m_browser; }

    // Properties to inject via wallpaperPropertyListener.applyUserProperties on load
    void setProperties (const std::map<std::string, std::string>& props) { m_properties = props; }

    CefRefPtr<CefRenderHandler> m_renderHandler = nullptr;

    IMPLEMENT_REFCOUNTING (BrowserClient);

private:
    std::atomic<bool> m_created {false};
    std::atomic<bool> m_closed {false};
    CefRefPtr<CefBrowser> m_browser;
    std::map<std::string, std::string> m_properties;
};
} // namespace WallpaperEngine::WebBrowser::CEF