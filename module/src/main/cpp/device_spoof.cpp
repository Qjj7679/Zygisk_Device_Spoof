/* Zygisk Device Spoof Module
 * Copyright (C) 2024 Qjj0204
 *
 * A Zygisk module to spoof android.os.Build fields for specific apps.
 */

#include <memory>
#include <string_view>
#include <unistd.h>

#include "zygisk.hpp"
#include "ConfigManager.h"
#include "HookManager.h"

#ifdef DEBUG
#include <android/log.h>
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "ZygiskDeviceSpoof", __VA_ARGS__)
#else
#define LOGD(...)
#endif

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

// Forward declaration
static void companion_handler(int socket_fd);

class SpoofModule : public zygisk::ModuleBase {
public:
    void onLoad(Api* api, JNIEnv* env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        auto* raw_app_name = env->GetStringUTFChars(args->nice_name, nullptr);
        if (!raw_app_name) return;
        std::string app_name(raw_app_name);
        env->ReleaseStringUTFChars(args->nice_name, raw_app_name);

        int fd = api->connectCompanion();
        if (fd < 0) {
            LOGD("Failed to connect to companion for %s", app_name.c_str());
            return;
        }

        write(fd, app_name.c_str(), app_name.length() + 1);

        std::string props_buffer;
        char chunk[1024];
        ssize_t bytes_read;
        while ((bytes_read = read(fd, chunk, sizeof(chunk))) > 0) {
            props_buffer.append(chunk, bytes_read);
        }
        close(fd);

        if (bytes_read < 0) {
            LOGD("Failed to read from companion for %s", app_name.c_str());
            return;
        }

        if (props_buffer.empty()) {
            return;
        }

        std::unordered_map<std::string, std::string> properties;
        std::string_view buf_view(props_buffer);
        size_t start = 0;
        while (start < buf_view.length()) {
            size_t key_end = buf_view.find('\0', start);
            if (key_end == std::string_view::npos) break;
            std::string key(buf_view.substr(start, key_end - start));
            start = key_end + 1;

            if (start >= buf_view.length()) break;

            size_t value_end = buf_view.find('\0', start);
            if (value_end == std::string_view::npos) break;
            std::string value(buf_view.substr(start, value_end - start));
            start = value_end + 1;
            
            properties[key] = value;
        }

        if (!properties.empty()) {
            if (!hookManager) {
                hookManager = std::make_unique<HookManager>(api);
            }
            hookManager->applyHooks(env, properties);
        }
        api->setOption(zygisk::Option::FORCE_DENYLIST_UNMOUNT);
    }

private:
    Api* api;
    JNIEnv* env;
    std::unique_ptr<HookManager> hookManager;
};

static void companion_handler(int socket_fd) {
    static ConfigManager configManager;
    
    char app_name[256] = {};
    ssize_t bytes_read = read(socket_fd, app_name, sizeof(app_name) - 1);
    if (bytes_read <= 0) {
        close(socket_fd);
        return;
    }
    app_name[bytes_read] = '\0';

    configManager.loadOrReloadConfig();

    auto props_opt = configManager.getSpoofPropertiesForApp(app_name);
    if (props_opt) {
        std::string buffer_str;
        for (const auto& [key, value] : props_opt->get()) {
            buffer_str += key;
            buffer_str += '\0';
            buffer_str += value;
            buffer_str += '\0';
        }
        if (!buffer_str.empty()) {
            write(socket_fd, buffer_str.c_str(), buffer_str.length());
        }
    }

    close(socket_fd);
}

REGISTER_ZYGISK_MODULE(SpoofModule)
REGISTER_ZYGISK_COMPANION(companion_handler)


