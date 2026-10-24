#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintSurfaceMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintCandidateBindingNormalizer.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringApplyProvider.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringExport.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringRegistration.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintSemanticProjection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::Blueprint::Surfaces
{
	using namespace CommandContract;
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;

	namespace
	{
		const FString SchemaId = TEXT("uai.blueprint.edit/1");
		const FString BlueprintType = TEXT("/Script/Engine.Blueprint");

		bool Fail(FWorkspaceError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic,
			const FString& Target = FString())
		{
			if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Target;
			OutError.SetFailure(MoveTemp(Diagnostic));
			return false;
		}

		TSharedPtr<FJsonValue> FieldValue(const TSharedPtr<FJsonObject>& Object, const FString& Field)
		{
			if (!Object.IsValid()) return nullptr;
			const auto* Value = Object->Values.Find(Field);
			if (!Value || !Value->IsValid()) return nullptr;
			if ((*Value)->Type != EJson::Object && (*Value)->Type != EJson::Array) return *Value;
			auto Type = MakeShared<FJsonObject>();
			Type->SetStringField(TEXT("type"), (*Value)->Type == EJson::Object ? TEXT("object") : TEXT("array"));
			return MakeShared<FJsonValueObject>(Type);
		}

		UE::UAI::Contract::FDiagnostic Fact(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Path, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected, const FString& Kind = FString())
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
			Diagnostic.TargetPath = Target;
			Diagnostic.PropertyPath = Path;
			Diagnostic.SemanticPointer = Path;
			Diagnostic.SourceKind = Kind;
			if (Kind == TEXT("command_input")) Diagnostic.Source.JsonPath = Path;
			Diagnostic.ActualValuePresent = Actual.IsValid();
			Diagnostic.ActualValue = Actual.IsValid() ? MoveTemp(Actual) : MakeShared<FJsonValueNull>();
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			return Diagnostic;
		}

		FString PackagePath(FString Target)
		{
			Target.TrimStartAndEndInline();
			return Target.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Target)
				: Target;
		}

		FString ObjectPath(const FString& Target)
		{
			if (Target.Contains(TEXT("."))) return Target;
			const FString Name = FPackageName::GetLongPackageAssetName(Target);
			return Name.IsEmpty() ? Target : Target + TEXT(".") + Name;
		}

		bool CanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FString& Out,
			FWorkspaceError& OutError)
		{
			return TryCanonicalizeObject(Object, Out, OutError);
		}

		bool EnsureAuthoringRegistered(FWorkspaceError& OutError)
		{
			static const TPair<bool, UE::UAI::Contract::FDiagnostic> Registration = []()
			{
				UE::UAI::Contract::FDiagnostic Error;
				const bool bRegistered = UeAgentBlueprintAuthoring::RegisterAuthoring(Error);
				return TPair<bool, UE::UAI::Contract::FDiagnostic>(bRegistered, MoveTemp(Error));
			}();
			if (!Registration.Key)
			{
				return Fail(OutError, Registration.Value);
			}
			return true;
		}

		bool TryRead(
			const FString& Target,
			FString& OutPayload,
			FString& OutRevision,
			FWorkspaceError& OutError)
		{
			UObject* Object = nullptr;
			DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture Capture;
			if (!TryCaptureObject(
					Target, BlueprintType, Object, Capture, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Snapshot;
			if (!TryBuildCaptureSnapshot(
					Target, BlueprintType, Capture, Snapshot, OutError))
			{
				return false;
			}
			OutRevision = Capture.Revision;
			return TryCanonicalizeObject(Snapshot, OutPayload, OutError);
		}

		bool TryImportSemanticWorkspace(
			const FString& Target,
			const FResolvedDomainHjsonDocument& Resolved,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			if (!Resolved.SemanticWorkspace.IsValid())
			{
				return Fail(OutError, Fact(TEXT("blueprint_semantic_workspace_missing"), TEXT("The resolved Blueprint import has no semantic workspace."),
					Target, TEXT("/semantic_workspace/available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("native_result")));
			}
			if (!EnsureAuthoringRegistered(OutError)) return false;

			const FString TargetPackage = PackagePath(Target);
			UBlueprint* Blueprint = UeAgentBlueprintAssetLifecycle::LoadBlueprint(TargetPackage);
			if (!Blueprint || Blueprint->GetClass() != UBlueprint::StaticClass())
			{
				return Fail(OutError, Fact(TEXT("blueprint_surface_target_unresolved"),
					Blueprint ? TEXT("The semantic workspace target must be an exact UBlueprint asset.") : TEXT("The semantic workspace target Blueprint could not be loaded."),
					Target, Blueprint ? TEXT("/target/class") : TEXT("/target/available"),
					Blueprint ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Blueprint->GetClass()->GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
					Blueprint ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(BlueprintType)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)), TEXT("native_result")));
			}

			ContextResource::FUeAgentResourceTracker Resources =
				MakeSemanticWorkspaceResourceTracker();
			StateSemantic::FUeAgentCanonicalSnapshotPtr CanonicalSnapshot;
			FAuthoringModelPtr Model;
			{
				TArray<UeAgentAuthoringState::FReadRequest> Requests;
				UE::UAI::Contract::FDiagnostic CaptureError;
				if (!UeAgentBlueprintAuthoring::BuildAssetExportSnapshotReadRequests(
						*Blueprint, Requests, CaptureError))
				{
					return Fail(OutError, MoveTemp(CaptureError), Target);
				}
				UeAgentAuthoringState::FCaptureResult Capture;
				if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
				{
					return Fail(OutError, MoveTemp(Capture.Diagnostic), Target);
				}
				if (!TryBuildCanonicalAuthoringCapture(
						TEXT("ue.blueprint.asset"),
						TEXT("llm_authoring"),
						Capture.Snapshot,
						Resources,
						CanonicalSnapshot,
						Model,
					OutError))
				{
					return false;
				}
				Capture.Snapshot.ReleaseNativeValues();
			}

			const FString TargetObjectPath = ObjectPath(Target);
			return TryCoordinateSemanticWorkspaceImport(
				Resolved.SemanticWorkspace.ToSharedRef(), Model.ToSharedRef(),
				CanonicalSnapshot.ToSharedRef(),
				Contract::FUeAgentDescriptorIdentity(
					TEXT("blueprint.semantic_workspace"), 1,
					Model->GetSemanticFingerprint()),
				TargetObjectPath,
				Resources,
				{
					TEXT("blueprint-surface-domain-provider"),
					Target,
					TargetObjectPath,
					TEXT("blueprint-semantic"),
					TEXT("blueprint_semantic_prepare_failed"),
					TEXT("blueprint_semantic_apply_failed"),
					TEXT("blueprint_surface_readback_payload_invalid"),
					TEXT("blueprint_surface_compile_not_settled"),
					[TargetPackage](TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error)
					{
						UBlueprint* Native = UeAgentBlueprintAssetLifecycle::LoadBlueprint(TargetPackage);
						if (!Native)
						{
							Error = Fact(TEXT("blueprint_surface_target_unresolved"), TEXT("The Blueprint disappeared before semantic readback."),
								TargetPackage, TEXT("/target/available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("native_result"));
							return false;
						}
						return UeAgentBlueprintAuthoring::BuildAssetExportSnapshotReadRequests(*Native, Requests, Error);
					}},
				[Target,
					TargetPackage,
					SemanticWorkspace = Resolved.SemanticWorkspace.ToSharedRef(),
					CurrentModel = Model.ToSharedRef(),
					&Blueprint](
					const FString& RequestId,
					const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
					Composition::HjsonImport::FSemanticWorkspaceChangeProjection&& Projection,
					FRootPropertyApplyOutcome& OutOutcome)
				{
					(void)Prepared;
					TSharedPtr<FJsonObject> ProviderExtension;
					FWorkspaceError ProjectionError;
					if (!UeAgentBlueprintAuthoring::TryProjectSemanticProviderExtension(
							SemanticWorkspace->GetModel(), Projection.ChangedUnitIds,
							Projection.PropertyChangesByUnitId,
							Projection.StructuralEdits, ProviderExtension, ProjectionError, &SemanticWorkspace.Get(), &CurrentModel.Get()))
					{
						OutOutcome.Diagnostic = ProjectionError.GetDiagnostic();
						OutOutcome.Phase = TEXT("semantic_projection");
						return false;
					}
					if (!TryNormalizeBlueprintCandidateBindings(
							Target, ProviderExtension, ProjectionError))
					{
						OutOutcome.Diagnostic = ProjectionError.GetDiagnostic();
						OutOutcome.Phase = TEXT("semantic_projection");
						return false;
					}
					UeAgentBlueprintAuthoring::FAssetApplyRequest ApplyRequest;
					ApplyRequest.Operation =
						UeAgentApplyInputContract::EOperation::Update;
					ApplyRequest.AssetPath = TargetPackage;
					ApplyRequest.Properties = MoveTemp(Projection.RootObjectProperties);
					ApplyRequest.bCompactValues = true;
					ApplyRequest.FinalReadback = OutOutcome.FinalReadback;
					ApplyRequest.ProviderExtension = MoveTemp(ProviderExtension);
					ApplyRequest.ProviderExtension->SetBoolField(
						TEXT("compile"), true);
					ApplyRequest.ProviderExtension->SetBoolField(TEXT("save"), true);
					UeAgentBlueprintAuthoring::FAssetApplyResult ApplyResult;
					Blueprint = nullptr;
					const bool bApplied = UeAgentBlueprintAuthoring::ApplyAsset(
						ApplyRequest, ApplyResult, RequestId);
					OutOutcome.bApplied = bApplied && ApplyResult.IsApplied();
					OutOutcome.Phase = MoveTemp(ApplyResult.Phase);
					OutOutcome.Diagnostic = MoveTemp(ApplyResult.Diagnostic);
					ApplyResult.Asset = nullptr;
					Blueprint = nullptr;
					return bApplied;
				},
				[&Target](FString& OutReadbackPayload, FString& OutRevision,
					FWorkspaceError& OutReadError)
				{
					return TryRead(
						Target, OutReadbackPayload, OutRevision, OutReadError);
				},
				OutPayload,
				OutError);
		}

		bool TryImport(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FString CanonicalParamsJson;
			if (!TryCanonicalizeObject(Params, CanonicalParamsJson, OutError))
			{
				return false;
			}
			FResolvedDomainHjsonDocument Resolved;
			if (!TryDecodeResolvedDocument(
					Target,
					CanonicalParamsJson,
					FDomainHjsonDocumentContract(
						TEXT("blueprint"), SchemaId, BlueprintType),
					Resolved,
					OutError))
			{
				return false;
			}
			UE::UAI::HjsonWorkspace::DomainAuthoring::FScopedDomainHjsonDiagnosticSource DiagnosticSource(Resolved, OutError);
			if (Resolved.bSemanticWorkspaceDirectory)
			{
				return TryImportSemanticWorkspace(
					Target, Resolved, OutPayload, OutError);
			}
			const TSharedPtr<FJsonObject>* DocumentField = nullptr;
			if (!Resolved.Root.IsValid()
				|| !Resolved.Root->TryGetObjectField(
					TEXT("document"), DocumentField)
				|| !DocumentField || !DocumentField->IsValid())
			{
				return Fail(OutError, Fact(TEXT("blueprint_surface_document_required"), TEXT("The Blueprint import requires an object-valued document."),
					Target, TEXT("/document"), FieldValue(Resolved.Root, TEXT("document")), MakeShared<FJsonValueString>(TEXT("object"))));
			}
			const TSharedPtr<FJsonObject> Document = *DocumentField;
			const FString& ExpectedRevision = Resolved.ExpectedRevision;
			FString Operation;
			const TSharedPtr<FJsonObject>* DocumentTarget = nullptr;
			FString AssetPath;
			if (!Document->TryGetStringField(TEXT("operation"), Operation))
				return Fail(OutError, Fact(TEXT("blueprint_surface_document_identity_required"), TEXT("document.operation must be a string."),
					Target, TEXT("/document/operation"), FieldValue(Document, TEXT("operation")), MakeShared<FJsonValueString>(TEXT("string"))));
			if (!Document->TryGetObjectField(TEXT("target"), DocumentTarget) || !DocumentTarget || !DocumentTarget->IsValid())
				return Fail(OutError, Fact(TEXT("blueprint_surface_document_identity_required"), TEXT("document.target must be an object."),
					Target, TEXT("/document/target"), FieldValue(Document, TEXT("target")), MakeShared<FJsonValueString>(TEXT("object"))));
			if (!(*DocumentTarget)->TryGetStringField(TEXT("asset_path"), AssetPath))
				return Fail(OutError, Fact(TEXT("blueprint_surface_document_identity_required"), TEXT("document.target.asset_path must be a string."),
					Target, TEXT("/document/target/asset_path"), FieldValue(*DocumentTarget, TEXT("asset_path")), MakeShared<FJsonValueString>(TEXT("string"))));
			Operation.TrimStartAndEndInline();
			Operation.ToLowerInline();
			if (Operation != TEXT("create")
				&& Operation != TEXT("update")
				&& Operation != TEXT("upsert"))
			{
				return Fail(OutError, Fact(TEXT("blueprint_surface_operation_unsupported"), TEXT("The Blueprint document operation must be create, update or upsert."),
					Target, TEXT("/document/operation"), MakeShared<FJsonValueString>(Operation), MakeShared<FJsonValueString>(TEXT("create | update | upsert"))));
			}
			const FString TargetPackage = PackagePath(Target);
			if (PackagePath(AssetPath) != TargetPackage)
			{
				return Fail(OutError, Fact(TEXT("blueprint_surface_target_identity_mismatch"), TEXT("The document asset path resolves to a different package than the command target."),
					Target, TEXT("/document/target/asset_path"), MakeShared<FJsonValueString>(AssetPath), MakeShared<FJsonValueString>(TargetPackage)));
			}

			UBlueprint* Existing = UeAgentBlueprintAssetLifecycle::LoadBlueprint(TargetPackage);
			if (Existing && Existing->GetClass() != UBlueprint::StaticClass())
			{
				return Fail(OutError, Fact(TEXT("blueprint_surface_target_type_unsupported"), TEXT("The target asset must be an exact UBlueprint."),
					Target, TEXT("/target/class"), MakeShared<FJsonValueString>(Existing->GetClass()->GetPathName()), MakeShared<FJsonValueString>(BlueprintType), TEXT("native_result")));
			}
			FString BeforePayload;
			FString CurrentRevision = TEXT("missing");
			if (Existing
				&& !TryRead(Target, BeforePayload, CurrentRevision, OutError))
			{
				return false;
			}
			if (ExpectedRevision != CurrentRevision)
			{
				auto Diagnostic = Fact(TEXT("blueprint_surface_stale_revision"), TEXT("The expected revision does not match the target's current revision."),
					Target, TEXT("/expected_revision"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("command_input"));
				Diagnostic.PropertyPath = TEXT("/revision_matches_expected");
				Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(CurrentRevision);
				Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedRevision);
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			if ((Operation == TEXT("create") && Existing)
				|| (Operation == TEXT("update") && !Existing))
			{
				return Fail(OutError, Fact(Existing ? TEXT("blueprint_surface_target_already_exists") : TEXT("blueprint_surface_target_missing"),
					Existing ? TEXT("Create requires a missing Blueprint target, but the target already exists.") : TEXT("Update requires an existing Blueprint target, but the target is missing."),
					Target, TEXT("/target/exists"), MakeShared<FJsonValueBoolean>(Existing != nullptr), MakeShared<FJsonValueBoolean>(Existing == nullptr), TEXT("native_result")));
			}

			TSharedPtr<FJsonObject> ProviderExtension;
			const TSharedPtr<FJsonObject>* ExistingExtension = nullptr;
			if (Document->TryGetObjectField(
					TEXT("provider_extension"), ExistingExtension)
				&& ExistingExtension && ExistingExtension->IsValid())
			{
				ProviderExtension = *ExistingExtension;
			}
			else
			{
				ProviderExtension = MakeShared<FJsonObject>();
				Document->SetObjectField(
					TEXT("provider_extension"), ProviderExtension);
			}
			if (!TryNormalizeBlueprintCandidateBindings(
					Target, ProviderExtension, OutError))
			{
				return false;
			}
			ProviderExtension->SetBoolField(TEXT("compile"), true);
			ProviderExtension->SetBoolField(TEXT("save"), true);
			if (!EnsureAuthoringRegistered(OutError)) return false;

			UeAgentBlueprintAuthoring::FAssetApplyResult ApplyResult;
			const FString RequestId = TEXT("blueprint-surface-")
				+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			Existing = nullptr;
			auto FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
			{
				FWorkspaceError ReadbackError;
				if (!TryReadbackImportPayload(
					TEXT("blueprint-surface-domain-provider"), Target, ObjectPath(Target), nullptr,
					TEXT("blueprint_surface_readback_payload_invalid"), TEXT("blueprint_surface_compile_not_settled"),
					[&](FString& Payload, FString& Revision, FWorkspaceError& ReadError) { return TryRead(Target, Payload, Revision, ReadError); },
					OutPayload, ReadbackError))
				{
					Failure.Diagnostic = ReadbackError.GetDiagnostic();
					return false;
				}
				return true;
			};
			if (!UeAgentBlueprintAuthoring::ApplyAssetInlineWorkspace(
				Document, TargetPackage, ApplyResult, RequestId, MoveTemp(FinalReadback)))
			{
				return Fail(OutError, MoveTemp(ApplyResult.Diagnostic), Target);
			}

			ApplyResult.Asset = nullptr;
			return true;
		}

		bool TryExport(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FString WorkspacePath;
			if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
				|| WorkspacePath.IsEmpty())
			{
				return Fail(OutError, Fact(TEXT("blueprint_surface_export_path_required"), TEXT("Export requires a nonempty workspace_path string."),
					Target, TEXT("/workspace_path"), FieldValue(Params, TEXT("workspace_path")), MakeShared<FJsonValueString>(TEXT("nonempty string")), TEXT("command_input")));
			}
			UBlueprint* Blueprint = Cast<UBlueprint>(StaticLoadObject(
				UBlueprint::StaticClass(), nullptr, *ObjectPath(Target)));
			if (!Blueprint || Blueprint->GetClass() != UBlueprint::StaticClass())
			{
				return Fail(OutError, Fact(TEXT("blueprint_surface_target_unresolved"),
					Blueprint ? TEXT("The export target must be an exact UBlueprint asset.") : TEXT("The export target Blueprint could not be loaded."),
					Target, Blueprint ? TEXT("/target/class") : TEXT("/target/available"),
					Blueprint ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Blueprint->GetClass()->GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
					Blueprint ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(BlueprintType)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)), TEXT("native_result")));
			}
			if (!EnsureAuthoringRegistered(OutError)) return false;
			UeAgentExportWorkspaceWriter::FWorkspaceResult ExportResult;
			if (!UeAgentBlueprintAuthoring::ExportAssetWorkspace(*Blueprint, WorkspacePath, TEXT("llm_authoring"), ExportResult))
			{
				return Fail(OutError, MoveTemp(ExportResult.Diagnostic), Target);
			}
			const TSharedPtr<FJsonObject> Result = ExportResult.ToPublicJsonObject();
			return CanonicalObject(Result, OutPayload, OutError);
		}
	}

	bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		FWorkspaceError& OutError)
	{
		OutPayloadJson.Reset();
		bOutReadOnly = false;
		OutError.Reset();
		ON_SCOPE_EXIT
		{
			if (!OutError.HasErrors()) return;
			auto Diagnostic = OutError.GetDiagnostic();
			if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = TargetIdentity;
			OutError.SetFailure(MoveTemp(Diagnostic));
		};
		TSharedPtr<FJsonObject> Params;
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError))
		{
			return Fail(OutError, MoveTemp(JsonError), TargetIdentity);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| ObjectPath(Target) != ObjectPath(TargetIdentity))
		{
			return Fail(OutError, Fact(TEXT("blueprint_surface_target_identity_mismatch"), TEXT("The command target field must identify the selected Blueprint target."),
				TargetIdentity, TEXT("/target"), FieldValue(Params, TEXT("target")), MakeShared<FJsonValueString>(ObjectPath(TargetIdentity)), TEXT("command_input")));
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(Target, Params, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			bOutReadOnly = true;
			return TryExport(Target, Params, OutPayloadJson, OutError);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString Revision;
			bOutReadOnly = true;
			return TryRead(Target, OutPayloadJson, Revision, OutError);
		}
		return Fail(OutError, Fact(TEXT("blueprint_surface_command_not_supported"), TEXT("This Blueprint surface requires a content_graph command."),
			Target, TEXT("/command"), MakeShared<FJsonValueString>(CommandId), MakeShared<FJsonValueString>(TEXT("content_graph.*")), TEXT("command_input")));
	}
}
