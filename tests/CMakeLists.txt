# Pure-logic unit tests -- no game, no minhook, no linking against mgmp.dll.
# Each covers a piece of the mod that can only otherwise be exercised in-game
# (a live desync, an AstroZombie fight), so the branch logic gets tested here
# and the in-game run is left to prove only the parts that need a game. See
# the header comment in each .cpp for which hook/wire-format it pins.

add_executable(test_proto test_proto.cpp)
target_include_directories(test_proto PRIVATE ${CMAKE_SOURCE_DIR}/src/net)
add_test(NAME test_proto COMMAND test_proto)

add_executable(test_checkpoint test_checkpoint.cpp ../src/session/mgmp_checkpoint_io.cpp)
target_include_directories(test_checkpoint PRIVATE ../src/core ../src/net ../src/session)
target_compile_definitions(test_checkpoint PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_test(NAME test_checkpoint COMMAND test_checkpoint)
set_tests_properties(test_checkpoint PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/..")

add_executable(test_hashring test_hashring.cpp)
target_include_directories(test_hashring PRIVATE ${CMAKE_SOURCE_DIR}/src/net)
add_test(NAME test_hashring COMMAND test_hashring)

add_executable(test_timedelay test_timedelay.cpp)
target_include_directories(test_timedelay PRIVATE ${CMAKE_SOURCE_DIR}/src/determinism)
add_test(NAME test_timedelay COMMAND test_timedelay)

add_executable(test_barrier test_barrier.cpp)
target_include_directories(test_barrier PRIVATE ${CMAKE_SOURCE_DIR}/src/session)
add_test(NAME test_barrier COMMAND test_barrier)

add_executable(test_split test_split.cpp)
target_include_directories(test_split PRIVATE ${CMAKE_SOURCE_DIR}/src/session)
add_test(NAME test_split COMMAND test_split)
add_executable(test_derived test_derived.cpp)
target_include_directories(test_derived PRIVATE ${CMAKE_SOURCE_DIR}/src/session)
add_test(NAME test_derived COMMAND test_derived)

add_executable(test_sigscan test_sigscan.cpp ${CMAKE_SOURCE_DIR}/src/core/mgmp_sigscan.cpp)
target_include_directories(test_sigscan PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
add_test(NAME test_sigscan COMMAND test_sigscan)

add_executable(test_resolve_live test_resolve_live.cpp ${CMAKE_SOURCE_DIR}/src/core/mgmp_sigscan.cpp)
target_include_directories(test_resolve_live PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
add_test(NAME test_resolve_live COMMAND test_resolve_live)

add_executable(test_battleid test_battleid.cpp)
target_include_directories(test_battleid PRIVATE ${CMAKE_SOURCE_DIR}/src/session)
add_test(NAME test_battleid COMMAND test_battleid)

# The one test that compiles a piece of the mod rather than a header: the config
# reader is a .cpp because it owns the Config singleton, and it is worth covering
# because it is the only parser in the project whose input a human types.
add_executable(test_config test_config.cpp ${CMAKE_SOURCE_DIR}/src/core/mgmp_config.cpp)
target_include_directories(test_config PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/third_party)
target_compile_definitions(test_config PRIVATE
    WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS NOMINMAX)
add_test(NAME test_config COMMAND test_config)

# Exercise the actual choice state machine and guarded memory reads, with only
# external services and the two resolved game callbacks supplied by the test.
add_executable(test_choice test_choice.cpp
    ${CMAKE_SOURCE_DIR}/src/session/mgmp_choice.cpp
    ${CMAKE_SOURCE_DIR}/src/core/mgmp_mem.cpp)
target_include_directories(test_choice PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/src/net
    ${CMAKE_SOURCE_DIR}/src/session)
target_compile_definitions(test_choice PRIVATE
    WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS NOMINMAX
    _ITERATOR_DEBUG_LEVEL=0) # Fake screens use the game's 32-byte MSVC string ABI.
add_test(NAME test_choice COMMAND test_choice)

foreach(module follow catsync)
    add_executable(test_${module} test_${module}.cpp
        ${CMAKE_SOURCE_DIR}/src/session/mgmp_${module}.cpp
        ${CMAKE_SOURCE_DIR}/src/core/mgmp_mem.cpp)
    target_include_directories(test_${module} PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/src/net
        ${CMAKE_SOURCE_DIR}/src/session ${CMAKE_SOURCE_DIR}/src/determinism)
    target_compile_definitions(test_${module} PRIVATE
        MGMP_WITH_CHECKPOINT MGMP_WITH_SETUP WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_link_libraries(test_${module} PRIVATE user32)
    add_test(NAME test_${module} COMMAND test_${module})
endforeach()

add_executable(test_roster test_roster.cpp ${CMAKE_SOURCE_DIR}/src/session/mgmp_roster.cpp)
target_include_directories(test_roster PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/src/net
    ${CMAKE_SOURCE_DIR}/src/session ${CMAKE_SOURCE_DIR}/src/determinism)
target_compile_definitions(test_roster PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_link_libraries(test_roster PRIVATE user32)
add_test(NAME test_roster COMMAND test_roster)

add_executable(test_setup test_setup.cpp ${CMAKE_SOURCE_DIR}/src/session/mgmp_setup.cpp ${CMAKE_SOURCE_DIR}/src/core/mgmp_mem.cpp)
target_include_directories(test_setup PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/src/net ${CMAKE_SOURCE_DIR}/src/session)
target_compile_definitions(test_setup PRIVATE
    MGMP_WITH_CHECKPOINT WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS NOMINMAX)
add_test(NAME test_setup COMMAND test_setup)

foreach(t test_proto test_hashring test_timedelay test_barrier test_split test_battleid test_config test_choice test_follow test_catsync test_roster test_setup)
    target_compile_options(${t} PRIVATE /W3)
endforeach()
