// SPDX-License-Identifier: GPL-3.0-only
// Main multiplayer window: a start page that picks the kind of game, then a
// small lobby - what the session is doing right now (hosting and waiting,
// players joining and at which step, in the game with the players and how
// the lockstep runs...), the one button that makes sense in that state (Host / Connect, Stop
// hosting, Disconnect) and the everyday log.
#pragma once

#include <string>

class MainMenu
{
public:
    void Draw();

private:
    std::string hostIp;      // bound to the "Host IP:" input field
    bool hostIpInitialized = false;
    bool autoScrollLog = true;
};

extern MainMenu g_mainMenu;
