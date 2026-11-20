# 要求 CMake 的最低版本
# 配置这个项目时，要求使用 CMake 3.21 或更新版本
cmake_minimum_required(VERSION 3.21)

# 定义项目名称和使用的语言
# project(...)	定义项目
# Huan 	项目名称
# LANGUAGES	指定项目使用哪些编程语言
# CXX	表示 C++
# 这里定义了项目名，CMake 会自动生成一个叫 PROJECT_NAME 的内置变量
project(Huan LANGUAGES CXX)



# 设置 C++ 标准
# CMAKE_CXX_STANDARD	设置 C++ 标准
# CMAKE_CXX_STANDARD_REQUIRED	表示 C++ 标准是必须的
# 20	表示使用 C++20 标准
# ON 表示 C++ 标准是必须的
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找 Qt6 库
# find_package(...)	查找库
# Qt6	表示查找 Qt6 库
# REQUIRED	表示必须找到 Qt6 库
# COMPONENTS	表示需要哪些组件
# Widgets	表示需要 Qt Widgets 组件
# Sql	表示需要 Qt Sql 组件
find_package(Qt6 REQUIRED COMPONENTS Widgets Sql)

# 设置 Qt6 的配置
# qt_standard_project_setup()	设置 Qt6 的配置 启用 Qt 项目常用的一组 CMake 设置。
qt_standard_project_setup()

# 定义要生成的程序，以及它包含哪些文件
# qt_add_executable(...)	定义要生成的程序
# ${PROJECT_NAME}	程序名称
# main.cpp	程序入口文件
# src/HOOK/HookManager.cpp	程序源文件
# src/HOOK/HookManager.h	程序头文件
qt_add_executable(${PROJECT_NAME} 
    WIN32
    src/main.cpp
    src/app.rc
    src/HOOK/HookManager.cpp
    src/HOOK/HookManager.h
    src/ENGINE/ActionEngine.cpp
    src/ENGINE/ActionEngine.h
    src/UI/WheelWindow.cpp
    src/UI/WheelWindow.h
    src/UTILS/Win32Utils.cpp
    src/UTILS/Win32Utils.h
    src/ENGINE/VariableResolver.cpp
    src/ENGINE/VariableResolver.h
    src/ENGINE/ScriptRunner.cpp
    src/ENGINE/ScriptRunner.h
    src/DATABASE/ActionRepository.cpp
    src/DATABASE/ActionRepository.h
    src/DATABASE/AppConfig.cpp
    src/DATABASE/AppConfig.h
    src/UI/MainWindow.cpp
    src/UI/MainWindow.h
    src/UI/ActionPage.cpp
    src/UI/ActionPage.h
)

# 离线单元测试（不发送真实按键，不会干扰桌面）
qt_add_executable(Huan_tests
    tests/test_key_parser.cpp
    src/UTILS/Win32Utils.cpp
    src/UTILS/Win32Utils.h
    src/ENGINE/VariableResolver.cpp
    src/ENGINE/VariableResolver.h
)
target_include_directories(Huan_tests PRIVATE src/ENGINE src/UTILS)
target_link_libraries(Huan_tests PRIVATE Qt6::Core Qt6::Gui user32)

# 脚本执行器集成测试（真实启动 Python / cmd / PowerShell，不碰鼠标键盘）
qt_add_executable(Huan_script_tests
    tests/test_script_runner.cpp
    src/ENGINE/ScriptRunner.cpp
    src/ENGINE/ScriptRunner.h
)
target_include_directories(Huan_script_tests PRIVATE src/ENGINE)
target_link_libraries(Huan_script_tests PRIVATE Qt6::Core)

# 数据库 / 设置持久化测试
qt_add_executable(Huan_db_tests
    tests/test_database.cpp
    src/DATABASE/ActionRepository.cpp
    src/DATABASE/ActionRepository.h
    src/DATABASE/AppConfig.cpp
    src/DATABASE/AppConfig.h
)
target_include_directories(Huan_db_tests PRIVATE src/DATABASE)
target_link_libraries(Huan_db_tests PRIVATE Qt6::Core Qt6::Sql)

# 手动诊断工具：真实输入注入。需要测试窗口拿到前台焦点，否则自动跳过。
# 用 EXCLUDE_FROM_ALL 避免默认构建/CI 受桌面环境影响。
add_executable(Huan_input_tests EXCLUDE_FROM_ALL
    tests/test_input_sender.cpp
    src/UTILS/Win32Utils.cpp
    src/UTILS/Win32Utils.h
)
target_include_directories(Huan_input_tests PRIVATE src/UTILS)
target_link_libraries(Huan_input_tests PRIVATE Qt6::Widgets user32)

# exe 图标：app.rc 里的 ICON 文件名按 /I 搜索。ico 现在在 resources/Logo and Main Icon/ 下，
# /I 必须指向这个子目录（含空格，靠转义引号传参），而不是 resources 根目录。
# 清单文件复制到构建目录，让 rc.exe 用相对名就能打开（它的工作目录是构建目录）。
configure_file(src/app.manifest "${CMAKE_CURRENT_BINARY_DIR}/app.manifest" COPYONLY)

set_source_files_properties(src/app.rc PROPERTIES
    COMPILE_FLAGS "/I\"${CMAKE_CURRENT_SOURCE_DIR}/resources/Logo and Main Icon\" /I\"${CMAKE_CURRENT_SOURCE_DIR}/src\"")

# 告诉编译器到哪里找头文件
# target_include_directories(...)	告诉编译器到哪里找头文件
# ${PROJECT_NAME}	程序名称
# PRIVATE	表示这些头文件只在程序内部使用
# src/HOOK	表示头文件在 src/HOOK 目录下
target_include_directories(${PROJECT_NAME} PRIVATE src src/HOOK src/ENGINE src/UI src/UTILS src/DATABASE)

# 给程序链接 Qt 库
# target_link_libraries(...)	给程序链接 Qt 库
# ${PROJECT_NAME}	程序名称
# PRIVATE	表示这些库只在程序内部使用
# Qt6::Widgets	表示链接 Qt Widgets 库
# Qt6::Sql	表示链接 Qt Sql 库
target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Widgets Qt6::Sql user32 shell32)

# 提权改由 src/app.manifest 实现（经 src/app.rc 以 RT_MANIFEST 资源嵌入 exe）。
# 原来的 /MANIFESTUAC 在这里不生效：本项目 vcxproj 里 GenerateManifest 为 false，
# link.exe 不生成清单，那个参数被直接忽略 —— exe 里连一个清单都没有，自然没有盾牌。

# 告诉 C++ 编译器：定义一个叫 APP_NAME 的宏，值是 "${PROJECT_NAME}"
#target_compile_definitions(${PROJECT_NAME} PRIVATE APP_NAME="${PROJECT_NAME}")
# 改成在 Constants.h 中统一管理


get_target_property(QT_BIN_DIR Qt6::qmake IMPORTED_LOCATION)        # 获取 qmake 工具的路径
get_filename_component(QT_BIN_DIR "${QT_BIN_DIR}" DIRECTORY)        # 获取 qmake 工具所在的目录
find_program(WINDEPLOYQT_EXE windeployqt HINTS "${QT_BIN_DIR}")     # 查找部署工具 windeployqt 

# 图标资源独立于 windeployqt：代码里所有图标都按 exe 同级 /resources/ 取，
# 缺了这一步，找不到 windeployqt 的机器上整个界面会退化成纯文字。
# copy_directory 是递归的，三个子目录会一起带过去。
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/resources"
    COMMENT "Copying resources (icons) next to the executable..."
)

# 如果找到了 windeployqt，再额外部署 Qt 运行时依赖
if(WINDEPLOYQT_EXE)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${WINDEPLOYQT_EXE}" "$<TARGET_FILE:${PROJECT_NAME}>"
        COMMENT "Running windeployqt to deploy Qt dependencies..."
    )
endif() # 结束前面的条件块。