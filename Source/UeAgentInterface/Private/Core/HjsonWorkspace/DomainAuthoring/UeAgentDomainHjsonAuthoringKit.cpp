#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::HjsonWorkspace::DomainAuthoring
{
	using namespace ContextResource;
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		bool Fail(
			FWorkspaceError& OutError,
			FString Code,
			FString Source = FString(),
			FString Pointer = TEXT("/"))
		{
			OutError.Set(MoveTemp(Code), MoveTemp(Source), MoveTemp(Pointer));
			return false;
		}

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 65536},
				{EUeAgentResourceDimension::Items, 65536},
				{EUeAgentResourceDimension::Files, 4096},
				{EUeAgentResourceDimension::ReferenceEdges, 65536},
				{EUeAgentResourceDimension::WorkUnits, 262144},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					512ll * 1024 * 1024},
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
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const FHjsonNodeRef& Item : Node.GetItems())
				{
					TSharedPtr<FJsonValue> Value = JsonValueFromHjson(*Item, OutError);
					if (!Value.IsValid()) return nullptr;
					Values.Add(MoveTemp(Value));
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Values));
			}
			case EHjsonNodeKind::Object:
			{
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				for (const TPair<FString, FHjsonNodeRef>& Entry : Node.GetEntries())
				{
					TSharedPtr<FJsonValue> Value =
						JsonValueFromHjson(*Entry.Value, OutError);
					if (!Value.IsValid()) return nullptr;
					Object->SetField(Entry.Key, MoveTemp(Value));
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			default:
				Fail(OutError, TEXT("domain_hjson_node_kind_invalid"));
				return nullptr;
			}
		}

		void AddFieldState(
			const FUeAgentContentGraphFieldState& State,
			const TSharedPtr<FJsonObject>& Value)
		{
			FString ConversionError;
			TSharedPtr<FJsonValue> StateValue;
			if (State.Value.IsValid()
				&& StateSemantic::CanonicalJson::TryToJsonValue(
					*State.Value, StateValue, ConversionError)
				&& StateValue.IsValid())
			{
				Value->SetField(TEXT("value"), StateValue);
			}
			const TCHAR* Override = TEXT("none");
			if (State.OverrideState == StateSemantic::EUeAgentOverrideState::Inherited)
			{
				Override = TEXT("inherited");
			}
			else if (State.OverrideState
				== StateSemantic::EUeAgentOverrideState::Overridden)
			{
				Override = TEXT("overridden");
			}
			Value->SetStringField(TEXT("override_state"), Override);
			Value->SetBoolField(
				TEXT("authored"),
				State.AuthoredPresence
					== StateSemantic::EUeAgentAuthoredPresence::Present);
		}
	}

	FDomainHjsonDocumentContract::FDomainHjsonDocumentContract(
		FString InDomainId,
		FString InSchemaId,
		FString InRootTypeIdentity)
		: DomainId(MoveTemp(InDomainId))
		, SchemaId(MoveTemp(InSchemaId))
		, RootTypeIdentity(MoveTemp(InRootTypeIdentity))
	{
	}

	bool FDomainHjsonDocumentContract::IsValid() const
	{
		return !DomainId.IsEmpty()
			&& !SchemaId.IsEmpty()
			&& !RootTypeIdentity.IsEmpty();
	}

	bool TryDecodeResolvedDocument(
		const FString& RequestTargetIdentity,
		const FString& CanonicalParamsJson,
		const FDomainHjsonDocumentContract& Contract,
		FResolvedDomainHjsonDocument& OutDocument,
		FWorkspaceError& OutError)
	{
		OutDocument = {};
		OutError.Reset();
		if (!Contract.IsValid())
		{
			return Fail(OutError, TEXT("domain_hjson_contract_invalid"));
		}
		TSharedPtr<FJsonObject> Params;
		FString JsonError;
		if (!CommandContract::Json::TryParseObject(
			CanonicalParamsJson, Params, JsonError) || !Params.IsValid())
		{
			return Fail(
				OutError, TEXT("domain_hjson_params_invalid"), JsonError);
		}
		if (Params->HasField(TEXT("carrier"))
			|| Params->HasField(TEXT("hjson_document")))
		{
			return Fail(
				OutError, TEXT("domain_hjson_legacy_source_contract_rejected"));
		}
		FString Target;
		FString ExpectedRevision;
		const TSharedPtr<FJsonObject>* Source = nullptr;
		FString Kind;
		FString SourceTarget;
		FString EntryFile;
		const TArray<TSharedPtr<FJsonValue>>* Documents = nullptr;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| Target.IsEmpty()
			|| Target != RequestTargetIdentity
			|| !Params->TryGetStringField(
				TEXT("expected_revision"), ExpectedRevision)
			|| ExpectedRevision.IsEmpty())
		{
			return Fail(
				OutError, TEXT("domain_hjson_target_or_revision_invalid"));
		}
		if (!Params->TryGetObjectField(TEXT("source"), Source)
			|| !Source || !Source->IsValid()
			|| !(*Source)->TryGetStringField(TEXT("kind"), Kind)
			|| Kind != TEXT("resolved")
			|| !(*Source)->TryGetStringField(TEXT("target_identity"), SourceTarget)
			|| !(*Source)->TryGetStringField(TEXT("entry_file"), EntryFile)
			|| EntryFile.IsEmpty()
			|| !(*Source)->TryGetArrayField(TEXT("documents"), Documents)
			|| !Documents || Documents->IsEmpty())
		{
			return Fail(OutError, TEXT("domain_hjson_resolved_source_invalid"));
		}
		if (SourceTarget != Target)
		{
			return Fail(
				OutError, TEXT("domain_hjson_target_identity_mismatch"), SourceTarget);
		}
		FString DocumentText;
		int32 EntryMatches = 0;
		for (const TSharedPtr<FJsonValue>& Value : *Documents)
		{
			const TSharedPtr<FJsonObject> Document =
				Value.IsValid() ? Value->AsObject() : nullptr;
			FString Path;
			FString Text;
			if (!Document.IsValid()
				|| !Document->TryGetStringField(TEXT("path"), Path)
				|| !Document->TryGetStringField(TEXT("document"), Text))
			{
				return Fail(
					OutError, TEXT("domain_hjson_resolved_source_invalid"));
			}
			if (Path == EntryFile)
			{
				DocumentText = MoveTemp(Text);
				++EntryMatches;
			}
		}
		if (EntryMatches > 1)
		{
			return Fail(
				OutError, TEXT("domain_hjson_entry_document_ambiguous"), EntryFile);
		}
		if (EntryMatches != 1 || DocumentText.IsEmpty())
		{
			return Fail(
				OutError, TEXT("domain_hjson_entry_document_missing"), EntryFile);
		}
		FUeAgentResourceTracker Resources = MakeResources();
		FHjsonDocumentPtr Document;
		if (!TryParseHjsonDocument(
			DocumentText,
			TEXT("resolved:") + Contract.DomainId + TEXT(":") + EntryFile,
			Resources,
			Document,
			OutError)
			|| !Document.IsValid())
		{
			return false;
		}
		TSharedPtr<FJsonValue> Converted =
			JsonValueFromHjson(*Document->GetRoot(), OutError);
		const TSharedPtr<FJsonObject> Canonical =
			Converted.IsValid() ? Converted->AsObject() : nullptr;
		FString Schema;
		const TSharedPtr<FJsonObject>* Root = nullptr;
		FString RootType;
		if (!Canonical.IsValid()
			|| !Canonical->TryGetStringField(TEXT("schema"), Schema))
		{
			return Fail(OutError, TEXT("domain_hjson_schema_missing"));
		}
		if (Schema != Contract.SchemaId)
		{
			return Fail(
				OutError, TEXT("domain_hjson_schema_mismatch"), Schema);
		}
		if (!Canonical->TryGetObjectField(TEXT("root"), Root)
			|| !Root || !Root->IsValid()
			|| !(*Root)->TryGetStringField(TEXT("type"), RootType))
		{
			return Fail(OutError, TEXT("domain_hjson_root_missing"));
		}
		if (RootType != Contract.RootTypeIdentity)
		{
			return Fail(
				OutError, TEXT("domain_hjson_root_type_mismatch"), RootType);
		}
		OutDocument.TargetIdentity = MoveTemp(Target);
		OutDocument.ExpectedRevision = MoveTemp(ExpectedRevision);
		OutDocument.EntryFile = MoveTemp(EntryFile);
		OutDocument.Params = MoveTemp(Params);
		OutDocument.Root = *Root;
		return true;
	}

	FString ObjectPathFromPackageIdentity(const FString& TargetIdentity)
	{
		if (TargetIdentity.Contains(TEXT("."))) return TargetIdentity;
		const FString AssetName =
			FPackageName::GetLongPackageAssetName(TargetIdentity);
		return AssetName.IsEmpty()
			? TargetIdentity
			: TargetIdentity + TEXT(".") + AssetName;
	}

	bool TryCaptureObject(
		const FString& TargetIdentity,
		const FString& ExpectedRootTypeIdentity,
		UObject*& OutObject,
		FUeAgentContentGraphCapture& OutCapture,
		FWorkspaceError& OutError)
	{
		OutObject = StaticLoadObject(
			UObject::StaticClass(), nullptr,
			*ObjectPathFromPackageIdentity(TargetIdentity));
		if (!OutObject)
		{
			return Fail(
				OutError, TEXT("domain_hjson_target_unresolved"), TargetIdentity);
		}
		if (!OutObject->GetClass()
			|| OutObject->GetClass()->GetPathName() != ExpectedRootTypeIdentity)
		{
			return Fail(
				OutError, TEXT("domain_hjson_target_type_mismatch"),
				OutObject->GetClass() ? OutObject->GetClass()->GetPathName() : FString());
		}
		FUeAgentContentGraphReadError CaptureError;
		if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
			*OutObject, OutCapture, CaptureError))
		{
			return Fail(
				OutError,
				CaptureError.Code.IsEmpty()
					? FString(TEXT("domain_hjson_capture_failed"))
					: CaptureError.Code,
				CaptureError.SubjectId,
				CaptureError.Detail);
		}
		return true;
	}

	bool TryBuildCaptureSnapshot(
		const FString& TargetIdentity,
		const FString& RootTypeIdentity,
		const FUeAgentContentGraphCapture& Capture,
		TSharedPtr<FJsonObject>& OutSnapshot,
		FWorkspaceError& OutError)
	{
		OutSnapshot.Reset();
		if (TargetIdentity.IsEmpty()
			|| RootTypeIdentity.IsEmpty()
			|| Capture.DomainId.IsEmpty()
			|| Capture.Revision.IsEmpty())
		{
			return Fail(OutError, TEXT("domain_hjson_capture_identity_invalid"));
		}
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("target"), TargetIdentity);
		Result->SetStringField(TEXT("root_type"), RootTypeIdentity);
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
			Relations.Add(MakeShared<FJsonValueObject>(Value));
		}
		Result->SetArrayField(TEXT("relations"), MoveTemp(Relations));

		TArray<TSharedPtr<FJsonValue>> Properties;
		for (const FUeAgentContentGraphProperty& Property : Capture.Properties)
		{
			const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("id"), Property.FieldId);
			Value->SetStringField(TEXT("owner"), Property.OwnerId);
			AddFieldState(Property.State, Value);
			Properties.Add(MakeShared<FJsonValueObject>(Value));
		}
		Result->SetArrayField(TEXT("properties"), MoveTemp(Properties));

		TArray<TSharedPtr<FJsonValue>> Parameters;
		for (const FUeAgentContentGraphParameter& Parameter : Capture.Parameters)
		{
			const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("id"), Parameter.ParameterId);
			Value->SetStringField(TEXT("owner"), Parameter.OwnerId);
			for (const TPair<FString, FString>& Selector
				: Parameter.SemanticIdentity.Selectors)
			{
				Value->SetStringField(Selector.Key, Selector.Value);
			}
			AddFieldState(Parameter.State, Value);
			Parameters.Add(MakeShared<FJsonValueObject>(Value));
		}
		Result->SetArrayField(TEXT("parameters"), MoveTemp(Parameters));

		bool bValid = true;
		TArray<TSharedPtr<FJsonValue>> Diagnostics;
		for (const FUeAgentContentGraphDiagnostic& Diagnostic : Capture.Diagnostics)
		{
			const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("id"), Diagnostic.DiagnosticId);
			Value->SetStringField(TEXT("severity"), Diagnostic.Severity);
			Value->SetStringField(TEXT("code"), Diagnostic.Code);
			Value->SetStringField(TEXT("message"), Diagnostic.Message);
			Value->SetBoolField(TEXT("resolved"), Diagnostic.bResolved);
			Diagnostics.Add(MakeShared<FJsonValueObject>(Value));
			bValid = bValid && (Diagnostic.bResolved
				|| !Diagnostic.Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase));
		}
		Result->SetArrayField(TEXT("diagnostics"), MoveTemp(Diagnostics));
		Result->SetStringField(
			TEXT("compile_status"), bValid ? TEXT("valid") : TEXT("error"));
		OutSnapshot = Result;
		OutError.Reset();
		return true;
	}

	bool TryCanonicalizeObject(
		const TSharedPtr<FJsonObject>& Object,
		FString& OutJson,
		FWorkspaceError& OutError)
	{
		FString Error;
		if (!Object.IsValid()
			|| !CommandContract::Json::TryCanonicalize(Object, OutJson, Error))
		{
			return Fail(
				OutError, TEXT("domain_hjson_json_invalid"), Error);
		}
		return true;
	}

	bool TryBuildImportSuccessPayload(
		const FString& ProviderId,
		const FString& TargetIdentity,
		const FString& Revision,
		const TSharedPtr<FJsonObject>& IndependentReadback,
		const TSharedPtr<FJsonObject>& Mutation,
		FString& OutPayloadJson,
		FWorkspaceError& OutError)
	{
		if (ProviderId.IsEmpty()
			|| TargetIdentity.IsEmpty()
			|| Revision.IsEmpty()
			|| !IndependentReadback.IsValid())
		{
			return Fail(OutError, TEXT("domain_hjson_result_identity_invalid"));
		}
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
		Result->SetStringField(TEXT("provider_id"), ProviderId);
		Result->SetStringField(TEXT("target"), TargetIdentity);
		Result->SetStringField(TEXT("revision"), Revision);
		Result->SetBoolField(TEXT("saved"), true);
		Result->SetBoolField(TEXT("reloaded"), true);
		Result->SetObjectField(TEXT("independent_readback"), IndependentReadback);
		if (Mutation.IsValid())
		{
			Result->SetObjectField(TEXT("mutation"), Mutation);
		}
		return TryCanonicalizeObject(Result, OutPayloadJson, OutError);
	}
}
