#include "DomainAdapters/UmgWidget/UeAgentUmgWidgetMechanics.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringRegistration.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringApplyProvider.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringExport.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringRegistration.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"

namespace UE::UAI::DomainAdapters::UmgWidget
{
	using namespace CommandContract;
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;

	namespace
	{
		const FString SchemaId = TEXT("uai.umg.edit/1");
		const FString WidgetBlueprintType = TEXT("/Script/UMGEditor.WidgetBlueprint");

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
				const bool bRegistered =
					UeAgentBlueprintAuthoring::RegisterAuthoring(Error)
					&& UeAgentUmgAuthoring::RegisterAuthoring(Error);
				return TPair<bool, FString>(bRegistered, MoveTemp(Error));
			}();
			if (!Registration.Key)
			{
				return Fail(OutError, TEXT("umg_widget_authoring_registration_failed"),
					Registration.Value);
			}
			return true;
		}

		UWidgetBlueprint* LoadWidgetBlueprint(const FString& Target)
		{
			return Cast<UWidgetBlueprint>(StaticLoadObject(
				UWidgetBlueprint::StaticClass(), nullptr, *ObjectPath(Target)));
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
					Target, WidgetBlueprintType, Object, Capture, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Snapshot;
			if (!TryBuildCaptureSnapshot(
					Target, WidgetBlueprintType, Capture, Snapshot, OutError))
			{
				return false;
			}
			OutRevision = Capture.Revision;
			return TryCanonicalizeObject(Snapshot, OutPayload, OutError);
		}

		bool TryOperation(
			FString Operation,
			UeAgentApplyInputContract::EOperation& OutOperation)
		{
			Operation.TrimStartAndEndInline();
			Operation.ToLowerInline();
			if (Operation == TEXT("create"))
			{
				OutOperation = UeAgentApplyInputContract::EOperation::Create;
				return true;
			}
			if (Operation == TEXT("update"))
			{
				OutOperation = UeAgentApplyInputContract::EOperation::Update;
				return true;
			}
			if (Operation == TEXT("upsert"))
			{
				OutOperation = UeAgentApplyInputContract::EOperation::Upsert;
				return true;
			}
			return false;
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
						TEXT("umg"), SchemaId, WidgetBlueprintType),
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
				return Fail(OutError, TEXT("umg_widget_document_required"));
			}
			const TSharedPtr<FJsonObject> Document = *DocumentField;
			const FString& ExpectedRevision = Resolved.ExpectedRevision;

			FString OperationText;
			UeAgentApplyInputContract::EOperation Operation =
				UeAgentApplyInputContract::EOperation::Update;
			const TSharedPtr<FJsonObject>* DocumentTarget = nullptr;
			FString AssetPath;
			if (!Document->TryGetStringField(TEXT("operation"), OperationText)
				|| !TryOperation(OperationText, Operation)
				|| !Document->TryGetObjectField(TEXT("target"), DocumentTarget)
				|| !DocumentTarget || !DocumentTarget->IsValid()
				|| !(*DocumentTarget)->TryGetStringField(TEXT("asset_path"), AssetPath))
			{
				return Fail(OutError, TEXT("umg_widget_document_identity_required"));
			}
			const FString TargetPackage = PackagePath(Target);
			if (PackagePath(AssetPath) != TargetPackage)
			{
				return Fail(OutError, TEXT("umg_widget_target_identity_mismatch"),
					AssetPath, Target);
			}

			UObject* ExistingObject = StaticLoadObject(
				UObject::StaticClass(), nullptr, *ObjectPath(Target));
			UWidgetBlueprint* Existing = Cast<UWidgetBlueprint>(ExistingObject);
			if (ExistingObject && (!Existing
				|| Existing->GetClass() != UWidgetBlueprint::StaticClass()))
			{
				return Fail(OutError, TEXT("umg_widget_target_type_unsupported"),
					ExistingObject->GetClass()->GetPathName());
			}
			FString BeforePayload;
			FString CurrentRevision = TEXT("missing");
			if (Existing && !TryRead(Target, BeforePayload, CurrentRevision, OutError))
			{
				return false;
			}
			if (ExpectedRevision != CurrentRevision)
			{
				return Fail(OutError, TEXT("umg_widget_stale_revision"),
					Target, CurrentRevision);
			}
			if ((Operation == UeAgentApplyInputContract::EOperation::Create && Existing)
				|| (Operation == UeAgentApplyInputContract::EOperation::Update && !Existing))
			{
				return Fail(OutError,
					Existing ? TEXT("umg_widget_target_already_exists")
						: TEXT("umg_widget_target_missing"), Target);
			}

			UeAgentUmgAuthoring::FAssetApplyRequest ApplyRequest;
			ApplyRequest.Operation = Operation;
			ApplyRequest.AssetPath = TargetPackage;
			(*DocumentTarget)->TryGetStringField(
				TEXT("source_asset_path"), ApplyRequest.SourceAssetPath);
			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			if (Document->TryGetArrayField(TEXT("properties"), Properties) && Properties)
			{
				ApplyRequest.Properties = *Properties;
			}
			const TSharedPtr<FJsonObject>* ProviderExtension = nullptr;
			if (Document->TryGetObjectField(TEXT("provider_extension"), ProviderExtension)
				&& ProviderExtension && ProviderExtension->IsValid())
			{
				ApplyRequest.ProviderExtension = *ProviderExtension;
			}
			else
			{
				ApplyRequest.ProviderExtension = MakeShared<FJsonObject>();
			}
			ApplyRequest.ProviderExtension->SetBoolField(TEXT("compile"), true);
			ApplyRequest.ProviderExtension->SetBoolField(TEXT("save"), true);
			if (!EnsureAuthoringRegistered(OutError)) return false;

			UeAgentUmgAuthoring::FAssetApplyResult ApplyResult;
			const FString RequestId = TEXT("umg-widget-")
				+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			if (!UeAgentUmgAuthoring::ApplyAsset(ApplyRequest, ApplyResult, RequestId))
			{
				return Fail(OutError,
					ApplyResult.ErrorCode.IsEmpty()
						? FString(TEXT("umg_widget_apply_failed"))
						: ApplyResult.ErrorCode,
					ApplyResult.Phase, ApplyResult.ErrorDetail);
			}

			UWidgetBlueprint* Applied = ApplyResult.Asset;
			UPackage* Package = Applied ? Applied->GetOutermost() : nullptr;
			if (!Package)
			{
				return Fail(OutError, TEXT("umg_widget_persisted_package_missing"), Target);
			}
			ApplyResult.Asset = nullptr;
			Applied = nullptr;
			Existing = nullptr;
			ExistingObject = nullptr;
			FText UnloadError;
			if (!UPackageTools::UnloadPackages({Package}, UnloadError, true))
			{
				return Fail(OutError, TEXT("umg_widget_unload_failed"),
					Target, UnloadError.ToString());
			}
			UWidgetBlueprint* Reloaded = LoadWidgetBlueprint(Target);
			if (!Reloaded || Reloaded->GetClass() != UWidgetBlueprint::StaticClass())
			{
				return Fail(OutError, TEXT("umg_widget_reload_failed"), Target);
			}
			FString ReadbackPayload;
			FString Revision;
			if (!TryRead(Target, ReadbackPayload, Revision, OutError)) return false;
			TSharedPtr<FJsonObject> Readback;
			FString JsonError;
			if (!Json::TryParseObject(ReadbackPayload, Readback, JsonError)
				|| !Readback.IsValid())
			{
				return Fail(OutError, TEXT("umg_widget_readback_payload_invalid"),
					Target, JsonError);
			}
			FString CompileStatus;
			if (!Readback->TryGetStringField(
					TEXT("compile_status"), CompileStatus)
				|| CompileStatus != TEXT("valid"))
			{
				return Fail(
					OutError,
					TEXT("umg_widget_compile_not_settled"),
					Target,
					CompileStatus);
			}

			const TSharedPtr<FJsonObject> Mutation = MakeShared<FJsonObject>();
			Mutation->SetStringField(TEXT("operation"), ApplyResult.ResolvedOperation);
			Mutation->SetNumberField(TEXT("mutations_applied"), ApplyResult.MutationsApplied);
			Mutation->SetNumberField(TEXT("widgets_added"), ApplyResult.Domain.WidgetsAdded);
			Mutation->SetNumberField(TEXT("widgets_updated"), ApplyResult.Domain.WidgetsUpdated);
			Mutation->SetNumberField(TEXT("widgets_removed"), ApplyResult.Domain.WidgetsRemoved);
			Mutation->SetNumberField(TEXT("widget_properties_applied"), ApplyResult.Domain.WidgetPropertiesApplied);
			Mutation->SetNumberField(TEXT("slot_properties_applied"), ApplyResult.Domain.SlotPropertiesApplied);
			Mutation->SetNumberField(TEXT("bindings_added"), ApplyResult.Domain.BindingsAdded);
			Mutation->SetNumberField(TEXT("bindings_updated"), ApplyResult.Domain.BindingsUpdated);
			Mutation->SetNumberField(TEXT("animations_added"), ApplyResult.Domain.AnimationsAdded);
			Mutation->SetNumberField(TEXT("animations_updated"), ApplyResult.Domain.AnimationsUpdated);
			Mutation->SetNumberField(TEXT("animation_keys_applied"), ApplyResult.Domain.AnimationKeysApplied);

			FDomainHjsonImportLifecycleReceipt LifecycleReceipt;
			LifecycleReceipt.bMutationApplied = true;
			LifecycleReceipt.bCompileSettled = true;
			LifecycleReceipt.bPackageSaved = true;
			LifecycleReceipt.bUnloadedReloaded = true;
			LifecycleReceipt.PersistenceReceiptId = TEXT("reload:")
				+ ObjectPath(Target) + TEXT(":") + Revision;
			return TryBuildImportSuccessPayload(
				TEXT("umg-widget-domain-provider"),
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
				return Fail(OutError, TEXT("umg_widget_export_path_required"), Target);
			}
			UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Target);
			if (!Blueprint || Blueprint->GetClass() != UWidgetBlueprint::StaticClass())
			{
				return Fail(OutError, TEXT("umg_widget_target_unresolved"), Target);
			}
			if (!EnsureAuthoringRegistered(OutError)) return false;
			UeAgentExportWorkspaceWriter::FWorkspaceResult ExportResult;
			FString ExportError;
			if (!UeAgentUmgAuthoring::ExportAssetWorkspace(
					*Blueprint, WorkspacePath, TEXT("llm_authoring"),
					ExportResult, ExportError))
			{
				return Fail(OutError,
					ExportResult.ErrorCode.IsEmpty()
						? FString(TEXT("umg_widget_export_failed"))
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
			return Fail(OutError, TEXT("umg_widget_params_invalid"), JsonError);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| ObjectPath(Target) != ObjectPath(TargetIdentity))
		{
			return Fail(OutError, TEXT("umg_widget_target_identity_mismatch"),
				TargetIdentity, Target);
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
		return Fail(OutError, TEXT("umg_widget_command_not_supported"), CommandId);
	}
}
