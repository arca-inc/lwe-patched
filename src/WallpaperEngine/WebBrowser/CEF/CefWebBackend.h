#pragma once

#include "WallpaperEngine/Media/MediaProvider.h"
#include "WallpaperEngine/WebBrowser/CEF/BrowserClient.h"
#include "WallpaperEngine/WebBrowser/CEF/RenderHandler.h"
#include "WallpaperEngine/WebBrowser/WebBackend.h"
#include "include/cef_browser.h"

#include <chrono>
#include <future>
#include <map>
#include <optional>
#include <string>

namespace WallpaperEngine::WebBrowser::CEF {
//! \brief CEF (Chromium Embedded Framework) implementation of IWebBackend.
//! Owns the windowless CefBrowser and paints into the host's GL texture via the
//! RenderHandler. This is the stable, default backend.
class CefWebBackend : public IWebBackend {
public:
    CefWebBackend (IWebBackendHost& host, WebBrowserContext& browserContext);
    ~CefWebBackend () override;

    void start (const std::string& url, int fps, const std::map<std::string, std::string>& properties) override;
    void tick () override;
    void resize () override;
    void mouseMove (int x, int y) override;
    void mouseClick (int x, int y, bool right, bool up) override;
    [[nodiscard]] bool ready () const override;

private:
    // Poll playerctl (off-thread), diff against the last pushed state and forward
    // now-playing media to the page via window.__lweMedia.* (see lwe_cef_subprocess).
    void pumpMedia ();

    IWebBackendHost& m_host;
    WebBrowserContext& m_browserContext;
    CefRefPtr<CefBrowser> m_browser = nullptr;
    CefRefPtr<BrowserClient> m_client = nullptr;
    RenderHandler* m_renderHandler = nullptr;

    // Media integration state.
    std::future<std::optional<Media::MediaInfo>> m_mediaPoll;
    std::chrono::steady_clock::time_point m_lastMediaPoll {};
    Media::MediaInfo m_lastMedia;
    bool m_haveMedia = false;        // a poll result has been applied at least once
    bool m_lastAvailable = false;    // last value sent to the status listener
    bool m_statusSent = false;
    std::future<Media::ArtData> m_artFuture;
    std::string m_artPendingUrl;     // artUrl currently being fetched
    std::string m_artSentUrl;        // artUrl whose thumbnail was last pushed
};
} // namespace WallpaperEngine::WebBrowser::CEF
