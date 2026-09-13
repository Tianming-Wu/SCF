#include "scfui.hpp"

#include <SharedCppLib2/json.hpp>

/*
    Note: I once wanted to use xml for the UI file format, but the xml parser in SharedCppLib2
    is not yet complete and is quite hard to use.

    Json is used for now. It should be easy to switch to xml later if needed.
*/

namespace scf {

window uiBuilder::applyUIFile(const scl2::bytearray &uiFileData)
{
    scl2::json uiJson;
    try {
        uiJson = scl2::json::fromString(uiFileData.toStdString());
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to parse UI file: " + std::string(e.what()));
    }

    struct windowMeta {
        scl2::Geometry geo;
        std::wstring title;
        scf::WindowFlags flags = scf::WindowFlags::None;
    } meta;

    auto get_number = [](const scl2::json_value& v) -> int64_t {
        if (v.is_int()) return v.as_int();
        else throw std::runtime_error("Invalid UI file: 'window.geometry' field should contain intergers only");
    };

    auto get_geo = [](const scl2::json_value& g_v) -> scl2::Geometry {
        if (!g_v.is_array() || g_v.array_size() != 4) {
            throw std::runtime_error("Invalid UI file: 'window.geometry' field should be an array of 4 integers");
        }
        scl2::Geometry geo;
        geo.x = g_v.at(0).as_int();
        geo.y = g_v.at(1).as_int();
        geo.w = g_v.at(2).as_int();
        geo.h = g_v.at(3).as_int();
        return geo;
    };

    if (uiJson.has_key("window")) {
        const auto& windowJson = uiJson["window"];
        if (!windowJson.is_object()) {
            throw std::runtime_error("Invalid UI file: 'window' must be an object");
        }

        // extract keys from ui file:
        // geometry (optional): [ x, y, w, h ] / [ x, y ]
        if (windowJson.has_key("geometry")) {
            const auto& geometryJson = windowJson["geometry"];
            if (!geometryJson.is_null()) {
                if (!geometryJson.is_array())
                    throw std::runtime_error("Invalid UI file: 'window.geometry' field should be array or null");

                switch (geometryJson.array_size()) {
                    case 0: break;
                    case 2: {
                        meta.geo.x = get_number(geometryJson.at(0));
                        meta.geo.y = get_number(geometryJson.at(1));
                        break;
                    }
                    case 4: {
                        meta.geo.x = get_number(geometryJson.at(0));
                        meta.geo.y = get_number(geometryJson.at(1));
                        meta.geo.w = get_number(geometryJson.at(2));
                        meta.geo.h = get_number(geometryJson.at(3));
                        break;
                    }
                    default:
                        throw std::runtime_error("Invalid UI file: 'window.geometry' field should contain 0, 2 or 4 elements");
                }
               
            } // else treat as default (no field at all)
        } // (field geometry)

        // title (optional): string / null
        if (windowJson.has_key("title")) {
            const auto& titleJson = windowJson["title"];
            if (!titleJson.is_null()) {
                if (!titleJson.is_string())
                    throw std::runtime_error("Invalid UI file: 'window.title' field should be string or null");
                meta.title = titleJson.as_wstring();
            } // else treat as empty title (default)
        } // (field title)

        ///TODO: flags (optional): array of strings / null

    } else {
        throw std::runtime_error("Invalid UI file: missing 'window' field");
    }

    // Create the window first.
    scf::window win(meta.geo, meta.title, meta.flags);

    // field "controls" is optional; if present, it must be an array of objects
    // also generate the controls while parsing, since different controls have different fields.
    if (uiJson.has_key("controls")) {
        const auto& controlsJson = uiJson["controls"];
        if (!controlsJson.is_array()) {
            throw std::runtime_error("Invalid UI file: 'controls' must be an array");
        }

        for (size_t i = 0; i < controlsJson.array_size(); ++i) {
            const auto& controlJson = controlsJson.at(i);
            if (!controlJson.is_object()) {
                throw std::runtime_error("Invalid UI file: each control must be an object");
            }

            std::wstring controlID; // optional, may be empty
            // controlID field is optional; if present, it must be a string
            // controlID is required if the control is to be referenced later.
            // without an valid id, the control cannot be accessed later after creation.
            if (controlJson.has_key("controlID")) {
                const auto& controlIDJson = controlJson["controlID"];
                if (!controlIDJson.is_null()) {
                    if (!controlIDJson.is_string())
                        throw std::runtime_error("Invalid UI file: 'controlID' field should be string or null");
                    controlID = controlIDJson.as_wstring();
                } // else treat as default (no field at all)
            }

            if (!controlID.empty()) {
                if (lookupControl(scl2::wstr_to_str(controlID))) {
                    throw std::runtime_error("Invalid UI file: duplicate controlID '" + scl2::wstr_to_str(controlID) + "'");
                }
                controlIDs[scl2::wstr_to_str(controlID)] = controlID;
            }

            // geometry field is required for controls, and must be an array of 4 integers
            if (!controlJson.has_key("geometry") || !controlJson["geometry"].is_array() || controlJson["geometry"].array_size() != 4)
                throw std::runtime_error("Invalid UI file: each control must have a 'geometry' field with 4 integers");
            scl2::Geometry controlGeo = get_geo(controlJson["geometry"]);

            // type field is required for controls, and must be a string
            if (!controlJson.has_key("type") || !controlJson["type"].is_string())
                throw std::runtime_error("Invalid UI file: each control must have a 'type' field with a string");
            std::wstring controlType = controlJson["type"].as_wstring();

            if (controlType == L"label") {
                // label: optional text field (string)
                std::wstring labelText;
                if (controlJson.has_key("text")) {
                    const auto& textJson = controlJson["text"];
                    if (!textJson.is_null()) {
                        if (!textJson.is_string())
                            throw std::runtime_error("Invalid UI file: 'label.text' field should be string or null");
                        labelText = textJson.as_wstring();
                    } // else treat as empty text
                } // (field text)

                auto lbl = new_label(controlGeo, labelText);
                win.add_child(lbl);

            } else if (controlType == L"button") {
                // button: optional text field (string)
                std::wstring buttonText;
                if (controlJson.has_key("text")) {
                    const auto& textJson = controlJson["text"];
                    if (!textJson.is_null()) {
                        if (!textJson.is_string())
                            throw std::runtime_error("Invalid UI file: 'button.text' field should be string or null");
                        buttonText = textJson.as_wstring();
                    } // else treat as empty text
                } // (field text)

                auto btn = new_button(controlGeo, buttonText);
                win.add_child(btn);

                // search for on_click callback
                if (!controlID.empty()) {
                    if (lookupEventHandler(scl2::wstr_to_str(controlID), "on_click")) {
                        btn->on_click(eventHandlers[scl2::wstr_to_str(controlID)]["on_click"]);
                    }
                }

            } else {
                throw std::runtime_error("Invalid UI file: unknown control type '" + scl2::wstr_to_str(controlType) + "'");
            }
            


        }
    }

    return win;
}

window uiBuilder::applyUIFile(const fs::path &uiFile)
{
    if (!fs::exists(uiFile)) {
        throw std::runtime_error("UI file does not exist: " + uiFile.string());
    }






    return window();
}

bool uiBuilder::lookupControl(const std::string &controlID)
{
    return (controlIDs.find(controlID) != controlIDs.end());
}

bool uiBuilder::lookupEventHandler(const std::string &controlID, const std::string &eventName)
{
    return (eventHandlers.find(controlID) != eventHandlers.end()) && (eventHandlers[controlID].find(eventName) != eventHandlers[controlID].end());
}

} // namespace scf