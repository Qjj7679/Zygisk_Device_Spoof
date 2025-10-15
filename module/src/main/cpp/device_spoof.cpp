/* Zygisk Device Spoof Module
 * Copyright (C) 2024 Qjj0204
 *
 * A Zygisk module to spoof android.os.Build fields for specific apps.
 */

#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <sys/file.h>
#include <android/log.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <fstream>
#include <sstream>
#include <string_view>
#include <sys/system_properties.h>
// PROPERTY_VALUE_MAX might not be defined in old NDK headers.
#ifndef PROPERTY_VALUE_MAX
#define PROPERTY_VALUE_MAX 92
#endif
#include <sys/sysmacros.h>

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
};

// 用于 IPC 的固定大小设备信息结构
struct DeviceInfoIPC {
    bool match_found;
    char brand[128];
    char model[128];
    char device[128];
    char manufacturer[128];
    char product[128];
};

// 全局变量 (Companion 进程)
static std::unordered_map<std::string, DeviceInfo> package_map;
static std::shared_mutex config_mutex;
static std::once_flag companion_init_flag;
static int lock_fd = -1;

// 全局变量 (Zygisk 模块)
static std::once_flag build_class_init_flag;
static jclass buildClass = nullptr;
static jfieldID modelField = nullptr;
static jfieldID brandField = nullptr;
static jfieldID deviceField = nullptr;
static jfieldID manufacturerField = nullptr;
static jfieldID productField = nullptr;

// 为原生层 Hook 定义的全局变量
static DeviceInfo g_device_info; // 存储当前进程的伪装信息
static int (*orig_system_property_get)(const char*, char*);


// 配置文件路径
constexpr const char* CONFIG_DIR = "/data/adb/modules/zygisk_device_spoof/config/";
constexpr const char* CONFIG_NAME = "config.json";
constexpr const char* COMPANION_LOCK_PATH = "/data/adb/modules/zygisk_device_spoof/companion.lock";

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

// Companion 进程的配置重载逻辑
bool reloadConfig(bool force) {
    if (!force) {
        // 在 inotify 模式下，这个检查不是必需的，但可以保留以防万一
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(config_mutex);

    char config_path[256];
    snprintf(config_path, sizeof(config_path), "%s%s", CONFIG_DIR, CONFIG_NAME);

    FILE* fp = fopen(config_path, "rb");
    if (!fp) {
        LOGE("Companion: Failed to open config file");
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0) {
        fclose(fp);
        LOGE("Companion: Config file is empty or invalid size");
        return false;
    }

    auto buffer = std::make_unique<char[]>(size + 1);
    fread(buffer.get(), 1, size, fp);
    buffer[size] = '\0';
    fclose(fp);

    rapidjson::Document doc;
    doc.Parse(buffer.get());

    if (doc.HasParseError()) {
        LOGE("Companion: JSON parse error: %d", doc.GetParseError());
        return false;
    }

    if (!doc.HasMember("apps") || !doc["apps"].IsArray()) {
        LOGE("Companion: Invalid JSON format: missing 'apps' array");
        return false;
    }

    package_map.clear();
    const auto& apps = doc["apps"].GetArray();
    for (const auto& app : apps) {
        if (!app.IsObject() || !app.HasMember("package") || !app["package"].IsString()) continue;
        
        std::string package = app["package"].GetString();
        DeviceInfo info;

        if (app.HasMember("brand") && app["brand"].IsString()) info.brand = app["brand"].GetString();
        if (app.HasMember("model") && app["model"].IsString()) info.model = app["model"].GetString();
        if (app.HasMember("device") && app["device"].IsString()) info.device = app["device"].GetString();
        if (app.HasMember("manufacturer") && app["manufacturer"].IsString()) info.manufacturer = app["manufacturer"].GetString();
        if (app.HasMember("product") && app["product"].IsString()) info.product = app["product"].GetString();

        package_map[package] = info;
        LOGD("Companion: Loaded config for package: %s", package.c_str());
    }

    LOGI("Companion: Config loaded/reloaded: %zu apps", package_map.size());
    return true;
}

// 辅助函数：获取 libc.so 的设备和 inode 信息
static bool get_libc_info(dev_t& dev, ino_t& inode) {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::string_view sv(line);
        // 通常 libc.so 路径是以 /libc.so 结尾
        if (sv.find("libc.so") != std::string::npos && sv.ends_with(".so")) {
            std::stringstream ss(line);
            std::string addr, perms, offset, dev_str, inode_str, path;
            ss >> addr >> perms >> offset >> dev_str >> inode_str >> path;

            // 确认路径确实是 libc.so
             if (path.ends_with("/libc.so")) {
                unsigned int dev_major, dev_minor;
                if (sscanf(dev_str.c_str(), "%x:%x", &dev_major, &dev_minor) != 2) continue;

                inode = std::stoul(inode_str);
                dev = makedev(dev_major, dev_minor);
                LOGD("Found libc.so in %s: dev=%lx, inode=%lu", path.c_str(), dev, inode);
                return true;
            }
        }
    }
    LOGE("libc.so not found in maps");
    return false;
}

// Hook 函数：替换 __system_property_get
int my_system_property_get(const char* name, char* value) {
    // 使用 thread_local 防止无限递归
    thread_local bool in_hook = false;
    if (in_hook) {
        // 如果我们正在重入，直接调用原始函数
        if (orig_system_property_get) return orig_system_property_get(name, value);
        return -1;
    }

    if (!name || !orig_system_property_get) {
        if (orig_system_property_get) return orig_system_property_get(name, value);
        // 如果原始函数指针为空，无法继续
        return -1;
    }
    
    in_hook = true; // 设置标志
    
    std::string_view prop_name(name);
    std::string spoof_val;

    // 根据属性名称，返回对应的伪装值
    if (prop_name == "ro.product.brand" && !g_device_info.brand.empty()) {
        spoof_val = g_device_info.brand;
    } else if (prop_name == "ro.product.model" && !g_device_info.model.empty()) {
        spoof_val = g_device_info.model;
    } else if (prop_name == "ro.product.device" && !g_device_info.device.empty()) {
        spoof_val = g_device_info.device;
    } else if (prop_name == "ro.product.manufacturer" && !g_device_info.manufacturer.empty()) {
        spoof_val = g_device_info.manufacturer;
    } else if (prop_name == "ro.build.product" && !g_device_info.product.empty()) {
        spoof_val = g_device_info.product;
    }

    if (!spoof_val.empty()) {
        // 关键修复：移除这里的 LOGD 调用以避免无限递归
        strncpy(value, spoof_val.c_str(), PROPERTY_VALUE_MAX - 1);
        value[PROPERTY_VALUE_MAX - 1] = '\0';
        in_hook = false; // 恢复标志
        return strlen(value);
    }

    int result = orig_system_property_get(name, value);
    in_hook = false; // 恢复标志
    return result;
}


void monitor_config_thread() {
    int fd = inotify_init();
    if (fd < 0) {
        LOGE("Companion: inotify_init failed");
        return;
    }

    int wd = inotify_add_watch(fd, CONFIG_DIR, IN_CLOSE_WRITE | IN_MOVED_TO);
    if (wd < 0) {
        LOGW("Companion: inotify_add_watch failed for dir, hot-reload may not work. Errno: %d", errno);
        close(fd);
        return;
    }

    LOGI("Companion: Started monitoring config directory for changes.");
    
    // 缓冲区至少需要容纳一个事件加上一个长文件名
    constexpr size_t EVENT_BUF_LEN = (sizeof(struct inotify_event) + NAME_MAX + 1) * 10;
    char buffer[EVENT_BUF_LEN];

    while (true) {
        int length = read(fd, buffer, EVENT_BUF_LEN);
        if (length < 0) {
            LOGE("Companion: read from inotify fd failed");
            continue;
        }

        int i = 0;
        while (i < length) {
            struct inotify_event *event = (struct inotify_event *) &buffer[i];
            if (event->len) {
                if (strcmp(event->name, CONFIG_NAME) == 0) {
                     if ((event->mask & IN_CLOSE_WRITE) || (event->mask & IN_MOVED_TO)) {
                        LOGI("Companion: Config file changed, reloading...");
                        reloadConfig(true);
                        // 找到我们关心的事件后，可以跳出内层循环
                        break;
                    }
                }
            }
            i += sizeof(struct inotify_event) + event->len;
        }
    }

    inotify_rm_watch(fd, wd);
    close(fd);
}

void companion_init() {
    // 1. 打开或创建锁文件
    lock_fd = open(COMPANION_LOCK_PATH, O_RDWR | O_CREAT, 0644);
    if (lock_fd < 0) {
        LOGE("Companion: Unable to open lock file, exiting.");
        exit(1); // 严重错误
    }

    // 2. 尝试获取非阻塞的排他锁
    if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        LOGW("Companion: Another instance is already running, exiting.");
        close(lock_fd);
        exit(0); // 正常退出
    }

    LOGI("Companion: one-time initialization");
    // 初始加载
    reloadConfig(true);

    // 启动监控线程
    std::thread(monitor_config_thread).detach();
}


class SpoofModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
        ensureBuildClass();
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        if (!args || !args->nice_name) {
            unloadModule();
            return;
        }

        JniString packageName(env, args->nice_name);
        if (!packageName) {
            unloadModule();
            return;
        }

        LOGD("preAppSpecialize: %s", packageName.c_str());

        int fd = api->connectCompanion();
        if (fd < 0) {
            LOGE("Failed to connect to companion process");
            unloadModule();
            return;
        }

        write(fd, packageName.c_str(), strlen(packageName.c_str()) + 1);

        DeviceInfoIPC ipc_info{};
        read(fd, &ipc_info, sizeof(ipc_info));
        close(fd);

        if (ipc_info.match_found) {
            DeviceInfo info {
                .brand = ipc_info.brand,
                .model = ipc_info.model,
                .device = ipc_info.device,
                .manufacturer = ipc_info.manufacturer,
                .product = ipc_info.product,
            };
            g_device_info = info; // 为原生 Hook 保存信息
            spoofDevice(info);
            LOGI("Package matched: %s", packageName.c_str());

            // 添加原生层 Hook
            dev_t libc_dev = 0;
            ino_t libc_ino = 0;
            bool hook_successful = false;
            if (get_libc_info(libc_dev, libc_ino)) {
                api->pltHookRegister(libc_dev, libc_ino, "__system_property_get",
                                     (void*)my_system_property_get,
                                     (void**)&orig_system_property_get);
                if (api->pltHookCommit()) {
                    LOGI("Successfully hooked __system_property_get");
                    hook_successful = true;
                } else {
                    LOGE("Failed to commit __system_property_get hook");
                }
            } else {
                LOGE("Failed to find libc.so information for native hook");
            }
             // 如果 Hook 成功，则不卸载模块，否则卸载
            if (!hook_successful) {
                unloadModule();
            }
        } else {
            LOGD("Package not in config, unloading");
            unloadModule();
        }
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

static void companion_handler(int socket_fd) {
    // 确保初始化逻辑只执行一次
    std::call_once(companion_init_flag, companion_init);

    // 处理来自模块的单个请求
    char pkg[256] = {0};
    int bytes_read = read(socket_fd, pkg, sizeof(pkg));
    if (bytes_read <= 0) {
        LOGE("Companion: read error or client closed, errno: %d", errno);
        close(socket_fd);
        return;
    }

    LOGD("Companion: Received request for package: %s", pkg);
    DeviceInfoIPC ipc_info{};
    ipc_info.match_found = false;
    {
        std::shared_lock<std::shared_mutex> lock(config_mutex);
        auto it = package_map.find(pkg);
        if (it != package_map.end()) {
            ipc_info.match_found = true;
            const auto& info = it->second;
            strncpy(ipc_info.brand, info.brand.c_str(), sizeof(ipc_info.brand) - 1);
            strncpy(ipc_info.model, info.model.c_str(), sizeof(ipc_info.model) - 1);
            strncpy(ipc_info.device, info.device.c_str(), sizeof(ipc_info.device) - 1);
            strncpy(ipc_info.manufacturer, info.manufacturer.c_str(), sizeof(ipc_info.manufacturer) - 1);
            strncpy(ipc_info.product, info.product.c_str(), sizeof(ipc_info.product) - 1);
        }
    }
    write(socket_fd, &ipc_info, sizeof(ipc_info));
    
    close(socket_fd);
}


REGISTER_ZYGISK_MODULE(SpoofModule)
REGISTER_ZYGISK_COMPANION(companion_handler)


