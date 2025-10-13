# TODO - Android 设备信息伪装模块待办事项

## 📋 必须完成的配置

### 1. 配置 Android SDK 路径 ⚠️

**优先级**: 🔴 **最高**（阻塞编译）

**问题**: 项目需要 Android SDK 才能编译

**解决方案**（二选一）:

#### 方法 A: 设置环境变量（推荐）
```bash
# Windows PowerShell
$env:ANDROID_HOME = "C:\Users\YourUsername\AppData\Local\Android\Sdk"

# Windows CMD
set ANDROID_HOME=C:\Users\YourUsername\AppData\Local\Android\Sdk

# Linux/Mac
export ANDROID_HOME=/path/to/android/sdk
```

#### 方法 B: 修改 local.properties
编辑项目根目录的 `local.properties` 文件：
```properties
sdk.dir=C\:\\Users\\YourUsername\\AppData\\Local\\Android\\Sdk
```

**常见 SDK 路径**:
- Windows: `C:\Users\YourUsername\AppData\Local\Android\Sdk`
- Mac: `/Users/YourUsername/Library/Android/sdk`
- Linux: `/home/YourUsername/Android/Sdk`

**验证方法**:
```bash
# 检查环境变量
echo $ANDROID_HOME  # Linux/Mac
echo %ANDROID_HOME%  # Windows CMD
$env:ANDROID_HOME   # Windows PowerShell

# 检查 SDK 目录
ls $ANDROID_HOME/platforms  # 应该看到 android-XX 目录
```

---

### 2. 安装必需的 Android SDK 组件 ⚠️

**优先级**: 🔴 **最高**（阻塞编译）

**所需组件**:
- Android SDK Platform 36 (Android 15)
- Android SDK Build-Tools 36.0.0
- Android NDK 28.1.13356709

**安装方法**:

#### 使用 Android Studio（推荐）
1. 打开 Android Studio
2. Tools → SDK Manager
3. SDK Platforms 标签页:
   - 勾选 "Android 15.0 (API 36)"
4. SDK Tools 标签页:
   - 勾选 "Android SDK Build-Tools 36.0.0"
   - 勾选 "NDK (Side by side)" → 选择 28.1.13356709
5. 点击 "Apply" 安装

#### 使用命令行
```bash
# 使用 sdkmanager
sdkmanager "platforms;android-36"
sdkmanager "build-tools;36.0.0"
sdkmanager "ndk;28.1.13356709"
```

**验证方法**:
```bash
ls $ANDROID_HOME/platforms/android-36
ls $ANDROID_HOME/build-tools/36.0.0
ls $ANDROID_HOME/ndk/28.1.13356709
```

---

## 🔨 编译和测试

### 3. 执行编译 🟡

**优先级**: 🟡 **高**（验证代码）

**前置条件**:
- ✅ Android SDK 已配置
- ✅ NDK 已安装

**编译步骤**:

#### Step 1: 清理构建缓存
```bash
./gradlew clean
```

#### Step 2: 编译 Debug 版本
```bash
./gradlew :module:assembleDebug
```

**预期输出**:
```
BUILD SUCCESSFUL in XXs
```

#### Step 3: 生成模块 ZIP 包
```bash
./gradlew :module:zipDebug
```

**产物位置**:
- SO 文件: `module/build/intermediates/stripped_native_libs/debug/`
- ZIP 包: `module/release/Zygisk-Device-Spoof-v1.0.0-XXX-debug.zip`

#### Step 4: 编译 Release 版本（可选）
```bash
./gradlew :module:assembleRelease
./gradlew :module:zipRelease
```

**验证方法**:
```bash
# 检查 SO 文件
ls module/build/intermediates/stripped_native_libs/debug/out/lib/
# 应该看到 4 个架构目录: arm64-v8a, armeabi-v7a, x86, x86_64

# 检查 ZIP 包
ls module/release/
# 应该看到 .zip 文件

# 查看 ZIP 包内容
unzip -l module/release/Zygisk-Device-Spoof-*.zip
```

**常见问题**:

| 错误 | 原因 | 解决方法 |
|------|------|----------|
| SDK location not found | SDK 路径未配置 | 参见 TODO #1 |
| NDK not found | NDK 未安装 | 参见 TODO #2 |
| Compilation failed | 代码错误 | 查看错误日志，修复代码 |
| Out of memory | 内存不足 | 增加 Gradle 内存: `org.gradle.jvmargs=-Xmx4096m` |

---

### 4. 在设备上测试 🟡

**优先级**: 🟡 **高**（验证功能）

**前置条件**:
- ✅ 模块已编译
- ✅ 有 Root 设备（Magisk 或 KernelSU）
- ✅ ADB 已连接

**测试步骤**:

#### Step 1: 安装模块
```bash
# 推送模块到设备
adb push module/release/Zygisk-Device-Spoof-*.zip /data/local/tmp/

# 安装模块（Magisk）
adb shell su -c "magisk --install-module /data/local/tmp/Zygisk-Device-Spoof-*.zip"

# 或安装模块（KernelSU）
adb shell su -c "/data/adb/ksud module install /data/local/tmp/Zygisk-Device-Spoof-*.zip"

# 重启设备
adb reboot
```

#### Step 2: 验证安装
```bash
# 等待设备重启完成
adb wait-for-device

# 检查模块目录
adb shell su -c "ls -la /data/adb/modules/zygisk_device_spoof/"

# 检查配置文件
adb shell su -c "cat /data/adb/modules/zygisk_device_spoof/config/config.json"

# 检查守护进程
adb shell su -c "ps | grep service.sh"
```

#### Step 3: 修改配置文件
```bash
# 编辑配置文件，添加测试应用包名
# 例如: com.android.settings

# 方法 A: 使用 vi 编辑
adb shell su -c "vi /data/adb/modules/zygisk_device_spoof/config/config.json"

# 方法 B: 从本地推送
# 1. 在本地编辑 test_config.json
# 2. 推送到设备
adb push test_config.json /data/local/tmp/
adb shell su -c "cp /data/local/tmp/test_config.json /data/adb/modules/zygisk_device_spoof/config/config.json"

# 等待缓存同步（2-3 秒）
sleep 3

# 验证缓存文件
adb shell su -c "cat /data/adb/modules/zygisk_device_spoof/cache/config_cached.json"
```

#### Step 4: 测试伪装效果
```bash
# 启动日志监控（Debug 版本）
adb logcat -c
adb logcat -s ZygiskDeviceSpoof:* &

# 启动测试应用
adb shell am start -n com.android.settings/.Settings

# 查看日志
adb logcat | grep ZygiskDeviceSpoof
```

**预期日志**:
```
ZygiskDeviceSpoof: Module loaded, version: 1.0.0
ZygiskDeviceSpoof: Build class initialized successfully
ZygiskDeviceSpoof: Config loaded: 2 apps
ZygiskDeviceSpoof: preAppSpecialize: com.android.settings
ZygiskDeviceSpoof: Package matched: com.android.settings
ZygiskDeviceSpoof: Spoofed device to: Google Pixel 7
ZygiskDeviceSpoof: Module unloaded (stealth mode)
```

#### Step 5: 验证隐身模式
```bash
# 获取应用 PID
PID=$(adb shell pidof com.android.settings)

# 检查模块是否已卸载
adb shell su -c "cat /proc/$PID/maps | grep zygisk_device_spoof"
# 应该无输出，表示模块已卸载
```

#### Step 6: 测试热重载
```bash
# 修改配置文件（例如改为 Pixel 8）
adb shell su -c "sed -i 's/Pixel 7/Pixel 8/g' /data/adb/modules/zygisk_device_spoof/config/config.json"

# 等待缓存同步
sleep 3

# 杀死并重启应用
adb shell am force-stop com.android.settings
adb shell am start -n com.android.settings/.Settings

# 查看日志，应该看到 "Pixel 8"
adb logcat | grep "Pixel 8"
```

**验收标准**:
- [ ] 模块成功安装
- [ ] 配置文件正确创建
- [ ] 目标应用 Build 字段被修改
- [ ] 非目标应用不受影响
- [ ] 配置修改后热重载生效
- [ ] Debug 版本输出日志
- [ ] 模块成功卸载（隐身模式）

---

## 📝 可选的改进

### 5. 性能测试 🟢

**优先级**: 🟢 **中**（可选）

**测试项目**:
1. 配置加载时间（目标 < 100ms）
2. 伪装操作时间（目标 < 10ms）
3. 应用启动延迟（目标 < 50ms）
4. 守护进程 CPU 占用（目标 < 1%）

**测试方法**:

#### 测试配置加载时间
在 `device_spoof.cpp` 中添加时间戳：
```cpp
auto start = std::chrono::high_resolution_clock::now();
reloadIfNeeded(true);
auto end = std::chrono::high_resolution_clock::now();
auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
LOGI("Config load time: %lld ms", duration.count());
```

#### 测试应用启动延迟
```bash
# 启用模块前
adb shell am start -W -n com.android.settings/.Settings
# 记录 TotalTime

# 启用模块后
adb shell am start -W -n com.android.settings/.Settings
# 记录 TotalTime

# 计算差值
```

#### 测试守护进程 CPU 占用
```bash
# 查找 service.sh 进程 PID
SERVICE_PID=$(adb shell su -c "ps | grep service.sh | awk '{print \$2}'")

# 监控 CPU 占用
adb shell su -c "top -n 10 -d 1 | grep ${SERVICE_PID}"
```

---

### 6. 扩展设备信息库 🟢

**优先级**: 🟢 **低**（增强功能）

**建议**: 在 `module/template/config/README.md` 中添加更多设备信息

**常见设备**:
- Google Pixel 系列（Pixel 6-9）
- Samsung Galaxy 系列（S21-S24, Note 系列）
- Xiaomi 系列（小米 11-14, Redmi 系列）
- OnePlus 系列（OnePlus 9-12）
- OPPO/Vivo 系列

**信息来源**:
- [Android Device List](https://storage.googleapis.com/play_public/supported_devices.html)
- [GSMArena](https://www.gsmarena.com/)
- 实际设备的 `adb shell getprop`

---

### 7. 添加配置验证 🟢

**优先级**: 🟢 **低**（增强功能）

**建议**: 在 `device_spoof.cpp` 中添加配置验证逻辑

**验证项目**:
- JSON 格式正确性
- 必需字段完整性（package 字段）
- 包名格式正确性（正则表达式）
- 字段值合理性（非空字符串）

**示例代码**:
```cpp
bool validateConfig(const rapidjson::Document& doc) {
    if (!doc.HasMember("apps") || !doc["apps"].IsArray()) {
        LOGE("Invalid config: missing 'apps' array");
        return false;
    }
    
    const auto& apps = doc["apps"].GetArray();
    for (const auto& app : apps) {
        if (!app.HasMember("package") || !app["package"].IsString()) {
            LOGE("Invalid config: missing 'package' field");
            return false;
        }
        
        std::string package = app["package"].GetString();
        if (package.empty()) {
            LOGE("Invalid config: empty package name");
            return false;
        }
        
        // 验证包名格式（简单正则）
        if (package.find('.') == std::string::npos) {
            LOGE("Invalid config: invalid package name format");
            return false;
        }
    }
    
    return true;
}
```

---

## 🐛 已知问题

### 当前无已知问题

如果在测试过程中发现问题，请记录在此处：

**问题模板**:
```
### 问题 #X: [问题标题]

**描述**: [问题详细描述]

**复现步骤**:
1. [步骤 1]
2. [步骤 2]
3. [步骤 3]

**预期行为**: [应该发生什么]

**实际行为**: [实际发生了什么]

**环境**:
- Android 版本: [例如 Android 14]
- 设备型号: [例如 Pixel 7]
- Magisk/KernelSU 版本: [例如 Magisk 27.0]

**日志**:
```
[粘贴相关日志]
```

**解决方案**: [如果已解决，描述解决方法]
```

---

## 📚 参考文档

### 项目文档
- `docs/device_spoof/ALIGNMENT_device_spoof.md` - 需求对齐文档
- `docs/device_spoof/CONSENSUS_device_spoof.md` - 共识文档
- `docs/device_spoof/DESIGN_device_spoof.md` - 架构设计文档
- `docs/device_spoof/TASK_device_spoof.md` - 任务拆分文档
- `docs/device_spoof/ACCEPTANCE_device_spoof.md` - 验收报告
- `docs/device_spoof/FINAL_device_spoof.md` - 项目总结

### 配置说明
- `module/template/config/README.md` - 配置文件格式说明

### 外部资源
- [Magisk 官方文档](https://topjohnwu.github.io/Magisk/)
- [KernelSU 官方文档](https://kernelsu.org/)
- [Zygisk API 参考](https://github.com/topjohnwu/Magisk/blob/master/native/src/zygisk/api.hpp)
- [RapidJSON 文档](https://rapidjson.org/)
- [Android JNI 文档](https://developer.android.com/training/articles/perf-jni)

---

## ✅ 完成清单

### 必须完成
- [ ] 配置 Android SDK 路径
- [ ] 安装必需的 SDK 组件
- [ ] 执行编译验证
- [ ] 在设备上测试功能

### 可选完成
- [ ] 执行性能测试
- [ ] 扩展设备信息库
- [ ] 添加配置验证

---

## 📞 获取帮助

如果遇到问题，请：

1. **查看文档**: 先查阅项目文档和参考资料
2. **检查日志**: 使用 `adb logcat` 查看详细日志
3. **搜索问题**: 在 GitHub Issues 或相关论坛搜索类似问题
4. **提供信息**: 报告问题时提供完整的环境信息和日志

---

## 🎉 完成后

当所有必须完成的任务都完成后：

1. ✅ 模块已成功编译
2. ✅ 模块已在设备上测试
3. ✅ 所有功能正常工作
4. ✅ 性能符合预期

**恭喜！** 🎊 您已成功完成 Zygisk Device Spoof 模块的开发和部署！

---

**最后更新**: 2025-10-13  
**项目版本**: v1.0.0


