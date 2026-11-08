# The playtest package's launcher (ror_launcher.cpp), built by tools/port/build.py dist: a settings window (Win32).
cmake_minimum_required(VERSION 3.20)
project(ror_launcher CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(RuleOfRose WIN32 ror_launcher.cpp)
target_compile_definitions(RuleOfRose PRIVATE UNICODE _UNICODE)
target_link_libraries(RuleOfRose PRIVATE bcrypt comdlg32 comctl32 shell32)
# Static C runtime: the launcher runs before anything else in the package is needed.
set_property(TARGET RuleOfRose PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
