// Layout designed visually in ImRAD (see imrad\mp_panel.cpp for the original
// export) - adapted by hand into a plain floating ImGui window (dropped the
// GLFW/MainWindow chrome ImRAD generates for standalone apps, since we embed
// via hooks\imgui_hook.cpp instead) and wired to the real net\build_sync
// connection instead of empty button stubs.
#pragma once

#include <imrad.h> // ImRad::HBox - the even-width button row layout from the ImRAD export
#include <string>

class MainMenu
{
public:
    void Draw();

private:
    void ResetLayout();

    std::string hostIp;      // bound to the "Host IP:" input field
    bool hostIpInitialized = false;

    ImRad::HBox hb2; // top row: Disconnect (disabled, no cancel yet) / Hide menu / Debug
    ImRad::HBox hb4; // Host / Connect row - drives net/save_sync
    ImRad::HBox hb5; // "Host IP:" label + input row

    bool autoScrollLog = true;
};

extern MainMenu g_mainMenu;
