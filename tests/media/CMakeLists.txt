# Video and audio decoding, take media, sound cues and the Licenses list. The test clips are encoded here with FFmpeg's
# own LGPL encoders, so nothing is downloaded.
add_executable(lila_media_tests main.cpp test_decode.cpp test_media.cpp test_cues.cpp test_licenses.cpp test_stream.cpp)
find_package(FFMPEG REQUIRED)
target_include_directories(lila_media_tests PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(lila_media_tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(lila_media_tests PRIVATE LilaUI doctest::doctest ${FFMPEG_LIBRARIES})
add_test(NAME lila_media_tests COMMAND lila_media_tests)
