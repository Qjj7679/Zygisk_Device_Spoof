# 阶段2: Architect (架构阶段) - 项目优化设计

**目标:** 定义清晰、可执行的架构设计方案，用于指导后续的代码清理和版本控制规范化。

## 1. 系统设计

本次优化的核心是项目结构的“净化”，而非功能变更。因此，设计主要围绕文件和目录的变动，以及版本控制策略的建立。

### 1.1 文件清理计划

根据 `CONSENSUS` 文档，以下文件和目录将被移除，以消除冗余代码和未使用的依赖：

- **待删除文件**:
    1. `module/src/main/cpp/example.cpp`
    2. `module/src/main/cpp/external/CMakeLists.txt`
- **待删除目录**:
    1. `module/src/main/cpp/external/libcxx/`

### 1.2 版本控制规范 (`.gitignore`)

为了保持仓库的整洁，并避免提交不必要的文件，将创建 `.gitignore` 文件，内容如下。该配置涵盖了 Android Studio/IntelliJ、Gradle、CMake 的构建产物，以及本地配置文件和操作系统生成的文件。

```gitignore
# IDE
.idea/
.vscode/
*.iml
*.ipr
*.iws

# Android/Gradle
.gradle/
build/
app/build/
module/build/
local.properties
*~
.DS_Store
*.apk
*.ap_
*.aab
*.aar
*.jar
*.zip

# CMake
.cxx/
module/.cxx/

# User Specific
.cursor/

# OS
.DS_Store
Thumbs.db
```

## 2. 设计原则

- **最小化变更**: 只执行 `CONSENSUS` 中明确同意的操作，不引入额外范围。
- **职责单一**: 本次优化只关注项目结构清理和版本控制，不涉及业务逻辑修改。
- **可验证性**: 所有操作都应是可验证的，例如，文件被删除后，项目依然可以成功编译。

## 3. 下一步

进入 **阶段3: Atomize (原子化阶段)**，将上述设计方案拆解为一系列具体的、可独立执行的原子任务，并记录在 `TASK_project_review.md` 文件中。
