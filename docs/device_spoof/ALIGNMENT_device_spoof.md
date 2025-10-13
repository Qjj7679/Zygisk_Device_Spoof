# ALIGNMENT - Android 设备信息伪装模块

## 1. 项目上下文分析

### 1.1 现有项目结构
- **项目类型**: Zygisk 模块模板项目
- **技术栈**: 
  - C++20 (NDK 开发)
  - Gradle/Kotlin DSL 构建系统
  - CMake 构建配置
  - Zygisk API (提供 Hook 机制)
  - 自定义 libc++ (无异常、无 RTTI)
- **架构模式**: 
  - 基于 Zygisk ModuleBase 的模块化架构
  - 使用 JNI 与 Java 层交互
  - 支持 Companion 进程通信机制
- **依赖关系**:
  - 已集成自定义 libc++ 静态库
  - 支持 arm64-v8a, armeabi-v7a, x86, x86_64 架构
  - Android API 26+ (Android 8.0+)
- **现有代码模式**:
  - 使用 `REGISTER_ZYGISK_MODULE` 宏注册模块
  - 实现 `onLoad`, `preAppSpecialize`, `postAppSpecialize` 等生命周期钩子
  - 使用 `DLCLOSE_MODULE_LIBRARY` 实现隐身模式
- **构建系统**:
  - 模块 ID 在 `build.gradle.kts` 中定义为 `sample`
  - 自动生成 Magisk/KernelSU 模块 ZIP 包
  - 支持 Debug/Release 构建变体

### 1.2 业务域理解
- **核心业务**: Android 设备信息伪装，绕过应用的设备检测机制
- **目标场景**: 游戏反作弊、银行 App 设备检测、应用兼容性测试
- **技术原理**: 在应用进程启动时，通过 Zygisk Hook 修改 `android.os.Build` 类的静态字段

## 2. 原始需求

基于 Zygisk 模块模板开发 Android 设备信息伪装模块，核心功能包括：
1. 动态修改 `android.os.Build` 类静态字段（MODEL、BRAND、DEVICE、MANUFACTURER、PRODUCT）
2. 通过包名匹配针对性应用伪装
3. JSON 配置文件支持，实现热重载
4. 隐身模式，完成伪装后卸载模块
5. 线程安全保障
6. 可开关的调试模式

## 3. 边界确认

### 3.1 功能边界
**包含范围**:
- ✅ 修改 `android.os.Build` 类的 5 个核心字段（MODEL、BRAND、DEVICE、MANUFACTURER、PRODUCT）
- ✅ 基于包名的精确匹配（支持多应用配置）
- ✅ JSON 配置文件解析和加载
- ✅ 配置文件热重载（通过 service.sh 守护进程监控）
- ✅ 线程安全的配置加载和伪装操作
- ✅ 隐身模式（DLCLOSE_MODULE_LIBRARY）
- ✅ 调试模式开关（Debug/Release 构建）
- ✅ 在 `preAppSpecialize` 和 `postAppSpecialize` 两个钩子中执行伪装

**不包含范围**:
- ❌ 修改 `android.os.Build` 之外的系统属性（如 `SystemProperties`）
- ❌ 修改硬件序列号、IMEI、MAC 地址等硬件标识
- ❌ 提供 GUI 配置界面
- ❌ 配置文件加密
- ❌ 网络远程配置
- ❌ 包名模糊匹配或正则表达式匹配
- ❌ 动态注入配置（运行时修改）

### 3.2 技术边界
- **最低 Android 版本**: API 26 (Android 8.0)
- **最高 Android 版本**: API 36 (Android 15) - 按现有项目配置
- **支持架构**: arm64-v8a, armeabi-v7a, x86, x86_64
- **配置文件路径**: `/data/adb/modules/<模块ID>/config/config.json`
- **缓存文件路径**: `/data/adb/modules/<模块ID>/cache/config_cached.json`

### 3.3 质量边界
- **性能要求**: 配置加载时间 < 100ms，伪装操作时间 < 10ms
- **稳定性要求**: 不得导致目标应用崩溃或系统不稳定
- **兼容性要求**: 兼容 Magisk 和 KernelSU
- **安全性要求**: 配置文件权限控制（0644），防止未授权修改

## 4. 需求理解

### 4.1 对现有项目的理解
1. **模块 ID 需要修改**: 当前为 `sample`，需改为 `zygisk_device_spoof`
2. **模块信息需要更新**: 
   - 名称: `Zygisk Device Spoof`
   - 作者: `Qjj0204`
   - 描述: `A module used to spoof device info.`
3. **需要移除 Companion 进程**: 当前示例使用了 Companion 机制，本模块不需要
4. **需要集成 RapidJSON**: 当前项目未包含 JSON 解析库，需要添加
5. **需要实现配置管理模块**: 包括文件读取、JSON 解析、缓存机制、热重载检测
6. **需要实现 JNI 伪装逻辑**: 查找 `android.os.Build` 类，修改静态字段
7. **需要实现 service.sh 守护进程**: 监控配置文件变更，同步到缓存

### 4.2 架构对齐
- **复用现有 Zygisk 模块结构**: 继承 `zygisk::ModuleBase`
- **复用现有构建系统**: 使用 CMake + Gradle
- **复用现有 libc++**: 使用项目自带的精简 libc++
- **复用现有模板文件**: `module.prop`, `service.sh`, `customize.sh` 等

### 4.3 关键技术点
1. **JNI 字段修改**: 使用 `SetStaticObjectField` 修改 `final` 字段（JNI 可绕过 Java 访问控制）
2. **线程安全**: 使用 `std::mutex` 保护配置加载，`std::once_flag` 确保类加载唯一性
3. **配置热重载**: 通过 `stat()` 检测 mtime 变化，优先读取缓存文件
4. **隐身模式**: 在伪装完成后立即调用 `setOption(DLCLOSE_MODULE_LIBRARY)`
5. **异常处理**: 所有 JNI 操作需检查异常并清除，防止崩溃

## 5. 疑问澄清

### 5.1 关键决策点（需要用户确认）

#### Q1: RapidJSON 集成方式
**问题**: 需求文档提到使用 RapidJSON 解析 JSON，但有以下选择：
- **选项 A**: 使用 RapidJSON header-only 模式（推荐）
  - 优点: 无需编译，集成简单，体积小
  - 缺点: 编译时间略长
- **选项 B**: 使用其他轻量级 JSON 库（如 nlohmann/json）
  - 优点: 更现代的 C++ API，更易用
  - 缺点: 与需求文档不符

**建议决策**: 使用 RapidJSON header-only 模式（符合需求文档）

#### Q2: 配置文件监控机制
**问题**: service.sh 监控配置文件变更的实现方式：
- **选项 A**: 优先使用 `inotifywait`（如果可用），回退到轮询
  - 优点: 实时性好，资源占用低
  - 缺点: 部分设备可能不支持 `inotifywait`
- **选项 B**: 仅使用轮询机制（每 2 秒检查一次）
  - 优点: 兼容性好
  - 缺点: 实时性差，资源占用略高

**建议决策**: 使用选项 A（需求文档已提供示例代码）

#### Q3: 调试日志控制
**问题**: 调试日志的开关方式：
- **选项 A**: 通过构建变体控制（Debug 开启，Release 关闭）
  - 优点: 编译时优化，Release 版本无日志开销
  - 缺点: 无法在 Release 版本中开启日志
- **选项 B**: 通过配置文件控制（JSON 中添加 `debug_mode` 字段）
  - 优点: 灵活，可动态开关
  - 缺点: Release 版本仍有日志代码

**建议决策**: 使用选项 A（需求文档提到"默认关闭"，符合构建变体控制）

#### Q4: 配置文件缺失处理
**问题**: 如果配置文件不存在或解析失败，模块行为：
- **选项 A**: 记录错误日志，卸载模块（不影响其他应用）
  - 优点: 安全，不会误伤无关应用
  - 缺点: 配置错误时模块无效
- **选项 B**: 使用默认配置（空配置，不伪装任何应用）
  - 优点: 容错性好
  - 缺点: 可能掩盖配置错误

**建议决策**: 使用选项 A（更安全）

#### Q5: 包名匹配失败处理
**问题**: 如果当前应用包名不在配置中，模块行为：
- **选项 A**: 立即卸载模块（需求文档描述的行为）
  - 优点: 减少内存占用，降低检测风险
  - 缺点: 每次启动非目标应用都会加载/卸载模块
- **选项 B**: 保持模块加载，但不执行伪装
  - 优点: 避免频繁加载/卸载
  - 缺点: 增加被检测风险

**建议决策**: 使用选项 A（符合需求文档的隐身模式要求）

#### Q6: 伪装时机
**问题**: 需求文档提到在 `preAppSpecialize` 和 `postAppSpecialize` 都执行伪装，但：
- **疑问**: 是否需要在两个钩子中都执行？
- **分析**: 
  - `preAppSpecialize`: 在应用进程 fork 后、专属化前执行，此时 JNI 环境可用
  - `postAppSpecialize`: 在应用进程专属化后执行，确保所有初始化完成
  - 理论上在 `preAppSpecialize` 中修改即可生效

**建议决策**: 主要在 `preAppSpecialize` 中执行伪装，在 `postAppSpecialize` 中再次验证并补充（防止某些情况下字段被重置）

#### Q7: 模块 ID 和命名空间
**问题**: 需要修改的标识符：
- 模块 ID: `sample` → `zygisk_device_spoof`
- 命名空间: `io.github.a13e300.zygisk.module.sample` → ?
- 模块名称: `Zygisk Module Sample` → `Zygisk Device Spoof`
- SO 库名: `sample.so` → `zygisk_device_spoof.so`

**建议决策**: 
- 模块 ID: `zygisk_device_spoof`
- 命名空间: `io.github.qjj0204.zygisk.device.spoof`
- 模块名称: `Zygisk Device Spoof`
- SO 库名: `zygisk_device_spoof.so`

**⚠️ 需要用户确认命名空间格式**

### 5.2 技术实现疑问

#### Q8: RapidJSON 版本选择
**问题**: 使用哪个版本的 RapidJSON？
- **建议**: 使用最新稳定版 v1.1.0（2016 年发布，成熟稳定）
- **获取方式**: 从 GitHub 下载 header-only 文件到 `module/src/main/cpp/external/rapidjson/`

#### Q9: 配置文件示例
**问题**: 是否需要在模块中内置默认配置文件？
- **建议**: 在 `module/template/config/config.json` 中提供示例配置，安装时复制到模块目录
- **内容**: 包含一个示例应用配置（注释说明）

#### Q10: 错误处理策略
**问题**: JNI 操作失败时的处理方式？
- **建议**: 
  - 检查 JNI 异常（`ExceptionCheck()`）
  - 清除异常（`ExceptionClear()`）
  - 记录错误日志（Debug 模式）
  - 继续执行或卸载模块（取决于错误严重性）

### 5.3 基于现有项目的决策（自动决策）

以下问题基于现有项目结构和行业最佳实践自动决策：

1. **构建系统**: 继续使用 CMake（已有配置）
2. **C++ 标准**: 继续使用 C++20（已配置）
3. **编译选项**: 继续使用 `-fno-exceptions -fno-rtti`（已配置）
4. **libc++**: 继续使用项目自带的精简版本（已集成）
5. **日志标签**: 使用 `ZygiskDeviceSpoof`（符合需求文档）
6. **文件权限**: 
   - 配置目录: 0755
   - 配置文件: 0644
   - 缓存文件: 0644
7. **配置文件格式**: 严格按照需求文档的 JSON 格式
8. **字段名称**: 使用小写（`brand`, `model`, `device`, `manufacturer`, `product`）

## 6. 任务范围总结

### 6.1 需要修改的文件
1. **C++ 源码**:
   - `module/src/main/cpp/example.cpp` → 重写为 `device_spoof.cpp`
   - `module/src/main/cpp/CMakeLists.txt` → 更新源文件和依赖
2. **构建配置**:
   - `build.gradle.kts` → 更新模块 ID、名称、命名空间
   - `module/build.gradle.kts` → 无需修改（继承根配置）
3. **模板文件**:
   - `module/template/module.prop` → 更新模块信息
   - `module/template/service.sh` → 实现配置监控逻辑
   - `module/template/config/config.json` → 创建示例配置（新增）
4. **外部依赖**:
   - 添加 RapidJSON 到 `module/src/main/cpp/external/rapidjson/`

### 6.2 需要创建的文件
1. `module/src/main/cpp/device_spoof.cpp` - 主模块实现
2. `module/src/main/cpp/config_manager.h` - 配置管理头文件（可选，可内联到主文件）
3. `module/template/config/config.json` - 示例配置文件
4. `module/src/main/cpp/external/rapidjson/` - RapidJSON 库（header-only）

### 6.3 需要删除的文件
1. `module/src/main/cpp/example.cpp` - 示例代码（被替换）

## 7. 验收标准

### 7.1 功能验收
- [ ] 模块成功编译并生成 ZIP 包
- [ ] 模块可在 Magisk/KernelSU 中安装
- [ ] 配置文件正确解析（包含多个应用配置）
- [ ] 目标应用启动时，`android.os.Build` 字段被正确修改
- [ ] 非目标应用不受影响
- [ ] 配置文件修改后，新启动的应用使用新配置
- [ ] Debug 版本输出日志，Release 版本无日志
- [ ] 模块在伪装后成功卸载（隐身模式）

### 7.2 性能验收
- [ ] 配置文件加载时间 < 100ms
- [ ] 伪装操作时间 < 10ms
- [ ] 不影响应用启动速度（增加时间 < 50ms）

### 7.3 稳定性验收
- [ ] 配置文件缺失时模块正常卸载
- [ ] 配置文件格式错误时模块正常卸载
- [ ] JNI 操作失败时不导致应用崩溃
- [ ] 目标应用正常运行，无崩溃

### 7.4 兼容性验收
- [ ] 支持 Android 8.0 - Android 15
- [ ] 支持 arm64-v8a, armeabi-v7a, x86, x86_64
- [ ] 兼容 Magisk 和 KernelSU

## 8. 风险评估

### 8.1 技术风险
1. **JNI 字段修改失败**: 某些 ROM 可能对 `android.os.Build` 有额外保护
   - **缓解措施**: 添加异常处理，记录失败日志
2. **配置文件权限问题**: SELinux 可能阻止文件访问
   - **缓解措施**: 使用标准路径 `/data/adb/modules/`，通常有正确的 SELinux 上下文
3. **RapidJSON 编译问题**: 可能与自定义 libc++ 冲突
   - **缓解措施**: 使用 header-only 模式，确保包含路径正确

### 8.2 兼容性风险
1. **高版本 Android 兼容性**: Android 14+ 可能有新的安全限制
   - **缓解措施**: 在多个 Android 版本上测试
2. **自定义 ROM 兼容性**: 某些 ROM 可能修改了 `android.os.Build` 实现
   - **缓解措施**: 添加兼容性检查，失败时优雅降级

### 8.3 安全风险
1. **配置文件被恶意修改**: 可能导致应用行为异常
   - **缓解措施**: 设置正确的文件权限（0644），仅 root 可写
2. **模块被检测**: 某些应用可能检测 Zygisk 模块
   - **缓解措施**: 使用隐身模式，伪装后立即卸载

## 9. 下一步行动

等待用户确认以下关键决策点：
1. ✅ **Q1**: RapidJSON 集成方式 → 建议使用 header-only 模式
2. ✅ **Q2**: 配置监控机制 → 建议使用 inotifywait + 轮询回退
3. ✅ **Q3**: 调试日志控制 → 建议通过构建变体控制
4. ✅ **Q4**: 配置文件缺失处理 → 建议记录错误并卸载模块
5. ✅ **Q5**: 包名匹配失败处理 → 建议立即卸载模块
6. ✅ **Q6**: 伪装时机 → 建议主要在 preAppSpecialize 执行
7. ⚠️ **Q7**: 命名空间格式 → **需要用户确认**

**用户需要明确回答的问题**:
- **命名空间**: 是否使用 `io.github.qjj0204.zygisk.device.spoof`？还是其他格式？
- **其他建议决策**: 是否同意上述所有建议决策（Q1-Q6）？

确认后将进入 **Architect 阶段**，生成详细的系统架构设计。


