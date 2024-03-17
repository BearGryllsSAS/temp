# 指定最低版本号
cmake_minimum_required(VERSION 3.0)

# 指定c++标准
set(CMAKE_CXX_STANDARD 11)

# 项目名称
project(Server)

# 指定可执行程序的输出路径---宏EXECUTABLE_OUTPUT_PATH (若路径不存在则自动生成)
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR})

# 设置编译器，默认为 g++
set(CMAKE_CXX_COMPILER g++)

# 设置调试模式，默认为启用
option(DEBUG "Enable debug mode" ON)

# 根据 DEBUG 变量设置编译选项
if(DEBUG)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2")
endif()

# 添加可执行文件，并指定源文件
add_executable(server main.cpp ./timer/lst_timer.cpp ./chat/chat_conn.cpp ./log/log.cpp ./mysql/sql_connection_pool.cpp server.cpp config/config.cpp)

# 链接 pthread 和 mysqlclient 库
target_link_libraries(server pthread mysqlclient)
