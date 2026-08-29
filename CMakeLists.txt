cmake_minimum_required(VERSION 3.16)
project(cpp-todo C CXX)

# AI 网关（ai/cpp/ai_gateway.cpp）需要 libcurl 转发到本地 AI 服务
find_package(CURL REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")

add_executable(todo
  src/main.cpp
  src/db.cpp
  src/lunar.cpp
  src/recurrence.cpp
  src/importer.cpp
  src/http.cpp
  src/api.cpp
  src/storage.cpp
  src/exporter.cpp
  src/backup.cpp
  ai/cpp/ai_gateway.cpp
  vendor/sqlite3.c
)

target_include_directories(todo PRIVATE src vendor ai/cpp)
target_compile_definitions(todo PRIVATE SQLITE_THREADSAFE=1 SQLITE_DEFAULT_FOREIGN_KEYS=1)

if(APPLE)
  # macOS 上显式注入 CommandLineTools SDK 头文件（部分环境 clang 默认路径探测失效）
  execute_process(COMMAND xcrun --show-sdk-path
                  OUTPUT_VARIABLE APPLE_SDK
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(APPLE_SDK)
    target_include_directories(todo PRIVATE
      ${APPLE_SDK}/usr/include/c++/v1
      ${APPLE_SDK}/usr/include
      ${APPLE_SDK}/usr/local/include
    )
  endif()
  target_link_libraries(todo PRIVATE pthread)
elseif(WIN32)
  # Windows 原生编译需注意：src/http.cpp 使用 POSIX socket API（socket/bind/listen/poll/MSG_NOSIGNAL），
  # 在 MSVC/MinGW 下需要替换为 Winsock2（WSAStartup/closesocket/ioctlsocket，无 MSG_NOSIGNAL）。
  # 推荐方案：Windows 用户使用 WSL2 跑 Linux 二进制（与 macOS 行为一致，零源码改动）。
  #   - MSVC: 线程走 <windows.h>，无需 pthread/dl
  #   - MinGW/MSYS2: 自带 pthread，dl 不需要
  # libcurl 通过 vcpkg/conan 提供，find_package(CURL REQUIRED) 已在上方处理
  if(MINGW)
    target_link_libraries(todo PRIVATE pthread)
  endif()
else()
  # Linux/Unix/BSD
  target_link_libraries(todo PRIVATE pthread dl)
endif()

# AI 网关依赖 libcurl（macOS/Linux 走系统，Windows 通过 vcpkg/conan 提供）
target_link_libraries(todo PRIVATE CURL::libcurl)

# Web 资源拷贝到构建目录，方便从任意 cwd 启动
# 独立 custom target（ALL + DEPENDS web 文件），使 web/ 变更能触发复制；
# 直接用 CMAKE_BINARY_DIR 避免对 todo 目标的循环依赖。
file(GLOB_RECURSE CPP_TODO_WEB_FILES ${CMAKE_CURRENT_SOURCE_DIR}/web/*)
add_custom_target(web_copy ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/web
          ${CMAKE_BINARY_DIR}/web
  DEPENDS ${CPP_TODO_WEB_FILES}
  COMMENT "复制 Web 静态资源"
)
