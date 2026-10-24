#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialDescriptors.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"

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
				FFileHelper::LoadFileToString(
					Text,
					*FixturePath(RelativePath))))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				FString(TEXT("parse ")) + Label,
				FJsonSerializer::Deserialize(Reader, Object)
					&& Object.IsValid()))
		{
			return nullptr;
		}
		return Object;
	}

	TSharedPtr<FJsonValue> ExpandValue(
		const TSharedPtr<FJsonValue>& Value,
		const TSharedPtr<FJsonObject>& Components,
		TSet<FString>& ExpansionStack,
		FString& OutError)
	{
		if (!Value.IsValid())
		{
			OutError = TEXT("fixture_schema_value_invalid");
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
					Item,
					Components,
					ExpansionStack,
					OutError);
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
			FString ComponentName;
			if (Object.IsValid()
				&& Object->TryGetStringField(
					TEXT("component"),
					ComponentName))
			{
				if (Object->Values.Num() != 1
					|| ComponentName.IsEmpty())
				{
					OutError = TEXT("fixture_component_reference_invalid");
					return nullptr;
				}
				const TSharedPtr<FJsonValue>* Definition =
					Components->Values.Find(ComponentName);
				if (!Definition || !Definition->IsValid())
				{
					OutError = TEXT("fixture_component_missing:")
						+ ComponentName;
					return nullptr;
				}
				if (ExpansionStack.Contains(ComponentName))
				{
					OutError = TEXT("fixture_component_cycle:")
						+ ComponentName;
					return nullptr;
				}
				ExpansionStack.Add(ComponentName);
				TSharedPtr<FJsonValue> Expanded = ExpandValue(
					*Definition,
					Components,
					ExpansionStack,
					OutError);
				ExpansionStack.Remove(ComponentName);
				return Expanded;
			}

			const TSharedPtr<FJsonObject> ExpandedObject =
				MakeShared<FJsonObject>();
			if (!Object.IsValid())
			{
				OutError = TEXT("fixture_schema_object_invalid");
				return nullptr;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
				: Object->Values)
			{
				TSharedPtr<FJsonValue> ExpandedField = ExpandValue(
					Pair.Value,
					Components,
					ExpansionStack,
					OutError);
				if (!ExpandedField.IsValid())
				{
					return nullptr;
				}
				ExpandedObject->SetField(
					Pair.Key,
					MoveTemp(ExpandedField));
			}
			return MakeShared<FJsonValueObject>(ExpandedObject);
		}
		default:
			OutError = TEXT("fixture_schema_json_kind_invalid");
			return nullptr;
		}
	}

	TSharedPtr<FJsonValue> ExpandFieldSchema(
		const TSharedPtr<FJsonObject>& Field,
		const TSharedPtr<FJsonObject>& Components,
		FString& OutError)
	{
		TSharedPtr<FJsonValue> Template;
		FString ComponentName;
		if (Field->TryGetStringField(TEXT("component"), ComponentName))
		{
			const TSharedPtr<FJsonObject> Reference =
				MakeShared<FJsonObject>();
			Reference->SetStringField(TEXT("component"), ComponentName);
			Template = MakeShared<FJsonValueObject>(Reference);
		}
		else
		{
			const TSharedPtr<FJsonObject>* Schema = nullptr;
			if (!Field->TryGetObjectField(TEXT("schema"), Schema)
				|| !Schema
				|| !Schema->IsValid())
			{
				OutError = TEXT("fixture_field_schema_missing");
				return nullptr;
			}
			Template = MakeShared<FJsonValueObject>(*Schema);
		}
		TSet<FString> Stack;
		return ExpandValue(
			Template,
			Components,
			Stack,
			OutError);
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
		const TSharedPtr<FJsonObject> Properties =
			MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> RequiredValues;
		for (const TSharedPtr<FJsonValue>& FieldValue : Fields)
		{
			const TSharedPtr<FJsonObject> Field =
				FieldValue.IsValid() ? FieldValue->AsObject() : nullptr;
			if (!Field.IsValid())
			{
				OutError = TEXT("fixture_field_invalid");
				return false;
			}
			FString Name;
			bool bRequired = false;
			if (!Field->TryGetStringField(TEXT("name"), Name)
				|| Name.IsEmpty()
				|| !Field->TryGetBoolField(TEXT("required"), bRequired)
				|| Properties->HasField(Name))
			{
				OutError = TEXT("fixture_field_identity_invalid");
				return false;
			}
			TSharedPtr<FJsonValue> Schema = ExpandFieldSchema(
				Field,
				Components,
				OutError);
			if (!Schema.IsValid())
			{
				return false;
			}
			Properties->SetField(Name, MoveTemp(Schema));
			if (bRequired)
			{
				OutRequired.Add(Name);
				RequiredValues.Add(
					MakeShared<FJsonValueString>(Name));
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
		return UE::UAI::Contract::Json::TryCanonicalize(
			Root,
			OutCanonical,
			OutError);
	}

	void CheckClosedObjectSchemas(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonValue>& Value,
		const FString& Path)
	{
		if (!Value.IsValid())
		{
			Test->AddError(TEXT("invalid expanded schema at ") + Path);
			return;
		}
		if (Value->Type == EJson::Array)
		{
			int32 Index = 0;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				CheckClosedObjectSchemas(
					Test,
					Item,
					FString::Printf(TEXT("%s[%d]"), *Path, Index++));
			}
			return;
		}
		if (Value->Type != EJson::Object)
		{
			return;
		}
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		FString Type;
		if (Object->TryGetStringField(TEXT("type"), Type)
			&& Type == TEXT("object"))
		{
			bool bAdditional = true;
			Test->TestTrue(
				TEXT("object schema closes additional properties: ") + Path,
				Object->TryGetBoolField(
					TEXT("additionalProperties"),
					bAdditional)
					&& !bAdditional);
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
			: Object->Values)
		{
			CheckClosedObjectSchemas(
				Test,
				Pair.Value,
				Path + TEXT(".") + Pair.Key);
		}
	}

	TSet<FString> StringSet(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field)
	{
		TSet<FString> Result;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(Field, Values) || !Values)
		{
			return Result;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			Result.Add(Value->AsString());
		}
		return Result;
	}

	FString SetText(const TSet<FString>& Values)
	{
		TArray<FString> Sorted = Values.Array();
		Sorted.Sort();
		return FString::Join(Sorted, TEXT("|"));
	}

	TSharedPtr<FJsonObject> BuildExpectedDescriptorObject(
		const TSharedPtr<FJsonObject>& Profile,
		const FString& InputSchema,
		const FString& ResultSchema)
	{
		TSharedPtr<FJsonObject> Input;
		TSharedPtr<FJsonObject> Result;
		FString ParseError;
		if (!UE::UAI::Contract::Json::TryParseObject(
				InputSchema,
				Input,
				ParseError)
			|| !UE::UAI::Contract::Json::TryParseObject(
				ResultSchema,
				Result,
				ParseError))
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject> Policy = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> StatePlanes;
		for (const TSharedPtr<FJsonValue>& Value
			: Profile->GetArrayField(TEXT("state_planes")))
		{
			StatePlanes.Add(
				MakeShared<FJsonValueString>(Value->AsString()));
		}
		TArray<TSharedPtr<FJsonValue>> RequiredFacets;
		for (const TSharedPtr<FJsonValue>& Value
			: Profile->GetArrayField(TEXT("required_facets")))
		{
			RequiredFacets.Add(
				MakeShared<FJsonValueString>(Value->AsString()));
		}
		Policy->SetArrayField(
			TEXT("allowed_state_planes"),
			MoveTemp(StatePlanes));
		Policy->SetArrayField(
			TEXT("required_facets"),
			MoveTemp(RequiredFacets));
		Policy->SetStringField(
			TEXT("resolution_policy"),
			Profile->GetStringField(TEXT("resolution_policy")));
		Policy->SetStringField(
			TEXT("transient_access_policy"),
			Profile->GetStringField(TEXT("transient_access_policy")));

		const TSharedPtr<FJsonObject> Descriptor =
			MakeShared<FJsonObject>();
		Descriptor->SetStringField(
			TEXT("capability_id"),
			Profile->GetStringField(TEXT("capability_id")));
		Descriptor->SetStringField(
			TEXT("descriptor_id"),
			Profile->GetStringField(TEXT("descriptor_id")));
		Descriptor->SetStringField(TEXT("domain_id"), TEXT("world_spatial"));
		Descriptor->SetObjectField(TEXT("execution_context_policy"), Policy);
		Descriptor->SetObjectField(TEXT("input_schema"), Input);
		const FString OperationKind =
			Profile->GetStringField(TEXT("operation_kind"));
		Descriptor->SetStringField(TEXT("operation_kind"), OperationKind);
		if (OperationKind == TEXT("query"))
		{
			const TSharedPtr<FJsonObject> Query = MakeShared<FJsonObject>();
			Query->SetStringField(
				TEXT("materialization_policy"),
				TEXT("runtime_observe"));
			Query->SetStringField(
				TEXT("provider_binding"),
				Profile->GetStringField(TEXT("provider_binding")));
			Query->SetStringField(
				TEXT("query_kind"),
				Profile->GetStringField(TEXT("query_kind")));
			Query->SetStringField(
				TEXT("side_effect_policy"),
				TEXT("no_persistent_mutation"));
			Descriptor->SetObjectField(TEXT("query"), Query);
		}
		Descriptor->SetObjectField(TEXT("result_schema"), Result);
		Descriptor->SetField(
			TEXT("version"),
			MakeShared<FJsonValueNumberString>(TEXT("1")));
		return Descriptor;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialExactCapabilityDescriptorsTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E1.ExactCapabilityDescriptors",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialExactCapabilityDescriptorsTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadObject(
		this,
		TEXT("DomainAdapters/WorldSpatial/Fixtures/"
			"world_spatial_exact_descriptor_cases.json"),
		TEXT("WorldSpatial exact descriptor fixture"));
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
		TEXT("fixture schema"),
		Fixture->GetStringField(TEXT("schema")),
		FString(TEXT(
			"uai.domain_adapter.world_spatial.exact_descriptor_fixture.v1")));
	const TSharedPtr<FJsonObject> Components =
		Fixture->GetObjectField(TEXT("components"));
	const TArray<TSharedPtr<FJsonValue>>& DescriptorValues =
		Fixture->GetArrayField(TEXT("descriptors"));

	TMap<FString, TSharedPtr<FJsonObject>> ActivationByCapability;
	for (const TSharedPtr<FJsonValue>& Value
		: Activation->GetArrayField(TEXT("profiles")))
	{
		const TSharedPtr<FJsonObject> Profile = Value->AsObject();
		if (Profile.IsValid()
			&& Profile->GetStringField(TEXT("adapter_id"))
				== TEXT("world_spatial"))
		{
			ActivationByCapability.Add(
				Profile->GetStringField(TEXT("capability_id")),
				Profile);
		}
	}
	TSet<FString> CapabilityIds;
	TSet<FString> DescriptorIds;
	TSet<FString> ProviderBindings;
	TMap<FString, FString> ExpectedInputSchemas;
	TMap<FString, FString> ExpectedResultSchemas;
	TMap<FString, FString> ExpectedDescriptorHashes;
	for (const TSharedPtr<FJsonValue>& Value : DescriptorValues)
	{
		const TSharedPtr<FJsonObject> Profile = Value->AsObject();
		if (!TestTrue(TEXT("descriptor profile is object"), Profile.IsValid()))
		{
			continue;
		}
		const FString CapabilityId =
			Profile->GetStringField(TEXT("capability_id"));
		const FString DescriptorId =
			Profile->GetStringField(TEXT("descriptor_id"));
		const FString OperationKind =
			Profile->GetStringField(TEXT("operation_kind"));
		TestFalse(
			TEXT("capability identity unique: ") + CapabilityId,
			CapabilityIds.Contains(CapabilityId));
		TestFalse(
			TEXT("descriptor identity unique: ") + DescriptorId,
			DescriptorIds.Contains(DescriptorId));
		CapabilityIds.Add(CapabilityId);
		DescriptorIds.Add(DescriptorId);

		const TSharedPtr<FJsonObject>* ActivationProfile =
			ActivationByCapability.Find(CapabilityId);
		TestNotNull(
			TEXT("activation profile exists: ") + CapabilityId,
			ActivationProfile);
		if (!ActivationProfile)
		{
			continue;
		}
		TestEqual(
			TEXT("descriptor identity matches activation"),
			DescriptorId,
			(*ActivationProfile)->GetStringField(TEXT("descriptor_id")));
		TestEqual(
			TEXT("operation kind matches activation"),
			OperationKind,
			(*ActivationProfile)->GetStringField(TEXT("operation_kind")));
		TestEqual(
			TEXT("oracle matches activation"),
			Profile->GetStringField(TEXT("oracle_kind")),
			(*ActivationProfile)->GetStringField(TEXT("oracle_kind")));

		FString InputSchema;
		FString ResultSchema;
		FString Error;
		TSet<FString> InputRequired;
		TSet<FString> InputOptional;
		TSet<FString> ResultRequired;
		TSet<FString> ResultOptional;
		TestTrue(
			TEXT("build exact input schema: ") + CapabilityId,
			TryBuildSchema(
				Profile->GetArrayField(TEXT("input_fields")),
				Components,
				InputSchema,
				InputRequired,
				InputOptional,
				Error));
		TestTrue(
			TEXT("build exact result schema: ") + CapabilityId,
			TryBuildSchema(
				Profile->GetArrayField(TEXT("result_fields")),
				Components,
				ResultSchema,
				ResultRequired,
				ResultOptional,
				Error));
		TestEqual(
			TEXT("required input fields match activation"),
			SetText(InputRequired),
			SetText(StringSet(*ActivationProfile, TEXT("input_required"))));
		TestEqual(
			TEXT("optional input fields match activation"),
			SetText(InputOptional),
			SetText(StringSet(*ActivationProfile, TEXT("input_optional"))));
		TestTrue(
			TEXT("result schema exposes an exact result: ") + CapabilityId,
			!ResultRequired.IsEmpty());

		TSharedPtr<const FUeAgentContractSchema> CompiledInput;
		TSharedPtr<const FUeAgentContractSchema> CompiledResult;
		FUeAgentContractSchemaError SchemaError;
		TestTrue(
			TEXT("compile exact input schema: ") + CapabilityId,
			FUeAgentContractSchema::TryCompile(
				FUeAgentContractValue(InputSchema),
				CompiledInput,
				SchemaError));
		TestTrue(
			TEXT("compile exact result schema: ") + CapabilityId,
			FUeAgentContractSchema::TryCompile(
				FUeAgentContractValue(ResultSchema),
				CompiledResult,
				SchemaError));

		TSharedPtr<FJsonObject> InputObject;
		TSharedPtr<FJsonObject> ResultObject;
		FString ParseError;
		UE::UAI::Contract::Json::TryParseObject(
			InputSchema,
			InputObject,
			ParseError);
		UE::UAI::Contract::Json::TryParseObject(
			ResultSchema,
			ResultObject,
			ParseError);
		CheckClosedObjectSchemas(
			this,
			MakeShared<FJsonValueObject>(InputObject),
			CapabilityId + TEXT(".input"));
		CheckClosedObjectSchemas(
			this,
			MakeShared<FJsonValueObject>(ResultObject),
			CapabilityId + TEXT(".result"));

		const TSharedPtr<FJsonObject> ExpectedDescriptor =
			BuildExpectedDescriptorObject(Profile, InputSchema, ResultSchema);
		FString CanonicalDescriptor;
		TestTrue(
			TEXT("canonicalize expected descriptor: ") + CapabilityId,
			ExpectedDescriptor.IsValid()
				&& UE::UAI::Contract::Json::TryCanonicalize(
					ExpectedDescriptor,
					CanonicalDescriptor,
					Error));
		ExpectedInputSchemas.Add(CapabilityId, InputSchema);
		ExpectedResultSchemas.Add(CapabilityId, ResultSchema);
		ExpectedDescriptorHashes.Add(
			CapabilityId,
			UE::UAI::Contract::Json::Sha256Utf8(CanonicalDescriptor));

		if (OperationKind == TEXT("query"))
		{
			const FString Provider =
				Profile->GetStringField(TEXT("provider_binding"));
			TestFalse(
				TEXT("query provider binding unique: ") + Provider,
				ProviderBindings.Contains(Provider));
			ProviderBindings.Add(Provider);
			TestEqual(
				TEXT("query provider binding matches activation"),
				Provider,
				(*ActivationProfile)->GetStringField(
					TEXT("provider_binding")));
		}
	}

	using namespace UE::UAI::DomainAdapters::WorldSpatial;
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	if (!TestTrue(
			TEXT("create production WorldSpatial descriptor registry"),
			TryCreateWorldSpatialDescriptorRegistry(
				Registry,
				RegistryError))
		|| !Registry.IsValid())
	{
		AddError(TEXT("world_spatial_descriptor_registry_unavailable:")
			+ RegistryError);
		return false;
	}
	for (const FUeAgentOperationDescriptor& Descriptor
		: Registry->GetDescriptors())
	{
		const FString CapabilityId = Descriptor.GetCapabilityId();
		const FString* ExpectedInput = ExpectedInputSchemas.Find(CapabilityId);
		const FString* ExpectedResult = ExpectedResultSchemas.Find(CapabilityId);
		const FString* ExpectedHash = ExpectedDescriptorHashes.Find(CapabilityId);
		TestNotNull(
			TEXT("production capability belongs to fixture: ") + CapabilityId,
			ExpectedInput);
		if (!ExpectedInput || !ExpectedResult || !ExpectedHash)
		{
			continue;
		}
		TestEqual(
			TEXT("exact production input schema"),
			Descriptor.GetInputSchema().GetJsonValue(),
			*ExpectedInput);
		TestEqual(
			TEXT("exact production result schema"),
			Descriptor.GetResultSchema().GetJsonValue(),
			*ExpectedResult);
		TestEqual(
			TEXT("descriptor hash derives from exact contract"),
			Descriptor.GetIdentity().GetDescriptorHash(),
			*ExpectedHash);
	}
	return true;
}
