# 开发者文档：Zygisk Device Spoof (重构版)

## 1. 项目概述

本项目是一个基于 Zygisk 框架的现代化 Android 设备属性伪装模块。它采用 C++20 标准，通过健壮的**双层 Hook** 机制和高性能的 **Companion 进程架构**，为特定应用程序动态地、安全地修改设备标识。

## 2. 架构设计

模块的核心设计思想是**职责分离**与**性能优先**。我们将耗时的文件操作与应用启动的关键路径完全解耦，以确保最高的稳定性和性能。

```mermaid
graph TD
    subgraph "应用进程空间"
        A[应用启动<br>(Zygote Fork)] --> B{SpoofModule::preAppSpecialize};
        B --> |1. 获取应用名| C{连接 Companion};
        C --> |2. 发送应用名| D[IPC Socket];
        D --> |5. 接收伪装数据| E{应用 Hook};
        E --> |JNI Hook| F[android.os.Build];
        E --> |Native PLT Hook| G[libc.so::__system_property_get];
    end

    subgraph "Root 权限后台空间"
        I(Companion 进程<br>companion_handler) --> J{ConfigManager};
        J --> K[读取/解析<br>config.json];
    end

    subgraph "进程间通信 (IPC)"
        D -- 3. 请求 --> I;
        I -- 4. 返回数据 --> D;
    end
```

### 2.1. 核心组件职责

- **`SpoofModule` (Zygote 模块)**:
  - **职责**: 作为在每个目标应用进程中运行的“客户端”，它的任务是**轻量**和**快速**的。
  - **生命周期**: 在 `preAppSpecialize` 阶段被激活。
  - **核心操作**:
    1.  获取当前应用的包名。
    2.  通过 Zygisk API `connectCompanion()` 与后台的 Companion 进程建立一个 Unix Domain Socket 连接。
    3.  将包名发送给 Companion 进程作为请求。
    4.  阻塞式地等待并接收 Companion 进程返回的、已序列化的伪装属性数据。
    5.  如果接收到有效数据，则初始化 `HookManager` 并应用双层 Hook。
    6.  调用 `api->setOption(FORCE_DENYLIST_UNMOUNT)` 增强隐蔽性。

- **`companion_handler` (Companion 进程)**:
  - **职责**: 作为一个拥有 Root 权限的独立后台服务，它负责所有**重型**和**耗时**的工作。
  - **生命周期**: 由 `connectCompanion()` 请求触发，一对一响应，完成后即退出。
  - **核心操作**:
    1.  接收 Zygote 模块发送的应用包名。
    2.  初始化 `ConfigManager` 的单例。
    3.  调用 `ConfigManager::loadOrReloadConfig()`，此方法会检查配置文件的时间戳，仅在文件被修改后才重新读取和解析。
    4.  根据应用包名查询匹配的伪装规则。
    5.  将查询到的属性（如 `brand`, `model` 及其对应值）序列化为一个特殊的字符串（键和值均以 `\0` 结尾）。
    6.  将序列化后的字符串通过 Socket 发回给 Zygote 模块。

- **`ConfigManager`**:
  - **职责**: 配置文件的管理中心，负责所有与 `config.json` 相关的逻辑。
  - **核心操作**:
    1.  **文件读取与解析**: 使用 `simdjson` 高效解析 JSON 文件。
    2.  **缓存**: 将解析后的配置缓存在内存中（一个 `unordered_map`），并记录文件最后修改时间。
    3.  **按需加载**: 只有当检测到 `config.json` 文件被修改时，才会执行实际的读盘和解析操作，否则直接使用内存缓存。
    4.  **查询**: 提供接口供 `companion_handler` 查询特定包名的伪装属性。

- **`HookManager`**:
  - **职责**: 所有 Hook 逻辑的封装。
  - **核心操作**:
    1.  **原生 Hook (PLT)**: 使用 Zygisk API `pltHookRegister` 来 Hook `libc.so` 中的 `__system_property_get` 函数。
    2.  **JNI Hook**: 直接通过 JNI `SetStaticObjectField` 接口修改 `android.os.Build` 类的静态字段。
    3.  **上下文传递**: 使用 `thread_local` 变量安全地将特定应用的伪装属性传递给静态的 Hook 回调函数。
    4.  **缓存优化**: 内部缓存了 `libc.so` 的设备号/inode 号以及 JNI 的 `jclass`/`jfieldID`，避免了每次 Hook 时的重复查找，提升了性能。

## 3. 关键技术决策

### 3.1. 为何选择 Companion 进程架构？
这是确保**性能**和**稳定性**的基石。在 `preAppSpecialize` 阶段执行任何文件 I/O 操作都存在阻塞应用启动的风险。将这些操作完全移至后台的 Companion 进程，使得 Zygote 模块的执行时间缩短到仅剩一次 IPC 的开销（通常在微秒级别），从根本上杜绝了应用启动缓慢或卡死的问题。

### 3.2. 为何保留双层 Hook？
- **JNI Hook** 对上层 Java 世界的应用有效，直接修改 `Build` 类，效果最直接。
- **Native Hook** 针对的是底层通过 `__system_property_get` 获取属性的应用或库（如游戏引擎、加固壳等），覆盖了 JNI Hook 无法触及的场景。
- 两者结合，形成了**互补**，最大化了伪装的覆盖面和一致性。

### 3.3. 为何使用 `simdjson`?
`simdjson` 是目前性能最高的开源 JSON 解析库之一。它利用 SIMD 指令集（如 NEON）来加速解析过程。考虑到 Companion 进程可能会被频繁调用，选择最快的解析器有助于降低系统资源的占用和每次请求的响应时间。

### 3.4. 隐蔽性策略
- **`FORCE_DENYLIST_UNMOUNT`**: 这是 Zygisk 提供的核心隐蔽性功能，通过在目标进程中卸载 Magisk 相关的文件挂载点，可以有效规避绝大多数基于文件路径的检测。
- **无日志 Hook**: Hook 回调函数中不包含任何 `log` 或文件写入操作，减少了运行时特征。

## 4. 编译与依赖
- **语言标准**: C++20
- **构建系统**: Gradle + CMake
- **核心依赖**:
  - `simdjson`: 作为子模块集成在 `module/src/main/cpp/external/` 目录下。
- **NDK 版本**: 推荐使用 r25c 或更高版本以获得最佳的 C++20 支持。
