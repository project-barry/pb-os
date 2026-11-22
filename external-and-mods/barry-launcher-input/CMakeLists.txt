# Barry.Input: Barry Launcher's keyboards and trackpad as a QML module, so
# the QML that draws them (and any skin) calls straight into the devices:
# no daemon, no socket, no request per key or pointer move.
cmake_minimum_required(VERSION 3.21)
project(barry_input LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS ON)

# The keyboards' words (glide typing, suggestions, autocorrect): barryglide,
# a static library of its own (built with -ffp-contract=off, as it says).
set(BARRYGLIDE_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(glide)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick Network DBus)
find_package(X11 REQUIRED)
if(NOT X11_Xtst_FOUND)
  message(FATAL_ERROR "Barry.Input needs libXtst")
endif()
qt_standard_project_setup(REQUIRES 6.5)

set(QT_QML_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml)
qt_add_qml_module(barryinput
  URI Barry.Input
  VERSION 1.0
  PLUGIN_TARGET barryinput
  SOURCES
    keymap.cpp keymap.h
    uinputdevice.cpp uinputdevice.h
    virtualmouse.cpp virtualmouse.h
    virtualkeyboard.cpp virtualkeyboard.h
    touchrules.h
    bottompanel.h
    device.cpp device.h
    gesture.cpp gesture.h
    panzoom.cpp panzoom.h
    panzoomarea.cpp panzoomarea.h
    touchtrackpad.cpp touchtrackpad.h
    trackpadarea.cpp trackpadarea.h
    xtesttyper.cpp xtesttyper.h
    typer.cpp typer.h
    keyboardlanguages.cpp keyboardlanguages.h
    glideengine.cpp glideengine.h
    deckcontrols.cpp deckcontrols.h
    haptics.cpp haptics.h
    keyboardcontroller.cpp keyboardcontroller.h
    keyboardgrid.cpp
    keyboardstate.cpp keyboardstate.h
    shellstate.cpp shellstate.h
    skin.cpp skin.h
    heattrail.cpp heattrail.h
    pageswipe.cpp pageswipe.h
    edgeswipe.cpp edgeswipe.h
    steamgame.cpp steamgame.h
    pbosqam.cpp pbosqam.h
  QML_FILES
    TileDrop.qml
)
target_compile_options(barryinput PRIVATE -Wall -Wextra -Werror)
target_link_libraries(barryinput PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network Qt6::DBus X11::X11 X11::Xtst
  barryglide)

# The glide equivalence test's tool (tests/glide_engine): GlideEngine, as
# the keyboards use it, answering shelld's cases.
option(BARRY_INPUT_GLIDE_TEST "Build tests/glide_engine's glide-engine-test" OFF)
if(BARRY_INPUT_GLIDE_TEST)
  qt_add_executable(glide-engine-test
    tests/glide_engine/glide_engine_test.cpp
    glideengine.cpp glideengine.h
    keyboardlanguages.cpp keyboardlanguages.h)
  target_compile_options(glide-engine-test PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(glide-engine-test PRIVATE Qt6::Core Qt6::Qml barryglide)
endif()

# The touch rules' tests (tests/gestures): PanZoomGesture alone (no Qt), and
# TouchScroll / PanZoomArea under Qt's touch events, offscreen.
option(BARRY_INPUT_GESTURE_TEST "Build tests/gestures' pan-zoom-test, bottom-panel-test and touch-surfaces-test" OFF)
if(BARRY_INPUT_GESTURE_TEST)
  add_executable(pan-zoom-test tests/gestures/pan_zoom_test.cpp panzoom.cpp panzoom.h touchrules.h)
  target_compile_options(pan-zoom-test PRIVATE -Wall -Wextra -Werror)
  add_executable(bottom-panel-test tests/gestures/bottom_panel_test.cpp bottompanel.h touchrules.h)
  target_compile_options(bottom-panel-test PRIVATE -Wall -Wextra -Werror)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  qt_add_executable(touch-surfaces-test
    tests/gestures/touch_surfaces_test.cpp
    panzoom.cpp panzoom.h panzoomarea.cpp panzoomarea.h pageswipe.cpp pageswipe.h touchrules.h)
  target_compile_options(touch-surfaces-test PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(touch-surfaces-test PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Test)
endif()

# KeyboardController's test (tests/keyboard_controller): offscreen, a stub
# Typer (the test's own, in typer.cpp's place) and a stub keyd.
option(BARRY_INPUT_KEYBOARD_TEST "Build tests/keyboard_controller's keyboard-controller-test" OFF)
if(BARRY_INPUT_KEYBOARD_TEST)
  qt_add_executable(keyboard-controller-test
    tests/keyboard_controller/keyboard_controller_test.cpp
    keyboardcontroller.cpp keyboardcontroller.h keyboardgrid.cpp
    glideengine.cpp glideengine.h
    keyboardlanguages.cpp keyboardlanguages.h
    haptics.cpp haptics.h
    typer.h xtesttyper.h
    virtualkeyboard.cpp virtualkeyboard.h
    uinputdevice.cpp uinputdevice.h
    keymap.cpp keymap.h)
  target_compile_options(keyboard-controller-test PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(keyboard-controller-test PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network barryglide)
endif()
