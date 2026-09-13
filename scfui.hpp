/*
    SCF UI file parser.

    This file is part of the SCF library.

    This module take a xml ui file and parse it to create a window using scf.
*/

#pragma once

#include <filesystem>
namespace fs = std::filesystem;

#include "scf.hpp"

namespace scf {

class SCF_EXPORT uiBuilder {
public:

    window applyUIFile(const scl2::bytearray& uiFileData);
    window applyUIFile(const fs::path& uiFile);

    void registerEventHandler(const std::string& controlID, const std::string& eventName, std::function<void()> callback);
    void registerEventHandler(const std::string& controlID, const std::string& eventName, std::function<void(window&)> callback);

    std::map<std::string, std::map<std::string, std::function<void()>>> eventHandlers;
    std::map<std::string, std::wstring> controlIDs;

protected:
    bool lookupControl(const std::string& controlID);
    bool lookupEventHandler(const std::string& controlID, const std::string& eventName);



};




} // namespace scf