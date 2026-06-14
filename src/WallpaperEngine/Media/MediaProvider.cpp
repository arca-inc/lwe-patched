#include "MediaProvider.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <stb_image.h>

namespace {

using clock_type = std::chrono::steady_clock;

// Spawn `program` (PATH-resolved) with args, capture stdout, drop stderr. Returns the
// captured bytes, or std::nullopt if the process could not be spawned. Kills the child
// once the deadline elapses so a hung playerctl/curl never blocks the caller forever.
std::optional<std::vector<char>> spawnAndCapture (
    const std::string& program, const std::vector<std::string>& args, std::chrono::milliseconds deadline
) {
    int outPipe[2];
    if (pipe (outPipe) != 0) {
        return std::nullopt;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init (&actions);
    posix_spawn_file_actions_adddup2 (&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen (&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose (&actions, outPipe[0]);
    posix_spawn_file_actions_addclose (&actions, outPipe[1]);

    std::vector<char*> argv;
    argv.reserve (args.size () + 2);
    argv.push_back (const_cast<char*> (program.c_str ()));
    for (const auto& arg : args) {
        argv.push_back (const_cast<char*> (arg.c_str ()));
    }
    argv.push_back (nullptr);

    pid_t pid = 0;
    const int spawnResult = posix_spawnp (&pid, program.c_str (), &actions, nullptr, argv.data (), environ);
    posix_spawn_file_actions_destroy (&actions);
    close (outPipe[1]);

    if (spawnResult != 0) {
        close (outPipe[0]);
        return std::nullopt;
    }

    const int fd = outPipe[0];
    fcntl (fd, F_SETFL, O_NONBLOCK);

    std::vector<char> output;
    const auto start = clock_type::now ();
    bool eof = false;
    while (!eof) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (clock_type::now () - start);
        if (elapsed >= deadline) {
            break;
        }
        const int remaining = static_cast<int> ((deadline - elapsed).count ());
        pollfd pfd { fd, POLLIN, 0 };
        const int ready = poll (&pfd, 1, std::min (remaining, 50));
        if (ready < 0) {
            break;
        }
        if (ready == 0) {
            continue;
        }
        char buffer[8192];
        const ssize_t n = read (fd, buffer, sizeof (buffer));
        if (n > 0) {
            output.insert (output.end (), buffer, buffer + n);
        } else if (n == 0) {
            eof = true;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
    }
    close (fd);

    // Reap the child; kill it if it outlived the deadline.
    int status = 0;
    if (waitpid (pid, &status, WNOHANG) == 0) {
        kill (pid, SIGKILL);
        waitpid (pid, &status, 0);
    }

    return output;
}

std::string trim (const std::string& s) {
    size_t a = 0;
    size_t b = s.size ();
    while (a < b && (std::isspace (static_cast<unsigned char> (s[a])) != 0)) {
        ++a;
    }
    while (b > a && (std::isspace (static_cast<unsigned char> (s[b - 1])) != 0)) {
        --b;
    }
    return s.substr (a, b - a);
}

std::vector<std::string> splitTabs (const std::string& s) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t tab = s.find ('\t', start);
        if (tab == std::string::npos) {
            parts.push_back (s.substr (start));
            break;
        }
        parts.push_back (s.substr (start, tab - start));
        start = tab + 1;
    }
    return parts;
}

double parseDoubleOrZero (const std::string& s) {
    try {
        return s.empty () ? 0.0 : std::stod (s);
    } catch (...) {
        return 0.0;
    }
}

std::optional<std::filesystem::path> parseLocalFileUri (const std::string& uri) {
    if (uri.rfind ("file://", 0) != 0) {
        return std::nullopt;
    }
    std::string path = uri.substr (7);
    // Strip an optional host component ("file://host/path").
    if (!path.empty () && path[0] != '/') {
        const size_t slash = path.find ('/');
        if (slash == std::string::npos) {
            return std::nullopt;
        }
        path = path.substr (slash);
    }
    // Percent-decode.
    std::string decoded;
    decoded.reserve (path.size ());
    for (size_t i = 0; i < path.size (); ++i) {
        if (path[i] == '%' && i + 2 < path.size ()) {
            const auto hex = path.substr (i + 1, 2);
            decoded.push_back (static_cast<char> (std::strtol (hex.c_str (), nullptr, 16)));
            i += 2;
        } else {
            decoded.push_back (path[i]);
        }
    }
    return std::filesystem::path (decoded);
}

std::vector<char> fetchArtBytes (const std::string& artUrl) {
    if (const auto path = parseLocalFileUri (artUrl); path.has_value ()) {
        std::ifstream file (*path, std::ios::binary);
        if (!file) {
            return {};
        }
        return { std::istreambuf_iterator<char> (file), std::istreambuf_iterator<char> () };
    }
    if (artUrl.rfind ("http://", 0) == 0 || artUrl.rfind ("https://", 0) == 0) {
        const auto bytes = spawnAndCapture (
            "curl", { "-L", "-s", "--max-time", "3", "--output", "-", artUrl }, std::chrono::milliseconds (3500)
        );
        return bytes.value_or (std::vector<char> {});
    }
    return {};
}

const char* detectMime (const std::vector<char>& b) {
    const auto* d = reinterpret_cast<const unsigned char*> (b.data ());
    const size_t n = b.size ();
    if (n >= 8 && d[0] == 0x89 && d[1] == 0x50 && d[2] == 0x4E && d[3] == 0x47) {
        return "image/png";
    }
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) {
        return "image/jpeg";
    }
    if (n >= 6 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F') {
        return "image/gif";
    }
    if (n >= 12 && std::memcmp (d, "RIFF", 4) == 0 && std::memcmp (d + 8, "WEBP", 4) == 0) {
        return "image/webp";
    }
    return "image/png";
}

std::string base64Encode (const std::vector<char>& in) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve (((in.size () + 2) / 3) * 4);
    size_t i = 0;
    const auto* d = reinterpret_cast<const unsigned char*> (in.data ());
    for (; i + 2 < in.size (); i += 3) {
        const unsigned v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
        out.push_back (table[(v >> 18) & 0x3F]);
        out.push_back (table[(v >> 12) & 0x3F]);
        out.push_back (table[(v >> 6) & 0x3F]);
        out.push_back (table[v & 0x3F]);
    }
    if (i < in.size ()) {
        const unsigned b0 = d[i];
        const unsigned b1 = (i + 1 < in.size ()) ? d[i + 1] : 0;
        const unsigned v = (b0 << 16) | (b1 << 8);
        out.push_back (table[(v >> 18) & 0x3F]);
        out.push_back (table[(v >> 12) & 0x3F]);
        out.push_back ((i + 1 < in.size ()) ? table[(v >> 6) & 0x3F] : '=');
        out.push_back ('=');
    }
    return out;
}

float luminance (const WallpaperEngine::Media::Rgb& c) {
    return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
}

std::string escapeJson (const std::string& s) {
    std::string out;
    out.reserve (s.size () + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char> (c) < 0x20) {
                    char buf[8];
                    std::snprintf (buf, sizeof (buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Hex CSS colour ("#rrggbb"). The WE web API hands colours straight to CSS
// (element.style background/color), so they must be valid CSS colour strings.
std::string toHex (const WallpaperEngine::Media::Rgb& c) {
    auto channel = [] (float v) {
        return static_cast<int> (std::lround (std::clamp (v, 0.0f, 1.0f) * 255.0f));
    };
    char buf[8];
    std::snprintf (buf, sizeof (buf), "#%02x%02x%02x", channel (c.r), channel (c.g), channel (c.b));
    return buf;
}

std::string fmtSeconds (double v) {
    char buf[32];
    std::snprintf (buf, sizeof (buf), "%.3f", v);
    return buf;
}

// Build a palette from decoded RGBA pixels: histogram coarse colour buckets, pick the
// most prominent vivid bucket as primary and the most distinct further buckets as
// secondary/tertiary; derive text/highContrast as black or white for legibility.
WallpaperEngine::Media::MediaColors computeColors (const unsigned char* px, int w, int h) {
    using WallpaperEngine::Media::MediaColors;
    using WallpaperEngine::Media::Rgb;
    MediaColors colors;
    if (px == nullptr || w <= 0 || h <= 0) {
        return colors;
    }

    struct Bucket {
        double r = 0, g = 0, b = 0;
        long count = 0;
    };
    std::map<int, Bucket> buckets;
    long total = 0;

    // Cap the number of sampled pixels so large art doesn't stall the worker.
    const long pixels = static_cast<long> (w) * h;
    const long stride = std::max<long> (1, pixels / 4096);
    for (long p = 0; p < pixels; p += stride) {
        const unsigned char* s = px + p * 4;
        if (s[3] < 16) {
            continue; // skip near-transparent
        }
        const double r = s[0] / 255.0;
        const double g = s[1] / 255.0;
        const double b = s[2] / 255.0;
        ++total;
        // 4 bits per channel => 4096 buckets.
        const int key = ((s[0] >> 4) << 8) | ((s[1] >> 4) << 4) | (s[2] >> 4);
        auto& bk = buckets[key];
        bk.r += r;
        bk.g += g;
        bk.b += b;
        ++bk.count;
    }

    if (total == 0) {
        return colors;
    }

    struct Cand {
        Rgb color;
        double score;
    };
    std::vector<Cand> cands;
    cands.reserve (buckets.size ());
    for (const auto& [key, bk] : buckets) {
        Rgb c { static_cast<float> (bk.r / bk.count), static_cast<float> (bk.g / bk.count),
                static_cast<float> (bk.b / bk.count) };
        const float mx = std::max ({ c.r, c.g, c.b });
        const float mn = std::min ({ c.r, c.g, c.b });
        const float sat = mx > 0.0001f ? (mx - mn) / mx : 0.0f;
        // Favour frequent, vivid colours over washed-out background tones.
        const double score = static_cast<double> (bk.count) * (0.25 + sat);
        cands.push_back ({ c, score });
    }
    std::sort (cands.begin (), cands.end (), [] (const Cand& a, const Cand& b) { return a.score > b.score; });

    auto dist = [] (const Rgb& a, const Rgb& b) {
        const float dr = a.r - b.r;
        const float dg = a.g - b.g;
        const float db = a.b - b.b;
        return std::sqrt (dr * dr + dg * dg + db * db);
    };

    colors.primary = cands[0].color;
    // secondary: most prominent candidate sufficiently distinct from primary.
    for (const auto& cand : cands) {
        if (dist (cand.color, colors.primary) > 0.25f) {
            colors.secondary = cand.color;
            break;
        }
    }
    // tertiary: distinct from both primary and secondary.
    for (const auto& cand : cands) {
        if (dist (cand.color, colors.primary) > 0.25f && dist (cand.color, colors.secondary) > 0.25f) {
            colors.tertiary = cand.color;
            break;
        }
    }

    const Rgb bw = luminance (colors.primary) > 0.55f ? Rgb { 0.0f, 0.0f, 0.0f } : Rgb { 1.0f, 1.0f, 1.0f };
    colors.text = bw;
    colors.highContrast = bw;
    return colors;
}

} // namespace

namespace WallpaperEngine::Media {

std::optional<MediaInfo> pollMediaInfo () {
    // Preferred player priority list (e.g. "spotify,%any"), forwarded to
    // `playerctl --player=`. Empty = playerctl's default selection.
    std::vector<std::string> args;
    if (const char* player = std::getenv ("LWE_MEDIA_PLAYER"); player != nullptr && player[0] != '\0') {
        args.emplace_back (std::string ("--player=") + player);
    }
    args.emplace_back ("metadata");
    args.emplace_back ("--format");
    args.emplace_back ("{{status}}\t{{title}}\t{{artist}}\t{{album}}\t{{mpris:length}}\t{{position}}\t{{mpris:artUrl}}");

    const auto captured = spawnAndCapture ("playerctl", args, std::chrono::milliseconds (400));
    if (!captured.has_value ()) {
        return std::nullopt;
    }

    std::string line (captured->begin (), captured->end ());
    if (const size_t nl = line.find ('\n'); nl != std::string::npos) {
        line = line.substr (0, nl);
    }
    line = trim (line);

    MediaInfo info;
    if (line.empty ()) {
        info.available = false;
        info.playbackState = 0;
        info.status = "Stopped";
        return info;
    }

    const auto parts = splitTabs (line);
    info.available = true;
    info.status = parts.size () > 0 ? parts[0] : "";
    info.title = parts.size () > 1 ? parts[1] : "";
    info.artist = parts.size () > 2 ? parts[2] : "";
    info.album = parts.size () > 3 ? parts[3] : "";
    info.duration = (parts.size () > 4 ? parseDoubleOrZero (parts[4]) : 0.0) / 1000000.0;
    info.position = (parts.size () > 5 ? parseDoubleOrZero (parts[5]) : 0.0) / 1000000.0;
    info.artUrl = parts.size () > 6 ? parts[6] : "";

    if (info.status == "Playing") {
        info.playbackState = 1;
    } else if (info.status == "Paused") {
        info.playbackState = 2;
    } else {
        info.playbackState = 0;
    }
    return info;
}

ArtData loadArt (const std::string& artUrl, bool wantDataUrl) {
    ArtData result;
    if (artUrl.empty ()) {
        return result;
    }

    const std::vector<char> bytes = fetchArtBytes (artUrl);
    if (bytes.empty ()) {
        return result;
    }

    int w = 0;
    int h = 0;
    int comp = 0;
    unsigned char* px = stbi_load_from_memory (
        reinterpret_cast<const stbi_uc*> (bytes.data ()), static_cast<int> (bytes.size ()), &w, &h, &comp, 4
    );
    if (px == nullptr) {
        return result;
    }
    result.colors = computeColors (px, w, h);
    stbi_image_free (px);
    result.ok = true;

    if (wantDataUrl) {
        result.dataUrl = std::string ("data:") + detectMime (bytes) + ";base64," + base64Encode (bytes);
    }
    return result;
}

std::string webStatusJson (bool enabled) {
    return std::string ("{\"enabled\":") + (enabled ? "true" : "false") + "}";
}

std::string webPropertiesJson (const MediaInfo& info) {
    std::string out = "{";
    out += "\"title\":\"" + escapeJson (info.title) + "\",";
    out += "\"artist\":\"" + escapeJson (info.artist) + "\",";
    out += "\"subTitle\":\"\",";
    out += "\"albumTitle\":\"" + escapeJson (info.album) + "\",";
    out += "\"albumArtist\":\"" + escapeJson (info.artist) + "\",";
    out += "\"genres\":\"\",";
    out += "\"contentType\":\"music\"}";
    return out;
}

std::string webPlaybackJson (int playbackState) {
    return std::string ("{\"state\":") + std::to_string (playbackState) + "}";
}

std::string webTimelineJson (double position, double duration) {
    return std::string ("{\"position\":") + fmtSeconds (position) + ",\"duration\":" + fmtSeconds (duration) + "}";
}

std::string webThumbnailJson (const ArtData& art) {
    std::string out = "{";
    out += "\"thumbnail\":\"" + escapeJson (art.dataUrl) + "\",";
    out += "\"primaryColor\":\"" + toHex (art.colors.primary) + "\",";
    out += "\"secondaryColor\":\"" + toHex (art.colors.secondary) + "\",";
    out += "\"tertiaryColor\":\"" + toHex (art.colors.tertiary) + "\",";
    out += "\"textColor\":\"" + toHex (art.colors.text) + "\",";
    out += "\"highContrastColor\":\"" + toHex (art.colors.highContrast) + "\"}";
    return out;
}

} // namespace WallpaperEngine::Media
