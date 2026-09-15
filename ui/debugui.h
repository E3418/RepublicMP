// Hand-written (not ImRAD-generated) debug window - opened from MainMenu's
// "Debug" button. Kept deliberately simple.
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
