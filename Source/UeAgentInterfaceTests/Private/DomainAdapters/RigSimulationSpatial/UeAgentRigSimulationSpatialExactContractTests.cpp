#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialDescriptors.h"

#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialExecutionChains.h"

namespace
{
	using namespace UE::UAI::Contract;

	FString FixturePath(const TCHAR* RelativePath)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("UeAgentInterface/Source/UeAgentInterfaceTests/Private"),
			RelativePath));
	}

	TSharedPtr<FJsonObject> LoadObject(
		FAutomationTestBase* Test,
		const TCHAR* RelativePath,
		const TCHAR* Label)
	{
		FString Text;
		if (!Test->TestTrue(
				FString(TEXT("load ")) + Label,
				FFileHelper::LoadFileToString(Text, *FixturePath(RelativePath))))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				FString(TEXT("parse ")) + Label,
				FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid()))
		{
			return nullptr;
		}
		return Object;
	}

	TSharedPtr<FJsonValue> ExpandValue(
		const TSharedPtr<FJsonValue>& Value,
		const TSharedPtr<FJsonObject>& Components,
		TSet<FString>& Stack,
		FString& OutError)
	{
		if (!Value.IsValid())
		{
			OutError = TEXT("timeline_fixture_schema_value_invalid");
			return nullptr;
		}
		switch (Value->Type)
		{
		case EJson::Null:
			return MakeShared<FJsonValueNull>();
		case EJson::String:
			return MakeShared<FJsonValueString>(Value->AsString());
		case EJson::Number:
			return MakeShared<FJsonValueNumber>(Value->AsNumber());
		case EJson::Boolean:
			return MakeShared<FJsonValueBoolean>(Value->AsBool());
		case EJson::Array:
		{
			TArray<TSharedPtr<FJsonValue>> Expanded;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				TSharedPtr<FJsonValue> ExpandedItem = ExpandValue(
					Item, Components, Stack, OutError);
				if (!ExpandedItem.IsValid())
				{
					return nullptr;
				}
				Expanded.Add(MoveTemp(ExpandedItem));
			}
			return MakeShared<FJsonValueArray>(MoveTemp(Expanded));
		}
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Object.IsValid())
			{
				OutError = TEXT("timeline_fixture_schema_object_invalid");
				return nullptr;
			}
			FString ComponentName;
			if (Object->TryGetStringField(TEXT("component"), ComponentName))
			{
				const TSharedPtr<FJsonValue>* Definition =
					Components->Values.Find(ComponentName);
				if (Object->Values.Num() != 1 || !Definition || Stack.Contains(ComponentName))
				{
					OutError = TEXT("timeline_fixture_component_invalid:") + ComponentName;
					return nullptr;
				}
				Stack.Add(ComponentName);
				TSharedPtr<FJsonValue> Result = ExpandValue(
					*Definition, Components, Stack, OutError);
				Stack.Remove(ComponentName);
				return Result;
			}
			const TSharedPtr<FJsonObject> ExpandedObject = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				TSharedPtr<FJsonValue> ExpandedField = ExpandValue(
					Pair.Value, Components, Stack, OutError);
				if (!ExpandedField.IsValid())
				{
					return nullptr;
				}
				ExpandedObject->SetField(Pair.Key, MoveTemp(ExpandedField));
			}
			return MakeShared<FJsonValueObject>(ExpandedObject);
		}
		default:
			OutError = TEXT("timeline_fixture_schema_kind_invalid");
			return nullptr;
		}
	}

	bool TryBuildSchema(
		const TArray<TSharedPtr<FJsonValue>>& Fields,
		const TSharedPtr<FJsonObject>& Components,
		FString& OutCanonical,
		TSet<FString>& OutRequired,
		TSet<FString>& OutOptional,
		FString& OutError)
	{
		OutCanonical.Reset();
		OutRequired.Reset();
		OutOptional.Reset();
		const TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> RequiredValues;
		for (const TSharedPtr<FJsonValue>& FieldValue : Fields)
		{
			const TSharedPtr<FJsonObject> Field =
				FieldValue.IsValid() ? FieldValue->AsObject() : nullptr;
			FString Name;
			bool bRequired = false;
			if (!Field.IsValid()
				|| !Field->TryGetStringField(TEXT("name"), Name)
				|| Name.IsEmpty()
				|| !Field->TryGetBoolField(TEXT("required"), bRequired)
				|| Properties->HasField(Name))
			{
				OutError = TEXT("timeline_fixture_field_invalid");
				return false;
			}
			TSharedPtr<FJsonValue> Template;
			FString ComponentName;
			if (Field->TryGetStringField(TEXT("component"), ComponentName))
			{
				const TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
				Ref->SetStringField(TEXT("component"), ComponentName);
				Template = MakeShared<FJsonValueObject>(Ref);
			}
			else
			{
				const TSharedPtr<FJsonObject>* Schema = nullptr;
				if (!Field->TryGetObjectField(TEXT("schema"), Schema)
					|| !Schema || !Schema->IsValid())
				{
					OutError = TEXT("timeline_fixture_field_schema_missing");
					return false;
				}
				Template = MakeShared<FJsonValueObject>(*Schema);
			}
			TSet<FString> Stack;
			TSharedPtr<FJsonValue> Expanded = ExpandValue(
				Template, Components, Stack, OutError);
			if (!Expanded.IsValid())
			{
				return false;
			}
			Properties->SetField(Name, MoveTemp(Expanded));
			if (bRequired)
			{
				OutRequired.Add(Name);
				RequiredValues.Add(MakeShared<FJsonValueString>(Name));
			}
			else
			{
				OutOptional.Add(Name);
			}
		}
		const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), TEXT("object"));
		Root->SetObjectField(TEXT("properties"), Properties);
		if (!RequiredValues.IsEmpty())
		{
			Root->SetArrayField(TEXT("required"), MoveTemp(RequiredValues));
		}
		Root->SetBoolField(TEXT("additionalProperties"), false);
		return Json::TryCanonicalize(Root, OutCanonical, OutError);
	}

	TSet<FString> StringSet(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field)
	{
		TSet<FString> Result;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Object->TryGetArrayField(Field, Values) && Values)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				Result.Add(Value->AsString());
			}
		}
		return Result;
	}

	FString SetText(const TSet<FString>& Values)
	{
		TArray<FString> Sorted = Values.Array();
		Sorted.Sort();
		return FString::Join(Sorted, TEXT("|"));
	}

	void CheckClosedObjects(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonValue>& Value,
		const FString& Path)
	{
		if (!Value.IsValid())
		{
			Test->AddError(TEXT("invalid expanded schema: ") + Path);
			return;
		}
		if (Value->Type == EJson::Array)
		{
			int32 Index = 0;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				CheckClosedObjects(
					Test, Item, FString::Printf(TEXT("%s[%d]"), *Path, Index++));
			}
			return;
		}
		if (Value->Type != EJson::Object)
		{
			return;
		}
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		FString Type;
		if (Object->TryGetStringField(TEXT("type"), Type) && Type == TEXT("object"))
		{
			bool bAdditional = true;
			Test->TestTrue(
				TEXT("nested object is closed: ") + Path,
				Object->TryGetBoolField(TEXT("additionalProperties"), bAdditional)
					&& !bAdditional);
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			CheckClosedObjects(Test, Pair.Value, Path + TEXT(".") + Pair.Key);
		}
	}

	TSharedPtr<FJsonObject> BuildExpectedDescriptor(
		const TSharedPtr<FJsonObject>& Profile,
		const FString& InputSchema,
		const FString& ResultSchema)
	{
		TSharedPtr<FJsonObject> Input;
		TSharedPtr<FJsonObject> Result;
		FString Error;
		if (!Json::TryParseObject(InputSchema, Input, Error)
			|| !Json::TryParseObject(ResultSchema, Result, Error))
		{
			return nullptr;
		}
		const FString OperationKind = Profile->GetStringField(TEXT("operation_kind"));
		const TSharedPtr<FJsonObject> Policy = MakeShared<FJsonObject>();
		Policy->SetArrayField(
			TEXT("allowed_state_planes"), Profile->GetArrayField(TEXT("state_planes")));
		Policy->SetArrayField(
			TEXT("required_facets"), Profile->GetArrayField(TEXT("required_facets")));
		Policy->SetStringField(TEXT("resolution_policy"), TEXT("explicit_only"));
		Policy->SetStringField(
			TEXT("transient_access_policy"),
			OperationKind == TEXT("query")
				? TEXT("read_only")
				: TEXT("temporary_mutation_allowed"));

		const TSharedPtr<FJsonObject> Descriptor = MakeShared<FJsonObject>();
		Descriptor->SetStringField(
			TEXT("capability_id"), Profile->GetStringField(TEXT("capability_id")));
		Descriptor->SetStringField(
			TEXT("descriptor_id"), Profile->GetStringField(TEXT("descriptor_id")));
		Descriptor->SetStringField(TEXT("domain_id"), TEXT("rig_simulation_spatial"));
		Descriptor->SetObjectField(TEXT("execution_context_policy"), Policy);
		Descriptor->SetObjectField(TEXT("input_schema"), Input);
		Descriptor->SetStringField(TEXT("operation_kind"), OperationKind);
		if (OperationKind == TEXT("query"))
		{
			const TSharedPtr<FJsonObject> Query = MakeShared<FJsonObject>();
			Query->SetStringField(TEXT("materialization_policy"), TEXT("runtime_observe"));
			Query->SetStringField(
				TEXT("provider_binding"), Profile->GetStringField(TEXT("provider_binding")));
			Query->SetStringField(
				TEXT("query_kind"), Profile->GetStringField(TEXT("query_kind")));
			Query->SetStringField(TEXT("side_effect_policy"), TEXT("no_persistent_mutation"));
			Descriptor->SetObjectField(TEXT("query"), Query);
		}
		Descriptor->SetObjectField(TEXT("result_schema"), Result);
		Descriptor->SetField(TEXT("version"), MakeShared<FJsonValueNumberString>(TEXT("1")));
		return Descriptor;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentRigSimulationSpatialExactCapabilityDescriptorsTest,
	"UeAgentInterface.DomainAdapters.RigSimulationSpatial.E1.ExactCapabilityDescriptors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentRigSimulationSpatialExactCapabilityDescriptorsTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadObject(
		this,
		TEXT("DomainAdapters/RigSimulationSpatial/Fixtures/"
			"rig_simulation_spatial_exact_contract_cases.json"),
		TEXT("RigSimulationSpatial exact fixture"));
	const TSharedPtr<FJsonObject> Activation = LoadObject(
		this,
		TEXT("Composition/TimelineSpatial/Fixtures/"
			"timeline_spatial_activation_contract_cases.json"),
		TEXT("TimelineSpatial activation fixture"));
	if (!Fixture.IsValid() || !Activation.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("fixture temporal owner"),
		Fixture->GetStringField(TEXT("temporal_contract_id")),
		FString(TEXT("uai.temporal_contract.v1")));
	TestEqual(
		TEXT("fixture space owner"),
		Fixture->GetStringField(TEXT("space_contract_id")),
		FString(TEXT("uai.space_contract.v1")));
	TestEqual(
		TEXT("fixture typed port owner"),
		Fixture->GetStringField(TEXT("typed_port_id")),
		FString(TEXT("uai.rig_simulation_spatial.typed_port.v1")));

	TMap<FString, TSharedPtr<FJsonObject>> ActivationByCapability;
	for (const TSharedPtr<FJsonValue>& Value : Activation->GetArrayField(TEXT("profiles")))
	{
		const TSharedPtr<FJsonObject> Profile = Value->AsObject();
		if (Profile.IsValid()
			&& Profile->GetStringField(TEXT("adapter_id")) == TEXT("rig_simulation_spatial"))
		{
			ActivationByCapability.Add(
				Profile->GetStringField(TEXT("capability_id")), Profile);
		}
	}

	const TSharedPtr<FJsonObject> Components = Fixture->GetObjectField(TEXT("components"));
	const TArray<TSharedPtr<FJsonValue>>& Profiles =
		Fixture->GetArrayField(TEXT("descriptors"));
	TMap<FString, FString> ExpectedInput;
	TMap<FString, FString> ExpectedResult;
	TMap<FString, FString> ExpectedHash;
	TSet<FString> DescriptorIds;
	TSet<FString> Providers;
	for (const TSharedPtr<FJsonValue>& Value : Profiles)
	{
		const TSharedPtr<FJsonObject> Profile = Value->AsObject();
		if (!TestTrue(TEXT("descriptor profile is object"), Profile.IsValid()))
		{
			continue;
		}
		const FString CapabilityId = Profile->GetStringField(TEXT("capability_id"));
		const FString DescriptorId = Profile->GetStringField(TEXT("descriptor_id"));
		const FString OperationKind = Profile->GetStringField(TEXT("operation_kind"));
		TestFalse(TEXT("descriptor id unique: ") + DescriptorId, DescriptorIds.Contains(DescriptorId));
		DescriptorIds.Add(DescriptorId);
		const TSharedPtr<FJsonObject>* ActivationProfile =
			ActivationByCapability.Find(CapabilityId);
		TestNotNull(TEXT("activation profile exists: ") + CapabilityId, ActivationProfile);
		if (!ActivationProfile)
		{
			continue;
		}
		TestEqual(
			TEXT("descriptor identity matches activation"), DescriptorId,
			(*ActivationProfile)->GetStringField(TEXT("descriptor_id")));
		TestEqual(
			TEXT("oracle matches activation"),
			Profile->GetStringField(TEXT("oracle_kind")),
			(*ActivationProfile)->GetStringField(TEXT("oracle_kind")));

		FString InputSchema;
		FString ResultSchema;
		FString Error;
		TSet<FString> RequiredInput;
		TSet<FString> OptionalInput;
		TSet<FString> RequiredResult;
		TSet<FString> OptionalResult;
		TestTrue(
			TEXT("build exact input schema: ") + CapabilityId,
			TryBuildSchema(
				Profile->GetArrayField(TEXT("input_fields")), Components,
				InputSchema, RequiredInput, OptionalInput, Error));
		TestTrue(
			TEXT("build exact result schema: ") + CapabilityId,
			TryBuildSchema(
				Profile->GetArrayField(TEXT("result_fields")), Components,
				ResultSchema, RequiredResult, OptionalResult, Error));
		TestEqual(
			TEXT("required fields match activation"), SetText(RequiredInput),
			SetText(StringSet(*ActivationProfile, TEXT("input_required"))));
		TestEqual(
			TEXT("optional fields match activation"), SetText(OptionalInput),
			SetText(StringSet(*ActivationProfile, TEXT("input_optional"))));
		TestTrue(TEXT("result is non-empty"), !RequiredResult.IsEmpty());

		TSharedPtr<const FUeAgentContractSchema> CompiledInput;
		TSharedPtr<const FUeAgentContractSchema> CompiledResult;
		FUeAgentContractSchemaError SchemaError;
		TestTrue(
			TEXT("compile input schema: ") + CapabilityId,
			FUeAgentContractSchema::TryCompile(
				FUeAgentContractValue(InputSchema), CompiledInput, SchemaError));
		TestTrue(
			TEXT("compile result schema: ") + CapabilityId,
			FUeAgentContractSchema::TryCompile(
				FUeAgentContractValue(ResultSchema), CompiledResult, SchemaError));
		TSharedPtr<FJsonObject> InputObject;
		TSharedPtr<FJsonObject> ResultObject;
		FString ParseError;
		Json::TryParseObject(InputSchema, InputObject, ParseError);
		Json::TryParseObject(ResultSchema, ResultObject, ParseError);
		CheckClosedObjects(
			this, MakeShared<FJsonValueObject>(InputObject), CapabilityId + TEXT(".input"));
		CheckClosedObjects(
			this, MakeShared<FJsonValueObject>(ResultObject), CapabilityId + TEXT(".result"));

		const TSharedPtr<FJsonObject> Descriptor =
			BuildExpectedDescriptor(Profile, InputSchema, ResultSchema);
		FString CanonicalDescriptor;
		TestTrue(
			TEXT("canonicalize expected descriptor: ") + CapabilityId,
			Descriptor.IsValid()
				&& Json::TryCanonicalize(Descriptor, CanonicalDescriptor, Error));
		ExpectedInput.Add(CapabilityId, InputSchema);
		ExpectedResult.Add(CapabilityId, ResultSchema);
		ExpectedHash.Add(CapabilityId, Json::Sha256Utf8(CanonicalDescriptor));

		if (OperationKind == TEXT("query"))
		{
			const FString Provider = Profile->GetStringField(TEXT("provider_binding"));
			TestFalse(TEXT("provider unique: ") + Provider, Providers.Contains(Provider));
			Providers.Add(Provider);
		}
		else
		{
		}
	}

	using namespace UE::UAI::DomainAdapters::RigSimulationSpatial;
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	if (!TestTrue(
			TEXT("create production RigSimulationSpatial descriptors"),
			TryCreateRigSimulationSpatialDescriptorRegistry(Registry, RegistryError))
		|| !Registry.IsValid())
	{
		AddError(TEXT("rig_simulation_spatial_descriptor_registry_unavailable:") + RegistryError);
		return false;
	}
	for (const FUeAgentOperationDescriptor& Descriptor : Registry->GetDescriptors())
	{
		const FString CapabilityId = Descriptor.GetCapabilityId();
		const FString* Input = ExpectedInput.Find(CapabilityId);
		const FString* Result = ExpectedResult.Find(CapabilityId);
		const FString* Hash = ExpectedHash.Find(CapabilityId);
		TestNotNull(TEXT("production capability belongs to fixture: ") + CapabilityId, Input);
		if (!Input || !Result || !Hash)
		{
			continue;
		}
		TestEqual(TEXT("exact production input"), Descriptor.GetInputSchema().GetJsonValue(), *Input);
		TestEqual(TEXT("exact production result"), Descriptor.GetResultSchema().GetJsonValue(), *Result);
		TestEqual(
			TEXT("descriptor hash derives from exact contract"),
			Descriptor.GetIdentity().GetDescriptorHash(), *Hash);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentRigSimulationSpatialExecutionChainContractTest,
	"UeAgentInterface.DomainAdapters.RigSimulationSpatial.E1.ExecutionChainContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentRigSimulationSpatialExecutionChainContractTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadObject(
		this,
		TEXT("DomainAdapters/RigSimulationSpatial/Fixtures/"
			"rig_simulation_spatial_exact_contract_cases.json"),
		TEXT("RigSimulationSpatial exact fixture"));
	const TSharedPtr<FJsonObject> Activation = LoadObject(
		this,
		TEXT("Composition/TimelineSpatial/Fixtures/"
			"timeline_spatial_activation_contract_cases.json"),
		TEXT("TimelineSpatial activation fixture"));
	if (!Fixture.IsValid() || !Activation.IsValid())
	{
		return false;
	}
	TMap<FString, TSharedPtr<FJsonObject>> ActivationByCapability;
	for (const TSharedPtr<FJsonValue>& Value : Activation->GetArrayField(TEXT("profiles")))
	{
		const TSharedPtr<FJsonObject> Profile = Value->AsObject();
		if (Profile.IsValid()
			&& Profile->GetStringField(TEXT("adapter_id")) == TEXT("rig_simulation_spatial"))
		{
			ActivationByCapability.Add(Profile->GetStringField(TEXT("capability_id")), Profile);
		}
	}
	const TArray<TSharedPtr<FJsonValue>>& ChainValues = Fixture->GetArrayField(TEXT("chains"));
	TSet<FString> TypedOperations;
	TSet<FString> TypedRequests;
	TSet<FString> TypedResults;
	int32 Queries = 0;
	int32 Operations = 0;
	for (const TSharedPtr<FJsonValue>& Value : ChainValues)
	{
		const TSharedPtr<FJsonObject> Chain = Value->AsObject();
		const FString CapabilityId = Chain->GetStringField(TEXT("capability_id"));
		const TSharedPtr<FJsonObject>* ActivationProfile = ActivationByCapability.Find(CapabilityId);
		TestNotNull(TEXT("activation chain profile exists: ") + CapabilityId, ActivationProfile);
		if (!ActivationProfile)
		{
			continue;
		}
		const FString TypedOperation = Chain->GetStringField(TEXT("typed_operation_id"));
		const FString TypedRequest = Chain->GetStringField(TEXT("typed_request_id"));
		const FString TypedResult = Chain->GetStringField(TEXT("typed_result_id"));
		TestEqual(
			TEXT("typed operation matches activation"), TypedOperation,
			(*ActivationProfile)->GetStringField(TEXT("typed_operation_id")));
		TestEqual(
			TEXT("binding matches activation"), Chain->GetStringField(TEXT("binding_id")),
			(*ActivationProfile)->GetStringField(TEXT("provider_binding")));
		TestEqual(
			TEXT("family matches activation"), Chain->GetStringField(TEXT("family_id")),
			(*ActivationProfile)->GetStringField(TEXT("family_id")));
		TestFalse(TEXT("typed operation unique"), TypedOperations.Contains(TypedOperation));
		TestFalse(TEXT("typed request unique"), TypedRequests.Contains(TypedRequest));
		TestFalse(TEXT("typed result unique"), TypedResults.Contains(TypedResult));
		TypedOperations.Add(TypedOperation);
		TypedRequests.Add(TypedRequest);
		TypedResults.Add(TypedResult);
		if ((*ActivationProfile)->GetStringField(TEXT("operation_kind")) == TEXT("query"))
		{
			++Queries;
			TestEqual(
				TEXT("query has no settle barrier"),
				Chain->GetStringField(TEXT("settle_barrier")), FString(TEXT("none")));
		}
		else
		{
			++Operations;
			TestNotEqual(
				TEXT("operation settle policy is explicit"),
				Chain->GetStringField(TEXT("settle_barrier")), FString(TEXT("none")));
		}
	}
	TestEqual(TEXT("5 C3Q chains"), Queries, 5);
	TestEqual(TEXT("13 C3O chains"), Operations, 13);

	using namespace UE::UAI::DomainAdapters::RigSimulationSpatial;
	const TArray<FUeAgentRigSimulationSpatialExecutionChainContract>& Contracts =
		GetRigSimulationSpatialExecutionChainContracts();
	for (const FUeAgentRigSimulationSpatialExecutionChainContract& Contract : Contracts)
	{
		const TSharedPtr<FJsonValue>* Match = ChainValues.FindByPredicate(
			[&Contract](const TSharedPtr<FJsonValue>& Candidate)
			{
				return Candidate->AsObject()->GetStringField(TEXT("capability_id"))
					== Contract.GetCapabilityId();
			});
		TestNotNull(TEXT("production chain belongs to fixture"), Match);
		if (!Match)
		{
			continue;
		}
		const TSharedPtr<FJsonObject> FixtureChain = (*Match)->AsObject();
		TestEqual(TEXT("production typed operation"), Contract.GetTypedOperationId(), FixtureChain->GetStringField(TEXT("typed_operation_id")));
		TestEqual(TEXT("production typed request"), Contract.GetTypedRequestId(), FixtureChain->GetStringField(TEXT("typed_request_id")));
		TestEqual(TEXT("production typed result"), Contract.GetTypedResultId(), FixtureChain->GetStringField(TEXT("typed_result_id")));
		TestEqual(TEXT("production family"), Contract.GetFamilyId(), FixtureChain->GetStringField(TEXT("family_id")));
		TestEqual(TEXT("production settle barrier"), Contract.GetSettleBarrier(), FixtureChain->GetStringField(TEXT("settle_barrier")));
		TestEqual(TEXT("C4 temporal owner"), Contract.GetTemporalContractId(), FString(TEXT("uai.temporal_contract.v1")));
		TestEqual(TEXT("C4 space owner"), Contract.GetSpaceContractId(), FString(TEXT("uai.space_contract.v1")));
		TestEqual(TEXT("formal typed port owner"), Contract.GetTypedPortId(), FString(TEXT("uai.rig_simulation_spatial.typed_port.v1")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentRigSimulationSpatialActivationBindingTest,
	"UeAgentInterface.DomainAdapters.RigSimulationSpatial.E3.ActivationBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentRigSimulationSpatialActivationBindingTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Activation = LoadObject(
		this,
		TEXT("Composition/TimelineSpatial/Fixtures/"
			"timeline_spatial_activation_contract_cases.json"),
		TEXT("TimelineSpatial activation fixture"));
	if (!Activation.IsValid())
	{
		return false;
	}

	using namespace UE::UAI::DomainAdapters::RigSimulationSpatial;
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	if (!TestTrue(
			TEXT("create RigSimulationSpatial E3 descriptor input"),
			TryCreateRigSimulationSpatialDescriptorRegistry(
				Registry,
				RegistryError))
		|| !Registry.IsValid())
	{
		AddError(
			TEXT("rig_simulation_spatial_e3_registry_unavailable:")
			+ RegistryError);
		return false;
	}

	const TArray<FUeAgentRigSimulationSpatialExecutionChainContract>& Contracts =
		GetRigSimulationSpatialExecutionChainContracts();
	TSet<FString> MatchedCapabilities;
	for (const TSharedPtr<FJsonValue>& Value
		: Activation->GetArrayField(TEXT("profiles")))
	{
		const TSharedPtr<FJsonObject> Profile = Value->AsObject();
		if (!Profile.IsValid()
			|| Profile->GetStringField(TEXT("adapter_id"))
				!= TEXT("rig_simulation_spatial"))
		{
			continue;
		}
		const FString CapabilityId =
			Profile->GetStringField(TEXT("capability_id"));
		MatchedCapabilities.Add(CapabilityId);
		const FUeAgentOperationDescriptor* Descriptor =
			Registry->GetDescriptors().FindByPredicate(
				[&CapabilityId](const FUeAgentOperationDescriptor& Candidate)
				{
					return Candidate.GetCapabilityId() == CapabilityId;
				});
		const FUeAgentRigSimulationSpatialExecutionChainContract* Contract =
			Contracts.FindByPredicate(
				[&CapabilityId](
					const FUeAgentRigSimulationSpatialExecutionChainContract&
						Candidate)
				{
					return Candidate.GetCapabilityId() == CapabilityId;
				});
		TestNotNull(
			TEXT("E3 descriptor exists: ") + CapabilityId,
			Descriptor);
		TestNotNull(
			TEXT("E3 execution chain exists: ") + CapabilityId,
			Contract);
		if (!Descriptor || !Contract)
		{
			continue;
		}
		TestEqual(
			TEXT("E3 descriptor identity"),
			Descriptor->GetIdentity().GetDescriptorId(),
			Profile->GetStringField(TEXT("descriptor_id")));
		TestEqual(
			TEXT("E3 exact binding"),
			Contract->GetBindingId(),
			Profile->GetStringField(TEXT("provider_binding")));
		TestEqual(
			TEXT("E3 exact family"),
			Contract->GetFamilyId(),
			Profile->GetStringField(TEXT("family_id")));
		TestEqual(
			TEXT("E3 exact typed operation"),
			Contract->GetTypedOperationId(),
			Profile->GetStringField(TEXT("typed_operation_id")));
		TestEqual(
			TEXT("E3 C3 lane"),
			Contract->GetC3Lane(),
			Profile->GetStringField(TEXT("operation_kind"))
				== TEXT("query")
					? FString(TEXT("c3q"))
					: FString(TEXT("c3o")));
		TestEqual(
			TEXT("E3 formal typed port"),
			Contract->GetTypedPortId(),
			FString(TEXT("uai.rig_simulation_spatial.typed_port.v1")));
		TestEqual(
			TEXT("E3 temporal owner"),
			Contract->GetTemporalContractId(),
			FString(TEXT("uai.temporal_contract.v1")));
		TestEqual(
			TEXT("E3 spatial owner"),
			Contract->GetSpaceContractId(),
			FString(TEXT("uai.space_contract.v1")));
	}
	for (const FUeAgentRigSimulationSpatialExecutionChainContract& Contract
		: Contracts)
	{
		TestTrue(
			TEXT("activation fixture covers execution-chain capability: ")
				+ Contract.GetCapabilityId(),
			MatchedCapabilities.Contains(Contract.GetCapabilityId()));
	}
	TestEqual(
		TEXT("activation fixture and execution-chain contract cover the same capabilities"),
		MatchedCapabilities.Num(),
		Contracts.Num());
	return true;
}
