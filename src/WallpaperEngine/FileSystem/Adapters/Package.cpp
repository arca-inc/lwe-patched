#include <fstream>
#include <memory>

#include "Package.h"

#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Data/Parsers/PackageParser.h"
#include "WallpaperEngine/Data/Utils/BinaryReader.h"
#include "WallpaperEngine/Data/Utils/MemoryStream.h"

#include <algorithm>
#include <cctype>

using namespace WallpaperEngine::FileSystem;
using namespace WallpaperEngine::FileSystem::Adapters;

namespace {
bool iequals (const std::string& a, const std::string& b) {
    return a.size () == b.size ()
	&& std::equal (a.begin (), a.end (), b.begin (), [] (unsigned char x, unsigned char y) {
	       return std::tolower (x) == std::tolower (y);
	   });
}

// Locate a package entry by name. Wallpaper Engine wallpapers are authored on
// case-insensitive Windows, so a scene routinely references 'models/background.json'
// while the package stores 'models/Background.json'. Try an exact match first (the
// common, well-formed case), then fall back to a case-insensitive match so those
// wallpapers load on Linux's case-sensitive filesystem instead of failing.
template <typename Files>
const WallpaperEngine::Data::Assets::FileEntry* findEntry (const Files& files, const std::string& name) {
    for (const auto& file : files) {
	if (file->filename == name) {
	    return file.get ();
	}
    }
    for (const auto& file : files) {
	if (iequals (file->filename, name)) {
	    return file.get ();
	}
    }
    return nullptr;
}
} // namespace

ReadStreamSharedPtr PackageAdapter::open (const std::filesystem::path& path) const {
    // find the file entry
    const auto* entry = findEntry (this->package->files, path.string ());

    if (entry == nullptr) {
	throw std::filesystem::filesystem_error ("Cannot find file", path, std::error_code ());
    }

    // read file into memory
    auto buffer = std::make_unique<char[]> (entry->length);

    // go to the file's position and read into the buffer
    this->package->file->base ().seekg (entry->offset + this->package->baseOffset, std::ios::beg);
    this->package->file->next (buffer.get (), entry->length);

    // create a memory stream and return that
    return std::make_shared<MemoryStream> (std::move (buffer), entry->length);
}

bool PackageAdapter::exists (const std::filesystem::path& path) const {
    return findEntry (this->package->files, path.string ()) != nullptr;
}

std::filesystem::path PackageAdapter::physicalPath (const std::filesystem::path& path) const {
    throw std::filesystem::filesystem_error ("Package adapter does not support realpath", path, std::error_code ());
}

bool PackageFactory::handlesMountpoint (const std::filesystem::path& path) const {
    const auto finalpath = std::filesystem::canonical (path);
    const auto status = std::filesystem::status (finalpath);

    return std::filesystem::exists (finalpath) && std::filesystem::is_regular_file (status)
	&& finalpath.extension () == ".pkg";
}

AdapterSharedPtr PackageFactory::create (const std::filesystem::path& path) const {
    const auto stream = std::make_shared<std::ifstream> (path, std::ios::binary);
    auto package = Data::Parsers::PackageParser::parse (stream);

    return std::make_unique<PackageAdapter> (std::move (package));
}
