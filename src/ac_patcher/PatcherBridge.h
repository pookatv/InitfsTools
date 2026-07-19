#pragma once
#include <QString>

class PatcherBridge
{
public:
    // Call this from any script/window that needs the anticheat patch applied.
    // gameDir = the directory containing the game exe.
    // Returns true if the patch was applied or already present, false on error.
    static bool apply(const QString& gameDir);
};