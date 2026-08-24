#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialExecutionChains.h"

namespace
{
	using UE::UAI::Contract::FUeAgentContractValue;

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

	TSharedPtr<FJsonValue> ExpandExample(
		const TSharedPtr<FJsonValue>& Value,
		const TSharedPtr<FJsonObject>& Examples,
		TSet<FString>& ExpansionStack,
		FString& OutError)
	{
		if (!Value.IsValid())
		{
			OutError = TEXT("world_spatial_example_value_invalid");
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
				TSharedPtr<FJsonValue> ExpandedItem = ExpandExample(
					Item,
					Examples,
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
			if (!Object.IsValid())
			{
				OutError = TEXT("world_spatial_example_object_invalid");
				return nullptr;
			}
			FString ExampleName;
			if (Object->TryGetStringField(TEXT("example"), ExampleName))
			{
				if (Object->Values.Num() != 1 || ExampleName.IsEmpty())
				{
					OutError =
						TEXT("world_spatial_example_reference_invalid");
					return nullptr;
				}
				const TSharedPtr<FJsonValue>* Definition =
					Examples->Values.Find(ExampleName);
				if (!Definition || !Definition->IsValid())
				{
					OutError = TEXT("world_spatial_example_missing:")
						+ ExampleName;
					return nullptr;
				}
				if (ExpansionStack.Contains(ExampleName))
				{
					OutError = TEXT("world_spatial_example_cycle:")
						+ ExampleName;
					return nullptr;
				}
				ExpansionStack.Add(ExampleName);
				TSharedPtr<FJsonValue> Expanded = ExpandExample(
					*Definition,
					Examples,
					ExpansionStack,
					OutError);
				ExpansionStack.Remove(ExampleName);
				return Expanded;
			}

			const TSharedPtr<FJsonObject> Expanded =
				MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
				: Object->Values)
			{
				TSharedPtr<FJsonValue> ExpandedField = ExpandExample(
					Pair.Value,
					Examples,
					ExpansionStack,
					OutError);
				if (!ExpandedField.IsValid())
				{
					return nullptr;
				}
				Expanded->SetField(Pair.Key, MoveTemp(ExpandedField));
			}
			return MakeShared<FJsonValueObject>(Expanded);
		}
		default:
			OutError = TEXT("world_spatial_example_json_kind_invalid");
			return nullptr;
		}
	}

	TSet<FString> FieldNames(
		const TSharedPtr<FJsonObject>& Descriptor,
		bool bRequired)
	{
		TSet<FString> Result;
		for (const TSharedPtr<FJsonValue>& Value
			: Descriptor->GetArrayField(TEXT("input_fields")))
		{
			const TSharedPtr<FJsonObject> Field = Value->AsObject();
			bool bFieldRequired = false;
			if (Field.IsValid()
				&& Field->TryGetBoolField(
					TEXT("required"),
					bFieldRequired)
				&& bFieldRequired == bRequired)
			{
				Result.Add(Field->GetStringField(TEXT("name")));
			}
		}
		return Result;
	}

	FString SortedSetText(const TSet<FString>& Values)
	{
		TArray<FString> Sorted = Values.Array();
		Sorted.Sort();
		return FString::Join(Sorted, TEXT("|"));
	}

	TSet<FString> ObjectKeys(const TSharedPtr<FJsonObject>& Object)
	{
		TSet<FString> Result;
		if (Object.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
				: Object->Values)
			{
				Result.Add(Pair.Key);
			}
		}
		return Result;
	}

	bool NonEmptyString(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& CapabilityId)
	{
		FString Value;
		return Test->TestTrue(
			FString::Printf(
				TEXT("%s is non-empty: %s"),
				Field,
				*CapabilityId),
			Object->TryGetStringField(Field, Value)
				&& !Value.IsEmpty());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialExactExecutionChainContractsTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E1.ExactExecutionChainContracts",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialExactExecutionChainContractsTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadObject(
		this,
		TEXT("DomainAdapters/WorldSpatial/Fixtures/"
			"world_spatial_execution_chain_cases.json"),
		TEXT("WorldSpatial execution-chain fixture"));
	const TSharedPtr<FJsonObject> Descriptors = LoadObject(
		this,
		TEXT("DomainAdapters/WorldSpatial/Fixtures/"
			"world_spatial_exact_descriptor_cases.json"),
		TEXT("WorldSpatial exact descriptor fixture"));
	const TSharedPtr<FJsonObject> Activation = LoadObject(
		this,
		TEXT("Composition/TimelineSpatial/Fixtures/"
			"timeline_spatial_activation_contract_cases.json"),
		TEXT("TimelineSpatial activation fixture"));
	if (!Fixture.IsValid()
		|| !Descriptors.IsValid()
		|| !Activation.IsValid())
	{
		return false;
	}

	TestEqual(
		TEXT("execution fixture schema"),
		Fixture->GetStringField(TEXT("schema")),
		FString(TEXT(
			"uai.domain_adapter.world_spatial."
			"execution_chain_fixture.v1")));
	TestEqual(
		TEXT("execution chain count declaration"),
		static_cast<int32>(Fixture->GetNumberField(TEXT("chain_count"))),
		22);
	const TArray<TSharedPtr<FJsonValue>>& Chains =
		Fixture->GetArrayField(TEXT("chains"));
	TestEqual(TEXT("execution chain count"), Chains.Num(), 22);

	const TSharedPtr<FJsonObject> Invariants =
		Fixture->GetObjectField(TEXT("invariants"));
	TestEqual(
		TEXT("exact dispatch only"),
		Invariants->GetStringField(TEXT("dispatch")),
		FString(TEXT("exact_binding_only")));
	TestEqual(
		TEXT("payloads remain strongly typed"),
		Invariants->GetStringField(TEXT("payload_contract")),
		FString(TEXT("strongly_typed_dto")));
	TestEqual(
		TEXT("one OperationContext is forwarded"),
		Invariants->GetStringField(TEXT("context_forwarding")),
		FString(TEXT("same_operation_context")));
	TestFalse(
		TEXT("mutation receipt cannot be the oracle"),
		Invariants->GetBoolField(TEXT("mutation_receipt_is_oracle")));
	TestTrue(
		TEXT("independent readback is mandatory"),
		Invariants->GetBoolField(
			TEXT("independent_readback_required")));

	TMap<FString, TSharedPtr<FJsonObject>> DescriptorByCapability;
	for (const TSharedPtr<FJsonValue>& Value
		: Descriptors->GetArrayField(TEXT("descriptors")))
	{
		const TSharedPtr<FJsonObject> Descriptor = Value->AsObject();
		if (Descriptor.IsValid())
		{
			DescriptorByCapability.Add(
				Descriptor->GetStringField(TEXT("capability_id")),
				Descriptor);
		}
	}
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
	TestEqual(
		TEXT("descriptor fixture count"),
		DescriptorByCapability.Num(),
		22);
	TestEqual(
		TEXT("activation profile count"),
		ActivationByCapability.Num(),
		22);

	const TSharedPtr<FJsonObject> Examples =
		Fixture->GetObjectField(TEXT("examples"));
	TSet<FString> CapabilityIds;
	TSet<FString> BindingIds;
	TSet<FString> RequestTypeIds;
	TSet<FString> ResultTypeIds;
	TSet<FString> FamilyIds;
	int32 QueryCount = 0;
	int32 OperationCount = 0;
	for (const TSharedPtr<FJsonValue>& Value : Chains)
	{
		const TSharedPtr<FJsonObject> Chain = Value->AsObject();
		if (!TestTrue(TEXT("chain is an object"), Chain.IsValid()))
		{
			continue;
		}
		const FString CapabilityId =
			Chain->GetStringField(TEXT("capability_id"));
		const FString DescriptorId =
			Chain->GetStringField(TEXT("descriptor_id"));
		const FString OperationKind =
			Chain->GetStringField(TEXT("operation_kind"));
		const FString FamilyId =
			Chain->GetStringField(TEXT("family_id"));
		const FString BindingId =
			Chain->GetStringField(TEXT("binding_id"));
		const FString RequestTypeId =
			Chain->GetStringField(TEXT("typed_request_id"));
		const FString ResultTypeId =
			Chain->GetStringField(TEXT("typed_result_id"));

		for (const TCHAR* Field : {
			TEXT("capability_id"),
			TEXT("descriptor_id"),
			TEXT("operation_kind"),
			TEXT("c3_lane"),
			TEXT("family_id"),
			TEXT("binding_id"),
			TEXT("typed_request_id"),
			TEXT("typed_result_id"),
			TEXT("readback_id"),
			TEXT("readback_owner"),
			TEXT("settle_barrier"),
			TEXT("oracle_kind")})
		{
			NonEmptyString(this, Chain, Field, CapabilityId);
		}

		TestFalse(
			TEXT("capability chain is unique: ") + CapabilityId,
			CapabilityIds.Contains(CapabilityId));
		TestFalse(
			TEXT("exact binding is unique: ") + BindingId,
			BindingIds.Contains(BindingId));
		TestFalse(
			TEXT("typed request is unique: ") + RequestTypeId,
			RequestTypeIds.Contains(RequestTypeId));
		TestFalse(
			TEXT("typed result is unique: ") + ResultTypeId,
			ResultTypeIds.Contains(ResultTypeId));
		CapabilityIds.Add(CapabilityId);
		BindingIds.Add(BindingId);
		RequestTypeIds.Add(RequestTypeId);
		ResultTypeIds.Add(ResultTypeId);
		FamilyIds.Add(FamilyId);

		const TSharedPtr<FJsonObject>* Descriptor =
			DescriptorByCapability.Find(CapabilityId);
		const TSharedPtr<FJsonObject>* ActivationProfile =
			ActivationByCapability.Find(CapabilityId);
		TestNotNull(
			TEXT("exact descriptor exists: ") + CapabilityId,
			Descriptor);
		TestNotNull(
			TEXT("activation profile exists: ") + CapabilityId,
			ActivationProfile);
		if (!Descriptor || !ActivationProfile)
		{
			continue;
		}
		TestEqual(
			TEXT("descriptor identity matches exact fixture"),
			DescriptorId,
			(*Descriptor)->GetStringField(TEXT("descriptor_id")));
		TestEqual(
			TEXT("operation kind matches exact fixture"),
			OperationKind,
			(*Descriptor)->GetStringField(TEXT("operation_kind")));
		TestEqual(
			TEXT("family matches activation"),
			FamilyId,
			(*ActivationProfile)->GetStringField(TEXT("family_id")));
		TestEqual(
			TEXT("binding matches activation typed operation"),
			BindingId,
			(*ActivationProfile)->GetStringField(
				TEXT("typed_operation_id")));
		TestEqual(
			TEXT("oracle matches exact descriptor"),
			Chain->GetStringField(TEXT("oracle_kind")),
			(*Descriptor)->GetStringField(TEXT("oracle_kind")));

		const TSharedPtr<FJsonObject> InputTemplate =
			Chain->GetObjectField(TEXT("input_example"));
		TSet<FString> ExpansionStack;
		FString ExampleError;
		const TSharedPtr<FJsonValue> ExpandedValue = ExpandExample(
			MakeShared<FJsonValueObject>(InputTemplate),
			Examples,
			ExpansionStack,
			ExampleError);
		const TSharedPtr<FJsonObject> ExpandedInput =
			ExpandedValue.IsValid() ? ExpandedValue->AsObject() : nullptr;
		TestTrue(
			TEXT("input example expands: ") + CapabilityId,
			ExpandedInput.IsValid());
		if (ExpandedInput.IsValid())
		{
			const TSet<FString> Required = FieldNames(*Descriptor, true);
			const TSet<FString> Optional = FieldNames(*Descriptor, false);
			const TSet<FString> Keys = ObjectKeys(ExpandedInput);
			TSet<FString> Missing = Required.Difference(Keys);
			TSet<FString> Unknown = Keys.Difference(Required.Union(Optional));
			TestEqual(
				TEXT("input example includes every required field"),
				SortedSetText(Missing),
				FString());
			TestEqual(
				TEXT("input example contains no unknown field"),
				SortedSetText(Unknown),
				FString());
			FString CanonicalInput;
			TestTrue(
				TEXT("input example canonicalizes: ") + CapabilityId,
				UE::UAI::Contract::Json::TryCanonicalize(
					ExpandedInput,
					CanonicalInput,
					ExampleError)
					&& FUeAgentContractValue(CanonicalInput).IsValid());
		}

		const bool bRequiresMutation =
			Chain->GetBoolField(TEXT("requires_native_mutation"));
		const bool bReadbackAfterMutation =
			Chain->GetBoolField(TEXT("readback_after_mutation"));
		if (OperationKind == TEXT("query"))
		{
			++QueryCount;
			TestEqual(
				TEXT("query enters C3Q"),
				Chain->GetStringField(TEXT("c3_lane")),
				FString(TEXT("c3q")));
			TestFalse(TEXT("query never mutates"), bRequiresMutation);
			TestFalse(
				TEXT("query readback is not post-mutation"),
				bReadbackAfterMutation);
			TestEqual(
				TEXT("query binding matches descriptor provider"),
				BindingId,
				(*Descriptor)->GetStringField(
					TEXT("provider_binding")));
			TestFalse(
				TEXT("query has no C6 change kind"),
				Chain->HasField(TEXT("c6_change_kind")));
			TestFalse(
				TEXT("query has no native mutation action"),
				Chain->HasField(TEXT("native_action_id")));
		}
		else
		{
			++OperationCount;
			TestEqual(
				TEXT("operation enters C3O"),
				Chain->GetStringField(TEXT("c3_lane")),
				FString(TEXT("c3o")));
			TestTrue(
				TEXT("operation requires one native mutation"),
				bRequiresMutation);
			TestTrue(
				TEXT("operation verifies after mutation"),
				bReadbackAfterMutation);
			NonEmptyString(
				this,
				Chain,
				TEXT("c6_change_kind"),
				CapabilityId);
			NonEmptyString(
				this,
				Chain,
				TEXT("native_action_id"),
				CapabilityId);
			TestNotEqual(
				TEXT("native action and readback remain separate"),
				Chain->GetStringField(TEXT("native_action_id")),
				Chain->GetStringField(TEXT("readback_id")));
		}
	}

	TestEqual(TEXT("query chain count"), QueryCount, 10);
	TestEqual(TEXT("operation chain count"), OperationCount, 12);
	TestEqual(TEXT("five family contracts are consumed"), FamilyIds.Num(), 5);
	TestEqual(TEXT("all exact capabilities are covered"), CapabilityIds.Num(), 22);
	TestEqual(TEXT("all exact bindings are unique"), BindingIds.Num(), 22);

	using namespace UE::UAI::DomainAdapters::WorldSpatial;
	const TArray<FUeAgentWorldSpatialExecutionChainContract>&
		ProductionContracts = GetWorldSpatialExecutionChainContracts();
	TestEqual(
		TEXT("production execution chain count"),
		ProductionContracts.Num(),
		22);
	TMap<FString, const FUeAgentWorldSpatialExecutionChainContract*>
		ProductionByCapability;
	for (const FUeAgentWorldSpatialExecutionChainContract& Contract
		: ProductionContracts)
	{
		ProductionByCapability.Add(
			Contract.GetCapabilityId(),
			&Contract);
	}
	for (const TSharedPtr<FJsonValue>& Value : Chains)
	{
		const TSharedPtr<FJsonObject> Chain = Value->AsObject();
		const FString CapabilityId =
			Chain->GetStringField(TEXT("capability_id"));
		const FUeAgentWorldSpatialExecutionChainContract* const* Found =
			ProductionByCapability.Find(CapabilityId);
		TestNotNull(
			TEXT("production chain exists: ") + CapabilityId,
			Found);
		if (!Found || !*Found)
		{
			continue;
		}
		const FUeAgentWorldSpatialExecutionChainContract& Contract = **Found;
		TestEqual(TEXT("production descriptor"), Contract.GetDescriptorId(), Chain->GetStringField(TEXT("descriptor_id")));
		TestEqual(TEXT("production operation kind"), Contract.GetOperationKind(), Chain->GetStringField(TEXT("operation_kind")));
		TestEqual(TEXT("production C3 lane"), Contract.GetC3Lane(), Chain->GetStringField(TEXT("c3_lane")));
		TestEqual(TEXT("production family"), Contract.GetFamilyId(), Chain->GetStringField(TEXT("family_id")));
		TestEqual(TEXT("production binding"), Contract.GetBindingId(), Chain->GetStringField(TEXT("binding_id")));
		TestEqual(TEXT("production request type"), Contract.GetTypedRequestId(), Chain->GetStringField(TEXT("typed_request_id")));
		TestEqual(TEXT("production result type"), Contract.GetTypedResultId(), Chain->GetStringField(TEXT("typed_result_id")));
		TestEqual(TEXT("production readback"), Contract.GetReadbackId(), Chain->GetStringField(TEXT("readback_id")));
		TestEqual(TEXT("production readback owner"), Contract.GetReadbackOwner(), Chain->GetStringField(TEXT("readback_owner")));
		TestEqual(TEXT("production settle barrier"), Contract.GetSettleBarrier(), Chain->GetStringField(TEXT("settle_barrier")));
		TestEqual(TEXT("production oracle"), Contract.GetOracleKind(), Chain->GetStringField(TEXT("oracle_kind")));
		TestEqual(TEXT("production mutation requirement"), Contract.RequiresNativeMutation(), Chain->GetBoolField(TEXT("requires_native_mutation")));
		TestEqual(TEXT("production readback ordering"), Contract.ReadsBackAfterMutation(), Chain->GetBoolField(TEXT("readback_after_mutation")));
		if (Contract.RequiresNativeMutation())
		{
			TestEqual(TEXT("production C6 change kind"), Contract.GetC6ChangeKind(), Chain->GetStringField(TEXT("c6_change_kind")));
			TestEqual(TEXT("production native action"), Contract.GetNativeActionId(), Chain->GetStringField(TEXT("native_action_id")));
		}
	}
	return true;
}
