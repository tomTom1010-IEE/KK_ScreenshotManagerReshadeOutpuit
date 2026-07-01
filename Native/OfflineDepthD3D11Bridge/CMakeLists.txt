cmake_minimum_required(VERSION 3.20)
project(OfflineDepthD3D11Bridge LANGUAGES CXX)

add_library(OfflineDepthD3D11Bridge SHARED bridge.cpp)
set_target_properties(OfflineDepthD3D11Bridge PROPERTIES PREFIX "")
target_compile_features(OfflineDepthD3D11Bridge PRIVATE cxx_std_17)
target_compile_definitions(OfflineDepthD3D11Bridge PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(OfflineDepthD3D11Bridge PRIVATE /W4 /EHsc)
else()
    target_compile_options(OfflineDepthD3D11Bridge PRIVATE -Wall -Wextra)
    target_link_options(OfflineDepthD3D11Bridge PRIVATE -static-libgcc -static-libstdc++ -static)
endif()

target_link_libraries(OfflineDepthD3D11Bridge PRIVATE d3d11 dxgi d3dcompiler)
