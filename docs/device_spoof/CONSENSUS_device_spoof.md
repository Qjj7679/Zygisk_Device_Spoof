# CONSENSUS - Android 设备信息伪装模块

## 1. 明确的需求描述

### 1.1 核心需求
开发一个基于 Zygisk 的 Android 设备信息伪装模块，能够在目标应用启动时动态修改 `android.os.Build` 类的静态字段，使应用读取到伪造的设备信息，从而绕过设备型号检测机制。

### 1.2 功能需求
1. **设备信息伪装**
   - 修改 `android.os.Build.MODEL`
   - 修改 `android.os.Build.BRAND`
   - 修改 `android.os.Build.DEVICE`
   - 修改 `android.os.Build.MANUFACTURER`
   - 修改 `android.os.Build.PRODUCT`

2. **包名精确匹配**
   - 根据应用包名决定是否执行伪装
   - 支持多个应用配置（一对一映射）
   - 非目标应用不受影响

3. **配置文件管理**
   - JSON 格式配置文件
   - 路径: `/data/adb/modules/zygisk_device_spoof/config/config.json`
   - 缓存路径: `/data/adb/modules/zygisk_device_spoof/cache/config_cached.json`
   - 支持热重载（通过守护进程监控）

4. **线程安全**
   - 配置加载使用互斥锁保护
   - Build 类加载使用 `std::once_flag` 确保单次执行

5. **隐身模式**
   - 伪装完成后卸载模块库（`DLCLOSE_MODULE_LIBRARY`）
   - 降低被检测风险

6. **调试模式**
   - Debug 构建：输出详细日志
   - Release 构建：关闭日志输出
   - 日志标签: `ZygiskDeviceSpoof`

## 2. 技术实现方案

### 2.1 整体架构
```
┌─────────────────────────────────────────────────────────────┐
│                      Zygisk Framework                        │
└─────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                   SpoofModule (ModuleBase)                   │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │   onLoad()   │  │preAppSpec()  │  │postAppSpec() │      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
└─────────────────────────────────────────────────────────────┘
                            │
        ┌───────────────────┼───────────────────┐
        ▼                   ▼                   ▼
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ Config Mgr   │  │  JNI Spoofer │  │ Thread Safe  │
│ - Load JSON  │  │ - Find Class │  │ - std::mutex │
│ - Parse      │  │ - Modify     │  │ - once_flag  │
│ - Cache      │  │   Fields     │  │              │
└──────────────┘  └──────────────┘  └──────────────┘
        │
        ▼
┌──────────────────────────────────────────────────────────┐
│              service.sh (Daemon Process)                  │
│  - Monitor config.json changes (inotifywait/polling)     │
│  - Sync to config_cached.json                            │
└──────────────────────────────────────────────────────────┘
```

### 2.2 核心组件

#### 2.2.1 SpoofModule 类
- **继承**: `zygisk::ModuleBase`
- **职责**: 模块生命周期管理、协调各组件
- **关键方法**:
  - `onLoad()`: 初始化环境，加载配置
  - `preAppSpecialize()`: 主要伪装时机
  - `postAppSpecialize()`: 补充验证
  - `onUnload()`: 清理资源

#### 2.2.2 配置管理模块
- **职责**: 配置文件加载、解析、缓存
- **关键功能**:
  - 优先读取缓存文件（`config_cached.json`）
  - 使用 `stat()` 检测文件修改时间
  - RapidJSON 解析 JSON
  - 填充 `std::unordered_map<std::string, DeviceInfo>`
- **线程安全**: 使用 `std::mutex` 保护

#### 2.2.3 JNI 伪装模块
- **职责**: 查找和修改 `android.os.Build` 字段
- **关键功能**:
  - 查找 `android/os/Build` 类
  - 获取静态字段 ID
  - 使用 `SetStaticObjectField` 修改字段
  - 异常处理和清理
- **线程安全**: 使用 `std::once_flag` 确保类加载唯一性

#### 2.2.4 守护进程 (service.sh)
- **职责**: 监控配置文件变更
- **实现方式**:
  1. 优先使用 `inotifywait` 监控 `close_write`, `modify`, `moved_to`, `attrib` 事件
  2. 回退到轮询模式（每 2 秒检查 mtime）
  3. 检测到变更后复制到缓存文件
- **权限控制**: 缓存文件权限 0644

### 2.3 数据结构

#### DeviceInfo 结构体
```cpp
struct DeviceInfo {
    std::string brand;
    std::string model;
    std::string device;
    std::string manufacturer;
    std::string product;
};
```

#### 全局变量
```cpp
// 配置映射: 包名 -> 设备信息
std::unordered_map<std::string, DeviceInfo> package_map;

// 当前应用的伪装信息
DeviceInfo current_info;

// 线程安全
std::mutex config_mutex;
std::once_flag build_class_init_flag;

// JNI 缓存
jclass buildClass = nullptr;
jfieldID modelField = nullptr;
jfieldID brandField = nullptr;
jfieldID deviceField = nullptr;
jfieldID manufacturerField = nullptr;
jfieldID productField = nullptr;

// 配置文件路径
const char* config_cache_path = "/data/adb/modules/zygisk_device_spoof/cache/config_cached.json";
const char* config_path = "/data/adb/modules/zygisk_device_spoof/config/config.json";
```

### 2.4 工作流程

#### 2.4.1 模块加载流程
```
onLoad()
  ├─> 保存 api 和 env
  ├─> ensureBuildClass() [std::call_once]
  │     ├─> FindClass("android/os/Build")
  │     ├─> NewGlobalRef(buildClass)
  │     ├─> GetStaticFieldID("MODEL", "Ljava/lang/String;")
  │     ├─> GetStaticFieldID("BRAND", "Ljava/lang/String;")
  │     ├─> GetStaticFieldID("DEVICE", "Ljava/lang/String;")
  │     ├─> GetStaticFieldID("MANUFACTURER", "Ljava/lang/String;")
  │     └─> GetStaticFieldID("PRODUCT", "Ljava/lang/String;")
  └─> reloadIfNeeded(true) [force reload]
        ├─> 优先读取 config_cached.json
        ├─> 回退到 config.json
        ├─> RapidJSON 解析
        └─> 填充 package_map
```

#### 2.4.2 应用伪装流程
```
preAppSpecialize(args)
  ├─> 获取包名 (args->nice_name)
  ├─> 检查包名是否为空
  ├─> reloadIfNeeded(false) [check mtime]
  ├─> 在 package_map 中查找包名
  │     ├─> 找到: 设置 current_info
  │     │         ├─> spoofDevice(current_info)
  │     │         │     ├─> NewStringUTF(info.model)
  │     │         │     ├─> SetStaticObjectField(buildClass, modelField, ...)
  │     │         │     ├─> DeleteLocalRef(...)
  │     │         │     └─> 重复处理其他字段
  │     │         └─> 记录日志
  │     └─> 未找到: 卸载模块
  └─> setOption(DLCLOSE_MODULE_LIBRARY) [隐身模式]

postAppSpecialize(args)
  ├─> 确保 buildClass 已加载
  ├─> 再次查找包名并伪装 (防止字段被重置)
  └─> setOption(DLCLOSE_MODULE_LIBRARY)
```

#### 2.4.3 配置热重载流程
```
service.sh (后台守护进程)
  ├─> 创建 cache 目录
  ├─> 初始复制 config.json -> config_cached.json
  └─> 循环监控
        ├─> inotifywait 监控 config.json (如果可用)
        │     └─> 检测到变更 -> 复制到 cache
        └─> 轮询模式 (回退方案)
              ├─> sleep 2 秒
              ├─> 检查 config.json 是否比 cache 新
              └─> 是 -> 复制到 cache
```

## 3. 技术约束和集成方案

### 3.1 技术约束
1. **Android 版本**: API 26 (Android 8.0) - API 36 (Android 15)
2. **架构支持**: arm64-v8a, armeabi-v7a, x86, x86_64
3. **C++ 标准**: C++20
4. **编译选项**: `-fno-exceptions -fno-rtti`
5. **依赖库**: 
   - Zygisk API (已有)
   - 自定义 libc++ (已有)
   - RapidJSON v1.1.0 (需添加)
6. **构建系统**: CMake 3.22.1+, Gradle 8.11.0

### 3.2 集成方案

#### 3.2.1 RapidJSON 集成
- **方式**: Header-only 模式
- **路径**: `module/src/main/cpp/external/rapidjson/include/`
- **版本**: v1.1.0
- **配置**: 在 CMakeLists.txt 中添加 include 路径

#### 3.2.2 构建配置修改
1. **根 build.gradle.kts**:
   - `moduleId`: `"sample"` → `"zygisk_device_spoof"`
   - `moduleName`: `"Zygisk Module Sample"` → `"Zygisk Device Spoof"`
   - `namespace`: 添加 `io.github.qjj0204.zygisk.device.spoof`

2. **CMakeLists.txt**:
   - 源文件: `example.cpp` → `device_spoof.cpp`
   - 添加 RapidJSON include 路径
   - 移除 Companion 相关代码

3. **module.prop**:
   - `id`: `zygisk_device_spoof`
   - `name`: `Zygisk Device Spoof`
   - `author`: `Qjj0204`
   - `description`: `A module used to spoof device info.`

#### 3.2.3 文件结构
```
module/
├── src/main/cpp/
│   ├── device_spoof.cpp          [新建] 主模块实现
│   ├── CMakeLists.txt             [修改] 更新源文件和依赖
│   ├── zygisk.hpp                 [保留] Zygisk API
│   └── external/
│       ├── rapidjson/             [新建] RapidJSON 库
│       │   └── include/
│       │       └── rapidjson/
│       │           ├── document.h
│       │           ├── rapidjson.h
│       │           └── ...
│       └── libcxx/                [保留] 自定义 libc++
└── template/
    ├── module.prop                [修改] 模块信息
    ├── service.sh                 [修改] 配置监控守护进程
    ├── customize.sh               [保留] 安装脚本
    └── config/                    [新建] 配置目录
        └── config.json            [新建] 示例配置
```

## 4. 任务边界限制

### 4.1 功能边界
**实现范围**:
- ✅ 修改 `android.os.Build` 的 5 个字段
- ✅ 基于包名的精确匹配
- ✅ JSON 配置文件解析
- ✅ 配置热重载（守护进程）
- ✅ 线程安全保障
- ✅ 隐身模式
- ✅ 调试日志控制

**不实现范围**:
- ❌ 修改系统属性（SystemProperties）
- ❌ 修改硬件标识（IMEI、MAC 等）
- ❌ GUI 配置界面
- ❌ 配置加密
- ❌ 网络远程配置
- ❌ 包名正则匹配
- ❌ 运行时动态配置

### 4.2 性能边界
- 配置加载: < 100ms
- 伪装操作: < 10ms
- 应用启动延迟: < 50ms
- 内存占用: < 1MB

### 4.3 兼容性边界
- 最低版本: Android 8.0 (API 26)
- 最高版本: Android 15 (API 36)
- 支持架构: arm64-v8a, armeabi-v7a, x86, x86_64
- 支持框架: Magisk, KernelSU

## 5. 验收标准

### 5.1 功能验收
1. **编译验收**
   - [ ] 项目成功编译，无错误
   - [ ] 生成 4 个架构的 SO 文件
   - [ ] 生成 Magisk/KernelSU 模块 ZIP 包

2. **安装验收**
   - [ ] 模块可在 Magisk 中安装
   - [ ] 模块可在 KernelSU 中安装
   - [ ] 安装后配置文件正确创建

3. **功能验收**
   - [ ] 配置文件正确解析（多应用配置）
   - [ ] 目标应用的 Build 字段被正确修改
   - [ ] 非目标应用不受影响
   - [ ] 配置修改后热重载生效

4. **日志验收**
   - [ ] Debug 版本输出详细日志
   - [ ] Release 版本无日志输出
   - [ ] 日志标签为 `ZygiskDeviceSpoof`

5. **隐身验证**
   - [ ] 伪装后模块成功卸载
   - [ ] `/proc/[pid]/maps` 中无模块痕迹

### 5.2 性能验收
- [ ] 配置加载时间 < 100ms
- [ ] 伪装操作时间 < 10ms
- [ ] 应用启动延迟 < 50ms
- [ ] 守护进程 CPU 占用 < 1%

### 5.3 稳定性验收
- [ ] 配置文件缺失时模块正常卸载
- [ ] 配置文件格式错误时模块正常卸载
- [ ] JNI 操作失败时不导致应用崩溃
- [ ] 目标应用正常运行，无崩溃
- [ ] 系统稳定，无重启

### 5.4 兼容性验收
- [ ] 在 Android 8.0 上测试通过
- [ ] 在 Android 12 上测试通过
- [ ] 在 Android 14 上测试通过
- [ ] 在 arm64-v8a 设备上测试通过
- [ ] 在 armeabi-v7a 设备上测试通过

## 6. 确认所有不确定性已解决

### 6.1 已解决的决策点
- ✅ RapidJSON 集成方式: Header-only 模式
- ✅ 配置监控机制: inotifywait + 轮询回退
- ✅ 调试日志控制: 构建变体控制
- ✅ 配置文件缺失处理: 记录错误并卸载
- ✅ 包名匹配失败处理: 立即卸载模块
- ✅ 伪装时机: preAppSpecialize 主要，postAppSpecialize 补充
- ✅ 命名空间: `io.github.qjj0204.zygisk.device.spoof`

### 6.2 技术实现细节已明确
- ✅ JNI 字段修改方式: `SetStaticObjectField`
- ✅ 线程安全机制: `std::mutex` + `std::once_flag`
- ✅ 配置文件路径: `/data/adb/modules/zygisk_device_spoof/config/config.json`
- ✅ 缓存文件路径: `/data/adb/modules/zygisk_device_spoof/cache/config_cached.json`
- ✅ 日志标签: `ZygiskDeviceSpoof`
- ✅ 文件权限: 目录 0755, 文件 0644

### 6.3 风险缓解措施已明确
- ✅ JNI 异常处理: `ExceptionCheck()` + `ExceptionClear()`
- ✅ 配置解析失败: 记录日志并卸载
- ✅ SELinux 权限: 使用标准 `/data/adb/modules/` 路径
- ✅ 兼容性问题: 多版本测试验证

## 7. 下一步

进入 **阶段2: Architect (架构阶段)**，创建详细的系统架构设计文档 `DESIGN_device_spoof.md`。


