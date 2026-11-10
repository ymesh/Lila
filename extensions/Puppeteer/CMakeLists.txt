# Puppeteer, built as <bin>/<config>/extensions/Puppeteer.lilaext next to lila.exe, which loads it by default.
set(PUPPETEER_RIG_SOURCES
    Puppeteer.cpp Puppeteer.h Common.h
    rig/RigMath.h rig/Rig.h rig/Character.cpp rig/RigElements.cpp rig/JointTools.cpp rig/Modes.cpp rig/Controllers.cpp rig/FullBody.cpp rig/Strings.cpp rig/AnimProxy.cpp rig/AnimProxy.h
    gizmo/Spinner.h gizmo/Spinner.cpp gizmo/SpinnerOps.cpp gizmo/StringsUi.cpp gizmo/LineOfActionTool.cpp gizmo/BreakdownPartsTool.cpp gizmo/RulesEditor.cpp gizmo/ControllerDraw.h gizmo/ControllerDraw.cpp)
# Animate-mode panels (Stacker, Mixer, Anim Bank, Cycle Maker, Poses): list their sources here.
set(PUPPETEER_ANIM_SOURCES
    anim/AnimPanels.cpp anim/CurveOverlay.cpp anim/AnimCommands.cpp anim/AnimCommands.h anim/AnimOps.cpp anim/AnimOps.h anim/KeyMath.h anim/WalkCycle.h anim/PanelModels.h)
lila_add_extension(lila_puppeteer_extension ID Puppeteer MANIFEST extension.json UI
    SOURCES ${PUPPETEER_RIG_SOURCES} ${PUPPETEER_ANIM_SOURCES})
target_include_directories(lila_puppeteer_extension PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_puppeteer_extension)
    endif()
endforeach()
