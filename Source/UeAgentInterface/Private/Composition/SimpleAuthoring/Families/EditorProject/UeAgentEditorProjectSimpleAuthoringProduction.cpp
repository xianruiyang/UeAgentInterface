#include "Composition/SimpleAuthoring/Families/EditorProject/UeAgentEditorProjectSimpleAuthoringProduction.h"

#include "Composition/SimpleAuthoring/Families/EditorProject/UeAgentEditorProjectSimpleAuthoringStateMechanics.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Composition::SimpleAuthoring::EditorProject
{
	namespace
	{
		using namespace UE::UAI::AssetDurable;
		using namespace UE::UAI::ContextResource;
		using namespace UE::UAI::Contract;
		using namespace UE::UAI::Discovery;
		using namespace UE::UAI::DomainAdapters::
			EditorProjectSimpleEffect;
		using namespace UE::UAI::DomainAdapters::GenericEffect;
		using namespace UE::UAI::ExecutionMutation;
		using namespace UE::UAI::PlanningPreflight;
		using namespace UE::UAI::StateSemantic;
		using namespace UE::UAI::Verification;

		constexpr const TCHAR* PreStatePortId =
			TEXT("editor-project.authoring-memory.v1");
		constexpr const TCHAR* PersistedStatePortId =
			TEXT("editor-project.independent-readback.v1");
		constexpr const TCHAR* RequestEvidencePrefix =
			TEXT("editor-project-request-v1:");
		constexpr const TCHAR* EvidenceDurableKind =
			TEXT("_uai_durable_kind");
		constexpr const TCHAR* EvidenceDurableLocation =
			TEXT("_uai_durable_location");

		using FEditorProjectProfile =
			FUeAgentEditorProjectSimpleEffectProductionProfile;

		struct FEditorProjectProfileCatalog
		{
			FEditorProjectProfileCatalog()
			{
				TryGetEditorProjectSimpleEffectProductionProfiles(
					Profiles,
					Error);
			}

			TArray<FEditorProjectProfile> Profiles;
			FString Error;
		};

		const FEditorProjectProfileCatalog& GetProfileCatalog()
		{
			static const FEditorProjectProfileCatalog Catalog;
			return Catalog;
		}

		const FEditorProjectProfile* FindProfile(
			const FUeAgentDescriptorIdentity& Identity)
		{
			for (const FEditorProjectProfile& Profile
				: GetProfileCatalog().Profiles)
			{
				if (Identity.GetDescriptorId() == Profile.DescriptorId
					&& Identity.GetVersion() == Profile.DescriptorVersion
					&& Identity.GetDescriptorHash()
						== Profile.DescriptorHash)
				{
					return &Profile;
				}
			}
			return nullptr;
		}

		void SetError(
			FUeAgentEditorProjectSimpleAuthoringError& OutError,
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

		bool TryParseEvidence(
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
					TEXT("editor_project_request_evidence_invalid");
				OutError.SubjectId = Binding.GetBindingId();
				return false;
			}
			return true;
		}

		bool TryReadEvidence(
			const TSharedPtr<FJsonObject>& Payload,
			FString& OutTargetRef,
			FString& OutExpectedRevision,
			EDurableArtifactKind& OutKind,
			FString& OutLocation)
		{
			OutTargetRef.Reset();
			OutExpectedRevision.Reset();
			OutLocation.Reset();
			FString Kind;
			if (!Payload.IsValid()
				|| !Payload->TryGetStringField(
					TEXT("target_ref"),
					OutTargetRef)
				|| !Payload->TryGetStringField(
					TEXT("expected_revision"),
					OutExpectedRevision)
				|| !Payload->TryGetStringField(
					EvidenceDurableKind,
					Kind)
				|| !Payload->TryGetStringField(
					EvidenceDurableLocation,
					OutLocation)
				|| OutTargetRef.IsEmpty()
				|| OutExpectedRevision.IsEmpty()
				|| OutLocation.IsEmpty())
			{
				return false;
			}
			if (Kind == TEXT("package"))
			{
				OutKind = EDurableArtifactKind::Package;
				return true;
			}
			if (Kind == TEXT("project_config"))
			{
				OutKind = EDurableArtifactKind::ProjectConfig;
				return true;
			}
			return false;
		}

		bool TryCanonicalJson(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			OutValue.Reset();
			FUeAgentStateSemanticError Error;
			if (!JsonValue.IsValid())
			{
				OutError = FExecutionMutationError(
					TEXT("editor_project_state_value_missing"));
				return false;
			}
			switch (JsonValue->Type)
			{
			case EJson::Null:
				if (FUeAgentCanonicalValue::TryNull(
						Resources,
						OutValue,
						Error))
				{
					return true;
				}
				break;
			case EJson::Boolean:
				if (FUeAgentCanonicalValue::TryBoolean(
						JsonValue->AsBool(),
						Resources,
						OutValue,
						Error))
				{
					return true;
				}
				break;
			case EJson::String:
				if (FUeAgentCanonicalValue::TryString(
						JsonValue->AsString(),
						Resources,
						OutValue,
						Error))
				{
					return true;
				}
				break;
			case EJson::Number:
				{
					const double Number = JsonValue->AsNumber();
					const bool bInteger = FMath::IsNearlyEqual(
							Number,
							FMath::FloorToDouble(Number))
						&& Number >= static_cast<double>(MIN_int64)
						&& Number <= static_cast<double>(MAX_int64);
					if (bInteger
						? FUeAgentCanonicalValue::TryInteger(
							FString::Printf(
								TEXT("%lld"),
								static_cast<long long>(Number)),
							Resources,
							OutValue,
							Error)
						: FUeAgentCanonicalValue::TryDecimal(
							FString::SanitizeFloat(Number),
							Resources,
							OutValue,
							Error))
					{
						return true;
					}
					break;
				}
			case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					for (const TSharedPtr<FJsonValue>& Item
						: JsonValue->AsArray())
					{
						FUeAgentCanonicalValuePtr Canonical;
						if (!TryCanonicalJson(
								Item,
								Resources,
								Canonical,
								OutError))
						{
							return false;
						}
						Items.Add(Canonical.ToSharedRef());
					}
					if (FUeAgentCanonicalValue::TryArray(
							Items,
							Resources,
							OutValue,
							Error))
					{
						return true;
					}
					break;
				}
			case EJson::Object:
				{
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
						: JsonValue->AsObject()->Values)
					{
						FUeAgentCanonicalValuePtr Canonical;
						if (!TryCanonicalJson(
								Pair.Value,
								Resources,
								Canonical,
								OutError))
						{
							return false;
						}
						Items.Emplace(Pair.Key, Canonical.ToSharedRef());
					}
					if (FUeAgentCanonicalValue::TryObject(
							Items,
							Resources,
							OutValue,
							Error))
					{
						return true;
					}
					break;
				}
			default:
				break;
			}
			OutError = FExecutionMutationError(Error.Code, Error.Detail);
			return false;
		}

		FUeAgentCanonicalType EditorProjectStateType(
			const FString& TypeId = TEXT("editor_project_current_state"))
		{
			return FUeAgentCanonicalType(
				TypeId,
				EUeAgentValueKind::Object,
				false,
				TEXT("object"));
		}

		FUeAgentDefaultValue EditorProjectStateDefault(
			const FUeAgentCanonicalValueRef& Value)
		{
			return FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::Schema,
				TEXT("editor-project:empty-default"),
				TEXT("EditorProject state has no implicit target"),
				Value);
		}

		FUeAgentStateDetails EditorProjectStateDetails(
			const FEditorProjectProfile& Profile)
		{
			return FUeAgentStateDetails(
				Profile.CapabilityId,
				TEXT(""),
				{
					TEXT("EditorProject"),
					TEXT("SimpleAuthoring"),
					Profile.NativeOperationType,
				},
				true,
				TEXT("available"),
				{});
		}

		class FEditorProjectTargetResolver final
			: public IUeAgentSimpleAuthoringTargetResolver
		{
		public:
			FEditorProjectTargetResolver(
				TSharedRef<
					IUeAgentEditorProjectSimpleEffectTargetResolver,
					ESPMode::ThreadSafe> InTargetResolver,
				TSharedRef<
					IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver,
					ESPMode::ThreadSafe> InDurableBindingResolver)
				: TargetResolver(MoveTemp(InTargetResolver))
				, DurableBindingResolver(MoveTemp(InDurableBindingResolver))
			{
			}

			bool TryResolveTarget(
				const FUeAgentOperationRequest& CanonicalRequest,
				const FUeAgentExecutionContextRef& Context,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				TOptional<FUeAgentCandidateBinding>& OutTarget,
				FUeAgentSimpleAuthoringProductionError& OutError) override
			{
				OutTarget.Reset();
				const FEditorProjectProfile* Profile =
					FindProfile(CanonicalRequest.GetDescriptorIdentity());
				TSharedPtr<FJsonObject> Payload;
				FString TargetRef;
				FString ExpectedRevision;
				if (!Profile
					|| !TryParseJson(
						CanonicalRequest.GetPayload().GetJsonValue(),
						Payload)
					|| !Payload->TryGetStringField(
						TEXT("target_ref"),
						TargetRef)
					|| !Payload->TryGetStringField(
						TEXT("expected_revision"),
						ExpectedRevision)
					|| TargetRef.IsEmpty()
					|| ExpectedRevision.IsEmpty())
				{
					SetProductionError(
						OutError,
						TEXT("editor_project_production_profile_missing"),
						CanonicalRequest.GetDescriptorIdentity()
							.GetDescriptorId());
					return false;
				}
				if (!Deadline.CanContinue() || Cancellation.IsCancelled())
				{
					SetProductionError(
						OutError,
						TEXT("editor_project_target_resolution_cancelled"),
						TargetRef);
					return false;
				}
				FUeAgentEditorProjectSimpleEffectResolvedTarget Target;
				FUeAgentEditorProjectSimpleAuthoringDurableBinding Binding;
				FExecutionMutationError Error;
				if (!TargetResolver->TryResolveTarget(
						TargetRef,
						Target,
						Error)
					|| !Target.IsValid()
					|| Target.GetCanonicalRef() != TargetRef
					|| Target.GetProfileId() != Profile->ProfileId
					|| Target.GetRevision() != ExpectedRevision)
				{
					SetProductionError(
						OutError,
						Error.GetCode().IsEmpty()
							? TEXT("editor_project_target_revision_mismatch")
							: Error.GetCode(),
						TargetRef,
						Error.GetDetail());
					return false;
				}
				if (!DurableBindingResolver->TryResolveDurableBinding(
						Target,
						Binding,
						Error)
					|| !Binding.IsValid())
				{
					SetProductionError(
						OutError,
						Error.GetCode().IsEmpty()
							? TEXT("editor_project_durable_binding_missing")
							: Error.GetCode(),
						TargetRef,
						Error.GetDetail());
					return false;
				}
				const EDurableArtifactKind RequiredKind =
					Profile->ProfileId == TEXT("string_table")
						? EDurableArtifactKind::Package
						: EDurableArtifactKind::ProjectConfig;
				if (Binding.GetArtifactKind() != RequiredKind)
				{
					SetProductionError(
						OutError,
						TEXT("editor_project_durable_binding_kind_invalid"),
						TargetRef);
					return false;
				}
				Payload->SetStringField(
					EvidenceDurableKind,
					RequiredKind == EDurableArtifactKind::Package
						? TEXT("package")
						: TEXT("project_config"));
				Payload->SetStringField(
					EvidenceDurableLocation,
					Binding.GetDurableLocation());
				FString Evidence;
				if (!TrySerializeJson(Payload, Evidence))
				{
					SetProductionError(
						OutError,
						TEXT("editor_project_request_evidence_invalid"),
						TargetRef);
					return false;
				}
				const FUeAgentCanonicalIdentity Identity(
					TEXT("uai.editor_project:") + TargetRef,
					Profile->NativeOperationType,
					Context.GetProjectInstanceId(),
					TargetRef,
					Profile->ProfileId,
					Context.GetContextRevision());
				OutTarget.Emplace(
					TEXT("editor-project:")
						+ CanonicalRequest.GetRequestId(),
					FUeAgentCandidateSummary(
						Identity,
						CanonicalRequest.GetDescriptorIdentity(),
						TEXT("editor_project"),
						TargetRef,
						TEXT("exact EditorProject typed target"),
						TArray<FString>{
							TEXT("EditorProject"),
							TEXT("SimpleAuthoring"),
						}),
					FUeAgentCatalogRevision(
						TEXT("editor-project-exact-target"),
						Context.GetContextRevision(),
						1),
					Binding.GetDurableLocation(),
					PersistedStatePortId,
					TArray<EUeAgentCandidateOperation>{
						EUeAgentCandidateOperation::Update,
					},
					FString(RequestEvidencePrefix) + Evidence);
				if (!OutTarget->IsValid())
				{
					SetProductionError(
						OutError,
						TEXT("editor_project_target_binding_invalid"),
						TargetRef);
					OutTarget.Reset();
					return false;
				}
				return true;
			}

		private:
			TSharedRef<
				IUeAgentEditorProjectSimpleEffectTargetResolver,
				ESPMode::ThreadSafe> TargetResolver;
			TSharedRef<
				IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver,
				ESPMode::ThreadSafe> DurableBindingResolver;
		};

		class FEditorProjectStateReadPort final
			: public IUeAgentStateReadPort
		{
		public:
			FEditorProjectStateReadPort(
				FString InPortId,
				const EUeAgentStatePlane InRequiredPlane,
				TSharedRef<
					IUeAgentEditorProjectSimpleEffectTargetResolver,
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
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentDiscoveryError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = FUeAgentDiscoveryError();
				const FEditorProjectProfile* Profile =
					Request.IsValid()
						? FindProfile(
							Request.GetBinding().GetSummary()
								.GetDescriptorIdentity())
						: nullptr;
				if (!Profile || Request.GetStatePlane() != RequiredPlane)
				{
					OutError.Code =
						TEXT("editor_project_state_request_invalid");
					return false;
				}
				if (!Deadline.CanContinue() || Cancellation.IsCancelled())
				{
					OutError.Code =
						TEXT("editor_project_state_request_cancelled");
					return false;
				}
				const FUeAgentCandidateBinding& Binding =
					Request.GetBinding();
				TSharedPtr<FJsonObject> Payload;
				FString TargetRef;
				FString ExpectedRevision;
				FString DurableLocation;
				EDurableArtifactKind DurableKind =
					EDurableArtifactKind::Package;
				if (!TryParseEvidence(Binding, Payload, OutError)
					|| !TryReadEvidence(
						Payload,
						TargetRef,
						ExpectedRevision,
						DurableKind,
						DurableLocation))
				{
					if (OutError.Code.IsEmpty())
					{
						OutError.Code =
							TEXT("editor_project_state_evidence_invalid");
						OutError.SubjectId = Binding.GetBindingId();
					}
					return false;
				}
				FUeAgentEditorProjectSimpleEffectResolvedTarget Target;
				FExecutionMutationError ResolveError;
				if (!Resolver->TryResolveTarget(
						TargetRef,
						Target,
						ResolveError)
					|| !Target.IsValid()
					|| Target.GetCanonicalRef() != TargetRef
					|| Target.GetProfileId() != Profile->ProfileId
					|| (RequiredPlane == EUeAgentStatePlane::AuthoringMemory
						&& Target.GetRevision() != ExpectedRevision))
				{
					OutError.Code = ResolveError.GetCode().IsEmpty()
						? TEXT("editor_project_state_target_mismatch")
						: ResolveError.GetCode();
					OutError.SubjectId = TargetRef;
					OutError.Detail = ResolveError.GetDetail();
					return false;
				}
				FUeAgentCanonicalValuePtr State;
				TArray<Private::FEditorProjectDependencyState> Dependencies;
				if (!Private::TryBuildEditorProjectTargetState(
						*Resolver,
						Target,
						Profile->ProfileId,
						Payload,
						RequiredPlane
							== EUeAgentStatePlane::AuthoringMemory,
						Resources,
						State,
						Dependencies,
						ResolveError))
				{
					OutError.Code = ResolveError.GetCode();
					OutError.SubjectId = TargetRef;
					OutError.Detail = ResolveError.GetDetail();
					return false;
				}
				FUeAgentCanonicalValuePtr Empty;
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryObject(
						{},
						Resources,
						Empty,
						StateError))
				{
					OutError.Code = StateError.Code;
					OutError.SubjectId = TargetRef;
					OutError.Detail = StateError.Detail;
					return false;
				}
				const FUeAgentStateRevision Revision(
					PortId,
					Binding.GetCatalogRevision().GetGeneration(),
					Binding.GetCatalogRevision().GetSequence());
				TArray<FUeAgentStateEntry> Entries;
				Entries.Emplace(
					Binding.GetSummary().GetIdentity(),
					EditorProjectStateType(),
					State.ToSharedRef(),
					EditorProjectStateDefault(Empty.ToSharedRef()),
					EditorProjectStateDetails(*Profile),
					Revision,
					EUeAgentAuthoredPresence::Present,
					EUeAgentOverrideState::Overridden);
				for (const Private::FEditorProjectDependencyState& Dependency
					: Dependencies)
				{
					const TSharedPtr<FJsonObject> DependencyObject =
						MakeShared<FJsonObject>();
					DependencyObject->SetStringField(
						TEXT("target_ref"),
						Dependency.TargetRef);
					DependencyObject->SetStringField(
						TEXT("target_revision"),
						Dependency.TargetRevision);
					DependencyObject->SetStringField(
						TEXT("profile_id"),
						Dependency.ProfileId);
					DependencyObject->SetStringField(
						TEXT("type_id"),
						Dependency.TypeId);
					FUeAgentCanonicalValuePtr DependencyValue;
					if (!Dependency.IsValid()
						|| !TryCanonicalJson(
							MakeShared<FJsonValueObject>(DependencyObject),
							Resources,
							DependencyValue,
							ResolveError))
					{
						OutError.Code = ResolveError.GetCode().IsEmpty()
							? TEXT("editor_project_dependency_state_invalid")
							: ResolveError.GetCode();
						OutError.SubjectId = Dependency.TargetRef;
						OutError.Detail = ResolveError.GetDetail();
						return false;
					}
					Entries.Emplace(
						FUeAgentCanonicalIdentity(
							TEXT("uai.editor_project.dependency:")
								+ Dependency.TargetRef,
							TEXT("editor_project.dependency.read"),
							Binding.GetSummary().GetIdentity().GetOwnerId(),
							Dependency.TargetRef,
							Dependency.ProfileId,
							Binding.GetSummary().GetIdentity().GetGeneration()),
						EditorProjectStateType(Dependency.TypeId),
						DependencyValue.ToSharedRef(),
						EditorProjectStateDefault(Empty.ToSharedRef()),
						EditorProjectStateDetails(*Profile),
						Revision,
						EUeAgentAuthoredPresence::Present,
						EUeAgentOverrideState::Overridden);
				}
				FUeAgentCanonicalSnapshotPtr Snapshot;
				if (!FUeAgentCanonicalSnapshot::TryCreate(
						TEXT("snapshot:editor-project:") + PortId
							+ TEXT(":") + Binding.GetBindingId(),
						RequiredPlane,
						Revision,
						FUeAgentTemporalPoint(
							TEXT("editor-project-operation"),
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
				IUeAgentEditorProjectSimpleEffectTargetResolver,
				ESPMode::ThreadSafe> Resolver;
		};

		class FEditorProjectDraftBuilder final
			: public IUeAgentSimpleAuthoringDraftBuilder
		{
		public:
			bool TryBuildDraft(
				const FUeAgentOperationRequest& CanonicalRequest,
				const FUeAgentExecutionContextRef& Context,
				const FUeAgentCanonicalSnapshotRef& PreState,
				FUeAgentResourceTracker& Resources,
				TOptional<FChangeSetDraft>& OutDraft,
				FUeAgentSimpleAuthoringProductionError& OutError) override
			{
				FExecutionMutationError Error;
				if (!TryBuildEditorProjectSimpleEffectChangeSetDraft(
						CanonicalRequest,
						Context,
						PreState,
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
		};

		class FEditorProjectChangeTargetProjector final
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
				FUeAgentSimpleAuthoringProductionError& OutError) override
			{
				OutTargets.Reset();
				const TArray<FChangeDraft>& Changes = Draft.GetChanges();
				for (int32 Index = 0; Index < Changes.Num(); ++Index)
				{
					const FChangeDraft& Change = Changes[Index];
					OutTargets.Emplace(
						ResolvedTarget.GetBindingId() + TEXT(":change:")
							+ FString::FromInt(Index),
						FUeAgentCandidateSummary(
							Change.GetIdentity(),
							CanonicalRequest.GetDescriptorIdentity(),
							TEXT("editor_project"),
							Change.GetIdentity().GetSelector(),
							TEXT("exact EditorProject change target"),
							TArray<FString>{
								TEXT("EditorProject"),
								TEXT("SimpleAuthoring"),
							}),
						ResolvedTarget.GetCatalogRevision(),
						ResolvedTarget.GetPublicRef(),
						ResolvedTarget.GetReadPortId(),
						TArray<EUeAgentCandidateOperation>{
							EUeAgentCandidateOperation::Update,
						},
						ResolvedTarget.GetBindingEvidence());
					if (!OutTargets.Last().IsValid())
					{
						SetProductionError(
							OutError,
							TEXT("editor_project_change_target_invalid"),
							Change.GetIdentity().GetCanonicalKey());
						OutTargets.Reset();
						return false;
					}
				}
				return !OutTargets.IsEmpty();
			}
		};

		class FEditorProjectMutationBindingProjector final
			: public IUeAgentSimpleAuthoringMutationBindingProjector
		{
		public:
			bool TryProjectMutationBindings(
				const FPlanningPreflightPlanRef& Plan,
				const TArray<FUeAgentCandidateBinding>& Targets,
				const FUeAgentCanonicalSnapshotRef& PreState,
				const FUeAgentExecutionContextRef& Context,
				FUeAgentResourceTracker& Resources,
				TArray<FUeAgentGenericEffectBinding>& OutBindings,
				FUeAgentSimpleAuthoringProductionError& OutError) override
			{
				OutBindings.Reset();
				const TArray<FPlannedChange>& Changes =
					Plan->GetChangeSet().GetChanges();
				if (Changes.Num() != Targets.Num())
				{
					SetProductionError(
						OutError,
						TEXT("editor_project_change_targets_missing"),
						Plan->GetRequestId());
					return false;
				}
				for (int32 Index = 0; Index < Changes.Num(); ++Index)
				{
					OutBindings.Emplace(
						Targets[Index],
						TEXT("UAI.EditorProject:CanonicalTypedUpdate"),
						FUeAgentCanonicalType(
							TEXT("editor_project_typed_update"),
							EUeAgentValueKind::Object,
							false,
							TEXT("object")),
						TEXT("editor-project:typed-native-binding"));
					if (Targets[Index].GetSummary().GetIdentity()
							!= Changes[Index].GetIdentity()
						|| !OutBindings.Last().IsValid())
					{
						SetProductionError(
							OutError,
							TEXT("editor_project_mutation_binding_invalid"),
							Changes[Index].GetIdentity().GetCanonicalKey());
						OutBindings.Reset();
						return false;
					}
				}
				return !OutBindings.IsEmpty();
			}
		};

		class FEditorProjectDurableTargetProjector final
			: public IUeAgentSimpleAuthoringDurableTargetProjector
		{
		public:
			bool TryProjectDurableTargets(
				const FPlanningPreflightPlanRef& Plan,
				const FUeAgentCandidateBinding& Target,
				const FUeAgentCanonicalSnapshotRef& PreState,
				FUeAgentResourceTracker& Resources,
				TArray<FAssetDurableTarget>& OutTargets,
				FUeAgentSimpleAuthoringProductionError& OutError) override
			{
				OutTargets.Reset();
				TSharedPtr<FJsonObject> Payload;
				FUeAgentDiscoveryError EvidenceError;
				FString TargetRef;
				FString ExpectedRevision;
				FString DurableLocation;
				EDurableArtifactKind Kind = EDurableArtifactKind::Package;
				const TArray<FPlannedChange>& Changes =
					Plan->GetChangeSet().GetChanges();
				if (Changes.Num() != 1
					|| !TryParseEvidence(Target, Payload, EvidenceError)
					|| !TryReadEvidence(
						Payload,
						TargetRef,
						ExpectedRevision,
						Kind,
						DurableLocation))
				{
					SetProductionError(
						OutError,
						EvidenceError.Code.IsEmpty()
							? TEXT("editor_project_durable_target_missing")
							: EvidenceError.Code,
						Target.GetBindingId(),
						EvidenceError.Detail);
					return false;
				}
				if (Kind == EDurableArtifactKind::Package)
				{
					OutTargets.Emplace(
						Changes[0].GetIdentity(),
						DurableLocation,
						EPackageHeaderKind::Asset,
						EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup);
				}
				else
				{
					OutTargets.Emplace(
						Changes[0].GetIdentity(),
						Kind,
						DurableLocation,
						EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup);
				}
				return OutTargets[0].IsValid();
			}
		};

		class FEditorProjectExpectedStateProjector final
			: public IUeAgentSimpleAuthoringExpectedStateProjector
		{
		public:
			bool TryProjectExpectedState(
				const FPlanningPreflightPlanRef& Plan,
				const FUeAgentCandidateBinding& ResolvedTarget,
				const TArray<FUeAgentCandidateBinding>& ChangeTargets,
				const FUeAgentCanonicalSnapshotRef& PreState,
				const FDurableInventory& Inventory,
				FUeAgentResourceTracker& Resources,
				TArray<FVerificationTarget>& OutTargets,
				FUeAgentSimpleAuthoringProductionError& OutError) override
			{
				OutTargets.Reset();
				const FEditorProjectProfile* Profile =
					FindProfile(Plan->GetDescriptorIdentity());
				const TArray<FPlannedChange>& Changes =
					Plan->GetChangeSet().GetChanges();
				if (!Profile || Changes.Num() != 1
					|| !Changes[0].GetBefore().IsValid()
					|| !Changes[0].GetAfter().IsValid())
				{
					SetProductionError(
						OutError,
						TEXT("editor_project_expected_change_invalid"),
						Plan->GetRequestId());
					return false;
				}
				const FUeAgentCandidateBinding* ChangeTarget =
					ChangeTargets.FindByPredicate(
						[&Changes](const FUeAgentCandidateBinding& Candidate)
						{
							return Candidate.GetSummary().GetIdentity()
								== Changes[0].GetIdentity();
						});
				FUeAgentCanonicalValuePtr ExpectedValue;
				FExecutionMutationError ProjectionError;
				if (!ChangeTarget
					|| !Private::TryProjectEditorProjectExpectedState(
						*Changes[0].GetBefore(),
						*Changes[0].GetAfter(),
						Resources,
						ExpectedValue,
						ProjectionError))
				{
					SetProductionError(
						OutError,
						ProjectionError.GetCode().IsEmpty()
							? TEXT("editor_project_verification_target_missing")
							: ProjectionError.GetCode(),
						Changes[0].GetIdentity().GetCanonicalKey(),
						ProjectionError.GetDetail());
					return false;
				}
				FUeAgentCanonicalValuePtr Empty;
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryObject(
						{},
						Resources,
						Empty,
						StateError))
				{
					SetProductionError(
						OutError,
						StateError.Code,
						Plan->GetRequestId(),
						StateError.Detail);
					return false;
				}
				const FUeAgentStateRevision Revision(
					TEXT("editor-project-operation-expected"),
					Plan->GetFingerprint(),
					1);
				TArray<FUeAgentStateEntry> Entries;
				Entries.Emplace(
					Changes[0].GetIdentity(),
					EditorProjectStateType(),
					ExpectedValue.ToSharedRef(),
					EditorProjectStateDefault(Empty.ToSharedRef()),
					EditorProjectStateDetails(*Profile),
					Revision,
					EUeAgentAuthoredPresence::Present,
					EUeAgentOverrideState::Overridden);
				FUeAgentCanonicalSnapshotPtr Expected;
				if (!FUeAgentCanonicalSnapshot::TryCreateExpected(
						TEXT("snapshot:editor-project:expected:")
							+ Plan->GetRequestId(),
						EUeAgentStatePlane::Persisted,
						Revision,
						FUeAgentTemporalPoint(
							TEXT("editor-project-operation"),
							TEXT("1"),
							TEXT("1"),
							TEXT("1"),
							TEXT("expected")),
						FUeAgentStateProvenance(
							TEXT("operation_expected_state"),
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
						Plan->GetRequestId(),
						StateError.Detail);
					return false;
				}
				TArray<FString> ArtifactIds;
				for (const FDurableArtifact& Artifact : Inventory.GetArtifacts())
				{
					ArtifactIds.Add(Artifact.GetArtifactId());
				}
				OutTargets.Emplace(
					Changes[0].GetIdentity(),
					*ChangeTarget,
					EVerificationOracleKind::Persisted,
					Expected,
					ArtifactIds,
					TArray<FString>());
				return OutTargets.Last().IsValid();
			}
		};

		class FEditorProjectUeHook final
			: public IUeAgentGenericEffectUeHook
		{
		public:
			explicit FEditorProjectUeHook(
				TSharedRef<
					IUeAgentEditorProjectSimpleEffectNativeHook,
					ESPMode::ThreadSafe> InNativeHook)
				: NativeHook(MoveTemp(InNativeHook))
			{
			}

			bool TryApply(
				const FExecutionMutationRequest& Request,
				const FLockFenceLease& Lease,
				const TArray<FUeAgentGenericEffectBinding>& Bindings,
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
				IUeAgentEditorProjectSimpleEffectNativeHook,
				ESPMode::ThreadSafe> NativeHook;
		};

		class FEditorProjectConfigStageSource final
			: public IProjectConfigDurableStageSource
		{
		public:
			FEditorProjectConfigStageSource(
				TSharedRef<
					IUeAgentEditorProjectSimpleEffectTargetResolver,
					ESPMode::ThreadSafe> InTargetResolver,
				TSharedRef<
					IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver,
					ESPMode::ThreadSafe> InDurableBindingResolver)
				: TargetResolver(MoveTemp(InTargetResolver))
				, DurableBindingResolver(MoveTemp(InDurableBindingResolver))
			{
			}

			bool TryWriteStagedArtifact(
				const FAssetDurableTarget& Target,
				const FString& StagedPath,
				FUeAgentResourceTracker& Resources,
				FAssetDurableError& OutError) override
			{
				OutError = FAssetDurableError();
				if (!Target.IsValid()
					|| Target.GetArtifactKind()
						!= EDurableArtifactKind::ProjectConfig)
				{
					OutError = FAssetDurableError(
						TEXT("editor_project_config_stage_target_kind_invalid"));
					return false;
				}
				FUeAgentEditorProjectSimpleEffectResolvedTarget Resolved;
				FUeAgentEditorProjectSimpleAuthoringDurableBinding Binding;
				FExecutionMutationError Error;
				if (!TargetResolver->TryResolveTarget(
						Target.GetIdentity().GetSelector(),
						Resolved,
						Error)
					|| !Resolved.IsValid()
					|| !DurableBindingResolver->TryResolveDurableBinding(
						Resolved,
						Binding,
						Error)
					|| !Binding.IsValid())
				{
					OutError = FAssetDurableError(
						Error.GetCode().IsEmpty()
							? TEXT("editor_project_config_stage_target_unresolved")
							: Error.GetCode(),
						Error.GetDetail());
					return false;
				}
				if (Binding.GetArtifactKind()
						!= EDurableArtifactKind::ProjectConfig
					|| Binding.GetDurableLocation()
						!= Target.GetDurableLocation())
				{
					OutError = FAssetDurableError(
						TEXT("editor_project_config_stage_binding_mismatch"),
						Target.GetDurableLocation());
					return false;
				}
				if (!Private::TryWriteEditorProjectStagedConfig(
						Resolved,
						StagedPath,
						Error))
				{
					OutError = FAssetDurableError(
						Error.GetCode(),
						Error.GetDetail());
					return false;
				}
				return true;
			}

		private:
			TSharedRef<
				IUeAgentEditorProjectSimpleEffectTargetResolver,
				ESPMode::ThreadSafe> TargetResolver;
			TSharedRef<
				IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver,
				ESPMode::ThreadSafe> DurableBindingResolver;
		};

		bool ValidateConfig(
			const FUeAgentEditorProjectSimpleAuthoringProviderConfig& Config,
			FUeAgentEditorProjectSimpleAuthoringError& OutError)
		{
			if (!Config.DescriptorRegistry.IsValid()
				|| !Config.MutationTargetResolver.IsValid()
				|| !Config.PreStateTargetResolver.IsValid()
				|| !Config.PersistedTargetResolver.IsValid()
				|| Config.PreStateTargetResolver
					== Config.PersistedTargetResolver
				|| !Config.DurableBindingResolver.IsValid()
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
					TEXT("editor_project_simple_authoring_dependencies_missing"));
				return false;
			}
			const FEditorProjectProfileCatalog& Catalog =
				GetProfileCatalog();
			int32 FieldCount = 0;
			if (!Catalog.Error.IsEmpty() || Catalog.Profiles.Num() != 7)
			{
				SetError(
					OutError,
					TEXT("editor_project_simple_authoring_profile_catalog_invalid"),
					TEXT("editor_project"),
					Catalog.Error);
				return false;
			}
			for (const FEditorProjectProfile& Profile : Catalog.Profiles)
			{
				FieldCount += Profile.FieldIds.Num();
				const FUeAgentOperationDescriptor* Descriptor =
					Config.DescriptorRegistry->FindByDescriptorId(
						Profile.DescriptorId);
				if (!Profile.IsValid()
					|| !Descriptor
					|| Descriptor->GetCapabilityId() != Profile.CapabilityId
					|| Descriptor->GetDomainId() != TEXT("editor_project")
					|| Descriptor->GetIdentity().GetVersion()
						!= Profile.DescriptorVersion
					|| Descriptor->GetIdentity().GetDescriptorHash()
						!= Profile.DescriptorHash)
				{
					SetError(
						OutError,
						TEXT("editor_project_simple_authoring_descriptor_missing"),
						Profile.DescriptorId);
					return false;
				}
			}
			if (FieldCount != 11)
			{
				SetError(
					OutError,
					TEXT("editor_project_simple_authoring_field_count_invalid"),
					TEXT("editor_project"));
				return false;
			}
			return true;
		}
	}

	FUeAgentEditorProjectSimpleAuthoringDurableBinding::
		FUeAgentEditorProjectSimpleAuthoringDurableBinding(
			const EDurableArtifactKind InArtifactKind,
			FString InDurableLocation)
		: ArtifactKind(InArtifactKind)
		, DurableLocation(MoveTemp(InDurableLocation))
		, bValid(!DurableLocation.IsEmpty())
	{
	}

	EDurableArtifactKind
	FUeAgentEditorProjectSimpleAuthoringDurableBinding::
		GetArtifactKind() const
	{
		return ArtifactKind;
	}

	const FString&
	FUeAgentEditorProjectSimpleAuthoringDurableBinding::
		GetDurableLocation() const
	{
		return DurableLocation;
	}

	bool FUeAgentEditorProjectSimpleAuthoringDurableBinding::IsValid() const
	{
		return bValid;
	}

	bool FUeAgentEditorProjectConfigStageSourceFactory::TryCreate(
		TSharedPtr<
			IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe> TargetResolver,
		TSharedPtr<
			IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver,
			ESPMode::ThreadSafe> DurableBindingResolver,
		TSharedPtr<IProjectConfigDurableStageSource>& OutStageSource,
		FUeAgentEditorProjectSimpleAuthoringError& OutError)
	{
		OutStageSource.Reset();
		OutError = FUeAgentEditorProjectSimpleAuthoringError();
		if (!TargetResolver.IsValid() || !DurableBindingResolver.IsValid())
		{
			SetError(
				OutError,
				TEXT("editor_project_config_stage_dependencies_missing"));
			return false;
		}
		OutStageSource = MakeShared<FEditorProjectConfigStageSource>(
			TargetResolver.ToSharedRef(),
			DurableBindingResolver.ToSharedRef());
		return OutStageSource.IsValid();
	}

	bool FUeAgentEditorProjectSimpleAuthoringProviderFactory::TryCreate(
		const FUeAgentEditorProjectSimpleAuthoringProviderConfig& Config,
		TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider>& OutProvider,
		FUeAgentEditorProjectSimpleAuthoringError& OutError)
	{
		OutProvider.Reset();
		OutError = FUeAgentEditorProjectSimpleAuthoringError();
		if (!ValidateConfig(Config, OutError))
		{
			return false;
		}
		const TSharedRef<
			IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe> MutationResolver =
				Config.MutationTargetResolver.ToSharedRef();
		const TSharedRef<
			IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe> PreStateResolver =
				Config.PreStateTargetResolver.ToSharedRef();
		const TSharedRef<
			IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe> PersistedResolver =
				Config.PersistedTargetResolver.ToSharedRef();
		const TSharedRef<
			IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver,
			ESPMode::ThreadSafe> DurableBindingResolver =
				Config.DurableBindingResolver.ToSharedRef();
		TSharedPtr<IUeAgentEditorProjectSimpleEffectNativeHook> NativeHook;
		FExecutionMutationError NativeError;
		if (!FUeAgentEditorProjectSimpleEffectNativeHookFactory::TryCreate(
				MutationResolver,
				NativeHook,
				NativeError)
			|| !NativeHook.IsValid())
		{
			SetError(
				OutError,
				NativeError.GetCode().IsEmpty()
					? TEXT("editor_project_native_hook_missing")
					: NativeError.GetCode(),
				TEXT("editor_project"),
				NativeError.GetDetail());
			return false;
		}

		FUeAgentSimpleAuthoringProductionProviderConfig ProviderConfig;
		ProviderConfig.FamilyId = TEXT("editor_project");
		ProviderConfig.OwnerLeaseId = Config.OwnerLeaseId;
		ProviderConfig.PreStatePlane = EUeAgentStatePlane::AuthoringMemory;
		ProviderConfig.AtomicityMode = EAtomicityMode::CompensatedSaga;
		ProviderConfig.MaxQueueItems = Config.MaxQueueItems;
		ProviderConfig.Projection = Config.Projection;
		ProviderConfig.TargetResolver =
			MakeShared<FEditorProjectTargetResolver>(
				PreStateResolver,
				DurableBindingResolver);
		ProviderConfig.PreStateReadPort =
			MakeShared<FEditorProjectStateReadPort>(
				PreStatePortId,
				EUeAgentStatePlane::AuthoringMemory,
				PreStateResolver);
		ProviderConfig.ChangeTargetProjector =
			MakeShared<FEditorProjectChangeTargetProjector>();
		ProviderConfig.DraftBuilder =
			MakeShared<FEditorProjectDraftBuilder>();
		ProviderConfig.MutationBindingProjector =
			MakeShared<FEditorProjectMutationBindingProjector>();
		ProviderConfig.DurableTargetProjector =
			MakeShared<FEditorProjectDurableTargetProjector>();
		ProviderConfig.DurableService = Config.DurableService;
		ProviderConfig.UeHook = MakeShared<FEditorProjectUeHook>(
			NativeHook.ToSharedRef());
		ProviderConfig.ExpectedStateProjector =
			MakeShared<FEditorProjectExpectedStateProjector>();
		const TSharedRef<IUeAgentStateReadPort, ESPMode::ThreadSafe>
			PersistedStateReadPort =
				MakeShared<FEditorProjectStateReadPort>(
				PersistedStatePortId,
				EUeAgentStatePlane::Persisted,
				PersistedResolver);
		ProviderConfig.PersistedStateOracle = MakeShared<
			FStateReadPortPersistedStateOracle,
			ESPMode::ThreadSafe>(PersistedStateReadPort);
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
