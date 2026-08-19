#include "Misc/AutomationTest.h"

#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectTestSupport.h"

#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectCapabilities.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Serialization/JsonWriter.h"

namespace
{
	using namespace UE::UAI::Tests::EditorProjectSimpleEffect;
}


namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::
		EditorProjectSimpleEffect;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
	MakeResources()
	{
		return MakeShared<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe>(
				FUeAgentResourceBudget({
					{EUeAgentResourceDimension::InputBytes, 1048576},
					{EUeAgentResourceDimension::OutputBytes, 1048576},
					{EUeAgentResourceDimension::StringBytes, 1048576},
					{EUeAgentResourceDimension::Nodes, 32768},
					{EUeAgentResourceDimension::Items, 32768},
					{EUeAgentResourceDimension::Files, 64},
					{EUeAgentResourceDimension::ReferenceEdges, 32768},
					{EUeAgentResourceDimension::WorkUnits, 1048576},
					{
						EUeAgentResourceDimension::
							EstimatedMemoryBytes,
						134217728,
					},
				}));
	}

	bool TryCanonicalValue(
		const TSharedPtr<FJsonValue>& JsonValue,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError)
	{
		OutValue.Reset();
		OutError = {};
		if (!JsonValue.IsValid())
		{
			return false;
		}
		switch (JsonValue->Type)
		{
		case EJson::Null:
			return FUeAgentCanonicalValue::TryNull(
				Resources,
				OutValue,
				OutError);
		case EJson::Boolean:
			return FUeAgentCanonicalValue::TryBoolean(
				JsonValue->AsBool(),
				Resources,
				OutValue,
				OutError);
		case EJson::String:
			return FUeAgentCanonicalValue::TryString(
				JsonValue->AsString(),
				Resources,
				OutValue,
				OutError);
		case EJson::Number:
			return FUeAgentCanonicalValue::TryDecimal(
				FString::SanitizeFloat(JsonValue->AsNumber()),
				Resources,
				OutValue,
				OutError);
		case EJson::Array:
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				for (const TSharedPtr<FJsonValue>& Item
					: JsonValue->AsArray())
				{
					FUeAgentCanonicalValuePtr CanonicalItem;
					if (!TryCanonicalValue(
							Item,
							Resources,
							CanonicalItem,
							OutError))
					{
						return false;
					}
					Items.Add(CanonicalItem.ToSharedRef());
				}
				return FUeAgentCanonicalValue::TryArray(
					Items,
					Resources,
					OutValue,
					OutError);
			}
		case EJson::Object:
			{
				TArray<
					TPair<FString, FUeAgentCanonicalValueRef>>
					Items;
				for (const TPair<
					FString,
					TSharedPtr<FJsonValue>>& Pair
					: JsonValue->AsObject()->Values)
				{
					FUeAgentCanonicalValuePtr CanonicalItem;
					if (!TryCanonicalValue(
							Pair.Value,
							Resources,
							CanonicalItem,
							OutError))
					{
						return false;
					}
					Items.Emplace(
						Pair.Key,
						CanonicalItem.ToSharedRef());
				}
				return FUeAgentCanonicalValue::TryObject(
					Items,
					Resources,
					OutValue,
					OutError);
			}
		default:
			return false;
		}
	}

	FUeAgentCanonicalSnapshotPtr MakeSnapshot(
		FAutomationTestBase* Test,
		const FUeAgentOperationDescriptor& Descriptor,
		const TSharedPtr<FJsonObject>& Row,
		FUeAgentResourceTracker& Resources,
		const FString& TargetRevision = TEXT("rev:target:1"),
		const FString& TargetProfileOverride = FString())
	{
		const FUeAgentStateRevision SnapshotRevision(
			TEXT("editor-project-e1"),
			TEXT("project:editor-project:r1"),
			1);
		TArray<FUeAgentStateEntry> Entries;
		auto AppendEntry =
			[&](
				const FString& Ref,
				const FString& Revision,
				const FString& Profile,
				const FString& TypeId)
			{
				const TSharedPtr<FJsonObject> State =
					MakeShared<FJsonObject>();
				State->SetStringField(TEXT("target_ref"), Ref);
				State->SetStringField(
					TEXT("target_revision"),
					Revision);
				State->SetStringField(TEXT("profile_id"), Profile);
				State->SetStringField(TEXT("type_id"), TypeId);
				State->SetObjectField(
					TEXT("fields"),
					MakeShared<FJsonObject>());

				FUeAgentCanonicalValuePtr Current;
				FUeAgentCanonicalValuePtr Default;
				FUeAgentStateSemanticError Error;
				if (!TryCanonicalValue(
						MakeShared<FJsonValueObject>(State),
						Resources,
						Current,
						Error)
					|| !FUeAgentCanonicalValue::TryObject(
						{},
						Resources,
						Default,
						Error))
				{
					return false;
				}
				Entries.Emplace(
					FUeAgentCanonicalIdentity(
						TEXT("uai.editor_project:") + Ref,
						TEXT("editor_project.typed_update"),
						TEXT("project:editor-project"),
						Ref,
						Profile,
						TEXT("project:editor-project:r1")),
					FUeAgentCanonicalType(
						TypeId,
						EUeAgentValueKind::Object,
						false,
						TEXT("object")),
					Current.ToSharedRef(),
					FUeAgentDefaultValue(
						EUeAgentDefaultSourceKind::Schema,
						TEXT("editor-project:e1:default"),
						TEXT("test-only independent state"),
						Default.ToSharedRef()),
					FUeAgentStateDetails(
						Ref,
						TEXT(""),
						{TEXT("EditorProjectSimpleEffect")},
						true,
						TEXT("available"),
						{}),
					SnapshotRevision);
				return true;
			};

		const FString Profile =
			TargetProfileOverride.IsEmpty()
				? Row->GetStringField(TEXT("profile_id"))
				: TargetProfileOverride;
		if (!AppendEntry(
				TEXT("uai://editor-project/fixture-target"),
				TargetRevision,
				Profile,
				TEXT("editor_project_target_state")))
		{
			Test->AddError(
				TEXT("editor_project_target_snapshot_failed"));
			return nullptr;
		}
		if (!AppendEntry(
				TEXT("uai://editor-project/device-profile/base"),
				TEXT("rev:base-profile:1"),
				TEXT("device_profile"),
				TEXT("editor_project_device_profile_state")))
		{
			Test->AddError(
				TEXT("editor_project_base_profile_snapshot_failed"));
			return nullptr;
		}
		if (!AppendEntry(
				TEXT("uai://editor-project-field/test-enabled"),
				TEXT("field:r1"),
				TEXT("project_settings_field"),
				TEXT("uai.value.boolean.v1")))
		{
			Test->AddError(
				TEXT("editor_project_field_snapshot_failed"));
			return nullptr;
		}

		FUeAgentCanonicalSnapshotPtr Snapshot;
		FUeAgentStateSemanticError Error;
		if (!Test->TestTrue(
				TEXT("create independent EditorProject snapshot"),
				FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("snapshot:editor-project:")
						+ Descriptor.GetCapabilityId(),
					EUeAgentStatePlane::AuthoringMemory,
					SnapshotRevision,
					FUeAgentTemporalPoint(
						TEXT("owned-worker-clock"),
						TEXT("1")),
					FUeAgentStateProvenance(
						TEXT("independent_readback"),
						TEXT("owned-editor-project-read-port"),
						TEXT(
							"editor-project:prestate-readback")),
					Entries,
					Resources,
					Snapshot,
					Error)))
		{
			Test->AddError(
				TEXT("editor_project_snapshot_failed:")
					+ Error.Code);
			return nullptr;
		}
		return Snapshot;
	}

	FUeAgentExecutionContextRefOwner MakeExecutionContext(
		const FUeAgentDescriptorIdentity& Descriptor)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("project:editor-project");
		Input.EditorInstanceId =
			TEXT("editor:editor-project-worker");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = EUeAgentExecutionWorldType::Editor;
		Input.ContextRevision =
			TEXT("project:editor-project:r1");
		Input.bResolvedFromCurrent = false;

		FUeAgentExecutionContextRefPtr Context;
		FUeAgentExecutionContextError Error;
		check(FUeAgentExecutionContextRef::TryCreate(
			Input,
			Descriptor,
			EUeAgentRequestKind::Operation,
			FUeAgentExecutionContextPolicy(
				{EUeAgentStatePlane::AuthoringMemory},
				{},
				EUeAgentContextResolutionPolicy::ExplicitOnly,
				EUeAgentTransientContextPolicy::
					TemporaryMutationAllowed),
			Context,
			Error));
		return Context.ToSharedRef();
	}

	bool BuildDraft(
		FAutomationTestBase* Test,
		const FUeAgentOperationDescriptor& Descriptor,
		const TSharedPtr<FJsonObject>& Input,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		FUeAgentResourceTracker& Resources,
		TOptional<FChangeSetDraft>& OutDraft,
		FExecutionMutationError& OutError)
	{
		FString InputJson;
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&InputJson);
		if (!Test->TestTrue(
				TEXT("serialize EditorProject canonical input"),
				FJsonSerializer::Serialize(
					Input.ToSharedRef(),
					Writer)))
		{
			return false;
		}

		TSharedPtr<const FUeAgentContractSchema> Compiled;
		FUeAgentContractSchemaError SchemaError;
		if (!FUeAgentContractSchema::TryCompile(
				Descriptor.GetInputSchema(),
				Compiled,
				SchemaError)
			|| !Compiled.IsValid())
		{
			return false;
		}
		TOptional<FUeAgentContractValue> CanonicalPayload;
		if (!Compiled->TryValidateAndCanonicalize(
				FUeAgentContractValue(InputJson),
				[](int64,
					const FString&,
					FUeAgentContractSchemaError&)
				{
					return true;
				},
				CanonicalPayload,
				SchemaError)
			|| !CanonicalPayload.IsSet())
		{
			OutError = FExecutionMutationError(
				TEXT("editor_project_input_schema_rejected"),
				SchemaError.Code + TEXT(":")
					+ SchemaError.Detail);
			return false;
		}

		const FUeAgentExecutionContextRefOwner Context =
			MakeExecutionContext(Descriptor.GetIdentity());
		return TryBuildEditorProjectSimpleEffectChangeSetDraft(
			FUeAgentOperationRequest(
				Descriptor.GetIdentity(),
				TEXT("request:editor-project"),
				TEXT("idempotency:editor-project"),
				CanonicalPayload.GetValue()),
			*Context,
			Snapshot,
			Resources,
			OutDraft,
			OutError);
	}

	const FUeAgentCanonicalValue* FindCanonicalField(
		const FUeAgentCanonicalValue& Value,
		const FString& Field)
	{
		if (Value.GetKind() != EUeAgentValueKind::Object)
		{
			return nullptr;
		}
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
			: Value.GetObjectItems())
		{
			if (Item.Key == Field)
			{
				return &Item.Value.Get();
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> FindChange(
		const TSharedPtr<FJsonObject>& Input,
		const FString& FieldId)
	{
		const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
		if (!Input->TryGetArrayField(TEXT("changes"), Changes)
			|| !Changes)
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Changes)
		{
			const TSharedPtr<FJsonObject> Change = Value->AsObject();
			if (Change.IsValid()
				&& Change->GetStringField(TEXT("field_id"))
					== FieldId)
			{
				return Change;
			}
		}
		return nullptr;
	}

	void RemoveChange(
		const TSharedPtr<FJsonObject>& Input,
		const FString& FieldId)
	{
		TArray<TSharedPtr<FJsonValue>> Changes =
			Input->GetArrayField(TEXT("changes"));
		Changes.RemoveAll(
			[&](const TSharedPtr<FJsonValue>& Value)
			{
				const TSharedPtr<FJsonObject> Change =
					Value->AsObject();
				return Change.IsValid()
					&& Change->GetStringField(TEXT("field_id"))
						== FieldId;
			});
		Input->SetArrayField(TEXT("changes"), MoveTemp(Changes));
	}

	void PrepareNegativeInput(
		const FString& CaseId,
		const TSharedPtr<FJsonObject>& Input)
	{
		if (CaseId == TEXT("stale_target_revision"))
		{
			Input->SetStringField(
				TEXT("expected_revision"),
				TEXT("rev:target:stale"));
		}
		else if (CaseId == TEXT("duplicate_field"))
		{
			TArray<TSharedPtr<FJsonValue>> Changes =
				Input->GetArrayField(TEXT("changes"));
			const TSharedPtr<FJsonObject> Duplicate =
				CloneJsonValue(Changes[0])->AsObject();
			Duplicate->GetObjectField(TEXT("value"))
				->SetStringField(
					TEXT("canonical_value"),
					TEXT("Linux"));
			Changes.Add(
				MakeShared<FJsonValueObject>(Duplicate));
			Input->SetArrayField(TEXT("changes"), MoveTemp(Changes));
		}
		else if (CaseId == TEXT("wrong_field_type"))
		{
			FindChange(Input, TEXT("native_culture"))
				->GetObjectField(TEXT("value"))
				->SetBoolField(TEXT("canonical_value"), true);
		}
		else if (CaseId == TEXT("presence_not_allowed"))
		{
			const TSharedPtr<FJsonObject> Change =
				FindChange(Input, TEXT("native_culture"));
			Change->SetStringField(TEXT("presence"), TEXT("null"));
			Change->RemoveField(TEXT("value"));
		}
		else if (CaseId == TEXT("unsafe_config_key"))
		{
			FindChange(Input, TEXT("entries"))
				->GetObjectField(TEXT("value"))
				->GetObjectField(TEXT("canonical_value"))
				->SetStringField(TEXT("Bad\nKey"), TEXT("1"));
		}
		else if (CaseId == TEXT("unbound_settings_field"))
		{
			const TArray<TSharedPtr<FJsonValue>>& Properties =
				FindChange(Input, TEXT("properties"))
					->GetObjectField(TEXT("value"))
					->GetArrayField(TEXT("canonical_value"));
			Properties[0]->AsObject()->SetStringField(
				TEXT("field_ref"),
				TEXT("uai://editor-project-field/unbound"));
		}
		else if (
			CaseId == TEXT("stale_settings_field_revision"))
		{
			const TArray<TSharedPtr<FJsonValue>>& Properties =
				FindChange(Input, TEXT("properties"))
					->GetObjectField(TEXT("value"))
					->GetArrayField(TEXT("canonical_value"));
			Properties[0]->AsObject()->SetStringField(
				TEXT("expected_revision"),
				TEXT("field:stale"));
		}
		else if (
			CaseId == TEXT("device_profile_inheritance_cycle"))
		{
			FindChange(Input, TEXT("base_profile_ref"))
				->GetObjectField(TEXT("value"))
				->SetStringField(
					TEXT("canonical_value"),
					TEXT(
						"uai://editor-project/fixture-target"));
		}
		else if (
			CaseId == TEXT("localization_native_culture_missing"))
		{
			RemoveChange(Input, TEXT("native_culture"));
		}
		else if (
			CaseId == TEXT("string_table_entry_duplicate"))
		{
			const TSharedPtr<FJsonObject> Value =
				FindChange(Input, TEXT("entries"))
					->GetObjectField(TEXT("value"));
			TArray<TSharedPtr<FJsonValue>> Entries =
				Value->GetArrayField(TEXT("canonical_value"));
			Entries.Add(CloneJsonValue(Entries[0]));
			Value->SetArrayField(
				TEXT("canonical_value"),
				MoveTemp(Entries));
		}
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentEditorProjectSimpleEffectPlanningTest,
	"UeAgentInterface.DomainAdapters.EditorProjectSimpleEffect.E1."
		"TypedPlanningAndPreflight",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentEditorProjectSimpleEffectPlanningTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	if (!TestTrue(
			TEXT("create EditorProject production registry"),
			TryCreateEditorProjectSimpleEffectDescriptorRegistry(
				Registry,
				RegistryError))
		|| !Registry.IsValid())
	{
		AddError(
			TEXT("editor_project_registry_failed:")
				+ RegistryError);
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Value
		: Fixture->GetArrayField(TEXT("descriptors")))
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		const FString DescriptorId =
			Row->GetStringField(TEXT("descriptor_id"));
		const FUeAgentOperationDescriptor* Descriptor =
			Registry->FindByDescriptorId(DescriptorId);
		if (!TestNotNull(
				*(DescriptorId + TEXT(" descriptor exists")),
				Descriptor))
		{
			continue;
		}
		const TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> Resources = MakeResources();
		const FUeAgentCanonicalSnapshotPtr Snapshot =
			MakeSnapshot(
				this,
				*Descriptor,
				Row,
				*Resources);
		if (!Snapshot.IsValid())
		{
			continue;
		}

		TOptional<FChangeSetDraft> Draft;
		FExecutionMutationError Error;
		const bool bBuilt = BuildDraft(
			this,
			*Descriptor,
			BuildValidInput(Fixture, Row),
			Snapshot.ToSharedRef(),
			*Resources,
			Draft,
			Error);
		TestTrue(
			*(DescriptorId + TEXT(" builds typed draft")),
			bBuilt);
		if (!bBuilt || !Draft.IsSet() || !Draft->IsValid())
		{
			AddError(
				DescriptorId + TEXT(":") + Error.GetCode());
			continue;
		}
		TestEqual(
			*(DescriptorId + TEXT(" produces one atomic change")),
			Draft->GetChanges().Num(),
			1);
		const FChangeDraft& Change = Draft->GetChanges()[0];
		TestEqual(
			*(DescriptorId + TEXT(" is an update")),
			Change.GetIntent(),
			EChangeIntent::Update);
		TestTrue(
			*(DescriptorId + TEXT(" freezes expected before")),
			Change.GetExpectedBefore().IsValid());
		TestTrue(
			*(DescriptorId + TEXT(" requires editor-project authority")),
			Change.GetRequiredAuthorityScopes().Contains(
				TEXT("uai:editor-project:write")));
		if (Row->GetStringField(TEXT("profile_id"))
			== TEXT("string_table"))
		{
			TestTrue(
				*(DescriptorId + TEXT(" requires asset authority")),
				Change.GetRequiredAuthorityScopes().Contains(
					TEXT("uai:asset:write")));
		}
		else
		{
			TestTrue(
				*(DescriptorId
					+ TEXT(" requires project-config authority")),
				Change.GetRequiredAuthorityScopes().Contains(
					TEXT("uai:project-config:write")));
		}
		const FUeAgentCanonicalValuePtr& After =
			Change.GetProposedAfter();
		for (const FString& Field : {
			TEXT("native_operation_type"),
			TEXT("native_binding_id"),
			TEXT("native_mechanic_id"),
			TEXT("profile_id"),
			TEXT("target_ref"),
			TEXT("target_revision"),
			TEXT("typed_changes"),
		})
		{
			TestNotNull(
				*(DescriptorId + TEXT(" freezes ") + Field),
				After.IsValid()
					? FindCanonicalField(*After, Field)
					: nullptr);
		}
		const FUeAgentCanonicalValue* TypedChanges =
			After.IsValid()
				? FindCanonicalField(*After, TEXT("typed_changes"))
				: nullptr;
		TestTrue(
			*(DescriptorId + TEXT(" keeps structured changes")),
			TypedChanges
				&& TypedChanges->GetKind()
					== EUeAgentValueKind::Array
				&& TypedChanges->GetArrayItems().Num()
					== FieldContracts(Fixture, Row).Num());
		if (TypedChanges)
		{
			for (const FUeAgentCanonicalValueRef& Unit
				: TypedChanges->GetArrayItems())
			{
				TestNotNull(
					*(DescriptorId
						+ TEXT(" freezes field binding")),
					FindCanonicalField(
						Unit.Get(),
						TEXT("field_binding_id")));
			}
		}
	}

	for (const TSharedPtr<FJsonValue>& Value
		: Fixture->GetArrayField(TEXT("e1_negative_cases")))
	{
		const TSharedPtr<FJsonObject> Case = Value->AsObject();
		const FString CaseId =
			Case->GetStringField(TEXT("case_id"));
		const FString DescriptorId =
			Case->GetStringField(TEXT("descriptor_id"));
		const FUeAgentOperationDescriptor* Descriptor =
			Registry->FindByDescriptorId(DescriptorId);
		if (!TestNotNull(
				*(CaseId + TEXT(" descriptor exists")),
				Descriptor))
		{
			continue;
		}
		TSharedPtr<FJsonObject> Row;
		for (const TSharedPtr<FJsonValue>& DescriptorValue
			: Fixture->GetArrayField(TEXT("descriptors")))
		{
			if (DescriptorValue->AsObject()->GetStringField(
					TEXT("descriptor_id"))
				== DescriptorId)
			{
				Row = DescriptorValue->AsObject();
				break;
			}
		}
		const TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> Resources = MakeResources();
		const FString ProfileOverride =
			CaseId == TEXT("target_profile_mismatch")
				? TEXT("wrong_profile")
				: FString();
		const FUeAgentCanonicalSnapshotPtr Snapshot =
			MakeSnapshot(
				this,
				*Descriptor,
				Row,
				*Resources,
				TEXT("rev:target:1"),
				ProfileOverride);
		TSharedPtr<FJsonObject> Input =
			BuildValidInput(Fixture, Row);
		PrepareNegativeInput(CaseId, Input);

		TOptional<FChangeSetDraft> Draft;
		FExecutionMutationError Error;
		const bool bBuilt = BuildDraft(
			this,
			*Descriptor,
			Input,
			Snapshot.ToSharedRef(),
			*Resources,
			Draft,
			Error);
		TestFalse(
			*(CaseId + TEXT(" fails before mutation")),
			bBuilt);
		TestFalse(
			*(CaseId + TEXT(" produces no draft")),
			Draft.IsSet());
		TestEqual(
			*(CaseId + TEXT(" returns stable error")),
			Error.GetCode(),
			Case->GetStringField(TEXT("expected_error")));
	}

	return true;
}
