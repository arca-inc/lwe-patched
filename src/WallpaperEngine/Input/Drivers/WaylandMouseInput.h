#pragma once

#ifdef ENABLE_WAYLAND

#include "WallpaperEngine/Input/MouseInput.h"

#include <glm/vec2.hpp>
#include <chrono>
#include <optional>
#include <thread>
#include <atomic>

namespace WallpaperEngine::Render::Drivers {
class WaylandOpenGLDriver;
};

namespace WallpaperEngine::Input::Drivers {
/**
 * Handles mouse input for the background
 */
class WaylandMouseInput final : public MouseInput {
public:
    explicit WaylandMouseInput (const WallpaperEngine::Render::Drivers::WaylandOpenGLDriver& driver);
    ~WaylandMouseInput() override;

    /**
     * Takes current mouse position and updates it
     */
    void update () override;

    /**
     * The virtual pointer's position
     */
    [[nodiscard]] glm::dvec2 position () const override;

    /**
     * @return The status of the mouse's left click
     */
    [[nodiscard]] MouseClickStatus leftClick () const override;

    /**
     * @return The status of the mouse's right click
     */
    [[nodiscard]] MouseClickStatus rightClick () const override;

private:
    [[nodiscard]] std::optional<glm::dvec2> queryHyprlandCursorPosition () const;

    /**
     * Wayland: Driver
     */
    const WallpaperEngine::Render::Drivers::WaylandOpenGLDriver& m_waylandDriver;

    glm::dvec2 m_pos = {};

    std::atomic<bool> m_running;
    std::atomic<double> m_cursorX;
    std::atomic<double> m_cursorY;
    std::thread m_pollingThread;
};
} // namespace WallpaperEngine::Input::Drivers

#endif /* ENABLE_WAYLAND */
