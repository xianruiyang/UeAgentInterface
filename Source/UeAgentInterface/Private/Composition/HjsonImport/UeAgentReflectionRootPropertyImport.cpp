#include "Composition/HjsonImport/UeAgentReflectionRootPropertyImport.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringAssets.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringState.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringExport.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"
#include "Misc/PackageName.h"
#include "UObject/Class.h"
#include "UObject/Object.h"

namespace UE::UAI::Composition::HjsonImport
{
using namespace HjsonWorkspace;
using namespace HjsonWorkspace::DomainAuthoring;
using namespace DomainAdapters::ContentGraphRead;
namespace
{
UE::UAI::Contract::FDiagnostic AssetMissing(const UeAgentReflectionAuthoring::FProfile& Profile, const FString& Target)
{
    auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("reflection_asset_missing"),
        TEXT("No asset exists at the target required for this reflection workspace operation."), TEXT("semantic_import"));
    Error.SourceKind = TEXT("native_result");
    Error.TargetPath = Target;
    Error.SubjectId = Profile.TypeId;
    Error.PropertyPath = TEXT("asset_present");
    Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
    Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
    return Error;
}
FString PackageIdentity(const FString& Target)
{ return Target.Contains(TEXT(".")) ? FPackageName::ObjectPathToPackageName(Target) : Target; }
}
bool TryExportReflectionRootWorkspace(const UeAgentReflectionAuthoring::FProfile& Profile,
 const FString& Target, const FString& WorkspacePath, FString& OutPayload, FWorkspaceError& OutError,
 FPreparedWorkspaceExport* PreparedExport)
{
	UE::UAI::Contract::FDiagnostic LoadError;
	UObject* Asset = UeAgentReflectionAuthoring::LoadAsset(Profile, PackageIdentity(Target), LoadError);
	if (!Asset)
	{
		OutError.SetFailure(LoadError.IsEmpty() ? AssetMissing(Profile, Target) : MoveTemp(LoadError), Target);
		return false;
	}
	UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
	if (!UeAgentReflectionAuthoring::ExportAssetWorkspace(Profile, *Asset, WorkspacePath, TEXT("llm_authoring"), Result, PreparedExport))
		{ OutError.SetFailure(Result.Diagnostic, Target); return false; }
	return TryCanonicalizeObject(Result.ToPublicJsonObject(), OutPayload, OutError);
}
bool TryImportReflectionWorkspace(const UeAgentReflectionAuthoring::FProfile& Profile,
 const FResolvedSemanticWorkspaceRef& Workspace, const FRootPropertyImportContract& ImportContract,
 TFunctionRef<void(TArray<TSharedPtr<FJsonValue>>&)> PrepareProperties,
 FString& OutPayload, FWorkspaceError& OutError)
{
 const FString& Target = ImportContract.TargetIdentity;
	UE::UAI::Contract::FDiagnostic LoadError;
	UObject* Asset = UeAgentReflectionAuthoring::LoadAsset(Profile, PackageIdentity(Target), LoadError);
	if (!Asset)
	{
		OutError.SetFailure(LoadError.IsEmpty() ? AssetMissing(Profile, Target) : MoveTemp(LoadError), Target);
		return false;
	}
	// Profiles may name a base class. Resolve membership through the same owner
	// as export, then capture the concrete identity selected by the workspace.
	const FString RootType = Asset->GetClass()->GetPathName();
	TArray<UeAgentAuthoringState::FReadRequest> Requests;
	UE::UAI::Contract::FDiagnostic CaptureError;
	if (!UeAgentReflectionAuthoring::BuildAssetExportSnapshotReadRequests(
		Profile, *Asset, Requests, CaptureError))
		{ OutError.SetFailure(CaptureError, Target); return false; }
	SelectPropertyImportReadRequests(Workspace.Get(), Requests, Profile.ExportTargetSemanticId);
	UeAgentAuthoringState::FCaptureResult Capture;
	if (!CapturePropertyImportState(Workspace.Get(), Requests, Capture, Profile.ExportTargetSemanticId))
		{ OutError.SetFailure(Capture.Diagnostic, Target); return false; }
	auto Resources = MakeSemanticWorkspaceResourceTracker();
	StateSemantic::FUeAgentCanonicalSnapshotPtr CanonicalSnapshot;
	FAuthoringModelPtr Model;
	if (!TryBuildPropertyImportCurrentCapture(&Workspace.Get(),
		ResolveTargetCompositionKind(Profile.TargetKind, Profile.TypeId),
		TEXT("llm_authoring"), Capture.Snapshot, Resources,
		CanonicalSnapshot, Model, OutError)
		|| !CanonicalSnapshot.IsValid() || !Model.IsValid()) return false;
	Capture.Snapshot.ReleaseNativeValues();
	const FString TargetPackage = PackageIdentity(Target);
	const FString TargetObjectPath = ObjectPathFromPackageIdentity(Target);
	FRootPropertyImportContract FinalContract = ImportContract;
	FinalContract.BuildFinalReadRequests = [&Profile, TargetPackage](TArray<UeAgentAuthoringState::FReadRequest>& ReadRequests, UE::UAI::Contract::FDiagnostic& Error)
	{
		UObject* Native = UeAgentReflectionAuthoring::LoadAsset(Profile, TargetPackage, Error);
		if (!Native)
		{
			if (Error.IsEmpty()) Error = AssetMissing(Profile, TargetPackage);
			return false;
		}
		return UeAgentReflectionAuthoring::BuildAssetExportSnapshotReadRequests(Profile, *Native, ReadRequests, Error);
	};
	return TryCoordinateSemanticWorkspaceImport(
		Workspace, Model.ToSharedRef(),
		CanonicalSnapshot.ToSharedRef(),
		BuildSemanticWorkspaceImportDescriptor(ImportContract.ProviderId + TEXT(".semantic_workspace"), *Model),
		TargetObjectPath, Resources,
		FinalContract,
		[&Profile, TargetPackage, &Asset, &PrepareProperties](const FString& RequestId,
			const FPreparedAuthoringImport& Prepared,
			FSemanticWorkspaceChangeProjection&& Projection,
			FRootPropertyApplyOutcome& Outcome)
		{
			PrepareProperties(Projection.RootObjectProperties);
			UeAgentReflectionAuthoring::FAssetApplyRequest Request;
			Request.TypeId = Profile.TypeId;
			Request.Operation = UeAgentApplyInputContract::EOperation::Update;
			Request.AssetPath = TargetPackage;
			Request.Properties = MoveTemp(Projection.RootObjectProperties);
			for (const auto& Change : Prepared.GetChangeSetDraft().GetChanges())
			{
				if (Change.GetIdentity().GetSemanticId() == TEXT("core.property")) continue;
				const bool bGenerationInput = !Profile.GenerationInputSemanticId.IsEmpty()
					&& Change.GetIdentity().GetSemanticId() == Profile.GenerationInputSemanticId;
				if (!bGenerationInput && (Profile.ProfileStateSemanticId.IsEmpty()
					|| Change.GetIdentity().GetSemanticId() != Profile.ProfileStateSemanticId))
				{
					Outcome.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("reflection_semantic_surface_not_supported"),
						FString::Printf(TEXT("The reflected asset does not support semantic surface '%s'."), *Change.GetIdentity().GetSemanticId()), TEXT("semantic_projection"));
					Outcome.Diagnostic.SemanticId = Change.GetIdentity().GetSemanticId();
					Outcome.Phase = TEXT("semantic_projection");
					return false;
				}
				TSharedPtr<FJsonValue> Value;
				if (!Change.GetProposedAfter().IsValid()
					|| !StateSemantic::CanonicalJson::TryToJsonValue(*Change.GetProposedAfter(), Value, Outcome.Diagnostic))
				{
					if (Outcome.Diagnostic.IsEmpty()) Outcome.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(
						TEXT("reflection_profile_state_projection_failed"), TEXT("The proposed profile state is missing."), TEXT("semantic_projection"));
					Outcome.Phase = TEXT("semantic_projection");
					return false;
				}
				if (!Request.ProviderExtension.IsValid()) Request.ProviderExtension = MakeShared<FJsonObject>();
				Request.ProviderExtension->SetField(bGenerationInput ? Profile.GenerationInputField : Profile.ProfileStateField, Value);
			}
			Request.bSave = true;
			Request.bCompactValues = true;
			Request.bEmitEditorNotifications = true;
			Request.FinalReadback = Outcome.FinalReadback;
			Asset = nullptr;
			UeAgentReflectionAuthoring::FAssetApplyResult Result;
			const bool bApplied = UeAgentReflectionAuthoring::ApplyAsset(
				Request, Result, RequestId);
			Outcome.bApplied = bApplied && Result.Status == TEXT("applied");
			Outcome.Phase = MoveTemp(Result.Phase);
			Outcome.Diagnostic = MoveTemp(Result.Diagnostic);
			return bApplied;
		},
		[&Target, &RootType](FString& Payload, FString& Revision, FWorkspaceError& Error)
		{
			TSharedPtr<FJsonObject> Snapshot;
			UObject* Owner = nullptr;
			FUeAgentContentGraphCapture ReadCapture;
			if (!TryCaptureObject(Target, RootType, Owner, ReadCapture, Error)) return false;
			if (!TryBuildCaptureSnapshot(Target, RootType, ReadCapture, Snapshot, Error)
				|| !TryCanonicalizeObject(Snapshot, Payload, Error)) return false;
			Revision = ReadCapture.Revision;
			return true;
		}, OutPayload, OutError);
}
}
