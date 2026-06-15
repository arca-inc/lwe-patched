#include "WaylandMouseInput.h"
#include "WallpaperEngine/Render/Drivers/WaylandOpenGLDriver.h"
#include <glm/common.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

using namespace WallpaperEngine::Input::Drivers;

namespace {
const WallpaperEngine::Render::Drivers::Output::WaylandOutputViewport* getActiveViewport (
    const WallpaperEngine::Render::Drivers::WaylandOpenGLDriver& driver
) {
    if (driver.viewportInFocus && driver.viewportInFocus->rendering) {
	return driver.viewportInFocus;
    }

    for (const auto* viewport : driver.m_screens) {
	if (viewport && viewport->rendering) {
	    return viewport;
	}
    }

    return nullptr;
}
}

WaylandMouseInput::WaylandMouseInput (const WallpaperEngine::Render::Drivers::WaylandOpenGLDriver& driver) :
    m_waylandDriver (driver), m_running (true), m_cursorX (0.0), m_cursorY (0.0) {
    
    this->m_pollingThread = std::thread ([this] () {
        while (this->m_running) {
            if (auto cursor = this->queryHyprlandCursorPosition ()) {
                this->m_cursorX.store (cursor->x, std::memory_order_relaxed);
                this->m_cursorY.store (cursor->y, std::memory_order_relaxed);
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (16));
        }
    });
}

WaylandMouseInput::~WaylandMouseInput () {
    this->m_running = false;
    if (this->m_pollingThread.joinable ()) {
        this->m_pollingThread.join ();
    }
}

void WaylandMouseInput::update () {
    if (!this->m_waylandDriver.getApp ().getContext ().settings.mouse.enabled) {
	this->m_pos = { 0, 0 };
	return;
    }

    if (m_waylandDriver.viewportInFocus && m_waylandDriver.viewportInFocus->rendering) {
	this->m_pos = m_waylandDriver.viewportInFocus->mousePos;
	return;
    }

    glm::dvec2 globalCursor = { this->m_cursorX.load (std::memory_order_relaxed),
                                this->m_cursorY.load (std::memory_order_relaxed) };

    for (const auto* viewport : this->m_waylandDriver.m_screens) {
	if (!viewport) {
	    continue;
	}

	// Hyprland reports the cursor in the global *logical* coordinate space, so the
	// per-output offset and bounds must use the output's logical position/size.
	// (viewport->position is never populated — only globalPosition is, via the
	// xdg-output/geometry handlers — which is why every output used to appear at
	// (0,0) and the first one tall enough to contain the cursor was picked, mapping
	// the Y against the wrong monitor height and reporting the cursor too low.)
	const glm::ivec2 logicalSize
	    = (viewport->logicalSize.x > 0 && viewport->logicalSize.y > 0) ? viewport->logicalSize : viewport->size;
	if (logicalSize.x <= 0 || logicalSize.y <= 0) {
	    continue;
	}

	const double localX = globalCursor.x - viewport->globalPosition.x;
	const double localY = globalCursor.y - viewport->globalPosition.y;
	if (localX < 0.0 || localY < 0.0 || localX > logicalSize.x || localY > logicalSize.y) {
	    continue;
	}

	// Normalize within this output, flip Y to GL's bottom-left origin, then express in
	// physical framebuffer pixels (size * scale) to match the GL viewport downstream.
	const double nx = localX / static_cast<double> (logicalSize.x);
	const double ny = localY / static_cast<double> (logicalSize.y);
	this->m_pos
	    = { nx * viewport->size.x * viewport->scale, (1.0 - ny) * viewport->size.y * viewport->scale };
	// Opt-in stderr trace (visible even under --silent) for diagnosing
	// multi-monitor cursor mapping.
	static const bool debugMouse = std::getenv ("LWE_DEBUG_MOUSE") != nullptr;
	if (debugMouse) {
	    fprintf (
		stderr, "[mouse] global=(%.0f,%.0f) -> %s globalPos=(%d,%d) local=(%.0f,%.0f) m_pos=(%.0f,%.0f)\n",
		globalCursor.x, globalCursor.y, viewport->name.c_str (), viewport->globalPosition.x,
		viewport->globalPosition.y, localX, localY, this->m_pos.x, this->m_pos.y);
	}
	return;
    }

    this->m_pos = { 0, 0 };
}

glm::dvec2 WaylandMouseInput::position () const {
    if (!this->m_waylandDriver.getApp ().getContext ().settings.mouse.enabled) {
	return { 0, 0 };
    }

    return this->m_pos;
}

WallpaperEngine::Input::MouseClickStatus WaylandMouseInput::leftClick () const {
    const auto* viewport = getActiveViewport (m_waylandDriver);
    if (viewport) {
	return viewport->leftClick;
    }

    return MouseClickStatus::Released;
}

std::optional<glm::dvec2> WaylandMouseInput::queryHyprlandCursorPosition () const {
    const char* signature = std::getenv ("HYPRLAND_INSTANCE_SIGNATURE");
    const char* runtime = std::getenv ("XDG_RUNTIME_DIR");
    if (!signature || !runtime) {
	return std::nullopt;
    }

    const std::string socketPath = std::string (runtime) + "/hypr/" + signature + "/.socket.sock";

    int fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
	return std::nullopt;
    }

    timeval timeout {};
    timeout.tv_usec = 50000;
    if (
	setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof (timeout)) != 0
	|| setsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof (timeout)) != 0
    ) {
	close (fd);
	return std::nullopt;
    }

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (socketPath.size () >= sizeof (addr.sun_path)) {
	close (fd);
	return std::nullopt;
    }
    std::strncpy (addr.sun_path, socketPath.c_str (), sizeof (addr.sun_path) - 1);

    if (connect (fd, reinterpret_cast<sockaddr*> (&addr), sizeof (addr)) != 0) {
	close (fd);
	return std::nullopt;
    }

    constexpr const char* request = "j/cursorpos";
    if (send (fd, request, std::strlen (request), 0) < 0) {
	close (fd);
	return std::nullopt;
    }
    shutdown (fd, SHUT_WR);

    std::string response;
    char buffer[256];
    ssize_t readBytes = 0;
    while ((readBytes = recv (fd, buffer, sizeof (buffer), 0)) > 0) {
	response.append (buffer, static_cast<std::size_t> (readBytes));
    }
    close (fd);

    static const std::regex xRegex (R"("x"\s*:\s*(-?\d+(?:\.\d+)?))");
    static const std::regex yRegex (R"("y"\s*:\s*(-?\d+(?:\.\d+)?))");
    std::smatch xMatch;
    std::smatch yMatch;
    if (!std::regex_search (response, xMatch, xRegex) || !std::regex_search (response, yMatch, yRegex)) {
	return std::nullopt;
    }

    try {
	return glm::dvec2 { std::stod (xMatch[1].str ()), std::stod (yMatch[1].str ()) };
    } catch (const std::exception&) {
	return std::nullopt;
    }
}

WallpaperEngine::Input::MouseClickStatus WaylandMouseInput::rightClick () const {
    const auto* viewport = getActiveViewport (m_waylandDriver);
    if (viewport) {
	return viewport->rightClick;
    }

    return MouseClickStatus::Released;
}
