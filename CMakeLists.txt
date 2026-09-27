cmake_minimum_required(VERSION 3.21)

project(kinmap VERSION 2.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_add_executable(kinmap
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
)

target_link_libraries(kinmap PRIVATE Qt6::Widgets)

install(TARGETS kinmap RUNTIME DESTINATION bin)

# Flathub desktop integration
install(FILES org.oneal.kinmap.desktop
        DESTINATION share/applications)
install(FILES org.oneal.kinmap.metainfo.xml
        DESTINATION share/metainfo)
install(FILES org.oneal.kinmap.svg
        DESTINATION share/icons/hicolor/scalable/apps)
