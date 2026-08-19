#include "Composition/SimpleAuthoring/Families/AssetData/UeAgentAssetDataSimpleAuthoringProduction.h"

#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/ArchiveObjectCrc32.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

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
			FString Error;
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

		FString BuildAssetRevision(UObject* Object)
		{
			return IsValid(Object)
				? FString::Printf(
					TEXT("asset-crc32:%08x"),
					FArchiveObjectCrc32().Crc32(Object))
				: FString();
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

		void SetError(
			FUeAgentAssetDataSimpleAuthoringError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		void SetProductionError(
			FUeAgentSimpleAuthoringProductionError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		bool TryParseJson(
			const FString& Json,
			TSharedPtr<FJsonObject>& OutObject)
		{
			OutObject.Reset();
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Json);
			return FJsonSerializer::Deserialize(Reader, OutObject)
				&& OutObject.IsValid();
		}

		bool TrySerializeJson(
			const TSharedPtr<FJsonObject>& Object,
			FString& OutJson)
		{
			OutJson.Reset();
			if (!Object.IsValid())
			{
				return false;
			}
			const TSharedRef<TJsonWriter<>> Writer =
				TJsonWriterFactory<>::Create(&OutJson);
			return FJsonSerializer::Serialize(
				Object.ToSharedRef(),
				Writer);
		}

		bool TryParseRequestEvidence(
			const FUeAgentCandidateBinding& Binding,
			TSharedPtr<FJsonObject>& OutPayload,
			FUeAgentDiscoveryError& OutError)
		{
			const FString& Evidence = Binding.GetBindingEvidence();
			if (!Evidence.StartsWith(RequestEvidencePrefix)
				|| !TryParseJson(
					Evidence.RightChop(
						FCString::Strlen(RequestEvidencePrefix)),
					OutPayload))
			{
				OutError.Code =
					TEXT("asset_data_request_evidence_invalid");
				OutError.SubjectId = Binding.GetBindingId();
				return false;
			}
			return true;
		}

		bool TryReadEvidence(
			const TSharedPtr<FJsonObject>& Payload,
			FString& OutTargetRef,
			FString& OutPackageName,
			FString& OutObjectName,
			FString& OutObjectPath,
			FString& OutExpectedClass)
		{
			OutTargetRef.Reset();
			OutPackageName.Reset();
			OutObjectName.Reset();
			OutObjectPath.Reset();
			OutExpectedClass.Reset();
			return Payload.IsValid()
				&& Payload->TryGetStringField(
					TEXT("target_ref"),
					OutTargetRef)
				&& Payload->TryGetStringField(
					EvidencePackageName,
					OutPackageName)
				&& Payload->TryGetStringField(
					EvidenceObjectName,
					OutObjectName)
				&& Payload->TryGetStringField(
					EvidenceObjectPath,
					OutObjectPath)
				&& Payload->TryGetStringField(
					EvidenceExpectedClass,
					OutExpectedClass)
				&& !OutTargetRef.IsEmpty()
				&& !OutPackageName.IsEmpty()
				&& !OutObjectName.IsEmpty()
				&& !OutObjectPath.IsEmpty()
				&& !OutExpectedClass.IsEmpty();
		}

		bool TryReadCloneEvidence(
			const TSharedPtr<FJsonObject>& Payload,
			FString& OutSourceRef,
			FString& OutSourceObjectPath,
			FString& OutSourceClass,
			FString& OutSourceRevision)
		{
			OutSourceRef.Reset();
			OutSourceObjectPath.Reset();
			OutSourceClass.Reset();
			OutSourceRevision.Reset();
			return Payload.IsValid()
				&& Payload->TryGetStringField(
					EvidenceSourceRef,
					OutSourceRef)
				&& Payload->TryGetStringField(
					EvidenceSourceObjectPath,
					OutSourceObjectPath)
				&& Payload->TryGetStringField(
					EvidenceSourceClass,
					OutSourceClass)
				&& Payload->TryGetStringField(
					EvidenceSourceRevision,
					OutSourceRevision)
				&& !OutSourceRef.IsEmpty()
				&& !OutSourceObjectPath.IsEmpty()
				&& !OutSourceClass.IsEmpty()
				&& !OutSourceRevision.IsEmpty();
		}

		bool TryCanonicalValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			OutValue.Reset();
			if (!JsonValue.IsValid())
			{
				OutError.Code =
					TEXT("asset_data_state_json_invalid");
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
					FString::SanitizeFloat(
						JsonValue->AsNumber()),
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
					TArray<TPair<
						FString,
						FUeAgentCanonicalValueRef>> Items;
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
				OutError.Code =
					TEXT("asset_data_state_json_kind_invalid");
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
			TArray<FString>& OutFieldIds)
		{
			OutFieldIds.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Changes =
				nullptr;
			if (!Payload.IsValid()
				|| !Payload->TryGetArrayField(
					TEXT("changes"),
					Changes)
				|| !Changes)
			{
				return false;
			}
			TSet<FString> Seen;
			for (const TSharedPtr<FJsonValue>& Value : *Changes)
			{
				const TSharedPtr<FJsonObject> Change =
					Value.IsValid()
						? Value->AsObject()
						: nullptr;
				FString FieldId;
				if (!Change.IsValid()
					|| !Change->TryGetStringField(
						TEXT("field_id"),
						FieldId)
					|| !Profile.FieldIds.Contains(FieldId)
					|| Seen.Contains(FieldId))
				{
					OutFieldIds.Reset();
					return false;
				}
				Seen.Add(FieldId);
				OutFieldIds.Add(MoveTemp(FieldId));
			}
			return !OutFieldIds.IsEmpty();
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
			const FAssetDataProfile& Profile)
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
				if (!Profile
					|| !IsSupportedProductionProfile(*Profile)
					|| !TryParseJson(
						CanonicalRequest.GetPayload()
							.GetJsonValue(),
						Payload)
					|| !Payload->TryGetStringField(
						TEXT("target_ref"),
						TargetRef)
					|| TargetRef.IsEmpty())
				{
					SetProductionError(
						OutError,
						TEXT(
							"asset_data_production_profile_missing"),
						CanonicalRequest.GetDescriptorIdentity()
							.GetDescriptorId());
					return false;
				}
				if (!Deadline.CanContinue()
					|| Cancellation.IsCancelled())
				{
					SetProductionError(
						OutError,
						TEXT("asset_data_target_resolution_cancelled"),
						TargetRef);
					return false;
				}
				UPackage* Package = nullptr;
				FName ObjectName;
				UObject* ExistingTarget = nullptr;
				FExecutionMutationError ResolveError;
				if (IsMutableProductionProfile(*Profile))
				{
					FString ExpectedRevision;
					if (!Payload->TryGetStringField(
							TEXT("expected_revision"),
							ExpectedRevision)
						|| ExpectedRevision.IsEmpty()
						|| !Resolver->TryResolveExisting(
							TargetRef,
							ExistingTarget,
							ResolveError)
						|| !IsValid(ExistingTarget))
					{
						SetProductionError(
							OutError,
							ResolveError.GetCode().IsEmpty()
								? TEXT(
									"asset_data_existing_target_unresolved")
								: ResolveError.GetCode(),
							TargetRef,
							ResolveError.GetDetail());
						return false;
					}
					if (BuildAssetRevision(ExistingTarget)
						!= ExpectedRevision)
					{
						SetProductionError(
							OutError,
							TEXT(
								"asset_data_target_revision_mismatch"),
							TargetRef);
						return false;
					}
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
					SetProductionError(
						OutError,
						ResolveError.GetCode().IsEmpty()
							? TEXT(
								"asset_data_creation_target_unresolved")
							: ResolveError.GetCode(),
						TargetRef,
						ResolveError.GetDetail());
					return false;
				}
				if (!IsMutableProductionProfile(*Profile)
					&& StaticFindObjectFast(
						UObject::StaticClass(),
						Package,
						ObjectName))
				{
					SetProductionError(
						OutError,
						TEXT("asset_data_target_already_exists"),
						TargetRef);
					return false;
				}
				UObject* Source = nullptr;
				FString SourceRef;
				FString SourceRevision;
				if (Profile->IsClone())
				{
					FString ExpectedSourceRevision;
					if (!Payload->TryGetStringField(
							TEXT("source_ref"),
							SourceRef)
						|| SourceRef.IsEmpty()
						|| !Payload->TryGetStringField(
							TEXT(
								"expected_source_revision"),
							ExpectedSourceRevision)
						|| ExpectedSourceRevision.IsEmpty())
					{
						SetProductionError(
							OutError,
							TEXT(
								"asset_data_clone_source_missing"),
							TargetRef);
						return false;
					}
					if (!TryResolveCloneSource(
							Resolver, SourceRef, Source, ResolveError)
						|| !IsValid(Source))
					{
						SetProductionError(
							OutError,
							ResolveError.GetCode().IsEmpty()
								? TEXT(
									"asset_data_clone_source_unresolved")
								: ResolveError.GetCode(),
							SourceRef,
							ResolveError.GetDetail());
						return false;
					}
					SourceRevision =
						BuildAssetRevision(Source);
					if (SourceRevision.IsEmpty()
						|| SourceRevision
							!= ExpectedSourceRevision)
					{
						SetProductionError(
							OutError,
							TEXT(
								"asset_data_source_revision_mismatch"),
							SourceRef);
						return false;
					}
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
					const TSharedPtr<FJsonObject>*
						FactoryRefObject = nullptr;
					const TSharedPtr<FJsonObject>*
						RevisionObject = nullptr;
					FString FactoryRef;
					FString SourceId;
					FString Generation;
					double Sequence = -1.0;
					if (!Payload->TryGetObjectField(
							TEXT("factory_ref"),
							FactoryRefObject)
						|| !FactoryRefObject
						|| !FactoryRefObject->IsValid()
						|| !(*FactoryRefObject)
							->TryGetStringField(
								TEXT("candidate_ref"),
								FactoryRef)
						|| !(*FactoryRefObject)
							->TryGetObjectField(
								TEXT("catalog_revision"),
								RevisionObject)
						|| !RevisionObject
						|| !RevisionObject->IsValid()
						|| !(*RevisionObject)
							->TryGetStringField(
								TEXT("source_id"),
								SourceId)
						|| !(*RevisionObject)
							->TryGetStringField(
								TEXT("generation"),
								Generation)
						|| !(*RevisionObject)
							->TryGetNumberField(
								TEXT("sequence"),
								Sequence)
						|| Sequence < 0.0
						|| !FMath::IsNearlyEqual(
							Sequence,
							FMath::FloorToDouble(Sequence)))
					{
						SetProductionError(
							OutError,
							TEXT(
								"asset_data_factory_ref_invalid"),
							Profile->ProfileId);
						return false;
					}
					const FUeAgentCatalogRevision Revision(
						SourceId,
						Generation,
						static_cast<int64>(Sequence));
					FUeAgentAssetDataSimpleEffectFactoryBinding
						FactoryBinding;
					if (!Revision.IsValid()
						|| !FactoryResolver
							->TryResolveForPlanning(
								FactoryRef,
								Revision,
								Profile->ProfileId,
								FactoryBinding,
								ResolveError))
					{
						SetProductionError(
							OutError,
							ResolveError.GetCode().IsEmpty()
								? TEXT(
									"asset_data_factory_candidate_unresolved")
								: ResolveError.GetCode(),
							Profile->ProfileId,
							ResolveError.GetDetail());
						return false;
					}
					FString FactoryError;
					const FUeAgentCandidateBinding*
						Candidate =
							FactoryBinding.GetCandidateBinding();
					if (!Candidate
						|| !TryValidateAssetDataSimpleEffectFactoryBinding(
							Profile->NativeBindingId,
							Candidate->GetBindingId(),
							Revision,
							FactoryBinding,
							FactoryError))
					{
						SetProductionError(
							OutError,
							FactoryError.IsEmpty()
								? TEXT(
									"asset_data_factory_binding_invalid")
								: FactoryError,
							Profile->ProfileId);
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
					SetProductionError(
						OutError,
						TEXT(
							"asset_data_target_class_invalid"),
						Profile->ProfileId);
					return false;
				}
				const FString PackageName = Package->GetName();
				const FString ObjectPath =
					ExistingTarget
						? ExistingTarget->GetPathName()
						: PackageName
							+ TEXT(".")
							+ ObjectName.ToString();
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
				if (!TrySerializeJson(Payload, EvidenceJson))
				{
					SetProductionError(
						OutError,
						TEXT(
							"asset_data_request_evidence_invalid"),
						TargetRef);
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
					SetProductionError(
						OutError,
						TEXT(
							"asset_data_target_binding_invalid"),
						TargetRef);
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
					OutError.Code =
						TEXT("asset_data_state_request_invalid");
					return false;
				}
				if (!Deadline.CanContinue()
					|| Cancellation.IsCancelled())
				{
					OutError.Code =
						TEXT("asset_data_state_request_cancelled");
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
						ExpectedClass))
				{
					if (OutError.Code.IsEmpty())
					{
						OutError.Code =
							TEXT(
								"asset_data_state_evidence_invalid");
						OutError.SubjectId =
							Binding.GetBindingId();
					}
					return false;
				}
				if (IsMutableProductionProfile(*Profile)
					&& !TryReadSelectedFieldIds(
						Payload,
						*Profile,
						SelectedFieldIds))
				{
					OutError.Code =
						TEXT(
							"asset_data_selected_fields_invalid");
					OutError.SubjectId =
						Binding.GetBindingId();
					return false;
				}
				if (Profile->IsClone()
					&& !TryReadCloneEvidence(
						Payload,
						SourceRef,
						ExpectedSourceObjectPath,
						ExpectedSourceClass,
						ExpectedSourceRevision))
				{
					OutError.Code =
						TEXT(
							"asset_data_clone_state_evidence_invalid");
					OutError.SubjectId =
						Binding.GetBindingId();
					return false;
				}
				if (Profile->IsClone()
					&& (ExpectedClass != ExpectedSourceClass
						|| ExpectedSourceObjectPath.IsEmpty()))
				{
					OutError.Code =
						TEXT(
							"asset_data_clone_expected_class_mismatch");
					OutError.SubjectId = SourceRef;
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
						OutError.Code =
							ResolveError.GetCode().IsEmpty()
								? TEXT(
									"asset_data_existing_target_mismatch")
								: ResolveError.GetCode();
						OutError.SubjectId = TargetRef;
						OutError.Detail =
							ResolveError.GetDetail();
						return false;
					}
					if (RequiredPlane
							== EUeAgentStatePlane::AuthoringMemory)
					{
						FString ExpectedRevision;
						if (!Payload->TryGetStringField(
								TEXT("expected_revision"),
								ExpectedRevision)
							|| BuildAssetRevision(Object)
								!= ExpectedRevision)
						{
							OutError.Code =
								TEXT(
									"asset_data_target_revision_mismatch");
							OutError.SubjectId = TargetRef;
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
						OutError.Code =
							ResolveError.GetCode().IsEmpty()
								? TEXT(
									"asset_data_prestate_target_unresolved")
								: ResolveError.GetCode();
						OutError.SubjectId = TargetRef;
						OutError.Detail =
							ResolveError.GetDetail();
						return false;
					}
					Object = StaticFindObjectFast(
						UObject::StaticClass(),
						Package,
						ObjectName);
					if (Object)
					{
						OutError.Code =
							TEXT(
								"asset_data_target_already_exists");
						OutError.SubjectId = TargetRef;
						return false;
					}
				}
				else if (!Resolver->TryResolveExisting(
						TargetRef,
						Object,
						ResolveError)
					|| !IsValid(Object))
				{
					OutError.Code =
						ResolveError.GetCode().IsEmpty()
							? TEXT(
								"asset_data_created_target_missing")
							: ResolveError.GetCode();
					OutError.SubjectId = TargetRef;
					OutError.Detail = ResolveError.GetDetail();
					return false;
				}
				if (Profile->IsClone())
				{
					if (!TryResolveCloneSource(
							Resolver, SourceRef, SourceObject, ResolveError)
						|| !IsValid(SourceObject))
					{
						OutError.Code =
							ResolveError.GetCode().IsEmpty()
								? TEXT(
									"asset_data_clone_source_unresolved")
								: ResolveError.GetCode();
						OutError.SubjectId = SourceRef;
						OutError.Detail =
							ResolveError.GetDetail();
						return false;
					}
					CurrentSourceRevision =
						BuildAssetRevision(SourceObject);
					if (SourceObject->GetPathName()
							!= ExpectedSourceObjectPath
						|| SourceObject->GetClass()
								->GetPathName()
							!= ExpectedSourceClass
						|| CurrentSourceRevision.IsEmpty())
					{
						OutError.Code =
							TEXT(
								"asset_data_clone_source_mismatch");
						OutError.SubjectId = SourceRef;
						return false;
					}
					if (RequiredPlane
							== EUeAgentStatePlane::AuthoringMemory
						&& CurrentSourceRevision
							!= ExpectedSourceRevision)
					{
						OutError.Code =
							TEXT(
								"asset_data_source_revision_mismatch");
						OutError.SubjectId = SourceRef;
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
						OutError.Code = StateError.Code;
						OutError.SubjectId = SourceRef;
						OutError.Detail = StateError.Detail;
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
						OutError.Code =
							TEXT(
								"asset_data_created_target_mismatch");
						OutError.SubjectId = TargetRef;
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
								*Resolver,
								Profile->NativeMechanicId,
								Object,
								SelectedFieldIds,
								Resources,
								CurrentFields,
								DefaultFields,
								ProjectionContext,
								ProjectionError)
							|| !TryBuildMutableAssetState(
								TargetRef,
								ActualObjectPath,
								ActualClass,
								*Profile,
								CurrentFields.ToSharedRef(),
								RequiredPlane
										== EUeAgentStatePlane::
											AuthoringMemory
									? BuildAssetRevision(Object)
									: FString(),
								RequiredPlane
										== EUeAgentStatePlane::
											AuthoringMemory
									? DefaultFields
									: FUeAgentCanonicalValuePtr(),
								RequiredPlane
										== EUeAgentStatePlane::
											AuthoringMemory
									? ProjectionContext
									: FUeAgentCanonicalValuePtr(),
								Resources,
								Value,
								StateError))
						{
							OutError.Code =
								!ProjectionError.GetCode()
										.IsEmpty()
									? ProjectionError.GetCode()
									: StateError.Code;
							OutError.SubjectId = TargetRef;
							OutError.Detail =
								!ProjectionError.GetDetail()
										.IsEmpty()
									? ProjectionError.GetDetail()
									: StateError.Detail;
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
								*Profile);
						}
						if (!TryCanonicalObject(
								StateObject,
								Resources,
								Value,
								StateError))
						{
							OutError.Code = StateError.Code;
							OutError.SubjectId = TargetRef;
							OutError.Detail =
								StateError.Detail;
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
						OutError.Code = StateError.Code;
						OutError.SubjectId = TargetRef;
						OutError.Detail = StateError.Detail;
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
							OutError.Code =
								ResolveError.GetCode().IsEmpty()
									? TEXT(
										"asset_data_reference_unresolved")
									: ResolveError.GetCode();
							OutError.SubjectId =
								RequestedRef;
							OutError.Detail =
								ResolveError.GetDetail();
							return false;
						}
						FUeAgentCanonicalValuePtr ReferenceValue;
						FUeAgentCanonicalValuePtr
							ReferenceDefault;
						FUeAgentStateSemanticError
							ReferenceStateError;
						if (!TryCanonicalObject(
								BuildCloneSourceState(
									RequestedRef,
									Referenced->GetClass()
										->GetPathName(),
									BuildAssetRevision(
										Referenced)),
								Resources,
								ReferenceValue,
								ReferenceStateError)
							|| !FUeAgentCanonicalValue::TryObject(
								{},
								Resources,
								ReferenceDefault,
								ReferenceStateError))
						{
							OutError.Code =
								ReferenceStateError.Code;
							OutError.SubjectId =
								RequestedRef;
							OutError.Detail =
								ReferenceStateError.Detail;
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
					OutError.Code = StateError.Code;
					OutError.SubjectId = TargetRef;
					OutError.Detail = StateError.Detail;
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
					SetProductionError(
						OutError,
						Error.GetCode(),
						CanonicalRequest.GetRequestId(),
						Error.GetDetail());
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
					SetProductionError(
						OutError,
						TEXT(
							"asset_data_change_profile_missing"),
						CanonicalRequest.GetDescriptorIdentity()
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
						SetProductionError(
							OutError,
							TEXT(
								"asset_data_change_target_invalid"),
							Change.GetIdentity()
								.GetCanonicalKey());
						OutTargets.Reset();
						return false;
					}
				}
				return !OutTargets.IsEmpty();
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
					SetProductionError(
						OutError,
						TEXT(
							"asset_data_change_targets_missing"),
						Plan->GetRequestId());
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
						SetProductionError(
							OutError,
							TEXT(
								"asset_data_mutation_binding_invalid"),
							Changes[Index]
								.GetIdentity()
								.GetCanonicalKey());
						OutBindings.Reset();
						return false;
					}
				}
				return !OutBindings.IsEmpty();
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
					SetProductionError(
						OutError,
						TEXT(
							"asset_data_durable_target_missing"),
						Target.GetBindingId());
					return false;
				}
				OutTargets.Emplace(
					Changes[0].GetIdentity(),
					Target.GetPublicRef(),
					EAssetDurableIntent::Save,
					ERollbackGrade::DurableBackup);
				return OutTargets[0].IsValid();
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
						ExpectedClass))
				{
					SetProductionError(
						OutError,
						EvidenceError.Code.IsEmpty()
							? TEXT(
								"asset_data_expected_profile_missing")
							: EvidenceError.Code,
						Plan->GetDescriptorIdentity()
							.GetDescriptorId(),
						EvidenceError.Detail);
					return false;
				}
				if (Profile->IsClone()
					&& !TryReadCloneEvidence(
						Payload,
						SourceRef,
						SourceObjectPath,
						SourceClass,
						SourceRevision))
				{
					SetProductionError(
						OutError,
						TEXT(
							"asset_data_clone_expected_evidence_invalid"),
						Plan->GetDescriptorIdentity()
							.GetDescriptorId());
					return false;
				}
				if (Profile->IsClone()
					&& (ExpectedClass != SourceClass
						|| SourceObjectPath.IsEmpty()))
				{
					SetProductionError(
						OutError,
						TEXT(
							"asset_data_clone_expected_class_mismatch"),
						SourceRef);
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
						SetProductionError(
							OutError,
							!ProjectionError.GetCode().IsEmpty()
								? ProjectionError.GetCode()
								: StateError.Code.IsEmpty()
									? TEXT(
										"asset_data_expected_field_state_invalid")
									: StateError.Code,
							TargetRef,
							!ProjectionError.GetDetail().IsEmpty()
								? ProjectionError.GetDetail()
								: StateError.Detail);
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
								*Profile);
					if (!TryCanonicalObject(
							ExpectedState,
							Resources,
							ExpectedValue,
							StateError))
					{
						SetProductionError(
							OutError,
							StateError.Code,
							TargetRef,
							StateError.Detail);
						return false;
					}
				}
				if (!FUeAgentCanonicalValue::TryObject(
						{},
						Resources,
						DefaultValue,
						StateError))
				{
					SetProductionError(
						OutError,
						StateError.Code,
						TargetRef,
						StateError.Detail);
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
						SetProductionError(
							OutError,
							TEXT(
								"asset_data_verification_target_missing"),
							Change.GetIdentity()
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
						SetProductionError(
							OutError,
							StateError.Code,
							Change.GetIdentity()
								.GetCanonicalKey(),
							StateError.Detail);
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
						SetProductionError(
							OutError,
							TEXT(
								"asset_data_verification_target_invalid"),
							Change.GetIdentity()
								.GetCanonicalKey());
						return false;
					}
				}
				return !OutTargets.IsEmpty();
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
				|| Config.OwnerLeaseId.IsEmpty()
				|| Config.MaxQueueItems < 1)
			{
				SetError(
					OutError,
					TEXT(
						"asset_data_simple_authoring_dependencies_missing"));
				return false;
			}
			const FAssetDataProfileCatalog& Catalog =
				GetProfileCatalog();
			if (!Catalog.Error.IsEmpty()
				|| Catalog.Profiles.Num() != 53)
			{
				SetError(
					OutError,
					TEXT(
						"asset_data_simple_authoring_profile_catalog_invalid"),
					TEXT("asset_data"),
					Catalog.Error);
				return false;
			}
			int32 CreateCount = 0;
			int32 StaticCreateCount = 0;
			int32 FactoryCreateCount = 0;
			int32 CloneCount = 0;
			for (const FAssetDataProfile& Profile
				: Catalog.Profiles)
			{
				CreateCount += Profile.IsCreate() ? 1 : 0;
				StaticCreateCount +=
					Profile.UsesStaticCreateClass() ? 1 : 0;
				FactoryCreateCount +=
					Profile.UsesFactoryCandidate() ? 1 : 0;
				CloneCount +=
					Profile.NativeOperationType
							== TEXT("asset.object.clone")
						? 1
						: 0;
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
					SetError(
						OutError,
						TEXT(
							"asset_data_simple_authoring_descriptor_missing"),
						Profile.DescriptorId);
					return false;
				}
			}
			if (CreateCount != 23
				|| StaticCreateCount != 19
				|| FactoryCreateCount != 4
				|| CloneCount != 1
				|| Catalog.Profiles.Num()
					- CreateCount
					- CloneCount
					!= 29)
			{
				SetError(
					OutError,
					TEXT(
						"asset_data_simple_authoring_profile_distribution_invalid"),
					TEXT("asset_data"));
				return false;
			}
			return true;
		}
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
			SetError(
				OutError,
				NativeError.GetCode().IsEmpty()
					? TEXT("asset_data_native_hook_missing")
					: NativeError.GetCode(),
				TEXT("asset_data"),
				NativeError.GetDetail());
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
		ProviderConfig.MaxQueueItems = Config.MaxQueueItems;
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
		ProviderConfig.PersistedStateReadPort =
			MakeShared<FAssetDataStateReadPort>(
				PersistedStatePortId,
				EUeAgentStatePlane::Persisted,
				PersistedResolver);
		ProviderConfig.AbsenceOracle = Config.AbsenceOracle;
		ProviderConfig.ReferenceOracle = Config.ReferenceOracle;

		FUeAgentSimpleAuthoringProductionError Error;
		if (!FUeAgentSimpleAuthoringProductionProvider::TryCreate(
				ProviderConfig,
				OutProvider,
				Error))
		{
			SetError(
				OutError,
				Error.Code,
				Error.SubjectId,
				Error.Detail);
			return false;
		}
		return OutProvider.IsValid();
	}
}
