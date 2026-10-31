#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"

class IHttpRequest;

namespace UE::UAI::Fab
{
struct FDownloadedFile
{
    FString Relative;
    int64 Size = 0;
    FString Hash;
};

/** Local provenance, never containing credentials or download URLs. */
struct FDownloadRecord
{
    FString Name;
    FString Asset;
    FString Publisher;
    FString Format;
    FString Filename;
    FString ListingId;
    FString FileId;
    FString Kind;
    FString Directory;
    int64 ExpectedSize = 0;
    TArray<FDownloadedFile> Files;
};

FString DownloadRoot();
bool IsSafeRelativeFile(const FString& Name);
bool LoadDownload(const FString& Name, FDownloadRecord& Out, Contract::FDiagnostic& Error);
bool VerifyDownload(const FDownloadRecord& Record, Contract::FDiagnostic& Error, TFunctionRef<bool()> IsCancelled = [] { return false; });
bool VerifyDownloadCopy(const FString& Source, const FString& Destination, TFunctionRef<bool()> IsCancelled = [] { return false; });
TSharedRef<FJsonObject> ProjectDownload(const FDownloadRecord& Record);
TSharedRef<FJsonObject> ListDownloads(int64 Page = 1);

/** Owns only its native HTTP/BPS task and its isolated cache directory. */
class FDownload final : public TSharedFromThis<FDownload, ESPMode::ThreadSafe>
{
public:
    static TSharedRef<FDownload, ESPMode::ThreadSafe> Create(FDownloadRecord Record, TSharedPtr<FJsonObject> Authorization);
#if WITH_DEV_AUTOMATION_TESTS
    static TSharedRef<FDownload, ESPMode::ThreadSafe> CreateForTesting(FDownloadRecord Record,
        TSharedPtr<FJsonObject> Authorization, TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request);
#endif
    ~FDownload();
    void Start();
    bool Tick(bool CancelRequested);
    void CancelAndDrain();
    bool IsComplete() const;
    const Contract::FDiagnostic& GetError() const;
    const FDownloadRecord& GetRecord() const;
private:
    class FState;
    FDownload(FDownloadRecord Record, TSharedPtr<FJsonObject> Authorization);
    TUniquePtr<FState> State;
};
}
