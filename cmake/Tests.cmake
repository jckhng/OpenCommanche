foreach(target
    effects_tests
    demo_tests
    playback_resource_tests
    renderer_tests
    loading_work_tests
    fanfare_timing_tests
    post_start_tests
    menu_entry_tests
    irq7_wave_tests
    flight_work_tests
    model_raster_tests
    menu_work_tests
    entity_clock_tests
    model_work_tests
    controller_work_tests
    keyboard_tests
    pilot_tests
    assets_tests
    frontend_tests
    mission_tests
    mission_work_tests
    presentation_tests
    gameplay_tests
    scene_tests
    audio_tests
    cold_audio_tests
    startup_timing_tests
    startup_storage_tests
    callback_audio_tests
    audio_scheduler_tests
    world_renderer_tests
    flight_tests
    training_tests
    collision_tests
    timing_tests
    playback_tests
    cockpit_tests
    combat_tests
    weapons_tests
    options_tests
    debrief_tests
    session_tests
    startup_input_tests
)
    add_executable(${target} tests/${target}.cpp)
    target_link_libraries(${target} PRIVATE comanchecd_renderer)
    add_test(NAME ${target} COMMAND ${target})
endforeach()

target_include_directories(startup_input_tests PRIVATE src)
target_include_directories(session_tests PRIVATE host)
find_package(Threads REQUIRED)
target_link_libraries(presentation_tests PRIVATE Threads::Threads)
