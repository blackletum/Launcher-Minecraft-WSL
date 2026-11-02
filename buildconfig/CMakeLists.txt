######## Configure the file with build properties ########

if(Launcher_NEW_UPDATER)
    if(NOT Launcher_UPDATER_SERIAL_FLOOR MATCHES "^(0|[1-9][0-9]*)$")
        message(FATAL_ERROR "Launcher_UPDATER_SERIAL_FLOOR must be a number, not '${Launcher_UPDATER_SERIAL_FLOOR}'")
    endif()
    # The keys document compiled in as the floor, as a string literal of its exact bytes
    set(Launcher_UPDATER_KEYS_FLOOR_BYTES "")
    if(Launcher_UPDATER_KEYS_FLOOR)
        if(NOT EXISTS "${Launcher_UPDATER_KEYS_FLOOR}")
            message(FATAL_ERROR "Launcher_UPDATER_KEYS_FLOOR: ${Launcher_UPDATER_KEYS_FLOOR} doesn't exist")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${Launcher_UPDATER_KEYS_FLOOR}")
        file(READ "${Launcher_UPDATER_KEYS_FLOOR}" floor_text)
        foreach(key ${Launcher_UPDATER_ROOT_KEYS})
            if(NOT floor_text MATCHES "\"${key}\"")
                message(FATAL_ERROR "Launcher_UPDATER_KEYS_FLOOR doesn't list the pinned root key ${key}")
            endif()
        endforeach()
        file(READ "${Launcher_UPDATER_KEYS_FLOOR}" floor_hex HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "\\\\x\\1" Launcher_UPDATER_KEYS_FLOOR_BYTES "${floor_hex}")
    endif()
endif()

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/BuildConfig.cpp.in" "${CMAKE_CURRENT_BINARY_DIR}/BuildConfig.cpp")

add_library(BuildConfig STATIC
    BuildConfig.h
    ${CMAKE_CURRENT_BINARY_DIR}/BuildConfig.cpp
)

target_link_libraries(BuildConfig Qt5::Core)
target_include_directories(BuildConfig PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(Launcher_NEW_UPDATER)
    # everything that knows the build configuration knows whether the new updater is in it
    target_compile_definitions(BuildConfig PUBLIC LAUNCHER_NEW_UPDATER)
endif()
