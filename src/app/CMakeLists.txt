find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

qt_add_executable(uvapp WIN32
    main.cpp
    MainWindow.cpp
    MainWindow.h
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
