#include "BrowserClient.h"
#include <sstream>

using namespace WallpaperEngine::WebBrowser::CEF;

BrowserClient::BrowserClient (CefRefPtr<CefRenderHandler> ptr) : m_renderHandler (std::move (ptr)) { }

CefRefPtr<CefRenderHandler> BrowserClient::GetRenderHandler () { return m_renderHandler; }

void BrowserClient::OnAfterCreated (CefRefPtr<CefBrowser> browser) {
    m_browser = browser;
    m_created.store (true);
}

void BrowserClient::OnBeforeClose (CefRefPtr<CefBrowser> browser) {
    m_browser = nullptr;
    m_closed.store (true);
}

static std::string escapeJsString (const std::string& s) {
    std::string out;
    out.reserve (s.size () + 4);
    for (char c : s) {
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else                out += c;
    }
    return out;
}

void BrowserClient::OnLoadEnd (CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int /*httpStatusCode*/) {
    if (!frame->IsMain () || m_properties.empty ()) return;

    // Build wallpaperPropertyListener.applyUserProperties({key:{value:"val"},...})
    std::ostringstream js;
    js << "(function(){"
       << "var p=window.wallpaperPropertyListener;"
       << "if(!p||!p.applyUserProperties)return;"
       << "p.applyUserProperties({";
    bool first = true;
    for (const auto& [k, v] : m_properties) {
        if (!first) js << ",";
        js << "\"" << escapeJsString (k) << "\":{\"value\":\"" << escapeJsString (v) << "\"}";
        first = false;
    }
    js << "});})();";

    frame->ExecuteJavaScript (js.str (), frame->GetURL (), 0);
}