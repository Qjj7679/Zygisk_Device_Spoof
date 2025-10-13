/* Zygisk Device Spoof Module
 * Copyright (C) 2024 Qjj0204
 * 
 * A Zygisk module to spoof android.os.Build fields for specific apps.
 */

#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <android/log.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>

#include "zygisk.hpp"
#include "document.h"
#include "filereadstream.h"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

// 日志宏定义
#ifdef DEBUG
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "ZygiskDeviceSpoof", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ZygiskDeviceSpoof", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "ZygiskDeviceSpoof", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ZygiskDeviceSpoof", __VA_ARGS__)
#else
#define LOGD(...)
#define LOGI(...)
#define LOGW(...)
#define LOGE(...)
#endif

// 设备信息结构
struct DeviceInfo {
    std::string brand;
    std::string model;
    std::string device;
    std::string manufacturer;
    std::string product;
    
    DeviceInfo() = default;
    DeviceInfo(const std::string& b, const std::string& m, 
               const std::string& d, const std::string& mf, 
               const std::string& p)
        : brand(b), model(m), device(d), manufacturer(mf), product(p) {}
};

// 全局变量
static std::unordered_map<std::string, DeviceInfo> package_map;
static DeviceInfo current_info;
static std::mutex config_mutex;
static std::once_flag build_class_init_flag;

// JNI 缓存
static jclass buildClass = nullptr;
static jfieldID modelField = nullptr;
static jfieldID brandField = nullptr;
static jfieldID deviceField = nullptr;
static jfieldID manufacturerField = nullptr;
static jfieldID productField = nullptr;

// 配置文件路径
constexpr const char* CONFIG_CACHE_PATH = 
    "/data/adb/modules/zygisk_device_spoof/cache/config_cached.json";
constexpr const char* CONFIG_PATH = 
    "/data/adb/modules/zygisk_device_spoof/config/config.json";

static time_t last_config_mtime = 0;

// RAII 包装器：自动管理 JNI 字符串
class JniString {
public:
    JniString(JNIEnv* env, jstring jstr) 
        : env_(env), jstr_(jstr), 
          cstr_(jstr ? env->GetStringUTFChars(jstr, nullptr) : nullptr) {}
    
    ~JniString() {
        if (cstr_) {
            env_->ReleaseStringUTFChars(jstr_, cstr_);
        }
    }
    
    const char* c_str() const { return cstr_; }
    operator bool() const { return cstr_ != nullptr; }
    
    JniString(const JniString&) = delete;
    JniString& operator=(const JniString&) = delete;
    
private:
    JNIEnv* env_;
    jstring jstr_;
    const char* cstr_;
};

// 异常处理辅助函数
inline bool checkAndClearException(JNIEnv* env, const char* operation) {
    if (env->ExceptionCheck()) {
        LOGE("JNI exception during %s", operation);
        #ifdef DEBUG
        env->ExceptionDescribe();
        #endif
        env->ExceptionClear();
        return true;
    }
    return false;
}

class SpoofModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
        
        LOGI("Module loaded, version: 1.0.0");
        
        ensureBuildClass();
        reloadIfNeeded(true); // 强制加载配置
    }
    
    void preAppSpecialize(AppSpecializeArgs *args) override {
        if (!args || !args->nice_name) {
            LOGD("Invalid args or package name");
            unloadModule();
            return;
        }
        
        JniString packageName(env, args->nice_name);
        if (!packageName) {
            LOGE("Failed to get package name");
            unloadModule();
            return;
        }
        
        std::string package = packageName.c_str();
        LOGD("preAppSpecialize: %s", package.c_str());
        
        // 重新加载配置（检查更新）
        reloadIfNeeded(false);
        
        // 查找包名
        auto it = package_map.find(package);
        if (it != package_map.end()) {
            current_info = it->second;
            spoofDevice(current_info);
            LOGI("Package matched: %s", package.c_str());
        } else {
            LOGD("Package not in config, unloading");
        }
        
        unloadModule();
    }
    
    void postAppSpecialize(const AppSpecializeArgs *args) override {
        if (!buildClass) {
            unloadModule();
            return;
        }
        
        // 再次验证并伪装（防止字段被重置）
        if (!current_info.model.empty()) {
            LOGD("postAppSpecialize: re-spoofing");
            spoofDevice(current_info);
        }
        
        unloadModule();
    }

private:
    Api *api = nullptr;
    JNIEnv *env = nullptr;
    
    void ensureBuildClass() {
        std::call_once(build_class_init_flag, [this]() {
            jclass localClass = env->FindClass("android/os/Build");
            if (!localClass || checkAndClearException(env, "FindClass(Build)")) {
                LOGE("Failed to find Build class");
                return;
            }
            
            buildClass = (jclass)env->NewGlobalRef(localClass);
            env->DeleteLocalRef(localClass);
            
            if (!buildClass) {
                LOGE("Failed to create global ref for Build class");
                return;
            }
            
            modelField = env->GetStaticFieldID(buildClass, "MODEL", "Ljava/lang/String;");
            brandField = env->GetStaticFieldID(buildClass, "BRAND", "Ljava/lang/String;");
            deviceField = env->GetStaticFieldID(buildClass, "DEVICE", "Ljava/lang/String;");
            manufacturerField = env->GetStaticFieldID(buildClass, "MANUFACTURER", "Ljava/lang/String;");
            productField = env->GetStaticFieldID(buildClass, "PRODUCT", "Ljava/lang/String;");
            
            if (!modelField || !brandField || !deviceField || 
                !manufacturerField || !productField) {
                LOGE("Failed to get Build field IDs");
                checkAndClearException(env, "GetStaticFieldID");
            } else {
                LOGD("Build class initialized successfully");
            }
        });
    }
    
    bool reloadIfNeeded(bool force = false) {
        std::lock_guard<std::mutex> lock(config_mutex);
        
        // 检查文件修改时间
        if (!force) {
            struct stat st;
            if (stat(CONFIG_PATH, &st) == 0) {
                if (st.st_mtime == last_config_mtime) {
                    return false; // 未修改
                }
                last_config_mtime = st.st_mtime;
            }
        }
        
        // 优先读取缓存文件
        const char* file_to_read = CONFIG_CACHE_PATH;
        FILE* fp = fopen(file_to_read, "rb");
        
        if (!fp) {
            LOGD("Cache file not found, trying config file");
            file_to_read = CONFIG_PATH;
            fp = fopen(file_to_read, "rb");
        }
        
        if (!fp) {
            LOGE("Failed to open config file");
            return false;
        }

        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        if (size <= 0) {
            fclose(fp);
            LOGE("Config file is empty or invalid size");
            return false;
        }

        auto buffer = std::make_unique<char[]>(size + 1);
        fread(buffer.get(), 1, size, fp);
        buffer[size] = '\0';
        fclose(fp);
        
        // 解析 JSON
        rapidjson::Document doc;
        doc.Parse(buffer.get());
        
        if (doc.HasParseError()) {
            LOGE("JSON parse error: %d", doc.GetParseError());
            return false;
        }
        
        if (!doc.HasMember("apps") || !doc["apps"].IsArray()) {
            LOGE("Invalid JSON format: missing 'apps' array");
            return false;
        }
        
        // 清空并重新填充 package_map
        package_map.clear();
        
        const auto& apps = doc["apps"].GetArray();
        for (const auto& app : apps) {
            if (!app.IsObject()) continue;
            
            if (!app.HasMember("package") || !app["package"].IsString()) continue;
            
            std::string package = app["package"].GetString();
            DeviceInfo info;
            
            if (app.HasMember("brand") && app["brand"].IsString())
                info.brand = app["brand"].GetString();
            if (app.HasMember("model") && app["model"].IsString())
                info.model = app["model"].GetString();
            if (app.HasMember("device") && app["device"].IsString())
                info.device = app["device"].GetString();
            if (app.HasMember("manufacturer") && app["manufacturer"].IsString())
                info.manufacturer = app["manufacturer"].GetString();
            if (app.HasMember("product") && app["product"].IsString())
                info.product = app["product"].GetString();
            
            package_map[package] = info;
            LOGD("Loaded config for package: %s", package.c_str());
        }
        
        LOGI("Config loaded: %zu apps", package_map.size());
        return true;
    }
    
    void spoofDevice(const DeviceInfo& info) {
        if (!buildClass) {
            LOGE("Build class not initialized");
            return;
        }
        
        auto setField = [this](jfieldID field, const std::string& value, const char* name) {
            if (value.empty()) return;
            
            jstring jstr = env->NewStringUTF(value.c_str());
            if (!jstr || checkAndClearException(env, "NewStringUTF")) {
                LOGE("Failed to create string for %s", name);
                return;
            }
            
            env->SetStaticObjectField(buildClass, field, jstr);
            if (checkAndClearException(env, "SetStaticObjectField")) {
                LOGE("Failed to set field %s", name);
            }
            
            env->DeleteLocalRef(jstr);
        };
        
        setField(modelField, info.model, "MODEL");
        setField(brandField, info.brand, "BRAND");
        setField(deviceField, info.device, "DEVICE");
        setField(manufacturerField, info.manufacturer, "MANUFACTURER");
        setField(productField, info.product, "PRODUCT");
        
        LOGI("Spoofed device to: %s %s", info.brand.c_str(), info.model.c_str());
    }
    
    void unloadModule() {
        if (api) {
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            LOGD("Module unloaded (stealth mode)");
        }
    }
};

REGISTER_ZYGISK_MODULE(SpoofModule)


