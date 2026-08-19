#include "DomainAdapters/UmgWidget/UeAgentUmgWidgetMechanics.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/Widget.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
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
	using namespace ContextResource;
	using namespace DomainAdapters::ContentGraphRead;
	using namespace HjsonWorkspace;
	using namespace StateSemantic;

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
				Fail(OutError, TEXT("umg_widget_hjson_node_invalid"));
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
				return Fail(OutError, TEXT("umg_widget_json_invalid"), Error);
			}
			return true;
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

		bool TryCapture(
			const FString& Target,
			FUeAgentContentGraphCapture& OutCapture,
			FWorkspaceError& OutError)
		{
			UObject* Object = StaticLoadObject(
				UObject::StaticClass(), nullptr, *ObjectPath(Target));
			UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(Object);
			if (!Blueprint)
			{
				return Fail(OutError, TEXT("umg_widget_target_unresolved"), Target);
			}
			if (Blueprint->GetClass() != UWidgetBlueprint::StaticClass())
			{
				return Fail(OutError, TEXT("umg_widget_target_type_unsupported"),
					Blueprint->GetClass()->GetPathName());
			}
			FUeAgentContentGraphReadError CaptureError;
			if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
					*Blueprint, OutCapture, CaptureError))
			{
				return Fail(OutError,
					CaptureError.Code.IsEmpty()
						? FString(TEXT("umg_widget_readback_failed"))
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
			Result->SetStringField(TEXT("root_type"), WidgetBlueprintType);
			Result->SetStringField(TEXT("domain_id"), TEXT("umg_widget"));
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
			if (UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Target))
			{
				if (Blueprint->WidgetTree)
				{
					Blueprint->WidgetTree->ForEachWidget(
						[&Items](UWidget* Widget)
						{
							if (!Widget || !Widget->Slot) return;
							const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
							Value->SetStringField(TEXT("id"),
								TEXT("slot:") + Widget->GetName());
							Value->SetStringField(TEXT("kind"), TEXT("slot"));
							Value->SetStringField(TEXT("class_path"),
								Widget->Slot->GetClass()->GetPathName());
							Value->SetStringField(TEXT("container_id"),
								TEXT("tree:WidgetTree"));
							Value->SetNumberField(TEXT("order"), Items.Num());
							const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
							State->SetStringField(TEXT("owner_widget"),
								TEXT("widget:") + Widget->GetName());
							State->SetStringField(TEXT("class"),
								Widget->Slot->GetClass()->GetPathName());
							Value->SetObjectField(TEXT("state"), State);
							Items.Add(MakeShared<FJsonValueObject>(Value));
						});
				}
				for (const FDelegateEditorBinding& Binding : Blueprint->Bindings)
				{
					const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("id"), FString::Printf(
						TEXT("binding:%s:%s"), *Binding.ObjectName,
						*Binding.PropertyName.ToString()));
					Value->SetStringField(TEXT("kind"), TEXT("binding"));
					Value->SetStringField(TEXT("class_path"), TEXT("uai.umg.binding"));
					Value->SetStringField(TEXT("container_id"),
						TEXT("tree:WidgetTree"));
					Value->SetNumberField(TEXT("order"), Items.Num());
					const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
					State->SetStringField(TEXT("widget_name"), Binding.ObjectName);
					State->SetStringField(TEXT("property_name"),
						Binding.PropertyName.ToString());
					State->SetStringField(TEXT("function_name"),
						Binding.FunctionName.ToString());
					State->SetStringField(TEXT("binding_kind"),
						Binding.Kind == EBindingKind::Property
							? TEXT("property_variable") : TEXT("function"));
					Value->SetObjectField(TEXT("state"), State);
					Items.Add(MakeShared<FJsonValueObject>(Value));
				}
				for (UWidgetAnimation* Animation : Blueprint->Animations)
				{
					if (!Animation) continue;
					const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("id"),
						TEXT("animation:") + Animation->GetName());
					Value->SetStringField(TEXT("kind"), TEXT("animation"));
					Value->SetStringField(TEXT("class_path"),
						Animation->GetClass()->GetPathName());
					Value->SetStringField(TEXT("container_id"),
						TEXT("tree:WidgetTree"));
					Value->SetNumberField(TEXT("order"), Items.Num());
					const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
					State->SetStringField(TEXT("name"), Animation->GetName());
					State->SetStringField(TEXT("display_label"),
						Animation->GetDisplayLabel());
					State->SetNumberField(TEXT("start_seconds"),
						Animation->GetStartTime());
					State->SetNumberField(TEXT("end_seconds"),
						Animation->GetEndTime());
					Value->SetObjectField(TEXT("state"), State);
					Items.Add(MakeShared<FJsonValueObject>(Value));
				}
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
					TEXT("umg_widget_legacy_source_contract_rejected"));
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
				return Fail(OutError, TEXT("umg_widget_resolved_source_invalid"));
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
					return Fail(OutError, TEXT("umg_widget_resolved_source_invalid"));
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
					TEXT("umg_widget_resolved_source_invalid"),
					EntryFile);
			}
			FUeAgentResourceTracker Resources = MakeResources();
			FHjsonDocumentPtr Parsed;
			if (!TryParseHjsonDocument(Text, TEXT("inline:umg-widget-domain"),
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
				|| Type != WidgetBlueprintType
				|| !(*DomainRoot)->TryGetObjectField(TEXT("document"), Document)
				|| !Document || !Document->IsValid())
			{
				return Fail(OutError, TEXT("umg_widget_schema_or_type_invalid"), Type);
			}
			OutDocument = *Document;
			return true;
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
			TSharedPtr<FJsonObject> Document;
			if (!ParseDocument(Params, Document, OutError)) return false;
			FString ExpectedRevision;
			if (!Params->TryGetStringField(TEXT("expected_revision"), ExpectedRevision)
				|| ExpectedRevision.IsEmpty())
			{
				return Fail(OutError, TEXT("umg_widget_expected_revision_required"));
			}

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

			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Result->SetStringField(TEXT("provider_id"), TEXT("umg-widget-domain-provider"));
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
