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

#include "DomainAdapters/LandscapeGeometryEffect/UeAgentLandscapeGeometryEffectCapabilities.h"

namespace
{
	using namespace UE::UAI::Contract;

	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT(
				"UeAgentInterface/Source/UeAgentInterfaceTests/"
				"Private/DomainAdapters/LandscapeGeometryEffect/"
				"Fixtures/"
				"landscape_geometry_effect_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(
		FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load LandscapeGeometryEffect contract fixture"),
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
				TEXT("parse LandscapeGeometryEffect contract fixture"),
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
		for (const FString& String : Strings)
		{
			Values.Add(
				MakeShared<FJsonValueString>(String));
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
			TEXT("landscape"));
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
					*Row->GetStringField(
						TEXT("descriptor_id"))),
				Json::TryCanonicalize(
					Descriptor,
					Canonical,
					Error)))
		{
			Test->AddError(
				TEXT(
					"landscape_descriptor_canonicalization_failed:")
				+ Error);
			return FString();
		}
		return Json::Sha256Utf8(Canonical);
	}

	bool TryCompileSchema(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& SchemaObject,
		const FString& DescriptorId,
		const TCHAR* Kind,
		TSharedPtr<const FUeAgentContractSchema>& OutSchema)
	{
		OutSchema.Reset();
		FString Canonical;
		FString JsonError;
		if (!Test->TestTrue(
				*FString::Printf(
					TEXT("%s %s canonicalizes"),
					*DescriptorId,
					Kind),
				Json::TryCanonicalize(
					SchemaObject,
					Canonical,
					JsonError)))
		{
			return false;
		}

		FUeAgentContractSchemaError Error;
		if (!Test->TestTrue(
				*FString::Printf(
					TEXT("%s %s compiles"),
					*DescriptorId,
					Kind),
				FUeAgentContractSchema::TryCompile(
					FUeAgentContractValue(Canonical),
					OutSchema,
					Error)))
		{
			Test->AddError(FString::Printf(
				TEXT(
					"landscape_schema_compile_failed:%s:%s:%s:%s"),
				*DescriptorId,
				Kind,
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
			TEXT("command"),
			TEXT("command_id"),
			TEXT("current"),
			TEXT("dry_run"),
			TEXT("hjson"),
			TEXT("http"),
			TEXT("output_file"),
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
			const FSchemaTask Task =
				Pending.Pop(EAllowShrinking::No);
			if (!Task.Schema.IsValid())
			{
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
							TEXT(
								"landscape_cross_owner_input_forbidden:")
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
		}
		return bValid;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLandscapeGeometryEffectCapabilityContractTest,
	"UeAgentInterface.DomainAdapters.LandscapeGeometryEffect.E0."
		"CapabilityContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentLandscapeGeometryEffectCapabilityContractTest::RunTest(
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
			"uai.domain_adapter.landscape_geometry_effect."
			"test_fixture.v1")));

	const TArray<TSharedPtr<FJsonValue>>* Descriptors = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Cases = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* NegativeCases = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Scenarios = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* LegacySources = nullptr;
	if (!TestTrue(
			TEXT("descriptor rows exist"),
			Fixture->TryGetArrayField(
				TEXT("descriptors"),
				Descriptors)
				&& Descriptors)
		|| !TestTrue(
			TEXT("contract cases exist"),
			Fixture->TryGetArrayField(
				TEXT("contract_cases"),
				Cases)
				&& Cases)
		|| !TestTrue(
			TEXT("E1 negative cases exist"),
			Fixture->TryGetArrayField(
				TEXT("e1_negative_cases"),
				NegativeCases)
				&& NegativeCases)
		|| !TestTrue(
			TEXT("E2 scenarios exist"),
			Fixture->TryGetArrayField(
				TEXT("e2_scenarios"),
				Scenarios)
				&& Scenarios)
		|| !TestTrue(
			TEXT("legacy source decisions exist"),
			Fixture->TryGetArrayField(
				TEXT("legacy_sources"),
				LegacySources)
				&& LegacySources))
	{
		return false;
	}

	const int32 DeclaredCount = static_cast<int32>(
		Fixture->GetNumberField(TEXT("descriptor_count")));
	TestEqual(TEXT("two formal descriptors"), DeclaredCount, 2);
	TestEqual(
		TEXT("descriptor count is exact"),
		Descriptors->Num(),
		DeclaredCount);
	TestEqual(
		TEXT("one schema case per descriptor"),
		Cases->Num(),
		DeclaredCount);
	TestEqual(
		TEXT("one E2 scenario per descriptor"),
		Scenarios->Num(),
		DeclaredCount);

	TSet<FString> AllowedNativeTypes;
	for (const TSharedPtr<FJsonValue>& Value
		: Fixture->GetArrayField(
			TEXT("native_operation_types")))
	{
		AllowedNativeTypes.Add(Value->AsString());
	}
	TestEqual(
		TEXT("two Landscape-only native operation types"),
		AllowedNativeTypes.Num(),
		2);
	TestTrue(
		TEXT("create native type is explicit"),
		AllowedNativeTypes.Contains(
			TEXT("landscape.geometry.create")));
	TestTrue(
		TEXT("height-delta native type is explicit"),
		AllowedNativeTypes.Contains(
			TEXT("landscape.geometry.height_delta")));

	TMap<FString, TSharedPtr<FJsonObject>> RowsByDescriptor;
	TSet<FString> CapabilityIds;
	TSet<FString> DescriptorHashes;
	bool bFixtureValid = true;
	for (const TSharedPtr<FJsonValue>& Value : *Descriptors)
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		if (!Row.IsValid())
		{
			AddError(TEXT("landscape_descriptor_row_invalid"));
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
			Row->GetStringField(
				TEXT("native_operation_type"));

		TestTrue(
			*(DescriptorId + TEXT(" is a Landscape operation")),
			DescriptorId.StartsWith(
				TEXT("uai.operation.landscape.")));
		TestTrue(
			*(DescriptorId
				+ TEXT(" has a Landscape-only native type")),
			AllowedNativeTypes.Contains(NativeType));
		TestFalse(
			*(DescriptorId + TEXT(" has no legacy identity")),
			Row->HasField(TEXT("legacy_command_id"))
				|| Row->HasField(TEXT("command"))
				|| Row->HasField(TEXT("route")));
		TestEqual(
			*(DescriptorId + TEXT(" freezes its exact hash")),
			DescriptorHash,
			ComputeDescriptorHash(this, Row));

		if (RowsByDescriptor.Contains(DescriptorId)
			|| CapabilityIds.Contains(CapabilityId)
			|| DescriptorHashes.Contains(DescriptorHash))
		{
			AddError(
				TEXT("landscape_descriptor_identity_duplicate:")
				+ DescriptorId);
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

		const TSharedPtr<FJsonObject> SpaceContract =
			Row->GetObjectField(TEXT("space_contract"));
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
				SpaceContract->HasField(Field));
		}
		TestEqual(
			*(DescriptorId
				+ TEXT(" is explicitly three-dimensional")),
			static_cast<int32>(
				SpaceContract->GetNumberField(
					TEXT("dimension"))),
			3);
		TestTrue(
			*(DescriptorId
				+ TEXT(" write frame is Landscape-specific")),
			SpaceContract
				->GetStringField(TEXT("write_frame"))
				.Contains(TEXT("landscape")));
	}
	TestTrue(
		TEXT("all descriptor rows satisfy the target boundary"),
		bFixtureValid);

	for (const TSharedPtr<FJsonValue>& Value : *Cases)
	{
		const TSharedPtr<FJsonObject> Case = Value->AsObject();
		const FString DescriptorId =
			Case->GetStringField(TEXT("descriptor_id"));
		const TSharedPtr<FJsonObject>* Row =
			RowsByDescriptor.Find(DescriptorId);
		if (!TestTrue(
				*(DescriptorId + TEXT(" case targets a descriptor")),
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

		TOptional<FUeAgentContractValue> Canonical;
		FUeAgentContractSchemaError Error;
		TestTrue(
			*(DescriptorId
				+ TEXT(" accepts its complete space contract")),
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
				Canonical,
				Error));
		TestTrue(
			*(DescriptorId
				+ TEXT(" produces canonical validated input")),
			Canonical.IsSet() && Canonical->IsValid());

		Canonical.Reset();
		Error = {};
		TestFalse(
			*(DescriptorId
				+ TEXT(" rejects an incomplete space contract")),
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
				Canonical,
				Error));
		TestTrue(
			*(DescriptorId
				+ TEXT(" returns a typed schema failure")),
			Error.IsValid());
	}

	TSet<FString> NegativeCaseIds;
	for (const TSharedPtr<FJsonValue>& Value : *NegativeCases)
	{
		const TSharedPtr<FJsonObject> Case = Value->AsObject();
		const FString CaseId =
			Case->GetStringField(TEXT("case_id"));
		TestTrue(
			*(CaseId + TEXT(" is unique")),
			!NegativeCaseIds.Contains(CaseId));
		NegativeCaseIds.Add(CaseId);
		TestTrue(
			*(CaseId + TEXT(" targets a formal descriptor")),
			RowsByDescriptor.Contains(
				Case->GetStringField(TEXT("descriptor_id"))));
		TestEqual(
			*(CaseId + TEXT(" fails before mutation")),
			static_cast<int32>(
				Case->GetNumberField(TEXT("mutation_count"))),
			0);
		TestFalse(
			*(CaseId + TEXT(" has a stable typed error")),
			Case->GetStringField(
				TEXT("expected_error")).IsEmpty());
	}
	TestEqual(
		TEXT("five non-schema preflight failures are frozen"),
		NegativeCaseIds.Num(),
		5);

	TSet<FString> ScenarioDescriptors;
	for (const TSharedPtr<FJsonValue>& Value : *Scenarios)
	{
		const TSharedPtr<FJsonObject> Scenario =
			Value->AsObject();
		const FString DescriptorId =
			Scenario->GetStringField(TEXT("descriptor_id"));
		TestTrue(
			*(DescriptorId + TEXT(" E2 oracle is unique")),
			!ScenarioDescriptors.Contains(DescriptorId));
		ScenarioDescriptors.Add(DescriptorId);
		for (const TCHAR* Field : {
			TEXT("oracle"),
			TEXT("rollback_oracle"),
			TEXT("negative_oracle"),
		})
		{
			TestFalse(
				*FString::Printf(
					TEXT("%s E2 %s is explicit"),
					*DescriptorId,
					Field),
				Scenario->GetStringField(Field).IsEmpty());
		}
	}
	TestEqual(
		TEXT("both descriptors own an independent E2 oracle"),
		ScenarioDescriptors.Num(),
		DeclaredCount);

	bool bSawLegacySmokeDelete = false;
	for (const TSharedPtr<FJsonValue>& Value : *LegacySources)
	{
		const TSharedPtr<FJsonObject> Legacy =
			Value->AsObject();
		FString LegacyTestId;
		if (Legacy->TryGetStringField(
				TEXT("legacy_test_id"),
				LegacyTestId)
			&& LegacyTestId
			== TEXT(
				"GptProjectTest.UeAgentInterface.Smoke."
				"LandscapeWorkflow"))
		{
			bSawLegacySmokeDelete = true;
			TestEqual(
				TEXT("monolithic Landscape smoke is deleted"),
				Legacy->GetStringField(TEXT("terminal")),
				FString(TEXT("DELETE")));
		}
	}
	TestTrue(
		TEXT("legacy smoke terminal is explicit"),
		bSawLegacySmokeDelete);

	TSharedPtr<const FUeAgentDescriptorRegistry> ProductionRegistry;
	FString RegistryError;
	const bool bCreated = TestTrue(
		TEXT("create production LandscapeGeometryEffect registry"),
		UE::UAI::DomainAdapters::LandscapeGeometryEffect::
			TryCreateLandscapeGeometryEffectDescriptorRegistry(
				ProductionRegistry,
				RegistryError));
	if (!bCreated)
	{
		AddError(
			TEXT("landscape_geometry_registry_failed:")
			+ RegistryError);
	}
	if (ProductionRegistry.IsValid())
	{
		TestEqual(
			TEXT("production registry owns two descriptors"),
			ProductionRegistry->GetDescriptors().Num(),
			DeclaredCount);
		for (const TSharedPtr<FJsonValue>& Value
			: *Descriptors)
		{
			const TSharedPtr<FJsonObject> Row =
				Value->AsObject();
			const FUeAgentOperationDescriptor* Descriptor =
				ProductionRegistry->FindByDescriptorId(
					Row->GetStringField(
						TEXT("descriptor_id")));
			TestTrue(
				*(Row->GetStringField(TEXT("descriptor_id"))
					+ TEXT(" is an exact production operation")),
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
