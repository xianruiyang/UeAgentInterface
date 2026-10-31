#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

namespace UE::UAI::Fab
{
/** Keeps native entry indices until all paths have been validated. */
class FDownloadArchive final
{
public:
    FDownloadArchive();
    ~FDownloadArchive();
    bool Open(const FString& Filename, Contract::FDiagnostic& Error);
    const TArray<FString>& GetFiles() const;
    bool Extract(const FString& Directory, TFunctionRef<bool()> IsCancelled, Contract::FDiagnostic& Error);
private:
    class FState;
    TUniquePtr<FState> State;
};
}
