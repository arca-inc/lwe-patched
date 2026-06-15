#include "PropertyParser.h"
#include "../Model/Property.h"

using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Model;

namespace {
// Wallpaper Engine's own data is loosely typed: bool/slider properties sometimes
// store their value as a string (e.g. "value": "true", "value": "150"). nlohmann
// throws type_error.302 on an implicit string->bool/float conversion, which
// aborts the whole wallpaper. Coerce by the actual JSON type instead.
bool coerceBool (const JSON& node) {
    if (node.is_boolean ()) return node.get<bool> ();
    if (node.is_number ()) return node.get<double> () != 0.0;
    if (node.is_string ()) {
        const auto s = node.get<std::string> ();
        return s == "true" || s == "1";
    }
    return false;
}

float coerceFloat (const JSON& node) {
    if (node.is_number ()) return static_cast<float> (node.get<double> ());
    if (node.is_boolean ()) return node.get<bool> () ? 1.0f : 0.0f;
    if (node.is_string ()) {
        try {
            return std::stof (node.get<std::string> ());
        } catch (...) {
            return 0.0f;
        }
    }
    return 0.0f;
}

// Combo option keys and the selected value must stringify the same way so the
// selection can be matched. WE sometimes stores them as booleans/numbers (e.g.
// a 12h/24h combo with "value": true), which would throw type_error.302 on a
// direct .get<std::string>(). Normalize every scalar to a stable string form.
std::string coerceComboKey (const JSON& node) {
    if (node.is_string ()) return node.get<std::string> ();
    if (node.is_number ()) return std::to_string (node.get<int> ());
    if (node.is_boolean ()) return node.get<bool> () ? "true" : "false";
    return node.dump ();
}
} // namespace

PropertySharedPtr PropertyParser::parse (const JSON& it, const std::string& name) {
    // type might not be included, in which case means the same as a group
    const auto type = it.optional ("type");

    if (type == "color") {
	return parseColor (it, name);
    }
    if (type == "bool") {
	return parseBoolean (it, name);
    }
    if (type == "slider") {
	return parseSlider (it, name);
    }
    if (type == "combo") {
	return parseCombo (it, name);
    }
    if (type == "text") {
	return parseText (it, name);
    }
    if (type == "scenetexture") {
	return parseSceneTexture (it, name);
    }
    if (type == "file" || type == "directory") {
	return parseFile (it, name);
    }
    if (type == "textinput") {
	return parseTextInput (it, name);
    }
    if (type == "usershortcut") {
	return parseTextInput (it, name);
    }

    if (type.has_value () && type != "group") {
	// show the error and ignore this property
	sLog.error ("Unexpected type for property: ", type);
	sLog.error (it.dump ());
    }

    return nullptr;
}

PropertySharedPtr PropertyParser::parseCombo (const JSON& it, const std::string& name) {
    std::map<std::string, std::string> optionsMap = {};

    const auto options = it.require ("options", "Combo property must have options");

    if (!options.is_array ()) {
	sLog.exception ("Property combo options should be an array");
    }

    for (auto& cur : options) {
	if (!cur.is_object ()) {
	    continue;
	}

	const auto value = cur.require ("value", "Combo option must have a value");

	optionsMap.emplace (
	    coerceComboKey (value),
	    cur.require ("label", "Combo option must have a label")
	);
    }

    const auto value = it.require ("value", "Combo property must have a value");

    return std::make_shared<PropertyCombo> (
	PropertyData {
	    .name = name,
	    .text = it.optional<std::string> ("text", ""),
	},
	ComboData { .values = optionsMap },
	coerceComboKey (value)
    );
}

PropertySharedPtr PropertyParser::parseColor (const JSON& it, const std::string& name) {
    return std::make_shared<PropertyColor> (
	PropertyData {
	    .name = name,
	    .text = it.optional<std::string> ("text", ""),
	},
	it.require ("value", "Property must have a value")
    );
}

PropertySharedPtr PropertyParser::parseBoolean (const JSON& it, const std::string& name) {
    bool value = false;
    if (const auto node = it.optional ("value")) {
	value = coerceBool (*node);
    }
    return std::make_shared<PropertyBoolean> (
	PropertyData {
	    .name = name,
	    .text = it.optional<std::string> ("text", ""),
	},
	value
    );
}

PropertySharedPtr PropertyParser::parseSlider (const JSON& it, const std::string& name) {
    return std::make_shared<PropertySlider> (
	PropertyData {
	    .name = name,
	    .text = it.optional<std::string> ("text", ""),
	},
	SliderData {
	    .min = it.optional ("min", 0.0f),
	    .max = it.optional ("max", 0.0f),
	    .step = it.optional ("step", 0.0f),
	},
	coerceFloat (it.require ("value", "Property must have a value"))
    );
}

PropertySharedPtr PropertyParser::parseText (const JSON& it, const std::string& name) {
    return std::make_shared<PropertyText> (PropertyData {
	.name = name,
	.text = it.optional<std::string> ("text", ""),
    });
}

PropertySharedPtr PropertyParser::parseSceneTexture (const JSON& it, const std::string& name) {
    return std::make_shared<PropertySceneTexture> (
	PropertyData {
	    .name = name,
	    .text = it.optional<std::string> ("text", ""),
	},
	it.require ("value", "Property must have a value")
    );
}

PropertySharedPtr PropertyParser::parseFile (const JSON& it, const std::string& name) {
    return std::make_shared<PropertyFile> (
	PropertyData {
	    .name = name,
	    .text = it.optional<std::string> ("text", ""),
	},
	it.optional<std::string> ("value", "")
    );
}

PropertySharedPtr PropertyParser::parseTextInput (const JSON& it, const std::string& name) {
    // textinput/usershortcut properties legitimately ship with no default value
    // (the user types one in); requiring "value" aborted the whole wallpaper.
    const auto value = it.optional ("value");

    return std::make_shared<PropertyTextInput> (
	PropertyData {
	    .name = name,
	    .text = it.optional<std::string> ("text", ""),
	},
	value.has_value () ? value->dump () : std::string ()
    );
}
