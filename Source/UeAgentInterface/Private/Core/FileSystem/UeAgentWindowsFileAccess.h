#pragma once

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"

namespace UE::UAI::FileSystem::Windows
{
    // Native relative opens reject reparse traversal in the kernel. Callers own
    // access/share policy and the lifetime/current-path proof of Parent.
    HANDLE OpenRelative(HANDLE Parent, const FString& Name, DWORD Access, DWORD Share,
        ULONG Disposition, ULONG Options);
    bool SetInformation(HANDLE File, void* Information, ULONG Size, ULONG Class);
    bool IsOrdinary(HANDLE File, bool bDirectory, DWORD* OutAttributes = nullptr);
}
#endif
