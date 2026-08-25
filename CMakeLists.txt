cmake_minimum_required(VERSION 3.16)
project(cpp-todo C CXX)

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
  vendor/sqlite3.c
)

target_include_directories(todo PRIVATE src vendor)
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
else()
  target_link_libraries(todo PRIVATE pthread dl)
endif()

# Web 资源拷贝到构建目录，方便从任意 cwd 启动
add_custom_command(TARGET todo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/web
          $<TARGET_FILE_DIR:todo>/web
  COMMENT "复制 Web 静态资源"
)
