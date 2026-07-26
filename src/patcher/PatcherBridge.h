#pragma once
#include <QString>

class PatcherBridge
{
public:
    static bool apply(const QString& gameDir);
    static bool revert(const QString& gameDir);
    static bool isPatched(const QString& gameDir);
};