cmake_minimum_required(VERSION 3.20)
project(weather_mcp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 输出统一落到 build/Release(或 build/Debug) 下，方便 CodeBuddy 配置绝对路径
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")

if(MSVC)
    # /utf-8: 源码里的中文字面量才不会被 MSVC 按本地代码页解析成乱码
    add_compile_options(/utf-8 /W3 /permissive-)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
else()
    add_compile_options(-Wall -Wextra)
endif()

add_executable(weather-mcp
    src/main.cpp
    src/transport_stdio.cpp
    src/server.cpp
    src/tools/weather.cpp
    src/http_winhttp.cpp
    src/console_win.cpp
)

target_include_directories(weather-mcp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)

# 客户端：拉起 server 子进程走一遍完整握手，把工具结果写到本地文件。
add_executable(mcp-client
    src/client_main.cpp
    src/client.cpp
    src/process_win.cpp
    src/console_win.cpp
)

target_include_directories(mcp-client PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)

if(WIN32)
    # shell32: CommandLineToArgvW（console_win.cpp 用来取 UTF-8 的命令行参数）
    target_link_libraries(mcp-client PRIVATE shell32.lib)
endif()

# Qt 界面客户端：自然语言 → DeepSeek → MCP 工具 → 自然语言，带对话窗口。
#
# Qt 是**可选依赖**：找不到就跳过这个 target，weather-mcp / mcp-client 照常构建。
# 找不到时把 Qt 的套件目录传进来再重新配置，例如：
#   cmake -S . -B build "-DCMAKE_PREFIX_PATH=C:/Qt/Qt5.12.12/5.12.12/msvc2015_64"
find_package(Qt5 COMPONENTS Widgets Concurrent QUIET)

if(Qt5_FOUND)
    add_executable(mcp-client-qt
        src/qt_main.cpp
        src/agent.cpp
        src/deepseek.cpp
        src/config.cpp
        src/client.cpp
        src/process_win.cpp
        src/http_winhttp.cpp
        src/console_win.cpp
    )

    target_include_directories(mcp-client-qt PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    )

    # Q_OBJECT 写在 .cpp 里，靠 AUTOMOC 生成 qt_main.moc（文件末尾 #include 引入）。
    set_target_properties(mcp-client-qt PROPERTIES AUTOMOC ON)

    if(WIN32)
        target_link_libraries(mcp-client-qt PRIVATE winhttp.lib shell32.lib)
    endif()
    target_link_libraries(mcp-client-qt PRIVATE Qt5::Widgets Qt5::Concurrent)

    # 把本地配置（含 API key）拷到 exe 同目录，否则从 build/ 里启动时找不到它。
    # 该文件已在 .gitignore 里，build/ 也是，所以不会被提交。
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deepseek.local.json")
        add_custom_command(TARGET mcp-client-qt POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/deepseek.local.json"
                    "$<TARGET_FILE_DIR:mcp-client-qt>/deepseek.local.json"
            COMMENT "拷贝 deepseek.local.json 到 exe 同目录"
            VERBATIM)
    endif()

    # 把 Qt 运行时拷到 exe 同目录，否则双击启动会报"找不到 Qt5Widgets.dll"。
    get_target_property(_qmake Qt5::qmake IMPORTED_LOCATION)
    if(_qmake)
        get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
        if(EXISTS "${_qt_bin}/windeployqt.exe")
            add_custom_command(TARGET mcp-client-qt POST_BUILD
                COMMAND "${_qt_bin}/windeployqt.exe" "$<TARGET_FILE:mcp-client-qt>"
                COMMENT "拷贝 Qt 运行时到 exe 同目录"
                VERBATIM)
        endif()
    endif()

    message(STATUS "Qt5 found: 构建 mcp-client-qt（Qt ${Qt5_VERSION}）")
else()
    message(STATUS "Qt5 not found: 跳过 mcp-client-qt（不影响 weather-mcp / mcp-client）")
endif()

if(WIN32)
    target_link_libraries(weather-mcp PRIVATE winhttp.lib shell32.lib)
endif()
