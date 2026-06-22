#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <unistd.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Logging/Log.h"
#include "lwe_bridge.h"
#include "ui_window.h"

using App = WallpaperEngine::Application::WallpaperApplication;
using Ctx = WallpaperEngine::Application::ApplicationContext;

#include "External/json/single_include/nlohmann/json.hpp"

struct IpcLoad {
    std::string bg;
    std::string presetDir;
    std::map<std::string, std::string> props;
    bool valid = false;
};

static std::atomic<App*> g_app {nullptr};
static std::atomic<bool> g_ipc_stop {false};
static std::mutex        g_ipc_mu;
static IpcLoad           g_ipc_next_load;  // protected by g_ipc_mu
static int               g_ipc_reply_fd {-1}; // protected by g_ipc_mu

void signalhandler (const int sig) {
    auto* a = g_app.load ();
    if (a) a->signal (sig);
}

// Background thread: accepts connections on srv_fd, reads one-line commands.
//   "stop\n"                 → graceful shutdown
//   "load:<path>\n"          → hot-swap (legacy protocol, no preset)
//   JSON {"cmd":"load",...}\n → hot-swap with preset_dir and props
static void ipc_thread_func (int srv_fd) {
    while (!g_ipc_stop.load ()) {
        fd_set rfds;
        FD_ZERO (&rfds);
        FD_SET (srv_fd, &rfds);
        struct timeval tv {0, 200000};
        if (select (srv_fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;

        int conn = accept (srv_fd, nullptr, nullptr);
        if (conn < 0) continue;

        char buf [65536] {};
        int n = read (conn, buf, sizeof (buf) - 1);
        if (n <= 0) { close (conn); continue; }

        std::string cmd (buf, n);
        while (!cmd.empty () && (cmd.back () == '\n' || cmd.back () == '\r'))
            cmd.pop_back ();

        auto* a = g_app.load ();

        if (cmd == "stop") {
            g_ipc_stop.store (true);
            [[maybe_unused]] ssize_t wr = write (conn, "OK\n", 3);
            close (conn);
            if (a) a->signal (SIGTERM);
        } else if (cmd == "inspect") {
            // Scene debug inspector: return the live object graph as JSON. Read-only,
            // so it's safe to serialize from this thread without pausing rendering.
            std::string json = a ? a->inspectScene () : std::string ("{}");
            [[maybe_unused]] ssize_t wr = write (conn, json.data (), json.size ());
            close (conn);
        } else if (cmd.rfind ("load:", 0) == 0) {
            // Legacy plain-text protocol
            std::lock_guard<std::mutex> lk (g_ipc_mu);
            if (g_ipc_reply_fd >= 0) { close (g_ipc_reply_fd); }
            g_ipc_next_load = { cmd.substr (5), {}, {}, true };
            g_ipc_reply_fd  = conn;
            if (a) a->signal (SIGTERM);
        } else if (!cmd.empty () && cmd[0] == '{') {
            // JSON protocol: {"cmd":"load","bg":"...","preset_dir":"...","props":{...}}
            // plus the debug-inspector commands (isolate/hide/show/reset/set), which
            // apply live to the running app without a reload.
            try {
                auto j = nlohmann::json::parse (cmd);
                const std::string c = j.value ("cmd", "");

                // Debug-inspector live commands: handled synchronously here (no reload).
                static const std::set<std::string> debugCmds = {
                    "isolate", "hide", "show", "reset", "set", "highlight", "setproperty"
                };
                if (debugCmds.count (c)) {
                    nlohmann::json ack;
                    ack["ok"] = true;
                    if (a == nullptr) {
                        ack["ok"] = false;
                        ack["error"] = "no app";
                    } else if (c == "isolate") {
                        if (j.contains ("id") && j["id"].is_number_integer ())
                            a->debugIsolate (j["id"].get<int> ());
                        else
                            a->debugIsolate (std::nullopt);
                    } else if (c == "hide" || c == "show") {
                        if (j.contains ("id") && j["id"].is_number_integer ())
                            a->debugSetHidden (j["id"].get<int> (), c == "hide");
                        else { ack["ok"] = false; ack["error"] = "missing id"; }
                    } else if (c == "reset") {
                        a->debugClear ();
                    } else if (c == "highlight") {
                        // Missing/non-integer id clears the highlight.
                        a->debugHighlight (
                            (j.contains ("id") && j["id"].is_number_integer ()) ? j["id"].get<int> () : -1);
                    } else if (c == "setproperty") {
                        // Live-apply a user property (schemecolor, barcount, …) with no reload.
                        const std::string name = j.value ("name", "");
                        std::string value;
                        if (j.contains ("value")) {
                            const auto& v = j["value"];
                            if (v.is_string ()) {
                                value = v.get<std::string> ();
                            } else if (v.is_array ()) {
                                // colours/vectors arrive as [r,g,b]; join space-separated.
                                for (auto& e : v) {
                                    if (!value.empty ()) value += ' ';
                                    value += e.is_number () ? std::to_string (e.get<double> ()) : e.dump ();
                                }
                            } else if (v.is_boolean ()) {
                                value = v.get<bool> () ? "1" : "0";
                            } else if (v.is_number ()) {
                                value = std::to_string (v.get<double> ());
                            }
                        }
                        if (name.empty () || a == nullptr || !a->setProperty (name, value)) {
                            ack["ok"] = false;
                            ack["error"] = "unknown property or no app";
                        }
                    } else if (c == "set") {
                        const std::string prop = j.value ("prop", "");
                        const int id = j.value ("id", -1);
                        float vals[3] = {0, 0, 0};
                        int count = 0;
                        if (j.contains ("value")) {
                            const auto& v = j["value"];
                            if (v.is_array ()) {
                                for (auto& e : v) {
                                    if (count < 3 && e.is_number ()) vals[count++] = e.get<float> ();
                                }
                            } else if (v.is_number ()) {
                                vals[0] = v.get<float> (); count = 1;
                            } else if (v.is_boolean ()) {
                                vals[0] = v.get<bool> () ? 1.0f : 0.0f; count = 1;
                            }
                        }
                        if (!a->debugEditObject (id, prop, vals, count)) {
                            ack["ok"] = false;
                            ack["error"] = "unknown id/prop or no value";
                        }
                    }
                    const std::string reply = ack.dump ();
                    [[maybe_unused]] ssize_t wr = write (conn, reply.data (), reply.size ());
                    close (conn);
                } else if (c == "load") {
                    IpcLoad load;
                    load.bg        = j.value ("bg", "");
                    load.presetDir = j.value ("preset_dir", "");
                    load.valid     = true;
                    if (j.contains ("props") && j["props"].is_object ()) {
                        for (auto& [k, v] : j["props"].items ()) {
                            if (v.is_string ())      load.props[k] = v.get<std::string> ();
                            else if (v.is_number ()) load.props[k] = v.dump ();
                            else if (v.is_boolean ()) load.props[k] = v.get<bool> () ? "1" : "0";
                        }
                    }
                    std::lock_guard<std::mutex> lk (g_ipc_mu);
                    if (g_ipc_reply_fd >= 0) { close (g_ipc_reply_fd); }
                    g_ipc_next_load = std::move (load);
                    g_ipc_reply_fd  = conn;
                    if (a) a->signal (SIGTERM);
                } else {
                    close (conn);
                }
            } catch (const nlohmann::json::exception&) {
                close (conn);
            }
        } else {
            close (conn);
        }
    }
}

// Watch the daemon (parent) for death and self-exit when it's gone, so a crashed or
// killed wepapered never leaves orphaned LWE renderers running on the desktop.
//
// We reuse the READY pipe: the daemon holds the read end, the subprocess the write end
// (fd). Once the read end is fully closed — which only happens when the daemon process
// dies, since it keeps the fd open for our whole lifetime — poll() on the write end
// reports POLLERR/POLLHUP. We dup the fd so the READY signal can still write+close the
// original without disturbing this watch, and poll the dup on a detached thread.
static void start_parent_death_watch (int fd) {
    if (fd < 0) {
        return;
    }
    const int dupfd = dup (fd);
    if (dupfd < 0) {
        return;
    }
    std::thread ([dupfd] () {
        // events=0: POLLERR/POLLHUP/POLLNVAL are reported regardless of requested events,
        // and we must NOT request POLLOUT (a pipe write end is almost always writable, which
        // would busy-spin).
        struct pollfd pfd { dupfd, 0, 0 };
        while (!g_ipc_stop.load ()) {
            const int r = poll (&pfd, 1, 1000);
            if (r > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                // Read end gone → daemon died. Don't try to tear down gracefully (the
                // render/CEF state may be mid-frame); just exit so we stop rendering.
                _exit (0);
            }
        }
        close (dupfd);
    }).detach ();
}

static void initLogging () {
    sLog.addOutput (new std::ostream (std::cout.rdbuf ()));
    sLog.addError  (new std::ostream (std::cerr.rdbuf ()));
}

int main (int argc, char* argv[]) {
    tzset ();
    try {
        // Detect CEF subprocess invocation (--type=zygote / --type=utility).
        // When browser_subprocess_path is set to lwe-cef-subprocess this should
        // never trigger, but keep it as a safety net.
        bool enableLogging = true;
        bool isUiWindow = false;
        bool isSubprocess = false;
        for (int i = 1; i < argc; i++) {
            if (strncmp ("--type=", argv[i], 7) == 0) {
                enableLogging = false;
                isSubprocess = true;
            }
            if (strncmp ("--ui-window", argv[i], 11) == 0) {
                isUiWindow = true;
            }
        }
        if (enableLogging) initLogging ();

        if (isUiWindow || isSubprocess) {
            return run_ui_window(argc, argv);
        }

        // IPC configuration from environment
        const char* ctrl_sock_path = std::getenv ("WEPAPERED_CTRL_SOCK");
        const char* ready_fd_str   = std::getenv ("WEPAPERED_READY_FD");
        int ready_fd = ready_fd_str ? std::atoi (ready_fd_str) : -1;

        // Self-exit if the daemon dies (orphan protection). Uses the READY pipe; harmless
        // when launched standalone (no WEPAPERED_READY_FD → fd is -1 → no-op).
        start_parent_death_watch (ready_fd);

        // Open control socket if requested
        int srv_fd = -1;
        std::thread ipc_th;

        if (ctrl_sock_path && *ctrl_sock_path) {
            unlink (ctrl_sock_path);
            srv_fd = socket (AF_UNIX, SOCK_STREAM, 0);
            if (srv_fd >= 0) {
                struct sockaddr_un sa {};
                sa.sun_family = AF_UNIX;
                strncpy (sa.sun_path, ctrl_sock_path, sizeof (sa.sun_path) - 1);
                if (bind (srv_fd, reinterpret_cast<sockaddr*> (&sa), sizeof (sa)) == 0 &&
                    listen (srv_fd, 4) == 0) {
                    ipc_th = std::thread (ipc_thread_func, srv_fd);
                } else {
                    close (srv_fd);
                    srv_fd = -1;
                }
            }
        }

        // Mutable argv copy so we can patch --bg for hot-swap iterations
        std::vector<std::string> args (argv, argv + argc);
        int reply_fd_for_this_iter = -1;

        while (true) {
            std::vector<char*> av;
            av.reserve (args.size ());
            for (auto& s : args) av.push_back (const_cast<char*> (s.c_str ()));

            Ctx ctx (static_cast<int> (av.size ()), av.data ());
            ctx.loadSettingsFromArgv ();

            bool wallpaperOk = false;
            try {
                auto* a = new App (ctx);
                g_app.store (a);

                if (ctx.settings.general.onlyListProperties) {
                    delete a;
                    g_app.store (nullptr);
                    break;
                }

                std::signal (SIGINT,  signalhandler);
                std::signal (SIGTERM, signalhandler);

                // Register the fd to write "READY" when the first frame is actually
                // rendered (from WaylandOpenGLDriver or RenderHandler::OnPaint).
                // This is more accurate than writing before show() — CEF wallpapers
                // can take many seconds before their first frame appears.
                int signal_fd = (ready_fd >= 0) ? ready_fd : reply_fd_for_this_iter;
                lwe_set_first_frame_fd (signal_fd);
                ready_fd = -1;
                reply_fd_for_this_iter = -1;

                a->show ();
                wallpaperOk = true;

                std::signal (SIGINT,  SIG_DFL);
                std::signal (SIGTERM, SIG_DFL);
                g_app.store (nullptr);
                delete a;
            } catch (const std::exception& e) {
                // Bad wallpaper (parse error, missing file, etc.) — log and exit.
                // Do NOT let the exception propagate: ipc_th is joinable and
                // destroying it without join calls std::terminate().
                std::cerr << e.what () << std::endl;
                std::signal (SIGINT,  SIG_DFL);
                std::signal (SIGTERM, SIG_DFL);
                g_app.store (nullptr);
                break;
            }

            if (!wallpaperOk || g_ipc_stop.load ()) break;

            // Check for pending hot-swap
            IpcLoad nextLoad;
            int rep_fd = -1;
            {
                std::lock_guard<std::mutex> lk (g_ipc_mu);
                nextLoad       = std::move (g_ipc_next_load);
                rep_fd         = g_ipc_reply_fd;
                g_ipc_reply_fd = -1;
                g_ipc_next_load = {};
            }

            // Natural exit with no pending command → leave the loop
            if (!nextLoad.valid || nextLoad.bg.empty ()) break;

            // Hot-swap: patch --bg for next iteration
            for (std::size_t i = 1; i + 1 < args.size (); i++) {
                if (args [i] == "--bg") { args [i + 1] = nextLoad.bg; break; }
            }

            // Strip stale --preset-dir and --set-property entries, then add new ones
            {
                std::vector<std::string> cleaned;
                cleaned.push_back (args [0]);
                for (std::size_t i = 1; i < args.size (); i++) {
                    if (args[i] == "--preset-dir" && i + 1 < args.size ()) { i++; continue; }
                    if ((args[i] == "--set-property" || args[i] == "--property") && i + 1 < args.size ()) {
                        i++; continue;
                    }
                    cleaned.push_back (args[i]);
                }
                args = std::move (cleaned);
            }
            if (!nextLoad.presetDir.empty ()) {
                args.push_back ("--preset-dir");
                args.push_back (nextLoad.presetDir);
            }
            for (const auto& [k, v] : nextLoad.props) {
                args.push_back ("--set-property");
                args.push_back (k + "=" + v);
            }

            reply_fd_for_this_iter = rep_fd;
        }

        // Cleanup
        g_ipc_stop.store (true);
        if (srv_fd >= 0) {
            close (srv_fd);
            if (ctrl_sock_path) unlink (ctrl_sock_path);
        }
        if (ipc_th.joinable ()) ipc_th.join ();

        return 0;

    } catch (const std::exception& e) {
        std::cerr << e.what () << std::endl;
        return 1;
    }
}
