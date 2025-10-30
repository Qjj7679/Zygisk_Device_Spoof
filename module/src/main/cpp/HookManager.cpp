#include "HookManager.h"
#include <sys/system_properties.h>
#include <sys/mman.h>
#include <string_view>
#include <map>
#include <fstream>
#include <unistd.h>
#include <sys/sysmacros.h>

#ifndef PROPERTY_VALUE_MAX
#define PROPERTY_VALUE_MAX 92
#endif

#ifdef DEBUG
#include <android/log.h>
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "ZygiskDeviceSpoof", __VA_ARGS__)
#else
#define LOGD(...)
#endif

// --- Static Member Initialization ---

int (*HookManager::original_system_property_get)(const char*, char*) = nullptr;
const std::map<std::string, std::string> HookManager::KEY_TO_PROP = {
    {"brand", "ro.product.brand"},
    {"model", "ro.product.model"}
};
const std::map<std::string, std::string> JNI_KEY_TO_FIELD = {
    {"brand", "BRAND"},
    {"model", "MODEL"}
};

// Thread-local storage for passing app-specific properties to the static hook function.
thread_local const std::unordered_map<std::string, std::string>* thread_local_spoof_properties = nullptr;


// --- Helper Functions ---

static std::pair<dev_t, ino_t> get_libc_info() {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.ends_with("libc.so")) {
            unsigned int dev_major, dev_minor;
            ino_t inode;
            if (sscanf(line.c_str(), "%*s %*s %*s %x:%x %lu", &dev_major, &dev_minor, &inode) == 3) {
                return {makedev(dev_major, dev_minor), inode};
            }
        }
    }
    return {0, 0};
}

void HookManager::ensure_libc_info() {
    if (libc_dev == 0 || libc_ino == 0) {
        auto [dev, ino] = get_libc_info();
        if (dev != 0 && ino != 0) {
            libc_dev = dev;
            libc_ino = ino;
        }
    }
}


// --- HookManager Implementation ---

HookManager::HookManager(zygisk::Api* api) : api(api) {}

void HookManager::applyHooks(JNIEnv* env, const std::unordered_map<std::string, std::string>& properties) {
    if (properties.empty()) {
        return;
    }

    LOGD("Applying hooks for current process.");

    installNativeHook(properties);
    spoofJniFields(env, properties);
}

void HookManager::installNativeHook(const std::unordered_map<std::string, std::string>& properties) {
    thread_local_spoof_properties = &properties;

    ensure_libc_info();

    if (libc_dev == 0 || libc_ino == 0) {
        LOGD("Failed to get libc.so info, skipping native hook.");
        return;
    }

    api->pltHookRegister(libc_dev, libc_ino, "__system_property_get",
                         reinterpret_cast<void*>(hooked_system_property_get),
                         reinterpret_cast<void**>(&original_system_property_get));

    if (!api->pltHookCommit()) {
        LOGD("Failed to commit PLT hook.");
        original_system_property_get = nullptr;
    } else {
        LOGD("Successfully hooked __system_property_get.");
    }
}

void HookManager::spoofJniFields(JNIEnv* env, const std::unordered_map<std::string, std::string>& properties) {
    if (!build_class_global_ref) {
        jclass local_build_class = env->FindClass("android/os/Build");
        if (local_build_class == nullptr) {
            LOGD("Failed to find android.os.Build class.");
            return;
        }
        build_class_global_ref = (jclass)env->NewGlobalRef(local_build_class);
        env->DeleteLocalRef(local_build_class);
    }

    if (build_class_global_ref == nullptr) {
        LOGD("Failed to find android.os.Build class.");
        return;
    }

    for (const auto& [key, value] : properties) {
        jfieldID field_id = nullptr;
        if (auto it = field_ids.find(key); it != field_ids.end()) {
            field_id = it->second;
        } else {
            auto jni_it = JNI_KEY_TO_FIELD.find(key);
            if (jni_it == JNI_KEY_TO_FIELD.end()) {
                continue;
            }
            const std::string& field_name = jni_it->second;
            field_id = env->GetStaticFieldID(build_class_global_ref, field_name.c_str(), "Ljava/lang/String;");
            if (field_id) {
                field_ids[key] = field_id;
            }
        }
        
        if (field_id != nullptr) {
            jstring spoof_value = env->NewStringUTF(value.c_str());
            env->SetStaticObjectField(build_class_global_ref, field_id, spoof_value);
            env->DeleteLocalRef(spoof_value);
        } else {
            env->ExceptionClear();
        }
    }
}

int HookManager::hooked_system_property_get(const char* name, char* value) {
    if (name == nullptr || thread_local_spoof_properties == nullptr) {
        return original_system_property_get(name, value);
    }
    
    // Prevent recursion
    thread_local bool in_hook = false;
    if (in_hook) {
        return original_system_property_get(name, value);
    }
    in_hook = true;

    for (const auto& [key, prop_name] : KEY_TO_PROP) {
        if (strcmp(name, prop_name.c_str()) == 0) {
            auto it = thread_local_spoof_properties->find(key);
            if (it != thread_local_spoof_properties->end()) {
                strncpy(value, it->second.c_str(), PROPERTY_VALUE_MAX - 1);
                value[PROPERTY_VALUE_MAX - 1] = '\0';
                in_hook = false;
                return strlen(value);
            }
        }
    }

    int result = original_system_property_get(name, value);
    in_hook = false;
    return result;
}
