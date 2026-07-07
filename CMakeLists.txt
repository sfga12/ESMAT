cmake_minimum_required(VERSION 3.15)
project(ESMAT CXX C)

# CSPICE.lib kullanırken arka planda Statik /MT CRT kullanıldığı için bizim de kendi projemizde Statik CRT /MT kullanmamız gerekiyor
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD) # Silence FetchContent_Populate deprecation
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Fetch GLFW
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# Fetch GLM master (fixes CMake compatibility issues)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        master
)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glm)

# Fetch Dear ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
)
FetchContent_Populate(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PRIVATE glfw)
target_compile_definitions(imgui PUBLIC IMGUI_USE_WCHAR32)

# Fetch ImPlot
FetchContent_Declare(
    implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        master
)
FetchContent_Populate(implot)

add_library(implot STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
    ${implot_SOURCE_DIR}/implot_demo.cpp
)
target_include_directories(implot PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(implot PUBLIC imgui)
target_compile_definitions(implot PUBLIC IMGUI_USE_WCHAR32 IMGUI_DEFINE_MATH_OPERATORS)

# GLAD setup (generated into dependencies/glad)
add_library(glad
    dependencies/glad/src/glad.c
)
target_include_directories(glad PUBLIC dependencies/glad/include)

# Fetch tinygltf
FetchContent_Declare(
    tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG        v2.8.23
)
set(TINYGLTF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(tinygltf)

# Our Executable
add_executable(ESMAT
    src/main.cpp
    src/app_icon.rc
)

target_link_libraries(ESMAT
    PRIVATE glfw
    PRIVATE glm
    PRIVATE glad
    PRIVATE imgui
    PRIVATE implot
    PRIVATE tinygltf
    PRIVATE ${CMAKE_SOURCE_DIR}/dependencies/cspice/lib/cspice.lib
)
target_include_directories(ESMAT PRIVATE ${CMAKE_SOURCE_DIR}/dependencies/cspice/include)
target_compile_definitions(ESMAT PRIVATE IMGUI_USE_WCHAR32)

# --- CPack Configuration ---
set(CPACK_PACKAGE_NAME "ESMAT")
set(CPACK_PACKAGE_VENDOR "ESMAT Team")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ESMAT - Ephemeris Soft Modular Analysis Tool")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "ESMAT")

# Files to include in the package
install(TARGETS ESMAT RUNTIME DESTINATION bin)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/data/" DESTINATION data)

# Windows specific (NSIS is usually the default for Windows installers via CPack)
if(WIN32)
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "ESMAT Simulation Tool")
    set(CPACK_NSIS_HELP_LINK "https://github.com/your-repo/ESMAT")
    set(CPACK_NSIS_CREATE_ICONS_EXTRA "CreateShortCut '$DESKTOP/ESMAT.lnk' '$INSTDIR/bin/ESMAT.exe'")
endif()

include(CPack)

include(CPack)
