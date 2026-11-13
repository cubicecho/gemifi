# ESP-IDF project (PlatformIO framework = espidf). The gemifi_esp component (platform/esp/) has the
# platform layer and the controller core; src/ is this firmware's main.
cmake_minimum_required(VERSION 3.16.0)
list(APPEND EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../../platform/esp)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hello)
