#pragma once

#include "../Utils/TypeCaster.h"
#include "DynamicValue.h"
#include <map>
#include <string>
#include <utility>

namespace WallpaperEngine::Data::Model {
using namespace WallpaperEngine::Data::Utils;
using namespace WallpaperEngine::Data::Builders;

struct PropertyData {
    std::string name;
    std::string text;
};

struct SliderData {
    float min;
    float max;
    float step;
};

struct ComboData {
    std::map<std::string, std::string> values;
};

class Property : public DynamicValue, public TypeCaster, public PropertyData {
public:
    explicit Property (PropertyData data) : DynamicValue (), TypeCaster (), PropertyData (std::move (data)) { }

    using DynamicValue::update;
    virtual void update (const std::string& value) = 0;
    [[nodiscard]] virtual std::string dump () const = 0;
};

class PropertySlider final : public Property, SliderData {
public:
    PropertySlider (PropertyData data, SliderData sliderData, const float value) :
	Property (std::move (data)), SliderData (std::move (sliderData)) {
	this->Property::update (value);
    }

    using Property::update;
    void update (const std::string& value) override { this->update (std::stof (value)); }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - slider" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tMin: " << this->min << std::endl
	   << "\tMax: " << this->max << std::endl
	   << "\tStep: " << this->step << std::endl
	   << "\tValue: " << this->toString () << std::endl;

	return ss.str ();
    }
};

class PropertyBoolean final : public Property {
public:
    explicit PropertyBoolean (PropertyData data, const bool value) : Property (std::move (data)) {
	this->Property::update (value);
    }

    using Property::update;
    void update (const std::string& value) override { this->update (value == "true" || value == "1"); }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - boolean" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->toString () << std::endl;

	return ss.str ();
    }
};

class PropertyColor final : public Property {
public:
    explicit PropertyColor (PropertyData data, const std::string& value) : Property (std::move (data)) {
	this->PropertyColor::update (value);
    }

    using Property::update;
    void update (const std::string& value) override {
	auto copy = value;

	// TODO: ENSURE ALL THIS PARSING IS CORRECT
	if (copy.find (',') != std::string::npos) {
	    // replace comma separator with spaces so it's
	    std::ranges::replace (copy, ',', ' ');
	}

	// hex colors should be converted to int colors
	if (copy.find ('#') == 0) {
	    auto number = copy.substr (1);

	    // support for css notation
	    if (number.size () == 3) {
		number = number[0] + number[0] + number[1] + number[1] + number[2] + number[2];
	    }

	    // remove alpha if it's present, should look into it more closely
	    if (number.size () > 6) {
		sLog.error ("Color value has alpha channel, which is not supported");
		number = number.substr (0, 6);
	    }

	    const auto color = std::stoi (number, nullptr, 16);

	    // format the number as float vector
	    copy = std::to_string (((color >> 16) & 0xFF) / 255.0) + " "
		+ std::to_string (((color >> 8) & 0xFF) / 255.0) + " " + std::to_string ((color & 0xFF) / 255.0);
	} else {
	    // Non-hex components, which may be 0-1 floats (how Wallpaper Engine stores
	    // colors) or 0-255 integers. The old "no decimal point ⇒ 0-255" heuristic
	    // misfired on whole-number floats: pure white "1 1 1" was divided by 255 to
	    // ~black, so a white clock turned invisible (the red glow behind it showed
	    // through, looking "red"). Decide by magnitude instead — only normalize when
	    // a component actually exceeds 1. Widen to vec4 with full alpha (the vec3
	    // overload stores w=0, which would propagate alpha 0 to bound colors and make
	    // the layer fully transparent); colors are always opaque.
	    glm::vec3 parsed = VectorBuilder::parse<glm::vec3> (copy);
	    if (parsed.x > 1.0f || parsed.y > 1.0f || parsed.z > 1.0f) {
		parsed /= 255.0f;
	    }
	    this->update (glm::vec4 (parsed, 1.0f));
	    return;
	}

	// Hex path: `copy` now holds a normalized "r g b" float string.
	this->update (glm::vec4 (VectorBuilder::parse<glm::vec3> (copy), 1.0f));
    }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - color" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->toString () << std::endl;

	return ss.str ();
    }
};

class PropertyCombo final : public Property, ComboData {
public:
    PropertyCombo (PropertyData data, ComboData comboData, const std::string& value) :
	Property (std::move (data)), ComboData (std::move (comboData)) {
	this->PropertyCombo::update (value);
    }

    using Property::update;
    void update (const std::string& value) override {
	if (this->values.contains (value) == false) {
	    sLog.error ("Combo value not found in combo options: ", value);
	    return;
	}

	// search for the value in the combo options or default to the textual value
	this->DynamicValue::update (value);
    }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - combo" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->toString () << std::endl
	   << "Values: " << std::endl;

	for (const auto& [key, value] : this->values) {
	    ss << "\t\t" << key << " = " << value << std::endl;
	}

	return ss.str ();
    }
};

class PropertyText final : public Property {
public:
    explicit PropertyText (PropertyData data) : Property (std::move (data)) { }

    using Property::update;
    void update (const std::string& value) override {
	throw std::runtime_error ("PropertyText::update() is not implemented");
    }

    [[nodiscard]] std::string toString () const override { return this->text; }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - text" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->toString () << std::endl;

	return ss.str ();
    }
};

class PropertySceneTexture final : public Property {
public:
    explicit PropertySceneTexture (PropertyData data, const std::string& value) : Property (std::move (data)) {
	this->PropertySceneTexture::update (value);
    }

    void update (const std::string& value) override { this->m_value = value; }

    [[nodiscard]] std::string toString () const override { return this->m_value; }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - scene texture" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->m_value << std::endl;

	return ss.str ();
    }

private:
    std::string m_value;
};

class PropertyFile final : public Property {
public:
    explicit PropertyFile (PropertyData data, const std::string& value) : Property (std::move (data)) {
	this->PropertyFile::update (value);
    }

    void update (const std::string& value) override { this->m_value = value; }

    [[nodiscard]] std::string toString () const override { return this->m_value; }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - file" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->m_value << std::endl;

	return ss.str ();
    }

private:
    std::string m_value;
};

class PropertyTextInput final : public Property {
public:
    explicit PropertyTextInput (PropertyData data, const std::string& value) : Property (std::move (data)) {
	this->PropertyTextInput::update (value);
    }

    void update (const std::string& value) override { this->m_value = value; }

    [[nodiscard]] std::string toString () const override { return this->m_value; }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - textinput" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->m_value << std::endl;

	return ss.str ();
    }

private:
    std::string m_value;
};
}