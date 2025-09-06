cmake_minimum_required(VERSION 3.15)
project(ReverseProxyC C)

set(CMAKE_C_STANDARD 99)

# Thêm include path
include_directories(src)

# Tìm tất cả file .c
file(GLOB_RECURSE SRC_FILES
    src/*.c
)

# Tạo executable
add_executable(proxy ${SRC_FILES})

# Link thư viện socket
if(WIN32)
    target_link_libraries(proxy ws2_32)
else()
    target_link_libraries(proxy pthread)
endif()
