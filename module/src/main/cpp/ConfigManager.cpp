#include "ConfigManager.h"
#include <fstream>
#include <sys/stat.h>
#include <android/log.h>

#ifdef DEBUG
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "ZygiskDeviceSpoof", __VA_ARGS__)
#else
#define LOGD(...)
#endif

const std::string ConfigManager::CONFIG_PATH = "/data/adb/modules/zygisk_device_spoof/config/config.json";

ConfigManager::ConfigManager() : last_modified_time() {}

std::chrono::system_clock::time_point ConfigManager::getFileModTime() const {
    struct stat result;
    if (stat(CONFIG_PATH.c_str(), &result) == 0) {
        return std::chrono::system_clock::from_time_t(result.st_mtime);
    }
    return std::chrono::system_clock::time_point();
}

void ConfigManager::loadOrReloadConfig() {
    std::scoped_lock lock(config_mutex);

    auto current_mod_time = getFileModTime();
    if (current_mod_time == last_modified_time && !app_configs.empty()) {
        return;
    }

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto error = parser.load(CONFIG_PATH).get(doc);

    if (error) {
        LOGD("Failed to load or parse config.json");
        app_configs.clear();
        return;
    }

    parseConfig(doc);
    last_modified_time = current_mod_time;
}

bool ConfigManager::isTargetApp(const std::string& app_name) const {
    std::scoped_lock lock(config_mutex);
    return app_configs.count(app_name) > 0;
}

std::optional<std::reference_wrapper<const std::unordered_map<std::string, std::string>>>
ConfigManager::getSpoofPropertiesForApp(const std::string& app_name) const {
    std::scoped_lock lock(config_mutex);
    auto it = app_configs.find(app_name);
    if (it != app_configs.end()) {
        return it->second;
    }
    return std::nullopt;
}

void ConfigManager::parseConfig(simdjson::dom::element &doc) {
    app_configs.clear();
    simdjson::dom::array apps;
    if (doc["apps"].get_array().get(apps) != simdjson::SUCCESS) {
        LOGD("Config parsing error: 'apps' array not found or is not an array.");
        return;
    }

    for (simdjson::dom::element app_element : apps) {
        std::string_view package_name;
        if (app_element["package"].get_string().get(package_name) != simdjson::SUCCESS) {
            continue;
        }

        simdjson::dom::object properties_obj;
        if (app_element["properties"].get_object().get(properties_obj) != simdjson::SUCCESS) {
            continue;
        }
        
        std::unordered_map<std::string, std::string> properties;
        for (auto field : properties_obj) {
            std::string_view key = field.key;
            std::string_view value;
            if (field.value.get_string().get(value) == simdjson::SUCCESS) {
                properties.emplace(key, value);
            }
        }

        if (!properties.empty()) {
            app_configs.emplace(package_name, std::move(properties));
        }
    }
    LOGD("Loaded config for %zu apps.", app_configs.size());
}
