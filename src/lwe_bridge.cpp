#include "lwe_bridge.h"
#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"
#include <atomic>
#include <csignal>
#include <iostream>
#include <ostream>
#include <string>

// Shared with WebBrowserContext.cpp so CEF can be pointed at the LWE binary.
std::string g_lwe_subprocess_path;

static std::atomic<WallpaperEngine::Application::WallpaperApplication*> g_app {nullptr};
// Set by lwe_stop() so a stop requested before g_app is stored is not lost.
static std::atomic<bool> g_stop_requested {false};
static bool g_log_initialized = false;

void lwe_set_subprocess_path (const char* path) {
    g_lwe_subprocess_path = path ? path : "";
}

int lwe_run (int argc, char** argv) {
    // Destroy the JS engine from the previous run so modules, intervals and
    // layer handles don't accumulate across repeated lwe_run() calls.
    WallpaperEngine::Scripting::ScriptEngine::resetSingleton ();
    g_stop_requested.store (false);   // reset for this run

    if (!g_log_initialized) {
        sLog.addOutput (new std::ostream (std::cout.rdbuf ()));
        sLog.addError (new std::ostream (std::cerr.rdbuf ()));
        g_log_initialized = true;
    }
    try {
        WallpaperEngine::Application::ApplicationContext ctx (argc, argv);
        ctx.loadSettingsFromArgv ();
        auto* app = new WallpaperEngine::Application::WallpaperApplication (ctx);
        g_app.store (app);
        // Deliver a stop that arrived while we were still constructing.
        if (g_stop_requested.load ()) {
            app->signal (SIGTERM);
        }
        app->show ();
        g_app.store (nullptr);
        delete app;
        return 0;
    } catch (const std::exception& e) {
        g_app.store (nullptr);
        std::cerr << "[lwe_bridge] " << e.what () << std::endl;
        return 1;
    }
}

void lwe_stop (void) {
    g_stop_requested.store (true);
    auto* app = g_app.load ();
    if (app) {
        app->signal (SIGTERM);
    }
}
