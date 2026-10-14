find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

qt_add_executable(uvapp WIN32
    main.cpp
    MainWindow.cpp
    MainWindow.h
    ProcessWorker.cpp
    ProcessWorker.h
    resources.qrc
    ultraviolet.rc
)

target_include_directories(uvapp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/assets
)

target_link_libraries(uvapp PRIVATE
    uvcore
    Qt6::Widgets
)

set_target_properties(uvapp PROPERTIES OUTPUT_NAME "Ultraviolet")

# Stage the Qt runtime (DLLs, platform/style/imageformat plugins) next to the
# exe after every build. Without this the app fails to launch on any machine
# that does not already have the matching Qt bin on PATH.
find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${Qt6_DIR}/../../../bin")
if(WINDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET uvapp POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --$<IF:$<CONFIG:Debug>,debug,release>
                --no-translations --no-compiler-runtime
                "$<TARGET_FILE:uvapp>"
        COMMENT "Deploying Qt runtime next to Ultraviolet.exe"
        VERBATIM)
endif()
