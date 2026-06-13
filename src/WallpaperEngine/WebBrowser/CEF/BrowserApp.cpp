#include "BrowserApp.h"
#include "WallpaperEngine/Logging/Log.h"
#include <cstdlib>
#include <string>

using namespace WallpaperEngine::WebBrowser::CEF;

BrowserApp::BrowserApp (WallpaperEngine::Application::WallpaperApplication& application) :
    SubprocessApp (application) { }

CefRefPtr<CefBrowserProcessHandler> BrowserApp::GetBrowserProcessHandler () { return this; }

void BrowserApp::OnContextInitialized () {
    // register all the needed schemes, "wp" + the background id is going to be our scheme
    for (const auto& [workshopId, factory] : this->getHandlerFactories ()) {
	CefRegisterSchemeHandlerFactory (
	    WPSchemeHandlerFactory::generateSchemeName (workshopId), static_cast<const char*> (nullptr), factory
	);
    }
}

void BrowserApp::OnBeforeCommandLineProcessing (const CefString& process_type, CefRefPtr<CefCommandLine> command_line) {
    command_line->AppendSwitchWithValue (
	"--disable-features",
	"IsolateOrigins,HardwareMediaKeyHandling,WebContentsOcclusion,RendererCodeIntegrityEnabled,site-per-process"
    );
    command_line->AppendSwitch ("--disable-gpu-shader-disk-cache");
    command_line->AppendSwitch ("--disable-site-isolation-trials");
    command_line->AppendSwitch ("--disable-web-security");
    command_line->AppendSwitchWithValue ("--remote-allow-origins", "*");
    command_line->AppendSwitchWithValue ("--autoplay-policy", "no-user-gesture-required");
    command_line->AppendSwitch ("--disable-background-timer-throttling");
    command_line->AppendSwitch ("--disable-backgrounding-occluded-windows");
    command_line->AppendSwitch ("--disable-background-media-suspend");
    command_line->AppendSwitch ("--disable-renderer-backgrounding");
    command_line->AppendSwitch ("--disable-test-root-certs");
    command_line->AppendSwitch ("--disable-bundled-ppapi-flash");
    command_line->AppendSwitch ("--disable-breakpad");
    command_line->AppendSwitch ("--disable-field-trial-config");
    command_line->AppendSwitch ("--no-experiments");
    // Use basic (unencrypted) password/cookie storage — avoids blocking keychain prompts.
    command_line->AppendSwitchWithValue ("--password-store", "basic");
    command_line->AppendSwitch ("--use-mock-keychain");
    // --no-sandbox: the namespace sandbox blocks GPU device files (/dev/dri/*,
    // /dev/nvidia*) and Vulkan ICD paths, causing VK_ERROR_INITIALIZATION_FAILED.
    // --disable-gpu-watchdog: software/first-run shader compilation can exceed the
    // default watchdog timeout, causing the GPU process to be killed and restarted.
    //
    // The GPU backend is configurable so hardware acceleration can be A/B tested
    // against the software default. By default we use SwiftShader (pure-software
    // ANGLE, no EGL/display connection) + headless ozone: this never hangs on
    // Wayland GPU init, but renders WebGL/canvas on the CPU (slow for GPU-heavy
    // wallpapers). Override via environment variables to use the real GPU:
    //   LWE_WEB_ANGLE    ANGLE backend: "swiftshader" (default, software) |
    //                    "gl-egl" | "gl" | "vulkan" (hardware; needs /dev/dri).
    //   LWE_WEB_OZONE    ozone platform: "headless" (default) | "x11" | "wayland".
    //   LWE_WEB_GPU_LOG  when set, stream Chromium GPU/ANGLE logs to stderr (-v=1).
    const char* angleEnv = std::getenv ("LWE_WEB_ANGLE");
    const std::string angle = (angleEnv && angleEnv [0]) ? angleEnv : "swiftshader";
    const char* ozoneEnv = std::getenv ("LWE_WEB_OZONE");
    const std::string ozone = (ozoneEnv && ozoneEnv [0]) ? ozoneEnv : "headless";
    const bool softwareGL = (angle == "swiftshader");

    command_line->AppendSwitchWithValue ("--use-angle", angle);
    command_line->AppendSwitch ("--no-sandbox");
    command_line->AppendSwitch ("--disable-gpu-watchdog");
    command_line->AppendSwitchWithValue ("--ozone-platform", ozone);
    if (!softwareGL) {
	// Hardware path: stop Chromium silently demoting to software via its GPU
	// blocklist, and let the page's WebGL/canvas use the GPU rasteriser.
	command_line->AppendSwitch ("--ignore-gpu-blocklist");
	command_line->AppendSwitch ("--enable-gpu-rasterization");
    }
    if (std::getenv ("LWE_WEB_GPU_LOG")) {
	command_line->AppendSwitchWithValue ("--enable-logging", "stderr");
	command_line->AppendSwitchWithValue ("--v", "1");
    }
    sLog.out (
	std::string ("CEF web GPU backend: --use-angle=") + angle + " --ozone-platform=" + ozone
	+ (softwareGL ? " [software]" : " [hardware]")
    );
}

void BrowserApp::OnBeforeChildProcessLaunch (CefRefPtr<CefCommandLine> command_line) {
    // Pass the actual computed scheme names so subprocesses (renderer, network
    // service) can register the same wp<id>:// schemes in OnRegisterCustomSchemes.
    // ProjectParser may assign counter-based IDs ("-1", "-2"…) that don't match
    // the directory names that subprocesses would otherwise derive from --bg args.
    std::string schemeList;
    for (const auto& [workshopId, factory] : this->getHandlerFactories ()) {
	if (!schemeList.empty ()) schemeList += ",";
	schemeList += WPSchemeHandlerFactory::generateSchemeName (workshopId);
    }
    if (!schemeList.empty ()) {
	command_line->AppendSwitchWithValue ("wp-schemes", schemeList);
    }
    // add back any parameters we had before so the new process can load up everything needed
    for (int i = 1; i < this->getApplication ().getContext ().getArgc (); i++) {
	command_line->AppendArgument (this->getApplication ().getContext ().getArgv ()[i]);
    }
}