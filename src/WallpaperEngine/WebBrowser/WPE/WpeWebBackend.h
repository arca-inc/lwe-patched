#pragma once

#include "WallpaperEngine/WebBrowser/WebBackend.h"

#include <map>
#include <memory>
#include <string>

namespace WallpaperEngine::WebBrowser::WPE {
// Opaque implementation state (holds all WPE/WebKit/GLib handles). Defined in the
// .cpp so this header pulls in no WPE headers and the renderer stays engine-agnostic.
struct WpeBackendImpl;

//! \brief WPE WebKit implementation of IWebBackend.
//!
//! A lightweight, Linux-native alternative to the CEF backend. The web page is
//! rendered offscreen by WPE WebKit through WPEBackend-fdo and the resulting
//! frame is uploaded into the host's GL texture each tick.
//!
//! EXPERIMENTAL. This first cut uses the SHM (CPU buffer) export path, mirroring
//! CEF's OnPaint -> glTexImage2D upload, so it reuses the existing texture upload
//! path. A zero-copy dmabuf/EGLImage export path is a planned follow-up once
//! wallpaper compatibility is confirmed.
class WpeWebBackend : public IWebBackend {
public:
    WpeWebBackend (IWebBackendHost& host, WebBrowserContext& browserContext);
    ~WpeWebBackend () override;

    void start (const std::string& url, int fps, const std::map<std::string, std::string>& properties) override;
    void tick () override;
    void resize () override;
    void mouseMove (int x, int y) override;
    void mouseClick (int x, int y, bool right, bool up) override;
    [[nodiscard]] bool ready () const override;

private:
    std::unique_ptr<WpeBackendImpl> m_impl;
};
} // namespace WallpaperEngine::WebBrowser::WPE
