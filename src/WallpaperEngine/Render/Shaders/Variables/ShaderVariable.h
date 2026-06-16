#pragma once

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Utils/TypeCaster.h"
#include <exception>
#include <string>

namespace WallpaperEngine::Render::Shaders::Variables {
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Utils;

class ShaderVariable : public DynamicValue, public TypeCaster {
public:
    using DynamicValue::DynamicValue;

    [[nodiscard]] const std::string& getIdentifierName () const;
    [[nodiscard]] const std::string& getName () const;

    void setIdentifierName (std::string identifierName);
    void setName (const std::string& name);

    // Wallpaper Engine flags some uniforms with "position":true in their shader
    // metadata. Their x component is stored normalized to height and must be
    // multiplied by the scene aspect ratio (width/height) before being handed to
    // the shader, otherwise circular shapes built from them come out elliptical.
    [[nodiscard]] bool isPosition () const { return m_position; }
    void setPosition (const bool position) { m_position = position; }

private:
    std::string m_identifierName;
    std::string m_name;
    bool m_position = false;
};
} // namespace WallpaperEngine::Render::Shaders::Variables
