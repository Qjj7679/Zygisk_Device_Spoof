# Zygisk Device Spoof

一个经过全面重构的 Zygisk 模块，旨在以一种现代化、稳定且高效的方式为特定 Android 应用程序动态修改设备属性。

## ✨ 核心特性

- **高度专注**: 模块经过精简，仅专注于伪装 `ro.product.brand` 和 `ro.product.model` 两个核心设备标识属性。
- **现代化 C++20 架构**: 整个项目代码库使用 C++20 标准重写，利用现代 C++ 特性提升代码的可读性、安全性和性能。
- **动态配置加载**: **无需重启手机**！当您修改配置文件后，只需重启目标应用，新的伪装配置即可立即生效。
- **简化的配置格式**: 配置文件使用直观的简称（`brand`, `model`）代替完整的属性名，使配置过程更简单、更不易出错。
- **健壮的双层 Hook 机制**:
  - **JNI 层**: 直接修改 `android.os.Build` 类的静态字段 (`BRAND`, `MODEL`)。
  - **原生层 (PLT Hook)**: 拦截底层的 `__system_property_get` 系统调用。
  - 双层机制确保了极高的伪装成功率和一致性。
- **性能优先**:
  - **Zygisk Companion 进程架构**: 所有耗时的文件 I/O 和 JSON 解析操作均在一个独立的后台进程中完成，完全不阻塞应用启动，从根本上解决了卡顿问题。
  - **`simdjson` 解析器**: 使用业界领先的高性能 JSON 解析库 `simdjson`，确保配置文件加载过程极速完成。
- **高稳定性与隐蔽性**:
  - **安全的初始化流程**: 严格遵守 Zygisk 开发规范，在 `onLoad` 阶段不做任何敏感操作，杜绝了影响系统启动的可能性。
  - **无日志 Hook**: 在高性能的 Hook 回调函数中严格禁止任何日志输出，避免了潜在的应用卡死风险和特征检测。
  - **强制卸载 (Denylist Unmount)**: 自动为目标进程强制卸载 Magisk 和模块相关的文件挂载点，有效对抗基于文件路径的检测。

## 🔧 如何使用

### 1. 配置文件

- **路径**: `/data/adb/modules/zygisk_device_spoof/config/config.json`
  *(模块安装后首次启动应用时会自动创建此文件)*

- **格式**:
  ```json
  {
    "apps": [
      {
        "package": "com.example.targetapp",
        "properties": {
          "brand": "Google",
          "model": "Pixel 7"
        }
      },
      {
        "package": "com.example.anotherapp",
        "properties": {
          "brand": "Samsung",
          "model": "Galaxy S23"
        }
      }
    ]
  }
  ```
- **字段说明**:
  - `package`: (字符串) 目标应用的包名。
  - `properties`: (对象) 一个包含伪装属性的 JSON 对象：
    - `"brand"`: (字符串) 您想要伪装的品牌名称。
    - `"model"`: (字符串) 您想要伪装的型号名称。

### 2. 应用配置

1.  将模块刷入 Magisk 并重启手机。
2.  编辑 `config.json` 文件以定义您的伪装规则。
3.  **强行停止并重启目标应用**，新的配置即刻生效。

## 🛠️ 如何编译

1.  确保您已配置好 Android NDK (推荐版本 r25c 或更高) 开发环境。
2.  在项目根目录运行 Gradle 任务:
    - **编译 Debug 版本**:
      ```bash
      ./gradlew :module:zipDebug
      ```
    - **编译 Release 版本**:
      ```bash
      ./gradlew :module:zipRelease
      ```
3.  编译好的 Magisk 模块 Zip 包将位于 `module/release/` 目录下。

