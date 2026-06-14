#pragma once

#include <cctype>
#include <map>
#include <memory>
#include <string>

namespace WallpaperEngine::WebBrowser {

/// Web rendering backend for "web" wallpapers. CWeb renders HTML/CSS/JS/WebGL
/// through one of these. This is the selector for the hot-swappable web-backend
/// module: the concrete backends plug in behind CWeb's render path, chosen at
/// startup via --web-backend / LWE_WEB_BACKEND.
enum class WebBackendType {
    /// Chromium Embedded Framework. Stable, full WebGL/canvas/video. The default.
    Cef,
    /// WPE WebKit. Lighter weight, experimental, not yet implemented.
    Wpe,
};

/// parseWebBackend maps a backend name (case-insensitive) to a type. Empty or
/// unknown names fall back to the stable Cef backend.
inline WebBackendType parseWebBackend (std::string name) {
    for (char& c : name) {
        c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
    }
    if (name == "wpe") {
        return WebBackendType::Wpe;
    }
    return WebBackendType::Cef;
}

/// webBackendName returns the canonical lowercase name of a backend type.
inline const char* webBackendName (WebBackendType type) {
    switch (type) {
        case WebBackendType::Wpe: return "wpe";
        case WebBackendType::Cef:
        default: return "cef";
    }
}

class WebBrowserContext;

/// The host (the web wallpaper) a backend renders into: it owns the GL texture
/// the browser paints onto and reports the current size.
class IWebBackendHost {
public:
    virtual ~IWebBackendHost () = default;
    /// OpenGL texture id the backend uploads each painted frame into.
    [[nodiscard]] virtual unsigned int webTexture () const = 0;
    [[nodiscard]] virtual int webWidth () const = 0;
    [[nodiscard]] virtual int webHeight () const = 0;
    /// Read a wallpaper-relative file through the wallpaper's asset container
    /// (project dir plus pkg/preset/assets overlays), the same resolution the
    /// CEF scheme handler uses. Returns false if the file is not found. Lets a
    /// backend that registers its own resource scheme (e.g. WPE) serve files
    /// without reimplementing LWE's path logic. CEF resolves files internally and
    /// does not call this.
    [[nodiscard]] virtual bool readWallpaperFile (const std::string& relPath, std::string& out) const = 0;
};

/// A pluggable web rendering backend. Drives a windowless browser that paints the
/// page into the host's texture. CWeb owns one, selected at runtime by type.
class IWebBackend {
public:
    virtual ~IWebBackend () = default;
    /// Begin rendering `url` at up to `fps`, applying `properties` once loaded.
    virtual void start (const std::string& url, int fps,
                        const std::map<std::string, std::string>& properties) = 0;
    /// Pump the backend's event loop and complete deferred init. Call each frame.
    virtual void tick () = 0;
    /// Re-notify the backend that the host texture/size changed.
    virtual void resize () = 0;
    /// Forward pointer input, in host (top-left origin) coordinates.
    virtual void mouseMove (int x, int y) = 0;
    virtual void mouseClick (int x, int y, bool right, bool up) = 0;
    /// True once the browser exists and is rendering.
    [[nodiscard]] virtual bool ready () const = 0;
};

/// Build the backend for `type`. Cef is implemented and stable; Wpe is not yet
/// implemented and falls back to Cef. Defined in CEF/CefWebBackend.cpp.
std::unique_ptr<IWebBackend> makeWebBackend (WebBackendType type, IWebBackendHost& host,
                                             WebBrowserContext& browserContext);

} // namespace WallpaperEngine::WebBrowser
