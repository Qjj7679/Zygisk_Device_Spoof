# Zygisk Device Spoof 开发文档

## 1. 项目概述
本项目是一个基于 Zygisk 的高性能 Android 设备信息伪装模块。它实现了一个**通用的、由配置驱动的属性伪装引擎**，通过双层 Hook 机制，为特定应用动态地、一致地修改设备属性信息。

## 2. 最终架构
模块采用一种高效的**请求-响应**模型。**Zygisk 模块**作为客户端，在每个目标应用启动时，通过 Zygisk 框架与 **Companion 进程**进行一次性的 IPC 通信来获取伪装数据。

```mermaid
graph TD
    subgraph "Zygisk Framework"
        A[Zygote Process]
    end

    subgraph "Companion Process (Forked per Request, Root)"
        C[companion_handler]
        C -- Ensures Serial Execution via --> G((companion.lock))
        C -- Reads & Parses --> E((config.json))
    end

    subgraph "App Process (Per-App)"
        B[SpoofModule]
        B -- 1. Connects & Sends Pkg --> C
        C -- 2. Finds Match & Serializes Props --> C
        C -- 3. Sends Serialized Props via IPC --> B
        B -- 4. Deserializes Props & Applies Hooks --> H{"Dual-Layer Hooking"}
        H -- JNI Hook --> F[android.os.Build]
        H -- Native PLT Hook --> I[libc.so<br>(__system_property_get)]
    end

    A -- Forks --> B
    A -- Forks & Connects --> C
```

## 3. 核心机制详解

### 3.1. 双层 Hook 机制
这是模块伪装能力的核心，确保了最大程度的兼容性和一致性。

- **JNI 层 Hook (`spoofDevice`)**:
  - **目标**: `android.os.Build` 类。
  - **实现**: 在 `preAppSpecialize` 阶段，通过 JNI `SetStaticObjectField` 函数直接修改 `android.os.Build` 类的静态字段（如 `MODEL`, `BRAND` 等）。
  - **作用**: 覆盖通过标准 Android Java API 获取设备信息的应用。

- **Native 层 Hook (`my_system_property_get`)**:
  - **目标**: `libc.so` 中的 `__system_property_get` 函数。
  - **实现**: 使用 Zygisk 的 `pltHookRegister` API，在运行时替换 `__system_property_get` 函数的地址。
  - **作用**: 拦截所有直接通过底层 C/C++ 库查询系统属性的调用，这是许多游戏和加固应用获取设备信息的方式。

- **重入保护 (`is_jni_hooking`)**:
  - **问题**: 在 JNI Hook 执行期间，内部实现可能会触发 `__system_property_get`，如果此时 Native Hook 也生效，会造成无限递归或死锁。
  - **方案**: 使用 `thread_local bool is_jni_hooking` 变量作为卫兵。在执行 `spoofDevice` 之前将其设为 `true`，之后设为 `false`。`my_system_property_get` 函数会检查此标志，如果为 `true`，则直接调用原始函数，从而打破了循环依赖。

### 3.2. Companion 进程 (`companion_handler`)
这是一个按需唤醒的辅助进程，负责安全地提供配置信息。

- **职责**:
  - 接收来自 Zygisk 模块的 IPC 连接，并读取目标应用的包名。
  - 读取并解析 `config.json` 文件。
  - 查找与目标包名匹配的伪装规则。
  - 将匹配到的所有属性序列化为一个长字符串（键和值由 `\0` 分隔）。
  - 将序列化后的字符串通过 socket 写回给 Zygisk 模块。
- **关键实现**:
  - **串行执行 (`flock`)**: 在处理请求的开始，通过对 `companion.lock` 文件加锁 (`LOCK_EX`)，保证了即使多个应用同时启动，Companion 进程也能串行、安全地访问配置文件，从根本上杜绝了文件读取的竞争条件。
  - **一次性**: 每个 Companion 进程在完成一次请求-响应后即退出，不作为常驻服务，资源占用低。

### 3.3. Zygisk 模块 (`SpoofModule`)
这是在每个目标应用进程中执行的轻量级客户端。

- **职责**:
  - 在 `preAppSpecialize` 阶段，通过 `api->connectCompanion()` 连接到 Companion 进程。
  - 发送包名并接收序列化的属性数据。
  - 将接收到的数据反序列化，存入全局的 `g_spoof_properties` 中。
  - 如果收到了有效的伪装数据，则启动并应用**双层 Hook 机制**。
- **关键实现**:
  - **隐蔽性**: 在完成任务后，调用 `api->setOption(zygisk::Option::FORCE_DENYLIST_UNMOUNT)` 将自身从应用的 Denylist 中移除，增加了隐蔽性。

### 3.4. 配置文件 (`config.json`)
- **路径**: `/data/adb/modules/zygisk_device_spoof/config/config.json`
- **灵活性**:
    - **部分伪装**: JSON 中未提供的属性不会被修改。
    - **空值忽略**: 值为空字符串 `""` 的属性会被自动忽略。

## 4. 后续开发注意事项
- **调试**:
  - `device_spoof.cpp` 文件顶部的 `DEBUG` 宏控制着日志的输出。在开发时可以打开它以获取详细的 `LOGD` 输出。
- **依赖**:
  - 本项目使用了 `rapidjson` 作为 JSON 解析库，已包含在 `module/src/main/cpp/external` 中。
  - C++ 标准为 `c++20`。