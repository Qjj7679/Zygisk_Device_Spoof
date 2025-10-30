#pragma once

#include "zygisk.hpp"
#include "ConfigManager.h"
#include <jni.h>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <map>

class HookManager {
public:
    explicit HookManager(zygisk::Api* api);

    void applyHooks(JNIEnv* env, const std::unordered_map<std::string, std::string>& properties);

private:
    void ensure_libc_info();
    void installNativeHook(const std::unordered_map<std::string, std::string>& properties);
    void spoofJniFields(JNIEnv* env, const std::unordered_map<std::string, std::string>& properties);

    static int hooked_system_property_get(const char* name, char* value);

    zygisk::Api* api;
    dev_t libc_dev = 0;
    ino_t libc_ino = 0;

    jclass build_class_global_ref = nullptr;
    std::unordered_map<std::string, jfieldID> field_ids;
    
    static int (*original_system_property_get)(const char*, char*);
    
    static const std::map<std::string, std::string> KEY_TO_PROP;
};
