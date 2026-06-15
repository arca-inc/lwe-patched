#include "ShaderCache.h"

#include "WallpaperEngine/Logging/Log.h"

#include <GL/glew.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace WallpaperEngine::Render::Shaders {

// ---------------------------------------------------------------------------
// FNV-1a 64-bit hash (fast, non-cryptographic, stable across runs)
// ---------------------------------------------------------------------------
static uint64_t fnv1a64 (const void* data, size_t len) {
    constexpr uint64_t basis = 14695981039346656037ULL;
    constexpr uint64_t prime = 1099511628211ULL;
    const auto* p = static_cast<const uint8_t*> (data);
    uint64_t hash = basis;
    for (size_t i = 0; i < len; ++i) {
	hash ^= p[i];
	hash *= prime;
    }
    return hash;
}

static uint64_t hashSources (const std::string& vert, const std::string& frag) {
    // Hash vert, a separator, then frag so we get a combined fingerprint.
    uint64_t h = fnv1a64 (vert.data (), vert.size ());
    constexpr uint8_t sep = 0xFF;
    h ^= fnv1a64 (&sep, 1);
    h ^= fnv1a64 (frag.data (), frag.size ());
    return h;
}

// ---------------------------------------------------------------------------
// Cache directory
// ---------------------------------------------------------------------------
std::filesystem::path ShaderCache::cacheDir () {
    std::filesystem::path base;
    if (const char* xdg = std::getenv ("XDG_CACHE_HOME"); xdg && *xdg) {
	base = xdg;
    } else if (const char* home = std::getenv ("HOME"); home && *home) {
	base = std::filesystem::path (home) / ".cache";
    } else {
	return {}; // cannot determine cache dir — disable cache
    }
    return base / "linux-wallpaperengine" / "programs";
}

std::string ShaderCache::cacheKey (const std::string& vert, const std::string& frag) {
    char buf[17];
    std::snprintf (buf, sizeof (buf), "%016llx", (unsigned long long) hashSources (vert, frag));
    return buf;
}

// ---------------------------------------------------------------------------
// tryLoad — returns true if a valid cached binary was loaded into programID
// ---------------------------------------------------------------------------
bool ShaderCache::tryLoad (const std::string& vert, const std::string& frag, GLuint& programID) {
    // Check that the driver supports program binaries at all
    GLint numFormats = 0;
    glGetIntegerv (GL_NUM_PROGRAM_BINARY_FORMATS, &numFormats);
    if (numFormats == 0) return false;

    const auto dir = cacheDir ();
    if (dir.empty ()) return false;

    const std::string key = cacheKey (vert, frag);
    const auto binFile = dir / (key + ".bin");
    const auto fmtFile = dir / (key + ".fmt");

    if (!std::filesystem::exists (binFile) || !std::filesystem::exists (fmtFile)) return false;

    // Read the binary format enum
    GLenum binaryFormat = 0;
    {
	std::ifstream f (fmtFile, std::ios::binary);
	if (!f) return false;
	f.read (reinterpret_cast<char*> (&binaryFormat), sizeof (binaryFormat));
    }

    // Read the binary blob
    std::vector<uint8_t> binary;
    {
	std::ifstream f (binFile, std::ios::binary | std::ios::ate);
	if (!f) return false;
	const auto size = f.tellg ();
	if (size <= 0) return false;
	binary.resize (static_cast<size_t> (size));
	f.seekg (0);
	f.read (reinterpret_cast<char*> (binary.data ()), size);
    }

    programID = glCreateProgram ();
    glProgramBinary (programID, binaryFormat, binary.data (), static_cast<GLsizei> (binary.size ()));

    GLint linked = GL_FALSE;
    glGetProgramiv (programID, GL_LINK_STATUS, &linked);
    if (linked == GL_FALSE) {
	// Cached binary is stale (driver update, etc.) — discard and fall through
	glDeleteProgram (programID);
	programID = 0;
	std::filesystem::remove (binFile);
	std::filesystem::remove (fmtFile);
	return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// save — serialise a linked program's binary to disk
// ---------------------------------------------------------------------------
void ShaderCache::save (const std::string& vert, const std::string& frag, GLuint programID) {
    GLint numFormats = 0;
    glGetIntegerv (GL_NUM_PROGRAM_BINARY_FORMATS, &numFormats);
    if (numFormats == 0) return;

    const auto dir = cacheDir ();
    if (dir.empty ()) return;

    // Hint the driver we'd like to retrieve the binary
    glProgramParameteri (programID, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);

    GLint binaryLength = 0;
    glGetProgramiv (programID, GL_PROGRAM_BINARY_LENGTH, &binaryLength);
    if (binaryLength <= 0) return;

    std::vector<uint8_t> binary (static_cast<size_t> (binaryLength));
    GLenum binaryFormat = 0;
    GLsizei actual = 0;
    glGetProgramBinary (programID, binaryLength, &actual, &binaryFormat, binary.data ());
    if (actual <= 0) return;

    std::error_code ec;
    std::filesystem::create_directories (dir, ec);
    if (ec) return;

    const std::string key = cacheKey (vert, frag);

    {
	std::ofstream f (dir / (key + ".fmt"), std::ios::binary | std::ios::trunc);
	if (!f) return;
	f.write (reinterpret_cast<const char*> (&binaryFormat), sizeof (binaryFormat));
    }
    {
	std::ofstream f (dir / (key + ".bin"), std::ios::binary | std::ios::trunc);
	if (!f) return;
	f.write (reinterpret_cast<const char*> (binary.data ()), actual);
    }
}

} // namespace WallpaperEngine::Render::Shaders
