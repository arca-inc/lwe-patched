// CEF implementation of the pluggable web backend. The CEF browser/render/input
// logic lives here; CWeb is a thin host that owns one IWebBackend and delegates.
#include "CefWebBackend.h"
#include "WallpaperEngine/Logging/Log.h"

#ifdef HAVE_WPE
#include "WallpaperEngine/WebBrowser/WPE/WpeWebBackend.h"
#endif

#include "include/cef_app.h"

#include <algorithm>
#include <cmath>
#include <unistd.h>

using namespace WallpaperEngine;
using namespace WallpaperEngine::WebBrowser;
using namespace WallpaperEngine::WebBrowser::CEF;

namespace {

using namespace std::chrono_literals;

// Run `window.__lweMedia.<method>(<jsonObject>)` in the page, guarding against the
// document-start bootstrap not being present yet (it could race a navigation). The JSON
// object literals come from Media::web*Json so the WE contract has one definition.
void emit (const CefRefPtr<CefFrame>& frame, const char* method, const std::string& jsonObject) {
    std::string js = "window.__lweMedia&&window.__lweMedia.";
    js += method;
    js += "(";
    js += jsonObject;
    js += ");";
    frame->ExecuteJavaScript (js, frame->GetURL (), 0);
}

} // namespace

CefWebBackend::CefWebBackend (IWebBackendHost& host, WebBrowserContext& browserContext) :
    m_host (host), m_browserContext (browserContext) { }

void CefWebBackend::start (
    const std::string& url, const int fps, const std::map<std::string, std::string>& properties
) {
    CefWindowInfo window_info;
    window_info.SetAsWindowless (0);

    this->m_renderHandler = new RenderHandler (this->m_host);

    CefBrowserSettings browserSettings;
    // documentation says that 60 fps is the maximum value
    browserSettings.windowless_frame_rate = std::max (60, fps);

    this->m_client = new BrowserClient (this->m_renderHandler);
    // Pass property overrides so OnLoadEnd can call wallpaperPropertyListener.applyUserProperties
    this->m_client->setProperties (properties);
    // CreateBrowserSync deadlocks in single-threaded mode because it blocks the
    // thread that must also pump CefDoMessageLoopWork().  Use async CreateBrowser
    // and finish initialisation in tick() once OnAfterCreated fires.
    CefBrowserHost::CreateBrowser (window_info, this->m_client, url, browserSettings, nullptr, nullptr);
}

void CefWebBackend::tick () {
    // Pump CEF on every loop iteration so OnAfterCreated fires promptly and mouse
    // events reach the browser at the full event-loop cadence.
    CefDoMessageLoopWork ();

    // Complete async browser initialisation once OnAfterCreated has fired.
    if (!this->m_browser && this->m_client->IsCreated ()) {
        this->m_browser = this->m_client->GetBrowser ();
        this->m_browser->GetHost ()->WasHidden (false);
        this->m_browser->GetHost ()->SetFocus (true);
        this->m_browser->GetHost ()->Invalidate (PET_VIEW);
    }

    this->pumpMedia ();
}

void CefWebBackend::pumpMedia () {
    if (!this->m_browser) {
        return;
    }
    CefRefPtr<CefFrame> frame = this->m_browser->GetMainFrame ();
    if (!frame) {
        return;
    }

    // 1. Apply a finished metadata poll.
    if (this->m_mediaPoll.valid ()
        && this->m_mediaPoll.wait_for (0ms) == std::future_status::ready) {
        const auto result = this->m_mediaPoll.get ();
        if (result.has_value ()) {
            const Media::MediaInfo& info = *result;

            // Status (availability) — push on first result and on change.
            if (!this->m_statusSent || info.available != this->m_lastAvailable) {
                emit (frame, "status", Media::webStatusJson (info.available));
                this->m_statusSent = true;
                this->m_lastAvailable = info.available;
            }

            // Properties — title / artist / album.
            if (!this->m_haveMedia || info.title != this->m_lastMedia.title
                || info.artist != this->m_lastMedia.artist || info.album != this->m_lastMedia.album) {
                emit (frame, "props", Media::webPropertiesJson (info));
            }

            // Playback state.
            if (!this->m_haveMedia || info.playbackState != this->m_lastMedia.playbackState) {
                emit (frame, "play", Media::webPlaybackJson (info.playbackState));
            }

            // Timeline — only when the second-resolution position or duration moved.
            if (!this->m_haveMedia || std::lround (info.position) != std::lround (this->m_lastMedia.position)
                || info.duration != this->m_lastMedia.duration) {
                emit (frame, "time", Media::webTimelineJson (info.position, info.duration));
            }

            // Album art — fetch off-thread when the URL changes.
            if (!info.artUrl.empty () && info.artUrl != this->m_artSentUrl
                && info.artUrl != this->m_artPendingUrl && !this->m_artFuture.valid ()) {
                this->m_artPendingUrl = info.artUrl;
                const std::string url = info.artUrl;
                this->m_artFuture = std::async (std::launch::async, [url] () { return Media::loadArt (url, true); });
            }

            this->m_lastMedia = info;
            this->m_haveMedia = true;
        }
    }

    // 2. Schedule the next poll (throttled to 750ms, off the render thread).
    if (!this->m_mediaPoll.valid ()) {
        const auto now = std::chrono::steady_clock::now ();
        if (this->m_lastMediaPoll.time_since_epoch ().count () == 0 || now - this->m_lastMediaPoll >= 750ms) {
            this->m_lastMediaPoll = now;
            this->m_mediaPoll = std::async (std::launch::async, Media::pollMediaInfo);
        }
    }

    // 3. Apply finished album-art load.
    if (this->m_artFuture.valid ()
        && this->m_artFuture.wait_for (0ms) == std::future_status::ready) {
        const Media::ArtData art = this->m_artFuture.get ();
        if (art.ok) {
            emit (frame, "thumb", Media::webThumbnailJson (art));
            this->m_artSentUrl = this->m_artPendingUrl;
        }
        this->m_artPendingUrl.clear ();
    }
}

void CefWebBackend::resize () {
    if (this->m_browser) {
        this->m_browser->GetHost ()->WasResized ();
    }
}

void CefWebBackend::mouseMove (const int x, const int y) {
    if (!this->m_browser) {
        return;
    }
    CefMouseEvent evt;
    evt.x = x;
    evt.y = y;
    this->m_browser->GetHost ()->SendMouseMoveEvent (evt, false);
}

void CefWebBackend::mouseClick (const int x, const int y, const bool right, const bool up) {
    if (!this->m_browser) {
        return;
    }
    CefMouseEvent evt;
    evt.x = x;
    evt.y = y;
    this->m_browser->GetHost ()->SendMouseClickEvent (
        evt, right ? CefBrowserHost::MouseButtonType::MBT_RIGHT : CefBrowserHost::MouseButtonType::MBT_LEFT, up, 1
    );
}

bool CefWebBackend::ready () const { return this->m_browser != nullptr; }

CefWebBackend::~CefWebBackend () {
    if (this->m_browser) {
        this->m_browser->GetHost ()->CloseBrowser (true);
        // Pump the message loop until CEF fires OnBeforeClose; without this, CEF
        // still holds a reference to the render handler and calls OnPaint after
        // this backend (and the host texture) is gone.
        int limit = 200;
        while (!this->m_client->IsClosed () && --limit > 0) {
            CefDoMessageLoopWork ();
            usleep (5000);
        }
    }
    // m_renderHandler lifetime is managed by BrowserClient's CefRefPtr — do NOT
    // delete it explicitly; the refcount frees it after OnBeforeClose.
}

std::unique_ptr<IWebBackend> WallpaperEngine::WebBrowser::makeWebBackend (
    WebBackendType type, IWebBackendHost& host, WebBrowserContext& browserContext
) {
    if (type == WebBackendType::Wpe) {
#ifdef HAVE_WPE
        sLog.out ("[web] using experimental WPE WebKit backend");
        return std::make_unique<WPE::WpeWebBackend> (host, browserContext);
#else
        sLog.out ("[web] backend 'wpe' was not built in; using the stable CEF backend");
#endif
    }
    return std::make_unique<CEF::CefWebBackend> (host, browserContext);
}
