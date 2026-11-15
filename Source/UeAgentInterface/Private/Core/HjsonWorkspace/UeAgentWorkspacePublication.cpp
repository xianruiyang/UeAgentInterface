#include "Core/HjsonWorkspace/UeAgentWorkspacePublication.h"

#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "String/LexFromString.h"
#include "Misc/Paths.h"

namespace UE::UAI::HjsonWorkspace
{
    const TCHAR* GetGenerationWorkspaceSchemaId() { return TEXT("uai.authoring.workspace/3"); }
    const TCHAR* GetDirectWorkspaceSchemaId() { return TEXT("uai.authoring.workspace/4"); }
    bool IsGenerationWorkspaceSchema(const FString& Schema)
    { return Schema == GetGenerationWorkspaceSchemaId() || Schema == GetDirectWorkspaceSchemaId(); }
    bool IsSupportedSemanticWorkspaceSchema(const FString& Schema)
    { return Schema == GetSemanticWorkspaceSchemaId() || IsGenerationWorkspaceSchema(Schema); }

    bool FailWorkspaceDataflowHandoff(FWorkspaceError& Error, const FString& Stage,
        const FString& Missing, const FString& SourceUnit, const FString& Actual,
        const FString& Expected, const FString& Producer)
    {
        auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("workspace_dataflow_handoff_missing"), FString::Printf(
            TEXT("Stage=%s; missing=%s; actual=%s; expected=%s; producer=%s. "
                "Implicit complete/legacy reconstruction is disabled in this development tree; "
                "this request stopped at the missing handoff."),
            *Stage, *Missing, *Actual, *Expected, *Producer), Stage);
        Error.SetFailure(MoveTemp(Diagnostic), SourceUnit);
        return false;
    }

    bool TryReadWorkspacePublication(const FHjsonNode& Manifest, const FString& Path,
        FWorkspacePublication& Out, FWorkspaceError& Error)
    {
        Out = {};
        const auto* Schema = Manifest.Get(TEXT("workspace_schema"));
        if (!Schema || Schema->GetKind() != EHjsonNodeKind::String || !IsSupportedSemanticWorkspaceSchema(Schema->GetText())) {
            Error.SetSourceFailure(TEXT("workspace_format_unsupported"), TEXT("The workspace is not a supported semantic workspace (/2, /3 or /4)."), Path);
            return false;
        }
        Out.bGenerationPublication = IsGenerationWorkspaceSchema(Schema->GetText());
        Out.bDirectDataflow = Schema->GetText() == GetDirectWorkspaceSchemaId();
        if (Out.bGenerationPublication) {
            const auto* Generation = Manifest.Get(TEXT("publication_generation"));
            const auto* State = Manifest.Get(TEXT("publication_state"));
            if (!Generation || Generation->GetKind() != EHjsonNodeKind::String || Generation->GetText().IsEmpty()
                || !State || State->GetKind() != EHjsonNodeKind::String) {
                Error.SetSourceFailure(TEXT("workspace_publication_invalid"), TEXT("A generation workspace requires a publication generation and state."), Path);
                return false;
            }
            Out.Generation = Generation->GetText(); Out.State = State->GetText();
            if (const auto* Organization = Manifest.Get(TEXT("machine_bucket_organization"));
                Organization && Organization->GetKind() == EHjsonNodeKind::String) {
                if (Organization->GetText() == TEXT("owner_scope_v1")) Out.ScopedMachineBuckets = true;
                else if (Organization->GetText() == TEXT("document_prefix_v1")) Out.ScopedMachineBuckets = false;
            }
            if (const auto* Count = Manifest.Get(TEXT("published_unit_count")); Count && Count->GetKind() == EHjsonNodeKind::Integer) {
                int32 Value = 0;
                if (LexTryParseString(Value, *Count->GetLexeme()) && Value >= 0) Out.PublishedUnitCount = Value;
            }
            if (const auto* Expanded = Manifest.Get(TEXT("expanded_author_documents"));
                Expanded && Expanded->GetKind() == EHjsonNodeKind::Array) {
                TArray<FString> Paths;
                bool bValid = true;
                for (const auto& Entry : Expanded->GetItems()) {
                    if (Entry->GetKind() != EHjsonNodeKind::String) { bValid = false; break; }
                    Paths.Add(Entry->GetText());
                }
                if (bValid) Out.ExpandedAuthorDocuments = MoveTemp(Paths);
            }
            if (Out.State != TEXT("committed")) {
                Error.SetSourceFailure(TEXT("workspace_publication_busy"), TEXT("The workspace publication is incomplete; retry after publication or recover its transaction before importing."), Path);
                return false;
            }
        }
        return true;
    }

    bool TryReadWorkspacePublication(const FString& Root, FWorkspacePublication& Out,
        FWorkspaceError& Error, FString* OutManifestText)
    {
        Out = {};
        const FString Path = Root / TEXT(".uai/workspace.hjson");
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *Path)) {
            Error.SetSourceFailure(TEXT("workspace_manifest_read_failed"), TEXT("Could not read the workspace publication metadata."), Path);
            return false;
        }
        auto Resources = MakeSemanticWorkspaceResourceTracker();
        FHjsonDocumentPtr Document;
        if (!TryParseHjsonDocument(Text, Path, Resources, Document, Error)
            || !TryReadWorkspacePublication(Document->GetRoot().Get(), Path, Out, Error)) return false;
        if (OutManifestText) *OutManifestText = MoveTemp(Text);
        return true;
    }

    bool TrySelectWorkspacePublication(const FString& Root, const FString& RequestedFormat,
        bool bInitialize, FWorkspacePublication& Out, FWorkspaceError& Error)
    {
        if (!RequestedFormat.IsEmpty() && RequestedFormat != TEXT("v2") && RequestedFormat != TEXT("v3") && RequestedFormat != TEXT("v4")) {
            Error.SetSourceFailure(TEXT("workspace_format_invalid"), TEXT("workspace_format must be v2, v3 or v4."), Root);
            return false;
        }
        if (!bInitialize) {
            if (!TryReadWorkspacePublication(Root, Out, Error)) return false;
        }
        // Format migration is a publication concern. The existing source-byte,
        // namespace and rename transaction preserves edits in this directory;
        // an old schema is not a reason to require another author workspace.
        Out.bGenerationPublication = RequestedFormat != TEXT("v2");
        Out.bDirectDataflow = RequestedFormat.IsEmpty() || RequestedFormat == TEXT("v4");
#if !PLATFORM_WINDOWS
        if (Out.bGenerationPublication) {
            Error.SetSourceFailure(TEXT("workspace_generation_backend_unavailable"),
                    TEXT("Generation publication requires the Windows file-proof backend. Use workspace_format=v2 on this platform."), Root);
            return false;
        }
#endif
        Out.Generation = Out.bGenerationPublication ? FGuid::NewGuid().ToString(EGuidFormats::Digits) : FString();
        Out.State = TEXT("committed");
        return true;
    }

    bool FWorkspacePublicationReadFence::Begin(const FString& Root, FWorkspaceError& Error)
    {
        FWorkspacePublication Publication;
        if (!TryReadWorkspacePublication(Root, Publication, Error, &ManifestText)) return false;
        RootPath = Root;
        bActive = Publication.bGenerationPublication;
        return true;
    }
    bool FWorkspacePublicationReadFence::End(FWorkspaceError& Error) const
    {
        if (!bActive) return true;
        FWorkspacePublication Publication; FString Current;
        if (!TryReadWorkspacePublication(RootPath, Publication, Error, &Current)) return false;
        if (!Publication.bGenerationPublication || Current != ManifestText) {
            Error.SetSourceFailure(TEXT("workspace_publication_changed"), TEXT("The workspace publication changed while it was being read; this read result cannot be used."), RootPath);
            return false;
        }
        return true;
    }
}
