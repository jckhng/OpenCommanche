foreach(target
    flight_work_probe
    movie_work_probe
    music_work_probe
    callback_audio_probe
    sound_device_probe
    effects_probe
    cinematic_probe
    demo_camera_probe
    render_probe
    loading_work_probe
    pilot_probe
    assets_probe
    frontend_probe
    audio_probe
    contact_probe
    timing_probe
    playback_probe
    cockpit_probe
    options_probe
    combat_probe
    debrief_probe
    targeting_probe
    mission_catalog_probe
    mission_route_probe
    completion_probe
    playback_resource_probe
    scenery_probe
    scenery_effects_probe
)
    add_executable(${target} tests/${target}.cpp)
    target_link_libraries(${target} PRIVATE comanchecd_renderer)
endforeach()

target_include_directories(completion_probe PRIVATE host)
