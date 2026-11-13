add_library(comanchecd_fm third_party/dbopl/dbopl.cpp)
target_include_directories(comanchecd_fm PUBLIC third_party/dbopl)

add_library(comanchecd_renderer
    src/menu_renderer.cpp
    src/pilot.cpp
    src/assets.cpp
    src/frontend.cpp
    src/mission.cpp
    src/mission_work.cpp
    src/gameplay.cpp
    src/scene.cpp
    src/audio.cpp
    src/audio_playback.cpp
    src/world_renderer.cpp
    src/flight.cpp
    src/training.cpp
    src/timing.cpp
    src/cockpit.cpp
    src/debrief.cpp
    src/session.cpp
    src/options.cpp
    src/playback.cpp
    src/effects.cpp
    src/sound_device.cpp
    src/callback_audio.cpp
    src/audio_scheduler.cpp
    src/startup_timing.cpp
    src/fanfare_timing.cpp
    src/startup_resource.cpp
    src/startup_storage.cpp
    src/movie_timing.cpp
    src/menu_entry_timing.cpp
    src/mission_loading.cpp
    src/loading_work.cpp
    src/flight_work.cpp
    src/flight_input.cpp
    src/model_raster.cpp
    src/model_work.cpp
    src/controller_contacts.cpp
    src/menu_work.cpp
    src/menu_input.cpp
    src/controller_contact_response.cpp
    src/keyboard.cpp
)
target_link_libraries(comanchecd_renderer PRIVATE comanchecd_fm)
target_include_directories(comanchecd_renderer PUBLIC include)
