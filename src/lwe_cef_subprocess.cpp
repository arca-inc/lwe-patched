// Minimal CEF subprocess helper binary.
// CEF spawns this with --type=renderer, --type=gpu-process, etc.
// Custom schemes must be registered here too, otherwise the renderer/network
// service processes won't recognise wp:// URLs and navigation fails.
#include "include/cef_app.h"
#include "include/cef_scheme.h"
#include "WallpaperEngine/WebBrowser/WebBrowserContext.h"

class SubprocessSchemeApp : public CefApp {
public:
    SubprocessSchemeApp () = default;

    void OnRegisterCustomSchemes (CefRawPtr<CefSchemeRegistrar> registrar) override {
        // Fixed scheme name — no workshopId suffix — covers all wallpapers and
        // hot-swaps without needing to re-register on every wallpaper change.
        registrar->AddCustomScheme (
            WPENGINE_SCHEME,
            CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE | CEF_SCHEME_OPTION_FETCH_ENABLED
        );
    }

    void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> command_line) override {
        command_line->AppendSwitch("disable-gpu");
        command_line->AppendSwitch("disable-gpu-compositing");
        command_line->AppendSwitch("disable-software-rasterizer");
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
