// Minimal CEF subprocess helper binary.
// CEF spawns this with --type=renderer, --type=gpu-process, etc.
// Custom schemes must be registered here too, otherwise the renderer/network
// service processes won't recognise wp:// URLs and navigation fails.
#include "include/cef_app.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_scheme.h"
#include "WallpaperEngine/WebBrowser/WebBrowserContext.h"
#include <cstdlib>
#include <string>

// Wallpaper Engine web "media integration" API. Web wallpapers call these globals at
// document-start to receive now-playing track info (title/artist/album), the album-art
// thumbnail + extracted colours, the play/pause state and the timeline. They MUST exist
// before the page's own scripts run, so they are installed here in the render process at
// context creation. The browser process polls playerctl (MPRIS) and pushes data later by
// calling window.__lweMedia.* via CefFrame::ExecuteJavaScript (see CefWebBackend).
static const char* const kMediaApiBootstrap =
    "(function(){"
    "if(window.__lweMedia)return;"
    "var M={cbStatus:null,cbProps:null,cbThumb:null,cbTime:null,cbPlay:null,cbAudio:null,"
    "lastStatus:null,lastProps:null,lastThumb:null,lastTime:null,lastPlay:null};"
    "window.wallpaperMediaIntegration=window.wallpaperMediaIntegration||"
    "{PLAYBACK_STOPPED:0,PLAYBACK_PLAYING:1,PLAYBACK_PAUSED:2};"
    "function reg(cbKey,lastKey){return function(cb){M[cbKey]=cb;"
    "if(typeof cb==='function'&&M[lastKey]!=null){try{cb(M[lastKey]);}catch(e){}}};}"
    "window.wallpaperRegisterMediaStatusListener=reg('cbStatus','lastStatus');"
    "window.wallpaperRegisterMediaPropertiesListener=reg('cbProps','lastProps');"
    "window.wallpaperRegisterMediaThumbnailListener=reg('cbThumb','lastThumb');"
    "window.wallpaperRegisterMediaTimelineListener=reg('cbTime','lastTime');"
    "window.wallpaperRegisterMediaPlaybackListener=reg('cbPlay','lastPlay');"
    // Audio-reactive listener: store the page's callback; the browser process pushes
    // the live FFT spectrum into window.__lweMedia.audio() each frame (see CWeb).
    "window.wallpaperRegisterAudioListener=function(cb){M.cbAudio=cb;};"
    // Directory properties (slideshow folders, etc.): the browser process enumerates the
    // folder and pushes its file list via __lweMedia.dir(prop,list). Each call to
    // wallpaperRequestRandomFileForProperty then hands back a fresh random file from that
    // list (matching Wallpaper Engine, where the page drives slideshow rotation).
    "M.dirFiles={};M.dirCb={};"
    "function pickRandom(prop){var f=M.dirFiles[prop];var c=M.dirCb[prop];"
    "if(f&&f.length&&typeof c==='function'){try{c(prop,f[Math.floor(Math.random()*f.length)]);}"
    "catch(e){if(window.console)console.log('[LWE] random-file callback error: '+e);}}}"
    "window.wallpaperRequestRandomFileForProperty=function(prop,cb){M.dirCb[prop]=cb;pickRandom(prop);};"
    "function fire(cbKey,lastKey,d){M[lastKey]=d;var c=M[cbKey];"
    "if(typeof c==='function'){try{c(d);}catch(e){if(window.console)console.log('[LWE] media listener error: '+e);}}}"
    "window.__lweMedia={"
    "status:function(d){fire('cbStatus','lastStatus',d);},"
    "props:function(d){fire('cbProps','lastProps',d);},"
    "thumb:function(d){fire('cbThumb','lastThumb',d);},"
    "time:function(d){fire('cbTime','lastTime',d);},"
    "play:function(d){fire('cbPlay','lastPlay',d);},"
    // A directory property's file list was (re)enumerated natively; store it and, if the
    // page already asked for a file from this property, satisfy that request immediately.
    "dir:function(prop,list){M.dirFiles[prop]=list;pickRandom(prop);},"
    // Audio has no 'last' replay (it streams every frame); just invoke the callback.
    "audio:function(a){var c=M.cbAudio;if(typeof c==='function'){"
    "try{c(a);}catch(e){if(window.console)console.log('[LWE] audio listener error: '+e);}}}};"
    "})();";

class SubprocessSchemeApp : public CefApp, public CefRenderProcessHandler {
public:
    SubprocessSchemeApp () = default;

    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler () override { return this; }

    // Install the WE media-integration API in every main-frame V8 context at
    // document-start, before the wallpaper's scripts execute.
    void OnContextCreated (
        CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> /*context*/
    ) override {
        if (frame && frame->IsMain ()) {
            frame->ExecuteJavaScript (kMediaApiBootstrap, frame->GetURL (), 0);
        }
    }

    void OnRegisterCustomSchemes (CefRawPtr<CefSchemeRegistrar> registrar) override {
        // Fixed scheme name — no workshopId suffix — covers all wallpapers and
        // hot-swaps without needing to re-register on every wallpaper change.
        registrar->AddCustomScheme (
            WPENGINE_SCHEME,
            CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE | CEF_SCHEME_OPTION_FETCH_ENABLED
        );
    }

    void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> command_line) override {
        // Mirror BrowserApp's gating: CEF launches the gpu-process/renderer through
        // this helper, so disabling the GPU here would override the parent's request
        // for hardware. Keep GPU disabled (software SwiftShader) by default, but when
        // LWE_WEB_ANGLE selects a hardware backend leave it enabled. The env var is
        // inherited from the process that spawned this subprocess.
        const char* angle = std::getenv("LWE_WEB_ANGLE");
        const bool softwareGL = !(angle && angle[0]) || std::string(angle) == "swiftshader";
        if (softwareGL) {
            command_line->AppendSwitch("disable-gpu");
            command_line->AppendSwitch("disable-gpu-compositing");
            command_line->AppendSwitch("disable-software-rasterizer");
        }
        command_line->AppendSwitchWithValue("ozone-platform-hint", "auto");
    }

private:
    IMPLEMENT_REFCOUNTING (SubprocessSchemeApp);
    DISALLOW_COPY_AND_ASSIGN (SubprocessSchemeApp);
};

int main (int argc, char** argv) {
    CefMainArgs args (argc, argv);
    CefRefPtr<SubprocessSchemeApp> app = new SubprocessSchemeApp ();
    return CefExecuteProcess (args, app, nullptr);
}
