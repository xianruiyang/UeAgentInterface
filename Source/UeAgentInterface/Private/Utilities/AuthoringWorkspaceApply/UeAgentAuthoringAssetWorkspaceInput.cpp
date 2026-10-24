// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceInput.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

namespace UeAgentAuthoringAssetWorkspaceInput
{
	namespace
	{
		void SetFailure(
			FString& OutPhase,
			UE::UAI::Contract::FDiagnostic& OutDiagnostic,
			UE::UAI::Contract::FDiagnostic Diagnostic)
		{
			OutPhase = Diagnostic.Stage;
			OutDiagnostic = MoveTemp(Diagnostic);
		}

		FString OrFallback(const FString& Value, const TCHAR* Fallback)
		{
			return Value.IsEmpty() ? FString(Fallback) : Value;
		}
	}

	bool Resolve(const FRequest& Request, FResult& OutResult)
	{
		using namespace UeAgentAuthoringOperationApply;

		OutResult = FResult();
		if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(
			Request.WorkspaceContractId,
			OutResult.WorkspaceContract))
		{
			SetFailure(OutResult.Phase, OutResult.Diagnostic, UE::UAI::Contract::FDiagnostic::Failure(OrFallback(
				Request.Errors.ContractMissing, TEXT("asset_workspace_contract_missing")), TEXT("The workspace contract is not registered."), TEXT("registration")));
            OutResult.Diagnostic.CandidateName = Request.WorkspaceContractId;
            OutResult.Diagnostic.PropertyPath = TEXT("workspace_contract_id");
            OutResult.Diagnostic.ExpectedValuePresent = true;
            OutResult.Diagnostic.ActualValuePresent = false;
            return false;
        }

		if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(
			OutResult.WorkspaceContract,
			Request.Source,
			OutResult.Load))
		{
			OutResult.Phase = TEXT("load");
			OutResult.Diagnostic = OutResult.Load.Diagnostic;
			return false;
		}

		FTargetRequest TargetRequest;
		TargetRequest.UnitId = Request.UnitId;
		TargetRequest.ExpectedAssetPath = Request.ExpectedAssetPath;
		TargetRequest.ExpectedAssetKind = Request.ExpectedAssetKind;
		TargetRequest.Errors = Request.Errors;
		FTargetResult Target;
		if (!ResolveTarget(OutResult.Load.Workspace, TargetRequest, Target))
		{
			OutResult.Phase = Target.Phase;
			OutResult.Diagnostic = Target.Diagnostic;
			return false;
		}
		OutResult.InputContract = MoveTemp(Target.InputContract);
		OutResult.Input = MoveTemp(Target.Input);
		OutResult.CanonicalUnitKey = MoveTemp(Target.CanonicalUnitKey);
		OutResult.AssetPath = MoveTemp(Target.AssetPath);
		OutResult.SourceAssetPath = MoveTemp(Target.SourceAssetPath);
		OutResult.AssetKind = MoveTemp(Target.AssetKind);
		OutResult.Phase = TEXT("resolved");
		return true;
	}

	bool ResolveTarget(
		const UeAgentAuthoringWorkspaceRouter::FWorkspace& Workspace,
		const FTargetRequest& Request,
		FTargetResult& OutResult)
	{
		using namespace UeAgentAuthoringOperationApply;

		OutResult = FTargetResult();
		const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit* Unit =
			Workspace.FindSingleUnit(Request.UnitId);
        if (!Unit || !Unit->Document.IsValid()) {
            SetFailure(OutResult.Phase, OutResult.Diagnostic, UE::UAI::Contract::FDiagnostic::Failure(
                OrFallback(Request.Errors.EntryInvalid, TEXT("asset_workspace_entry_invalid")),
                !Unit ? TEXT("The requested entry does not resolve to exactly one workspace unit.") : TEXT("The requested workspace unit has no parsed document."), TEXT("preflight")));
            OutResult.Diagnostic.CandidateName = Request.UnitId;
            OutResult.Diagnostic.PropertyPath = !Unit ? TEXT("unit_id") : TEXT("document");
            if (!Unit) {
                const auto Matches = Workspace.FindUnits(Request.UnitId);
                OutResult.Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
                OutResult.Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(Matches.Num());
                if (Matches.Num() > 1) {
                    TArray<TSharedPtr<FJsonValue>> Choices;
                    for (const auto* Match : Matches) Choices.Add(MakeShared<FJsonValueString>(Match->RelativePath));
                    OutResult.Diagnostic.CandidateChoices = MakeShared<FJsonValueArray>(Choices);
                }
            } else {
                OutResult.Diagnostic.ExpectedValuePresent = true;
                OutResult.Diagnostic.ActualValuePresent = false;
                OutResult.Diagnostic.Source = Unit->ResolveDiagnosticSource();
            }
            return false;
        }
        ON_SCOPE_EXIT {
            if (!OutResult.Diagnostic.IsEmpty() && OutResult.Diagnostic.SourceKind != TEXT("native_result")
                && OutResult.Diagnostic.SourceKind != TEXT("command_input") && !OutResult.Diagnostic.Source.bCommandInput
                && OutResult.Diagnostic.Source.SourceFile.IsEmpty()) {
                const FString Pointer = OutResult.Diagnostic.Source.JsonPath.IsEmpty() ? OutResult.Diagnostic.PropertyPath : OutResult.Diagnostic.Source.JsonPath;
                OutResult.Diagnostic.Source = Unit->ResolveDiagnosticSource(Pointer);
            }
        };
        FOperationFailure InputFailure;
        if (!TryResolveWorkspaceOperationInput(*Unit, UeAgentApplyInputContract::FRegistry::Get(), OutResult.InputContract, OutResult.Input, InputFailure)) {
            OutResult.Phase = TEXT("preflight");
            OutResult.Diagnostic = InputFailure.Diagnostic;
            if (OutResult.Diagnostic.IsEmpty()) OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(
                OrFallback(Request.Errors.EntryInvalid, TEXT("asset_workspace_entry_invalid")), TEXT("Resolving the workspace operation failed without reporting a diagnostic."), TEXT("preflight"));
            return false;
        }
        const auto RejectAssetPath = [&](const TCHAR* Reason) {
            const TSharedPtr<FJsonValue> Value = OutResult.Input.Target ? OutResult.Input.Target->Values.FindRef(TEXT("asset_path")) : nullptr;
            SetFailure(OutResult.Phase, OutResult.Diagnostic, UeAgentAuthoringDiagnostics::MakeValueFailure(
                OrFallback(Request.Errors.EntryInvalid, TEXT("asset_workspace_entry_invalid")), TEXT("/target/asset_path"),
                MakeShared<FJsonValueString>(TEXT("nonempty asset package path")), Value, TEXT("preflight"), Reason));
            return false;
        };
        if (!OutResult.Input.Target.IsValid() || !OutResult.Input.Target->TryGetStringField(TEXT("asset_path"), OutResult.AssetPath))
            return RejectAssetPath(TEXT("The operation target requires an asset_path string."));
		OutResult.CanonicalUnitKey = Unit->CanonicalUnitKey;
		{
			OutResult.Input.Target->TryGetStringField(
				TEXT("source_asset_path"),
				OutResult.SourceAssetPath);
			OutResult.Input.Target->TryGetStringField(TEXT("asset_kind"), OutResult.AssetKind);
		}
		OutResult.AssetPath =
			UeAgentAuthoringAssetIdentity::NormalizePackagePath(OutResult.AssetPath);
        if (OutResult.AssetPath.IsEmpty()) return RejectAssetPath(TEXT("The asset path is empty after normalization."));
		OutResult.AssetKind.TrimStartAndEndInline();
		OutResult.AssetKind.ToLowerInline();

		FString ExpectedKind = Request.ExpectedAssetKind;
		ExpectedKind.TrimStartAndEndInline();
		ExpectedKind.ToLowerInline();
		if (!ExpectedKind.IsEmpty()
			&& !OutResult.AssetKind.IsEmpty()
			&& OutResult.AssetKind != ExpectedKind)
		{
            SetFailure(OutResult.Phase, OutResult.Diagnostic, UeAgentAuthoringDiagnostics::MakeValueFailure(
                OrFallback(Request.Errors.AssetKindConflict, TEXT("asset_workspace_asset_kind_conflict")), TEXT("/target/asset_kind"),
                MakeShared<FJsonValueString>(ExpectedKind), MakeShared<FJsonValueString>(OutResult.AssetKind), TEXT("preflight"), TEXT("The asset kind does not match the expected kind.")));
			return false;
		}

		const FString ExpectedPath =
			UeAgentAuthoringAssetIdentity::NormalizePackagePath(Request.ExpectedAssetPath);
		if (!ExpectedPath.IsEmpty()
			&& !OutResult.AssetPath.Equals(ExpectedPath, ESearchCase::IgnoreCase))
		{
            SetFailure(OutResult.Phase, OutResult.Diagnostic, UeAgentAuthoringDiagnostics::MakeValueFailure(
                OrFallback(Request.Errors.AssetPathConflict, TEXT("asset_workspace_asset_path_conflict")), TEXT("/target/asset_path"),
                MakeShared<FJsonValueString>(ExpectedPath), MakeShared<FJsonValueString>(OutResult.AssetPath), TEXT("preflight"), TEXT("The asset path does not match the expected path.")));
			return false;
		}

		OutResult.Phase = TEXT("resolved");
		return true;
	}
}
