// WPE WebKit implementation of the pluggable web backend. The web wallpaper is
// rendered offscreen by WPE WebKit and each finished frame is uploaded into the
// host's GL texture, mirroring CEF's OnPaint -> glTexImage2D upload.
//
// We drive WebKit through WPEPlatform's *headless* display (WebKit 2.0 API), NOT
// the older WPEBackend-fdo path. Why: fdo runs a nested in-process Wayland
// compositor that the WebKit web process talks to over Wayland-EGL; on NVIDIA the
// web process then loads libnvidia-egl-wayland, which aborts negotiating dmabuf
// feedback with fdo's nested compositor:
//
//   WPEWebProcess: egl-wayland/.../wayland-egldisplay.c: dmabuf_feedback_check_main_device:
//   Assertion `dev->size == sizeof(dev_t)' failed.
//
// The headless display has no Wayland connection at all, so egl-wayland is never
// loaded — and it still renders on the GPU through a surfaceless EGL context. LWE
// keeps its own (fd-based) Wayland connection for output; the two are independent.
#include "WpeWebBackend.h"
#include "WallpaperEngine/Logging/Log.h"
#include "lwe_bridge.h"

#include "MimeTypes.h"

#include <GL/glew.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <wpe/webkit.h>
#include <wpe/headless/wpe-headless.h>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>

using namespace WallpaperEngine::WebBrowser;
using namespace WallpaperEngine::WebBrowser::WPE;

namespace WallpaperEngine::WebBrowser::WPE {
// All WPE/WebKit/GLib state for one web view. Defined at namespace scope (not
// nested in WpeWebBackend) so the C callbacks below can name it.
struct WpeBackendImpl {
    IWebBackendHost& host;

    WPEDisplay* display = nullptr;
    WebKitWebContext* webContext = nullptr;
    WebKitUserContentManager* ucm = nullptr;
    WebKitWebView* view = nullptr;

    // The platform view WebKit renders into. Owned by `view`; we never unref it,
    // but we connect to its "buffer-rendered" signal to capture frames.
    ::WPEView* wpeView = nullptr;
    gulong bufferRenderedHandler = 0;

    // Zero-copy DMABuf path: the host texture samples the rendered frame through an
    // EGLImage (NVIDIA cannot CPU-map gbm buffers, so import_to_pixels fails). The
    // texture has no copy of its own, so the image backing the *current* frame must
    // stay alive until the next frame replaces it.
    EGLImageKHR currentImage = EGL_NO_IMAGE_KHR;
    EGLDisplay imageDisplay = EGL_NO_DISPLAY;

    bool started = false;

    int width = 16;
    int height = 16;

    // Currently-held pointer buttons, as WPEModifiers bits, so motion events
    // during a drag carry the pressed state.
    WPEModifiers buttonModifiers = static_cast<WPEModifiers> (0);

    // Property overrides applied once the page finishes loading.
    std::map<std::string, std::string> properties;

    // Diagnostics: log the first painted frame / first pointer move once.
    bool loggedFirstFrame = false;
    bool loggedMouseMove = false;

    explicit WpeBackendImpl (IWebBackendHost& h) : host (h) { }
};
} // namespace WallpaperEngine::WebBrowser::WPE

namespace {
#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif

// EGL/GL entry points for the zero-copy DMABuf import path, resolved once. (GLEW
// covers core GL but not these EGLImage extension functions.)
struct EglImageProcs {
    PFNEGLCREATEIMAGEKHRPROC create = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind = nullptr;
    bool resolved = false;
    bool ok = false;
};

EglImageProcs& eglImageProcs () {
    // Render thread only (LWE builds with -fno-threadsafe-statics); single-init is safe.
    static EglImageProcs p;
    if (!p.resolved) {
        p.resolved = true;
        p.create = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC> (eglGetProcAddress ("eglCreateImageKHR"));
        p.destroy = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC> (eglGetProcAddress ("eglDestroyImageKHR"));
        p.bind =
            reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC> (eglGetProcAddress ("glEGLImageTargetTexture2DOES"));
        p.ok = p.create != nullptr && p.destroy != nullptr && p.bind != nullptr;
    }
    return p;
}

// Zero-copy GPU path: import the rendered DMABuf into LWE's *own* EGL display (the
// one current on this render thread — NOT the headless WPEDisplay's, which is a
// different EGLDisplay) and bind it to the host texture. NVIDIA renders web content
// into block-linear GBM buffers that gbm_bo_map (import_to_pixels) cannot CPU-map,
// so this EGLImage import is the only working path on NVIDIA.
bool uploadDmaBuf (WpeBackendImpl* d, WPEBuffer* buffer) {
    EglImageProcs& procs = eglImageProcs ();
    if (!procs.ok) {
        sLog.error ("[wpe] EGL dmabuf import unavailable (eglCreateImageKHR/glEGLImageTargetTexture2DOES missing)");
        return false;
    }
    EGLDisplay dpy = eglGetCurrentDisplay ();
    if (dpy == EGL_NO_DISPLAY) {
        sLog.error ("[wpe] no current EGL display for dmabuf import");
        return false;
    }

    WPEBufferDMABuf* dma = WPE_BUFFER_DMA_BUF (buffer);
    const int width = wpe_buffer_get_width (buffer);
    const int height = wpe_buffer_get_height (buffer);
    const guint32 fourcc = wpe_buffer_dma_buf_get_format (dma);
    const guint32 nPlanes = wpe_buffer_dma_buf_get_n_planes (dma);
    const guint64 modifier = wpe_buffer_dma_buf_get_modifier (dma);
    const bool haveModifier = modifier != DRM_FORMAT_MOD_INVALID;

    static const EGLint planeFd[4] = {
        EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
    static const EGLint planeOffset[4] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT,
        EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static const EGLint planePitch[4] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static const EGLint planeModLo[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static const EGLint planeModHi[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};

    EGLint attribs[64];
    int n = 0;
    attribs[n++] = EGL_WIDTH;
    attribs[n++] = width;
    attribs[n++] = EGL_HEIGHT;
    attribs[n++] = height;
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[n++] = static_cast<EGLint> (fourcc);
    for (guint32 p = 0; p < nPlanes && p < 4u; ++p) {
        attribs[n++] = planeFd[p];
        attribs[n++] = wpe_buffer_dma_buf_get_fd (dma, p);
        attribs[n++] = planeOffset[p];
        attribs[n++] = static_cast<EGLint> (wpe_buffer_dma_buf_get_offset (dma, p));
        attribs[n++] = planePitch[p];
        attribs[n++] = static_cast<EGLint> (wpe_buffer_dma_buf_get_stride (dma, p));
        if (haveModifier) {
            attribs[n++] = planeModLo[p];
            attribs[n++] = static_cast<EGLint> (modifier & 0xFFFFFFFFu);
            attribs[n++] = planeModHi[p];
            attribs[n++] = static_cast<EGLint> (modifier >> 32);
        }
    }
    attribs[n++] = EGL_NONE;

    EGLImageKHR image =
        procs.create (dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, static_cast<EGLClientBuffer> (nullptr), attribs);
    if (image == EGL_NO_IMAGE_KHR) {
        sLog.error ("[wpe] eglCreateImageKHR(dmabuf) failed: egl error ", eglGetError ());
        return false;
    }

    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, d->host.webTexture ());
    procs.bind (GL_TEXTURE_2D, static_cast<GLeglImageOES> (image));
    // EGLImage textures are not mipmap-complete; sample them linearly.
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture (GL_TEXTURE_2D, 0);

    // The host texture now samples the new dmabuf through `image`; the previous
    // frame's image is no longer referenced, so release it (and its dmabuf).
    if (d->currentImage != EGL_NO_IMAGE_KHR && d->imageDisplay != EGL_NO_DISPLAY) {
        procs.destroy (d->imageDisplay, d->currentImage);
    }
    d->currentImage = image;
    d->imageDisplay = dpy;
    return true;
}

// CPU fallback for SHM buffers (rare on a GPU display): map to pixels and upload,
// the same shape as CEF's OnPaint.
bool uploadPixels (WpeBackendImpl* d, WPEBuffer* buffer) {
    GError* error = nullptr;
    GBytes* bytes = wpe_buffer_import_to_pixels (buffer, &error);
    if (bytes == nullptr) {
        sLog.error ("[wpe] import_to_pixels failed: ", (error != nullptr && error->message != nullptr) ? error->message : "?");
        if (error != nullptr) {
            g_error_free (error);
        }
        return false;
    }
    gsize size = 0;
    const void* pixels = g_bytes_get_data (bytes, &size);
    const int width = wpe_buffer_get_width (buffer);
    const int height = wpe_buffer_get_height (buffer);
    // WPE_PIXEL_FORMAT_ARGB8888 is little-endian, i.e. B,G,R,A byte order in
    // memory — matches GL_BGRA_EXT.
    const int stride = (height > 0 && size > 0) ? static_cast<int> (size / static_cast<gsize> (height)) : width * 4;

    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, d->host.webTexture ());
    glPixelStorei (GL_UNPACK_ROW_LENGTH, stride / 4);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture (GL_TEXTURE_2D, 0);

    g_bytes_unref (bytes);
    return true;
}

// A frame finished rendering: upload it into the host's GL texture. Runs during
// tick()'s main-context iteration on the render thread, so LWE's GL context is
// current here. The page is GPU-composited in the web process; we keep the frame
// on the GPU via a DMABuf EGLImage (SHM is only a fallback).
void onBufferRendered (::WPEView* /*view*/, WPEBuffer* buffer, gpointer data) {
    auto* d = static_cast<WpeBackendImpl*> (data);
    if (d->wpeView == nullptr || buffer == nullptr) {
        return;
    }

    const bool isDmaBuf = WPE_IS_BUFFER_DMA_BUF (buffer);
    if (!(isDmaBuf ? uploadDmaBuf (d, buffer) : uploadPixels (d, buffer))) {
        return;
    }

    if (!d->loggedFirstFrame) {
        sLog.out (
            "[wpe] first frame painted ", wpe_buffer_get_width (buffer), "x", wpe_buffer_get_height (buffer),
            isDmaBuf ? " (dmabuf/GPU)" : " (shm)", " into texture ", d->host.webTexture ()
        );
        d->loggedFirstFrame = true;
    }

    // Signal wepapered the web wallpaper is visible (no-op after the first call).
    lwe_signal_first_frame ();
}

// Custom "wp" scheme: serve wallpaper files through the host (LWE's asset
// container), mirroring the CEF WPSchemeHandler. URL is wp://root/<path>; the
// host ("root") is ignored, only the path is used.
void onUriSchemeRequest (WebKitURISchemeRequest* request, gpointer user_data) {
    auto* d = static_cast<WpeBackendImpl*> (user_data);

    const char* rawPath = webkit_uri_scheme_request_get_path (request);
    std::string file = rawPath != nullptr ? rawPath : "";
    if (!file.empty () && file.front () == '/') {
        file = file.substr (1);
    }

    std::string contents;
    if (!d->host.readWallpaperFile (file, contents)) {
        sLog.error ("[wpe] scheme request NOT FOUND: ", file);
        GError* error =
            g_error_new (g_quark_from_static_string ("wpe-wallpaper"), 404, "File not found: %s", file.c_str ());
        webkit_uri_scheme_request_finish_error (request, error);
        g_error_free (error);
        return;
    }

    const char* mime = MimeTypes::getType (file.c_str ());
    if (mime == nullptr) {
        mime = "application/octet-stream";
    }
    sLog.out ("[wpe] scheme served: ", file, " (", contents.size (), " bytes, ", mime, ")");

    // g_memory_input_stream copies via the GBytes; contents can go out of scope.
    GBytes* bytes = g_bytes_new (contents.data (), contents.size ());
    GInputStream* stream = g_memory_input_stream_new_from_bytes (bytes);
    webkit_uri_scheme_request_finish (request, stream, static_cast<gint64> (contents.size ()), mime);
    g_object_unref (stream);
    g_bytes_unref (bytes);
}

gboolean onLoadFailed (
    WebKitWebView* /*view*/, WebKitLoadEvent /*event*/, char* failingURI, GError* error, gpointer /*user_data*/
) {
    sLog.error (
        "[wpe] load FAILED: ", failingURI != nullptr ? failingURI : "?", " : ",
        (error != nullptr && error->message != nullptr) ? error->message : "?"
    );
    return FALSE; // let WebKit run its default (error page) handling
}

void onWebProcessTerminated (WebKitWebView* /*view*/, WebKitWebProcessTerminationReason reason, gpointer /*user_data*/) {
    // reason: 0=crashed, 1=exceeded-memory-limit, 2=terminated-by-API.
    sLog.error ("[wpe] WEB PROCESS TERMINATED, reason=", static_cast<int> (reason));
}

// Push property overrides into the page once loaded, the same contract as the CEF
// backend's BrowserClient::OnLoadEnd: call the page's window.wallpaperPropertyListener
// .applyUserProperties({...}) if it defines one.
void onLoadChanged (WebKitWebView* view, WebKitLoadEvent event, gpointer user_data) {
    static const char* const names[] = {"STARTED", "REDIRECTED", "COMMITTED", "FINISHED"};
    if (event <= WEBKIT_LOAD_FINISHED) {
        sLog.out ("[wpe] load: ", names[event]);
    }
    if (event != WEBKIT_LOAD_FINISHED) {
        return;
    }
    auto* d = static_cast<WpeBackendImpl*> (user_data);
    if (d->properties.empty ()) {
        return;
    }

    std::ostringstream js;
    js << "(function(){var p=window.wallpaperPropertyListener;"
       << "if(!p||!p.applyUserProperties)return;p.applyUserProperties({";
    bool first = true;
    for (const auto& [key, value] : d->properties) {
        if (!first) {
            js << ",";
        }
        first = false;
        js << "\"" << key << "\":{\"value\":\"" << value << "\"}";
    }
    js << "});})();";

    const std::string script = js.str ();
    webkit_web_view_evaluate_javascript (
        view, script.c_str (), static_cast<gssize> (script.size ()), nullptr, nullptr, nullptr, nullptr, nullptr
    );
}

// WebKit uses the event time for ordering and click/drag detection; a constant 0
// breaks those. Use the GLib monotonic clock in milliseconds.
uint32_t pointerTime () {
    return static_cast<uint32_t> (g_get_monotonic_time () / 1000);
}
} // namespace

WpeWebBackend::WpeWebBackend (IWebBackendHost& host, WebBrowserContext& /*browserContext*/) :
    m_impl (std::make_unique<WpeBackendImpl> (host)) { }

void WpeWebBackend::start (const std::string& url, int /*fps*/, const std::map<std::string, std::string>& properties) {
    auto* d = m_impl.get ();
    // start() is single-shot (CWeb calls it once from its constructor).
    if (d->started) {
        return;
    }
    d->width = d->host.webWidth ();
    d->height = d->host.webHeight ();
    d->properties = properties;

    // Belt-and-braces: with an explicit headless display WebKit no longer talks
    // Wayland, but clearing WAYLAND_DISPLAY here (inherited by the web/GPU child
    // process spawned below) guarantees no auxiliary Wayland-EGL probe can reload
    // libnvidia-egl-wayland and re-trigger the dmabuf_feedback assertion on NVIDIA.
    // Safe for LWE itself: its own Wayland output connection is already open (this
    // runs on the render thread, long after startup) and is not re-resolved from env.
    unsetenv ("WAYLAND_DISPLAY");

    // Headless WPEPlatform display: offscreen, no Wayland connection -> never loads
    // libnvidia-egl-wayland, so it sidesteps the fdo nested-compositor dmabuf-feedback
    // crash on NVIDIA, while still rendering on the GPU via a surfaceless EGL context.
    d->display = wpe_display_headless_new ();
    if (d->display == nullptr) {
        sLog.error ("[wpe] wpe_display_headless_new failed");
        return;
    }
    GError* error = nullptr;
    if (!wpe_display_connect (d->display, &error)) {
        sLog.error ("[wpe] wpe_display_connect failed: ", (error != nullptr && error->message != nullptr) ? error->message : "?");
        if (error != nullptr) {
            g_error_free (error);
        }
        g_clear_object (&d->display);
        return;
    }
    // Make it the process default too, so the spawned web process renders on the
    // headless display even if it resolves the display via wpe_display_get_default().
    wpe_display_set_primary (d->display);

    // Isolated web context + register the wallpaper resource scheme. Register on
    // the security manager BEFORE any load so WebGL/fetch from wp:// are allowed.
    d->webContext = webkit_web_context_new ();
    WebKitSecurityManager* security = webkit_web_context_get_security_manager (d->webContext);
    webkit_security_manager_register_uri_scheme_as_secure (security, "wp");
    webkit_security_manager_register_uri_scheme_as_cors_enabled (security, "wp");
    webkit_web_context_register_uri_scheme (d->webContext, "wp", onUriSchemeRequest, d, nullptr);

    WebKitSettings* settings = webkit_settings_new ();
    webkit_settings_set_enable_webgl (settings, TRUE);
    webkit_settings_set_enable_webaudio (settings, TRUE);
    webkit_settings_set_enable_media (settings, TRUE);
    webkit_settings_set_enable_2d_canvas_acceleration (settings, TRUE);
    webkit_settings_set_enable_write_console_messages_to_stdout (settings, TRUE);
    webkit_settings_set_allow_file_access_from_file_urls (settings, TRUE);
    webkit_settings_set_allow_universal_access_from_file_urls (settings, TRUE);

    d->ucm = webkit_user_content_manager_new ();

    // Wallpaper Engine web API shim. WE web wallpapers call host-provided globals
    // (wallpaperRegisterAudioListener, …) that LWE does not implement; an undefined
    // call throws and aborts the rest of the page's script (e.g. the fluid sim never
    // starts). Inject no-op stubs at document-start so the page runs; audio-reactive
    // features simply stay idle. The page still defines window.wallpaperPropertyListener
    // itself — that one we call into (see onLoadChanged), so do not stub it.
    {
        static const char* const weShim =
            "(function(){var noop=function(){};"
            "window.wallpaperRegisterAudioListener=window.wallpaperRegisterAudioListener||noop;"
            "window.wallpaperRequestRandomFileForProperty=window.wallpaperRequestRandomFileForProperty||noop;"
            "window.wallpaperRegisterMediaPropertiesListener=window.wallpaperRegisterMediaPropertiesListener||noop;"
            "window.wallpaperRegisterMediaThumbnailListener=window.wallpaperRegisterMediaThumbnailListener||noop;"
            "window.wallpaperRegisterMediaTimelineListener=window.wallpaperRegisterMediaTimelineListener||noop;"
            "window.wallpaperRegisterMediaPlaybackListener=window.wallpaperRegisterMediaPlaybackListener||noop;})();";
        WebKitUserScript* script = webkit_user_script_new (
            weShim, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr
        );
        webkit_user_content_manager_add_script (d->ucm, script);
        webkit_user_script_unref (script);
    }

    // TEMP input probe: log DOM mouse events to console (-> stdout) so we can tell
    // whether dispatched pointer events actually reach the page's DOM.
    {
        static const char* const probe =
            "(function(){var n=0;"
            "document.addEventListener('mousemove',function(e){if(n++%30===0)"
            "console.log('[wpe-input] mousemove '+e.clientX+','+e.clientY);},true);"
            "document.addEventListener('mousedown',function(e){"
            "console.log('[wpe-input] mousedown b'+e.button+' '+e.clientX+','+e.clientY);},true);"
            "document.addEventListener('mouseup',function(e){"
            "console.log('[wpe-input] mouseup b'+e.button+' '+e.clientX+','+e.clientY);},true);})();";
        WebKitUserScript* script = webkit_user_script_new (
            probe, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr
        );
        webkit_user_content_manager_add_script (d->ucm, script);
        webkit_user_script_unref (script);
    }

    // Construct the web view on our headless display (the "display" property binds
    // it; WebKit creates the WPEView/WPEToplevel on that display). Construct-only
    // properties require g_object_new.
    d->view = WEBKIT_WEB_VIEW (g_object_new (
        WEBKIT_TYPE_WEB_VIEW, "display", d->display, "web-context", d->webContext, "user-content-manager", d->ucm,
        "settings", settings, nullptr
    ));
    g_object_unref (settings);

    if (d->view == nullptr) {
        sLog.error ("[wpe] failed to create WebKitWebView");
        g_clear_object (&d->ucm);
        g_clear_object (&d->webContext);
        g_clear_object (&d->display);
        return;
    }

    g_signal_connect (d->view, "load-changed", G_CALLBACK (onLoadChanged), d);
    g_signal_connect (d->view, "load-failed", G_CALLBACK (onLoadFailed), d);
    g_signal_connect (d->view, "web-process-terminated", G_CALLBACK (onWebProcessTerminated), d);

    // The platform view is where frames land. Capture them via "buffer-rendered",
    // and drive it to our target size + visible/mapped so an offscreen view is not
    // render-throttled (an unmapped/hidden view is suspended and never paints).
    d->wpeView = webkit_web_view_get_wpe_view (d->view);
    if (d->wpeView != nullptr) {
        d->bufferRenderedHandler =
            g_signal_connect (d->wpeView, "buffer-rendered", G_CALLBACK (onBufferRendered), d);

        WPEToplevel* toplevel = wpe_view_get_toplevel (d->wpeView);
        if (toplevel != nullptr) {
            wpe_toplevel_resized (toplevel, d->width, d->height);
        }
        wpe_view_resized (d->wpeView, d->width, d->height);
        wpe_view_set_visible (d->wpeView, TRUE);
        wpe_view_map (d->wpeView);
        wpe_view_focus_in (d->wpeView);
    } else {
        sLog.error ("[wpe] webkit_web_view_get_wpe_view returned null; no frames will be captured");
    }

    webkit_web_view_load_uri (d->view, url.c_str ());
    d->started = true;
    sLog.out ("[wpe] backend started (headless), loading ", url, " at ", d->width, "x", d->height);
}

void WpeWebBackend::tick () {
    auto* d = m_impl.get ();
    if (!d->started) {
        return;
    }
    // Drive WebKit's global-default GLib context (scheme requests, WebKit IPC, frame
    // exports, signals) without blocking. Bounded so a busy rAF/WebGL page that keeps
    // its sources ready can't starve LWE's render loop — leftover work runs next tick.
    for (int i = 0; i < 32 && g_main_context_iteration (nullptr, FALSE); ++i) {
        // dispatch one ready source per iteration, up to the budget
    }
}

void WpeWebBackend::resize () {
    auto* d = m_impl.get ();
    d->width = d->host.webWidth ();
    d->height = d->host.webHeight ();
    if (d->wpeView != nullptr) {
        WPEToplevel* toplevel = wpe_view_get_toplevel (d->wpeView);
        if (toplevel != nullptr) {
            wpe_toplevel_resized (toplevel, d->width, d->height);
        }
        wpe_view_resized (d->wpeView, d->width, d->height);
    }
}

void WpeWebBackend::mouseMove (int x, int y) {
    auto* d = m_impl.get ();
    if (d->wpeView == nullptr) {
        return;
    }
    if (!d->loggedMouseMove) {
        sLog.out ("[wpe] first mouseMove ", x, ",", y);
        d->loggedMouseMove = true;
    }
    WPEEvent* event = wpe_event_pointer_move_new (
        WPE_EVENT_POINTER_MOVE, d->wpeView, WPE_INPUT_SOURCE_MOUSE, pointerTime (), d->buttonModifiers,
        static_cast<double> (x), static_cast<double> (y), 0.0, 0.0
    );
    wpe_view_event (d->wpeView, event);
    wpe_event_unref (event);
}

void WpeWebBackend::mouseClick (int x, int y, bool right, bool up) {
    auto* d = m_impl.get ();
    if (d->wpeView == nullptr) {
        return;
    }
    const guint button = right ? WPE_BUTTON_SECONDARY : WPE_BUTTON_PRIMARY;
    const WPEModifiers buttonBit = right ? WPE_MODIFIER_POINTER_BUTTON3 : WPE_MODIFIER_POINTER_BUTTON1;
    if (up) {
        d->buttonModifiers = static_cast<WPEModifiers> (d->buttonModifiers & ~buttonBit);
    } else {
        d->buttonModifiers = static_cast<WPEModifiers> (d->buttonModifiers | buttonBit);
    }
    sLog.out ("[wpe] mouseClick ", x, ",", y, " button=", right ? 3 : 1, " state=", up ? 0 : 1);

    // press_count must be 0 for a release; WPE asserts !pressCount unless type is DOWN.
    const guint pressCount = up ? 0u : 1u;
    WPEEvent* event = wpe_event_pointer_button_new (
        up ? WPE_EVENT_POINTER_UP : WPE_EVENT_POINTER_DOWN, d->wpeView, WPE_INPUT_SOURCE_MOUSE, pointerTime (),
        d->buttonModifiers, button, static_cast<double> (x), static_cast<double> (y), pressCount
    );
    wpe_view_event (d->wpeView, event);
    wpe_event_unref (event);
}

bool WpeWebBackend::ready () const {
    return m_impl->started;
}

WpeWebBackend::~WpeWebBackend () {
    auto* d = m_impl.get ();
    if (d == nullptr) {
        return;
    }
    // Flush already-queued sources (pending frame signals, scheme requests) while the
    // view is still alive, so none fire on freed state after teardown.
    if (d->started) {
        for (int i = 0; i < 64 && g_main_context_iteration (nullptr, FALSE); ++i) {
            // drain pending work before teardown
        }
    }
    if (d->wpeView != nullptr && d->bufferRenderedHandler != 0) {
        g_signal_handler_disconnect (d->wpeView, d->bufferRenderedHandler);
        d->bufferRenderedHandler = 0;
    }
    // Release the in-flight frame's EGLImage (runs on the render thread with the
    // EGL display still valid).
    if (d->currentImage != EGL_NO_IMAGE_KHR && d->imageDisplay != EGL_NO_DISPLAY) {
        EglImageProcs& procs = eglImageProcs ();
        if (procs.destroy != nullptr) {
            procs.destroy (d->imageDisplay, d->currentImage);
        }
        d->currentImage = EGL_NO_IMAGE_KHR;
        d->imageDisplay = EGL_NO_DISPLAY;
    }
    d->wpeView = nullptr; // owned by the web view
    if (d->view != nullptr) {
        g_object_unref (d->view);
        d->view = nullptr;
    }
    if (d->ucm != nullptr) {
        g_object_unref (d->ucm);
        d->ucm = nullptr;
    }
    if (d->webContext != nullptr) {
        g_object_unref (d->webContext);
        d->webContext = nullptr;
    }
    if (d->display != nullptr) {
        g_object_unref (d->display);
        d->display = nullptr;
    }
}
