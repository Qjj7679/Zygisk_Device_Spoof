# Zygisk Device Spoof

这是一个功能强大且高度优化的 Zygisk 模块，旨在为特定的 Android 应用程序动态修改（Spoof）`android.os.Build` 类中的设备信息字段。

## ✨ 核心特性

- **高性能架构**: 采用 **Zygisk Companion 守护进程**模型。所有配置的读取和解析都在一个独立的、以 root 权限运行的单例进程中完成，极大地降低了对应用启动性能的影响。
- **健壮的热重载**: 模块能够实时监控配置文件的变更。通过监控**配置文件所在目录**，即使是通过“原子保存”（删除->重命名）方式修改配置文件，热加载也能稳定生效，无需重启设备。
- **进程唯一性保证**: 通过**文件锁 (`flock`)** 机制，确保任何时候都只有一个 Companion 进程在运行，从根本上杜绝了因模块覆盖安装导致旧进程残留、数据陈旧的问题。
- **灵活的配置**:
  - 支持为任意数量的应用配置不同的设备信息。
  - 支持**部分伪造**：您可以只在配置文件中提供需要修改的字段（如 `model`, `brand`），未提供的字段将保持应用原始值不变。
  - 支持**空值忽略**：对于配置文件中值为空字符串 `""` 的字段，模块会自动忽略，不会进行修改。
- **隐蔽性**: 模块在对目标应用完成注入后，会从其进程空间中自动卸载，增加了隐蔽性。

## 🔧 如何使用

### 1. 配置文件

模块的所有行为都由一个 JSON 配置文件控制。

- **配置文件路径**: `/data/adb/modules/zygisk_device_spoof/config/config.json`
  *(配置文件和目录在安装模块时自动创建并使用默认配置)*

- **配置文件格式**:
  ```json
  {
    "apps": [
      {
        "package": "com.liuzh.deviceinfo",
        "brand": "Google",
        "model": "Pixel 7",
        "device": "panther",
        "manufacturer": "Google",
        "product": "panther"
      },
      {
        "package": "com.example.targetapp2",
        "brand": "Samsung",
        "model": "Galaxy S23"
      },
      {
        "package": "com.example.targetapp3",
        "brand": "OnePlus",
        "model": "12",
        "manufacturer": "",
        "product": ""
      }
    ]
  }
  ```

### 2. 生效方式
- 将模块刷入 Magisk 并重启。
- 在指定路径创建 `config.json` 文件并编辑您的规则。
- **无需再次重启**，Companion 进程会自动加载您的配置。
- 当您修改 `config.json` 并保存后，热加载功能会自动让新配置生效。

## 🛠️ 如何编译

1.  确保您已配置好 Android NDK 开发环境。
2.  在项目根目录运行 Gradle 任务:
    - 编译 Debug 版本的模块包:
      ```bash
      ./gradlew :module:zipDebug
      ```
    - 编译 Release 版本的模块包:
      ```bash
      ./gradlew :module:zipRelease
      ```
3.  编译好的 Magisk 模块 Zip 包将位于 `module/release/` 目录下。

---
*该项目基于 Zygisk Module Template 进行深度开发和优化。*
