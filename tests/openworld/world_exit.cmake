# Headless runs that must exit cleanly: exit code 0 and no crash report.
#   1. world_exit.py with every default extension: a small world built, saved (building cache written after the save), exit.
#   2. Empty sessions with only the Jolt users Destruction and Masonry loaded, where a teardown fault of Jolt's statics
#      showed in most runs; repeated so one lucky run can't pass it.
# Inputs: LILABATCH, EXT_DIR (bin/<config>/extensions), SCRIPT, WORK (a scratch folder this test owns).
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/out")

function(run_clean label cfg)
    set(ENV{LILA_CONFIG_DIR} "${cfg}")
    set(ENV{LILA_CRASH_NO_UI} "1")
    execute_process(COMMAND "${LILABATCH}" --batch ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${label}: lilabatch exited with ${rc}\n${out}")
    endif()
    file(GLOB reports "${cfg}/crashes/*")
    if(reports)
        message(FATAL_ERROR "${label}: crash report written: ${reports}\n${out}")
    endif()
endfunction()

run_clean("world" "${WORK}/cfg_world" "${SCRIPT}" "${WORK}/out")

file(GLOB packages LIST_DIRECTORIES true "${EXT_DIR}/*.lilaext")
set(disabled "")
foreach(p IN LISTS packages)
    get_filename_component(name "${p}" NAME_WE)
    if(NOT name STREQUAL "Destruction" AND NOT name STREQUAL "Masonry")
        list(APPEND disabled "\"${name}\"")
    endif()
endforeach()
list(JOIN disabled "," disabled)
file(WRITE "${WORK}/cfg_jolt/prefs/general.json" "{\"extensions\": {\"disabled\": [${disabled}]}}")
file(WRITE "${WORK}/empty.py" "print('empty session')\n")
foreach(i RANGE 1 8)
    run_clean("jolt session ${i}" "${WORK}/cfg_jolt" "${WORK}/empty.py")
endforeach()
