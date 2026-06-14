#pragma once

// Matrices manipulation for OpenGL
#include <glm/ext.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Media/MediaProvider.h"
#include "WallpaperEngine/Render/CWallpaper.h"
#include "WallpaperEngine/WebBrowser/CEF/BrowserClient.h"
#include "WallpaperEngine/WebBrowser/CEF/RenderHandler.h"

#include "WallpaperEngine/Data/Model/Wallpaper.h"

namespace WallpaperEngine::WebBrowser::CEF {
class RenderHandler;
}

namespace WallpaperEngine::Render::Wallpapers {
class CWeb : public CWallpaper {
public:
    CWeb (
	const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
	WallpaperEngine::WebBrowser::WebBrowserContext& browserContext,
	const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
    );
    ~CWeb () override;
    [[nodiscard]] int getWidth () const override { return this->m_width; }

    [[nodiscard]] int getHeight () const override { return this->m_height; }

    void setSize (int width, int height);

protected:
    void tickInput (const glm::ivec4& viewport) override;
    void renderFrame (const glm::ivec4& viewport) override;
    void updateMouse (const glm::ivec4& viewport);
    const Web& getWeb () const { return *this->getWallpaperData ().as<Web> (); }
    // Poll playerctl (off-thread) and push now-playing media into the page via
    // window.__lweMedia.* (installed at document-start by lwe-cef-subprocess).
    void pumpMedia ();
    // Push the live audio FFT spectrum into the page's wallpaperRegisterAudioListener
    // callback (window.__lweMedia.audio), throttled to ~30 Hz.
    void pumpAudio ();

    friend class CWallpaper;

private:
    WallpaperEngine::WebBrowser::WebBrowserContext& m_browserContext;
    CefRefPtr<CefBrowser> m_browser = nullptr;
    CefRefPtr<WallpaperEngine::WebBrowser::CEF::BrowserClient> m_client = nullptr;
    WallpaperEngine::WebBrowser::CEF::RenderHandler* m_renderHandler = nullptr;

    int m_width = 16;
    int m_height = 17;

    // Media integration state.
    std::future<std::optional<WallpaperEngine::Media::MediaInfo>> m_mediaPoll;
    std::chrono::steady_clock::time_point m_lastMediaPoll {};
    WallpaperEngine::Media::MediaInfo m_lastMedia;
    bool m_haveMedia = false;     // a poll result has been applied at least once
    bool m_lastAvailable = false; // last availability sent to the status listener
    bool m_statusSent = false;
    std::future<WallpaperEngine::Media::ArtData> m_artFuture;
    std::string m_artPendingUrl; // artUrl currently being fetched
    std::string m_artSentUrl;    // artUrl whose thumbnail was last pushed

    std::chrono::steady_clock::time_point m_lastAudioPush {}; // ~30 Hz audio push throttle

    // When set (LWE_MEDIA_TO_TEXT), push now-playing title/artist into the page's
    // headerText / subheaderText wallpaper properties.
    bool m_forwardMediaText = false;

    WallpaperEngine::Input::MouseClickStatus m_leftClick = Input::Released;
    WallpaperEngine::Input::MouseClickStatus m_rightClick = Input::Released;

    glm::vec2 m_mousePosition = {};
    glm::vec2 m_mousePositionLast = {};
};
}
