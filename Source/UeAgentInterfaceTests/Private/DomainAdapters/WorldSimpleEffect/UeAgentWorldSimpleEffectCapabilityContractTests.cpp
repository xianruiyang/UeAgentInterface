#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectCapabilities.h"

namespace
{
	using namespace UE::UAI::Contract;

	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT(
				"UeAgentInterface/Source/UeAgentInterfaceTests/"
				"Private/DomainAdapters/WorldSimpleEffect/Fixtures/"
				"world_simple_effect_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(
		FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load WorldSimpleEffect contract fixture"),
				FFileHelper::LoadFileToString(
					Text,
					*FixturePath())))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse WorldSimpleEffect contract fixture"),
				FJsonSerializer::Deserialize(Reader, Fixture)
					&& Fixture.IsValid()))
		{
			return nullptr;
		}
		return Fixture;
	}

	TArray<TSharedPtr<FJsonValue>> SortedStringValues(
		const TSharedPtr<FJsonObject>& Row,
		const TCHAR* Field)
	{
		TArray<FString> Strings;
		for (const TSharedPtr<FJsonValue>& Value
			: Row->GetArrayField(Field))
		{
			Strings.Add(Value->AsString());
		}
		Strings.Sort();

		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Strings.Num());
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	FString ComputeDescriptorHash(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Row)
	{
		const TSharedPtr<FJsonObject> Policy =
			MakeShared<FJsonObject>();
		Policy->SetArrayField(
			TEXT("allowed_state_planes"),
			SortedStringValues(
				Row,
				TEXT("allowed_state_planes")));
		Policy->SetArrayField(
			TEXT("required_facets"),
			SortedStringValues(
				Row,
				TEXT("required_context_facets")));
		Policy->SetStringField(
			TEXT("resolution_policy"),
			TEXT("explicit_only"));
		Policy->SetStringField(
			TEXT("transient_access_policy"),
			TEXT("temporary_mutation_allowed"));

		const TSharedPtr<FJsonObject> Descriptor =
			MakeShared<FJsonObject>();
		Descriptor->SetStringField(
			TEXT("capability_id"),
			Row->GetStringField(TEXT("capability_id")));
		Descriptor->SetStringField(
			TEXT("descriptor_id"),
			Row->GetStringField(TEXT("descriptor_id")));
		Descriptor->SetStringField(
			TEXT("domain_id"),
			TEXT("world"));
		Descriptor->SetObjectField(
			TEXT("execution_context_policy"),
			Policy);
		Descriptor->SetObjectField(
			TEXT("input_schema"),
			Row->GetObjectField(TEXT("input_schema")));
		Descriptor->SetStringField(
			TEXT("operation_kind"),
			TEXT("operation"));
		Descriptor->SetObjectField(
			TEXT("result_schema"),
			Row->GetObjectField(TEXT("result_schema")));
		Descriptor->SetNumberField(
			TEXT("version"),
			Row->GetNumberField(TEXT("version")));

		FString Canonical;
		FString Error;
		if (!Test->TestTrue(
				*FString::Printf(
					TEXT("%s descriptor canonicalizes"),
					*Row->GetStringField(TEXT("descriptor_id"))),
				Json::TryCanonicalize(
					Descriptor,
					Canonical,
					Error)))
		{
			Test->AddError(
				TEXT("descriptor_canonicalization_failed:")
				+ Error);
			return FString();
		}
		return Json::Sha256Utf8(Canonical);
	}

	bool TryCompileSchema(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& SchemaObject,
		const FString& DescriptorId,
		const TCHAR* SchemaKind,
		TSharedPtr<const FUeAgentContractSchema>& OutSchema)
	{
		OutSchema.Reset();
		FString Canonical;
		FString JsonError;
		if (!Test->TestTrue(
				*FString::Printf(
					TEXT("%s %s canonicalizes"),
					*DescriptorId,
					SchemaKind),
				Json::TryCanonicalize(
					SchemaObject,
					Canonical,
					JsonError)))
		{
			Test->AddError(
				TEXT("schema_canonicalization_failed:")
				+ DescriptorId + TEXT(":") + JsonError);
			return false;
		}

		FUeAgentContractSchemaError Error;
		if (!Test->TestTrue(
				*FString::Printf(
					TEXT("%s %s compiles"),
					*DescriptorId,
					SchemaKind),
				FUeAgentContractSchema::TryCompile(
					FUeAgentContractValue(Canonical),
					OutSchema,
					Error)))
		{
			Test->AddError(FString::Printf(
				TEXT("schema_compile_failed:%s:%s:%s:%s"),
				*DescriptorId,
				SchemaKind,
				*Error.Code,
				*Error.Detail));
			return false;
		}
		return OutSchema.IsValid();
	}

	FUeAgentContractValue ContractValue(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Object,
		const FString& Label)
	{
		FString Canonical;
		FString Error;
		Test->TestTrue(
			*Label,
			Json::TryCanonicalize(
				Object,
				Canonical,
				Error));
		if (Canonical.IsEmpty())
		{
			Test->AddError(Label + TEXT(":") + Error);
		}
		return FUeAgentContractValue(MoveTemp(Canonical));
	}

	bool IsForbiddenInputField(const FString& Field)
	{
		static const TSet<FString> Forbidden = {
			TEXT("allow_destructive"),
			TEXT("command"),
			TEXT("command_id"),
			TEXT("current"),
			TEXT("dry_run"),
			TEXT("hjson"),
			TEXT("http"),
			TEXT("output_file"),
			TEXT("provider_binding"),
			TEXT("route"),
			TEXT("save"),
			TEXT("save_after_apply"),
			TEXT("selection"),
			TEXT("transaction"),
			TEXT("verify"),
		};
		return Forbidden.Contains(Field.ToLower());
	}

	bool ValidateFieldOwnership(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Root,
		const FString& DescriptorId)
	{
		struct FSchemaTask
		{
			TSharedPtr<FJsonObject> Schema;
			FString Path;
		};

		TArray<FSchemaTask> Pending;
		Pending.Add({Root, TEXT("$")});
		bool bValid = true;
		while (!Pending.IsEmpty())
		{
			const FSchemaTask Task = Pending.Pop(EAllowShrinking::No);
			if (!Task.Schema.IsValid())
			{
				Test->AddError(
					TEXT("schema_object_missing:")
					+ DescriptorId + TEXT(":") + Task.Path);
				bValid = false;
				continue;
			}

			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (Task.Schema->TryGetObjectField(
					TEXT("properties"),
					Properties)
				&& Properties
				&& Properties->IsValid())
			{
				for (const TPair<
					FString,
					TSharedPtr<FJsonValue>>& Pair
					: (*Properties)->Values)
				{
					if (IsForbiddenInputField(Pair.Key))
					{
						Test->AddError(
							TEXT("cross_owner_input_forbidden:")
							+ DescriptorId + TEXT(":")
							+ Task.Path + TEXT("/")
							+ Pair.Key);
						bValid = false;
					}
					if (Pair.Value.IsValid()
						&& Pair.Value->Type == EJson::Object)
					{
						Pending.Add({
							Pair.Value->AsObject(),
							Task.Path + TEXT("/") + Pair.Key});
					}
				}
			}

			const TSharedPtr<FJsonObject>* Items = nullptr;
			if (Task.Schema->TryGetObjectField(
					TEXT("items"),
					Items)
				&& Items
				&& Items->IsValid())
			{
				Pending.Add({
					*Items,
					Task.Path + TEXT("/items")});
			}

			for (const TCHAR* BranchKeyword
				: {TEXT("allOf"), TEXT("anyOf"), TEXT("oneOf")})
			{
				const TArray<TSharedPtr<FJsonValue>>* Branches =
					nullptr;
				if (!Task.Schema->TryGetArrayField(
						BranchKeyword,
						Branches)
					|| !Branches)
				{
					continue;
				}
				for (int32 Index = 0;
					Index < Branches->Num();
					++Index)
				{
					const TSharedPtr<FJsonValue>& Branch =
						(*Branches)[Index];
					if (Branch.IsValid()
						&& Branch->Type == EJson::Object)
					{
						Pending.Add({
							Branch->AsObject(),
							FString::Printf(
								TEXT("%s/%s/%d"),
								*Task.Path,
								BranchKeyword,
								Index)});
					}
				}
			}
		}
		return bValid;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSimpleEffectCapabilityContractTest,
	"UeAgentInterface.DomainAdapters.WorldSimpleEffect.E0."
		"CapabilityContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSimpleEffectCapabilityContractTest::RunTest(
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
			"uai.domain_adapter.world_simple_effect."
			"test_fixture.v1")));

	const TArray<TSharedPtr<FJsonValue>>* DescriptorValues = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* CaseValues = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ScenarioValues = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* LegacyValues = nullptr;
	if (!TestTrue(
			TEXT("descriptors exist"),
			Fixture->TryGetArrayField(
				TEXT("descriptors"),
				DescriptorValues)
				&& DescriptorValues)
		|| !TestTrue(
			TEXT("contract cases exist"),
			Fixture->TryGetArrayField(
				TEXT("contract_cases"),
				CaseValues)
				&& CaseValues)
		|| !TestTrue(
			TEXT("E2 scenarios exist"),
			Fixture->TryGetArrayField(
				TEXT("e2_scenarios"),
				ScenarioValues)
				&& ScenarioValues)
		|| !TestTrue(
			TEXT("legacy source decisions exist"),
			Fixture->TryGetArrayField(
				TEXT("legacy_sources"),
				LegacyValues)
				&& LegacyValues))
	{
		return false;
	}

	TSet<FString> AllowedNativeTypes;
	for (const TSharedPtr<FJsonValue>& Value
		: Fixture->GetArrayField(TEXT("native_operation_types")))
	{
		AllowedNativeTypes.Add(Value->AsString());
	}
	TMap<FString, TSharedPtr<FJsonObject>> RowsByDescriptor;
	TSet<FString> CapabilityIds;
	TSet<FString> DescriptorHashes;
	TSet<FString> SpatialDescriptors;
	bool bFixtureValid = true;
	for (const TSharedPtr<FJsonValue>& Value : *DescriptorValues)
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		if (!Row.IsValid())
		{
			AddError(TEXT("descriptor_row_invalid"));
			bFixtureValid = false;
			continue;
		}

		const FString DescriptorId =
			Row->GetStringField(TEXT("descriptor_id"));
		const FString CapabilityId =
			Row->GetStringField(TEXT("capability_id"));
		const FString DescriptorHash =
			Row->GetStringField(TEXT("descriptor_hash"));
		const FString NativeType =
			Row->GetStringField(TEXT("native_operation_type"));

		TestTrue(
			*FString::Printf(
				TEXT("%s is an operation descriptor"),
				*DescriptorId),
			DescriptorId.StartsWith(
				TEXT("uai.operation.world.")));
		TestEqual(
			*FString::Printf(
				TEXT("%s starts at version one"),
				*DescriptorId),
			static_cast<int32>(
				Row->GetNumberField(TEXT("version"))),
			1);
		TestTrue(
			*FString::Printf(
				TEXT("%s uses a stable native operation type"),
				*DescriptorId),
			AllowedNativeTypes.Contains(NativeType));
		TestFalse(
			*FString::Printf(
				TEXT("%s has no legacy command identity"),
				*DescriptorId),
			Row->HasField(TEXT("legacy_command_id"))
				|| Row->HasField(TEXT("command"))
				|| Row->HasField(TEXT("route")));
		TestEqual(
			*FString::Printf(
				TEXT("%s freezes its C0 descriptor hash"),
				*DescriptorId),
			DescriptorHash,
			ComputeDescriptorHash(this, Row));

		if (RowsByDescriptor.Contains(DescriptorId))
		{
			AddError(
				TEXT("descriptor_duplicate:") + DescriptorId);
			bFixtureValid = false;
		}
		if (CapabilityIds.Contains(CapabilityId))
		{
			AddError(
				TEXT("capability_duplicate:") + CapabilityId);
			bFixtureValid = false;
		}
		if (DescriptorHashes.Contains(DescriptorHash))
		{
			AddError(
				TEXT("descriptor_hash_duplicate:")
				+ DescriptorHash);
			bFixtureValid = false;
		}
		RowsByDescriptor.Add(DescriptorId, Row);
		CapabilityIds.Add(CapabilityId);
		DescriptorHashes.Add(DescriptorHash);

		TSharedPtr<const FUeAgentContractSchema> InputSchema;
		TSharedPtr<const FUeAgentContractSchema> ResultSchema;
		bFixtureValid =
			TryCompileSchema(
				this,
				Row->GetObjectField(TEXT("input_schema")),
				DescriptorId,
				TEXT("input_schema"),
				InputSchema)
			&& bFixtureValid;
		bFixtureValid =
			TryCompileSchema(
				this,
				Row->GetObjectField(TEXT("result_schema")),
				DescriptorId,
				TEXT("result_schema"),
				ResultSchema)
			&& bFixtureValid;
		bFixtureValid =
			ValidateFieldOwnership(
				this,
				Row->GetObjectField(TEXT("input_schema")),
				DescriptorId)
			&& bFixtureValid;

		const TSharedPtr<FJsonObject>* SpaceContract = nullptr;
		if (Row->TryGetObjectField(
				TEXT("space_contract"),
				SpaceContract)
			&& SpaceContract
			&& SpaceContract->IsValid())
		{
			SpatialDescriptors.Add(DescriptorId);
			for (const TCHAR* Field : {
				TEXT("subject"),
				TEXT("dimension"),
				TEXT("semantic_frame"),
				TEXT("write_frame"),
				TEXT("read_frame"),
				TEXT("verify_frame"),
				TEXT("conversion_chain"),
			})
			{
				TestTrue(
					*FString::Printf(
						TEXT("%s space contract owns %s"),
						*DescriptorId,
						Field),
					(*SpaceContract)->HasField(Field));
			}
			TestEqual(
				*FString::Printf(
					TEXT("%s is explicitly three-dimensional"),
						*DescriptorId),
				static_cast<int32>(
					(*SpaceContract)->GetNumberField(
						TEXT("dimension"))),
				3);
			TestEqual(
				*FString::Printf(
					TEXT("%s writes only after C4 normalization"),
					*DescriptorId),
				(*SpaceContract)->GetStringField(
					TEXT("write_frame")),
				FString(TEXT("ue_world")));
		}
	}

	TestTrue(
		TEXT("all descriptor rows satisfy the frozen boundary"),
		bFixtureValid);
	TestTrue(
		TEXT("actor create owns a space contract"),
		SpatialDescriptors.Contains(
			TEXT("uai.operation.world.actor.create")));
	TestTrue(
		TEXT("wall owns a space contract"),
		SpatialDescriptors.Contains(
			TEXT(
				"uai.operation.world.wall.create_with_opening")));
	TestTrue(
		TEXT("streaming create owns a space contract"),
		SpatialDescriptors.Contains(
			TEXT(
				"uai.operation.world.streaming_relation.create")));
	TestTrue(
		TEXT("streaming state owns a space contract"),
		SpatialDescriptors.Contains(
			TEXT(
				"uai.operation.world."
				"streaming_relation.state.update")));

	for (const TSharedPtr<FJsonValue>& Value : *CaseValues)
	{
		const TSharedPtr<FJsonObject> Case = Value->AsObject();
		const FString DescriptorId =
			Case->GetStringField(TEXT("descriptor_id"));
		const TSharedPtr<FJsonObject>* Row =
			RowsByDescriptor.Find(DescriptorId);
		if (!TestTrue(
				*FString::Printf(
					TEXT("%s contract case targets a descriptor"),
					*DescriptorId),
				Row != nullptr))
		{
			continue;
		}

		TSharedPtr<const FUeAgentContractSchema> Compiled;
		if (!TryCompileSchema(
				this,
				(*Row)->GetObjectField(TEXT("input_schema")),
				DescriptorId,
				TEXT("case_input_schema"),
				Compiled))
		{
			continue;
		}

		TOptional<FUeAgentContractValue> CanonicalInput;
		FUeAgentContractSchemaError Error;
		const bool bValidAccepted =
			Compiled->TryValidateAndCanonicalize(
				ContractValue(
					this,
					Case->GetObjectField(TEXT("valid_input")),
					DescriptorId
						+ TEXT(" valid input canonicalizes")),
				[](int64,
					const FString&,
					FUeAgentContractSchemaError&)
				{
					return true;
				},
				CanonicalInput,
				Error);
		TestTrue(
			*FString::Printf(
				TEXT("%s accepts the valid case"),
				*DescriptorId),
			bValidAccepted);
		TestTrue(
			*FString::Printf(
				TEXT("%s produces canonical validated input"),
				*DescriptorId),
			CanonicalInput.IsSet()
				&& CanonicalInput->IsValid());

		CanonicalInput.Reset();
		Error = {};
		const bool bInvalidAccepted =
			Compiled->TryValidateAndCanonicalize(
				ContractValue(
					this,
					Case->GetObjectField(TEXT("invalid_input")),
					DescriptorId
						+ TEXT(" invalid input canonicalizes")),
				[](int64,
					const FString&,
					FUeAgentContractSchemaError&)
				{
					return true;
				},
				CanonicalInput,
				Error);
		TestFalse(
			*FString::Printf(
				TEXT("%s rejects the invalid case"),
				*DescriptorId),
			bInvalidAccepted);
		TestTrue(
			*FString::Printf(
				TEXT("%s returns a typed schema error"),
				*DescriptorId),
			Error.IsValid());
	}

	TSet<FString> ScenarioDescriptors;
	for (const TSharedPtr<FJsonValue>& Value : *ScenarioValues)
	{
		const TSharedPtr<FJsonObject> Scenario = Value->AsObject();
		const FString DescriptorId =
			Scenario->GetStringField(TEXT("descriptor_id"));
		TestTrue(
			*FString::Printf(
				TEXT("%s E2 oracle targets a descriptor"),
				*DescriptorId),
			RowsByDescriptor.Contains(DescriptorId));
		TestFalse(
			*FString::Printf(
				TEXT("%s E2 oracle is independent"),
				*DescriptorId),
			Scenario->GetStringField(TEXT("oracle")).IsEmpty());
		TestFalse(
			*FString::Printf(
				TEXT("%s owns a zero-mutation negative oracle"),
				*DescriptorId),
			Scenario->GetStringField(
				TEXT("negative_oracle")).IsEmpty());
		TestFalse(
			*FString::Printf(
				TEXT("%s has one E2 scenario"),
				*DescriptorId),
			ScenarioDescriptors.Contains(DescriptorId));
		ScenarioDescriptors.Add(DescriptorId);
	}
	bool bSawTopologyDelete = false;
	bool bSawStreamingCurrentSplit = false;
	bool bSawHlodSplit = false;
	for (const TSharedPtr<FJsonValue>& Value : *LegacyValues)
	{
		const TSharedPtr<FJsonObject> Legacy = Value->AsObject();
		const FString LegacyCommand =
			Legacy->GetStringField(TEXT("legacy_command_id"));
		const FString Terminal =
			Legacy->GetStringField(TEXT("terminal"));
		const TArray<TSharedPtr<FJsonValue>>& Capabilities =
			Legacy->GetArrayField(TEXT("capability_ids"));
		if (LegacyCommand
			== TEXT("level_topology_set_actor_assignments"))
		{
			bSawTopologyDelete = true;
			TestEqual(
				TEXT("unsafe topology compound is deleted"),
				Terminal,
				FString(TEXT("DELETE")));
			TestEqual(
				TEXT("deleted topology command creates no descriptor"),
				Capabilities.Num(),
				0);
		}
		else if (LegacyCommand
			== TEXT("level_streaming_set_state"))
		{
			bSawStreamingCurrentSplit = true;
			const TArray<TSharedPtr<FJsonValue>>& External =
				Legacy->GetArrayField(
					TEXT("external_capability_ids"));
			TestEqual(
				TEXT("current-level state has one external owner"),
				External.Num(),
				1);
			TestEqual(
				TEXT("current-level state belongs to session"),
				External[0]->AsString(),
				FString(TEXT("editor.current_level.set")));
		}
		else if (LegacyCommand == TEXT("hlod_assign_actors"))
		{
			bSawHlodSplit = true;
			TestEqual(
				TEXT("HLOD legacy command maps to two exact capabilities"),
				Capabilities.Num(),
				2);
		}
	}
	TestTrue(
		TEXT("topology delete is explicit"),
		bSawTopologyDelete);
	TestTrue(
		TEXT("current-level split is explicit"),
		bSawStreamingCurrentSplit);
	TestTrue(TEXT("HLOD split is explicit"), bSawHlodSplit);
	TestFalse(
		TEXT("deleted topology command is absent from descriptors"),
		RowsByDescriptor.Contains(
			TEXT("level_topology_set_actor_assignments")));

	TSharedPtr<const FUeAgentDescriptorRegistry> ProductionRegistry;
	FString RegistryError;
	const bool bRegistryCreated = TestTrue(
		TEXT("create production WorldSimpleEffect registry"),
		UE::UAI::DomainAdapters::WorldSimpleEffect::
			TryCreateWorldSimpleEffectDescriptorRegistry(
				ProductionRegistry,
				RegistryError));
	if (!bRegistryCreated)
	{
		AddError(
			TEXT("world_simple_registry_failed:")
			+ RegistryError);
	}
	if (ProductionRegistry.IsValid())
	{
		for (const TSharedPtr<FJsonValue>& Value
			: *DescriptorValues)
		{
			const TSharedPtr<FJsonObject> Row = Value->AsObject();
			const FUeAgentOperationDescriptor* Descriptor =
				ProductionRegistry->FindByDescriptorId(
					Row->GetStringField(TEXT("descriptor_id")));
			TestTrue(
				*FString::Printf(
					TEXT("%s is an exact production operation"),
					*Row->GetStringField(TEXT("descriptor_id"))),
				Descriptor
					&& Descriptor->GetOperationKind()
						== EUeAgentDescriptorOperationKind::
							Operation
					&& !Descriptor->GetQueryBinding().IsSet()
					&& Descriptor->GetIdentity()
						.GetDescriptorHash()
						== Row->GetStringField(
							TEXT("descriptor_hash")));
		}
	}
	return true;
}
