#pragma once

#include <GL/glew.h>
#include <filesystem>
#include <string>

namespace WallpaperEngine::Render::Shaders {

/**
 * Disk-backed cache for compiled OpenGL program binaries.
 *
 * On a cache hit the driver's glCompileShader + glLinkProgram are skipped entirely,
 * which eliminates the per-wallpaper 0.5-2 s startup stall caused by GPU-side GLSL
 * compilation.  The cache is keyed by a 64-bit FNV-1a hash of the concatenated
 * vertex + fragment GLSL source strings; a GPU driver upgrade automatically
 * invalidates all entries because the binary format enum changes.
 *
 * Cache directory: $XDG_CACHE_HOME/linux-wallpaperengine/programs/
 *                  (~/.cache/linux-wallpaperengine/programs/ if XDG_CACHE_HOME unset)
 */
class ShaderCache {
public:
    /**
     * Try to load a previously cached GL program for the given vertex/fragment GLSL pair.
     *
     * @param vertSrc  Final compiled vertex GLSL (after ShaderUnit::compile()).
     * @param fragSrc  Final compiled fragment GLSL.
     * @param programID Out: the newly created GL program ID on success.
     * @return true if the cache hit succeeded and programID is ready to use.
     */
    static bool tryLoad (const std::string& vertSrc, const std::string& fragSrc, GLuint& programID);

    /**
     * Serialize a successfully linked GL program to the disk cache.
     *
     * @param vertSrc  Vertex GLSL used to key the entry.
     * @param fragSrc  Fragment GLSL used to key the entry.
     * @param programID The linked GL program to cache.
     */
    static void save (const std::string& vertSrc, const std::string& fragSrc, GLuint programID);

private:
    static std::filesystem::path cacheDir ();
    static std::string cacheKey (const std::string& vertSrc, const std::string& fragSrc);
};

} // namespace WallpaperEngine::Render::Shaders
