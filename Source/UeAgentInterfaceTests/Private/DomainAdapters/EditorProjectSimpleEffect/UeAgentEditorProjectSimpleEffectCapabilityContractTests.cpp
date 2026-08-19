#include "Misc/AutomationTest.h"

#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectTestSupport.h"

#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectCapabilities.h"

namespace
{
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Tests::EditorProjectSimpleEffect;

	bool RequiresValue(const FString& Presence)
	{
		return Presence == TEXT("set")
			|| Presence == TEXT("remove")
			|| Presence == TEXT("replace");
	}

	bool ValueMatchesKind(
		const TSharedPtr<FJsonValue>& Value,
		const FString& Kind)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Kind == TEXT("string"))
		{
			return Value->Type == EJson::String;
		}
		if (Kind == TEXT("boolean"))
		{
			return Value->Type == EJson::Boolean;
		}
		if (Kind == TEXT("integer")
			|| Kind == TEXT("number"))
		{
			return Value->Type == EJson::Number;
		}
		if (Kind == TEXT("array"))
		{
			return Value->Type == EJson::Array;
		}
		if (Kind == TEXT("object"))
		{
			return Value->Type == EJson::Object;
		}
		return false;
	}

	bool TryValidate(
		const TSharedPtr<const FUeAgentContractSchema>& Schema,
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Input,
		const FString& Label,
		const bool bExpected)
	{
		TOptional<FUeAgentContractValue> Canonical;
		FUeAgentContractSchemaError Error;
		const bool bAccepted =
			Schema.IsValid()
			&& Schema->TryValidateAndCanonicalize(
				ContractValue(
					Test,
					Input,
					Label + TEXT(" canonicalizes")),
				[](int64,
					const FString&,
					FUeAgentContractSchemaError&)
				{
					return true;
				},
				Canonical,
				Error);
		Test->TestEqual(*Label, bAccepted, bExpected);
		if (bExpected)
		{
			Test->TestTrue(
				*(Label + TEXT(" yields canonical value")),
				Canonical.IsSet() && Canonical->IsValid());
		}
		else
		{
			Test->TestTrue(
				*(Label + TEXT(" returns schema error")),
				Error.IsValid());
		}
		return bAccepted == bExpected;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentEditorProjectSimpleEffectCapabilityContractTest,
	"UeAgentInterface.DomainAdapters.EditorProjectSimpleEffect.E0."
		"CapabilityContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentEditorProjectSimpleEffectCapabilityContractTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}

	TestEqual(
		TEXT("fixture schema"),
		Fixture->GetStringField(TEXT("schema")),
		FString(TEXT(
			"uai.domain_adapter.editor_project_simple_effect."
			"test_fixture.v1")));
	const int32 DescriptorCount = static_cast<int32>(
		Fixture->GetNumberField(TEXT("descriptor_count")));
	const TArray<TSharedPtr<FJsonValue>>& DescriptorRows =
		Fixture->GetArrayField(TEXT("descriptors"));
	TestEqual(TEXT("seven formal descriptors"), DescriptorCount, 7);
	TestEqual(
		TEXT("descriptor rows are exact"),
		DescriptorRows.Num(),
		DescriptorCount);

	TSet<FString> NativeTypes;
	for (const TSharedPtr<FJsonValue>& Value
		: Fixture->GetArrayField(TEXT("native_operation_types")))
	{
		NativeTypes.Add(Value->AsString());
	}
	TestEqual(
		TEXT("one unified native operation type"),
		NativeTypes.Num(),
		1);
	TestTrue(
		TEXT("unified typed update is explicit"),
		NativeTypes.Contains(TEXT("editor_project.typed_update")));

	const int32 FieldContractCount = static_cast<int32>(
		Fixture->GetNumberField(TEXT("field_contract_count")));
	const TArray<TSharedPtr<FJsonValue>>& FieldRows =
		Fixture->GetArrayField(TEXT("field_contracts"));
	TestEqual(
		TEXT("eleven exact field contracts"),
		FieldContractCount,
		11);
	TestEqual(
		TEXT("field contract rows are exact"),
		FieldRows.Num(),
		FieldContractCount);

	TSet<FString> FieldKeys;
	TSet<FString> FieldBindings;
	for (const TSharedPtr<FJsonValue>& Value : FieldRows)
	{
		const TSharedPtr<FJsonObject> Field = Value->AsObject();
		if (!Field.IsValid())
		{
			AddError(TEXT("editor_project_field_row_invalid"));
			continue;
		}
		const FString FieldKey =
			Field->GetStringField(TEXT("profile_id"))
				+ TEXT(":")
				+ Field->GetStringField(TEXT("field_id"));
		const FString BindingId =
			Field->GetStringField(TEXT("field_binding_id"));
		const FString ValueKind =
			Field->GetStringField(TEXT("value_kind"));
		const TArray<FString> Presences =
			StringArray(Field, TEXT("allowed_presences"));
		TestTrue(
			*(FieldKey + TEXT(" is unique")),
			!FieldKeys.Contains(FieldKey));
		TestTrue(
			*(BindingId + TEXT(" is unique")),
			!FieldBindings.Contains(BindingId));
		TestTrue(
			*(FieldKey + TEXT(" has stable typed identity")),
			!BindingId.IsEmpty()
				&& !Field->GetStringField(TEXT("type_id")).IsEmpty());
		TestTrue(
			*(FieldKey + TEXT(" has presence contract")),
			!Presences.IsEmpty());
		bool bHasValuePresence = false;
		for (const FString& Presence : Presences)
		{
			bHasValuePresence |= RequiresValue(Presence);
		}
		TestTrue(
			*(FieldKey + TEXT(" can carry a value")),
			bHasValuePresence);
		TestTrue(
			*(FieldKey + TEXT(" example matches value kind")),
			ValueMatchesKind(
				Field->TryGetField(TEXT("example_value")),
				ValueKind));
		FieldKeys.Add(FieldKey);
		FieldBindings.Add(BindingId);
	}

	TSet<FString> ForbiddenFields;
	for (const TSharedPtr<FJsonValue>& Value
		: Fixture->GetArrayField(TEXT("forbidden_input_fields")))
	{
		ForbiddenFields.Add(Value->AsString());
	}
	TestEqual(
		TEXT("sixteen old boundary fields are forbidden"),
		ForbiddenFields.Num(),
		16);

	TSet<FString> DescriptorIds;
	TSet<FString> CapabilityIds;
	TSet<FString> Profiles;
	TSet<FString> Mechanics;
	TSet<FString> SourceCommands;
	TMap<FString, FString> HashesByDescriptor;
	for (const TSharedPtr<FJsonValue>& Value : DescriptorRows)
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		if (!Row.IsValid())
		{
			AddError(TEXT("editor_project_descriptor_row_invalid"));
			continue;
		}
		const FString DescriptorId =
			Row->GetStringField(TEXT("descriptor_id"));
		const FString CapabilityId =
			Row->GetStringField(TEXT("capability_id"));
		const FString ProfileId =
			Row->GetStringField(TEXT("profile_id"));
		const FString NativeType =
			Row->GetStringField(TEXT("native_operation_type"));
		const FString MechanicId =
			Row->GetStringField(TEXT("native_mechanic_id"));
		TestTrue(
			*(DescriptorId + TEXT(" is namespaced")),
			DescriptorId.StartsWith(
				TEXT("uai.operation.editor_project.")));
		TestTrue(
			*(DescriptorId + TEXT(" uses unified native type")),
			NativeType == TEXT("editor_project.typed_update"));
		TestTrue(
			*(DescriptorId + TEXT(" mechanic is exact")),
			MechanicId.StartsWith(
				TEXT("uai.editor_project.mechanic:")));
		TestTrue(
			*(DescriptorId + TEXT(" is unique")),
			!DescriptorIds.Contains(DescriptorId));
		TestTrue(
			*(CapabilityId + TEXT(" is unique")),
			!CapabilityIds.Contains(CapabilityId));
		TestTrue(
			*(ProfileId + TEXT(" is unique")),
			!Profiles.Contains(ProfileId));
		TestTrue(
			*(MechanicId + TEXT(" is unique")),
			!Mechanics.Contains(MechanicId));

		const TArray<TSharedPtr<FJsonObject>> Fields =
			FieldContracts(Fixture, Row);
		TestEqual(
			*(DescriptorId + TEXT(" field count is exact")),
			Fields.Num(),
			Row->GetArrayField(TEXT("field_ids")).Num());
		const TSharedPtr<FJsonObject> InputSchema =
			BuildInputSchema(Fixture, Row);
		const TSharedPtr<FJsonObject> ResultSchema =
			BuildResultSchema();
		FString Forbidden;
		TestFalse(
			*(DescriptorId
				+ TEXT(" schema has no cross-owner field")),
			ContainsForbiddenField(
				InputSchema,
				ForbiddenFields,
				Forbidden));

		TSharedPtr<const FUeAgentContractSchema> CompiledInput;
		TSharedPtr<const FUeAgentContractSchema> CompiledResult;
		if (TryCompileSchema(
				this,
				InputSchema,
				DescriptorId + TEXT(" input schema"),
				CompiledInput))
		{
			TryValidate(
				CompiledInput,
				this,
				BuildValidInput(Fixture, Row),
				DescriptorId + TEXT(" accepts valid input"),
				true);
			for (const FString& ForbiddenField : ForbiddenFields)
			{
				const TSharedPtr<FJsonObject> Invalid =
					CloneJsonValue(
						MakeShared<FJsonValueObject>(
							BuildValidInput(Fixture, Row)))
						->AsObject();
				Invalid->SetStringField(
					ForbiddenField,
					TEXT("forbidden"));
				TryValidate(
					CompiledInput,
					this,
					Invalid,
					DescriptorId
						+ TEXT(" rejects ")
						+ ForbiddenField,
					false);
			}
		}
		TryCompileSchema(
			this,
			ResultSchema,
			DescriptorId + TEXT(" result schema"),
			CompiledResult);
		HashesByDescriptor.Add(
			DescriptorId,
			ComputeDescriptorHash(
				this,
				Row,
				InputSchema,
				ResultSchema));

		for (const TSharedPtr<FJsonValue>& Source
			: Row->GetArrayField(TEXT("source_commands")))
		{
			const FString SourceCommand = Source->AsString();
			TestTrue(
				*(SourceCommand + TEXT(" maps once")),
				!SourceCommands.Contains(SourceCommand));
			SourceCommands.Add(SourceCommand);
		}
		DescriptorIds.Add(DescriptorId);
		CapabilityIds.Add(CapabilityId);
		Profiles.Add(ProfileId);
		Mechanics.Add(MechanicId);
	}
	TestEqual(
		TEXT("seven legacy sources map exactly once"),
		SourceCommands.Num(),
		7);

	const TArray<TSharedPtr<FJsonValue>>& NegativeCases =
		Fixture->GetArrayField(TEXT("e1_negative_cases"));
	TSet<FString> NegativeIds;
	for (const TSharedPtr<FJsonValue>& Value : NegativeCases)
	{
		const TSharedPtr<FJsonObject> Case = Value->AsObject();
		const FString CaseId =
			Case->GetStringField(TEXT("case_id"));
		TestTrue(
			*(CaseId + TEXT(" is unique")),
			!NegativeIds.Contains(CaseId));
		TestTrue(
			*(CaseId + TEXT(" targets formal descriptor")),
			DescriptorIds.Contains(
				Case->GetStringField(TEXT("descriptor_id"))));
		TestEqual(
			*(CaseId + TEXT(" is zero mutation")),
			static_cast<int32>(
				Case->GetNumberField(TEXT("mutation_count"))),
			0);
		TestFalse(
			*(CaseId + TEXT(" has stable error")),
			Case->GetStringField(TEXT("expected_error")).IsEmpty());
		NegativeIds.Add(CaseId);
	}
	TestEqual(
		TEXT("eleven semantic negative cases are frozen"),
		NegativeIds.Num(),
		11);

	const TArray<TSharedPtr<FJsonValue>>& Scenarios =
		Fixture->GetArrayField(TEXT("e2_scenarios"));
	TSet<FString> ScenarioDescriptors;
	for (const TSharedPtr<FJsonValue>& Value : Scenarios)
	{
		const TSharedPtr<FJsonObject> Scenario = Value->AsObject();
		const FString DescriptorId =
			Scenario->GetStringField(TEXT("descriptor_id"));
		TestTrue(
			*(DescriptorId + TEXT(" has one E2 scenario")),
			!ScenarioDescriptors.Contains(DescriptorId));
		for (const TCHAR* Field : {
			TEXT("fixture_kind"),
			TEXT("oracle"),
			TEXT("rollback_oracle"),
			TEXT("negative_oracle"),
		})
		{
			TestFalse(
				*FString::Printf(
					TEXT("%s %s is explicit"),
					*DescriptorId,
					Field),
				Scenario->GetStringField(Field).IsEmpty());
		}
		ScenarioDescriptors.Add(DescriptorId);
	}
	TestEqual(
		TEXT("each descriptor has owned E2 oracle"),
		ScenarioDescriptors.Num(),
		DescriptorCount);

	using namespace UE::UAI::DomainAdapters::
		EditorProjectSimpleEffect;
	using namespace UE::UAI::StateSemantic;

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	if (TestTrue(
			TEXT("create production EditorProject registry"),
			TryCreateEditorProjectSimpleEffectDescriptorRegistry(
				Registry,
				RegistryError))
		&& Registry.IsValid())
	{
		TestEqual(
			TEXT("production registry has seven descriptors"),
			Registry->GetDescriptors().Num(),
			DescriptorCount);
		for (const TSharedPtr<FJsonValue>& Value
			: DescriptorRows)
		{
			const TSharedPtr<FJsonObject> Row = Value->AsObject();
			const FString DescriptorId =
				Row->GetStringField(TEXT("descriptor_id"));
			const FUeAgentOperationDescriptor* Descriptor =
				Registry->FindByDescriptorId(DescriptorId);
			if (!TestNotNull(
					*(DescriptorId + TEXT(" exists in production")),
					Descriptor))
			{
				continue;
			}
			TestEqual(
				*(DescriptorId + TEXT(" hash is exact")),
				Descriptor->GetIdentity().GetDescriptorHash(),
				HashesByDescriptor[DescriptorId]);
			TestEqual(
				*(DescriptorId + TEXT(" is operation only")),
				Descriptor->GetOperationKind(),
				EUeAgentDescriptorOperationKind::Operation);
			TestFalse(
				*(DescriptorId + TEXT(" has no query binding")),
				Descriptor->GetQueryBinding().IsSet());

			const FString NativeBindingId =
				TEXT("uai.editor_project.binding:")
					+ Row->GetStringField(TEXT("capability_id"))
					+ TEXT(":v1");
			FString NativeType;
			FString ProfileId;
			FString MechanicId;
			FString BindingError;
			TestTrue(
				*(DescriptorId + TEXT(" resolves exact native binding")),
				TryResolveEditorProjectSimpleEffectNativeBinding(
					NativeBindingId,
					NativeType,
					ProfileId,
					MechanicId,
					BindingError));
			TestEqual(
				*(DescriptorId + TEXT(" native type matches")),
				NativeType,
				Row->GetStringField(TEXT("native_operation_type")));
			TestEqual(
				*(DescriptorId + TEXT(" profile matches")),
				ProfileId,
				Row->GetStringField(TEXT("profile_id")));
			TestEqual(
				*(DescriptorId + TEXT(" mechanic matches")),
				MechanicId,
				Row->GetStringField(TEXT("native_mechanic_id")));

			for (const TSharedPtr<FJsonObject>& Field
				: FieldContracts(Fixture, Row))
			{
				FString FieldBindingId;
				FString TypeId;
				EUeAgentValueKind ValueKind =
					EUeAgentValueKind::Null;
				TArray<FString> Presences;
				BindingError.Reset();
				TestTrue(
					*(DescriptorId + TEXT(":")
						+ Field->GetStringField(TEXT("field_id"))
						+ TEXT(" resolves field contract")),
					TryResolveEditorProjectSimpleEffectFieldContract(
						NativeBindingId,
						Field->GetStringField(TEXT("field_id")),
						FieldBindingId,
						TypeId,
						ValueKind,
						Presences,
						BindingError));
				TestEqual(
					TEXT("field binding matches"),
					FieldBindingId,
					Field->GetStringField(
						TEXT("field_binding_id")));
				TestEqual(
					TEXT("field type matches"),
					TypeId,
					Field->GetStringField(TEXT("type_id")));
				TestEqual(
					TEXT("field presences match"),
					Presences,
					StringArray(Field, TEXT("allowed_presences")));
			}
		}
	}
	else
	{
		AddError(
			TEXT("editor_project_registry_failed:")
				+ RegistryError);
	}

	return true;
}
