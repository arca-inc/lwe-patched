#include "BrowserApp.h"
#include "WallpaperEngine/Logging/Log.h"

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
    // swiftshader: pure-software ANGLE backend, no EGL/display connection needed.
    // headless ozone: GPU subprocess needs no display connection so navigation starts
    // immediately without waiting for GPU EGL initialisation (which hangs on Wayland).
    // disable-gpu-watchdog: SwiftShader's first-run shader compilation can exceed the
    // default watchdog timeout causing the GPU process to be killed and restarted.
    command_line->AppendSwitchWithValue ("--use-angle", "swiftshader");
    command_line->AppendSwitch ("--no-sandbox");
    command_line->AppendSwitch ("--disable-gpu-watchdog");
    command_line->AppendSwitchWithValue ("--ozone-platform", "headless");
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