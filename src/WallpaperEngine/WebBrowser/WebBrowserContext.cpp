#include "WebBrowserContext.h"
#include "CEF/BrowserApp.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/WebBrowser/CEF/SubprocessApp.h"
#include "WallpaperEngine/WebBrowser/CEF/WPSchemeHandlerFactory.h"
#include "include/cef_app.h"
#include "include/cef_scheme.h"
#include <filesystem>
#include <random>
#include <string>
#include <unistd.h>

// Defined in lwe_bridge.cpp; non-empty when LWE runs embedded in another process.
extern std::string g_lwe_subprocess_path;

// CEF can only be initialized once per process lifetime.
static bool s_cef_alive = false;

using namespace WallpaperEngine::WebBrowser;

// TODO: THIS IS USED TO GENERATE A RANDOM FOLDER FOR THE CHROME PROFILE, MAYBE A DIFFERENT APPROACH WOULD BE BETTER?
namespace uuid {
static std::random_device rd;
static std::mt19937 gen (rd ());
static std::uniform_int_distribution<> dis (0, 15);
static std::uniform_int_distribution<> dis2 (8, 11);

std::string generate_uuid_v4 () {
    std::stringstream ss;
    int i;
    ss << std::hex;
    for (i = 0; i < 8; i++) {
	ss << dis (gen);
    }
    ss << "-";
    for (i = 0; i < 4; i++) {
	ss << dis (gen);
    }
    ss << "-4";
    for (i = 0; i < 3; i++) {
	ss << dis (gen);
    }
    ss << "-";
    ss << dis2 (gen);
    for (i = 0; i < 3; i++) {
	ss << dis (gen);
    }
    ss << "-";
    for (i = 0; i < 12; i++) {
	ss << dis (gen);
    };
    return ss.str ();
}
}

WebBrowserContext::WebBrowserContext (WallpaperEngine::Application::WallpaperApplication& wallpaperApplication) :
    m_browserApplication (nullptr), m_wallpaperApplication (wallpaperApplication) {
    CefMainArgs main_args (
	this->m_wallpaperApplication.getContext ().getArgc (), this->m_wallpaperApplication.getContext ().getArgv ()
    );

    // only care about app if the process is the main process
    // we should maybe use a better lib for handling command line arguments instead
    // or using C's version on some places and CefCommandLine on others
    // TODO: ANOTHER THING TO TAKE CARE OF BEFORE MERGING
    const CefRefPtr<CefCommandLine> commandLine = CefCommandLine::CreateCommandLine ();

    commandLine->InitFromArgv (main_args.argc, main_args.argv);

    if (!commandLine->HasSwitch ("type")) {
	this->m_browserApplication = new CEF::BrowserApp (wallpaperApplication);
    } else {
	this->m_browserApplication = new CEF::SubprocessApp (wallpaperApplication);
    }

    // this blocks for anything not-main-thread
    const int exit_code = CefExecuteProcess (main_args, this->m_browserApplication, nullptr);

    // this is needed to kill subprocesses after they're done
    if (exit_code >= 0) {
	// Sub proccess has endend, so exit
	exit (exit_code);
    }

    // Configurate Chromium
    CefSettings settings;
    // Use a per-screen profile so concurrent LWE processes (one per monitor) don't
    // race for the same CEF SingletonLock and crash.  Shader caches still persist
    // across restarts for the same screen name.
    std::string profileSuffix = "default";
    {
	const auto& screenBgs = wallpaperApplication.getContext ().settings.general.screenBackgrounds;
	if (!screenBgs.empty ()) {
	    profileSuffix = screenBgs.begin ()->first;
	    for (auto& c : profileSuffix) {
		if (c == '/' || c == ' ' || c == '\\') c = '_';
	    }
	}
    }
    std::string cache_path =
	(std::filesystem::temp_directory_path () / ("lwe-cef-profile-" + profileSuffix)).string ();
    cef_string_utf8_to_utf16 (cache_path.c_str (), cache_path.length (), &settings.root_cache_path);
    settings.windowless_rendering_enabled = true;
    settings.no_sandbox = true; // chrome-sandbox requires setuid root which LWE doesn't ship with

    // Resolve the directory containing linux-wallpaperengine so CEF can find
    // icudtl.dat, locales/, and other runtime resources.
    {
	char exe_buf [4096] {};
	ssize_t n = ::readlink ("/proc/self/exe", exe_buf, sizeof (exe_buf) - 1);
	if (n > 0) {
	    std::string exe_dir = std::filesystem::path (exe_buf).parent_path ().string ();
	    CefString (&settings.resources_dir_path) = exe_dir;
	    CefString (&settings.locales_dir_path)   = exe_dir + "/locales";
	}
    }

    // Point CEF at the minimal subprocess helper so it never re-execs the main
    // binary (which would fail because the main binary needs wallpaper args).
    // Priority: CGo-embedded path > LWE_CEF_SUBPROCESS_PATH env var.
    if (!g_lwe_subprocess_path.empty ()) {
	CefString (&settings.browser_subprocess_path) = g_lwe_subprocess_path;
    } else if (const char* envPath = std::getenv ("LWE_CEF_SUBPROCESS_PATH")) {
	if (envPath [0] != '\0') {
	    CefString (&settings.browser_subprocess_path) = envPath;
	}
    }

    // Remove stale singleton locks left by a previously killed process.
    // CEF writes SingletonLock (symlink hostname:pid) and related files; if the
    // process was SIGKILLed they are never cleaned up and CefInitialize fails.
    for (const char* name : {"SingletonLock", "SingletonSocket", "SingletonCookie"}) {
	std::filesystem::remove (std::filesystem::path (cache_path) / name);
    }

    // CEF can only be initialized once per process; skip if already alive.
    if (!s_cef_alive) {
	if (!CefInitialize (main_args, settings, this->m_browserApplication, nullptr)) {
	    sLog.exception ("CefInitialize: failed");
	}
	s_cef_alive = true;
    } else {
	// Hot-swap: CEF is already running.  Re-register scheme handler factories
	// so the new wallpaper's files are served under the fixed "wp://" scheme.
	const auto* app = static_cast<CEF::SubprocessApp*> (this->m_browserApplication.get ());
	for (const auto& [workshopId, factory] : app->getHandlerFactories ()) {
	    CefRegisterSchemeHandlerFactory (
		CEF::WPSchemeHandlerFactory::generateSchemeName (workshopId), CefString (), factory
	    );
	}
    }
}

WebBrowserContext::~WebBrowserContext () {
    // CEF can only be initialized once per process lifetime.  Skip shutdown
    // when we know the process will call CefInitialize again:
    //   • embedded CGo mode (g_lwe_subprocess_path set via lwe_set_subprocess_path)
    //   • hot-swap daemon mode (WEPAPERED_CTRL_SOCK set — main loop re-runs)
    if (!g_lwe_subprocess_path.empty () || std::getenv ("WEPAPERED_CTRL_SOCK")) {
	return;
    }
    sLog.out ("Shutting down CEF");
    CefShutdown ();
    s_cef_alive = false;
}
