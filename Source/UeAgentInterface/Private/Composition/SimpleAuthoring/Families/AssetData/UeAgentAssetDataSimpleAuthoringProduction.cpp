#include "Composition/SimpleAuthoring/Families/AssetData/UeAgentAssetDataSimpleAuthoringProduction.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentExecutionContextPolicy.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Core/AssetDurable/UeAgentPackageAssetRevision.h"
#include "Core/AssetDurable/UeAgentPersistedPackage.h"
#include "Core/AssetDurable/UeAgentPersistedWorld.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include <initializer_list>

namespace UE::UAI::Composition::SimpleAuthoring::AssetData
{
	namespace
	{
		using namespace UE::UAI::AssetDurable;
		using namespace UE::UAI::ContextResource;
		using namespace UE::UAI::Contract;
		using namespace UE::UAI::Discovery;
		using namespace
			UE::UAI::DomainAdapters::AssetDataSimpleEffect;
		using namespace UE::UAI::DomainAdapters::GenericEffect;
		using namespace UE::UAI::ExecutionMutation;
		using namespace UE::UAI::PlanningPreflight;
		using namespace UE::UAI::StateSemantic;
		using namespace UE::UAI::Verification;

		constexpr const TCHAR* PreStatePortId =
			TEXT("asset-data.authoring-memory.v1");
		constexpr const TCHAR* PersistedStatePortId =
			TEXT("asset-data.independent-readback.v1");
		constexpr const TCHAR* RequestEvidencePrefix =
			TEXT("asset-data-request-v1:");
		constexpr const TCHAR* EvidencePackageName =
			TEXT("_uai_package_name");
		constexpr const TCHAR* EvidenceObjectName =
			TEXT("_uai_object_name");
		constexpr const TCHAR* EvidenceObjectPath =
			TEXT("_uai_object_path");
		constexpr const TCHAR* EvidenceExpectedClass =
			TEXT("_uai_expected_class");
		constexpr const TCHAR* EvidenceSourceRef =
			TEXT("_uai_source_ref");
		constexpr const TCHAR* EvidenceSourceObjectPath =
			TEXT("_uai_source_object_path");
		constexpr const TCHAR* EvidenceSourceClass =
			TEXT("_uai_source_class");
		constexpr const TCHAR* EvidenceSourceRevision =
			TEXT("_uai_source_revision");
		constexpr const TCHAR* EvidenceTargetRevision =
			TEXT("_uai_target_revision");

		using FAssetDataProfile =
			FUeAgentAssetDataSimpleEffectProductionProfile;

		struct FAssetDataProfileCatalog
		{
			FAssetDataProfileCatalog()
			{
				TryGetAssetDataSimpleEffectProductionProfiles(
					Profiles,
					Error);
			}

			TArray<FAssetDataProfile> Profiles;
			UE::UAI::Contract::FDiagnostic Error;
		};

		const FAssetDataProfileCatalog& GetProfileCatalog()
		{
			static const FAssetDataProfileCatalog Catalog;
			return Catalog;
		}

		const FAssetDataProfile* FindProfile(
			const FUeAgentDescriptorIdentity& Identity)
		{
			for (const FAssetDataProfile& Profile
				: GetProfileCatalog().Profiles)
			{
				if (Identity.GetDescriptorId()
						== Profile.DescriptorId
					&& Identity.GetVersion()
						== Profile.DescriptorVersion
					&& Identity.GetDescriptorHash()
						== Profile.DescriptorHash)
				{
					return &Profile;
				}
			}
			return nullptr;
		}

		bool IsSupportedProductionProfile(
			const FAssetDataProfile& Profile)
		{
			return (Profile.IsCreate() || Profile.IsClone())
					? Profile.FieldIds.IsEmpty()
					: !Profile.FieldIds.IsEmpty()
						&& CanProjectAssetDataSimpleEffectFieldState(
							Profile.NativeMechanicId);
		}

		bool IsMutableProductionProfile(
			const FAssetDataProfile& Profile)
		{
			return !Profile.IsCreate() && !Profile.IsClone()
				&& IsSupportedProductionProfile(Profile);
		}

		bool IsGenericFactoryProfile(
			const FAssetDataProfile& Profile)
		{
			return Profile.CapabilityId == TEXT("asset.object.create");
		}

		bool TryResolveCloneSource(
			const TSharedRef<IUeAgentAssetDataSimpleEffectAssetResolver>& Resolver,
			const FString& SourceRef,
			UObject*& OutSource,
			FExecutionMutationError& OutError)
		{
			return Resolver->TryResolveCloneSource(
				SourceRef, OutSource, OutError);
		}

		void SetError(FUeAgentAssetDataSimpleAuthoringError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic,
            const FString& SubjectId = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SubjectId;
        }

		void SetProductionError(FUeAgentSimpleAuthoringProductionError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic,
            const FString& SubjectId = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SubjectId;
        }

		FDiagnostic ProductionFailure(const TCHAR* Code, const FString& Reason, const FString& Path,
			const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected,
			const FString& SourceKind = TEXT("native_result"))
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("asset_data_production"));
			Error.SourceKind = SourceKind;
			Error.PropertyPath = Path;
			Error.ActualValue = Actual;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValue = Expected;
			if (SourceKind == TEXT("command_input"))
			{
				Error.SemanticPointer = Path;
				Error.Source.JsonPath = Path;
			}
			return Error;
		}

		void MarkNativeFailure(FDiagnostic& Error)
		{
			Error.SourceKind = TEXT("native_result");
			if (Error.Source.InternalJsonPath.IsEmpty()) Error.Source.InternalJsonPath = Error.Source.JsonPath;
			static_cast<FPublicSourceLocation&>(Error.Source) = FPublicSourceLocation();
			Error.SemanticPointer.Reset();
			for (FDiagnostic& Child : Error.Diagnostics) MarkNativeFailure(Child);
			if (Error.PublicCause.IsValid())
			{
				auto Cause = MakeShared<FDiagnostic>(*Error.PublicCause);
				MarkNativeFailure(*Cause);
				Error.PublicCause = Cause;
			}
		}

		struct FRequiredEvidenceString
		{
			const TCHAR* Key;
			const TCHAR* PublicField;
			FString* Output;
			bool bInternalValue = false;
		};

		bool TryReadEvidenceStrings(const TSharedPtr<FJsonObject>& Payload,
			std::initializer_list<FRequiredEvidenceString> Fields, const TCHAR* Code, FDiagnostic& OutError)
		{
			for (const auto& Field : Fields) Field.Output->Reset();
			if (!Payload.IsValid())
			{
				OutError = ProductionFailure(Code, TEXT("The captured asset evidence object is unavailable."),
					TEXT("evidence"), nullptr, MakeShared<FJsonValueString>(TEXT("object")));
				return false;
			}
			// Keep the existing type-check order before checking any empty strings.
			for (const auto& Field : Fields)
			{
				if (!Payload->TryGetStringField(Field.Key, *Field.Output))
				{
					const auto* Value = Payload->Values.Find(Field.Key);
					OutError = ProductionFailure(Code,
						FString::Printf(TEXT("The captured asset evidence requires a string for %s."), Field.PublicField),
						Field.PublicField, Field.bInternalValue ? nullptr : (Value ? *Value : nullptr),
						MakeShared<FJsonValueString>(TEXT("string")));
					OutError.ActualValuePresent = Value && Value->IsValid();
					if (Field.bInternalValue && Value) OutError.ActualEvidence = *Value;
					OutError.Source.InternalJsonPath = Field.Key;
					return false;
				}
			}
			for (const auto& Field : Fields)
			{
				if (Field.Output->IsEmpty())
				{
					OutError = ProductionFailure(Code,
						FString::Printf(TEXT("The captured asset evidence has an empty %s."), Field.PublicField),
						Field.PublicField, MakeShared<FJsonValueString>(FString()),
						MakeShared<FJsonValueString>(TEXT("non-empty string")));
					OutError.Source.InternalJsonPath = Field.Key;
					return false;
				}
			}
			return true;
		}

		FDiagnostic ObjectEvidenceMismatch(const TCHAR* Code, UObject* Object,
			const FString& ExpectedPath, const FString& ExpectedClass,
			const FString& ExpectedPackage, const FString& ExpectedName)
		{
			if (!IsValid(Object))
				return ProductionFailure(Code, TEXT("The resolved asset object is unavailable."),
					TEXT("object_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			const FString Actual[] = { Object->GetPathName(), Object->GetClass()->GetPathName(), Object->GetOutermost()->GetName(), Object->GetName() };
			const FString Expected[] = { ExpectedPath, ExpectedClass, ExpectedPackage, ExpectedName };
			const TCHAR* Fields[] = { TEXT("object_path"), TEXT("class"), TEXT("package_name"), TEXT("object_name") };
			for (int32 Index = 0; Index < 4; ++Index)
			{
				if (Actual[Index] != Expected[Index])
					return ProductionFailure(Code, FString::Printf(TEXT("The resolved asset %s differs from its captured evidence."), Fields[Index]),
						Fields[Index], MakeShared<FJsonValueString>(Actual[Index]), MakeShared<FJsonValueString>(Expected[Index]));
			}
			return ProductionFailure(Code, TEXT("The asset resolver reported failure without a diagnostic."),
				TEXT("resolution_succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		}

		bool TryValidateObservation(const FUeAgentPersistedCreatedAssetObservation& Observation, FDiagnostic& OutError)
		{
			if (Observation.IsValid()) return true;
			if (Observation.SourceObjectPath.IsEmpty())
				OutError = ProductionFailure(TEXT("asset_data_persisted_observation_invalid"), TEXT("The persisted observation has no source object path."), TEXT("source_object_path"), MakeShared<FJsonValueString>(Observation.SourceObjectPath), MakeShared<FJsonValueString>(TEXT("non-empty asset path")));
			else if (Observation.ClassPath.IsEmpty())
				OutError = ProductionFailure(TEXT("asset_data_persisted_observation_invalid"), TEXT("The persisted observation has no asset class."), TEXT("class"), MakeShared<FJsonValueString>(Observation.ClassPath), MakeShared<FJsonValueString>(TEXT("non-empty class path")));
			else if (Observation.SnapshotSequence < 0)
			{
				OutError = ProductionFailure(TEXT("asset_data_persisted_observation_invalid"), TEXT("The persisted observation has an invalid snapshot sequence."), TEXT("snapshot_sequence_valid"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.ActualEvidence = MakeShared<FJsonValueNumber>(Observation.SnapshotSequence);
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(TEXT("non-negative sequence"));
			}
			else
				OutError = ProductionFailure(TEXT("asset_data_persisted_observation_invalid"), TEXT("The persisted world has no persistent level."), TEXT("persistent_level_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.TargetPath = Observation.SourceObjectPath;
			return false;
		}

		Contract::FDiagnostic RevisionMismatch(const TCHAR* Code, const FString& Reason, const FString& Target,
			const FString& ActualRevision, const FString& ExpectedRevision)
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("revision_check"));
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.TargetPath = Target;
			Diagnostic.PropertyPath = TEXT("revision_matches");
			Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(ActualRevision);
			Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedRevision);
			return Diagnostic;
		}

		bool TrySerializeJson(const TSharedPtr<FJsonObject>& Object, FString& OutJson, FDiagnostic& OutError)
		{
			OutJson.Reset();
			if (!Object.IsValid())
			{
				OutError = ProductionFailure(TEXT("asset_data_request_evidence_invalid"), TEXT("The captured request evidence object is unavailable."), TEXT("evidence"), nullptr, MakeShared<FJsonValueString>(TEXT("object")));
				return false;
			}
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
			if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
			{
				OutError = ProductionFailure(TEXT("asset_data_request_evidence_invalid"), TEXT("The captured request evidence could not be serialized."), TEXT("evidence_serialized"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			return true;
		}

		bool TryParseRequestEvidence(
			const FUeAgentCandidateBinding& Binding,
			TSharedPtr<FJsonObject>& OutPayload,
			FUeAgentDiscoveryError& OutError)
		{
			const FString& Evidence = Binding.GetBindingEvidence();
			if (!Evidence.StartsWith(RequestEvidencePrefix))
			{
				OutError = ProductionFailure(TEXT("asset_data_request_evidence_invalid"),
					TEXT("The captured asset request evidence has an unsupported format."),
					TEXT("evidence_format_matches"), MakeShared<FJsonValueBoolean>(false),
					MakeShared<FJsonValueBoolean>(true));
				OutError.SubjectId = Binding.GetBindingId();
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Evidence.Left(FCString::Strlen(RequestEvidencePrefix)));
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(RequestEvidencePrefix);
				return false;
			}
			if (!Contract::Json::TryParseObject(
				Evidence.RightChop(FCString::Strlen(RequestEvidencePrefix)), OutPayload, OutError))
			{
				MarkNativeFailure(OutError);
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Binding.GetBindingId();
				return false;
			}
			return true;
		}

		bool TryReadEvidence(
			const TSharedPtr<FJsonObject>& Payload,
			FString& OutTargetRef, FString& OutPackageName, FString& OutObjectName,
			FString& OutObjectPath, FString& OutExpectedClass, FDiagnostic& OutError)
		{
			return TryReadEvidenceStrings(Payload, {
				{ TEXT("target_ref"), TEXT("target_ref"), &OutTargetRef },
				{ EvidencePackageName, TEXT("package_name"), &OutPackageName },
				{ EvidenceObjectName, TEXT("object_name"), &OutObjectName },
				{ EvidenceObjectPath, TEXT("object_path"), &OutObjectPath },
				{ EvidenceExpectedClass, TEXT("expected_class"), &OutExpectedClass }
			}, TEXT("asset_data_state_evidence_invalid"), OutError);
		}

		bool TryReadCloneEvidence(
			const TSharedPtr<FJsonObject>& Payload,
			FString& OutSourceRef, FString& OutSourceObjectPath,
			FString& OutSourceClass, FString& OutSourceRevision, FDiagnostic& OutError)
		{
			return TryReadEvidenceStrings(Payload, {
				{ EvidenceSourceRef, TEXT("source_ref"), &OutSourceRef },
				{ EvidenceSourceObjectPath, TEXT("source_object_path"), &OutSourceObjectPath },
				{ EvidenceSourceClass, TEXT("source_class"), &OutSourceClass },
				{ EvidenceSourceRevision, TEXT("source_revision"), &OutSourceRevision, true }
			}, TEXT("asset_data_clone_state_evidence_invalid"), OutError);
		}

		bool TryCanonicalValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError, const FString& Path = FString())
		{
			OutValue.Reset();
			const auto Result = [&](bool bSuccess)
			{
				if (!bSuccess)
				{
					MarkNativeFailure(OutError);
					if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Path;
				}
				return bSuccess;
			};
			if (!JsonValue.IsValid())
			{
				OutError = ProductionFailure(TEXT("asset_data_state_json_invalid"),
					TEXT("A generated asset state value is missing."), Path, nullptr,
					MakeShared<FJsonValueString>(TEXT("JSON value")));
				return false;
			}
			switch (JsonValue->Type)
			{
			case EJson::Null:
				return Result(FUeAgentCanonicalValue::TryNull(Resources, OutValue, OutError));
			case EJson::Boolean:
				return Result(FUeAgentCanonicalValue::TryBoolean(JsonValue->AsBool(), Resources, OutValue, OutError));
			case EJson::String:
				return Result(FUeAgentCanonicalValue::TryString(JsonValue->AsString(), Resources, OutValue, OutError));
			case EJson::Number:
				return Result(FUeAgentCanonicalValue::TryDecimal(FString::SanitizeFloat(JsonValue->AsNumber()), Resources, OutValue, OutError));
			case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					for (int32 Index = 0; Index < JsonValue->AsArray().Num(); ++Index)
					{
						FUeAgentCanonicalValuePtr Item;
						if (!TryCanonicalValue(JsonValue->AsArray()[Index], Resources, Item, OutError, Path + TEXT("/") + ::LexToString(Index)))
							return false;
						Items.Add(Item.ToSharedRef());
					}
					return Result(FUeAgentCanonicalValue::TryArray(Items, Resources, OutValue, OutError));
				}
			case EJson::Object:
				{
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
					for (const auto& Pair : JsonValue->AsObject()->Values)
					{
						FUeAgentCanonicalValuePtr Item;
						const FString Token = Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
						if (!TryCanonicalValue(Pair.Value, Resources, Item, OutError, Path + TEXT("/") + Token))
							return false;
						Items.Emplace(Pair.Key, Item.ToSharedRef());
					}
					return Result(FUeAgentCanonicalValue::TryObject(Items, Resources, OutValue, OutError));
				}
			default:
				OutError = ProductionFailure(TEXT("asset_data_state_json_kind_invalid"),
					TEXT("The generated asset state contains an unsupported JSON kind."), Path,
					MakeShared<FJsonValueNumber>(static_cast<int32>(JsonValue->Type)),
					MakeShared<FJsonValueString>(TEXT("null, boolean, string, number, array or object")));
				return false;
			}
		}

		bool TryCanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			return TryCanonicalValue(
				MakeShared<FJsonValueObject>(Object),
				Resources,
				OutValue,
				OutError);
		}

		const FUeAgentCanonicalValue* FindCanonicalField(
			const FUeAgentCanonicalValue& Object,
			const FString& FieldId)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<
				FString,
				FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == FieldId)
				{
					return &Item.Value.Get();
				}
			}
			return nullptr;
		}

		bool TryReadSelectedFieldIds(
			const TSharedPtr<FJsonObject>& Payload,
			const FAssetDataProfile& Profile,
			TArray<FString>& OutFieldIds, FDiagnostic& OutError)
		{
			OutFieldIds.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
			if (!Payload.IsValid() || !Payload->TryGetArrayField(TEXT("changes"), Changes) || !Changes)
			{
				const auto* Value = Payload.IsValid() ? Payload->Values.Find(TEXT("changes")) : nullptr;
				OutError = ProductionFailure(TEXT("asset_data_selected_fields_invalid"),
					TEXT("Asset changes must be an array."), TEXT("/changes"),
					Value ? *Value : nullptr, MakeShared<FJsonValueString>(TEXT("non-empty array")), TEXT("command_input"));
				return false;
			}
			TMap<FString, int32> Seen;
			for (int32 Index = 0; Index < Changes->Num(); ++Index)
			{
				const auto& Value = (*Changes)[Index];
				const FString Path = TEXT("/changes/") + ::LexToString(Index);
				const TSharedPtr<FJsonObject> Change =
					Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
				FString FieldId;
				if (!Change.IsValid())
				{
					OutError = ProductionFailure(TEXT("asset_data_selected_fields_invalid"),
						TEXT("Each asset change must be an object."), Path, Value,
						MakeShared<FJsonValueString>(TEXT("object")), TEXT("command_input"));
				}
				else if (!Change->TryGetStringField(TEXT("field_id"), FieldId))
				{
					const auto* Field = Change->Values.Find(TEXT("field_id"));
					OutError = ProductionFailure(TEXT("asset_data_selected_fields_invalid"),
						TEXT("Each asset change requires a string field_id."), Path + TEXT("/field_id"),
						Field ? *Field : nullptr, MakeShared<FJsonValueString>(TEXT("string")), TEXT("command_input"));
				}
				else if (!Profile.FieldIds.Contains(FieldId))
				{
					TArray<TSharedPtr<FJsonValue>> Allowed;
					for (const auto& Id : Profile.FieldIds) Allowed.Add(MakeShared<FJsonValueString>(Id));
					OutError = ProductionFailure(TEXT("asset_data_selected_fields_invalid"),
						TEXT("The field_id is not supported by this asset profile."), Path + TEXT("/field_id"),
						MakeShared<FJsonValueString>(FieldId), MakeShared<FJsonValueArray>(Allowed), TEXT("command_input"));
				}
				else if (const int32* FirstIndex = Seen.Find(FieldId))
				{
					OutError = ProductionFailure(TEXT("asset_data_selected_fields_invalid"),
						FString::Printf(TEXT("The field_id repeats the change at index %d."), *FirstIndex),
						Path + TEXT("/field_id"), MakeShared<FJsonValueString>(FieldId),
						MakeShared<FJsonValueString>(TEXT("unique field_id")), TEXT("command_input"));
				}
				else
				{
					Seen.Add(FieldId, Index);
					OutFieldIds.Add(MoveTemp(FieldId));
					continue;
				}
				OutFieldIds.Reset();
				return false;
			}
			if (OutFieldIds.IsEmpty())
			{
				OutError = ProductionFailure(TEXT("asset_data_selected_fields_invalid"),
					TEXT("At least one asset field change is required."), TEXT("/changes"),
					MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one change")), TEXT("command_input"));
				return false;
			}
			return true;
		}

		void CollectRequestedAssetRefs(
			const TSharedPtr<FJsonValue>& Value,
			TSet<FString>& OutRefs)
		{
			if (!Value.IsValid())
			{
				return;
			}
			if (Value->Type == EJson::String)
			{
				const FString Ref = Value->AsString();
				if (Ref.StartsWith(TEXT("uai://asset/")))
				{
					OutRefs.Add(Ref);
				}
				return;
			}
			if (Value->Type == EJson::Array)
			{
				for (const TSharedPtr<FJsonValue>& Item
					: Value->AsArray())
				{
					CollectRequestedAssetRefs(Item, OutRefs);
				}
				return;
			}
			if (Value->Type == EJson::Object)
			{
				for (const TPair<
					FString,
					TSharedPtr<FJsonValue>>& Item
					: Value->AsObject()->Values)
				{
					CollectRequestedAssetRefs(
						Item.Value,
						OutRefs);
				}
			}
		}

		bool TryBuildMutableAssetState(
			const FString& TargetRef,
			const FString& ObjectPath,
			const FString& ClassPath,
			const FAssetDataProfile& Profile,
			const FUeAgentCanonicalValueRef& FieldState,
			const FString& AssetRevision,
			const FUeAgentCanonicalValuePtr& FieldDefaults,
			const FUeAgentCanonicalValuePtr& ProjectionContext,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			OutValue.Reset();
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				Items;
			auto AddString =
				[&Items, &Resources, &OutError](
					const FString& Field,
					const FString& Value)
				{
					FUeAgentCanonicalValuePtr Canonical;
					if (!FUeAgentCanonicalValue::TryString(
							Value,
							Resources,
							Canonical,
							OutError))
					{
						return false;
					}
					Items.Emplace(
						Field,
						Canonical.ToSharedRef());
					return true;
				};
			if (!AddString(TEXT("target_ref"), TargetRef)
				|| !AddString(
					TEXT("profile_id"),
					Profile.ProfileId)
				|| !AddString(
					TEXT("asset_object_path"),
					ObjectPath)
				|| !AddString(
					TEXT("asset_class"),
					ClassPath))
			{
				return false;
			}
			Items.Emplace(TEXT("field_state"), FieldState);
			if (!AssetRevision.IsEmpty()
				&& !AddString(
					TEXT("asset_revision"),
					AssetRevision))
			{
				return false;
			}
			if (FieldDefaults.IsValid())
			{
				Items.Emplace(
					TEXT("field_defaults"),
					FieldDefaults.ToSharedRef());
			}
			if (ProjectionContext.IsValid())
			{
				Items.Emplace(
					TEXT("field_projection_context"),
					ProjectionContext.ToSharedRef());
			}
			return FUeAgentCanonicalValue::TryObject(
				Items,
				Resources,
				OutValue,
				OutError);
		}

		FUeAgentCanonicalType AssetDataStateType()
		{
			return FUeAgentCanonicalType(
				TEXT("asset_data_current_state"),
				EUeAgentValueKind::Object,
				false,
				TEXT("object"));
		}

		FUeAgentDefaultValue AssetDataStateDefault(
			const FUeAgentCanonicalValueRef& Value)
		{
			return FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::Schema,
				TEXT("asset-data:empty-default"),
				TEXT("AssetData state has no implicit asset"),
				Value);
		}

		FUeAgentStateDetails AssetDataStateDetails(
			const FAssetDataProfile& Profile)
		{
			return FUeAgentStateDetails(
				Profile.CapabilityId,
				TEXT(""),
				{
					TEXT("AssetData"),
					TEXT("SimpleAuthoring"),
					Profile.NativeOperationType,
				},
				true,
				TEXT("available"),
				{});
		}

		TSharedPtr<FJsonObject> BuildCreatedAssetState(
			const FString& TargetRef,
			const FString& ObjectPath,
			const FString& ClassPath,
			const FAssetDataProfile& Profile,
			const UObject* Object = nullptr,
			const bool bExpectedState = false)
		{
			const TSharedPtr<FJsonObject> State =
				MakeShared<FJsonObject>();
			State->SetStringField(TEXT("target_ref"), TargetRef);
			State->SetStringField(
				TEXT("profile_id"),
				Profile.ProfileId);
			State->SetStringField(
				TEXT("asset_object_path"),
				ObjectPath);
			State->SetStringField(
				TEXT("asset_class"),
				ClassPath);
			if (IsGenericFactoryProfile(Profile))
			{
				UClass* AssetClass = FindObject<UClass>(nullptr, *ClassPath);
				if (!AssetClass)
				{
					AssetClass = LoadObject<UClass>(nullptr, *ClassPath);
				}
				const bool bWorld = AssetClass
					&& AssetClass->IsChildOf(UWorld::StaticClass());
				State->SetBoolField(TEXT("is_world"), bWorld);
				if (bWorld)
				{
					const UWorld* World = Cast<UWorld>(Object);
					State->SetBoolField(
						TEXT("has_persistent_level"),
						bExpectedState
							|| (World && World->PersistentLevel));
				}
			}
			return State;
		}

		TSharedPtr<FJsonObject> BuildCloneSourceState(
			const FString& SourceRef,
			const FString& SourceClass,
			const FString& SourceRevision)
		{
			const TSharedPtr<FJsonObject> State =
				MakeShared<FJsonObject>();
			State->SetStringField(TEXT("target_ref"), SourceRef);
			State->SetStringField(
				TEXT("profile_id"),
				TEXT("clone_source"));
			State->SetStringField(
				TEXT("asset_class"),
				SourceClass);
			State->SetStringField(
				TEXT("asset_revision"),
				SourceRevision);
			State->SetStringField(
				TEXT("field_fingerprint"),
				SourceRevision);
			return State;
		}

		TSharedPtr<FJsonObject> BuildClonedAssetState(
			const FString& TargetRef,
			const FString& ObjectPath,
			const FString& ClassPath,
			const FString& SourceRef,
			const FString& SourceRevision,
			const bool bSourceUnchanged,
			const bool bDurableFlags,
			const FAssetDataProfile& Profile)
		{
			const TSharedPtr<FJsonObject> State =
				BuildCreatedAssetState(
					TargetRef,
					ObjectPath,
					ClassPath,
					Profile);
			State->SetStringField(
				TEXT("source_ref"),
				SourceRef);
			State->SetStringField(
				TEXT("source_revision"),
				SourceRevision);
			State->SetBoolField(
				TEXT("source_unchanged"),
				bSourceUnchanged);
			State->SetBoolField(
				TEXT("durable_asset_flags"),
				bDurableFlags);
			return State;
		}

		class FAssetDataTargetResolver final
			: public IUeAgentSimpleAuthoringTargetResolver
		{
		public:
			FAssetDataTargetResolver(
				TSharedRef<
					IUeAgentAssetDataSimpleEffectAssetResolver,
					ESPMode::ThreadSafe> InResolver,
				TSharedRef<
					IUeAgentAssetDataSimpleEffectFactoryCandidateResolver,
					ESPMode::ThreadSafe> InFactoryResolver)
				: Resolver(MoveTemp(InResolver))
				, FactoryResolver(MoveTemp(InFactoryResolver))
			{
			}

			bool TryResolveTarget(
				const FUeAgentOperationRequest& CanonicalRequest,
				const FUeAgentExecutionContextRef& Context,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				TOptional<FUeAgentCandidateBinding>& OutTarget,
				FUeAgentSimpleAuthoringProductionError&
					OutError) override
			{
				OutTarget.Reset();
				const FAssetDataProfile* Profile =
					FindProfile(
						CanonicalRequest.GetDescriptorIdentity());
				TSharedPtr<FJsonObject> Payload;
				FString TargetRef;
				if (!Profile || !IsSupportedProductionProfile(*Profile))
				{
					SetProductionError(OutError, ProductionFailure(TEXT("asset_data_production_profile_missing"), TEXT("No supported asset production profile is registered for this operation."), TEXT("profile_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), CanonicalRequest.GetDescriptorIdentity().GetDescriptorId());
					return false;
				}
				if (!Contract::Json::TryParseObject(CanonicalRequest.GetPayload().GetJsonValue(), Payload, OutError))
				{
					OutError.SourceKind = TEXT("command_input");
					return false;
				}
				if (!Payload->TryGetStringField(TEXT("target_ref"), TargetRef) || TargetRef.IsEmpty())
				{
					const auto* Value = Payload->Values.Find(TEXT("target_ref"));
					OutError = ProductionFailure(TEXT("asset_data_production_profile_missing"), TEXT("The asset operation requires a non-empty target_ref."), TEXT("/target_ref"), Value ? *Value : nullptr, MakeShared<FJsonValueString>(TEXT("non-empty string")), TEXT("command_input"));
					return false;
				}
				if (Cancellation.IsCancelled())
				{
					SetProductionError(OutError, ProductionFailure(TEXT("asset_data_target_resolution_cancelled"), TEXT("Asset target resolution was cancelled."), TEXT("cancelled"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)), TargetRef);
					OutError.TargetPath = TargetRef;
					return false;
				}
				UPackage* Package = nullptr;
				FName ObjectName;
				UObject* ExistingTarget = nullptr;
				FExecutionMutationError ResolveError;
				if (IsMutableProductionProfile(*Profile))
				{
					if (!Resolver->TryResolveExisting(
							TargetRef,
							ExistingTarget,
							ResolveError)
						|| !IsValid(ExistingTarget))
					{
						SetProductionError(OutError, ResolveError.Code.IsEmpty()
								? ProductionFailure(TEXT("asset_data_existing_target_unresolved"), TEXT("The asset resolver did not return a valid target object."), TEXT("target_ref"), MakeShared<FJsonValueString>(TargetRef), MakeShared<FJsonValueString>(TEXT("resolvable existing asset")))
								: ResolveError, TargetRef);
						return false;
					}
					FString TargetRevision;
					Contract::FDiagnostic RevisionError;
					if (!TryReadStableAssetRevision(ExistingTarget, TargetRevision, RevisionError))
					{
						SetProductionError(OutError, MoveTemp(RevisionError), TargetRef);
						return false;
					}
					Payload->SetStringField(
						EvidenceTargetRevision,
						TargetRevision);
					Package =
						ExistingTarget->GetOutermost();
					ObjectName =
						ExistingTarget->GetFName();
				}
				else if (!Resolver->TryResolveCreationTarget(
							TargetRef,
							Package,
							ObjectName,
							ResolveError)
						|| !Package
						|| ObjectName.IsNone())
				{
					SetProductionError(OutError, ResolveError.Code.IsEmpty()
							? ProductionFailure(TEXT("asset_data_creation_target_unresolved"), TEXT("The creation target did not resolve to a package and object name."), TEXT("target_ref"), MakeShared<FJsonValueString>(TargetRef), MakeShared<FJsonValueString>(TEXT("valid package and object name")))
							: ResolveError, TargetRef);
					return false;
				}
				if (!IsMutableProductionProfile(*Profile)
					&& StaticFindObjectFast(
						UObject::StaticClass(),
						Package,
						ObjectName))
				{
					SetProductionError(OutError, ProductionFailure(TEXT("asset_data_target_already_exists"), TEXT("An object already exists at the requested creation target."), TEXT("target_exists"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)), TargetRef);
					return false;
				}
				UObject* Source = nullptr;
				FString SourceRef;
				FString SourceRevision;
				if (Profile->IsClone())
				{
					if (!Payload->TryGetStringField(
							TEXT("source_ref"),
							SourceRef)
						|| SourceRef.IsEmpty())
					{
						const auto* Value = Payload->Values.Find(TEXT("source_ref"));
						SetProductionError(OutError, ProductionFailure(TEXT("asset_data_clone_source_missing"), TEXT("Cloning requires a non-empty source_ref."), TEXT("/source_ref"), Value ? *Value : nullptr, MakeShared<FJsonValueString>(TEXT("non-empty string")), TEXT("command_input")), TargetRef);
						return false;
					}
					if (!TryResolveCloneSource(
							Resolver, SourceRef, Source, ResolveError)
						|| !IsValid(Source))
					{
						SetProductionError(OutError, ResolveError.Code.IsEmpty()
								? ProductionFailure(TEXT("asset_data_clone_source_unresolved"), TEXT("The clone source did not resolve to a valid asset."), TEXT("source_ref"), MakeShared<FJsonValueString>(SourceRef), MakeShared<FJsonValueString>(TEXT("resolvable existing asset")))
								: ResolveError, SourceRef);
						return false;
					}
					SourceRef = Source->GetPathName();
					Payload->SetStringField(TEXT("source_ref"), SourceRef);
					Contract::FDiagnostic RevisionError;
					if (!TryReadStableAssetRevision(Source, SourceRevision, RevisionError))
					{
						SetProductionError(OutError, MoveTemp(RevisionError), SourceRef);
						return false;
					}
					Payload->SetStringField(
						EvidenceSourceRevision,
						SourceRevision);
				}
				UClass* ExpectedClass = nullptr;
				if (Profile->UsesStaticCreateClass())
				{
					ExpectedClass =
						LoadObject<UClass>(
							nullptr,
							*Profile->StaticCreateClassPath);
				}
				else if (Profile->UsesFactoryCandidate())
				{
					FString FactoryName;
					TMap<FString, FString> FactorySelect;
					UE::UAI::Contract::FDiagnostic SelectorError;
					if (!TryParseAssetDataSimpleEffectFactorySelector(
							*Payload,
							IsGenericFactoryProfile(*Profile),
							FactoryName,
							FactorySelect,
							SelectorError))
					{
						SetProductionError(OutError, SelectorError, Profile->ProfileId);
						return false;
					}
					FUeAgentAssetDataSimpleEffectFactoryBinding
						FactoryBinding;
					if (!FactoryResolver
							->TryResolveForPlanning(
								FactoryName,
								FactorySelect,
								Profile->ProfileId,
								FactoryBinding,
								ResolveError))
					{
						SetProductionError(OutError, ResolveError.Code.IsEmpty()
								? ProductionFailure(TEXT("asset_data_factory_candidate_unresolved"), TEXT("No factory candidate resolved for the requested selector."), TEXT("factory_name"), MakeShared<FJsonValueString>(FactoryName), MakeShared<FJsonValueString>(TEXT("matching factory candidate")))
								: ResolveError, Profile->ProfileId);
						return false;
					}
					UE::UAI::Contract::FDiagnostic FactoryError;
					const FUeAgentCandidateBinding*
						Candidate =
						FactoryBinding.GetCandidateBinding();
					const FUeAgentCatalogRevision Revision = Candidate
						? Candidate->GetCatalogRevision()
						: FUeAgentCatalogRevision(TEXT("invalid"), TEXT("invalid"), -1);
					if (!Candidate
						|| !TryValidateAssetDataSimpleEffectFactoryBinding(
							Profile->NativeBindingId,
							Candidate->GetBindingId(),
							Revision,
							FactoryBinding,
							FactoryError))
					{
						SetProductionError(OutError, FactoryError.IsEmpty() ? ProductionFailure(TEXT("asset_data_factory_binding_invalid"), TEXT("The resolved factory does not provide its candidate binding."), TEXT("factory_binding_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)) : FactoryError, Profile->ProfileId);
						return false;
					}
					ExpectedClass =
						FactoryBinding.GetConcreteClass();
				}
				else if (Profile->IsClone())
				{
					ExpectedClass = Source->GetClass();
				}
				else if (IsMutableProductionProfile(*Profile))
				{
					ExpectedClass = ExistingTarget->GetClass();
				}
				if (!ExpectedClass
					|| ExpectedClass->HasAnyClassFlags(
						CLASS_Abstract))
				{
					SetProductionError(OutError, ProductionFailure(TEXT("asset_data_target_class_invalid"), ExpectedClass ? TEXT("The resolved target class is abstract.") : TEXT("The target class could not be resolved."), TEXT("target_class"), MakeShared<FJsonValueString>(ExpectedClass ? ExpectedClass->GetPathName() : Profile->StaticCreateClassPath), MakeShared<FJsonValueString>(TEXT("available concrete class"))), Profile->ProfileId);
					return false;
				}
				const FString PackageName = Package->GetName();
				const FString ObjectPath =
					ExistingTarget
						? ExistingTarget->GetPathName()
						: PackageName
							+ TEXT(".")
							+ ObjectName.ToString();
				TargetRef = ObjectPath;
				Payload->SetStringField(TEXT("target_ref"), TargetRef);
				Payload->SetStringField(
					EvidencePackageName,
					PackageName);
				Payload->SetStringField(
					EvidenceObjectName,
					ObjectName.ToString());
				Payload->SetStringField(
					EvidenceObjectPath,
					ObjectPath);
				Payload->SetStringField(
					EvidenceExpectedClass,
					ExpectedClass->GetPathName());
				if (Profile->IsClone())
				{
					Payload->SetStringField(
						EvidenceSourceRef,
						SourceRef);
					Payload->SetStringField(
						EvidenceSourceObjectPath,
						Source->GetPathName());
					Payload->SetStringField(
						EvidenceSourceClass,
						Source->GetClass()->GetPathName());
					Payload->SetStringField(
						EvidenceSourceRevision,
						SourceRevision);
				}
				FString EvidenceJson;
				if (!TrySerializeJson(Payload, EvidenceJson, OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					return false;
				}
				const FUeAgentCanonicalIdentity Identity(
					TEXT("uai.asset_data:") + TargetRef,
					Profile->NativeOperationType,
					Context.GetProjectInstanceId(),
					TargetRef,
					TEXT("asset_data"),
					Context.GetContextRevision());
				OutTarget.Emplace(
					TEXT("asset-data:")
						+ CanonicalRequest.GetRequestId(),
					FUeAgentCandidateSummary(
						Identity,
						CanonicalRequest.GetDescriptorIdentity(),
						TEXT("asset_data"),
						TargetRef,
						TEXT(
							"exact AssetData operation target"),
						TArray<FString>{
							TEXT("AssetData"),
							TEXT("SimpleAuthoring"),
						}),
					FUeAgentCatalogRevision(
						TEXT("asset-data-exact-target"),
						Context.GetContextRevision(),
						ExistingTarget
							? ExistingTarget->GetUniqueID()
							: Package->GetUniqueID()),
					PackageName,
					PersistedStatePortId,
					TArray<EUeAgentCandidateOperation>{
						IsMutableProductionProfile(*Profile)
							? EUeAgentCandidateOperation::Update
							: EUeAgentCandidateOperation::Create,
					},
					FString(RequestEvidencePrefix)
						+ EvidenceJson);
				if (!OutTarget->IsValid())
				{
					SetProductionError(OutError, *OutTarget->GetValidationDiagnostic(), TargetRef);
					MarkNativeFailure(OutError);
					OutTarget.Reset();
					return false;
				}
				return true;
			}

		private:
			TSharedRef<
				IUeAgentAssetDataSimpleEffectAssetResolver,
				ESPMode::ThreadSafe> Resolver;
			TSharedRef<
				IUeAgentAssetDataSimpleEffectFactoryCandidateResolver,
				ESPMode::ThreadSafe> FactoryResolver;
		};

		class FAssetDataStateReadPort final
			: public IUeAgentStateReadPort
		{
		public:
			FAssetDataStateReadPort(
				FString InPortId,
				const EUeAgentStatePlane InRequiredPlane,
				TSharedRef<
					IUeAgentAssetDataSimpleEffectAssetResolver,
					ESPMode::ThreadSafe> InResolver)
				: PortId(MoveTemp(InPortId))
				, RequiredPlane(InRequiredPlane)
				, Resolver(MoveTemp(InResolver))
			{
			}

			const FString& GetPortId() const override
			{
				return PortId;
			}

			bool TryRead(
				const FUeAgentStateReadRequest& Request,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				TSharedPtr<const FUeAgentCanonicalSnapshot>&
					OutSnapshot,
				FUeAgentDiscoveryError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = FUeAgentDiscoveryError();
				const FAssetDataProfile* Profile =
					Request.IsValid()
						? FindProfile(
							Request.GetBinding()
								.GetSummary()
								.GetDescriptorIdentity())
						: nullptr;
				if (!Profile
					|| !IsSupportedProductionProfile(*Profile)
					|| Request.GetStatePlane() != RequiredPlane)
				{
					if (!Request.IsValid()) OutError = *Request.GetBinding().GetValidationDiagnostic();
					else if (!Profile || !IsSupportedProductionProfile(*Profile))
						OutError = ProductionFailure(TEXT("asset_data_state_request_invalid"), TEXT("No supported asset profile is registered for the state request."), TEXT("profile_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					else
						OutError = ProductionFailure(TEXT("asset_data_state_request_invalid"), TEXT("The requested state plane does not match this read port."), TEXT("state_plane"), MakeShared<FJsonValueString>(Contract::LexToString(Request.GetStatePlane())), MakeShared<FJsonValueString>(Contract::LexToString(RequiredPlane)));
					MarkNativeFailure(OutError);
					return false;
				}
				if (Cancellation.IsCancelled())
				{
					OutError = ProductionFailure(TEXT("asset_data_state_request_cancelled"), TEXT("The asset state read was cancelled."), TEXT("cancelled"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					return false;
				}

				const FUeAgentCandidateBinding& Binding =
					Request.GetBinding();
				TSharedPtr<FJsonObject> Payload;
				FString TargetRef;
				FString PackageName;
				FString ObjectNameString;
				FString ExpectedObjectPath;
				FString ExpectedClass;
				FString SourceRef;
				FString ExpectedSourceObjectPath;
				FString ExpectedSourceClass;
				FString ExpectedSourceRevision;
				TArray<FString> SelectedFieldIds;
				if (!TryParseRequestEvidence(
						Binding,
						Payload,
						OutError)
					|| !TryReadEvidence(
						Payload,
						TargetRef,
						PackageName,
						ObjectNameString,
						ExpectedObjectPath,
						ExpectedClass, OutError))
				{
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Binding.GetBindingId();
					return false;
				}
				if (IsMutableProductionProfile(*Profile)
					&& !TryReadSelectedFieldIds(
						Payload,
						*Profile,
						SelectedFieldIds, OutError))
				{
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Binding.GetBindingId();
					return false;
				}
				if (Profile->IsClone()
					&& !TryReadCloneEvidence(
						Payload,
						SourceRef,
						ExpectedSourceObjectPath,
						ExpectedSourceClass,
						ExpectedSourceRevision, OutError))
				{
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Binding.GetBindingId();
					return false;
				}
				if (Profile->IsClone()
					&& (ExpectedClass != ExpectedSourceClass
						|| ExpectedSourceObjectPath.IsEmpty()))
				{
					OutError = ExpectedClass != ExpectedSourceClass
						? ProductionFailure(TEXT("asset_data_clone_expected_class_mismatch"), TEXT("The clone source class differs from the expected target class."), TEXT("source_class"), MakeShared<FJsonValueString>(ExpectedSourceClass), MakeShared<FJsonValueString>(ExpectedClass))
						: ProductionFailure(TEXT("asset_data_clone_expected_class_mismatch"), TEXT("The captured clone source path is empty."), TEXT("source_object_path"), MakeShared<FJsonValueString>(ExpectedSourceObjectPath), MakeShared<FJsonValueString>(TEXT("non-empty asset path")));
					OutError.TargetPath = SourceRef;
					return false;
				}

				UObject* Object = nullptr;
				UObject* SourceObject = nullptr;
				FString CurrentSourceRevision;
				UPackage* Package = nullptr;
				FName ObjectName;
				FExecutionMutationError ResolveError;
				if (IsMutableProductionProfile(*Profile))
				{
					if (!Resolver->TryResolveExisting(
							TargetRef,
							Object,
							ResolveError)
						|| !IsValid(Object)
						|| Object->GetPathName()
							!= ExpectedObjectPath
						|| Object->GetClass()->GetPathName()
							!= ExpectedClass
						|| Object->GetOutermost()->GetName()
							!= PackageName
						|| Object->GetName()
							!= ObjectNameString)
					{
						OutError = ResolveError.IsEmpty()
							? ObjectEvidenceMismatch(TEXT("asset_data_existing_target_mismatch"), Object,
								ExpectedObjectPath, ExpectedClass, PackageName, ObjectNameString)
							: ResolveError;
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
						return false;
					}
					if (RequiredPlane
							== EUeAgentStatePlane::AuthoringMemory)
					{
						FString ExpectedRevision;
						if (!Payload->TryGetStringField(
								EvidenceTargetRevision,
								ExpectedRevision))
						{
							OutError = RevisionMismatch(TEXT("asset_data_target_revision_mismatch"),
								TEXT("The captured target state does not provide its expected revision."), TargetRef, FString(), FString());
							OutError.PropertyPath = TEXT("expected_revision_available");
							return false;
						}
						FString ActualRevision;
						if (!TryReadStableAssetRevision(Object, ActualRevision, OutError)) return false;
						if (ActualRevision != ExpectedRevision)
						{
							OutError = RevisionMismatch(TEXT("asset_data_target_revision_mismatch"),
								TEXT("The target asset revision changed after its state was captured."), TargetRef, ActualRevision, ExpectedRevision);
							return false;
						}
					}
				}
				else if (RequiredPlane
					== EUeAgentStatePlane::AuthoringMemory)
				{
					if (!Resolver->TryResolveCreationTarget(
							TargetRef,
							Package,
							ObjectName,
							ResolveError)
						|| !Package
						|| Package->GetName() != PackageName
						|| ObjectName.ToString()
							!= ObjectNameString)
					{
						OutError = ResolveError;
						if (OutError.IsEmpty())
						{
							if (!Package || Package->GetName() != PackageName)
								OutError = ProductionFailure(TEXT("asset_data_prestate_target_unresolved"), TEXT("The resolved creation package differs from the captured target."), TEXT("package_name"), Package ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Package->GetName())) : nullptr, MakeShared<FJsonValueString>(PackageName));
							else if (ObjectName.ToString() != ObjectNameString)
								OutError = ProductionFailure(TEXT("asset_data_prestate_target_unresolved"), TEXT("The resolved creation object name differs from the captured target."), TEXT("object_name"), MakeShared<FJsonValueString>(ObjectName.ToString()), MakeShared<FJsonValueString>(ObjectNameString));
							else
								OutError = ProductionFailure(TEXT("asset_data_prestate_target_unresolved"), TEXT("The creation target resolver reported failure without a diagnostic."), TEXT("resolution_succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						}
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
						return false;
					}
					Object = StaticFindObjectFast(
						UObject::StaticClass(),
						Package,
						ObjectName);
					if (Object)
					{
						OutError = ProductionFailure(TEXT("asset_data_target_already_exists"), TEXT("An object already exists at the creation target."), TEXT("target_exists"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
						OutError.TargetPath = TargetRef;
						return false;
					}
				}
				else if (!Resolver->TryResolveExisting(
						TargetRef,
						Object,
						ResolveError)
					|| !IsValid(Object))
				{
					OutError = ResolveError.IsEmpty()
						? ProductionFailure(TEXT("asset_data_created_target_missing"), TEXT("The created asset could not be resolved."), TEXT("target_ref"), MakeShared<FJsonValueString>(TargetRef), MakeShared<FJsonValueString>(TEXT("resolvable existing asset"))) : ResolveError;
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					return false;
				}
				if (Profile->IsClone())
				{
					if (!TryResolveCloneSource(
							Resolver, SourceRef, SourceObject, ResolveError)
						|| !IsValid(SourceObject))
					{
						OutError = ResolveError.IsEmpty()
							? ProductionFailure(TEXT("asset_data_clone_source_unresolved"), TEXT("The clone source could not be resolved."), TEXT("source_ref"), MakeShared<FJsonValueString>(SourceRef), MakeShared<FJsonValueString>(TEXT("resolvable existing asset"))) : ResolveError;
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = SourceRef;
						return false;
					}
					if (!TryReadStableAssetRevision(SourceObject, CurrentSourceRevision, OutError)) return false;
					if (SourceObject->GetPathName()
							!= ExpectedSourceObjectPath
						|| SourceObject->GetClass()
								->GetPathName()
								!= ExpectedSourceClass)
					{
						const bool bPathMismatch = SourceObject->GetPathName() != ExpectedSourceObjectPath;
						OutError = ProductionFailure(TEXT("asset_data_clone_source_mismatch"),
							bPathMismatch ? TEXT("The clone source path differs from its captured evidence.") : TEXT("The clone source class differs from its captured evidence."),
							bPathMismatch ? TEXT("source_object_path") : TEXT("source_class"),
							MakeShared<FJsonValueString>(bPathMismatch ? SourceObject->GetPathName() : SourceObject->GetClass()->GetPathName()),
							MakeShared<FJsonValueString>(bPathMismatch ? ExpectedSourceObjectPath : ExpectedSourceClass));
						OutError.TargetPath = SourceRef;
						return false;
					}
					if (RequiredPlane
							== EUeAgentStatePlane::AuthoringMemory
						&& CurrentSourceRevision
							!= ExpectedSourceRevision)
					{
						OutError = RevisionMismatch(TEXT("asset_data_source_revision_mismatch"),
							TEXT("The clone source revision changed after its state was captured."), SourceRef, CurrentSourceRevision, ExpectedSourceRevision);
						return false;
					}
				}

				TArray<FUeAgentStateEntry> Entries;
				int64 Sequence =
					Binding.GetCatalogRevision().GetSequence();
				if (Profile->IsClone()
					&& RequiredPlane
						== EUeAgentStatePlane::AuthoringMemory)
				{
					FUeAgentCanonicalValuePtr SourceValue;
					FUeAgentCanonicalValuePtr DefaultValue;
					FUeAgentStateSemanticError StateError;
					if (!TryCanonicalObject(
							BuildCloneSourceState(
								SourceRef,
								ExpectedSourceClass,
								CurrentSourceRevision),
							Resources,
							SourceValue,
							StateError)
						|| !FUeAgentCanonicalValue::TryObject(
							{},
							Resources,
							DefaultValue,
							StateError))
					{
						OutError = StateError;
						OutError.SubjectId = SourceRef;
						return false;
					}
					Sequence = SourceObject->GetUniqueID();
					const FUeAgentStateRevision SourceStateRevision(
						PortId,
						Binding.GetCatalogRevision()
							.GetGeneration(),
						Sequence);
					Entries.Emplace(
						FUeAgentCanonicalIdentity(
							TEXT("uai.asset_data:")
								+ SourceRef,
							TEXT("asset.object.clone"),
							SourceObject->GetOutermost()
								->GetName(),
							SourceRef,
							TEXT("asset_data"),
							Binding.GetSummary()
								.GetIdentity()
								.GetGeneration()),
						AssetDataStateType(),
						SourceValue.ToSharedRef(),
						AssetDataStateDefault(
							DefaultValue.ToSharedRef()),
						AssetDataStateDetails(*Profile),
						SourceStateRevision,
						EUeAgentAuthoredPresence::Present,
						EUeAgentOverrideState::Overridden);
				}
				if (Object)
				{
					const FString ActualObjectPath =
						Object->GetPathName();
					const FString ActualClass =
						Object->GetClass()->GetPathName();
					if (ActualObjectPath != ExpectedObjectPath
						|| ActualClass != ExpectedClass
						|| Object->GetOutermost()->GetName()
							!= PackageName
						|| Object->GetName()
							!= ObjectNameString)
					{
						OutError = ObjectEvidenceMismatch(TEXT("asset_data_created_target_mismatch"), Object,
							ExpectedObjectPath, ExpectedClass, PackageName, ObjectNameString);
						OutError.TargetPath = TargetRef;
						return false;
					}
					FUeAgentCanonicalValuePtr Value;
					FUeAgentStateSemanticError StateError;
					if (IsMutableProductionProfile(*Profile))
					{
						FUeAgentCanonicalValuePtr CurrentFields;
						FUeAgentCanonicalValuePtr DefaultFields;
						FUeAgentCanonicalValuePtr
							ProjectionContext;
						FExecutionMutationError ProjectionError;
						if (!TryProjectAssetDataSimpleEffectCurrentFieldState(
								*Resolver, Profile->NativeMechanicId, Object, SelectedFieldIds,
								Resources, CurrentFields, DefaultFields, ProjectionContext, ProjectionError))
						{
							OutError = MoveTemp(ProjectionError);
							if (OutError.IsEmpty())
							{
								OutError = Contract::FDiagnostic::Failure(TEXT("asset_data_field_projection_failed"),
									TEXT("The current asset fields could not be projected and no diagnostic was provided."), TEXT("state_capture"));
								OutError.SourceKind = TEXT("native_result");
							}
							if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
							return false;
						}
						FString StateRevision;
						if (RequiredPlane == EUeAgentStatePlane::AuthoringMemory
							&& !TryReadStableAssetRevision(Object, StateRevision, OutError)) return false;
						if (!TryBuildMutableAssetState(
								TargetRef, ActualObjectPath, ActualClass, *Profile, CurrentFields.ToSharedRef(),
								StateRevision,
								RequiredPlane == EUeAgentStatePlane::AuthoringMemory ? DefaultFields : FUeAgentCanonicalValuePtr(),
								RequiredPlane == EUeAgentStatePlane::AuthoringMemory ? ProjectionContext : FUeAgentCanonicalValuePtr(),
								Resources, Value, StateError))
						{
							OutError = MoveTemp(StateError);
							if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TargetRef;
							if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
							return false;
						}
					}
					else
					{
						TSharedPtr<FJsonObject> StateObject;
						if (Profile->IsClone())
						{
							const bool bSourceUnchanged =
								CurrentSourceRevision
									== ExpectedSourceRevision;
							StateObject = BuildClonedAssetState(
								TargetRef,
								ActualObjectPath,
								ActualClass,
								SourceRef,
								CurrentSourceRevision,
								bSourceUnchanged,
								Object->HasAllFlags(
									RF_Public
										| RF_Standalone
										| RF_Transactional),
								*Profile);
						}
						else
						{
							StateObject = BuildCreatedAssetState(
								TargetRef,
								ActualObjectPath,
								ActualClass,
								*Profile,
								Object);
						}
						if (!TryCanonicalObject(
								StateObject,
								Resources,
								Value,
								StateError))
						{
						OutError = StateError;
							OutError.SubjectId = TargetRef;
							return false;
						}
					}
					FUeAgentCanonicalValuePtr DefaultValue;
					if (!FUeAgentCanonicalValue::TryObject(
							{},
							Resources,
							DefaultValue,
							StateError))
					{
						OutError = StateError;
						OutError.SubjectId = TargetRef;
						return false;
					}
					Sequence = Object->GetUniqueID();
					const FUeAgentStateRevision Revision(
						PortId,
						Binding.GetCatalogRevision()
							.GetGeneration(),
						Sequence);
					Entries.Emplace(
						Binding.GetSummary().GetIdentity(),
						AssetDataStateType(),
						Value.ToSharedRef(),
						AssetDataStateDefault(
							DefaultValue.ToSharedRef()),
						AssetDataStateDetails(*Profile),
						Revision,
						EUeAgentAuthoredPresence::Present,
						EUeAgentOverrideState::Overridden);
				}
				if (IsMutableProductionProfile(*Profile)
					&& RequiredPlane
						== EUeAgentStatePlane::AuthoringMemory)
				{
					TSet<FString> RequestedRefs;
					CollectRequestedAssetRefs(
						MakeShared<FJsonValueObject>(Payload),
						RequestedRefs);
					RequestedRefs.Remove(TargetRef);
					TArray<FString> OrderedRefs =
						RequestedRefs.Array();
					OrderedRefs.Sort();
					for (const FString& RequestedRef
						: OrderedRefs)
					{
						UObject* Referenced = nullptr;
						if (!Resolver->TryResolveExisting(
								RequestedRef,
								Referenced,
								ResolveError)
							|| !IsValid(Referenced))
						{
							OutError = ResolveError.IsEmpty()
								? ProductionFailure(TEXT("asset_data_reference_unresolved"), TEXT("A referenced asset could not be resolved."), TEXT("reference"), MakeShared<FJsonValueString>(RequestedRef), MakeShared<FJsonValueString>(TEXT("resolvable existing asset"))) : ResolveError;
							if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = RequestedRef;
							return false;
						}
						FUeAgentCanonicalValuePtr ReferenceValue;
						FUeAgentCanonicalValuePtr
							ReferenceDefault;
						FUeAgentStateSemanticError
							ReferenceStateError;
						FString ReferenceRevisionValue;
						if (!TryReadStableAssetRevision(Referenced, ReferenceRevisionValue, OutError)) return false;
						if (!TryCanonicalObject(
								BuildCloneSourceState(
									RequestedRef,
									Referenced->GetClass()
										->GetPathName(),
								ReferenceRevisionValue),
								Resources,
								ReferenceValue,
								ReferenceStateError)
							|| !FUeAgentCanonicalValue::TryObject(
								{},
								Resources,
								ReferenceDefault,
								ReferenceStateError))
						{
							OutError = ReferenceStateError;
							MarkNativeFailure(OutError);
							if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = RequestedRef;
							return false;
						}
						const FUeAgentStateRevision
							ReferenceRevision(
								PortId,
								Binding.GetCatalogRevision()
									.GetGeneration(),
								Sequence);
						Entries.Emplace(
							FUeAgentCanonicalIdentity(
								TEXT("uai.asset_data.reference:")
									+ RequestedRef,
								TEXT("asset.reference"),
								Referenced->GetOutermost()
									->GetName(),
								RequestedRef,
								TEXT("asset_data"),
								Binding.GetSummary()
									.GetIdentity()
									.GetGeneration()),
							AssetDataStateType(),
							ReferenceValue.ToSharedRef(),
							AssetDataStateDefault(
								ReferenceDefault.ToSharedRef()),
							AssetDataStateDetails(*Profile),
							ReferenceRevision,
							EUeAgentAuthoredPresence::Present,
							EUeAgentOverrideState::Overridden);
					}
				}

				const FUeAgentStateRevision Revision(
					PortId,
					Binding.GetCatalogRevision().GetGeneration(),
					Sequence);
				FUeAgentCanonicalSnapshotPtr Snapshot;
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalSnapshot::TryCreate(
						TEXT("snapshot:asset-data:")
							+ PortId
							+ TEXT(":")
							+ Binding.GetBindingId(),
						RequiredPlane,
						Revision,
						FUeAgentTemporalPoint(
							TEXT("asset-data-operation"),
							TEXT("1"),
							TEXT("1"),
							TEXT("1"),
							TEXT("observed")),
						FUeAgentStateProvenance(
							TEXT("independent_readback"),
							PortId,
							TargetRef),
						MoveTemp(Entries),
						Resources,
						Snapshot,
						StateError))
				{
					OutError = StateError;
					OutError.SubjectId = TargetRef;
					return false;
				}
				OutSnapshot = MoveTemp(Snapshot);
				return OutSnapshot.IsValid();
			}

		private:
			FString PortId;
			EUeAgentStatePlane RequiredPlane =
				EUeAgentStatePlane::AuthoringMemory;
			TSharedRef<
				IUeAgentAssetDataSimpleEffectAssetResolver,
				ESPMode::ThreadSafe> Resolver;
		};

		class FAssetDataPersistedStateOracle final
			: public IPersistedStateOracle
		{
		public:
			explicit FAssetDataPersistedStateOracle(
				TSharedRef<FAssetDataStateReadPort, ESPMode::ThreadSafe>
					InFallbackReadPort)
				: FallbackReadPort(MoveTemp(InFallbackReadPort))
				, FallbackOracle(FallbackReadPort)
			{
			}

			const FString& GetReadPortId() const override
			{
				return FallbackReadPort->GetPortId();
			}

			bool TryObserve(
				const FVerificationTarget& Target,
				const TArray<FDurableArtifact>& Artifacts,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				FUeAgentCanonicalSnapshotPtr& OutSnapshot,
				FVerificationError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = FVerificationError();
				const FUeAgentCandidateBinding& Binding =
					Target.GetBinding();
				const FAssetDataProfile* Profile =
					Target.IsValid()
						? FindProfile(
							Binding.GetSummary()
								.GetDescriptorIdentity())
						: nullptr;
				if (!Profile || !IsGenericFactoryProfile(*Profile))
				{
					return FallbackOracle.TryObserve(
						Target,
						Artifacts,
						Resources,
						Deadline,
						Cancellation,
						OutSnapshot,
						OutError);
				}
				if (Artifacts.IsEmpty()
					|| Cancellation.IsCancelled())
				{
					OutError = Artifacts.IsEmpty()
						? ProductionFailure(TEXT("asset_data_persisted_snapshot_request_invalid"), TEXT("Persisted verification requires at least one durable artifact."), TEXT("artifact_count"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one artifact")))
						: ProductionFailure(TEXT("asset_data_persisted_snapshot_request_invalid"), TEXT("Persisted asset verification was cancelled."), TEXT("cancelled"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					return false;
				}

				TSharedPtr<FJsonObject> Payload;
				FString TargetRef;
				FString PackageName;
				FString ObjectName;
				FString ExpectedObjectPath;
				FString ExpectedClass;
				FUeAgentDiscoveryError EvidenceError;
				if (!TryParseRequestEvidence(
						Binding,
						Payload,
						EvidenceError)
					|| !TryReadEvidence(
						Payload,
						TargetRef,
						PackageName,
						ObjectName,
						ExpectedObjectPath,
						ExpectedClass, EvidenceError))
				{
					OutError = EvidenceError;
					return false;
				}

				FUeAgentPersistedCreatedAssetObservation Observation;
				auto BuildSnapshot = [&Resources,
					&Binding,
					Profile,
					&TargetRef,
					&ExpectedObjectPath,
					&ExpectedClass,
					&Observation,
					this](
					const bool bObservedPersistentLevel,
					FUeAgentCanonicalSnapshotPtr& Result,
					UE::UAI::Contract::FDiagnostic& BuildError)
				{
					FUeAgentCanonicalValuePtr Value;
					FUeAgentCanonicalValuePtr DefaultValue;
					FUeAgentStateSemanticError StateError;
					if (!TryCanonicalObject(
							BuildCreatedAssetState(
								TargetRef,
								ExpectedObjectPath,
								ExpectedClass,
								*Profile,
								nullptr,
								bObservedPersistentLevel),
							Resources,
							Value,
							StateError)
						|| !FUeAgentCanonicalValue::TryObject(
							{},
							Resources,
							DefaultValue,
							StateError))
					{
						BuildError = StateError;
						return false;
					}
					const FUeAgentStateRevision Revision(
						GetReadPortId(),
						Binding.GetCatalogRevision().GetGeneration(),
						Observation.SnapshotSequence);
					TArray<FUeAgentStateEntry> Entries;
					Entries.Emplace(
						Binding.GetSummary().GetIdentity(),
						AssetDataStateType(),
						Value.ToSharedRef(),
						AssetDataStateDefault(DefaultValue.ToSharedRef()),
						AssetDataStateDetails(*Profile),
						Revision,
						EUeAgentAuthoredPresence::Present,
						EUeAgentOverrideState::Overridden);
					if (!FUeAgentCanonicalSnapshot::TryCreate(
							TEXT("snapshot:asset-data:persisted-isolated:")
								+ Binding.GetBindingId(),
							EUeAgentStatePlane::Persisted,
							Revision,
							FUeAgentTemporalPoint(
								TEXT("asset-data-operation"),
								TEXT("1"),
								TEXT("1"),
								TEXT("1"),
								TEXT("observed")),
							FUeAgentStateProvenance(
								TEXT("independent_readback"),
								GetReadPortId(),
								TargetRef),
							MoveTemp(Entries),
							Resources,
							Result,
							StateError))
					{
						BuildError = StateError;
						return false;
					}
					if (!Result.IsValid())
					{
						BuildError = ProductionFailure(TEXT("asset_data_persisted_snapshot_missing"), TEXT("The snapshot builder returned no persisted asset snapshot."), TEXT("snapshot_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						BuildError.TargetPath = TargetRef;
						return false;
					}
					return true;
				};

				FUeAgentAssetDataSimpleAuthoringError ObservationError;
				if (!TryObservePersistedCreatedAsset(
						Artifacts,
						PackageName,
						ObjectName,
						ExpectedObjectPath,
						ExpectedClass,
						Observation,
						ObservationError))
				{
					OutError = ObservationError;
					return false;
				}
				UE::UAI::Contract::FDiagnostic BuildError;
				if (!BuildSnapshot(
						Observation.bHasPersistentLevel,
						OutSnapshot,
						BuildError))
				{
					OutError = BuildError;
					return false;
				}
				return OutSnapshot.IsValid();
			}

		private:
			TSharedRef<FAssetDataStateReadPort, ESPMode::ThreadSafe>
				FallbackReadPort;
			FStateReadPortPersistedStateOracle FallbackOracle;
		};

		class FAssetDataDraftBuilder final
			: public IUeAgentSimpleAuthoringDraftBuilder
		{
		public:
			explicit FAssetDataDraftBuilder(
				TSharedRef<
					IUeAgentAssetDataSimpleEffectFactoryCandidateResolver,
					ESPMode::ThreadSafe> InFactoryResolver)
				: FactoryResolver(MoveTemp(InFactoryResolver))
			{
			}

			bool TryBuildDraft(
				const FUeAgentOperationRequest& CanonicalRequest,
				const FUeAgentExecutionContextRef& Context,
				const FUeAgentCanonicalSnapshotRef& PreState,
				FUeAgentResourceTracker& Resources,
				TOptional<FChangeSetDraft>& OutDraft,
				FUeAgentSimpleAuthoringProductionError&
					OutError) override
			{
				FExecutionMutationError Error;
				if (!TryBuildAssetDataSimpleEffectChangeSetDraft(
						CanonicalRequest,
						Context,
						PreState,
						*FactoryResolver,
						Resources,
						OutDraft,
						Error))
				{
					SetProductionError(OutError, Error, CanonicalRequest.GetRequestId());
					return false;
				}
				return true;
			}

		private:
			TSharedRef<
				IUeAgentAssetDataSimpleEffectFactoryCandidateResolver,
				ESPMode::ThreadSafe> FactoryResolver;
		};

		class FAssetDataChangeTargetProjector final
			: public IUeAgentSimpleAuthoringChangeTargetProjector
		{
		public:
			bool TryProjectChangeTargets(
				const FUeAgentOperationRequest& CanonicalRequest,
				const FUeAgentExecutionContextRef& Context,
				const FChangeSetDraft& Draft,
				const FUeAgentCandidateBinding& ResolvedTarget,
				const FUeAgentCanonicalSnapshotRef& PreState,
				FUeAgentResourceTracker& Resources,
				TArray<FUeAgentCandidateBinding>& OutTargets,
				FUeAgentSimpleAuthoringProductionError&
					OutError) override
			{
				OutTargets.Reset();
				const FAssetDataProfile* Profile =
					FindProfile(
						CanonicalRequest.GetDescriptorIdentity());
				if (!Profile
					|| !IsSupportedProductionProfile(*Profile))
				{
					SetProductionError(OutError, ProductionFailure(TEXT("asset_data_change_profile_missing"), TEXT("No supported asset profile is available for projecting change targets."), TEXT("profile_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), CanonicalRequest.GetDescriptorIdentity()
							.GetDescriptorId());
					return false;
				}
				const TArray<FChangeDraft>& Changes =
					Draft.GetChanges();
				for (int32 Index = 0;
					Index < Changes.Num();
					++Index)
				{
					const FChangeDraft& Change = Changes[Index];
					OutTargets.Emplace(
						ResolvedTarget.GetBindingId()
							+ TEXT(":change:")
							+ FString::FromInt(Index),
						FUeAgentCandidateSummary(
							Change.GetIdentity(),
							CanonicalRequest
								.GetDescriptorIdentity(),
							TEXT("asset_data"),
							Change.GetIdentity().GetSelector(),
							TEXT(
								"exact AssetData change target"),
							TArray<FString>{
								TEXT("AssetData"),
								TEXT("SimpleAuthoring"),
							}),
						ResolvedTarget.GetCatalogRevision(),
						ResolvedTarget.GetPublicRef(),
						ResolvedTarget.GetReadPortId(),
						TArray<EUeAgentCandidateOperation>{
							IsMutableProductionProfile(*Profile)
								? EUeAgentCandidateOperation::Update
								: EUeAgentCandidateOperation::Create,
						},
						ResolvedTarget.GetBindingEvidence());
					if (!OutTargets.Last().IsValid())
					{
						SetProductionError(OutError, *OutTargets.Last().GetValidationDiagnostic(), Change.GetIdentity()
								.GetCanonicalKey());
						OutTargets.Reset();
						return false;
					}
				}
				if (OutTargets.IsEmpty())
				{
					OutError = ProductionFailure(TEXT("asset_data_change_target_missing"), TEXT("No asset change target was produced because the draft has no changes."), TEXT("change_count"), MakeShared<FJsonValueNumber>(Changes.Num()), MakeShared<FJsonValueString>(TEXT("at least one change")));
					return false;
				}
				return true;
			}
		};

		class FAssetDataMutationBindingProjector final
			: public IUeAgentSimpleAuthoringMutationBindingProjector
		{
		public:
			bool TryProjectMutationBindings(
				const FPlanningPreflightPlanRef& Plan,
				const TArray<FUeAgentCandidateBinding>& Targets,
				const FUeAgentCanonicalSnapshotRef& PreState,
				const FUeAgentExecutionContextRef& Context,
				FUeAgentResourceTracker& Resources,
				TArray<FUeAgentGenericEffectBinding>&
					OutBindings,
				FUeAgentSimpleAuthoringProductionError&
					OutError) override
			{
				OutBindings.Reset();
				const TArray<FPlannedChange>& Changes =
					Plan->GetChangeSet().GetChanges();
				if (Changes.Num() != Targets.Num())
				{
					SetProductionError(OutError, ProductionFailure(TEXT("asset_data_change_targets_missing"), TEXT("The number of mutation targets differs from the planned changes."), TEXT("target_count"), MakeShared<FJsonValueNumber>(Targets.Num()), MakeShared<FJsonValueNumber>(Changes.Num())), Plan->GetRequestId());
					return false;
				}
				for (int32 Index = 0;
					Index < Changes.Num();
					++Index)
				{
					OutBindings.Emplace(
						Targets[Index],
						TEXT(
							"UAI.AssetData:CanonicalObject"),
						FUeAgentCanonicalType(
							TEXT(
								"asset_data_canonical_object"),
							EUeAgentValueKind::Object,
							false,
							TEXT("object")),
						TEXT(
							"asset-data:typed-native-binding"));
					if (Targets[Index].GetSummary().GetIdentity()
							!= Changes[Index].GetIdentity()
						|| !OutBindings.Last().IsValid())
					{
						if (Targets[Index].GetSummary().GetIdentity() != Changes[Index].GetIdentity())
						{
							OutError = ProductionFailure(TEXT("asset_data_mutation_binding_invalid"), TEXT("The mutation target identity differs from the planned change."), TEXT("target_identity_matches"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
							OutError.ActualEvidence = MakeShared<FJsonValueString>(Targets[Index].GetSummary().GetIdentity().GetCanonicalKey());
							OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Changes[Index].GetIdentity().GetCanonicalKey());
						}
						else OutError = OutBindings.Last().GetValidationDiagnostic();
						OutError.OperationIndex = Index;
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Changes[Index].GetIdentity().GetSelector();
						OutBindings.Reset();
						return false;
					}
				}
				if (OutBindings.IsEmpty())
				{
					OutError = ProductionFailure(TEXT("asset_data_mutation_binding_missing"), TEXT("No mutation binding was produced because the plan has no changes."), TEXT("change_count"), MakeShared<FJsonValueNumber>(Changes.Num()), MakeShared<FJsonValueString>(TEXT("at least one change")));
					return false;
				}
				return true;
			}
		};

		class FAssetDataDurableTargetProjector final
			: public IUeAgentSimpleAuthoringDurableTargetProjector
		{
		public:
			bool TryProjectDurableTargets(
				const FPlanningPreflightPlanRef& Plan,
				const FUeAgentCandidateBinding& Target,
				const FUeAgentCanonicalSnapshotRef& PreState,
				FUeAgentResourceTracker& Resources,
				TArray<FAssetDurableTarget>& OutTargets,
				FUeAgentSimpleAuthoringProductionError&
					OutError) override
			{
				OutTargets.Reset();
				const TArray<FPlannedChange>& Changes =
					Plan->GetChangeSet().GetChanges();
				if (Changes.IsEmpty()
					|| Target.GetPublicRef().IsEmpty())
				{
					SetProductionError(OutError, Changes.IsEmpty()
						? ProductionFailure(TEXT("asset_data_durable_target_missing"), TEXT("A durable target requires at least one planned change."), TEXT("change_count"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one change")))
						: ProductionFailure(TEXT("asset_data_durable_target_missing"), TEXT("The durable target has no public asset reference."), TEXT("target_ref"), MakeShared<FJsonValueString>(Target.GetPublicRef()), MakeShared<FJsonValueString>(TEXT("non-empty asset reference"))), Target.GetBindingId());
					return false;
				}
				TSharedPtr<FJsonObject> Payload;
				FString TargetRef;
				FString PackageName;
				FString ObjectName;
				FString ObjectPath;
				FString ExpectedClassPath;
				FUeAgentDiscoveryError EvidenceError;
				if (!TryParseRequestEvidence(Target, Payload, EvidenceError)
					|| !TryReadEvidence(
						Payload,
						TargetRef,
						PackageName,
						ObjectName,
						ObjectPath,
						ExpectedClassPath, EvidenceError))
				{
					SetProductionError(OutError, EvidenceError, Target.GetBindingId());
					return false;
				}
				UClass* const ExpectedClass =
					LoadObject<UClass>(nullptr, *ExpectedClassPath);
				if (!ExpectedClass)
				{
					SetProductionError(OutError, ProductionFailure(TEXT("asset_data_durable_expected_class_missing"), TEXT("The expected persisted asset class could not be loaded."), TEXT("expected_class"), MakeShared<FJsonValueString>(ExpectedClassPath), MakeShared<FJsonValueString>(TEXT("loadable asset class"))), ExpectedClassPath);
					return false;
				}
				OutTargets.Emplace(
					Changes[0].GetIdentity(),
					Target.GetPublicRef(),
					ExpectedClass->IsChildOf(UWorld::StaticClass())
						? EPackageHeaderKind::Map
						: EPackageHeaderKind::Asset,
					EAssetDurableIntent::Save,
					ERollbackGrade::DurableBackup);
				if (!OutTargets[0].IsValid())
				{
					OutError = OutTargets[0].GetValidationDiagnostic();
					MarkNativeFailure(OutError);
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					return false;
				}
				return true;
			}
		};

		class FAssetDataExpectedStateProjector final
			: public IUeAgentSimpleAuthoringExpectedStateProjector
		{
		public:
			explicit FAssetDataExpectedStateProjector(
				TSharedRef<
					IUeAgentAssetDataSimpleEffectAssetResolver,
					ESPMode::ThreadSafe> InResolver)
				: Resolver(MoveTemp(InResolver))
			{
			}

			bool TryProjectExpectedState(
				const FPlanningPreflightPlanRef& Plan,
				const FUeAgentCandidateBinding& ResolvedTarget,
				const TArray<FUeAgentCandidateBinding>&
					ChangeTargets,
				const FUeAgentCanonicalSnapshotRef& PreState,
				const FDurableInventory& Inventory,
				FUeAgentResourceTracker& Resources,
				TArray<FVerificationTarget>& OutTargets,
				FUeAgentSimpleAuthoringProductionError&
					OutError) override
			{
				OutTargets.Reset();
				const FAssetDataProfile* Profile =
					FindProfile(Plan->GetDescriptorIdentity());
				TSharedPtr<FJsonObject> Payload;
				FString TargetRef;
				FString PackageName;
				FString ObjectName;
				FString ObjectPath;
				FString ExpectedClass;
				FString SourceRef;
				FString SourceObjectPath;
				FString SourceClass;
				FString SourceRevision;
				FUeAgentDiscoveryError EvidenceError;
				if (!Profile
					|| !IsSupportedProductionProfile(*Profile)
					|| !TryParseRequestEvidence(
						ResolvedTarget,
						Payload,
						EvidenceError)
					|| !TryReadEvidence(
						Payload,
						TargetRef,
						PackageName,
						ObjectName,
						ObjectPath,
						ExpectedClass, EvidenceError))
				{
					SetProductionError(OutError, EvidenceError.Code.IsEmpty()
							? ProductionFailure(TEXT("asset_data_expected_profile_missing"), TEXT("No supported asset profile is registered for expected-state projection."), TEXT("profile_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true))
							: EvidenceError, Plan->GetDescriptorIdentity()
							.GetDescriptorId());
					return false;
				}
				if (Profile->IsClone()
					&& !TryReadCloneEvidence(
						Payload,
						SourceRef,
						SourceObjectPath,
						SourceClass,
						SourceRevision, EvidenceError))
				{
					SetProductionError(OutError, EvidenceError, Plan->GetDescriptorIdentity().GetDescriptorId());
					return false;
				}
				if (Profile->IsClone()
					&& (ExpectedClass != SourceClass
						|| SourceObjectPath.IsEmpty()))
				{
					OutError = ExpectedClass != SourceClass
						? ProductionFailure(TEXT("asset_data_clone_expected_class_mismatch"), TEXT("The clone source class differs from the expected target class."), TEXT("source_class"), MakeShared<FJsonValueString>(SourceClass), MakeShared<FJsonValueString>(ExpectedClass))
						: ProductionFailure(TEXT("asset_data_clone_expected_class_mismatch"), TEXT("The expected clone source path is empty."), TEXT("source_object_path"), MakeShared<FJsonValueString>(SourceObjectPath), MakeShared<FJsonValueString>(TEXT("non-empty asset path")));
					OutError.TargetPath = SourceRef;
					return false;
				}
				FUeAgentCanonicalValuePtr ExpectedValue;
				FUeAgentCanonicalValuePtr DefaultValue;
				FUeAgentStateSemanticError StateError;
				if (IsMutableProductionProfile(*Profile))
				{
					const TArray<FPlannedChange>& Changes =
						Plan->GetChangeSet().GetChanges();
					const FUeAgentCanonicalValue* CurrentFields =
						Changes.Num() == 1
							&& Changes[0].GetBefore().IsValid()
							? FindCanonicalField(
								*Changes[0].GetBefore(),
								TEXT("field_state"))
							: nullptr;
					const FUeAgentCanonicalValue* DefaultFields =
						Changes.Num() == 1
							&& Changes[0].GetBefore().IsValid()
							? FindCanonicalField(
								*Changes[0].GetBefore(),
								TEXT("field_defaults"))
							: nullptr;
					const FUeAgentCanonicalValue*
						ProjectionContext =
							Changes.Num() == 1
								&& Changes[0]
									.GetBefore()
									.IsValid()
								? FindCanonicalField(
									*Changes[0].GetBefore(),
									TEXT(
										"field_projection_context"))
								: nullptr;
					FUeAgentCanonicalValuePtr ExpectedFields;
					FExecutionMutationError ProjectionError;
					if (!CurrentFields
						|| !DefaultFields
						|| !ProjectionContext
						|| !Changes[0].GetAfter().IsValid()
						|| !TryProjectAssetDataSimpleEffectExpectedFieldState(
							*Resolver,
							Profile->NativeMechanicId,
							*CurrentFields,
							*DefaultFields,
							*ProjectionContext,
							*Changes[0].GetAfter(),
							Resources,
							ExpectedFields,
							ProjectionError)
						|| !TryBuildMutableAssetState(
							TargetRef,
							ObjectPath,
							ExpectedClass,
							*Profile,
							ExpectedFields.ToSharedRef(),
							FString(),
							FUeAgentCanonicalValuePtr(),
							FUeAgentCanonicalValuePtr(),
							Resources,
							ExpectedValue,
							StateError))
					{
						if (!ProjectionError.IsEmpty())
						{
							OutError = ProjectionError;
							if (Changes.Num() == 1 && Changes[0].GetAfter().IsValid())
								LocateAssetDataSimpleEffectInputFailure(OutError, *Changes[0].GetAfter());
						}
						else if (!StateError.IsEmpty()) { OutError = StateError; MarkNativeFailure(OutError); }
						else if (Changes.Num() != 1)
							OutError = ProductionFailure(TEXT("asset_data_expected_field_state_invalid"), TEXT("Mutable asset state projection requires exactly one planned change."), TEXT("change_count"), MakeShared<FJsonValueNumber>(Changes.Num()), MakeShared<FJsonValueNumber>(1));
						else
						{
							const FString MissingField = !CurrentFields ? TEXT("field_state") : !DefaultFields ? TEXT("field_defaults")
								: !ProjectionContext ? TEXT("field_projection_context") : TEXT("proposed_after");
							OutError = ProductionFailure(TEXT("asset_data_expected_field_state_invalid"),
								TEXT("A required captured value is missing from expected-state projection."), MissingField,
								nullptr, MakeShared<FJsonValueString>(TEXT("available captured value")));
						}
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
						return false;
					}
				}
				else
				{
					const TSharedPtr<FJsonObject> ExpectedState =
						Profile->IsClone()
							? BuildClonedAssetState(
								TargetRef,
								ObjectPath,
								ExpectedClass,
								SourceRef,
								SourceRevision,
								true,
								true,
								*Profile)
							: BuildCreatedAssetState(
								TargetRef,
								ObjectPath,
								ExpectedClass,
								*Profile,
								nullptr,
								true);
					if (!TryCanonicalObject(
							ExpectedState,
							Resources,
							ExpectedValue,
							StateError))
					{
						SetProductionError(OutError, StateError, TargetRef);
						return false;
					}
				}
				if (!FUeAgentCanonicalValue::TryObject(
						{},
						Resources,
						DefaultValue,
						StateError))
				{
					SetProductionError(OutError, StateError, TargetRef);
					return false;
				}
				const FUeAgentStateRevision Revision(
					TEXT("asset-data-operation-expected"),
					Plan->GetFingerprint(),
					1);
				TArray<FString> ArtifactIds;
				for (const FDurableArtifact& Artifact
					: Inventory.GetArtifacts())
				{
					ArtifactIds.Add(Artifact.GetArtifactId());
				}
				for (const FPlannedChange& Change
					: Plan->GetChangeSet().GetChanges())
				{
					const FUeAgentCandidateBinding* ChangeTarget =
						ChangeTargets.FindByPredicate(
							[&Change](
								const FUeAgentCandidateBinding&
									Candidate)
							{
								return Candidate.GetSummary()
									.GetIdentity()
									== Change.GetIdentity();
							});
					if (!ChangeTarget)
					{
						SetProductionError(OutError, ProductionFailure(TEXT("asset_data_verification_target_missing"), TEXT("No verification binding matches this planned asset change."), TEXT("target_binding_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), Change.GetIdentity()
								.GetCanonicalKey());
						return false;
					}
					TArray<FUeAgentStateEntry> Entries;
					Entries.Emplace(
						Change.GetIdentity(),
						AssetDataStateType(),
						ExpectedValue.ToSharedRef(),
						AssetDataStateDefault(
							DefaultValue.ToSharedRef()),
						AssetDataStateDetails(*Profile),
						Revision,
						EUeAgentAuthoredPresence::Present,
						EUeAgentOverrideState::Overridden);
					FUeAgentCanonicalSnapshotPtr Expected;
					if (!FUeAgentCanonicalSnapshot::
							TryCreateExpected(
								TEXT(
									"snapshot:asset-data:"
									"expected:")
									+ Plan->GetRequestId(),
								EUeAgentStatePlane::Persisted,
								Revision,
								FUeAgentTemporalPoint(
									TEXT(
										"asset-data-operation"),
									TEXT("1"),
									TEXT("1"),
									TEXT("1"),
									TEXT("expected")),
								FUeAgentStateProvenance(
									TEXT(
										"operation_expected_state"),
									Plan->GetRequestId(),
									Plan->GetFingerprint()),
								MoveTemp(Entries),
								Resources,
								Expected,
								StateError))
					{
						SetProductionError(OutError, StateError, Change.GetIdentity()
								.GetCanonicalKey());
						return false;
					}
					OutTargets.Emplace(
						Change.GetIdentity(),
						*ChangeTarget,
						EVerificationOracleKind::Persisted,
						Expected,
						ArtifactIds,
						TArray<FString>());
					if (!OutTargets.Last().IsValid())
					{
						const FVerificationError* ValidationError = OutTargets.Last().GetValidationDiagnostic();
						OutError = ValidationError ? *ValidationError
							: ProductionFailure(TEXT("asset_data_verification_target_invalid"), TEXT("The asset verification target is invalid and returned no validation diagnostic."), TEXT("verification_target_valid"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						MarkNativeFailure(OutError);
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Change.GetIdentity().GetSelector();
						return false;
					}
				}
				if (OutTargets.IsEmpty())
				{
					OutError = ProductionFailure(TEXT("asset_data_verification_target_missing"), TEXT("No verification target was produced because the plan has no changes."), TEXT("change_count"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one change")));
					return false;
				}
				return true;
			}

		private:
			TSharedRef<
				IUeAgentAssetDataSimpleEffectAssetResolver,
				ESPMode::ThreadSafe> Resolver;
		};

		class FAssetDataUeHook final
			: public IUeAgentGenericEffectUeHook
		{
		public:
			explicit FAssetDataUeHook(
				TSharedRef<
					IUeAgentAssetDataSimpleEffectNativeHook,
					ESPMode::ThreadSafe> InNativeHook)
				: NativeHook(MoveTemp(InNativeHook))
			{
			}

			bool TryApply(
				const FExecutionMutationRequest& Request,
				const FLockFenceLease& Lease,
				const TArray<FUeAgentGenericEffectBinding>&
					Bindings,
				FUeAgentResourceTracker& Resources,
				FUeAgentGenericEffectApplyResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				return NativeHook->TryApply(
					Request,
					Lease,
					Bindings,
					Resources,
					OutResult,
					OutError);
			}

			bool TryRollback(
				const FExecutionMutationRequest& Request,
				const FUeAgentGenericEffectApplyResult& Applied,
				FUeAgentResourceTracker& Resources,
				bool& bOutConfirmed,
				FExecutionMutationError& OutError) override
			{
				return NativeHook->TryRollback(
					Request,
					Applied,
					Resources,
					bOutConfirmed,
					OutError);
			}

			void ReleaseVerifiedOperation(
				const FString& OperationId) override
			{
				NativeHook->ReleaseVerifiedOperation(OperationId);
			}

			bool TryWaitForSettle(
				const FBarrierRequest& Request,
				FUeAgentResourceTracker& Resources,
				FBarrierResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				return NativeHook->TryWaitForSettle(
					Request,
					Resources,
					OutResult,
					OutError);
			}

		private:
			TSharedRef<
				IUeAgentAssetDataSimpleEffectNativeHook,
				ESPMode::ThreadSafe> NativeHook;
		};

		bool ValidateConfig(
			const FUeAgentAssetDataSimpleAuthoringProviderConfig&
				Config,
			FUeAgentAssetDataSimpleAuthoringError& OutError)
		{
			if (!Config.DescriptorRegistry.IsValid()
				|| !Config.MutationAssetResolver.IsValid()
				|| !Config.PreStateAssetResolver.IsValid()
				|| !Config.PersistedAssetResolver.IsValid()
				|| Config.PreStateAssetResolver
					== Config.PersistedAssetResolver
				|| !Config.FactoryResolver.IsValid()
				|| !Config.DurableService.IsValid()
				|| !Config.AbsenceOracle.IsValid()
				|| !Config.ReferenceOracle.IsValid()
				|| !Config.Projection.IsSet()
				|| !Config.Projection->IsValid()
				|| Config.OwnerLeaseId.IsEmpty())
			{
				const FString Field = !Config.DescriptorRegistry.IsValid() ? TEXT("descriptor_registry")
					: !Config.MutationAssetResolver.IsValid() ? TEXT("mutation_asset_resolver")
					: !Config.PreStateAssetResolver.IsValid() ? TEXT("prestate_asset_resolver")
					: !Config.PersistedAssetResolver.IsValid() ? TEXT("persisted_asset_resolver")
					: Config.PreStateAssetResolver == Config.PersistedAssetResolver ? TEXT("independent_persisted_resolver")
					: !Config.FactoryResolver.IsValid() ? TEXT("factory_resolver")
					: !Config.DurableService.IsValid() ? TEXT("durable_service")
					: !Config.AbsenceOracle.IsValid() ? TEXT("absence_oracle")
					: !Config.ReferenceOracle.IsValid() ? TEXT("reference_oracle")
					: !Config.Projection.IsSet() ? TEXT("projection_available")
					: !Config.Projection->IsValid() ? TEXT("projection_valid") : TEXT("owner_lease_available");
				OutError = ProductionFailure(TEXT("asset_data_simple_authoring_dependencies_missing"),
					FString::Printf(TEXT("The asset authoring provider requires %s."), *Field),
					Field, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			const FAssetDataProfileCatalog& Catalog =
				GetProfileCatalog();
			int32 AssetDataDescriptorCount = 0;
			for (const FUeAgentOperationDescriptor& Descriptor
				: Config.DescriptorRegistry->GetDescriptors())
			{
				AssetDataDescriptorCount +=
					Descriptor.GetDomainId() == TEXT("asset_data")
						? 1
						: 0;
			}
			if (!Catalog.Error.IsEmpty()
				|| Catalog.Profiles.Num()
					!= AssetDataDescriptorCount)
			{
				SetError(OutError, Catalog.Error.IsEmpty()
					? ProductionFailure(TEXT("asset_data_simple_authoring_profile_catalog_invalid"), TEXT("The asset profile count differs from the registered operation count."), TEXT("profile_count"), MakeShared<FJsonValueNumber>(Catalog.Profiles.Num()), MakeShared<FJsonValueNumber>(AssetDataDescriptorCount))
					: Catalog.Error, TEXT("asset_data"));
				return false;
			}
			for (const FAssetDataProfile& Profile
				: Catalog.Profiles)
			{
				const FUeAgentOperationDescriptor* Descriptor =
					Config.DescriptorRegistry
						->FindByDescriptorId(Profile.DescriptorId);
				if (!Descriptor
					|| Descriptor->GetCapabilityId()
						!= Profile.CapabilityId
					|| Descriptor->GetDomainId()
						!= TEXT("asset_data")
					|| Descriptor->GetIdentity().GetVersion()
						!= Profile.DescriptorVersion
					|| Descriptor->GetIdentity()
						.GetDescriptorHash()
						!= Profile.DescriptorHash)
				{
					if (!Descriptor)
						OutError = ProductionFailure(TEXT("asset_data_simple_authoring_descriptor_missing"), TEXT("An asset profile has no registered operation descriptor."), TEXT("descriptor_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					else
					{
						const FString Field = Descriptor->GetCapabilityId() != Profile.CapabilityId ? TEXT("capability")
							: Descriptor->GetDomainId() != TEXT("asset_data") ? TEXT("domain")
							: Descriptor->GetIdentity().GetVersion() != Profile.DescriptorVersion ? TEXT("version") : TEXT("hash");
						OutError = ProductionFailure(TEXT("asset_data_simple_authoring_descriptor_missing"),
							FString::Printf(TEXT("The registered descriptor %s differs from its asset profile."), *Field),
							TEXT("descriptor_") + Field + TEXT("_matches"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						if (Field == TEXT("version"))
						{
							OutError.ActualEvidence = MakeShared<FJsonValueNumber>(Descriptor->GetIdentity().GetVersion());
							OutError.ExpectedEvidence = MakeShared<FJsonValueNumber>(Profile.DescriptorVersion);
						}
						else
						{
							OutError.ActualEvidence = MakeShared<FJsonValueString>(Field == TEXT("capability") ? Descriptor->GetCapabilityId()
								: Field == TEXT("domain") ? Descriptor->GetDomainId() : Descriptor->GetIdentity().GetDescriptorHash());
							OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Field == TEXT("capability") ? Profile.CapabilityId
								: Field == TEXT("domain") ? FString(TEXT("asset_data")) : Profile.DescriptorHash);
						}
					}
					OutError.SubjectId = Profile.DescriptorId;
					return false;
				}
			}
			return true;
		}
	}

	bool TryObservePersistedCreatedAsset(
		const TArray<AssetDurable::FDurableArtifact>& Artifacts,
		const FString& SourcePackageName,
		const FString& SourceObjectName,
		const FString& SourceObjectPath,
		const FString& ExpectedClassPath,
		FUeAgentPersistedCreatedAssetObservation& OutObservation,
		FUeAgentAssetDataSimpleAuthoringError& OutError)
	{
		using namespace AssetDurable;
		OutObservation = {};
		OutError = {};
		UClass* ExpectedClass =
			FindObject<UClass>(nullptr, *ExpectedClassPath);
		if (!ExpectedClass)
		{
			ExpectedClass =
				LoadObject<UClass>(nullptr, *ExpectedClassPath);
		}
		if (Artifacts.IsEmpty()
			|| !FPackageName::IsValidLongPackageName(SourcePackageName)
			|| SourceObjectName.IsEmpty()
			|| SourceObjectPath
				!= SourcePackageName + TEXT(".") + SourceObjectName
			|| !ExpectedClass)
		{
			if (Artifacts.IsEmpty())
				OutError = ProductionFailure(TEXT("asset_data_persisted_snapshot_request_invalid"), TEXT("Persisted observation requires at least one durable artifact."), TEXT("artifact_count"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one artifact")));
			else if (!FPackageName::IsValidLongPackageName(SourcePackageName))
				OutError = ProductionFailure(TEXT("asset_data_persisted_snapshot_request_invalid"), TEXT("The persisted source package name is invalid."), TEXT("package_name"), MakeShared<FJsonValueString>(SourcePackageName), MakeShared<FJsonValueString>(TEXT("valid long package name")));
			else if (SourceObjectName.IsEmpty())
				OutError = ProductionFailure(TEXT("asset_data_persisted_snapshot_request_invalid"), TEXT("The persisted source object name is empty."), TEXT("object_name"), MakeShared<FJsonValueString>(SourceObjectName), MakeShared<FJsonValueString>(TEXT("non-empty object name")));
			else if (SourceObjectPath != SourcePackageName + TEXT(".") + SourceObjectName)
				OutError = ProductionFailure(TEXT("asset_data_persisted_snapshot_request_invalid"), TEXT("The persisted source path differs from its package and object name."), TEXT("object_path"), MakeShared<FJsonValueString>(SourceObjectPath), MakeShared<FJsonValueString>(SourcePackageName + TEXT(".") + SourceObjectName));
			else
				OutError = ProductionFailure(TEXT("asset_data_persisted_snapshot_request_invalid"), TEXT("The expected persisted asset class could not be loaded."), TEXT("expected_class"), MakeShared<FJsonValueString>(ExpectedClassPath), MakeShared<FJsonValueString>(TEXT("loadable asset class")));
			OutError.TargetPath = SourceObjectPath;
			return false;
		}

		const bool bWorld =
			ExpectedClass->IsChildOf(UWorld::StaticClass());
		if (bWorld)
		{
			FPersistedWorldLoadError LoadError;
			const bool bObserved = TryObservePersistedWorld(
				Artifacts,
				[&](
					UWorld& SnapshotWorld,
					const FPersistedWorldReferenceMap& ReferenceMap,
					FPersistedWorldLoadError& ObserverError)
				{
					FString SnapshotRef;
					if (!ReferenceMap.TryMapReference(
							SourceObjectPath,
							SnapshotRef)
						|| SnapshotRef != SnapshotWorld.GetPathName()
						|| SnapshotWorld.GetClass()->GetPathName()
							!= ExpectedClassPath
						|| !SnapshotWorld.PersistentLevel)
					{
						if (SnapshotRef.IsEmpty() || SnapshotRef != SnapshotWorld.GetPathName())
						{
							ObserverError = ProductionFailure(TEXT("asset_data_persisted_world_identity_mismatch"), TEXT("The persisted world does not match the requested source object."), TEXT("snapshot_identity_matches"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
							ObserverError.ActualEvidence = MakeShared<FJsonValueString>(SnapshotWorld.GetPathName());
							ObserverError.ExpectedEvidence = MakeShared<FJsonValueString>(SnapshotRef);
						}
						else if (SnapshotWorld.GetClass()->GetPathName() != ExpectedClassPath)
							ObserverError = ProductionFailure(TEXT("asset_data_persisted_world_identity_mismatch"), TEXT("The persisted world class differs from the expected class."), TEXT("class"), MakeShared<FJsonValueString>(SnapshotWorld.GetClass()->GetPathName()), MakeShared<FJsonValueString>(ExpectedClassPath));
						else
							ObserverError = ProductionFailure(TEXT("asset_data_persisted_world_identity_mismatch"), TEXT("The persisted world has no persistent level."), TEXT("persistent_level_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						ObserverError.TargetPath = SourceObjectPath;
						return false;
					}
					OutObservation.SourceObjectPath = SourceObjectPath;
					OutObservation.ClassPath = ExpectedClassPath;
					OutObservation.bWorld = true;
					OutObservation.bHasPersistentLevel = true;
					OutObservation.SnapshotSequence =
						SnapshotWorld.GetUniqueID();
					return TryValidateObservation(OutObservation, ObserverError);
				},
				LoadError);
			if (!bObserved)
			{
				OutError = LoadError;
				OutError.SubjectId = SourceObjectPath;
			}
			return bObserved && TryValidateObservation(OutObservation, OutError);
		}

		FPersistedPackageLoadError LoadError;
		const bool bObserved = TryObservePersistedPackage(
			Artifacts,
			SourcePackageName,
			[&](
				UPackage& SnapshotPackage,
				FPersistedPackageLoadError& ObserverError)
			{
				UObject* SnapshotObject = StaticFindObjectFast(
					UObject::StaticClass(),
					&SnapshotPackage,
					FName(*SourceObjectName));
				if (!IsValid(SnapshotObject)
					|| SnapshotObject->GetOutermost()
						!= &SnapshotPackage
					|| SnapshotObject->GetClass()->GetPathName()
						!= ExpectedClassPath)
				{
					if (!IsValid(SnapshotObject))
						ObserverError = ProductionFailure(TEXT("asset_data_persisted_object_identity_mismatch"), TEXT("The persisted package does not contain the requested object."), TEXT("object_name"), MakeShared<FJsonValueString>(SourceObjectName), MakeShared<FJsonValueString>(TEXT("existing object in persisted package")));
					else if (SnapshotObject->GetOutermost() != &SnapshotPackage)
					{
						ObserverError = ProductionFailure(TEXT("asset_data_persisted_object_identity_mismatch"), TEXT("The persisted object belongs to a different snapshot package."), TEXT("package_matches"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						ObserverError.ActualEvidence = MakeShared<FJsonValueString>(SnapshotObject->GetOutermost()->GetName());
						ObserverError.ExpectedEvidence = MakeShared<FJsonValueString>(SnapshotPackage.GetName());
					}
					else
						ObserverError = ProductionFailure(TEXT("asset_data_persisted_object_identity_mismatch"), TEXT("The persisted object class differs from the expected class."), TEXT("class"), MakeShared<FJsonValueString>(SnapshotObject->GetClass()->GetPathName()), MakeShared<FJsonValueString>(ExpectedClassPath));
					ObserverError.TargetPath = SourceObjectPath;
					return false;
				}
				OutObservation.SourceObjectPath = SourceObjectPath;
				OutObservation.ClassPath = ExpectedClassPath;
				OutObservation.bWorld = false;
				OutObservation.bHasPersistentLevel = false;
				OutObservation.SnapshotSequence =
					SnapshotObject->GetUniqueID();
				return TryValidateObservation(OutObservation, ObserverError);
			},
			LoadError);
		if (!bObserved)
		{
			OutError = LoadError;
			OutError.SubjectId = SourceObjectPath;
		}
		return bObserved && TryValidateObservation(OutObservation, OutError);
	}

	bool FUeAgentAssetDataSimpleAuthoringProviderFactory::TryCreate(
		const FUeAgentAssetDataSimpleAuthoringProviderConfig& Config,
		TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider>&
			OutProvider,
		FUeAgentAssetDataSimpleAuthoringError& OutError)
	{
		OutProvider.Reset();
		OutError = FUeAgentAssetDataSimpleAuthoringError();
		if (!ValidateConfig(Config, OutError))
		{
			return false;
		}

		const TSharedRef<
			IUeAgentAssetDataSimpleEffectAssetResolver,
			ESPMode::ThreadSafe> MutationResolver =
				Config.MutationAssetResolver.ToSharedRef();
		const TSharedRef<
			IUeAgentAssetDataSimpleEffectAssetResolver,
			ESPMode::ThreadSafe> PreStateResolver =
				Config.PreStateAssetResolver.ToSharedRef();
		const TSharedRef<
			IUeAgentAssetDataSimpleEffectAssetResolver,
			ESPMode::ThreadSafe> PersistedResolver =
				Config.PersistedAssetResolver.ToSharedRef();
		const TSharedRef<
			IUeAgentAssetDataSimpleEffectFactoryCandidateResolver,
			ESPMode::ThreadSafe> FactoryResolver =
				Config.FactoryResolver.ToSharedRef();
		TSharedPtr<IUeAgentAssetDataSimpleEffectNativeHook>
			NativeHook;
		FExecutionMutationError NativeError;
		if (!FUeAgentAssetDataSimpleEffectNativeHookFactory::
				TryCreate(
					MutationResolver,
					FactoryResolver,
					NativeHook,
					NativeError)
			|| !NativeHook.IsValid())
		{
			OutError = NativeError.IsValid() ? NativeError : ProductionFailure(TEXT("asset_data_native_hook_missing"), TEXT("The asset native-hook factory returned no hook."), TEXT("native_hook_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TEXT("asset_data");
			return false;
		}

		FUeAgentSimpleAuthoringProductionProviderConfig
			ProviderConfig;
		ProviderConfig.FamilyId = TEXT("asset_data");
		ProviderConfig.OwnerLeaseId = Config.OwnerLeaseId;
		ProviderConfig.PreStatePlane =
			EUeAgentStatePlane::AuthoringMemory;
		ProviderConfig.AtomicityMode =
			EAtomicityMode::CompensatedSaga;
		ProviderConfig.Projection = Config.Projection;
		ProviderConfig.TargetResolver =
			MakeShared<FAssetDataTargetResolver>(
				PreStateResolver,
				FactoryResolver);
		ProviderConfig.PreStateReadPort =
			MakeShared<FAssetDataStateReadPort>(
				PreStatePortId,
				EUeAgentStatePlane::AuthoringMemory,
				PreStateResolver);
		ProviderConfig.ChangeTargetProjector =
			MakeShared<FAssetDataChangeTargetProjector>();
		ProviderConfig.DraftBuilder =
			MakeShared<FAssetDataDraftBuilder>(
				FactoryResolver);
		ProviderConfig.MutationBindingProjector =
			MakeShared<FAssetDataMutationBindingProjector>();
		ProviderConfig.DurableTargetProjector =
			MakeShared<FAssetDataDurableTargetProjector>();
		ProviderConfig.DurableService = Config.DurableService;
		ProviderConfig.UeHook =
			MakeShared<FAssetDataUeHook>(
				NativeHook.ToSharedRef());
		ProviderConfig.ExpectedStateProjector =
			MakeShared<FAssetDataExpectedStateProjector>(
				PreStateResolver);
		const TSharedRef<FAssetDataStateReadPort, ESPMode::ThreadSafe>
			PersistedStateReadPort = MakeShared<FAssetDataStateReadPort>(
				PersistedStatePortId,
				EUeAgentStatePlane::Persisted,
				PersistedResolver);
		ProviderConfig.PersistedStateOracle = MakeShared<
			FAssetDataPersistedStateOracle,
			ESPMode::ThreadSafe>(PersistedStateReadPort);
		ProviderConfig.AbsenceOracle = Config.AbsenceOracle;
		ProviderConfig.ReferenceOracle = Config.ReferenceOracle;

		FUeAgentSimpleAuthoringProductionError Error;
		if (!FUeAgentSimpleAuthoringProductionProvider::TryCreate(
				ProviderConfig,
				OutProvider,
				Error))
		{
			OutError = Error;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Error.SubjectId;
			return false;
		}
		if (!OutProvider.IsValid())
		{
			OutError = ProductionFailure(TEXT("asset_data_provider_missing"), TEXT("The asset authoring provider factory returned no provider."), TEXT("provider_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		return true;
	}
}
