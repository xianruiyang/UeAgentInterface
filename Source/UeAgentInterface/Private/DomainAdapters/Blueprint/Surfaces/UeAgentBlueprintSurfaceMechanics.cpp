#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintSurfaceMechanics.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintCandidateBindingNormalizer.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringApplyProvider.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringExport.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringRegistration.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
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

		bool Fail(
			FWorkspaceError& OutError,
			FString Code,
			FString Source = FString(),
			FString Pointer = TEXT("/"))
		{
			OutError.Set(MoveTemp(Code), MoveTemp(Source), MoveTemp(Pointer));
			return false;
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
			static const TPair<bool, FString> Registration = []()
			{
				FString Error;
				const bool bRegistered = UeAgentBlueprintAuthoring::RegisterAuthoring(Error);
				return TPair<bool, FString>(bRegistered, MoveTemp(Error));
			}();
			if (!Registration.Key)
			{
				return Fail(OutError, TEXT("blueprint_surface_authoring_registration_failed"),
					Registration.Value);
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
			const TSharedPtr<FJsonObject>* DocumentField = nullptr;
			if (!Resolved.Root.IsValid()
				|| !Resolved.Root->TryGetObjectField(
					TEXT("document"), DocumentField)
				|| !DocumentField || !DocumentField->IsValid())
			{
				return Fail(
					OutError, TEXT("blueprint_surface_document_required"));
			}
			const TSharedPtr<FJsonObject> Document = *DocumentField;
			const FString& ExpectedRevision = Resolved.ExpectedRevision;
			FString Operation;
			const TSharedPtr<FJsonObject>* DocumentTarget = nullptr;
			FString AssetPath;
			if (!Document->TryGetStringField(TEXT("operation"), Operation)
				|| !Document->TryGetObjectField(TEXT("target"), DocumentTarget)
				|| !DocumentTarget || !DocumentTarget->IsValid()
				|| !(*DocumentTarget)->TryGetStringField(TEXT("asset_path"), AssetPath))
			{
				return Fail(OutError, TEXT("blueprint_surface_document_identity_required"));
			}
			Operation.TrimStartAndEndInline();
			Operation.ToLowerInline();
			if (Operation != TEXT("create")
				&& Operation != TEXT("update")
				&& Operation != TEXT("upsert"))
			{
				return Fail(OutError, TEXT("blueprint_surface_operation_unsupported"), Operation);
			}
			const FString TargetPackage = PackagePath(Target);
			if (PackagePath(AssetPath) != TargetPackage)
			{
				return Fail(OutError, TEXT("blueprint_surface_target_identity_mismatch"),
					AssetPath, Target);
			}

			UBlueprint* Existing = UeAgentBlueprintAssetLifecycle::LoadBlueprint(TargetPackage);
			if (Existing && Existing->GetClass() != UBlueprint::StaticClass())
			{
				return Fail(OutError, TEXT("blueprint_surface_target_type_unsupported"),
					Existing->GetClass()->GetPathName());
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
				return Fail(OutError, TEXT("blueprint_surface_stale_revision"),
					Target, CurrentRevision);
			}
			if ((Operation == TEXT("create") && Existing)
				|| (Operation == TEXT("update") && !Existing))
			{
				return Fail(OutError,
					Existing ? TEXT("blueprint_surface_target_already_exists")
						: TEXT("blueprint_surface_target_missing"), Target);
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
			if (!ProviderExtension.IsValid())
			{
				return Fail(OutError, TEXT("blueprint_surface_provider_extension_invalid"));
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
			if (!UeAgentBlueprintAuthoring::ApplyAssetInlineWorkspace(
				Document, TargetPackage, ApplyResult, RequestId))
			{
				return Fail(OutError,
					ApplyResult.ErrorCode.IsEmpty()
						? FString(TEXT("blueprint_surface_apply_failed"))
						: ApplyResult.ErrorCode,
					ApplyResult.Phase, ApplyResult.ErrorDetail);
			}

			UBlueprint* Applied = Cast<UBlueprint>(ApplyResult.Asset);
			UPackage* Package = Applied ? Applied->GetOutermost() : nullptr;
			if (!Package)
			{
				return Fail(OutError, TEXT("blueprint_surface_persisted_package_missing"), Target);
			}
			ApplyResult.Asset = nullptr;
			Applied = nullptr;
			FText UnloadError;
			if (!UPackageTools::UnloadPackages({Package}, UnloadError, true))
			{
				return Fail(OutError, TEXT("blueprint_surface_unload_failed"),
					Target, UnloadError.ToString());
			}
			UBlueprint* Reloaded = Cast<UBlueprint>(StaticLoadObject(
				UBlueprint::StaticClass(), nullptr, *ObjectPath(Target)));
			if (!Reloaded || Reloaded->GetClass() != UBlueprint::StaticClass())
			{
				return Fail(OutError, TEXT("blueprint_surface_reload_failed"), Target);
			}
			FString ReadbackPayload;
			FString Revision;
			if (!TryRead(Target, ReadbackPayload, Revision, OutError)) return false;
			TSharedPtr<FJsonObject> Readback;
			FString JsonError;
			if (!Json::TryParseObject(ReadbackPayload, Readback, JsonError)
				|| !Readback.IsValid())
			{
				return Fail(OutError, TEXT("blueprint_surface_readback_payload_invalid"),
					Target, JsonError);
			}
			FString CompileStatus;
			if (!Readback->TryGetStringField(
					TEXT("compile_status"), CompileStatus)
				|| CompileStatus != TEXT("valid"))
			{
				return Fail(
					OutError,
					TEXT("blueprint_surface_compile_not_settled"),
					Target,
					CompileStatus);
			}
			const TSharedPtr<FJsonObject> Mutation = MakeShared<FJsonObject>();
			Mutation->SetStringField(TEXT("operation"), ApplyResult.ResolvedOperation);
			Mutation->SetNumberField(TEXT("mutations_applied"), ApplyResult.MutationsApplied);
			Mutation->SetNumberField(TEXT("interfaces_added"), ApplyResult.Domain.InterfacesAdded);
			Mutation->SetNumberField(TEXT("interfaces_updated"), ApplyResult.Domain.InterfacesUpdated);
			Mutation->SetNumberField(TEXT("interfaces_removed"), ApplyResult.Domain.InterfacesRemoved);
			Mutation->SetNumberField(TEXT("variables_added"), ApplyResult.Domain.VariablesAdded);
			Mutation->SetNumberField(TEXT("components_added"), ApplyResult.Domain.ComponentsAdded);
			Mutation->SetNumberField(TEXT("graphs_added"), ApplyResult.Domain.GraphsAdded);
			Mutation->SetNumberField(TEXT("nodes_added"), ApplyResult.Domain.NodesAdded);

			FDomainHjsonImportLifecycleReceipt LifecycleReceipt;
			LifecycleReceipt.bMutationApplied = true;
			LifecycleReceipt.bCompileSettled = true;
			LifecycleReceipt.bPackageSaved = true;
			LifecycleReceipt.bUnloadedReloaded = true;
			LifecycleReceipt.PersistenceReceiptId = TEXT("reload:")
				+ ObjectPath(Target) + TEXT(":") + Revision;
			return TryBuildImportSuccessPayload(
				TEXT("blueprint-surface-domain-provider"),
				Target,
				Revision,
				LifecycleReceipt,
				Readback,
				Mutation,
				OutPayload,
				OutError);
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
				return Fail(OutError, TEXT("blueprint_surface_export_path_required"), Target);
			}
			UBlueprint* Blueprint = Cast<UBlueprint>(StaticLoadObject(
				UBlueprint::StaticClass(), nullptr, *ObjectPath(Target)));
			if (!Blueprint || Blueprint->GetClass() != UBlueprint::StaticClass())
			{
				return Fail(OutError, TEXT("blueprint_surface_target_unresolved"), Target);
			}
			if (!EnsureAuthoringRegistered(OutError)) return false;
			UeAgentExportWorkspaceWriter::FWorkspaceResult ExportResult;
			FString ExportError;
			if (!UeAgentBlueprintAuthoring::ExportAssetWorkspace(
				*Blueprint, WorkspacePath, TEXT("llm_authoring"),
				ExportResult, ExportError))
			{
				return Fail(OutError,
					ExportResult.ErrorCode.IsEmpty()
						? FString(TEXT("blueprint_surface_export_failed"))
						: ExportResult.ErrorCode,
					Target, ExportError.IsEmpty()
						? ExportResult.ErrorDetail : ExportError);
			}
			const TSharedPtr<FJsonObject> Result = ExportResult.ToJsonObject();
			Result->SetStringField(TEXT("target"), Target);
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
		TSharedPtr<FJsonObject> Params;
		FString JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			return Fail(OutError, TEXT("blueprint_surface_params_invalid"), JsonError);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| ObjectPath(Target) != ObjectPath(TargetIdentity))
		{
			return Fail(OutError, TEXT("blueprint_surface_target_identity_mismatch"),
				TargetIdentity, Target);
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(Target, Params, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.authoring_schema"))
		{
			FString ReadPayload;
			FString Revision;
			if (!TryRead(Target, ReadPayload, Revision, OutError))
			{
				return false;
			}
			const TSharedRef<FJsonObject> TemplateTarget = MakeShared<FJsonObject>();
			TemplateTarget->SetStringField(TEXT("asset_path"), Target);
			const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
			Document->SetStringField(TEXT("operation"), TEXT("update"));
			Document->SetObjectField(TEXT("target"), TemplateTarget);
			Document->SetObjectField(
				TEXT("provider_extension"), MakeShared<FJsonObject>());
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("type"), BlueprintType);
			Root->SetObjectField(TEXT("document"), Document);
			const TSharedRef<FJsonObject> Template = MakeShared<FJsonObject>();
			Template->SetStringField(TEXT("schema"), SchemaId);
			Template->SetObjectField(TEXT("root"), Root);
			const TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("schema_id"), SchemaId);
			Schema->SetStringField(TEXT("root_type"), BlueprintType);
			Schema->SetStringField(
				TEXT("provider"), TEXT("blueprint-surface-domain-provider"));
			Schema->SetStringField(TEXT("revision"), Revision);
			Schema->SetObjectField(TEXT("template"), Template);
			const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("command_id"), CommandId);
			Response->SetStringField(TEXT("target"), Target);
			Response->SetBoolField(TEXT("read_only"), true);
			Response->SetBoolField(TEXT("complete"), true);
			Response->SetBoolField(TEXT("has_more"), false);
			Response->SetObjectField(TEXT("query_result"), Schema);
			bOutReadOnly = true;
			return CanonicalObject(Response, OutPayloadJson, OutError);
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
		return Fail(OutError, TEXT("blueprint_surface_command_not_supported"), CommandId);
	}
}
