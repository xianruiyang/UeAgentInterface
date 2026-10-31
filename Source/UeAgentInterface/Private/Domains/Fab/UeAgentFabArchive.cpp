#include "Domains/Fab/UeAgentFabArchive.h"
#include "Domains/Fab/UeAgentFabDownload.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/Archive.h"

THIRD_PARTY_INCLUDES_START
#include <libzip/zip.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::Fab
{
class FDownloadArchive::FState
{
public:
    zip_t* Archive = nullptr;
    struct FEntry { FString Name; zip_uint64_t Index; };
    TArray<FEntry> Entries;
    TArray<FString> Files;
    ~FState() { if (Archive) zip_discard(Archive); }
};
namespace
{
bool Fail(Contract::FDiagnostic& Error, const TCHAR* Code, const FString& Reason, const FString& Target)
{
    Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("fab_import"));
    Error.TargetPath = Target;
    Error.SourceKind = TEXT("native_result");
    return false;
}
}
FDownloadArchive::FDownloadArchive() : State(MakeUnique<FState>()) {}
FDownloadArchive::~FDownloadArchive() = default;
bool FDownloadArchive::Open(const FString& Filename, Contract::FDiagnostic& Error)
{
    State = MakeUnique<FState>();
    zip_error_t NativeError; zip_error_init(&NativeError);
    ON_SCOPE_EXIT { zip_error_fini(&NativeError); };
#if PLATFORM_WINDOWS
    zip_source_t* Source = zip_source_win32w_create(*Filename, 0, -1, &NativeError);
#else
    zip_source_t* Source = zip_source_file_create(TCHAR_TO_UTF8(*Filename), 0, -1, &NativeError);
#endif
    if (!Source) return Fail(Error, TEXT("fab_archive_unreadable"), UTF8_TO_TCHAR(zip_error_strerror(&NativeError)), FPaths::GetCleanFilename(Filename));
    State->Archive = zip_open_from_source(Source, ZIP_RDONLY, &NativeError);
    if (!State->Archive)
    {
        zip_source_free(Source);
        return Fail(Error, TEXT("fab_archive_unsupported"), UTF8_TO_TCHAR(zip_error_strerror(&NativeError)), FPaths::GetCleanFilename(Filename));
    }
    const zip_int64_t Count = zip_get_num_entries(State->Archive, 0);
    if (Count < 0) return Fail(Error, TEXT("fab_archive_unsupported"), UTF8_TO_TCHAR(zip_strerror(State->Archive)), FPaths::GetCleanFilename(Filename));
    TSet<FString> FileKeys, DirectoryKeys;
    for (zip_int64_t Index = 0; Index < Count; ++Index)
    {
        zip_stat_t Info; zip_stat_init(&Info);
        if (zip_stat_index(State->Archive, Index, 0, &Info) != 0 || !(Info.valid & ZIP_STAT_NAME) || !Info.name)
            return Fail(Error, TEXT("fab_archive_unsupported"), TEXT("An archive entry has no readable filename."), FPaths::GetCleanFilename(Filename));
        const FString Name = UTF8_TO_TCHAR(Info.name);
        const bool bDirectory = Name.EndsWith(TEXT("/"));
        const FString Path = bDirectory ? Name.LeftChop(1) : Name;
        if (!IsSafeRelativeFile(Path))
            return Fail(Error, TEXT("fab_archive_path_invalid"), TEXT("Archive contains an unsafe relative path."), Name);
        const FString Key = Path.ToLower();
        if (bDirectory) { DirectoryKeys.Add(Key); continue; }
        if (FileKeys.Contains(Key))
            return Fail(Error, TEXT("fab_archive_path_invalid"), TEXT("Archive contains file paths that collide when extracted."), Name);
        FileKeys.Add(Key);
        State->Entries.Add({Name, static_cast<zip_uint64_t>(Index)});
        State->Files.Add(Name);
    }
    for (const FString& File : State->Files)
    {
        const FString Key = File.ToLower();
        if (DirectoryKeys.Contains(Key))
            return Fail(Error, TEXT("fab_archive_path_invalid"), TEXT("An archive path is both a file and a directory."), File);
        FString Parent = FPaths::GetPath(Key);
        while (!Parent.IsEmpty())
        {
            if (FileKeys.Contains(Parent))
                return Fail(Error, TEXT("fab_archive_path_invalid"), TEXT("An archive file is used as another entry's parent directory."), File);
            Parent = FPaths::GetPath(Parent);
        }
    }
    State->Files.Sort();
    return true;
}
const TArray<FString>& FDownloadArchive::GetFiles() const { return State->Files; }
bool FDownloadArchive::Extract(const FString& Directory, TFunctionRef<bool()> IsCancelled, Contract::FDiagnostic& Error)
{
    for (const auto& Entry : State->Entries)
    {
        if (IsCancelled()) return Fail(Error, TEXT("fab_cancelled"), TEXT("Import cancelled during private archive extraction."), Entry.Name);
        zip_file_t* Input = zip_fopen_index(State->Archive, Entry.Index, 0);
        if (!Input) return Fail(Error, TEXT("fab_archive_extract_failed"), UTF8_TO_TCHAR(zip_strerror(State->Archive)), Entry.Name);
        ON_SCOPE_EXIT { zip_fclose(Input); };
        const FString Output = Directory / Entry.Name;
        if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Output), true))
            return Fail(Error, TEXT("fab_archive_extract_failed"), TEXT("Could not create the entry's private extraction directory."), Entry.Name);
        TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*Output));
        if (!Writer) return Fail(Error, TEXT("fab_archive_extract_failed"), TEXT("Could not create the private extracted file."), Entry.Name);
        uint8 Buffer[64 * 1024];
        for (;;)
        {
            if (IsCancelled()) return Fail(Error, TEXT("fab_cancelled"), TEXT("Import cancelled during private archive extraction."), Entry.Name);
            const zip_int64_t Bytes = zip_fread(Input, Buffer, sizeof(Buffer));
            if (Bytes < 0) return Fail(Error, TEXT("fab_archive_extract_failed"), UTF8_TO_TCHAR(zip_file_strerror(Input)), Entry.Name);
            if (Bytes == 0) break;
            Writer->Serialize(Buffer, Bytes);
            if (Writer->IsError()) return Fail(Error, TEXT("fab_archive_extract_failed"), TEXT("Could not write the private extracted file."), Entry.Name);
        }
        if (!Writer->Close()) return Fail(Error, TEXT("fab_archive_extract_failed"), TEXT("Could not finish writing the private extracted file."), Entry.Name);
    }
    return true;
}
}
