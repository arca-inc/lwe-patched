// This code is a modification of the original projects that can be found at
// https://github.com/if1live/cef-gl-example
// https://github.com/andmcgregor/cefgui
#include "CWeb.h"
#include "WallpaperEngine/WebBrowser/CEF/WPSchemeHandlerFactory.h"

#include "WallpaperEngine/Audio/AudioContext.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/PlaybackRecorder.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Media/MediaProvider.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <future>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Render::Wallpapers;

using namespace WallpaperEngine::WebBrowser;
using namespace WallpaperEngine::WebBrowser::CEF;

namespace {
using namespace std::chrono_literals;

// Escape a string for embedding inside a double-quoted JS string literal.
std::string jsEscape (const std::string& s) {
    std::string out;
    out.reserve (s.size () + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

// Format a property's current value as a JS literal for applyUserProperties, matching how
// Wallpaper Engine types each value: booleans as booleans, sliders/ints as numbers, and
// everything else (colors "r g b", combos, text) as a double-quoted string. Vectors print
// space-separated like WE (DynamicValue uses ", "). Shared by the load-time seeding in the
// constructor and the live applyLiveProperty path so both stay byte-for-byte consistent.
std::string formatPropertyLiteral (const WallpaperEngine::Data::Model::Property& prop, std::string raw) {
    using Model = WallpaperEngine::Data::Model::DynamicValue;
    for (std::size_t pos = raw.find (", "); pos != std::string::npos; pos = raw.find (", ", pos)) {
        raw.replace (pos, 2, " ");
        pos += 1;
    }
    switch (prop.getType ()) {
        case Model::Boolean:
            return (raw == "1" || raw == "true" || raw == "True") ? "true" : "false";
        case Model::Float:
        case Model::Int:
            return raw.empty () ? "0" : raw;
        default:
            return "\"" + jsEscape (raw) + "\"";
    }
}

// Run `window.__lweMedia.<method>(<jsonObject>)` in the page, guarding against the
// document-start bootstrap not being present yet (it could race a navigation).
void emitMedia (const CefRefPtr<CefFrame>& frame, const char* method, const std::string& jsonObject) {
    std::string js = "window.__lweMedia&&window.__lweMedia.";
    js += method;
    js += "(";
    js += jsonObject;
    js += ");";
    frame->ExecuteJavaScript (js, frame->GetURL (), 0);
}
} // namespace

CWeb::CWeb (
    const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext, WebBrowserContext& browserContext,
    const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
) : CWallpaper (wallpaper, context, audioContext, scalingMode, clampMode), m_browserContext (browserContext) {
    // setup framebuffers
    this->setupFramebuffers ();

    if (const char* e = std::getenv ("LWE_MEDIA_TO_TEXT"); e != nullptr && e[0] != '\0') {
        this->m_forwardMediaText = true;
    }

    CefWindowInfo window_info;
    window_info.SetAsWindowless (0);

    this->m_renderHandler = new WebBrowser::CEF::RenderHandler (this);

    CefBrowserSettings browserSettings;
    // documentaion says that 60 fps is maximum value
    browserSettings.windowless_frame_rate = std::min (60, context.getApp ().getContext ().settings.render.maximumFPS);

    this->m_client = new WebBrowser::CEF::BrowserClient (m_renderHandler);
    // Build the property set for wallpaperPropertyListener.applyUserProperties. Two things matter:
    //   1. Seed *every* declared property at its default — web wallpapers often gate rendering on a
    //      default (e.g. the CORSAIR collection waits for its "scene" combo "circuit" before drawing).
    //   2. Emit each value with its real JS type. Wallpaper Engine hands booleans as booleans and
    //      sliders as numbers; pages branch on `property.value` directly, so a boolean sent as the
    //      string "0" reads truthy and wrongly enables debug overlays / disables backgrounds. So the
    //      values are pre-formatted here into JS literals (BrowserClient emits them verbatim).
    // Command-line overrides win over defaults.
    const auto& overrides = context.getApp ().getContext ().settings.general.properties;
    std::map<std::string, std::string> webProperties;
    for (const auto& [name, prop] : this->getWeb ().project.properties) {
	if (prop == nullptr) {
	    continue;
	}
	const auto it = overrides.find (name);
	const std::string raw = it != overrides.end () ? it->second : prop->toString ();
	webProperties[name] = formatPropertyLiteral (*prop, raw);
    }
    // Any command-line override without a declared property (unusual) is forwarded as a string.
    for (const auto& [key, value] : overrides) {
	if (!webProperties.contains (key)) {
	    webProperties[key] = "\"" + jsEscape (value) + "\"";
	}
    }
    this->m_client->setProperties (webProperties);

    // Enumerate directory-backed properties (slideshow folders live under "directories/<name>"
    // in the preset). Wallpaper Engine exposes their contents through
    // wallpaperRequestRandomFileForProperty; collect the files here so BrowserClient can hand
    // the list to the page on load. Paths stay relative to the wallpaper root so the page can
    // load them straight through the wp:// scheme.
    const std::string& presetDir = context.getApp ().getContext ().settings.general.presetDir;
    if (!presetDir.empty ()) {
	std::map<std::string, std::vector<std::string>> directoryFiles;
	const std::filesystem::path directoriesRoot = std::filesystem::path (presetDir) / "directories";
	std::error_code ec;
	if (std::filesystem::is_directory (directoriesRoot, ec)) {
	    for (const auto& dir : std::filesystem::directory_iterator (directoriesRoot, ec)) {
		if (!dir.is_directory ()) {
		    continue;
		}
		const std::string property = dir.path ().filename ().string ();
		std::vector<std::string> files;
		for (const auto& file : std::filesystem::directory_iterator (dir.path (), ec)) {
		    if (file.is_regular_file ()) {
			// Hand back an absolute filesystem path with the leading slash removed:
			// these wallpapers build the image URL as "file:///" + path (a Windows-ism,
			// where paths start "C:/"), so on Linux the result must be "file:///home/..."
			// rather than "file:////home/...". CEF loads it directly (web security is off).
			std::string absolute = file.path ().string ();
			if (!absolute.empty () && absolute.front () == '/') {
			    absolute.erase (0, 1);
			}
			files.push_back (absolute);
		    }
		}
		if (!files.empty ()) {
		    std::sort (files.begin (), files.end ());
		    directoryFiles.emplace (property, std::move (files));
		}
	    }
	}
	this->m_client->setDirectoryFiles (directoryFiles);
    }
    // use the custom scheme for the wallpaper's files
    const std::string htmlURL = WPSchemeHandlerFactory::generateSchemeName (this->getWeb ().project.workshopId)
	+ "://root/" + this->getWeb ().filename;
    // CreateBrowserSync deadlocks in single-threaded mode because it blocks the
    // thread that must also pump CefDoMessageLoopWork().  Use async CreateBrowser
    // and finish initialisation in renderFrame() once OnAfterCreated fires.
    CefBrowserHost::CreateBrowser (window_info, this->m_client, htmlURL, browserSettings, nullptr, nullptr);
}

void CWeb::setSize (const int width, const int height) {
    this->m_width = width > 0 ? width : this->m_width;
    this->m_height = height > 0 ? height : this->m_height;

    // do not refresh the texture if any of the sizes are invalid
    if (this->m_width <= 0 || this->m_height <= 0) {
	return;
    }

    // reconfigure the texture
    glBindTexture (GL_TEXTURE_2D, this->getWallpaperTexture ());
    glTexImage2D (
	GL_TEXTURE_2D, 0, GL_RGBA8, this->getWidth (), this->getHeight (), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr
    );

    // Notify cef that it was resized(maybe it's not even needed)
    this->m_browser->GetHost ()->WasResized ();
}

void CWeb::tickInput (const glm::ivec4& viewport) {
    // Pump CEF on every loop iteration — not just when a new frame is due — so
    // OnAfterCreated fires promptly and mouse events reach the browser at the
    // full event-loop cadence rather than only at the capped render frame rate.
    CefDoMessageLoopWork ();

    // Complete async browser initialisation once OnAfterCreated has fired.
    if (!this->m_browser && this->m_client->IsCreated ()) {
        this->m_browser = this->m_client->GetBrowser ();
        this->m_browser->GetHost ()->WasHidden (false);
        this->m_browser->GetHost ()->SetFocus (true);
        this->m_browser->GetHost ()->Invalidate (PET_VIEW);
    }

    if (this->m_browser) {
        this->updateMouse (viewport);
        this->pumpMedia ();
        this->pumpAudio ();
        this->flushPendingProperties ();
    }
}

void CWeb::applyLiveProperty (const std::string& name) {
    // IPC thread: just queue the name; the value is read and pushed on the render thread.
    std::lock_guard<std::mutex> lk (this->m_pendingPropMutex);
    this->m_pendingProperties.push_back (name);
}

void CWeb::flushPendingProperties () {
    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> lk (this->m_pendingPropMutex);
        if (this->m_pendingProperties.empty ()) {
            return;
        }
        pending.swap (this->m_pendingProperties);
    }

    CefRefPtr<CefFrame> frame = this->m_browser->GetMainFrame ();
    if (!frame) {
        return;
    }

    // Build a single applyUserProperties call carrying every queued property at its current
    // value, mirroring how WE pushes runtime property changes to the page.
    std::string body;
    for (const std::string& name : pending) {
        const auto it = this->getWeb ().project.properties.find (name);
        if (it == this->getWeb ().project.properties.end () || it->second == nullptr) {
            continue;
        }
        if (!body.empty ()) {
            body += ',';
        }
        body += "\"" + jsEscape (name) + "\":{\"value\":" + formatPropertyLiteral (*it->second, it->second->toString ()) + "}";
    }
    if (body.empty ()) {
        return;
    }

    const std::string js
        = "window.wallpaperPropertyListener&&window.wallpaperPropertyListener.applyUserProperties&&"
          "window.wallpaperPropertyListener.applyUserProperties({" + body + "});";
    frame->ExecuteJavaScript (js, frame->GetURL (), 0);
}

void CWeb::pumpAudio () {
    CefRefPtr<CefFrame> frame = this->m_browser->GetMainFrame ();
    if (!frame) {
        return;
    }

    // Throttle to ~30 Hz: tickInput runs every event-loop iteration, far faster than
    // the spectrum meaningfully changes, and each push is a parsed ExecuteJavaScript.
    const auto now = std::chrono::steady_clock::now ();
    if (this->m_lastAudioPush.time_since_epoch ().count () != 0 && now - this->m_lastAudioPush < 30ms) {
        return;
    }
    this->m_lastAudioPush = now;

    auto& recorder = this->getAudioContext ().getRecorder ();
    recorder.update ();

    // WE's audio listener receives 128 floats: indices [0..63] left, [64..127] right.
    // LWE capture is mono, so both halves carry the same 64-band spectrum. When audio
    // processing is disabled the recorder feeds zeros, so this is a harmless no-op.
    std::string arr;
    arr.reserve (128 * 8);
    arr.push_back ('[');
    char buf[16];
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < 64; ++i) {
            if (pass != 0 || i != 0) {
                arr.push_back (',');
            }
            std::snprintf (buf, sizeof (buf), "%.4f", recorder.audio64[i]);
            arr += buf;
        }
    }
    arr.push_back (']');

    emitMedia (frame, "audio", arr);
}

void CWeb::pumpMedia () {
    CefRefPtr<CefFrame> frame = this->m_browser->GetMainFrame ();
    if (!frame) {
        return;
    }

    // 1. Apply a finished metadata poll.
    if (this->m_mediaPoll.valid () && this->m_mediaPoll.wait_for (0ms) == std::future_status::ready) {
        const auto result = this->m_mediaPoll.get ();
        if (result.has_value ()) {
            const Media::MediaInfo& info = *result;

            // Status (availability) — push on first result and on change.
            if (!this->m_statusSent || info.available != this->m_lastAvailable) {
                emitMedia (frame, "status", Media::webStatusJson (info.available));
                this->m_statusSent = true;
                this->m_lastAvailable = info.available;
            }
            // Properties — title / artist / album.
            if (!this->m_haveMedia || info.title != this->m_lastMedia.title
                || info.artist != this->m_lastMedia.artist || info.album != this->m_lastMedia.album) {
                emitMedia (frame, "props", Media::webPropertiesJson (info));

                // Optional bridge: drive wallpapers that show a text label (but don't
                // use the WE media API) by pushing the track into their headerText /
                // subheaderText properties via the standard wallpaperPropertyListener.
                if (this->m_forwardMediaText && info.available && !info.title.empty ()) {
                    std::string js
                        = "window.wallpaperPropertyListener&&window.wallpaperPropertyListener.applyUserProperties&&"
                          "window.wallpaperPropertyListener.applyUserProperties({"
                          "\"headerText\":{\"value\":\""
                        + jsEscape (info.title) + "\"},\"subheaderText\":{\"value\":\"" + jsEscape (info.artist)
                        + "\"}});";
                    frame->ExecuteJavaScript (js, frame->GetURL (), 0);
                }
            }
            // Playback state.
            if (!this->m_haveMedia || info.playbackState != this->m_lastMedia.playbackState) {
                emitMedia (frame, "play", Media::webPlaybackJson (info.playbackState));
            }
            // Timeline — only when the second-resolution position or duration moved.
            if (!this->m_haveMedia || std::lround (info.position) != std::lround (this->m_lastMedia.position)
                || info.duration != this->m_lastMedia.duration) {
                emitMedia (frame, "time", Media::webTimelineJson (info.position, info.duration));
            }
            // Album art — fetch off-thread when the URL changes.
            if (!info.artUrl.empty () && info.artUrl != this->m_artSentUrl && info.artUrl != this->m_artPendingUrl
                && !this->m_artFuture.valid ()) {
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

    // 3. Apply a finished album-art load.
    if (this->m_artFuture.valid () && this->m_artFuture.wait_for (0ms) == std::future_status::ready) {
        const Media::ArtData art = this->m_artFuture.get ();
        if (art.ok) {
            emitMedia (frame, "thumb", Media::webThumbnailJson (art));
            this->m_artSentUrl = this->m_artPendingUrl;
        }
        this->m_artPendingUrl.clear ();
    }
}

void CWeb::renderFrame (const glm::ivec4& viewport) {
    if (!this->m_browser) {
        return;
    }

    // ensure the viewport matches the window size, and resize if needed
    if (viewport.z != this->getWidth () || viewport.w != this->getHeight ()) {
	this->setSize (viewport.z, viewport.w);
    }

    // use the scene's framebuffer by default
    glBindFramebuffer (GL_FRAMEBUFFER, this->getWallpaperFramebuffer ());
    // ensure we render over the whole framebuffer
    glViewport (0, 0, this->getWidth (), this->getHeight ());
}

void CWeb::updateMouse (const glm::ivec4& viewport) {
    // update virtual mouse position first
    auto& input = this->getContext ().getInputContext ().getMouseInput ();

    const glm::dvec2 position = input.position ();
    const auto leftClick = input.leftClick ();
    const auto rightClick = input.rightClick ();

    CefMouseEvent evt;
    // Set mouse current position. Maybe clamps are not needed
    evt.x = std::clamp (static_cast<int> (position.x - viewport.x), 0, viewport.z);
    // Convert from OpenGL coordinates (Y=0 at bottom) to CEF coordinates (Y=0 at top)
    evt.y = viewport.w - std::clamp (static_cast<int> (position.y - viewport.y), 0, viewport.w);
    // Send mouse position to cef
    this->m_browser->GetHost ()->SendMouseMoveEvent (evt, false);

    // TODO: ANY OTHER MOUSE EVENTS TO SEND?
    if (leftClick != this->m_leftClick) {
	this->m_browser->GetHost ()->SendMouseClickEvent (
	    evt, CefBrowserHost::MouseButtonType::MBT_LEFT,
	    leftClick == WallpaperEngine::Input::MouseClickStatus::Released, 1
	);
    }

    if (rightClick != this->m_rightClick) {
	this->m_browser->GetHost ()->SendMouseClickEvent (
	    evt, CefBrowserHost::MouseButtonType::MBT_RIGHT,
	    rightClick == WallpaperEngine::Input::MouseClickStatus::Released, 1
	);
    }

    this->m_leftClick = leftClick;
    this->m_rightClick = rightClick;
}

CWeb::~CWeb () {
    if (this->m_browser) {
        this->m_browser->GetHost ()->CloseBrowser (true);
        // Pump the message loop until CEF fires OnBeforeClose; without this,
        // CEF still holds a reference to the render handler after ~CWeb returns
        // and calls OnPaint/GetViewRect on the already-freed CWeb pointer.
        int limit = 200;
        while (!this->m_client->IsClosed () && --limit > 0) {
            CefDoMessageLoopWork ();
            usleep (5000);
        }
    }
    // m_renderHandler lifetime is managed by BrowserClient's CefRefPtr —
    // do NOT delete it explicitly; the refcount will free it after OnBeforeClose.
}
