#include <iostream>
#include "include/cef_app.h"
#include "include/cef_client.h"

// Defined in lwe_bridge.cpp
extern std::string g_lwe_subprocess_path;

class SimpleClient : public CefClient, public CefLifeSpanHandler {
public:
    SimpleClient() {}

    virtual CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }

    virtual void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        CefQuitMessageLoop();
    }

    IMPLEMENT_REFCOUNTING(SimpleClient);
};

class SimpleApp : public CefApp, public CefBrowserProcessHandler {
public:
    SimpleApp() {}

    virtual CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }

    virtual void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> command_line) override {
        // Disable GPU hardware acceleration to prevent EGL/Vulkan crashes on Wayland
        command_line->AppendSwitch("disable-gpu");
        command_line->AppendSwitch("disable-gpu-compositing");
        command_line->AppendSwitch("disable-software-rasterizer");
        command_line->AppendSwitch("disable-web-security");
        command_line->AppendSwitchWithValue("remote-allow-origins", "*");
    }

    virtual void OnContextInitialized() override {
        CefWindowInfo window_info;
        // On Linux, empty window_info creates a standard X11 window
        
        CefBrowserSettings browser_settings;

        std::string url = "http://localhost:9001"; // Default
        CefRefPtr<CefCommandLine> command_line = CefCommandLine::GetGlobalCommandLine();
        if (command_line->HasSwitch("ui-window")) {
            std::string ui_url = command_line->GetSwitchValue("ui-window").ToString();
            if (!ui_url.empty()) {
                url = ui_url;
            }
        }
        CefRefPtr<SimpleClient> client(new SimpleClient());

        CefBrowserHost::CreateBrowser(window_info, client, url, browser_settings, nullptr, nullptr);
    }

    IMPLEMENT_REFCOUNTING(SimpleApp);
};

int run_ui_window(int argc, char* argv[]) {
    CefMainArgs main_args(argc, argv);
    CefRefPtr<SimpleApp> app(new SimpleApp);

    int exit_code = CefExecuteProcess(main_args, app.get(), nullptr);
    if (exit_code >= 0) {
        return exit_code;
    }

    CefSettings settings;
    settings.windowless_rendering_enabled = false;
    settings.no_sandbox = true;

    char exe_buf[4096] {};
    ssize_t n = ::readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (n > 0) {
        std::string exe_dir = std::string(exe_buf);
        size_t last_slash = exe_dir.find_last_of('/');
        if (last_slash != std::string::npos) {
            exe_dir = exe_dir.substr(0, last_slash);
            CefString(&settings.resources_dir_path) = exe_dir;
            CefString(&settings.locales_dir_path)   = exe_dir + "/locales";
        }
    }

    if (!g_lwe_subprocess_path.empty()) {
        CefString(&settings.browser_subprocess_path) = g_lwe_subprocess_path;
    } else if (const char* envPath = std::getenv("LWE_CEF_SUBPROCESS_PATH")) {
        if (envPath[0] != '\0') {
            CefString(&settings.browser_subprocess_path) = envPath;
        }
    }

    if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
        return 1;
    }

    CefRunMessageLoop();
    CefShutdown();
    return 0;
}
