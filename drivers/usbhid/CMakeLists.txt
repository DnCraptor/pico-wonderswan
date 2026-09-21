add_library(usbhid INTERFACE)
target_sources(usbhid INTERFACE ${CMAKE_CURRENT_LIST_DIR}/usbhid.cpp)
target_include_directories(usbhid INTERFACE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/../../src ${CMAKE_CURRENT_LIST_DIR}/../nespad)
target_link_libraries(usbhid INTERFACE pico_stdlib tinyusb_host tinyusb_board)
