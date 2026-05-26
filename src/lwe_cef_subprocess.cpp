// Minimal CEF subprocess helper binary.
// CEF spawns this with --type=renderer, --type=gpu-process, etc.
// All it needs to do is call CefExecuteProcess and exit.
#include "include/cef_app.h"

int main (int argc, char** argv) {
    CefMainArgs args (argc, argv);
    return CefExecuteProcess (args, nullptr, nullptr);
}
