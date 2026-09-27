// Debug window (opened from the main menu): manual sync connections and the
// verbose sync log.
#pragma once

class Debugui
{
public:
    void Open();
    void Close();
    void Draw();

private:
    bool isOpen = false;
};

extern Debugui g_debugui;
