function(kcf_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE kcf_warnings)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

kcf_add_test(chatter_filter_tests chatter_filter_tests.cpp)
target_link_libraries(chatter_filter_tests PRIVATE kcf_core)

kcf_add_test(configuration_tests configuration_tests.cpp)
target_link_libraries(configuration_tests PRIVATE kcf_core)

kcf_add_test(common_tests common_tests.cpp)
target_link_libraries(common_tests PRIVATE kcf_common)

kcf_add_test(adapter_logic_tests adapter_logic_tests.cpp)
target_link_libraries(adapter_logic_tests PRIVATE kcf_adapter_logic)

if(UNIX)
    kcf_add_test(posix_tests posix_tests.cpp)
    target_link_libraries(posix_tests PRIVATE kcf_common)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Drives the real evdev/uinput adapter with a synthetic chattering keyboard. Needs read/write
    # access to /dev/uinput and /dev/input/event*; exits with 77 (skipped) without it.
    kcf_add_test(linux_uinput_integration_tests linux_uinput_integration_tests.cpp)
    target_link_libraries(linux_uinput_integration_tests PRIVATE kcf_platform)
    set_tests_properties(linux_uinput_integration_tests PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(APPLE)
    # Drives the real event tap with synthetic HID-state events. Needs Accessibility permission for
    # the process running it; exits with 77 (skipped) without it.
    kcf_add_test(macos_event_tap_integration_tests macos_event_tap_integration_tests.cpp)
    target_link_libraries(macos_event_tap_integration_tests PRIVATE kcf_platform "-framework ApplicationServices")
    set_tests_properties(macos_event_tap_integration_tests PROPERTIES SKIP_RETURN_CODE 77)
endif()
