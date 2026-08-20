#include "HellRunNavigationDebugLog.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/Archive.h"

namespace
{
    FCriticalSection NavigationLogMutex;
    TUniquePtr<FArchive> NavigationLogArchive;
    FString NavigationLogPath;
    double NavigationLogStartTime = 0.0;
    bool bNavigationLogEnabled = false;

    FString SanitizeField(FString Value)
    {
        Value.ReplaceInline(TEXT("\r"), TEXT(" "));
        Value.ReplaceInline(TEXT("\n"), TEXT(" "));
        Value.ReplaceInline(TEXT("\t"), TEXT(" "));
        return Value;
    }

    void WriteUtf8Line(FArchive& Archive, const FString& Line)
    {
        const FTCHARToUTF8 Utf8(*Line);
        Archive.Serialize(const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length());
        static const ANSICHAR NewLine[] = "\r\n";
        Archive.Serialize(const_cast<ANSICHAR*>(NewLine), 2);
        Archive.Flush();
    }
}

void FHellRunNavigationDebugLog::SetEnabled(bool bEnabled)
{
    FScopeLock Lock(&NavigationLogMutex);
    if (bNavigationLogEnabled == bEnabled)
    {
        return;
    }

    bNavigationLogEnabled = bEnabled;
    if (!bEnabled)
    {
        if (NavigationLogArchive)
        {
            WriteUtf8Line(*NavigationLogArchive, TEXT("# AI navigation trace stopped"));
            NavigationLogArchive.Reset();
        }
        return;
    }

    IFileManager::Get().MakeDirectory(*FPaths::ProjectLogDir(), true);
    NavigationLogPath = FPaths::Combine(
        FPaths::ProjectLogDir(),
        FString::Printf(TEXT("HellRunAINavigation-%s.log"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"))));
    NavigationLogArchive.Reset(IFileManager::Get().CreateFileWriter(*NavigationLogPath));
    NavigationLogStartTime = FPlatformTime::Seconds();
    if (NavigationLogArchive)
    {
        WriteUtf8Line(*NavigationLogArchive,
            TEXT("# seconds\tworld\tactor\tevent\tdetails"));
    }
}

bool FHellRunNavigationDebugLog::IsEnabled()
{
    FScopeLock Lock(&NavigationLogMutex);
    return bNavigationLogEnabled && NavigationLogArchive.IsValid();
}

FString FHellRunNavigationDebugLog::GetLogFilePath()
{
    FScopeLock Lock(&NavigationLogMutex);
    return NavigationLogPath;
}

void FHellRunNavigationDebugLog::Write(const UObject* Context, const TCHAR* Event, const FString& Details)
{
    FScopeLock Lock(&NavigationLogMutex);
    if (!bNavigationLogEnabled || !NavigationLogArchive)
    {
        return;
    }

    const UWorld* World = Context ? Context->GetWorld() : nullptr;
    const AActor* Actor = Cast<AActor>(Context);
    if (!Actor && Context)
    {
        Actor = Context->GetTypedOuter<AActor>();
    }
    const double Elapsed = FPlatformTime::Seconds() - NavigationLogStartTime;
    WriteUtf8Line(*NavigationLogArchive, FString::Printf(TEXT("%.3f\t%s\t%s\t%s\t%s"),
        Elapsed,
        *SanitizeField(GetNameSafe(World)),
        *SanitizeField(GetNameSafe(Actor ? Actor : Context)),
        Event ? Event : TEXT("UNKNOWN"),
        *SanitizeField(Details)));
}
