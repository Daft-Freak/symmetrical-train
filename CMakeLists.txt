cmake_minimum_required(VERSION 3.13.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

project(brick-train-international-server)

if(MSVC)
  add_compile_options("/W4" "/wd4244" "/wd4324" "/wd4458" "/wd4100")
else()
  add_compile_options("-Wall" "-Wextra" "-Wno-unused-parameter")
endif()

add_executable(BrickTrainServer
  IniFile.cpp
  Main.cpp
  Socket.cpp
)
