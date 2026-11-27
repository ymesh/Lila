lila_module(lila_slots Scene
    Slots.cpp
    Slots.h
    SlotCommands.cpp
    SlotSet.cpp
    SlotsGn.cpp
    SlotsGn.h
    SlotTypes.cpp)
target_link_libraries(lila_slots PUBLIC lila_scene lila_gn)
