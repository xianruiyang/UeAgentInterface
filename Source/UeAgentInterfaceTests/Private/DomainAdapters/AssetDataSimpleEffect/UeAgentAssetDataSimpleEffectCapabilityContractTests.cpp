#include "Misc/AutomationTest.h"

#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTestSupport.h"

// Re-evaluate the SA-005 production seam after the header is introduced.
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"

namespace
{
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Tests::AssetDataSimpleEffect;

	bool RequiresValue(const FString& Presence)
	{
		return Presence == TEXT("set")
			|| Presence == TEXT("replace")
			|| Presence == TEXT("remove");
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDataSimpleEffectCapabilityContractTest,
	"UeAgentInterface.DomainAdapters.AssetDataSimpleEffect.E0."
		"CapabilityContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDataSimpleEffectCapabilityContractTest::RunTest(
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
			"uai.domain_adapter.asset_data_simple_effect."
			"test_fixture.v2")));
	const int32 DescriptorCount = static_cast<int32>(
		Fixture->GetNumberField(TEXT("descriptor_count")));
	const TArray<TSharedPtr<FJsonValue>>& Rows =
		Fixture->GetArrayField(TEXT("descriptors"));
	TestEqual(TEXT("53 formal descriptors"), DescriptorCount, 53);
	TestEqual(
		TEXT("descriptor rows are exact"),
		Rows.Num(),
		DescriptorCount);

	TSet<FString> AllowedTypes;
	for (const TSharedPtr<FJsonValue>& Value
		: Fixture->GetArrayField(TEXT("native_operation_types")))
	{
		AllowedTypes.Add(Value->AsString());
	}
	TestEqual(
		TEXT("five unified native operation types"),
		AllowedTypes.Num(),
		5);

	const int32 FactoryContractCount = static_cast<int32>(
		Fixture->GetNumberField(TEXT("factory_contract_count")));
	const TArray<TSharedPtr<FJsonValue>>& FactoryContractRows =
		Fixture->GetArrayField(TEXT("factory_contracts"));
	TestEqual(
		TEXT("four revision-bound factory contracts"),
		FactoryContractCount,
		4);
	TestEqual(
		TEXT("factory contract rows are exact"),
		FactoryContractRows.Num(),
		FactoryContractCount);
	TSet<FString> FactoryContractIds;
	TSet<FString> FactoryBindingIds;
	TSet<FString> FactoryProfiles;
	for (const TSharedPtr<FJsonValue>& Value : FactoryContractRows)
	{
		const TSharedPtr<FJsonObject> Contract = Value->AsObject();
		const FString ContractId =
			Contract->GetStringField(TEXT("factory_contract_id"));
		const FString BindingId =
			Contract->GetStringField(TEXT("factory_binding_id"));
		const FString ProfileId =
			Contract->GetStringField(TEXT("profile_id"));
		TestTrue(
			*(ContractId + TEXT(" is unique")),
			!FactoryContractIds.Contains(ContractId));
		TestTrue(
			*(BindingId + TEXT(" is unique")),
			!FactoryBindingIds.Contains(BindingId));
		TestTrue(
			*(ProfileId + TEXT(" has one factory contract")),
			!FactoryProfiles.Contains(ProfileId));
		TestTrue(
			*(ContractId + TEXT(" has canonical candidate ref")),
			Contract->GetStringField(TEXT("candidate_ref"))
				.StartsWith(TEXT("uai://factory/")));
		TestTrue(
			*(ContractId + TEXT(" freezes catalog revision")),
			!Contract->GetStringField(
				TEXT("catalog_source_id")).IsEmpty()
				&& !Contract->GetStringField(
					TEXT("catalog_generation")).IsEmpty()
				&& Contract->GetNumberField(
					TEXT("catalog_sequence")) >= 0);
		TestTrue(
			*(ContractId + TEXT(" has exact class constraint")),
			!Contract->GetStringField(
				TEXT("class_constraint_id")).IsEmpty()
				&& !Contract->GetStringField(
					TEXT("asset_class_path")).IsEmpty());
		FactoryContractIds.Add(ContractId);
		FactoryBindingIds.Add(BindingId);
		FactoryProfiles.Add(ProfileId);
	}

	const int32 FieldContractCount = static_cast<int32>(
		Fixture->GetNumberField(TEXT("field_contract_count")));
	const TArray<TSharedPtr<FJsonValue>>& FieldContractsRows =
		Fixture->GetArrayField(TEXT("field_contracts"));
	TestEqual(TEXT("67 exact field contracts"), FieldContractCount, 67);
	TestEqual(
		TEXT("field contract rows are exact"),
		FieldContractsRows.Num(),
		FieldContractCount);
	TSet<FString> FieldContractKeys;
	TSet<FString> FieldBindingIds;
	for (const TSharedPtr<FJsonValue>& Value : FieldContractsRows)
	{
		const TSharedPtr<FJsonObject> Field = Value->AsObject();
		const FString ProfileId =
			Field->GetStringField(TEXT("profile_id"));
		const FString FieldId =
			Field->GetStringField(TEXT("field_id"));
		const FString FieldKey = ProfileId + TEXT(":") + FieldId;
		const FString BindingId =
			Field->GetStringField(TEXT("field_binding_id"));
		const FString TypeId =
			Field->GetStringField(TEXT("type_id"));
		const FString ValueKind =
			Field->GetStringField(TEXT("value_kind"));
		TestTrue(
			*(FieldKey + TEXT(" is unique")),
			!FieldContractKeys.Contains(FieldKey));
		TestTrue(
			*(BindingId + TEXT(" is unique")),
			!FieldBindingIds.Contains(BindingId));
		TestTrue(
			*(FieldKey + TEXT(" has stable typed identity")),
			!BindingId.IsEmpty() && !TypeId.IsEmpty());
		TestTrue(
			*(FieldKey + TEXT(" has supported JSON value kind")),
			ValueKind == TEXT("string")
				|| ValueKind == TEXT("boolean")
				|| ValueKind == TEXT("integer")
				|| ValueKind == TEXT("number")
				|| ValueKind == TEXT("array")
				|| ValueKind == TEXT("object"));
		const TArray<FString> Presences =
			StringArray(Field, TEXT("allowed_presences"));
		TestTrue(
			*(FieldKey + TEXT(" has per-field presence")),
			!Presences.IsEmpty());
		bool bNeedsValue = false;
		for (const FString& Presence : Presences)
		{
			bNeedsValue |= RequiresValue(Presence);
		}
		TestTrue(
			*(FieldKey + TEXT(" has a value-carrying mechanic")),
			bNeedsValue);
		const TSharedPtr<FJsonValue> Example =
			Field->TryGetField(TEXT("example_value"));
		TestTrue(
			*(FieldKey + TEXT(" has native structured example")),
			Example.IsValid()
				&& (ValueKind != TEXT("array")
					|| Example->Type == EJson::Array)
				&& (ValueKind != TEXT("object")
					|| Example->Type == EJson::Object));
		FieldContractKeys.Add(FieldKey);
		FieldBindingIds.Add(BindingId);
	}

	TSet<FString> Forbidden;
	for (const TSharedPtr<FJsonValue>& Value
		: Fixture->GetArrayField(TEXT("forbidden_input_fields")))
	{
		Forbidden.Add(Value->AsString());
	}

	const TSharedPtr<FJsonObject> ResultSchema =
		BuildResultSchema();
	TSharedPtr<const FUeAgentContractSchema> CompiledResult;
	TryCompileSchema(
		this,
		ResultSchema,
		TEXT("shared result schema"),
		CompiledResult);

	TMap<FString, FString> HashesByDescriptor;
	TMap<FString, FString> CapabilitiesByDescriptor;
	TMap<FString, FString> ProfilesByDescriptor;
	TMap<FString, FString> NativeTypesByDescriptor;
	TSet<FString> CapabilityIds;
	TSet<FString> SourceScenarios;
	TMap<FString, int32> CountsByType;
	for (const TSharedPtr<FJsonValue>& Value : Rows)
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		const FString DescriptorId =
			Row->GetStringField(TEXT("descriptor_id"));
		const FString CapabilityId =
			Row->GetStringField(TEXT("capability_id"));
		const FString NativeType =
			Row->GetStringField(TEXT("native_operation_type"));
		const FString ProfileId =
			Row->GetStringField(TEXT("profile_id"));
		const TArray<TSharedPtr<FJsonObject>> BoundFields =
			FieldContracts(Fixture, Row);
		const TSharedPtr<FJsonObject> FactoryContract =
			FindFactoryContract(Fixture, Row);

		TestTrue(
			*(DescriptorId + TEXT(" uses asset namespace")),
			DescriptorId.StartsWith(
				TEXT("uai.operation.asset.")));
		TestTrue(
			*(DescriptorId + TEXT(" has a stable profile")),
			!ProfileId.IsEmpty());
		TestTrue(
			*(DescriptorId + TEXT(" maps to one unified type")),
			AllowedTypes.Contains(NativeType));
		TestFalse(
			*(DescriptorId + TEXT(" has no legacy identity")),
			Row->HasField(TEXT("legacy_command_id"))
				|| Row->HasField(TEXT("command"))
				|| Row->HasField(TEXT("route")));
		TestFalse(
			*(DescriptorId + TEXT(" is unique")),
			HashesByDescriptor.Contains(DescriptorId));
		TestFalse(
			*(CapabilityId + TEXT(" is unique")),
			CapabilityIds.Contains(CapabilityId));
		TestEqual(
			*(DescriptorId
				+ TEXT(" binds every declared field exactly once")),
			BoundFields.Num(),
			StringArray(Row, TEXT("field_ids")).Num());
		if (FactoryContract.IsValid())
		{
			TestTrue(
				*(DescriptorId
					+ TEXT(" is a controlled polymorphic create")),
				IsCreate(Row)
					&& FactoryProfiles.Contains(ProfileId));
		}
		for (const TSharedPtr<FJsonObject>& Field : BoundFields)
		{
			TestTrue(
				*(DescriptorId + TEXT(" field binding exists")),
				FieldContractKeys.Contains(
					ProfileId + TEXT(":")
						+ Field->GetStringField(TEXT("field_id"))));
		}

		const TSharedPtr<FJsonObject> InputSchema =
			BuildInputSchema(Fixture, Row);
		TSharedPtr<const FUeAgentContractSchema> CompiledInput;
		if (!TryCompileSchema(
				this,
				InputSchema,
				DescriptorId + TEXT(" input schema"),
				CompiledInput))
		{
			continue;
		}
		FString ForbiddenField;
		TestFalse(
			*(DescriptorId + TEXT(" excludes cross-owner fields")),
			ContainsForbiddenField(
				InputSchema,
				Forbidden,
				ForbiddenField));

		TOptional<FUeAgentContractValue> Canonical;
		FUeAgentContractSchemaError ValidationError;
		TestTrue(
			*(DescriptorId + TEXT(" accepts valid input")),
			CompiledInput->TryValidateAndCanonicalize(
				ContractValue(
					this,
					BuildValidInput(Fixture, Row),
					DescriptorId
						+ TEXT(" valid input canonicalizes")),
				[](int64,
					const FString&,
					FUeAgentContractSchemaError&)
				{
					return true;
				},
				Canonical,
				ValidationError));
		TestTrue(
			*(DescriptorId + TEXT(" produces canonical input")),
			Canonical.IsSet() && Canonical->IsValid());

		const TSharedPtr<FJsonObject> MissingTarget =
			BuildValidInput(Fixture, Row);
		MissingTarget->RemoveField(TEXT("target_ref"));
		Canonical.Reset();
		ValidationError = {};
		TestFalse(
			*(DescriptorId + TEXT(" rejects missing target_ref")),
			CompiledInput->TryValidateAndCanonicalize(
				ContractValue(
					this,
					MissingTarget,
					DescriptorId
						+ TEXT(" invalid input canonicalizes")),
				[](int64,
					const FString&,
					FUeAgentContractSchemaError&)
				{
					return true;
				},
				Canonical,
				ValidationError));
		TestTrue(
			*(DescriptorId + TEXT(" returns typed schema error")),
			ValidationError.IsValid());

		for (const TSharedPtr<FJsonValue>& SourceValue
			: Row->GetArrayField(TEXT("source_commands")))
		{
			const FString Source = SourceValue->AsString();
			TestTrue(
				*(Source + TEXT(" source maps once")),
				!SourceScenarios.Contains(Source));
			SourceScenarios.Add(Source);
		}

		HashesByDescriptor.Add(
			DescriptorId,
			ComputeDescriptorHash(
				this,
				Row,
				InputSchema,
				ResultSchema));
		CapabilitiesByDescriptor.Add(
			DescriptorId,
			CapabilityId);
		ProfilesByDescriptor.Add(DescriptorId, ProfileId);
		NativeTypesByDescriptor.Add(DescriptorId, NativeType);
		CapabilityIds.Add(CapabilityId);
		CountsByType.FindOrAdd(NativeType) += 1;
	}
	TestEqual(
		TEXT("53 exact descriptor hashes"),
		HashesByDescriptor.Num(),
		DescriptorCount);
	TestEqual(
		TEXT("64 unique migrated or retained scenarios"),
		SourceScenarios.Num(),
		64);

	const TSharedPtr<FJsonObject> ExpectedCounts =
		Fixture->GetObjectField(TEXT("expected_type_counts"));
	for (const FString& NativeType : AllowedTypes)
	{
		TestEqual(
			*(NativeType + TEXT(" count")),
			CountsByType.FindRef(NativeType),
			static_cast<int32>(
				ExpectedCounts->GetNumberField(NativeType)));
	}

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
		NegativeIds.Add(CaseId);
		TestTrue(
			*(CaseId + TEXT(" targets a formal descriptor")),
			HashesByDescriptor.Contains(
				Case->GetStringField(TEXT("descriptor_id"))));
		TestEqual(
			*(CaseId + TEXT(" fails before mutation")),
			static_cast<int32>(
				Case->GetNumberField(TEXT("mutation_count"))),
			0);
		TestFalse(
			*(CaseId + TEXT(" has stable error")),
			Case->GetStringField(TEXT("expected_error"))
				.IsEmpty());
	}
	TestEqual(
		TEXT("21 semantic negative cases"),
		NegativeIds.Num(),
		21);

	const TArray<TSharedPtr<FJsonValue>>& E2Scenarios =
		Fixture->GetArrayField(TEXT("e2_scenarios"));
	TSet<FString> E2Ids;
	TSet<FString> E2Types;
	TSet<FString> E2Classes;
	int32 MonomorphicCreateCount = 0;
	int32 PolymorphicCreateCount = 0;
	for (const TSharedPtr<FJsonValue>& Value : E2Scenarios)
	{
		const TSharedPtr<FJsonObject> Scenario =
			Value->AsObject();
		const FString ScenarioId =
			Scenario->GetStringField(TEXT("scenario_id"));
		TestTrue(
			*(ScenarioId + TEXT(" is unique")),
			!E2Ids.Contains(ScenarioId));
		E2Ids.Add(ScenarioId);
		E2Types.Add(
			Scenario->GetStringField(
				TEXT("native_operation_type")));
		E2Classes.Add(
			Scenario->GetStringField(TEXT("asset_class")));
		if (Scenario->GetStringField(
				TEXT("native_operation_type"))
			== TEXT("asset.object.create"))
		{
			if (Scenario->HasTypedField<EJson::String>(
					TEXT("factory_contract_id")))
			{
				++PolymorphicCreateCount;
				TestTrue(
					*(ScenarioId
						+ TEXT(" uses a formal factory contract")),
					FactoryContractIds.Contains(
						Scenario->GetStringField(
							TEXT("factory_contract_id"))));
			}
			else
			{
				++MonomorphicCreateCount;
			}
			TestTrue(
				*(ScenarioId
					+ TEXT(" freezes concrete class path")),
				Scenario->HasTypedField<EJson::String>(
					TEXT("asset_class_path"))
					&& !Scenario->GetStringField(
						TEXT("asset_class_path")).IsEmpty());
		}
		TestTrue(
			*(ScenarioId + TEXT(" uses a formal descriptor")),
			HashesByDescriptor.Contains(
				Scenario->GetStringField(TEXT("descriptor_id"))));
		TestFalse(
			*(ScenarioId + TEXT(" has independent oracle")),
			Scenario->GetStringField(TEXT("oracle")).IsEmpty());
		TestFalse(
			*(ScenarioId + TEXT(" has rollback oracle")),
			Scenario->GetStringField(TEXT("rollback")).IsEmpty());
	}
	TestEqual(TEXT("71 owned E2 scenarios"), E2Ids.Num(), 71);
	TestEqual(
		TEXT("19 monomorphic create scenarios"),
		MonomorphicCreateCount,
		19);
	TestEqual(
		TEXT("4 polymorphic create scenarios"),
		PolymorphicCreateCount,
		4);
	TestEqual(
		TEXT("all five native types have E2 coverage"),
		E2Types.Num(),
		5);
	for (const FString& RequiredClass
		: TArray<FString>{
			TEXT("UDataAsset"),
			TEXT("UDataTable"),
			TEXT("UTextureRenderTarget2D"),
			TEXT("UStaticMesh"),
			TEXT("USkeletalMesh"),
			TEXT("UMediaPlayer"),
			TEXT("UMediaTexture"),
		})
	{
		TestTrue(
			*(RequiredClass + TEXT(" is covered by owned E2")),
			E2Classes.Contains(RequiredClass));
	}

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	const bool bCreated =
		UE::UAI::DomainAdapters::AssetDataSimpleEffect::
			TryCreateAssetDataSimpleEffectDescriptorRegistry(
				Registry,
				RegistryError);
	TestTrue(
		TEXT("create production AssetDataSimpleEffect registry"),
		bCreated);
	if (!bCreated)
	{
		AddError(
			TEXT("asset_data_registry_failed:")
				+ RegistryError);
	}
	if (Registry.IsValid())
	{
		TestEqual(
			TEXT("production registry has 53 descriptors"),
			Registry->GetDescriptors().Num(),
			DescriptorCount);
		for (const TPair<FString, FString>& Pair
			: HashesByDescriptor)
		{
			const FUeAgentOperationDescriptor* Descriptor =
				Registry->FindByDescriptorId(Pair.Key);
			TestTrue(
				*(Pair.Key
					+ TEXT(" is an exact production operation")),
				Descriptor
					&& Descriptor->GetOperationKind()
						== EUeAgentDescriptorOperationKind::
							Operation
					&& !Descriptor->GetQueryBinding().IsSet()
					&& Descriptor->GetCapabilityId()
						== CapabilitiesByDescriptor[Pair.Key]
					&& Descriptor->GetDomainId()
						== TEXT("asset_data")
					&& Descriptor->GetExecutionContextPolicy()
							.GetRequiredFacets()
							.IsEmpty()
					&& Descriptor->GetIdentity()
							.GetDescriptorHash()
						== Pair.Value);
			if (Descriptor)
			{
				FString BoundNativeType;
				FString BoundProfile;
				FString NativeMechanicId;
				FString StaticCreateClassPath;
				FString BindingError;
				const FString NativeBindingId =
					TEXT("uai.asset_data.binding:")
					+ CapabilitiesByDescriptor[Pair.Key]
					+ TEXT(":v1");
				TestTrue(
					*(Pair.Key
						+ TEXT(" resolves one compiled native binding")),
					UE::UAI::DomainAdapters::
						AssetDataSimpleEffect::
						TryResolveAssetDataSimpleEffectNativeBinding(
							NativeBindingId,
							BoundNativeType,
							BoundProfile,
							NativeMechanicId,
							StaticCreateClassPath,
							BindingError)
						&& BoundNativeType
							== NativeTypesByDescriptor[Pair.Key]
						&& BoundProfile
							== ProfilesByDescriptor[Pair.Key]);
				TestFalse(
					*(Pair.Key
						+ TEXT(" has an executable native mechanic")),
					NativeMechanicId.IsEmpty());
				FString FactoryContractId;
				FString ClassConstraintId;
				TestTrue(
					*(Pair.Key
						+ TEXT(" resolves compiled factory policy")),
					UE::UAI::DomainAdapters::
						AssetDataSimpleEffect::
						TryResolveAssetDataSimpleEffectFactoryContract(
							NativeBindingId,
							FactoryContractId,
							ClassConstraintId,
							BindingError));
				const FString ProfileId =
					ProfilesByDescriptor[Pair.Key];
				TSharedPtr<FJsonObject> ExpectedFactoryContract;
				if (NativeTypesByDescriptor[Pair.Key]
					== TEXT("asset.object.create"))
				{
					for (const TSharedPtr<FJsonValue>& FactoryValue
						: FactoryContractRows)
					{
						const TSharedPtr<FJsonObject> Candidate =
							FactoryValue->AsObject();
						if (Candidate->GetStringField(
								TEXT("profile_id"))
							== ProfileId)
						{
							ExpectedFactoryContract = Candidate;
							break;
						}
					}
				}
				if (ExpectedFactoryContract.IsValid())
				{
					TestEqual(
						*(Pair.Key
							+ TEXT(" freezes factory contract id")),
						FactoryContractId,
						ExpectedFactoryContract->GetStringField(
							TEXT("factory_contract_id")));
					TestEqual(
						*(Pair.Key
							+ TEXT(" freezes class constraint id")),
						ClassConstraintId,
						ExpectedFactoryContract->GetStringField(
							TEXT("class_constraint_id")));
					TestTrue(
						*(Pair.Key
							+ TEXT(" does not use static class path")),
						StaticCreateClassPath.IsEmpty());
				}
				else
				{
					TestTrue(
						*(Pair.Key
							+ TEXT(" has no candidate factory policy")),
						FactoryContractId.IsEmpty()
							&& ClassConstraintId.IsEmpty());
				}
				TSharedPtr<FJsonObject> ProductionInputSchema;
				const TSharedRef<TJsonReader<>> Reader =
					TJsonReaderFactory<>::Create(
						Descriptor->GetInputSchema()
							.GetJsonValue());
				TestTrue(
					*(Pair.Key
						+ TEXT(" input schema parses")),
					FJsonSerializer::Deserialize(
						Reader,
						ProductionInputSchema)
						&& ProductionInputSchema.IsValid());
			}
		}
	}

	return true;
}
