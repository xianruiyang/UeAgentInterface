#pragma once

#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"

namespace UE::UAI::HjsonWorkspace
{
    /** Publication metadata is independent of asset/model/workspace revisions. */
    struct FWorkspacePublication
    {
        bool bGenerationPublication = false;
        bool bDirectDataflow = false;
        FString Generation;
        FString State;
        // Physical facts emitted by this publication's actual row writer.
        // Absence in an older header is unknown, not a formatter-cache miss
        // and not permission to guess where a document's rows were stored.
        TOptional<bool> ScopedMachineBuckets;
        TOptional<TArray<FString>> ExpandedAuthorDocuments;
        TOptional<int32> PublishedUnitCount;
    };

    const TCHAR* GetGenerationWorkspaceSchemaId();
    const TCHAR* GetDirectWorkspaceSchemaId();
    bool IsGenerationWorkspaceSchema(const FString& Schema);
    bool IsSupportedSemanticWorkspaceSchema(const FString& Schema);
    /** Development dataflow rule: missing local handoff material is an error,
     * never permission to invoke a complete legacy producer. Complete work is
     * selected only by an explicit input operation (full/initialization/domain).
     * All HJSON handoff boundaries use this diagnostic owner. */
    bool FailWorkspaceDataflowHandoff(FWorkspaceError& Error, const FString& Stage,
        const FString& Missing, const FString& SourceUnit, const FString& Actual,
        const FString& Expected, const FString& Producer);
    bool TryReadWorkspacePublication(const FHjsonNode& Manifest, const FString& SourceUnit,
        FWorkspacePublication& Out, FWorkspaceError& Error);
    bool TryReadWorkspacePublication(const FString& Root, FWorkspacePublication& Out,
        FWorkspaceError& Error, FString* OutManifestText = nullptr);
    bool TrySelectWorkspacePublication(const FString& Root, const FString& RequestedFormat,
        bool bInitialize, FWorkspacePublication& Out, FWorkspaceError& Error);

    /** Formal readers only return a /3 result from one committed generation. */
    class FWorkspacePublicationReadFence
    {
    public:
        bool Begin(const FString& Root, FWorkspaceError& Error);
        bool End(FWorkspaceError& Error) const;
    private:
        FString RootPath;
        FString ManifestText;
        bool bActive = false;
    };
}
