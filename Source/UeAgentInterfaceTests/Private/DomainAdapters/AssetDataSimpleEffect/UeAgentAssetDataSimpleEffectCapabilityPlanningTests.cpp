#include "Misc/AutomationTest.h"

#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTestSupport.h"

#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"

#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectFactoryCandidateTestSupport.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Serialization/JsonWriter.h"

namespace
{
	using namespace UE::UAI::Tests::AssetDataSimpleEffect;

	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
	MakeAssetDataResources()
	{
		return MakeShared<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe>();
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

	struct FSnapshotOptions
	{
		bool bIncludeTarget = false;
		bool bIncludeSource = false;
		FString TargetRevision = TEXT("rev:target:1");
		FString SourceRevision = TEXT("rev:source:1");
		FString TargetProfile;
		TArray<FString> AdditionalSourceRefs;
	};

	FUeAgentCanonicalSnapshotPtr MakeSnapshot(
		FAutomationTestBase* Test,
		const FUeAgentOperationDescriptor& Descriptor,
		const TSharedPtr<FJsonObject>& Row,
		const FSnapshotOptions& Options,
		FUeAgentResourceTracker& Resources)
	{
		const FUeAgentStateRevision SnapshotRevision(
			TEXT("asset-data-e1"),
			TEXT("project:asset-data:r1"),
			1);
		TArray<FUeAgentStateEntry> Entries;
		auto AppendEntry =
			[&](
				const FString& Ref,
				const FString& Revision,
				const FString& Profile)
			{
				const TSharedPtr<FJsonObject> State =
					MakeShared<FJsonObject>();
				State->SetStringField(TEXT("target_ref"), Ref);
				State->SetStringField(
					TEXT("asset_revision"),
					Revision);
				State->SetStringField(
					TEXT("profile_id"),
					Profile);
				State->SetStringField(
					TEXT("asset_class"),
					TEXT("/Script/Engine.DataAsset"));
				State->SetStringField(
					TEXT("field_fingerprint"),
					TEXT("fixture:fields:r1"));

				FUeAgentCanonicalValuePtr Current;
				FUeAgentStateSemanticError ValueError;
				if (!TryCanonicalValue(
						MakeShared<FJsonValueObject>(State),
						Resources,
						Current,
						ValueError))
				{
					return false;
				}
				FUeAgentCanonicalValuePtr Default;
				if (!FUeAgentCanonicalValue::TryObject(
						{},
						Resources,
						Default,
						ValueError))
				{
					return false;
				}
				Entries.Emplace(
					FUeAgentCanonicalIdentity(
						TEXT("uai.asset_data:") + Ref,
						Row->GetStringField(
							TEXT("native_operation_type")),
						TEXT("/Game/UAI/AssetData"),
						Ref,
						TEXT("asset_data"),
						TEXT("project:asset-data:r1")),
					FUeAgentCanonicalType(
						TEXT("asset_data_current_state"),
						EUeAgentValueKind::Object,
						false,
						TEXT("object")),
					Current.ToSharedRef(),
					FUeAgentDefaultValue(
						EUeAgentDefaultSourceKind::Schema,
						TEXT("asset-data:e1:empty-default"),
						TEXT("test-only independent state"),
						Default.ToSharedRef()),
					FUeAgentStateDetails(
						Ref,
						TEXT(""),
						{TEXT("AssetDataSimpleEffect")},
						true,
						TEXT("available"),
						{}),
					SnapshotRevision);
				return true;
			};

		const FString Profile =
			Options.TargetProfile.IsEmpty()
				? Row->GetStringField(TEXT("profile_id"))
				: Options.TargetProfile;
		if (Options.bIncludeTarget
			&& !AppendEntry(
				TEXT("uai://asset/fixture-target"),
				Options.TargetRevision,
				Profile))
		{
			Test->AddError(
				TEXT("asset_data_target_snapshot_failed"));
			return nullptr;
		}
		if (Options.bIncludeSource
			&& !AppendEntry(
				TEXT("uai://asset/fixture-source"),
				Options.SourceRevision,
				TEXT("data_object")))
		{
			Test->AddError(
				TEXT("asset_data_source_snapshot_failed"));
			return nullptr;
		}
		for (const FString& SourceRef
			: Options.AdditionalSourceRefs)
		{
			if (!AppendEntry(
					SourceRef,
					Options.SourceRevision,
					TEXT("data_object")))
			{
				Test->AddError(
					TEXT(
						"asset_data_additional_source_"
						"snapshot_failed:") + SourceRef);
				return nullptr;
			}
		}

		FUeAgentCanonicalSnapshotPtr Snapshot;
		FUeAgentStateSemanticError Error;
		if (!Test->TestTrue(
				TEXT("create independent AssetData snapshot"),
				FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("snapshot:asset-data:")
						+ Descriptor.GetCapabilityId(),
					EUeAgentStatePlane::AuthoringMemory,
					SnapshotRevision,
					FUeAgentTemporalPoint(
						TEXT("owned-worker-clock"),
						TEXT("1")),
					FUeAgentStateProvenance(
						TEXT("independent_readback"),
						TEXT("owned-asset-profile-read-port"),
						TEXT("asset-data:prestate-readback")),
					Entries,
					Resources,
					Snapshot,
					Error)))
		{
			Test->AddError(
				TEXT("asset_data_snapshot_failed:")
					+ Error.Code);
			return nullptr;
		}
		return Snapshot;
	}

	FUeAgentExecutionContextRefOwner MakeExecutionContext(
		const FUeAgentDescriptorIdentity& Descriptor)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("project:asset-data");
		Input.EditorInstanceId = TEXT("editor:asset-data-worker");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = EUeAgentExecutionWorldType::Editor;
		Input.ContextRevision = TEXT("project:asset-data:r1");
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

	bool SerializeInput(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Input,
		FString& OutJson)
	{
		OutJson.Reset();
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&OutJson);
		return Test->TestTrue(
			TEXT("serialize AssetData canonical input"),
			FJsonSerializer::Serialize(
				Input.ToSharedRef(),
				Writer));
	}

	bool BuildDraft(
		FAutomationTestBase* Test,
		const FUeAgentOperationDescriptor& Descriptor,
		const TSharedPtr<FJsonObject>& Input,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		IUeAgentAssetDataSimpleEffectFactoryCandidateResolver&
			FactoryCandidates,
		FUeAgentResourceTracker& Resources,
		TOptional<FChangeSetDraft>& OutDraft,
		FExecutionMutationError& OutError)
	{
		FString InputJson;
		if (!SerializeInput(Test, Input, InputJson))
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
				TEXT("asset_data_input_schema_rejected"),
				SchemaError.Code + TEXT(":")
					+ SchemaError.Detail);
			return false;
		}
		const FUeAgentExecutionContextRefOwner Context =
			MakeExecutionContext(Descriptor.GetIdentity());
		return TryBuildAssetDataSimpleEffectChangeSetDraft(
			FUeAgentOperationRequest(
				Descriptor.GetIdentity(),
				TEXT("request:asset-data"),
				TEXT("idempotency:asset-data"),
				CanonicalPayload.GetValue()),
			*Context,
			Snapshot,
			FactoryCandidates,
			Resources,
			OutDraft,
			OutError);
	}

	bool HasCanonicalField(
		const FUeAgentCanonicalValuePtr& Value,
		const FString& Field)
	{
		if (!Value.IsValid()
			|| Value->GetKind() != EUeAgentValueKind::Object)
		{
			return false;
		}
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
			: Value->GetObjectItems())
		{
			if (Item.Key == Field)
			{
				return true;
			}
		}
		return false;
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

	EUeAgentValueKind ExpectedValueKind(const FString& ValueKind)
	{
		if (ValueKind == TEXT("boolean"))
		{
			return EUeAgentValueKind::Boolean;
		}
		if (ValueKind == TEXT("integer"))
		{
			return EUeAgentValueKind::Integer;
		}
		if (ValueKind == TEXT("number"))
		{
			return EUeAgentValueKind::Decimal;
		}
		if (ValueKind == TEXT("array"))
		{
			return EUeAgentValueKind::Array;
		}
		if (ValueKind == TEXT("object"))
		{
			return EUeAgentValueKind::Object;
		}
		return EUeAgentValueKind::String;
	}

	TSharedPtr<FJsonObject> FindRow(
		const TSharedPtr<FJsonObject>& Fixture,
		const FString& DescriptorId)
	{
		for (const TSharedPtr<FJsonValue>& Value
			: Fixture->GetArrayField(TEXT("descriptors")))
		{
			const TSharedPtr<FJsonObject> Row =
				Value->AsObject();
			if (Row->GetStringField(TEXT("descriptor_id"))
				== DescriptorId)
			{
				return Row;
			}
		}
		return nullptr;
	}

	FSnapshotOptions DefaultSnapshotOptions(
		const TSharedPtr<FJsonObject>& Row)
	{
		FSnapshotOptions Options;
		Options.bIncludeSource = IsClone(Row);
		Options.bIncludeTarget =
			!IsCreate(Row) && !IsClone(Row);
		const FString ProfileId =
			Row->GetStringField(TEXT("profile_id"));
		if (ProfileId == TEXT("texture_array")
			|| ProfileId == TEXT("texture_cube_array"))
		{
			Options.AdditionalSourceRefs = {
				TEXT("uai://asset/fixture-source"),
				TEXT("uai://asset/fixture-source-b"),
			};
		}
		else if (ProfileId == TEXT("texture_cube"))
		{
			Options.AdditionalSourceRefs = {
				TEXT("uai://asset/fixture-source"),
				TEXT("uai://asset/fixture-source-b"),
				TEXT("uai://asset/fixture-source-c"),
				TEXT("uai://asset/fixture-source-d"),
				TEXT("uai://asset/fixture-source-e"),
				TEXT("uai://asset/fixture-source-f"),
			};
		}
		else if (ProfileId == TEXT("volume_texture"))
		{
			Options.AdditionalSourceRefs = {
				TEXT("uai://asset/fixture-source"),
				TEXT("uai://asset/fixture-source-b"),
				TEXT("uai://asset/fixture-source-c"),
			};
		}
		else if (ProfileId == TEXT("sound_mix")
			|| ProfileId == TEXT("data_object"))
		{
			Options.AdditionalSourceRefs = {
				TEXT("uai://asset/fixture-source"),
			};
		}
		else if (ProfileId == TEXT("sound_submix"))
		{
			Options.AdditionalSourceRefs = {
				TEXT("uai://asset/fixture-source"),
				TEXT("uai://asset/fixture-source-b"),
			};
		}
		else if (ProfileId == TEXT("static_mesh"))
		{
			Options.AdditionalSourceRefs = {
				TEXT("uai://asset/fixture-material"),
			};
		}
		else if (ProfileId == TEXT("skeletal_mesh"))
		{
			Options.AdditionalSourceRefs = {
				TEXT("uai://asset/fixture-material"),
				TEXT("uai://asset/fixture-lod-settings"),
				TEXT("uai://asset/fixture-skeleton"),
			};
		}
		return Options;
	}

	bool RejectsAtSchema(
		const FUeAgentOperationDescriptor& Descriptor,
		const TSharedPtr<FJsonObject>& Input)
	{
		FString InputJson;
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&InputJson);
		if (!FJsonSerializer::Serialize(
				Input.ToSharedRef(),
				Writer))
		{
			return false;
		}
		TSharedPtr<const FUeAgentContractSchema> Compiled;
		FUeAgentContractSchemaError Error;
		if (!FUeAgentContractSchema::TryCompile(
				Descriptor.GetInputSchema(),
				Compiled,
				Error)
			|| !Compiled.IsValid())
		{
			return false;
		}
		TOptional<FUeAgentContractValue> Canonical;
		return !Compiled->TryValidateAndCanonicalize(
			FUeAgentContractValue(InputJson),
			[](int64,
				const FString&,
				FUeAgentContractSchemaError&)
			{
				return true;
			},
			Canonical,
			Error);
	}

	bool RunPlanningTests(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Fixture)
	{
		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString RegistryError;
		if (!Test->TestTrue(
				TEXT("create AssetData production registry"),
				TryCreateAssetDataSimpleEffectDescriptorRegistry(
					Registry,
					RegistryError))
			|| !Registry.IsValid())
		{
			return false;
		}
		const TSharedRef<
			FFixtureFactoryCandidateResolver,
			ESPMode::ThreadSafe> FactoryCandidates =
				MakeShared<
					FFixtureFactoryCandidateResolver,
					ESPMode::ThreadSafe>(Fixture);

		for (const TSharedPtr<FJsonValue>& Value
			: Fixture->GetArrayField(TEXT("descriptors")))
		{
			const TSharedPtr<FJsonObject> Row =
				Value->AsObject();
			const FString DescriptorId =
				Row->GetStringField(TEXT("descriptor_id"));
			const FUeAgentOperationDescriptor* Descriptor =
				Registry->FindByDescriptorId(DescriptorId);
			if (!Test->TestNotNull(
					*(DescriptorId
						+ TEXT(" production descriptor exists")),
					Descriptor))
			{
				continue;
			}
			const TSharedRef<
				FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources =
					MakeAssetDataResources();
			const FUeAgentCanonicalSnapshotPtr Snapshot =
				MakeSnapshot(
					Test,
					*Descriptor,
					Row,
					DefaultSnapshotOptions(Row),
					*Resources);
			if (!Snapshot.IsValid())
			{
				continue;
			}

			TOptional<FChangeSetDraft> Draft;
			FExecutionMutationError Error;
			const bool bBuilt = BuildDraft(
				Test,
				*Descriptor,
				BuildValidInput(Fixture, Row),
				Snapshot.ToSharedRef(),
				*FactoryCandidates,
				*Resources,
				Draft,
				Error);
			Test->TestTrue(
				*(DescriptorId + TEXT(" builds typed draft")),
				bBuilt);
			if (!bBuilt || !Draft.IsSet() || !Draft->IsValid())
			{
				Test->AddError(
					DescriptorId + TEXT(":")
						+ Error.GetCode());
				continue;
			}
			Test->TestEqual(
				*(DescriptorId
					+ TEXT(" produces one atomic change")),
				Draft->GetChanges().Num(),
				1);
			const FChangeDraft& Change =
				Draft->GetChanges()[0];
			Test->TestEqual(
				*(DescriptorId
					+ TEXT(" freezes unified native type")),
				Change.GetIdentity().GetTargetKind(),
				Row->GetStringField(
					TEXT("native_operation_type")));
			Test->TestEqual(
				*(DescriptorId
					+ TEXT(" freezes correct mutation intent")),
				Change.GetIntent(),
				IsCreate(Row) || IsClone(Row)
					? EChangeIntent::Create
					: EChangeIntent::Update);
			Test->TestEqual(
				*(DescriptorId
					+ TEXT(" before-state presence is exact")),
				Change.GetExpectedBefore().IsValid(),
				!IsCreate(Row) && !IsClone(Row));
			for (const FString& Field
				: TArray<FString>{
					TEXT("native_operation_type"),
					TEXT("native_binding_id"),
					TEXT("profile_id"),
					TEXT("target_ref"),
					TEXT("typed_changes"),
				})
			{
				Test->TestTrue(
					*(DescriptorId
						+ TEXT(" freezes ")
						+ Field),
					HasCanonicalField(
						Change.GetProposedAfter(),
						Field));
			}
			if (IsClone(Row))
			{
				Test->TestTrue(
					*(DescriptorId
						+ TEXT(" freezes source_ref")),
					HasCanonicalField(
						Change.GetProposedAfter(),
						TEXT("source_ref")));
				Test->TestTrue(
					*(DescriptorId
						+ TEXT(" freezes source revision")),
					HasCanonicalField(
						Change.GetProposedAfter(),
						TEXT("source_revision")));
			}
			else if (FindFactoryContract(Fixture, Row).IsValid())
			{
				for (const FString& Field
					: TArray<FString>{
						TEXT("factory_binding_id"),
						TEXT("factory_catalog_source_id"),
						TEXT("factory_catalog_generation"),
						TEXT("factory_catalog_sequence"),
					})
				{
					Test->TestTrue(
						*(DescriptorId
							+ TEXT(" freezes ")
							+ Field),
						HasCanonicalField(
							Change.GetProposedAfter(),
							Field));
				}
				Test->TestFalse(
					*(DescriptorId
						+ TEXT(" does not freeze public factory selector")),
					HasCanonicalField(
						Change.GetProposedAfter(),
						TEXT("factory")));
			}
			else if (!IsCreate(Row))
			{
				const TArray<TSharedPtr<FJsonObject>> BoundFields =
					FieldContracts(Fixture, Row);
				const FUeAgentCanonicalValue* TypedChanges =
					FindCanonicalField(
						*Change.GetProposedAfter(),
						TEXT("typed_changes"));
				const FUeAgentCanonicalValue* Unit =
					TypedChanges
						&& TypedChanges->GetKind()
							== EUeAgentValueKind::Array
						&& !TypedChanges->GetArrayItems().IsEmpty()
						? &TypedChanges->GetArrayItems()[0].Get()
						: nullptr;
				const FUeAgentCanonicalValue* Binding =
					Unit
						? FindCanonicalField(
							*Unit,
							TEXT("field_binding_id"))
						: nullptr;
				const FUeAgentCanonicalValue* TypedValue =
					Unit
						? FindCanonicalField(*Unit, TEXT("value"))
						: nullptr;
				const FUeAgentCanonicalValue* TypeId =
					TypedValue
						? FindCanonicalField(
							*TypedValue,
							TEXT("type_id"))
						: nullptr;
				const FUeAgentCanonicalValue* CanonicalValue =
					TypedValue
						? FindCanonicalField(
							*TypedValue,
							TEXT("canonical_value"))
						: nullptr;
				Test->TestEqual(
					*(DescriptorId
						+ TEXT(" freezes one field binding")),
					Binding
						? Binding->GetText()
						: FString(),
					BoundFields[0]->GetStringField(
						TEXT("field_binding_id")));
				Test->TestEqual(
					*(DescriptorId
						+ TEXT(" freezes exact type id")),
					TypeId ? TypeId->GetText() : FString(),
					BoundFields[0]->GetStringField(TEXT("type_id")));
				Test->TestEqual(
					*(DescriptorId
						+ TEXT(" preserves canonical value kind")),
					CanonicalValue
						? CanonicalValue->GetKind()
						: EUeAgentValueKind::Null,
					ExpectedValueKind(
						BoundFields[0]->GetStringField(
							TEXT("kind"))));
			}
			for (const FString& CrossOwnerField
				: TArray<FString>{
					TEXT("command"),
					TEXT("folder_path"),
					TEXT("hjson"),
					TEXT("property_path"),
					TEXT("route"),
					TEXT("save"),
					TEXT("transaction"),
					TEXT("verify"),
				})
			{
				Test->TestFalse(
					*(DescriptorId
						+ TEXT(" excludes ")
						+ CrossOwnerField),
					HasCanonicalField(
						Change.GetProposedAfter(),
						CrossOwnerField));
			}
		}

		for (const TSharedPtr<FJsonValue>& Value
			: Fixture->GetArrayField(TEXT("e1_negative_cases")))
		{
			const TSharedPtr<FJsonObject> Case =
				Value->AsObject();
			const FString CaseId =
				Case->GetStringField(TEXT("case_id"));
			const FString DescriptorId =
				Case->GetStringField(TEXT("descriptor_id"));
			const TSharedPtr<FJsonObject> Row =
				FindRow(Fixture, DescriptorId);
			const FUeAgentOperationDescriptor* Descriptor =
				Registry->FindByDescriptorId(DescriptorId);
			if (!Row.IsValid()
				|| !Test->TestNotNull(
					*(CaseId + TEXT(" descriptor exists")),
					Descriptor))
			{
				continue;
			}

			const TSharedPtr<FJsonObject> Input =
				BuildValidInput(Fixture, Row);
			FSnapshotOptions Options =
				DefaultSnapshotOptions(Row);
			bool bSchemaCase = false;
			if (CaseId == TEXT("forbidden_save"))
			{
				Input->SetBoolField(TEXT("save"), true);
				bSchemaCase = true;
			}
			else if (CaseId == TEXT("forbidden_hjson"))
			{
				Input->SetStringField(
					TEXT("hjson"),
					TEXT("fixture"));
				bSchemaCase = true;
			}
			else if (CaseId
				== TEXT("forbidden_property_path"))
			{
				Input->SetStringField(
					TEXT("property_path"),
					TEXT("Any.Property"));
				bSchemaCase = true;
			}
			else if (CaseId == TEXT("unknown_field_id"))
			{
				Input->GetArrayField(TEXT("changes"))[0]
					->AsObject()
					->SetStringField(
						TEXT("field_id"),
						TEXT("unknown_field"));
				bSchemaCase = true;
			}
			else if (CaseId == TEXT("presence_not_allowed"))
			{
				Input->GetArrayField(TEXT("changes"))[0]
					->AsObject()
					->SetStringField(
						TEXT("presence"),
						TEXT("clear"));
				bSchemaCase = true;
			}
			else if (CaseId == TEXT("implicit_empty_clear"))
			{
				Input->GetArrayField(TEXT("changes"))[0]
					->AsObject()
					->RemoveField(TEXT("presence"));
				bSchemaCase = true;
			}
			else if (CaseId == TEXT("wrong_type_id"))
			{
				Input->GetArrayField(TEXT("changes"))[0]
					->AsObject()
					->GetObjectField(TEXT("value"))
					->SetStringField(
						TEXT("type_id"),
						TEXT("uai.value.wrong.v1"));
				bSchemaCase = true;
			}
			else if (CaseId == TEXT("wrong_value_kind"))
			{
				Input->GetArrayField(TEXT("changes"))[0]
					->AsObject()
					->GetObjectField(TEXT("value"))
					->SetStringField(
						TEXT("canonical_value"),
						TEXT("stringified-object"));
				bSchemaCase = true;
			}
			else if (CaseId == TEXT("public_field_binding"))
			{
				Input->GetArrayField(TEXT("changes"))[0]
					->AsObject()
					->SetStringField(
						TEXT("field_binding_id"),
						TEXT("forged"));
				bSchemaCase = true;
			}
			else if (CaseId == TEXT("factory_candidate_missing"))
			{
				Input->RemoveField(TEXT("factory"));
				bSchemaCase = true;
			}
			else if (CaseId
				== TEXT("factory_candidate_class_mismatch"))
			{
				Input->GetObjectField(TEXT("factory"))
					->SetStringField(TEXT("name"), TEXT("missing-factory"));
			}
			else if (CaseId == TEXT("create_target_exists"))
			{
				Options.bIncludeTarget = true;
			}
			else if (CaseId == TEXT("clone_source_missing"))
			{
				Options.bIncludeSource = false;
			}
			else if (CaseId == TEXT("update_target_missing"))
			{
				Options.bIncludeTarget = false;
			}
			else if (CaseId == TEXT("profile_mismatch"))
			{
				Options.TargetProfile =
					TEXT("different_profile");
			}
			else if (CaseId == TEXT("duplicate_field_unit"))
			{
				const TArray<TSharedPtr<FJsonValue>>& Changes =
					Input->GetArrayField(TEXT("changes"));
				const TSharedPtr<FJsonObject> Original =
					Changes[0]->AsObject();
				const TSharedPtr<FJsonObject> Duplicate =
					MakeShared<FJsonObject>();
				Duplicate->SetStringField(
					TEXT("field_id"),
					Original->GetStringField(TEXT("field_id")));
				Duplicate->SetStringField(
					TEXT("presence"),
					Original->GetStringField(TEXT("presence")));
				const TSharedPtr<FJsonObject> DuplicateValue =
					MakeShared<FJsonObject>();
				DuplicateValue->SetStringField(
					TEXT("type_id"),
					Original->GetObjectField(TEXT("value"))
						->GetStringField(TEXT("type_id")));
				TSharedPtr<FJsonValue> Distinct =
					CloneJsonValue(
						Original->GetObjectField(TEXT("value"))
							->TryGetField(TEXT("canonical_value")));
				if (Distinct->Type == EJson::Array)
				{
					TArray<TSharedPtr<FJsonValue>> Items =
						Distinct->AsArray();
					Items.Add(
						MakeShared<FJsonValueString>(
							TEXT("different")));
					Distinct =
						MakeShared<FJsonValueArray>(
							MoveTemp(Items));
				}
				else if (Distinct->Type == EJson::Object)
				{
					Distinct->AsObject()->SetBoolField(
						TEXT("different"),
						true);
				}
				else if (Distinct->Type == EJson::String)
				{
					Distinct =
						MakeShared<FJsonValueString>(
							Distinct->AsString()
								+ TEXT("-different"));
				}
				DuplicateValue->SetField(
					TEXT("canonical_value"),
					Distinct);
				Duplicate->SetObjectField(
					TEXT("value"),
					DuplicateValue);
				TArray<TSharedPtr<FJsonValue>> NewChanges =
					Changes;
				NewChanges.Add(
					MakeShared<FJsonValueObject>(Duplicate));
				Input->SetArrayField(
					TEXT("changes"),
					MoveTemp(NewChanges));
			}
			else if (CaseId == TEXT("reference_unresolved"))
			{
				Input->GetArrayField(TEXT("changes"))[0]
					->AsObject()
					->GetObjectField(TEXT("value"))
					->SetStringField(
						TEXT("canonical_value"),
						TEXT("uai://asset/missing-reference"));
			}
			else if (CaseId
				== TEXT("nested_reference_unresolved"))
			{
				const TSharedPtr<FJsonObject> Nested =
					MakeShared<FJsonObject>();
				Nested->SetStringField(
					TEXT("ref"),
					TEXT("uai://asset/missing-reference"));
				const TSharedPtr<FJsonObject> Root =
					MakeShared<FJsonObject>();
				Root->SetObjectField(TEXT("nested"), Nested);
				Input->GetArrayField(TEXT("changes"))[0]
					->AsObject()
					->GetObjectField(TEXT("value"))
					->SetObjectField(
						TEXT("canonical_value"),
						Root);
			}

			if (bSchemaCase)
			{
				Test->TestTrue(
					*(CaseId + TEXT(" rejected by C0 schema")),
					RejectsAtSchema(*Descriptor, Input));
				continue;
			}

			const TSharedRef<
				FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources =
					MakeAssetDataResources();
			const FUeAgentCanonicalSnapshotPtr Snapshot =
				MakeSnapshot(
					Test,
					*Descriptor,
					Row,
					Options,
					*Resources);
			if (!Snapshot.IsValid())
			{
				continue;
			}
			TOptional<FChangeSetDraft> Draft;
			FExecutionMutationError Error;
			const bool bBuilt = BuildDraft(
				Test,
				*Descriptor,
				Input,
				Snapshot.ToSharedRef(),
				*FactoryCandidates,
				*Resources,
				Draft,
				Error);
			Test->TestFalse(
				*(CaseId + TEXT(" fails before mutation")),
				bBuilt);
			Test->TestEqual(
				*(CaseId + TEXT(" returns stable error")),
				Error.GetCode(),
				Case->GetStringField(TEXT("expected_error")));
			Test->TestFalse(
				*(CaseId + TEXT(" produces no draft")),
				Draft.IsSet());
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDataSimpleEffectPlanningTest,
	"UeAgentInterface.DomainAdapters.AssetDataSimpleEffect.E1."
		"TypedPlanningAndPreflight",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDataSimpleEffectPlanningTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	return RunPlanningTests(this, Fixture);
}
