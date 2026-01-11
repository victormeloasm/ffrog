cmake_minimum_required(VERSION 3.22)

project(ffrog VERSION 1.7 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt autogen (moc/uic/rcc)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets DBus Concurrent)

add_executable(ffrog
  src/main.cpp
  src/MainWindow.cpp
  src/UDisks2.cpp
  src/MainWindow.h
  src/UDisks2.h
)

target_include_directories(ffrog PRIVATE src)
target_link_libraries(ffrog PRIVATE Qt6::Widgets Qt6::DBus Qt6::Concurrent)

# Keep Qt keywords enabled (signals/slots). Do NOT define QT_NO_KEYWORDS.
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  target_compile_options(ffrog PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(ffrog PROPERTIES
  OUTPUT_NAME "ffrog"
)

install(TARGETS ffrog RUNTIME DESTINATION bin)

