# MotoNav PC 模拟器：编译真实固件渲染代码（185b 板）+ shim 层 + SDL2
cmake_minimum_required(VERSION 3.20)
project(motonav_sim C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(FW "${ROOT}/firmware/boards/185b/MotoNav")
set(FW_LIB "${ROOT}/firmware/libraries")

find_package(SDL2 REQUIRED)

add_executable(motonav_sim
  src/sim_main.cpp
  src/sim_net.cpp
  shims/arduino_shim.cpp
  shims/periph_shim.cpp
  shims/gfx_shim.cpp
  shims/spi_shim.cpp
  shims/ble_shim.cpp
  shims/audio_shim.cpp
  shims/arduino_shim_src/WString.cpp
  shims/arduino_shim_src/stdlib_noniso.c
  ${FW}/MotoNav.cpp
  ${FW}/NavMap.cpp
  ${FW}/HomeGifAnimation.cpp
  vendor/miniz/miniz.c
  vendor/miniz/miniz_tinfl.c
  vendor/miniz/miniz_tdef.c
)

target_include_directories(motonav_sim PRIVATE
  shims                 # shim 头文件最优先（覆盖 ESP/Arduino 同名头）
  shims/arduino_shim_src
  src
  ${FW}                 # MotoNav.h/pin_config.h/PortScale.h...
  ${FW_LIB}/GFX_Library_for_Arduino/src/font   # glcdfont.h（经典 5x7）
  vendor/miniz
)

target_compile_definitions(motonav_sim PRIVATE HOST_SIM=1)
target_link_libraries(motonav_sim PRIVATE SDL2::SDL2main SDL2::SDL2)
if(WIN32)
  target_link_libraries(motonav_sim PRIVATE ws2_32)
endif()

# 固件代码有大量 String/float 运算，关闭无关紧要的警告
if(MSVC)
  target_compile_options(motonav_sim PRIVATE /W1)
else()
  target_compile_options(motonav_sim PRIVATE -w)
endif()
