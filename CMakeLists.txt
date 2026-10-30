cmake_minimum_required(VERSION 3.16)
project(DnsSwitch VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(DnsSwitch WIN32 src/main.cpp res/app.rc)

target_compile_definitions(DnsSwitch PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(DnsSwitch PRIVATE ws2_32 iphlpapi comctl32 advapi32 shell32 uxtheme)

if(MSVC)
  # il manifest arriva dal .rc: niente manifest generato dal linker
  target_link_options(DnsSwitch PRIVATE /MANIFEST:NO)
  target_compile_options(DnsSwitch PRIVATE /W4 /utf-8 /permissive-)
  set_property(TARGET DnsSwitch PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_compile_options(DnsSwitch PRIVATE -Wall -Wextra -O2)
  target_link_options(DnsSwitch PRIVATE -municode -static -s)
endif()
