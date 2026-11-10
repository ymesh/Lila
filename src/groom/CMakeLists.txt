add_subdirectory(groomlab)
# Its own static lib so GroomLab's tests link it too.
add_library(lila_groom_evaluator STATIC GroomDisplay.h GroomEvaluator.cpp GroomEvaluator.h GroomEvaluatorAsync.cpp GroomEvaluatorAsync.h GroomEvaluatorStroke.cpp GroomEvaluatorViews.cpp GroomStrokeHost.cpp GroomStrokeHost.h)
target_include_directories(lila_groom_evaluator PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lila_groom_evaluator PUBLIC groomlab_core PRIVATE nlohmann_json::nlohmann_json)
# The look engine on a LookDevice (Lila's Vulkan compute), shared with the GroomLab tests.
add_library(lila_groom_look STATIC LookDevice.h LookEngineDevice.cpp LookEngineDevice.h)
target_link_libraries(lila_groom_look PUBLIC groomlab_core)
target_include_directories(lila_groom_look PUBLIC ${CMAKE_SOURCE_DIR}/src)
set_target_properties(lila_groom_evaluator PROPERTIES FOLDER groom)
lila_module(lila_groom Scene Attrs.cpp Attrs.h Groom.cpp Groom.h GroomBrush.cpp GroomBrush.h GroomExport.h GroomIo.cpp GroomLook.cpp GroomLook.h YetiImport.cpp YetiImport.h YetiImport.cpp YetiImport.h GroomCommands.cpp GroomLockCommands.cpp GroomEdit.h GroomGnNodes.cpp GroomGnNodes.h GroomLab.cpp GroomLab.h GroomLabBridge.h GroomNodes.cpp GroomNodes.h GroomPresets.cpp GroomPresets.h GroomGraph.cpp GroomGraph.h GuideData.cpp GuideData.h LockOps.cpp LockOps.h LookDevice.cpp
    HairCards.cpp HairCards.h Locks.cpp Locks.h RodSettle.cpp RodSettle.h Settle.cpp Settle.h Shapes.cpp Shapes.h Shell.cpp Shell.h Style.cpp Style.h)
target_link_libraries(lila_groom PUBLIC lila_curve_scene lila_clay lila_brush TBB::tbb PRIVATE groomlab_core lila_groom_evaluator lila_groom_look lila_gn nlohmann_json::nlohmann_json)
