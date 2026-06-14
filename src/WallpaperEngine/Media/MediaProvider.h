#pragma once

#include <optional>
#include <string>

// Shared "now-playing" media provider. Wraps the MPRIS query (via the external
// `playerctl` binary) and album-art loading/palette extraction so both the scene
// scripting engine (ScriptEngine) and the CEF web backend can consume the same
// data without duplicating the subprocess/decode logic.
namespace WallpaperEngine::Media {

struct Rgb {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

// Palette extracted from the current track's album art. Defaults match the
// neutral placeholder colours the scene engine used before real extraction.
struct MediaColors {
    Rgb primary {0.12f, 0.12f, 0.12f};
    Rgb secondary {0.0f, 0.0f, 0.0f};
    Rgb tertiary {0.25f, 0.25f, 0.25f};
    Rgb text {1.0f, 1.0f, 1.0f};
    Rgb highContrast {1.0f, 1.0f, 1.0f};
};

// Parsed MPRIS state for the active player.
struct MediaInfo {
    bool available = false;
    int playbackState = 0; // 0 = stopped, 1 = playing, 2 = paused
    std::string status = "Stopped";
    std::string title;
    std::string artist;
    std::string album;
    std::string artUrl;
    double duration = 0.0; // seconds
    double position = 0.0; // seconds
};

struct ArtData {
    bool ok = false;
    MediaColors colors;
    std::string dataUrl; // "data:<mime>;base64,..." — only filled when requested
};

// Run `playerctl metadata` once and parse it. Returns std::nullopt when playerctl
// could not be spawned or timed out; a MediaInfo with available=false when there is
// no active player. This blocks on the subprocess (use a worker thread on hot paths).
std::optional<MediaInfo> pollMediaInfo ();

// Load the art at artUrl (file:// read directly, http(s):// fetched via curl), decode
// it and compute a palette. When wantDataUrl is true the original image bytes are also
// returned as a base64 data URL (suitable for an <img src>). ok=false when no art could
// be loaded. Blocking — call from a worker thread.
ArtData loadArt (const std::string& artUrl, bool wantDataUrl);

// JSON object literals for the Wallpaper Engine web media API, matching the fields the
// page's wallpaperRegisterMedia*Listener callbacks expect. Centralised here so the
// (fragile) web contract has a single definition shared by every web backend. Each
// returns the bare object, e.g. {"enabled":true} — wrap in window.__lweMedia.<m>(...).
std::string webStatusJson (bool enabled);
std::string webPropertiesJson (const MediaInfo& info);
std::string webPlaybackJson (int playbackState);
std::string webTimelineJson (double position, double duration);
std::string webThumbnailJson (const ArtData& art);

} // namespace WallpaperEngine::Media
