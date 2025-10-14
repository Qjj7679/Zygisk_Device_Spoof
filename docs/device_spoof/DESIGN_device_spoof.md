# DESIGN - Android 设备信息伪造模块 (最终架构)

**目标:** 定义基于 Companion 进程的高性能、高健壮性的最终架构，明确核心组件、通信协议和关键实现细节。

## 1. 整体架构图

```mermaid
graph TD
    subgraph "Zygisk Framework"
        A[Zygote Process]
    end

    subgraph "Companion Process (Singleton, Root)"
        C[companion_handler]
        C -- Ensures Uniqueness via --> G((companion.lock))
        C -- Manages --> D{Config Cache<br/>(in-memory map)}
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

## 2. 核心组件设计

### 2.1 Companion 进程 (`companion_handler` & `companion_init`)
作为配置服务中心，其核心职责通过 `std::call_once` 调用 `companion_init` 一次性初始化，后续 `companion_handler` 仅处理请求。

- **进程唯一性**:
  - 在 `companion_init` 中，通过对 `/data/adb/modules/.../companion.lock` 文件进行 `flock` 操作，确保任何时候只有一个 Companion 进程实例在运行。若无法获取锁，进程将自动退出。

- **启动与热重载**:
  - **首次加载**: 在 `companion_init` 中，首次加载 `config.json` 到内存中的 `std::unordered_map`。
  - **健壮的热重载**: 创建一个独立的监控线程 (`monitor_config_thread`)，通过 `inotify` **监控配置文件所在的目录**。该线程能可靠地处理各种编辑器（包括采用“原子保存”策略的编辑器）对 `config.json` 的修改（`IN_CLOSE_WRITE` 或 `IN_MOVED_TO` 事件），触发配置的重新加载。

- **通信逻辑**:
  - `companion_handler` 负责监听并处理来自 `SpoofModule` 的 IPC 请求。
  - 它在内存中查询包名，并将结果通过 `DeviceInfoIPC` 结构体返回。

- **线程安全**:
  - 使用 `std::shared_mutex` 保证配置数据 `package_map` 在并发读（多应用查询）和写（热重载）操作下的安全性。

### 2.2 Zygisk 模块 (`SpoofModule`)
作为纯粹的 IPC 客户端，其职责大幅简化：
- **`onLoad()`**: 初始化 JNI 字段缓存。
- **`preAppSpecialize()`**: 连接到 Companion 进程，发送当前包名，接收返回的 `DeviceInfoIPC` 结构体。根据 `match_found` 标志位决定是否执行伪造，最后卸载自身以保持隐蔽。

## 3. IPC 通信协议
- **数据结构 (`DeviceInfoIPC`)**: 使用包含 `bool match_found` 标志位的固定大小结构体进行数据传输。
- **流程**:
    1.  **Client -> Server**: 发送 C-style 字符串（包名）。
    2.  **Server -> Client**: 返回 `DeviceInfoIPC` 结构体。
        -   若找到配置，`match_found` 为 `true`，并填充设备信息。
        -   若未找到，`match_found` 为 `false`，其他字段内容无意义。
- **决策**: 客户端根据 `match_found` 的值来决定是否进行伪造，逻辑清晰且健壮。

---
*此文档已于最终修复后更新，反映了项目最终的技术架构，涵盖了性能、进程唯一性及热重载的所有设计细节。*


