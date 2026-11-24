# The Attributes stage's panels offscreen: Attribute Editor authoring, Channel Box, Drive popover, connection and
# relationship editors, component spreadsheet. Widget state only, no GPU or display.
add_executable(lila_attribute_tests main.cpp test_ae_authoring.cpp test_ae_header.cpp test_channel_box.cpp test_drive.cpp test_connect.cpp test_relationships.cpp test_scene_node_editor.cpp test_component_sheet.cpp)
# GuiPrivate: QWindowSystemInterface, for presses Qt treats as real input (focus on press).
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
target_link_libraries(lila_attribute_tests PRIVATE LilaScene LilaUI Qt6::GuiPrivate doctest::doctest)
lila_deploy_qt(lila_attribute_tests)
add_test(NAME lila_attribute_tests COMMAND lila_attribute_tests)
