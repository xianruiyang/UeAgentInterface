#include "Core/FileSystem/UeAgentWindowsFileAccess.h"

#if PLATFORM_WINDOWS
namespace UE::UAI::FileSystem::Windows
{
    namespace
    {
        struct FNativeName { USHORT Length, MaximumLength; PWSTR Buffer; };
        struct FNativeObjectAttributes {
            ULONG Length; HANDLE RootDirectory; FNativeName* ObjectName; ULONG Attributes;
            void* SecurityDescriptor; void* SecurityQualityOfService;
        };
        struct FNativeIoStatus { void* Status; ULONG_PTR Information; };
        using FCreateRelative = LONG (NTAPI*)(HANDLE*, ACCESS_MASK, FNativeObjectAttributes*, FNativeIoStatus*,
            LARGE_INTEGER*, ULONG, ULONG, ULONG, ULONG, void*, ULONG);
        using FSetRelative = LONG (NTAPI*)(HANDLE, FNativeIoStatus*, void*, ULONG, ULONG);
        using FStatusError = ULONG (NTAPI*)(LONG);
        bool NativeStatus(LONG Status)
        {
            if (Status >= 0) return true;
            static const auto Convert = reinterpret_cast<FStatusError>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
            SetLastError(Convert ? Convert(Status) : ERROR_NOT_SUPPORTED);
            return false;
        }
    }

    HANDLE OpenRelative(HANDLE Parent, const FString& Name, DWORD Access, DWORD Share, ULONG Disposition, ULONG Options)
    {
        static const auto Create = reinterpret_cast<FCreateRelative>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
        if (!Create || Name.IsEmpty() || Name.Len() > MAX_uint16 / sizeof(WCHAR) - 1) {
            SetLastError(ERROR_NOT_SUPPORTED); return INVALID_HANDLE_VALUE;
        }
        FNativeName NativeName{static_cast<USHORT>(Name.Len() * sizeof(WCHAR)),
            static_cast<USHORT>((Name.Len() + 1) * sizeof(WCHAR)), const_cast<PWSTR>(*Name)};
        FNativeObjectAttributes Attributes{sizeof(FNativeObjectAttributes), Parent, &NativeName,
            0x1040 /* OBJ_DONT_REPARSE | OBJ_CASE_INSENSITIVE */, nullptr, nullptr};
        FNativeIoStatus Io{}; HANDLE Result = nullptr;
        if (!NativeStatus(Create(&Result, Access | SYNCHRONIZE, &Attributes, &Io, nullptr,
            FILE_ATTRIBUTE_NORMAL, Share, Disposition,
            Options | 0x20 /* FILE_SYNCHRONOUS_IO_NONALERT */ | 0x200000 /* FILE_OPEN_REPARSE_POINT */,
            nullptr, 0))) return INVALID_HANDLE_VALUE;
        return Result;
    }

    bool SetInformation(HANDLE File, void* Information, ULONG Size, ULONG Class)
    {
        static const auto Set = reinterpret_cast<FSetRelative>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
        if (!Set) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
        FNativeIoStatus Io{};
        return NativeStatus(Set(File, &Io, Information, Size, Class));
    }

    bool IsOrdinary(HANDLE File, bool bDirectory, DWORD* OutAttributes)
    {
        FILE_ATTRIBUTE_TAG_INFO Attributes{};
        const bool bOrdinary = GetFileInformationByHandleEx(File, FileAttributeTagInfo, &Attributes, sizeof(Attributes))
            && !(Attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            && bool(Attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == bDirectory;
        if (OutAttributes) *OutAttributes = bOrdinary ? Attributes.FileAttributes : 0;
        return bOrdinary;
    }
}
#endif
