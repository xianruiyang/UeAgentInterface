#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintSurfaceMechanics.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringApplyProvider.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringExport.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringRegistration.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::Blueprint::Surfaces
{
	using namespace CommandContract;
	using namespace ContextResource;
	using namespace DomainAdapters::ContentGraphRead;
	using namespace HjsonWorkspace;
	using namespace StateSemantic;

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

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 65536},
				{EUeAgentResourceDimension::Items, 262144},
				{EUeAgentResourceDimension::Files, 4096},
				{EUeAgentResourceDimension::ReferenceEdges, 262144},
				{EUeAgentResourceDimension::WorkUnits, 1048576},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 512 * 1024 * 1024},
			}));
		}

		TSharedPtr<FJsonValue> JsonValueFromHjson(
			const FHjsonNode& Node,
			FWorkspaceError& OutError)
		{
			switch (Node.GetKind())
			{
			case EHjsonNodeKind::Null:
				return MakeShared<FJsonValueNull>();
			case EHjsonNodeKind::Boolean:
				return MakeShared<FJsonValueBoolean>(Node.GetBoolean());
			case EHjsonNodeKind::String:
				return MakeShared<FJsonValueString>(Node.GetText());
			case EHjsonNodeKind::Integer:
			case EHjsonNodeKind::Decimal:
				return MakeShared<FJsonValueNumber>(FCString::Atod(*Node.GetLexeme()));
			case EHjsonNodeKind::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const FHjsonNodeRef& Item : Node.GetItems())
				{
					TSharedPtr<FJsonValue> Converted = JsonValueFromHjson(*Item, OutError);
					if (!Converted.IsValid()) return nullptr;
					Items.Add(MoveTemp(Converted));
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Items));
			}
			case EHjsonNodeKind::Object:
			{
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				for (const TPair<FString, FHjsonNodeRef>& Entry : Node.GetEntries())
				{
					TSharedPtr<FJsonValue> Converted = JsonValueFromHjson(*Entry.Value, OutError);
					if (!Converted.IsValid()) return nullptr;
					Object->SetField(Entry.Key, MoveTemp(Converted));
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			default:
				Fail(OutError, TEXT("blueprint_surface_hjson_node_invalid"));
				return nullptr;
			}
		}

		bool CanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FString& Out,
			FWorkspaceError& OutError)
		{
			FString Error;
			if (!Object.IsValid() || !Json::TryCanonicalize(Object, Out, Error))
			{
				return Fail(OutError, TEXT("blueprint_surface_json_invalid"), Error);
			}
			return true;
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

		bool TryCapture(
			const FString& Target,
			FUeAgentContentGraphCapture& OutCapture,
			FWorkspaceError& OutError)
		{
			UObject* Object = StaticLoadObject(
				UObject::StaticClass(), nullptr, *ObjectPath(Target));
			UBlueprint* Blueprint = Cast<UBlueprint>(Object);
			if (!Blueprint)
			{
				return Fail(OutError, TEXT("blueprint_surface_target_unresolved"), Target);
			}
			if (Blueprint->GetClass() != UBlueprint::StaticClass())
			{
				return Fail(OutError, TEXT("blueprint_surface_target_type_unsupported"),
					Blueprint->GetClass()->GetPathName());
			}
			FUeAgentContentGraphReadError CaptureError;
			if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
				*Blueprint, OutCapture, CaptureError))
			{
				return Fail(OutError,
					CaptureError.Code.IsEmpty()
						? FString(TEXT("blueprint_surface_readback_failed"))
						: CaptureError.Code,
					CaptureError.SubjectId, CaptureError.Detail);
			}
			return true;
		}

		void AddCanonicalState(
			const FUeAgentCanonicalValuePtr& State,
			const TSharedPtr<FJsonObject>& Object)
		{
			if (!State.IsValid() || !Object.IsValid()) return;
			TSharedPtr<FJsonValue> JsonValue;
			FString Error;
			if (CanonicalJson::TryToJsonValue(*State, JsonValue, Error)
				&& JsonValue.IsValid())
			{
				Object->SetField(TEXT("state"), JsonValue);
			}
		}

		TSharedPtr<FJsonObject> CaptureJson(
			const FString& Target,
			const FUeAgentContentGraphCapture& Capture)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("target"), Target);
			Result->SetStringField(TEXT("root_type"), BlueprintType);
			Result->SetStringField(TEXT("domain_id"), Capture.DomainId);
			Result->SetStringField(TEXT("revision"), Capture.Revision);
			Result->SetBoolField(TEXT("complete"), true);
			Result->SetBoolField(TEXT("has_more"), false);

			TArray<TSharedPtr<FJsonValue>> Containers;
			for (const FUeAgentContentGraphContainer& Container : Capture.Containers)
			{
				const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("id"), Container.ContainerId);
				Value->SetStringField(TEXT("kind"), Container.ContainerKind);
				Value->SetStringField(TEXT("owner"), Container.OwnerId);
				Value->SetStringField(TEXT("parent"), Container.ParentContainerId);
				Value->SetNumberField(TEXT("order"), Container.Order);
				AddCanonicalState(Container.State, Value);
				Containers.Add(MakeShared<FJsonValueObject>(Value));
			}
			Result->SetArrayField(TEXT("containers"), MoveTemp(Containers));

			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FUeAgentContentGraphItem& Item : Capture.Items)
			{
				const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("id"), Item.ItemId);
				Value->SetStringField(TEXT("kind"), Item.ItemKind);
				Value->SetStringField(TEXT("class_path"), Item.ClassPath);
				Value->SetStringField(TEXT("container_id"), Item.ContainerId);
				Value->SetNumberField(TEXT("order"), Item.Order);
				AddCanonicalState(Item.State, Value);
				Items.Add(MakeShared<FJsonValueObject>(Value));
			}
			Result->SetArrayField(TEXT("items"), MoveTemp(Items));

			TArray<TSharedPtr<FJsonValue>> Relations;
			for (const FUeAgentContentGraphRelation& Relation : Capture.Relations)
			{
				const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("id"), Relation.RelationId);
				Value->SetStringField(TEXT("kind"), Relation.RelationKind);
				Value->SetStringField(TEXT("source"), Relation.SourceItemId);
				Value->SetStringField(TEXT("target"), Relation.TargetItemId);
				AddCanonicalState(Relation.State, Value);
				Relations.Add(MakeShared<FJsonValueObject>(Value));
			}
			Result->SetArrayField(TEXT("relations"), MoveTemp(Relations));

			TArray<TSharedPtr<FJsonValue>> Properties;
			for (const FUeAgentContentGraphProperty& Property : Capture.Properties)
			{
				const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("owner"), Property.OwnerId);
				Value->SetStringField(TEXT("field"), Property.FieldId);
				AddCanonicalState(Property.State.Value, Value);
				Properties.Add(MakeShared<FJsonValueObject>(Value));
			}
			Result->SetArrayField(TEXT("properties"), MoveTemp(Properties));
			Result->SetNumberField(TEXT("property_count"), Capture.Properties.Num());

			bool bCompileValid = true;
			TArray<TSharedPtr<FJsonValue>> Diagnostics;
			for (const FUeAgentContentGraphDiagnostic& Diagnostic : Capture.Diagnostics)
			{
				const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("severity"), Diagnostic.Severity);
				Value->SetStringField(TEXT("code"), Diagnostic.Code);
				Value->SetBoolField(TEXT("resolved"), Diagnostic.bResolved);
				Diagnostics.Add(MakeShared<FJsonValueObject>(Value));
				if (!Diagnostic.bResolved
					&& Diagnostic.Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
				{
					bCompileValid = false;
				}
			}
			Result->SetArrayField(TEXT("diagnostics"), MoveTemp(Diagnostics));
			Result->SetStringField(TEXT("compile_status"),
				bCompileValid ? TEXT("valid") : TEXT("error"));
			return Result;
		}

		bool TryRead(
			const FString& Target,
			FString& OutPayload,
			FString& OutRevision,
			FWorkspaceError& OutError)
		{
			FUeAgentContentGraphCapture Capture;
			if (!TryCapture(Target, Capture, OutError)) return false;
			OutRevision = Capture.Revision;
			return CanonicalObject(CaptureJson(Target, Capture), OutPayload, OutError);
		}

		bool ParseDocument(
			const TSharedPtr<FJsonObject>& Params,
			TSharedPtr<FJsonObject>& OutDocument,
			FWorkspaceError& OutError)
		{
			OutDocument.Reset();
			if (!Params.IsValid()
				|| Params->HasField(TEXT("carrier"))
				|| Params->HasField(TEXT("hjson_document")))
			{
				return Fail(
					OutError,
					TEXT("blueprint_surface_legacy_source_contract_rejected"));
			}
			FString Target;
			const TSharedPtr<FJsonObject>* Source = nullptr;
			FString Kind;
			FString TargetIdentity;
			FString EntryFile;
			const TArray<TSharedPtr<FJsonValue>>* Documents = nullptr;
			if (!Params->TryGetStringField(TEXT("target"), Target)
				|| Target.IsEmpty()
				|| !Params->TryGetObjectField(TEXT("source"), Source)
				|| !Source || !Source->IsValid()
				|| !(*Source)->TryGetStringField(TEXT("kind"), Kind)
				|| Kind != TEXT("resolved")
				|| !(*Source)->TryGetStringField(TEXT("target_identity"), TargetIdentity)
				|| TargetIdentity != Target
				|| !(*Source)->TryGetStringField(TEXT("entry_file"), EntryFile)
				|| EntryFile.IsEmpty()
				|| !(*Source)->TryGetArrayField(TEXT("documents"), Documents)
				|| !Documents || Documents->IsEmpty())
			{
				return Fail(OutError, TEXT("blueprint_surface_resolved_source_invalid"));
			}
			FString Text;
			int32 EntryMatches = 0;
			for (const TSharedPtr<FJsonValue>& Value : *Documents)
			{
				const TSharedPtr<FJsonObject> DocumentObject = Value.IsValid()
					? Value->AsObject() : nullptr;
				FString RelativePath;
				FString DocumentText;
				if (!DocumentObject.IsValid()
					|| !DocumentObject->TryGetStringField(TEXT("path"), RelativePath)
					|| !DocumentObject->TryGetStringField(TEXT("document"), DocumentText))
				{
					return Fail(OutError, TEXT("blueprint_surface_resolved_source_invalid"));
				}
				if (RelativePath == EntryFile)
				{
					Text = MoveTemp(DocumentText);
					++EntryMatches;
				}
			}
			if (EntryMatches != 1 || Text.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("blueprint_surface_resolved_source_invalid"),
					EntryFile);
			}
			FUeAgentResourceTracker Resources = MakeResources();
			FHjsonDocumentPtr Parsed;
			if (!TryParseHjsonDocument(Text, TEXT("inline:blueprint-surface-domain"),
				Resources, Parsed, OutError)
				|| !Parsed.IsValid())
			{
				return false;
			}
			TSharedPtr<FJsonValue> Converted = JsonValueFromHjson(*Parsed->GetRoot(), OutError);
			const TSharedPtr<FJsonObject> Root = Converted.IsValid()
				? Converted->AsObject() : nullptr;
			FString Schema;
			const TSharedPtr<FJsonObject>* DomainRoot = nullptr;
			const TSharedPtr<FJsonObject>* Document = nullptr;
			FString Type;
			if (!Root.IsValid()
				|| !Root->TryGetStringField(TEXT("schema"), Schema)
				|| Schema != SchemaId
				|| !Root->TryGetObjectField(TEXT("root"), DomainRoot)
				|| !DomainRoot || !DomainRoot->IsValid()
				|| !(*DomainRoot)->TryGetStringField(TEXT("type"), Type)
				|| Type != BlueprintType
				|| !(*DomainRoot)->TryGetObjectField(TEXT("document"), Document)
				|| !Document || !Document->IsValid())
			{
				return Fail(OutError, TEXT("blueprint_surface_schema_or_type_invalid"), Type);
			}
			OutDocument = *Document;
			return true;
		}

		bool TryImport(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			TSharedPtr<FJsonObject> Document;
			if (!ParseDocument(Params, Document, OutError)) return false;
			FString ExpectedRevision;
			if (!Params->TryGetStringField(TEXT("expected_revision"), ExpectedRevision)
				|| ExpectedRevision.IsEmpty())
			{
				return Fail(OutError, TEXT("blueprint_surface_expected_revision_required"));
			}
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

			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Result->SetStringField(TEXT("provider_id"),
				TEXT("blueprint-surface-domain-provider"));
			Result->SetStringField(TEXT("target"), Target);
			Result->SetStringField(TEXT("revision"), Revision);
			Result->SetBoolField(TEXT("saved"), true);
			Result->SetBoolField(TEXT("reloaded"), true);
			Result->SetObjectField(TEXT("independent_readback"), Readback);
			Result->SetObjectField(TEXT("mutation"), Mutation);
			return CanonicalObject(Result, OutPayload, OutError);
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
