# Zygisk Device Spoof 开发文档

## 1. 项目概述
本项目是一个基于 Zygisk 的高性能 Android 设备信息伪装模块。它通过拦截 Zygote 进程孵化新应用的时机，为特定应用动态修改 `android.os.Build` 类中的设备信息字段。

## 2. 最终架构
模块采用 **客户端-服务器 (Client-Server)** 架构，由 **Zygisk 模块**本身作为客户端，并利用 Zygisk 框架的特性启动一个常驻的 **Companion 进程**作为服务器。

```mermaid
graph TD
    subgraph "Zygisk Framework"
        A[Zygote Process]
    end

    subgraph "Companion Process (Singleton, Root)"
        C[companion_handler]
        C -- Ensures Uniqueness via --> G((companion.lock))
        C -- Manages --> D{Config Cache<br/>(std::unordered_map)}
        C -- Monitors Directory of --> E((config.json))
    end

    subgraph "App Process (Per-App)"
        B[SpoofModule]
        B -- 1. Connects & Sends Pkg --> C
        C -- 2. Lookups Pkg --> D
        D -- 3. Returns DeviceInfo --> C
        C -- 4. Sends DeviceInfoIPC --> B
        B -- 5. Spoofs Build Fields --> F[android.os.Build]
    end

    A -- Forks --> B
    A -- Forks --> C
```

## 3. 核心机制详解

### 3.1. Companion 进程 (`companion_handler` & `companion_init`)
这是模块的核心，作为配置和服务的中心。
- **职责**:
  - 维护一份内存中的配置缓存 (`std::unordered_map`)。
  - 监听来自 Zygisk 模块的 IPC 请求，查询并返回设备信息。
  - 监控配置文件的变更，并执行热重载。
- **关键实现**:
  - **`std::call_once`**: 确保初始化逻辑 `companion_init` 在进程生命周期内只执行一次。
  - **文件锁 (`flock`)**: 在 `companion_init` 中，通过对 `companion.lock` 文件加锁，保证了 Companion 进程的**单例性**，解决了多进程实例导致的数据陈旧问题。
  - **目录监控 (`inotify`)**: `monitor_config_thread` 线程监控 `config` 目录而非单个文件，这使其能可靠地响应各种编辑器的“原子保存”行为，保证了**热重载的健壮性**。
  - **线程安全 (`std::shared_mutex`)**: 在多线程环境（IPC 请求和热重载）下安全地访问配置缓存。

### 3.2. Zygisk 模块 (`SpoofModule`)
这是一个轻量级客户端，在每个目标应用进程中执行。
- **职责**:
  - 在 `preAppSpecialize` 阶段获取当前应用的包名。
  - 通过 Zygisk API 连接到 Companion 进程。
  - 发送包名并接收包含伪造信息的 `DeviceInfoIPC` 结构体。
  - 根据 `match_found` 标志位决定是否调用 JNI 函数修改 `android.os.Build` 字段。
- **关键实现**:
  - **IPC 通信**: 通过 `api->connectCompanion()` 建立与服务端的 socket 连接，进行一次性的请求-响应通信。
  - **隐蔽性**: 在完成任务后，立即调用 `api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY)` 从进程中卸载自身。

### 3.3. 配置文件 (`config.json`)
- **路径**: `/data/adb/modules/zygisk_device_spoof/config/config.json` (模块安装时会自动创建)
- **灵活性**: 支持部分伪造（JSON 中未提供的字段不会被修改）和空值忽略（值为空字符串 `""` 的字段不会被修改）。这是通过 C++ 代码中的 `if (value.empty())` 判断实现的。

## 4. 后续开发注意事项
- **扩展性**:
  - **新增伪造字段**: 如果需要伪造 `android.os.Build` 中的其他字段，只需：
    1.  在 `DeviceInfo` 和 `DeviceInfoIPC` 结构体中增加新成员。
    2.  在 `ensureBuildClass()` 中获取新字段的 `jfieldID`。
    3.  在 `reloadConfig()` 中增加对新字段的 JSON 解析逻辑。
    4.  在 `companion_handler()` 和 `preAppSpecialize()` 中增加新字段的数据复制逻辑。
    5.  在 `spoofDevice()` 中增加对新字段的 `setField` 调用。
- **调试**:
  - `device_spoof.cpp` 文件顶部的 `DEBUG` 宏控制着日志的输出。在开发时可以打开它以获取详细的 `LOGD` 输出。
- **依赖**:
  - 本项目使用了 `rapidjson` 作为 JSON 解析库，已包含在 `module/src/main/cpp/external` 中。
  - C++ 标准为 `c++20`。