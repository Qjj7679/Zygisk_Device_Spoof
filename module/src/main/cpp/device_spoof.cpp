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
#include <vector>
#include <fstream>
#include <sstream>
#include <string_view>
#include <sys/system_properties.h>
// PROPERTY_VALUE_MAX might not be defined in old NDK headers.
#ifndef PROPERTY_VALUE_MAX
#define PROPERTY_VALUE_MAX 92
#endif
#include <sys/sysmacros.h>
#include <time.h>
#include <stdarg.h>

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

// For runtime debug logging
static bool g_is_debug_enabled = false;
static std::mutex g_log_mutex;
constexpr const char* DEBUG_FLAG_PATH = "/data/adb/modules/zygisk_device_spoof/DEBUG";
constexpr const char* LOG_FILE_PATH = "/data/adb/modules/zygisk_device_spoof/debug.log";

static void log_to_file(const char* fmt, ...) {
    if (!g_is_debug_enabled) return;

    std::lock_guard<std::mutex> lock(g_log_mutex);
    FILE* fp = fopen(LOG_FILE_PATH, "a");
    if (!fp) return;

    time_t now = time(nullptr);
    tm tm_now;
    localtime_r(&now, &tm_now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%m-%d %H:%M:%S", &tm_now);
    fprintf(fp, "[%s] [%d] ", time_buf, getpid());

    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
    fprintf(fp, "\n");
    fclose(fp);
}

static void initialize_logging(bool is_companion) {
    if (access(DEBUG_FLAG_PATH, F_OK) != 0) {
        g_is_debug_enabled = false;
        return;
    }

    g_is_debug_enabled = true;
    if (is_companion) {
        // Companion process clears the log file on its first startup
        std::lock_guard<std::mutex> lock(g_log_mutex);
        if (FILE* fp = fopen(LOG_FILE_PATH, "w")) {
            fclose(fp);
        }
    }
    log_to_file("Debug logging enabled for process %d (is_companion=%d)", getpid(), is_companion);
}

// 设备信息现在是一个通用的属性 map
using DeviceInfo = std::unordered_map<std::string, std::string>;

// 全局变量 (Companion 进程)
static std::unordered_map<std::string, DeviceInfo> package_map;
static std::shared_mutex config_mutex;
static std::once_flag companion_init_flag;
static int lock_fd = -1;

// 全局变量 (Zygisk 模块)
static std::once_flag build_class_init_flag;
static DeviceInfo g_spoof_properties; // 存储当前进程的伪装属性
static int (*orig_system_property_get)(const char*, char*);
static jclass buildClass = nullptr;
static jfieldID modelField = nullptr;
static jfieldID brandField = nullptr;
static jfieldID deviceField = nullptr;
static jfieldID manufacturerField = nullptr;
static jfieldID productField = nullptr;


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

        if (app.HasMember("properties") && app["properties"].IsObject()) {
            for (const auto& prop : app["properties"].GetObject()) {
                if (prop.value.IsString()) {
                    std::string value_str = prop.value.GetString();
                    if (!value_str.empty()) { // 关键修复：忽略空值属性
                        info[prop.name.GetString()] = value_str;
                    }
                }
            }
        }

        if (!info.empty()) {
            package_map[package] = info;
            LOGD("Companion: Loaded %zu properties for package: %s", info.size(), package.c_str());
        }
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
    
    auto it = g_spoof_properties.find(name);
    if (it != g_spoof_properties.end()) {
        const std::string& spoof_val = it->second;
        log_to_file("Spoofing property '%s' to '%s'", name, spoof_val.c_str());
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
    initialize_logging(true);

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
        initialize_logging(false);
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

        // 动态读取 Companion 发送的数据
        std::vector<char> buffer(4096);
        int bytes_read = read(fd, buffer.data(), buffer.size() - 1);
        close(fd);

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0'; // 确保 C 风格字符串安全

            // 反序列化属性
            const char* p = buffer.data();
            while (*p) {
                std::string key = p;
                p += key.length() + 1;
                if (!*p) break; // 避免 key 后面没有 value 的情况
                std::string val = p;
                p += val.length() + 1;
                g_spoof_properties[key] = val;
            }

            if (!g_spoof_properties.empty()) {
                spoofDevice(g_spoof_properties);
                LOGI("Package matched: %s, applied %zu properties.", packageName.c_str(), g_spoof_properties.size());

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
                 unloadModule(); // 没有属性需要伪装
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
            if (value.empty() || !field) return;

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

        for (const auto& [key, value] : info) {
            if (key == "ro.product.model") setField(modelField, value, "MODEL");
            else if (key == "ro.product.brand") setField(brandField, value, "BRAND");
            else if (key == "ro.product.device") setField(deviceField, value, "DEVICE");
            else if (key == "ro.product.manufacturer") setField(manufacturerField, value, "MANUFACTURER");
            else if (key == "ro.build.product") setField(productField, value, "PRODUCT");
        }

        LOGI("JNI fields spoofed based on config.");
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
    {
        std::shared_lock<std::shared_mutex> lock(config_mutex);
        auto it = package_map.find(pkg);
        if (it != package_map.end()) {
            const auto& props = it->second;
            std::string buffer;
            for (const auto& [key, val] : props) {
                buffer.append(key);
                buffer.push_back('\0');
                buffer.append(val);
                buffer.push_back('\0');
            }
            if (!buffer.empty()) {
                write(socket_fd, buffer.c_str(), buffer.length());
            }
        }
    }
    close(socket_fd);
}


REGISTER_ZYGISK_MODULE(SpoofModule)
REGISTER_ZYGISK_COMPANION(companion_handler)


