#pragma once

#include "simdjson.h"
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <functional>
#include <chrono>

class ConfigManager {
public:
    ConfigManager();

    void loadOrReloadConfig();
    bool isTargetApp(const std::string& app_name) const;
    std::optional<std::reference_wrapper<const std::unordered_map<std::string, std::string>>>
    getSpoofPropertiesForApp(const std::string& app_name) const;

private:
    void parseConfig(simdjson::dom::element& doc);
    std::chrono::system_clock::time_point getFileModTime() const;

    static const std::string CONFIG_PATH;
    std::chrono::system_clock::time_point last_modified_time;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> app_configs;
    mutable std::mutex config_mutex;
};
