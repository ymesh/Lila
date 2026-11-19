lila_module(lila_vcs Scene
    GitProvider.cpp
    PerforceProvider.cpp
    Process.cpp
    Provider.cpp
    SourceControl.cpp
    SvnProvider.cpp
    VcsCommands.cpp)
target_link_libraries(lila_vcs PUBLIC lila_ops)
