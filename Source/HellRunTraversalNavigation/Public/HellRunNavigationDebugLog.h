#pragma once

#include "CoreMinimal.h"

/** Event-based navigation trace written to Saved/Logs while AI navigation debug is enabled. */
class HELLRUNTRAVERSALNAVIGATION_API FHellRunNavigationDebugLog
{
public:
    static void SetEnabled(bool bEnabled);
    static bool IsEnabled();
    static FString GetLogFilePath();
    static void Write(const UObject* Context, const TCHAR* Event, const FString& Details);
};
