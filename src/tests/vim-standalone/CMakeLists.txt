cmake_minimum_required(VERSION 3.16)
project(VimRegressionTests LANGUAGES CXX)
set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 14)
find_package(Qt5 REQUIRED COMPONENTS Widgets Test PrintSupport)
find_library(QSCI_LIBRARY qscintilla2_qt5 REQUIRED)
add_executable(test-vim-input ../TestVimInputHandler.cpp ../TestVimInputHandler.h
    ../../VimInputHandler.cpp ../../VimInputHandler.h)
target_include_directories(test-vim-input PRIVATE ../..)
target_link_libraries(test-vim-input PRIVATE Qt5::Widgets Qt5::Test Qt5::PrintSupport ${QSCI_LIBRARY})
enable_testing()
add_test(NAME vim-regressions COMMAND test-vim-input)
set_tests_properties(vim-regressions PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

