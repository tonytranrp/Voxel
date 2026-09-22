file(GLOB_RECURSE APP_SRC 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

add_executable(${PROJECT_NAME} ${APP_SRC})

# Add include directories for the application
target_include_directories(${PROJECT_NAME} 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/app-framework/src
)

target_link_libraries(${PROJECT_NAME} 
    PRIVATE 
        voxy_engine
)

# First copy engine resources (including engine shaders if they exist)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/app-framework/resources"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/resources"
)

# Then copy application-specific shaders, merging with engine shaders if they exist
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/resources/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/src/vox/shaders"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/resources/shaders"
)

# Then copy application-specific resources
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/src/resources"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/resources"
)

# Copy required DLLs
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
    -E copy_if_different
    $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}>
    $<TARGET_FILE_DIR:${PROJECT_NAME}>
    COMMAND_EXPAND_LISTS
)

# Slang loads its downstream compilers (slang-glslang, slang-rt, ...) as plugins at runtime, so
# they are not in TARGET_RUNTIME_DLLS and have to be copied explicitly next to the executable.
if(WIN32)
    file(GLOB SLANG_RUNTIME_DLLS "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin/slang*.dll")
    foreach(SLANG_DLL ${SLANG_RUNTIME_DLLS})
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SLANG_DLL}" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        )
    endforeach()
endif()