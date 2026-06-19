#include "BrowserClient.h"
#include "WallpaperEngine/Logging/Log.h"
#include <sstream>
#include <string>

using namespace WallpaperEngine::WebBrowser::CEF;

bool BrowserClient::OnConsoleMessage (
    CefRefPtr<CefBrowser> /*browser*/, cef_log_severity_t level, const CefString& message,
    const CefString& source, int line
) {
    const std::string msg = message.ToString ();
    // Surface our own [LWE] probe lines always; surface page errors too, but stay
    // quiet for ordinary wallpaper console spam.
    if (msg.rfind ("[LWE]", 0) == 0 || level >= LOGSEVERITY_ERROR) {
        sLog.out ("[cef-console] " + msg + " (" + source.ToString () + ":" + std::to_string (line) + ")");
    }
    return false; // let CEF apply its default handling as well
}

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
    if (!frame->IsMain ()) return;

    // Probe which WebGL backend the page actually received: "SwiftShader" means
    // software (CPU) rendering, a Mesa/AMD/Intel/NVIDIA string means hardware.
    // The result is logged via OnConsoleMessage — the key signal when A/B testing
    // LWE_WEB_ANGLE=swiftshader vs gl-egl/vulkan.
    frame->ExecuteJavaScript (
        "(function(){try{"
        "var c=document.createElement('canvas');"
        "var gl=c.getContext('webgl2')||c.getContext('webgl');"
        "if(!gl){console.log('[LWE][webgl] context=NONE');return;}"
        "var d=gl.getExtension('WEBGL_debug_renderer_info');"
        "console.log('[LWE][webgl] renderer='+(d?gl.getParameter(d.UNMASKED_RENDERER_WEBGL):'?')"
        "+' | vendor='+(d?gl.getParameter(d.UNMASKED_VENDOR_WEBGL):'?')"
        "+' | '+gl.getParameter(gl.VERSION));"
        "}catch(e){console.log('[LWE][webgl] probe-error '+e);}})();",
        frame->GetURL (), 0
    );

    if (m_properties.empty ()) return;

    // Build wallpaperPropertyListener.applyUserProperties({key:{value:<literal>},...}).
    // Values arrive pre-formatted as JS literals (booleans/numbers unquoted, strings already
    // quoted and escaped — see CWeb) so pages that branch on property.value see the right type.
    std::ostringstream js;
    js << "(function(){"
       << "var p=window.wallpaperPropertyListener;"
       << "if(!p||!p.applyUserProperties)return;"
       << "p.applyUserProperties({";
    bool first = true;
    for (const auto& [k, v] : m_properties) {
        if (!first) js << ",";
        js << "\"" << escapeJsString (k) << "\":{\"value\":" << v << "}";
        first = false;
    }
    js << "});})();";

    frame->ExecuteJavaScript (js.str (), frame->GetURL (), 0);
}