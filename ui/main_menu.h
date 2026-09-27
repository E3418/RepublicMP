// Main multiplayer window: Host / Connect (save sync), Disconnect, status and
// the everyday log. Layout exported from ImRAD (imrad\mp_panel.cpp) and
// embedded as a plain floating ImGui window.
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

    ImRad::HBox hb2; // top row: Disconnect / Hide menu / Debug
    ImRad::HBox hb4; // Host / Connect row - drives net/save_sync
    ImRad::HBox hb5; // "Host IP:" label + input row

    bool autoScrollLog = true;
};

extern MainMenu g_mainMenu;
