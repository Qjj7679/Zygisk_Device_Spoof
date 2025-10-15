# Zygisk Device Spoof

这是一个功能强大且高度优化的 Zygisk 模块，旨在为特定的 Android 应用程序动态修改设备信息。它实现了一个**通用的、由配置驱动的属性伪装引擎**，允许您伪装任意系统属性。

## ✨ 核心特性

- **通用伪装引擎**: 不再局限于固定的设备字段，您可以通过配置文件伪装**任意**您需要的系统属性，例如 `ro.product.board` 或 `ro.build.version.security_patch`。
- **双层 Hook 机制**:
  - **JNI 层**: 动态修改 `android.os.Build` 类中的设备信息字段。
  - **原生层**: 通过 Zygisk 的 PLT Hook 技术，直接 Hook 底层 C 库的 `__system_property_get` 函数。
  - 这确保了无论应用通过哪种方式获取设备信息，都能得到伪装后的值，大大提高了伪装的成功率和一致性。
- **高性能架构**: 采用 **Zygisk Companion 守护进程**模型。所有配置的读取和解析都在一个独立的、以 root 权限运行的单例进程中完成，极大地降低了对应用启动性能的影响。
- **健壮的热重载**: 模块能够实时监控配置文件的变更。通过监控**配置文件所在目录**，即使是通过“原子保存”（删除->重命名）方式修改配置文件，热加载也能稳定生效，无需重启设备。
- **进程唯一性保证**: 通过**文件锁 (`flock`)** 机制，确保任何时候都只有一个 Companion 进程在运行，从根本上杜绝了因模块覆盖安装导致旧进程残留、数据陈旧的问题。
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
        "package": "com.example.targetapp",
        "properties": {
          "ro.product.brand": "Google",
          "ro.product.model": "Pixel 7",
          "ro.product.manufacturer": "Google",
          "ro.product.device": "cheetah",
          "ro.build.product": "cheetah"
        }
      },
      {
        "package": "com.example.targetapp2",
        "properties": {
          "ro.product.brand": "Samsung",
          "ro.product.model": "Galaxy S23",
          "ro.build.version.security_patch": "2023-10-05"
        }
      }
    ]
  }
  ```
- **配置说明**:
  - `package`: 目标应用的包名。
  - `properties`: 一个 JSON 对象，包含了所有您想要伪装的属性。
    - **键 (Key)**: 必须是您想要伪装的**标准 Android 系统属性名称** (例如 `ro.product.model`)。
    - **值 (Value)**: 您想要设置的伪装值。

### 2. 生效方式
- 将模块刷入 Magisk / Kernel SU / APatch 并重启手机。
- 在指定路径下找到 `config.json` 文件并编辑您的规则。
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
