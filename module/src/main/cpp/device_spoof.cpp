/* Zygisk Device Spoof Module
 * Copyright (C) 2024 Qjj0204
 *
 * A Zygisk module to spoof android.os.Build fields for specific apps.
 */

#include <string>
#include <sys/system_properties.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <thread>
#include <vector>
#include <unordered_map>
#include <string_view>
#include <fstream>
#include <sstream>
#include <sys/sysmacros.h>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <memory>
#include <android/log.h>


#include "zygisk.hpp"
#include "document.h" // rapidjson

#ifndef PROPERTY_VALUE_MAX
#define PROPERTY_VALUE_MAX 92
#endif

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;


// --- Global Variables & Forward Declarations ---

#ifdef DEBUG
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "ZygiskDeviceSpoof", __VA_ARGS__)
#else
#define LOGD(...)
#endif

using DeviceInfo = std::unordered_map<std::string, std::string>;
static DeviceInfo g_spoof_properties;
static int (*orig_system_property_get)(const char *, char *);
static dev_t libc_dev = 0;
static ino_t libc_ino = 0;

thread_local bool is_jni_hooking = false;

static void spoofDevice(JNIEnv *env);
static void do_hook(Api *api);

const char* MODULE_ID = "zygisk_device_spoof";
std::string read_config_blocking() { 
    std::ifstream file(std::string("/data/adb/modules/") + MODULE_ID + "/config/config.json");
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}
void write_data_to_socket(int fd, const char* data, size_t len) { write(fd, data, len); }
void inotify_init_and_watch() {}

// --- Hooking Logic ---

static std::pair<dev_t, ino_t> get_libc_info() {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libc.so") != std::string::npos) {
            unsigned int dev_major, dev_minor;
            ino_t inode;
            char path[256];
            if (sscanf(line.c_str(), "%*s %*s %*s %x:%x %lu %s", &dev_major, &dev_minor, &inode, path) == 4) {
                dev_t dev = makedev(dev_major, dev_minor);
                LOGD("Found libc.so: dev=%lx, inode=%lu", (unsigned long)dev, (unsigned long)inode);
                return {dev, inode};
            }
        }
    }
    LOGD("Could not find libc.so");
    return {0, 0};
}

int my_system_property_get(const char* name, char* value) {
    if (is_jni_hooking) {
        if (orig_system_property_get) return orig_system_property_get(name, value);
        return -1;
    }
    
    thread_local bool in_hook = false;
    if (in_hook) {
        if (orig_system_property_get) return orig_system_property_get(name, value);
        return -1;
    }

    if (!name || !orig_system_property_get) {
        if (orig_system_property_get) return orig_system_property_get(name, value);
        return -1;
    }
    
    in_hook = true;
    
    auto it = g_spoof_properties.find(name);
    if (it != g_spoof_properties.end()) {
        const std::string& spoof_val = it->second;
        strncpy(value, spoof_val.c_str(), PROPERTY_VALUE_MAX - 1);
        value[PROPERTY_VALUE_MAX - 1] = '\0';
        in_hook = false;
        return strlen(value);
    }

    int result = orig_system_property_get(name, value);
    in_hook = false;
    return result;
}

static void do_hook(Api *api) {
    if (!api) return;
    auto [dev, ino] = get_libc_info();
    if (dev == 0 || ino == 0) {
        LOGD("Failed to get libc info, native hook skipped.");
        return;
    }
    libc_dev = dev;
    libc_ino = ino;
    api->pltHookRegister(libc_dev, libc_ino, "__system_property_get",
                         (void *)my_system_property_get,
                         (void **)&orig_system_property_get);

    if (!api->pltHookCommit()) {
        LOGD("Failed to commit plt hook.");
        orig_system_property_get = nullptr;
    } else {
        LOGD("Successfully hooked __system_property_get");
    }
}

static void spoofDevice(JNIEnv *env) {
    if (g_spoof_properties.empty()) return;

    jclass build_class = env->FindClass("android/os/Build");
    if (build_class == nullptr) {
        LOGD("Class 'android/os/Build' not found");
        return;
    }

    for (const auto& [key, value] : g_spoof_properties) {
        std::string spoof_key = key;
        if (spoof_key.rfind("ro.product.", 0) == 0) spoof_key = spoof_key.substr(11);
        else if (spoof_key.rfind("ro.build.", 0) == 0) spoof_key = spoof_key.substr(9);
        else if (spoof_key.rfind("ro.", 0) == 0) spoof_key = spoof_key.substr(3);
        
        std::replace(spoof_key.begin(), spoof_key.end(), '.', '_');
        std::transform(spoof_key.begin(), spoof_key.end(), spoof_key.begin(), ::toupper);

        jfieldID field = env->GetStaticFieldID(build_class, spoof_key.c_str(), "Ljava/lang/String;");
        if (field != nullptr) {
            jstring spoof_value = env->NewStringUTF(value.c_str());
            env->SetStaticObjectField(build_class, field, spoof_value);
            env->DeleteLocalRef(spoof_value);
        }
    }
    env->DeleteLocalRef(build_class);
}

// --- Zygisk Module Implementation ---

class SpoofModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        const char *process = env->GetStringUTFChars(args->nice_name, nullptr);
        if (!process) {
            api->setOption(zygisk::Option::FORCE_DENYLIST_UNMOUNT);
            return;
        }

        int fd = api->connectCompanion();
        if (fd == -1) {
            LOGD("Failed to connect to companion for %s", process);
            env->ReleaseStringUTFChars(args->nice_name, process);
            api->setOption(zygisk::Option::FORCE_DENYLIST_UNMOUNT);
            return;
        }

        write(fd, process, strlen(process) + 1);

        std::vector<char> buffer(4096);
        int read_len = read(fd, buffer.data(), buffer.size() -1);
        close(fd);

        env->ReleaseStringUTFChars(args->nice_name, process);

        if (read_len <= 0) {
            api->setOption(zygisk::Option::FORCE_DENYLIST_UNMOUNT);
            return;
        }

        g_spoof_properties.clear();
        std::string_view buf_view(buffer.data(), read_len);
        
        size_t start = 0;
        while(start < (size_t)read_len) {
            size_t key_end = buf_view.find('\0', start);
            if (key_end == std::string_view::npos) break;
            std::string key(buf_view.substr(start, key_end - start));
            start = key_end + 1;

            if (start >= (size_t)read_len) break;

            size_t value_end = buf_view.find('\0', start);
            if (value_end == std::string_view::npos) break;
            std::string value(buf_view.substr(start, value_end - start));
            start = value_end + 1;
            
            g_spoof_properties[key] = value;
        }

        if (!g_spoof_properties.empty()) {
            do_hook(api);

            is_jni_hooking = true;
            spoofDevice(env);
            is_jni_hooking = false;
        }
        
        api->setOption(zygisk::Option::FORCE_DENYLIST_UNMOUNT);
    }

private:
    Api *api;
    JNIEnv *env;
};

// --- Companion Implementation ---

static void companion_handler(int i) {
    // 1. First, read the unique data from the socket. This can be done in parallel.
    char target_package[256] = {0};
    if (read(i, target_package, sizeof(target_package) -1) < 0) {
        close(i);
        return;
    }

    // 2. Now, acquire the lock just before accessing the shared resource (config file).
    char lock_path[256];
    snprintf(lock_path, sizeof(lock_path), "/data/adb/modules/%s/companion.lock", MODULE_ID);
    int lock_fd = open(lock_path, O_RDONLY | O_CREAT, 0644);
    if (lock_fd >= 0) {
        if (flock(lock_fd, LOCK_EX) == -1) {
            LOGD("Failed to acquire companion lock for %s", lock_path);
        }
    } else {
        LOGD("Failed to open companion lock file %s", lock_path);
    }

    // 3. Access the shared resource.
    std::string config_data = read_config_blocking();
    if (config_data.empty()) {
        if (lock_fd >= 0) close(lock_fd);
        close(i);
        return;
    }
    
    rapidjson::Document document;
    document.Parse(config_data.c_str());

    if (document.HasParseError() || !document.IsObject() || !document.HasMember("apps") || !document["apps"].IsArray()) {
        if (lock_fd >= 0) close(lock_fd);
        close(i);
        return;
    }

    DeviceInfo properties;
    const rapidjson::Value& apps = document["apps"];
    for (const auto& app : apps.GetArray()) {
        if (app.IsObject() && app.HasMember("package") && app["package"].IsString() &&
            strcmp(app["package"].GetString(), target_package) == 0) {
            
            if (app.HasMember("properties") && app["properties"].IsObject()) {
                for (const auto& prop : app["properties"].GetObject()) {
                    if (prop.value.IsString()) {
                        std::string value_str = prop.value.GetString();
                        if (!value_str.empty()) {
                            properties[prop.name.GetString()] = value_str;
                        }
                    }
                }
            }
            break; 
        }
    }
    
    std::string buffer_str;
    for (const auto& [key, value] : properties) {
        if (!value.empty()) {
            buffer_str += key;
            buffer_str += '\0';
            buffer_str += value;
            buffer_str += '\0';
        }
    }
    
    if (!buffer_str.empty()){
        write_data_to_socket(i, buffer_str.c_str(), buffer_str.length());
    }
    
    // 4. Release the lock and close the connection.
    if (lock_fd >= 0) close(lock_fd);
    close(i);
}

static void companion_init() {
    char lock_path[256];
    snprintf(lock_path, sizeof(lock_path), "/data/adb/modules/%s/companion.lock", MODULE_ID);

    int lock_fd = open(lock_path, O_RDONLY | O_CREAT, 0644);
    if (lock_fd < 0) return;

    if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        close(lock_fd);
        return;
    }
    
    inotify_init_and_watch();
}

REGISTER_ZYGISK_MODULE(SpoofModule)
REGISTER_ZYGISK_COMPANION(companion_handler)


