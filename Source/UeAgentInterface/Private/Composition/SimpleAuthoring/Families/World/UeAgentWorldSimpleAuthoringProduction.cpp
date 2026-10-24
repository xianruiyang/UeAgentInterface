#include "Composition/SimpleAuthoring/Families/World/UeAgentWorldSimpleAuthoringProduction.h"
#include "Core/Contract/UeAgentContractJson.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Core/AssetDurable/UeAgentPersistedWorld.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/WorldPartition.h"

namespace UE::UAI::Composition::SimpleAuthoring::World
{
	namespace
	{
		using namespace AssetDurable;
		using namespace ContextResource;
		using namespace Contract;
		using namespace Discovery;
		using namespace DomainAdapters::GenericEffect;
		using namespace DomainAdapters::WorldSimpleEffect;
		using namespace ExecutionMutation;
		using namespace PlanningPreflight;
		using namespace StateSemantic;
		using namespace Verification;

		constexpr const TCHAR* PreStatePortId =
			TEXT("world-simple.authoring-memory.v1");
		constexpr const TCHAR* PersistedStatePortId =
			TEXT("world-simple.independent-readback.v1");
		constexpr const TCHAR* RequestEvidencePrefix =
			TEXT("world-simple-request-v1:");

		enum class EWorldProfileMechanic : uint8
		{
			ActorCreate,
			WallCreate,
			DataLayerCreate,
			DataLayerMembership,
			DataLayerState,
			ActorHlodMembership,
			ActorStreamingState,
			StreamingRelationCreate,
			StreamingRelationState,
			WorldPartitionStreamingState,
			LevelGameModeState,
		};

		struct FWorldProfileSpec
		{
			const TCHAR* DescriptorId;
			const TCHAR* CapabilityId;
			const TCHAR* NativeTargetKind;
			const TCHAR* StateKind;
			EWorldProfileMechanic Mechanic;
			EUeAgentCandidateOperation Operation;
		};

		const FWorldProfileSpec WorldProfiles[] = {
			{
				TEXT("uai.operation.world.actor.create"),
				TEXT("world.actor.create"),
				TEXT("world.entity.create"),
				TEXT("actor"),
				EWorldProfileMechanic::ActorCreate,
				EUeAgentCandidateOperation::Create,
			},
			{
				TEXT(
					"uai.operation.world."
					"wall.create_with_opening"),
				TEXT("world.wall.create_with_opening"),
				TEXT("world.entity.create"),
				TEXT("wall_segment"),
				EWorldProfileMechanic::WallCreate,
				EUeAgentCandidateOperation::Create,
			},
			{
				TEXT(
					"uai.operation.world."
					"data_layer_instance.create"),
				TEXT("world.data_layer_instance.create"),
				TEXT("world.entity.create"),
				TEXT("data_layer_instance"),
				EWorldProfileMechanic::DataLayerCreate,
				EUeAgentCandidateOperation::Create,
			},
			{
				TEXT(
					"uai.operation.world."
					"data_layer_relation.update"),
				TEXT("world.data_layer_relation.update"),
				TEXT("world.membership.update"),
				TEXT("data_layer_relation"),
				EWorldProfileMechanic::DataLayerMembership,
				EUeAgentCandidateOperation::Update,
			},
			{
				TEXT(
					"uai.operation.world."
					"data_layer_state.update"),
				TEXT("world.data_layer_state.update"),
				TEXT("world.state.update"),
				TEXT("data_layer"),
				EWorldProfileMechanic::DataLayerState,
				EUeAgentCandidateOperation::Update,
			},
			{
				TEXT(
					"uai.operation.world."
					"actor_hlod_layer.update"),
				TEXT("world.actor_hlod_layer.update"),
				TEXT("world.membership.update"),
				TEXT("actor_hlod_layer"),
				EWorldProfileMechanic::ActorHlodMembership,
				EUeAgentCandidateOperation::Update,
			},
			{
				TEXT(
					"uai.operation.world."
					"actor_streaming_policy.update"),
				TEXT("world.actor_streaming_policy.update"),
				TEXT("world.state.update"),
				TEXT("actor_streaming_policy"),
				EWorldProfileMechanic::ActorStreamingState,
				EUeAgentCandidateOperation::Update,
			},
			{
				TEXT(
					"uai.operation.world."
					"streaming_relation.create"),
				TEXT("world.streaming_relation.create"),
				TEXT("world.entity.create"),
				TEXT("streaming_relation"),
				EWorldProfileMechanic::StreamingRelationCreate,
				EUeAgentCandidateOperation::Create,
			},
			{
				TEXT(
					"uai.operation.world."
					"streaming_relation.state.update"),
				TEXT("world.streaming_relation.state.update"),
				TEXT("world.state.update"),
				TEXT("streaming_relation"),
				EWorldProfileMechanic::StreamingRelationState,
				EUeAgentCandidateOperation::Update,
			},
			{
				TEXT(
					"uai.operation.world."
					"world_partition_streaming.update"),
				TEXT(
					"world.world_partition_streaming.update"),
				TEXT("world.state.update"),
				TEXT("world_partition_streaming"),
				EWorldProfileMechanic::
					WorldPartitionStreamingState,
				EUeAgentCandidateOperation::Update,
			},
			{
				TEXT("uai.operation.world.level_game_mode.update"),
				TEXT("world.level_game_mode.update"),
				TEXT("world.state.update"),
				TEXT("level_game_mode"),
				EWorldProfileMechanic::LevelGameModeState,
				EUeAgentCandidateOperation::Update,
			},
		};

		const FWorldProfileSpec* FindProfile(
			const FUeAgentDescriptorIdentity& Identity)
		{
			for (const FWorldProfileSpec& Profile : WorldProfiles)
			{
				if (Identity.GetDescriptorId()
					== Profile.DescriptorId)
				{
					return &Profile;
				}
			}
			return nullptr;
		}

		FDiagnostic WorldFailure(const FString& Code, const FString& Reason, const FString& Field)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("simple_authoring"));
			Error.Schema = TEXT("uai.runtime.diagnostic/1");
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			return Error;
		}

		FDiagnostic JsonValueFailure(const FString& Path, const TSharedPtr<FJsonValue>& Actual, const FString& Expected)
		{
			auto Error = WorldFailure(TEXT("world_simple_state_field_invalid"),
				TEXT("The World state field does not satisfy the required value."), Path);
			Error.ActualValuePresent = Actual.IsValid();
			if (Actual.IsValid() && (Actual->Type == EJson::Array || Actual->Type == EJson::Object))
			{
				auto Summary = MakeShared<FJsonObject>();
				Summary->SetStringField(TEXT("kind"), Actual->Type == EJson::Array ? TEXT("array") : TEXT("object"));
				if (Actual->Type == EJson::Array) Summary->SetNumberField(TEXT("count"), Actual->AsArray().Num());
				else if (Actual->AsObject().IsValid()) Summary->SetNumberField(TEXT("count"), Actual->AsObject()->Values.Num());
				Error.ActualValue = MakeShared<FJsonValueObject>(Summary);
			}
			else Error.ActualValue = Actual;
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return Error;
		}

		FDiagnostic JsonFieldFailure(const TSharedPtr<FJsonObject>& Object, const FString& Field,
			const FString& Expected, const FString& Path = FString())
		{
			return JsonValueFailure(Path.IsEmpty() ? Field : Path,
				Object.IsValid() ? Object->Values.FindRef(Field) : nullptr, Expected);
		}

		FDiagnostic MissingProfile(const FUeAgentDescriptorIdentity& Identity)
		{
			auto Error = WorldFailure(TEXT("world_simple_authoring_profile_missing"),
				TEXT("No World authoring profile is registered for the requested descriptor."), TEXT("descriptor_id"));
			Error.CandidateName = Identity.GetDescriptorId();
			Error.ActualValue = MakeShared<FJsonValueString>(Identity.GetDescriptorId());
			TArray<TSharedPtr<FJsonValue>> Supported;
			for (const auto& Profile : WorldProfiles) Supported.Add(MakeShared<FJsonValueString>(Profile.DescriptorId));
			Error.ExpectedValue = MakeShared<FJsonValueArray>(Supported);
			return Error;
		}

		FDiagnostic CountFailure(const FString& Code, const FString& Reason, const FString& Field, const int32 Actual, const int32 Expected)
		{
			auto Error = WorldFailure(Code, Reason, Field);
			Error.ActualValue = MakeShared<FJsonValueNumber>(Actual);
			Error.ExpectedValue = MakeShared<FJsonValueNumber>(Expected);
			return Error;
		}


		


		EUeAgentCandidateOperation CandidateOperation(
			const EChangeIntent Intent)
		{
			switch (Intent)
			{
			case EChangeIntent::Create:
				return EUeAgentCandidateOperation::Create;
			case EChangeIntent::Replace:
				return EUeAgentCandidateOperation::Replace;
			case EChangeIntent::Delete:
				return EUeAgentCandidateOperation::Delete;
			case EChangeIntent::Update:
			default:
				return EUeAgentCandidateOperation::Update;
			}
		}

		FUeAgentCanonicalIdentity TargetIdentity(
			const FUeAgentExecutionContextRef& Context,
			const FUeAgentOperationRequest& Request)
		{
			return FUeAgentCanonicalIdentity(
				TEXT("uai.world.simple.authoring:")
					+ Request.GetDescriptorIdentity()
						.GetDescriptorId(),
				TEXT("world.simple.operation"),
				Context.GetWorldIdentity(),
				Context.GetLevelIdentity(),
				Request.GetDescriptorIdentity()
					.GetDescriptorId(),
				Context.GetContextRevision());
		}

		FUeAgentCanonicalIdentity StateIdentity(
			const FWorldProfileSpec& Profile,
			const FUeAgentCandidateBinding& Binding,
			const FString& Selector)
		{
			const FUeAgentCanonicalIdentity& Target =
				Binding.GetSummary().GetIdentity();
			return FUeAgentCanonicalIdentity(
				TEXT("uai.world.simple:")
					+ FString(Profile.StateKind)
					+ TEXT(":")
					+ Selector,
				Profile.NativeTargetKind,
				Target.GetOwnerId(),
				Selector,
				Profile.StateKind,
				Target.GetGeneration());
		}

		bool TryParseRequestEvidence(
			const FUeAgentCandidateBinding& Binding,
			TSharedPtr<FJsonObject>& OutPayload,
			FUeAgentDiscoveryError& OutError)
		{
			OutPayload.Reset();
			const FString& Evidence =
				Binding.GetBindingEvidence();
			const FString Prefix(RequestEvidencePrefix);
			if (!Evidence.StartsWith(Prefix))
			{
				OutError = WorldFailure(TEXT("world_simple_state_scope_missing"),
					TEXT("The World binding evidence has an unsupported format prefix."), TEXT("binding_evidence/prefix"));
				OutError.SubjectId = Binding.GetBindingId();
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Evidence.Left(Prefix.Len()));
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Prefix);
				return false;
			}
			if (!Contract::Json::TryParseObject(Evidence.RightChop(Prefix.Len()), OutPayload, OutError))
			{
				OutError.SourceKind = TEXT("native_result");
				if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = TEXT("binding_evidence");
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Binding.GetBindingId();
				return false;
			}
			return true;
		}

		TArray<FString> ReadStringArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field)
		{
			TArray<FString> Result;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object.IsValid()
				|| !Object->TryGetArrayField(Field, Values)
				|| !Values)
			{
				return Result;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Text;
				if (Value.IsValid()
					&& Value->TryGetString(Text)
					&& !Text.IsEmpty())
				{
					Result.Add(MoveTemp(Text));
				}
			}
			return Result;
		}

		bool TryCanonicalJsonValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError,
			const FString& Path = TEXT("state"))
		{
			OutValue.Reset();
			if (!JsonValue.IsValid())
			{
				OutError = WorldFailure(TEXT("world_simple_state_value_missing"),
					TEXT("The generated World state contains a missing JSON value."), Path);
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			const auto Complete = [&](const bool bSucceeded)
			{
				if (!bSucceeded)
				{
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = OutError.PropertyPath.IsEmpty()
						? Path : Path + (OutError.PropertyPath.StartsWith(TEXT("/")) ? OutError.PropertyPath : TEXT("/") + OutError.PropertyPath);
				}
				return bSucceeded;
			};
			switch (JsonValue->Type)
			{
			case EJson::Null:
				return Complete(FUeAgentCanonicalValue::TryNull(
					Resources,
					OutValue,
					OutError));
			case EJson::Boolean:
				return Complete(FUeAgentCanonicalValue::TryBoolean(
					JsonValue->AsBool(),
					Resources,
					OutValue,
					OutError));
			case EJson::String:
				return Complete(FUeAgentCanonicalValue::TryString(
					JsonValue->AsString(),
					Resources,
					OutValue,
					OutError));
			case EJson::Number:
				return Complete(FUeAgentCanonicalValue::TryDecimal(
					FString::SanitizeFloat(
						JsonValue->AsNumber()),
					Resources,
					OutValue,
					OutError));
			case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					const auto& Values = JsonValue->AsArray();
					for (int32 Index = 0; Index < Values.Num(); ++Index)
					{
						FUeAgentCanonicalValuePtr CanonicalItem;
						if (!TryCanonicalJsonValue(
								Values[Index],
								Resources,
								CanonicalItem,
								OutError,
								Path + TEXT("/") + FString::FromInt(Index)))
						{
							return false;
						}
						Items.Add(CanonicalItem.ToSharedRef());
					}
					return Complete(FUeAgentCanonicalValue::TryArray(
						Items,
						Resources,
						OutValue,
						OutError));
				}
			case EJson::Object:
				{
					TArray<
						TPair<
							FString,
							FUeAgentCanonicalValueRef>> Items;
					for (const TPair<
						FString,
						TSharedPtr<FJsonValue>>& Pair
						: JsonValue->AsObject()->Values)
					{
						FUeAgentCanonicalValuePtr CanonicalItem;
						if (!TryCanonicalJsonValue(
								Pair.Value,
								Resources,
								CanonicalItem,
								OutError,
								Path + TEXT("/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))))
						{
							return false;
						}
						Items.Emplace(
							Pair.Key,
							CanonicalItem.ToSharedRef());
					}
					return Complete(FUeAgentCanonicalValue::TryObject(
						Items,
						Resources,
						OutValue,
						OutError));
				}
			default:
				OutError = WorldFailure(TEXT("world_simple_state_value_kind_invalid"),
					TEXT("The generated World state contains an unsupported JSON kind."), Path);
				OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(JsonValue->Type));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("null, boolean, string, number, array or object"));
				return false;
			}
		}

		bool TryCanonicalJsonObject(
			const TSharedRef<FJsonObject>& Object,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			return TryCanonicalJsonValue(
				MakeShared<FJsonValueObject>(Object),
				Resources,
				OutValue,
				OutError);
		}

		template <typename TObject>
		bool TryResolveTypedReference(
			IUeAgentWorldSimpleEffectReferenceResolver&
				ReferenceResolver,
			UWorld& World,
			const FString& Selector,
			TObject*& OutObject,
			FUeAgentDiscoveryError& OutError)
		{
			OutObject = nullptr;
			UObject* ResolvedObject = nullptr;
			FExecutionMutationError ResolveError;
			if (!ReferenceResolver.TryResolveReference(
					World,
					Selector,
					ResolvedObject,
					ResolveError))
			{
				OutError = ResolveError;
				if (OutError.IsEmpty())
					OutError = WorldFailure(TEXT("world_simple_state_reference_unresolved"),
						TEXT("The World reference resolver failed without returning a diagnostic."), TEXT("reference"));
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Selector;
				return false;
			}
			OutObject = Cast<TObject>(ResolvedObject);
			if (!OutObject)
			{
				OutError = WorldFailure(TEXT("world_simple_state_reference_type_invalid"),
					ResolvedObject ? TEXT("The resolved World reference has an incompatible object type.")
						: TEXT("The World reference resolver returned no object."), TEXT("reference"));
				OutError.TargetPath = Selector;
				OutError.ActualValuePresent = ResolvedObject != nullptr;
				if (ResolvedObject) OutError.ActualValue = MakeShared<FJsonValueString>(ResolvedObject->GetClass()->GetPathName());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TObject::StaticClass()->GetPathName());
				return false;
			}
			return true;
		}

		bool TryMakeWorldPartitionStreamingValue(
			const bool bEnabled,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			FUeAgentCanonicalValuePtr Kind;
			FUeAgentCanonicalValuePtr Enabled;
			if (!FUeAgentCanonicalValue::TryString(
					TEXT("world_partition_streaming"),
					Resources,
					Kind,
					OutError)
				|| !FUeAgentCanonicalValue::TryBoolean(
					bEnabled,
					Resources,
					Enabled,
					OutError))
			{
				return false;
			}
			return FUeAgentCanonicalValue::TryObject(
				{
					{
						TEXT("enabled"),
						Enabled.ToSharedRef(),
					},
					{
						TEXT("state_kind"),
						Kind.ToSharedRef(),
					},
				},
				Resources,
				OutValue,
				OutError);
		}

		bool TryMakeLevelGameModeValue(
			const UWorld& World,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			const AWorldSettings* WorldSettings = World.GetWorldSettings();
			if (!WorldSettings)
			{
				OutError = WorldFailure(TEXT("world_simple_world_settings_missing"),
					TEXT("The target world has no WorldSettings object."), TEXT("world_settings"));
				OutError.TargetPath = World.GetOutermost()->GetName();
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			const FString GameModeClassRef = WorldSettings->DefaultGameMode
				? WorldSettings->DefaultGameMode->GetPathName()
				: FString();
			FUeAgentCanonicalValuePtr Kind;
			FUeAgentCanonicalValuePtr ClassRef;
			if (!FUeAgentCanonicalValue::TryString(
					TEXT("level_game_mode"),
					Resources,
					Kind,
					OutError)
				|| !FUeAgentCanonicalValue::TryString(
					GameModeClassRef,
					Resources,
					ClassRef,
					OutError))
			{
				return false;
			}
			return FUeAgentCanonicalValue::TryObject(
				{
					{TEXT("game_mode_class_ref"), ClassRef.ToSharedRef()},
					{TEXT("state_kind"), Kind.ToSharedRef()},
				},
				Resources,
				OutValue,
				OutError);
		}

		bool TryMakeActorStreamingPolicyValue(
			const AActor& Actor,
			const FString& TargetRef,
			const TSharedPtr<FJsonObject>& Payload,
			const bool bReadFullState,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			const TSharedPtr<FJsonObject>* Changes = nullptr;
			Payload->TryGetObjectField(
				TEXT("changes"),
				Changes);
			const bool bReadSpatial =
				bReadFullState
				|| (Changes
					&& (*Changes)->HasField(
						TEXT("is_spatially_loaded")));
			const bool bReadGrid =
				bReadFullState
				|| (Changes
					&& (*Changes)->HasField(
						TEXT("runtime_grid")));

			FUeAgentCanonicalValuePtr Kind;
			FUeAgentCanonicalValuePtr Ref;
			if (!FUeAgentCanonicalValue::TryString(
					TEXT("actor_streaming_policy"),
					Resources,
					Kind,
					OutError)
				|| !FUeAgentCanonicalValue::TryString(
					TargetRef,
					Resources,
					Ref,
					OutError))
			{
				return false;
			}
			TArray<
				TPair<FString, FUeAgentCanonicalValueRef>>
					Items;
			Items.Emplace(
				TEXT("state_kind"),
				Kind.ToSharedRef());
			Items.Emplace(
				TEXT("target_ref"),
				Ref.ToSharedRef());
			if (bReadSpatial)
			{
				FUeAgentCanonicalValuePtr Spatial;
				if (!FUeAgentCanonicalValue::TryBoolean(
						Actor.GetIsSpatiallyLoaded(),
						Resources,
						Spatial,
						OutError))
				{
					return false;
				}
				Items.Emplace(
					TEXT("is_spatially_loaded"),
					Spatial.ToSharedRef());
			}
			if (bReadGrid)
			{
				const FName RuntimeGrid =
					Actor.GetRuntimeGrid();
				FUeAgentCanonicalValuePtr Intent;
				if (!FUeAgentCanonicalValue::TryString(
						RuntimeGrid.IsNone()
							? TEXT("clear")
							: TEXT("set"),
						Resources,
						Intent,
						OutError))
				{
					return false;
				}
				Items.Emplace(
					TEXT("runtime_grid_intent"),
					Intent.ToSharedRef());
				if (!RuntimeGrid.IsNone())
				{
					FUeAgentCanonicalValuePtr Value;
					if (!FUeAgentCanonicalValue::TryString(
							RuntimeGrid.ToString(),
							Resources,
							Value,
							OutError))
					{
						return false;
					}
					Items.Emplace(
						TEXT("runtime_grid_value"),
						Value.ToSharedRef());
				}
			}
			return FUeAgentCanonicalValue::TryObject(
				Items,
				Resources,
				OutValue,
				OutError);
		}

		FString DataLayerRuntimeStateText(
			const EDataLayerRuntimeState State)
		{
			switch (State)
			{
			case EDataLayerRuntimeState::Activated:
				return TEXT("activated");
			case EDataLayerRuntimeState::Loaded:
				return TEXT("loaded");
			case EDataLayerRuntimeState::Unloaded:
			default:
				return TEXT("unloaded");
			}
		}

		bool TryMakeDataLayerStateValue(
			const UDataLayerInstance& Instance,
			const FString& TargetRef,
			const TSharedPtr<FJsonObject>& Payload,
			const bool bReadFullState,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			const TSharedPtr<FJsonObject>* Changes = nullptr;
			Payload->TryGetObjectField(
				TEXT("changes"),
				Changes);
			const TSharedRef<FJsonObject> Value =
				MakeShared<FJsonObject>();
			Value->SetStringField(
				TEXT("state_kind"),
				TEXT("data_layer"));
			Value->SetStringField(
				TEXT("target_ref"),
				TargetRef);
			if (bReadFullState
				|| (Changes
					&& (*Changes)->HasField(
						TEXT("initial_runtime_state"))))
			{
				Value->SetStringField(
					TEXT("initial_runtime_state"),
					DataLayerRuntimeStateText(
						Instance.GetInitialRuntimeState()));
			}
			if (bReadFullState
				|| (Changes
					&& (*Changes)->HasField(
						TEXT("initially_visible"))))
			{
				Value->SetBoolField(
					TEXT("initially_visible"),
					Instance.IsInitiallyVisible());
			}
			if (bReadFullState
				|| (Changes
					&& (*Changes)->HasField(
						TEXT("loaded_in_editor"))))
			{
				Value->SetBoolField(
					TEXT("loaded_in_editor"),
					Instance.IsLoadedInEditor());
			}
			if (bReadFullState
				|| (Changes
					&& (*Changes)->HasField(
						TEXT("visible"))))
			{
				Value->SetBoolField(
					TEXT("visible"),
					Instance.IsVisible());
			}
			if (bReadFullState
				|| (Changes
					&& (*Changes)->HasField(
						TEXT("short_name"))))
			{
				Value->SetStringField(
					TEXT("short_name"),
					Instance.GetDataLayerShortName());
			}
			return TryCanonicalJsonObject(
				Value,
				Resources,
				OutValue,
				OutError);
		}

		TArray<TSharedPtr<FJsonValue>> MakeNumberArray(
			std::initializer_list<double> Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(
				static_cast<int32>(Values.size()));
			for (const double Value : Values)
			{
				Result.Add(
					MakeShared<FJsonValueNumber>(Value));
			}
			return Result;
		}

		bool TryMakeStreamingRelationStateValue(
			const ULevelStreaming& Relation,
			const FString& TargetRef,
			const TSharedPtr<FJsonObject>& Payload,
			const bool bReadFullState,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			const TSharedPtr<FJsonObject>* Changes = nullptr;
			Payload->TryGetObjectField(
				TEXT("changes"),
				Changes);
			const TSharedRef<FJsonObject> Value =
				MakeShared<FJsonObject>();
			Value->SetStringField(
				TEXT("state_kind"),
				TEXT("streaming_relation"));
			Value->SetStringField(
				TEXT("target_ref"),
				TargetRef);
			if (bReadFullState
				|| (Changes
					&& (*Changes)->HasField(
						TEXT("loaded"))))
			{
				Value->SetBoolField(
					TEXT("loaded"),
					Relation.ShouldBeLoaded());
			}
			if (bReadFullState
				|| (Changes
					&& (*Changes)->HasField(
						TEXT("visible"))))
			{
				Value->SetBoolField(
					TEXT("visible"),
					Relation.ShouldBeVisible());
			}
			const TSharedPtr<FJsonObject>* Placement = nullptr;
			const bool bReadTransform =
				bReadFullState
				|| (Changes
					&& (*Changes)->TryGetObjectField(
						TEXT("placement"),
						Placement)
					&& Placement);
			if (bReadTransform)
			{
				FString Unit = TEXT("centimeter");
				if (!bReadFullState
					&& Placement
					&& (*Placement)->HasTypedField<EJson::String>(
						TEXT("unit")))
				{
					Unit = (*Placement)->GetStringField(
						TEXT("unit"));
				}
				const double TranslationScale =
					Unit == TEXT("meter") ? 0.01 : 1.0;
				const FVector Translation =
					Relation.LevelTransform.GetLocation()
					* TranslationScale;
				FQuat Rotation =
					Relation.LevelTransform.GetRotation();
				Rotation.Normalize();
				const FVector Scale =
					Relation.LevelTransform.GetScale3D();
				Value->SetBoolField(
					TEXT("has_transform"),
					true);
				Value->SetArrayField(
					TEXT("translation"),
					MakeNumberArray({
						Translation.X,
						Translation.Y,
						Translation.Z,
					}));
				Value->SetArrayField(
					TEXT("rotation_quaternion_xyzw"),
					MakeNumberArray({
						Rotation.X,
						Rotation.Y,
						Rotation.Z,
						Rotation.W,
					}));
				Value->SetArrayField(
					TEXT("scale"),
					MakeNumberArray({
						Scale.X,
						Scale.Y,
						Scale.Z,
					}));
				Value->SetStringField(
					TEXT("unit"),
					Unit);
			}
			return TryCanonicalJsonObject(
				Value,
				Resources,
				OutValue,
				OutError);
		}

		bool TryReadNumberArray(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Field,
			const int32 ExpectedCount,
			TArray<double>& OutValues,
			FDiagnostic& OutError,
			const FString& ParentPath = FString())
		{
			OutValues.Reset();
			const FString Path = ParentPath.IsEmpty() ? Field : ParentPath + TEXT("/") + Field;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values)
			{
				OutError = JsonFieldFailure(Object, Field, TEXT("number array"), Path);
				return false;
			}
			if (Values->Num() != ExpectedCount)
			{
				OutError = CountFailure(TEXT("world_simple_state_array_size_invalid"),
					TEXT("The World state array has the wrong number of elements."), Path, Values->Num(), ExpectedCount);
				return false;
			}
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
				double Number = 0.0;
				if (!Value.IsValid() || !Value->TryGetNumber(Number) || !FMath::IsFinite(Number))
				{
					OutError = JsonValueFailure(Path + TEXT("/") + FString::FromInt(Index), Value, TEXT("finite number"));
					return false;
				}
				OutValues.Add(Number);
			}
			return true;
		}

		bool TryReadCreatePlacement(
			const TSharedPtr<FJsonObject>& Payload,
			bool& bOutHasTransform,
			FString& OutUnit,
			FTransform& OutTransform,
			FDiagnostic& OutError)
		{
			bOutHasTransform = false;
			OutUnit = TEXT("centimeter");
			OutTransform = FTransform::Identity;
			const TSharedPtr<FJsonObject>* Placement = nullptr;
			if (!Payload.IsValid()
				|| !Payload->TryGetObjectField(
					TEXT("placement"),
					Placement)
				|| !Placement)
			{
				if (Payload.IsValid()) return true;
				OutError = JsonValueFailure(TEXT("payload"), nullptr, TEXT("object"));
				return false;
			}
			TArray<double> Translation;
			TArray<double> Rotation;
			TArray<double> Scale;
			if (!TryReadNumberArray(*Placement, TEXT("translation"), 3, Translation, OutError, TEXT("placement"))
				|| !TryReadNumberArray(*Placement, TEXT("rotation_quaternion_xyzw"), 4, Rotation, OutError, TEXT("placement"))
				|| !TryReadNumberArray(*Placement, TEXT("scale"), 3, Scale, OutError, TEXT("placement"))) return false;
			if (!(*Placement)->TryGetStringField(TEXT("unit"), OutUnit))
			{
				OutError = JsonFieldFailure(*Placement, TEXT("unit"), TEXT("string"), TEXT("placement/unit"));
				return false;
			}
			const double UnitScale =
				OutUnit == TEXT("meter") ? 100.0 : 1.0;
			FQuat Quaternion(
				Rotation[0],
				Rotation[1],
				Rotation[2],
				Rotation[3]);
			Quaternion.Normalize();
			OutTransform = FTransform(
				Quaternion,
				FVector(
					Translation[0] * UnitScale,
					Translation[1] * UnitScale,
					Translation[2] * UnitScale),
				FVector(Scale[0], Scale[1], Scale[2]));
			bOutHasTransform = true;
			return true;
		}

		AActor* FindActorByExactLabel(
			UWorld& World,
			const FString& Label,
			int32& OutMatchCount)
		{
			AActor* Match = nullptr;
			OutMatchCount = 0;
			TSet<AActor*> SeenActors;
			const auto VisitLevel =
				[&Label, &Match, &OutMatchCount, &SeenActors](
					ULevel* Level)
			{
				if (!Level)
				{
					return;
				}
				for (AActor* Actor : Level->Actors)
				{
					if (!IsValid(Actor)
						|| Actor->IsTemplate()
						|| SeenActors.Contains(Actor))
					{
						continue;
					}
					SeenActors.Add(Actor);
					if (Actor->GetActorLabel() == Label)
					{
						Match = Actor;
						++OutMatchCount;
					}
				}
			};
			VisitLevel(World.PersistentLevel);
			for (ULevel* Level : World.GetLevels())
			{
				VisitLevel(Level);
			}
			return Match;
		}

		FTransform ReadAuthoredActorTransform(const AActor& Actor)
		{
			const USceneComponent* RootComponent =
				Actor.GetRootComponent();
			if (RootComponent && !RootComponent->GetAttachParent())
			{
				return RootComponent->GetRelativeTransform();
			}
			return Actor.GetActorTransform();
		}

		ULevelStreaming* FindExactStreamingRelation(
			UWorld& World,
			const FString& LevelPackage,
			UClass& StreamingClass,
			const FTransform& Transform,
			int32& OutMatchCount)
		{
			ULevelStreaming* Match = nullptr;
			OutMatchCount = 0;
			for (ULevelStreaming* Relation
				: World.GetStreamingLevels())
			{
				if (Relation
					&& Relation->GetWorldAssetPackageName()
						== LevelPackage
					&& Relation->GetClass() == &StreamingClass
					&& Relation->LevelTransform.Equals(
						Transform,
						KINDA_SMALL_NUMBER))
				{
					Match = Relation;
					++OutMatchCount;
				}
			}
			return Match;
		}

		bool TryCheckCreateTargetAbsent(
			const FWorldProfileSpec& Profile,
			UWorld& World,
			IUeAgentWorldSimpleEffectReferenceResolver&
				ReferenceResolver,
			const TSharedPtr<FJsonObject>& Payload,
			FUeAgentDiscoveryError& OutError)
		{
			if (Profile.Mechanic
				== EWorldProfileMechanic::ActorCreate)
			{
				FString ActorClassRef;
				FString Label;
				if (!Payload->TryGetStringField(TEXT("actor_class_ref"), ActorClassRef))
				{
					OutError = JsonFieldFailure(Payload, TEXT("actor_class_ref"), TEXT("string"));
					return false;
				}
				if (!Payload->TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
				{
					OutError = JsonFieldFailure(Payload, TEXT("label"), TEXT("nonempty string"));
					return false;
				}
				UClass* ActorClass = nullptr;
				if (!TryResolveTypedReference(ReferenceResolver, World, ActorClassRef, ActorClass, OutError)) return false;
				if (!ActorClass->IsChildOf(AActor::StaticClass()))
				{
					OutError = WorldFailure(TEXT("world_simple_create_reference_type_invalid"),
						TEXT("The requested actor class does not derive from Actor."), TEXT("actor_class_ref"));
					OutError.TargetPath = ActorClassRef;
					OutError.ActualValue = MakeShared<FJsonValueString>(ActorClass->GetPathName());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(AActor::StaticClass()->GetPathName());
					return false;
				}
				bool bHasTransform = false;
				FString Unit;
				FTransform Transform;
				if (!TryReadCreatePlacement(Payload, bHasTransform, Unit, Transform, OutError)) return false;
				if (!bHasTransform)
				{
					OutError = JsonFieldFailure(Payload, TEXT("placement"), TEXT("placement object with translation, rotation_quaternion_xyzw, scale and unit"));
					return false;
				}
				int32 MatchCount = 0;
				FindActorByExactLabel(World, Label, MatchCount);
				if (MatchCount > 0)
				{
					OutError = CountFailure(TEXT("world_simple_create_target_exists"),
						TEXT("An actor with the requested label already exists."), TEXT("label"), MatchCount, 0);
					OutError.CandidateName = Label;
					OutError.TargetPath = World.GetOutermost()->GetName();
					return false;
				}
				return true;
			}

			if (Profile.Mechanic
				== EWorldProfileMechanic::WallCreate)
			{
				const TSharedPtr<FJsonObject>* Segment = nullptr;
				if (!Payload->TryGetObjectField(
						TEXT("segment_template"),
						Segment)
					|| !Segment)
				{
					OutError = JsonFieldFailure(Payload, TEXT("segment_template"), TEXT("object"));
					return false;
				}
				const FString ActorClassRef =
					(*Segment)->GetStringField(
						TEXT("actor_class_ref"));
				const FString MeshRef =
					(*Segment)->GetStringField(
						TEXT("mesh_ref"));
				UClass* ActorClass = nullptr;
				UStaticMesh* Mesh = nullptr;
				if (!TryResolveTypedReference(ReferenceResolver, World, ActorClassRef, ActorClass, OutError)) return false;
				if (!ActorClass->IsChildOf(AActor::StaticClass()))
				{
					OutError = WorldFailure(TEXT("world_simple_create_reference_type_invalid"),
						TEXT("The wall segment class does not derive from Actor."), TEXT("segment_template/actor_class_ref"));
					OutError.TargetPath = ActorClassRef;
					OutError.ActualValue = MakeShared<FJsonValueString>(ActorClass->GetPathName());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(AActor::StaticClass()->GetPathName());
					return false;
				}
				if (!TryResolveTypedReference(ReferenceResolver, World, MeshRef, Mesh, OutError)) return false;
				FString LabelPrefix = TEXT("WorldSimpleWall_");
				(*Segment)->TryGetStringField(
					TEXT("label_prefix"),
					LabelPrefix);
				for (int32 Index = 0; Index < 4; ++Index)
				{
					const FString Label = FString::Printf(
						TEXT("%s%d"),
						*LabelPrefix,
						Index);
					int32 MatchCount = 0;
					FindActorByExactLabel(
						World,
						Label,
						MatchCount);
					if (MatchCount > 0)
					{
						OutError = CountFailure(TEXT("world_simple_create_target_exists"),
							TEXT("A wall segment actor with the generated label already exists."), TEXT("segment_template/label_prefix"), MatchCount, 0);
						OutError.CandidateName = Label;
						OutError.TargetPath = World.GetOutermost()->GetName();
						return false;
					}
				}
				return true;
			}

			if (Profile.Mechanic
				== EWorldProfileMechanic::DataLayerCreate)
			{
				AWorldDataLayers* WorldDataLayers =
					World.GetWorldDataLayers();
				if (!WorldDataLayers)
				{
					OutError = WorldFailure(TEXT("world_simple_world_data_layers_missing"),
						TEXT("The target world has no WorldDataLayers object."), TEXT("world_data_layers"));
					OutError.TargetPath = World.GetOutermost()->GetName();
					OutError.ActualValuePresent = false;
					OutError.ExpectedValuePresent = true;
					return false;
				}
				FString ParentRef;
				if (Payload->TryGetStringField(
						TEXT("parent_ref"),
						ParentRef)
					&& !ParentRef.IsEmpty())
				{
					UDataLayerInstance* Parent = nullptr;
					if (!TryResolveTypedReference(
							ReferenceResolver,
							World,
							ParentRef,
							Parent,
							OutError))
					{
						return false;
					}
				}
				bool bPrivate = false;
				Payload->TryGetBoolField(
					TEXT("private"),
					bPrivate);
				if (bPrivate)
				{
					const FString ShortName =
						Payload->GetStringField(
							TEXT("short_name"));
					const int32 MatchCount = UeAgentLevelAuthoring::
						FindDataLayerInstancesExact(
							*WorldDataLayers,
							ShortName,
							UeAgentLevelAuthoring::
								EDataLayerTargetScope::PrivateOnly)
						.Num();
					if (MatchCount > 0)
					{
						OutError = CountFailure(TEXT("world_simple_create_target_exists"),
							TEXT("A private data layer with the requested name already exists."), TEXT("short_name"), MatchCount, 0);
						OutError.CandidateName = ShortName;
						OutError.TargetPath = World.GetOutermost()->GetName();
						return false;
					}
					return true;
				}
				const FString AssetRef =
					Payload->GetStringField(TEXT("asset_ref"));
				UDataLayerAsset* Asset = nullptr;
				if (!TryResolveTypedReference(
						ReferenceResolver,
						World,
						AssetRef,
						Asset,
						OutError))
				{
					return false;
				}
				if (WorldDataLayers->GetDataLayerInstance(Asset))
				{
				OutError = WorldFailure(TEXT("world_simple_create_target_exists"),
					TEXT("The world already has an instance for the requested data-layer asset."), TEXT("asset_ref"));
				OutError.TargetPath = AssetRef;
				OutError.ActualValuePresent = true;
				OutError.ExpectedValuePresent = false;
					return false;
				}
				return true;
			}

			const FString LevelRef =
				Payload->GetStringField(TEXT("level_asset_ref"));
			const FString ClassRef =
				Payload->GetStringField(
					TEXT("streaming_class_ref"));
			UWorld* LevelAsset = nullptr;
			UClass* StreamingClass = nullptr;
			if (!TryResolveTypedReference(ReferenceResolver, World, LevelRef, LevelAsset, OutError)
				|| !TryResolveTypedReference(ReferenceResolver, World, ClassRef, StreamingClass, OutError)) return false;
			if (!StreamingClass->IsChildOf(ULevelStreaming::StaticClass()))
			{
				OutError = WorldFailure(TEXT("world_simple_create_reference_type_invalid"),
					TEXT("The requested streaming class does not derive from LevelStreaming."), TEXT("streaming_class_ref"));
				OutError.TargetPath = ClassRef;
				OutError.ActualValue = MakeShared<FJsonValueString>(StreamingClass->GetPathName());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(ULevelStreaming::StaticClass()->GetPathName());
				return false;
			}
			bool bHasTransform = false;
			FString Unit;
			FTransform Transform;
			if (!TryReadCreatePlacement(Payload, bHasTransform, Unit, Transform, OutError)) return false;
			int32 MatchCount = 0;
			FindExactStreamingRelation(
				World,
				LevelAsset->GetOutermost()->GetName(),
				*StreamingClass,
				Transform,
				MatchCount);
			if (MatchCount > 0)
			{
				OutError = CountFailure(TEXT("world_simple_create_target_exists"),
					TEXT("A matching streaming relation already exists."), TEXT("level_asset_ref"), MatchCount, 0);
				OutError.CandidateName = LevelRef;
				OutError.TargetPath = World.GetOutermost()->GetName();
				return false;
			}
			return true;
		}

		bool TryMakeActorCreateValue(
			UWorld& World,
			IUeAgentWorldSimpleEffectReferenceResolver&
				ReferenceResolver,
			const TSharedPtr<FJsonObject>& Payload,
			const FString& Selector,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentDiscoveryError& OutError)
		{
			const FString Label =
				Payload->GetStringField(TEXT("label"));
			if (Selector != TEXT("actor-label:") + Label)
			{
				OutError = WorldFailure(TEXT("world_simple_create_identity_invalid"),
					TEXT("The actor readback selector differs from the requested label selector."), TEXT("selector"));
				OutError.ActualValue = MakeShared<FJsonValueString>(Selector);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("actor-label:") + Label);
				return false;
			}
			int32 MatchCount = 0;
			AActor* Actor = FindActorByExactLabel(
				World,
				Label,
				MatchCount);
			if (!Actor || MatchCount != 1)
			{
				OutError = CountFailure(MatchCount > 1 ? TEXT("world_simple_create_identity_ambiguous") : TEXT("world_simple_created_target_unresolved"),
					TEXT("The created actor readback requires exactly one matching target."), TEXT("label"), MatchCount, 1);
				OutError.CandidateName = Label;
				OutError.TargetPath = World.GetOutermost()->GetName();
				return false;
			}
			const FString ActorClassRef =
				Payload->GetStringField(TEXT("actor_class_ref"));
			UClass* ExpectedClass = nullptr;
			if (!TryResolveTypedReference(
					ReferenceResolver,
					World,
					ActorClassRef,
					ExpectedClass,
					OutError))
			{
				return false;
			}
			bool bHasTransform = false;
			FString Unit;
			FTransform ExpectedTransform;
			if (!TryReadCreatePlacement(Payload, bHasTransform, Unit, ExpectedTransform, OutError)) return false;
			if (!bHasTransform)
			{
				OutError = JsonFieldFailure(Payload, TEXT("placement"), TEXT("placement object with translation, rotation_quaternion_xyzw, scale and unit"));
				return false;
			}

			const FTransform ActualTransform =
				ReadAuthoredActorTransform(*Actor);
			const double TranslationScale =
				Unit == TEXT("meter") ? 0.01 : 1.0;
			const FVector Translation =
				ActualTransform.GetLocation() * TranslationScale;
			FQuat Rotation = ActualTransform.GetRotation();
			Rotation.Normalize();
			const FVector Scale = ActualTransform.GetScale3D();
			const TSharedRef<FJsonObject> Value =
				MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("entity_kind"), TEXT("actor"));
			Value->SetStringField(
				TEXT("actor_class_ref"),
				Actor->GetClass() == ExpectedClass
					? ActorClassRef
					: Actor->GetClass()->GetPathName());
			Value->SetStringField(TEXT("label"), Actor->GetActorLabel());
			if (Payload->HasField(TEXT("folder_path")))
			{
				const FName Folder = Actor->GetFolderPath();
				Value->SetStringField(
					TEXT("folder_path"),
					Folder.IsNone() ? FString() : Folder.ToString());
			}
			if (Payload->HasField(TEXT("tags")))
			{
				TArray<TSharedPtr<FJsonValue>> ActualTags;
				for (const FString& ExpectedTag
					: ReadStringArray(Payload, TEXT("tags")))
				{
					ActualTags.Add(MakeShared<FJsonValueString>(
						Actor->ActorHasTag(FName(*ExpectedTag))
							? ExpectedTag
							: TEXT("missing:") + ExpectedTag));
				}
				Value->SetArrayField(TEXT("tags"), ActualTags);
			}
			Value->SetArrayField(
				TEXT("translation"),
				MakeNumberArray({
					Translation.X,
					Translation.Y,
					Translation.Z,
				}));
			Value->SetArrayField(
				TEXT("rotation_quaternion_xyzw"),
				MakeNumberArray({
					Rotation.X,
					Rotation.Y,
					Rotation.Z,
					Rotation.W,
				}));
			Value->SetArrayField(
				TEXT("scale"),
				MakeNumberArray({Scale.X, Scale.Y, Scale.Z}));
			Value->SetStringField(TEXT("unit"), Unit);

			FUeAgentStateSemanticError StateError;
			if (!TryCanonicalJsonObject(
					Value,
					Resources,
					OutValue,
					StateError))
			{
				OutError = StateError;
				return false;
			}
			return true;
		}

		bool TryMakeWallCreateValue(
			UWorld& World,
			IUeAgentWorldSimpleEffectReferenceResolver&
				ReferenceResolver,
			const TSharedPtr<FJsonObject>& Payload,
			const FString& Selector,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentDiscoveryError& OutError)
		{
			FString IndexText;
			int32 SegmentIndex = INDEX_NONE;
			if (!Selector.Split(
					TEXT(":"),
					nullptr,
					&IndexText,
					ESearchCase::CaseSensitive,
					ESearchDir::FromEnd)
				|| !LexTryParseString(
					SegmentIndex,
					*IndexText)
				|| SegmentIndex < 0
				|| SegmentIndex >= 4)
			{
				OutError = WorldFailure(TEXT("world_simple_create_identity_invalid"),
					TEXT("The wall readback selector must end in a segment index from 0 to 3."), TEXT("selector"));
				OutError.ActualValue = MakeShared<FJsonValueString>(Selector);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("colon followed by an integer from 0 to 3"));
				return false;
			}
			const TSharedPtr<FJsonObject>* Segment = nullptr;
			if (!Payload->TryGetObjectField(
					TEXT("segment_template"),
					Segment)
				|| !Segment)
			{
				OutError = JsonFieldFailure(Payload, TEXT("segment_template"), TEXT("object"));
				return false;
			}
			FString LabelPrefix = TEXT("WorldSimpleWall_");
			(*Segment)->TryGetStringField(
				TEXT("label_prefix"),
				LabelPrefix);
			const FString Label = FString::Printf(
				TEXT("%s%d"),
				*LabelPrefix,
				SegmentIndex);
			int32 MatchCount = 0;
			AActor* Actor = FindActorByExactLabel(
				World,
				Label,
				MatchCount);
			if (!Actor || MatchCount != 1)
			{
				OutError = CountFailure(MatchCount > 1 ? TEXT("world_simple_create_identity_ambiguous") : TEXT("world_simple_created_target_unresolved"),
					TEXT("The created wall segment readback requires exactly one matching target."), TEXT("segment_template/label_prefix"), MatchCount, 1);
				OutError.CandidateName = Label;
				OutError.TargetPath = World.GetOutermost()->GetName();
				return false;
			}
			const FString ActorClassRef =
				(*Segment)->GetStringField(
					TEXT("actor_class_ref"));
			const FString MeshRef =
				(*Segment)->GetStringField(TEXT("mesh_ref"));
			UClass* ExpectedClass = nullptr;
			UStaticMesh* ExpectedMesh = nullptr;
			if (!TryResolveTypedReference(
					ReferenceResolver,
					World,
					ActorClassRef,
					ExpectedClass,
					OutError)
				|| !TryResolveTypedReference(
					ReferenceResolver,
					World,
					MeshRef,
					ExpectedMesh,
					OutError))
			{
				return false;
			}
			UStaticMesh* ActualMesh = nullptr;
			if (const AStaticMeshActor* MeshActor =
				Cast<AStaticMeshActor>(Actor))
			{
				ActualMesh =
					MeshActor->GetStaticMeshComponent()
						->GetStaticMesh();
			}
			const FTransform Transform =
				ReadAuthoredActorTransform(*Actor);
			const FVector Location = Transform.GetLocation();
			const FVector Normal =
				Transform.GetRotation().GetForwardVector();
			const FVector Up =
				Transform.GetRotation().GetUpVector();
			const FVector Dimensions =
				Transform.GetScale3D() * 100.0;
			const TSharedRef<FJsonObject> Value =
				MakeShared<FJsonObject>();
			Value->SetStringField(
				TEXT("entity_kind"),
				TEXT("wall_segment"));
			Value->SetStringField(
				TEXT("actor_class_ref"),
				Actor->GetClass() == ExpectedClass
					? ActorClassRef
					: Actor->GetClass()->GetPathName());
			Value->SetStringField(
				TEXT("mesh_ref"),
				ActualMesh == ExpectedMesh
					? MeshRef
					: ActualMesh
						? ActualMesh->GetPathName()
						: TEXT("missing"));
			Value->SetStringField(
				TEXT("label"),
				Actor->GetActorLabel());
			const FName Folder = Actor->GetFolderPath();
			Value->SetStringField(
				TEXT("folder_path"),
				Folder.IsNone()
					? FString()
					: Folder.ToString());
			Value->SetArrayField(
				TEXT("position"),
				MakeNumberArray({
					Location.X,
					Location.Y,
					Location.Z,
				}));
			Value->SetArrayField(
				TEXT("basis_normal"),
				MakeNumberArray({
					Normal.X,
					Normal.Y,
					Normal.Z,
				}));
			Value->SetArrayField(
				TEXT("basis_up"),
				MakeNumberArray({Up.X, Up.Y, Up.Z}));
			Value->SetArrayField(
				TEXT("dimensions_cm"),
				MakeNumberArray({
					Dimensions.X,
					Dimensions.Y,
					Dimensions.Z,
				}));
			FUeAgentStateSemanticError StateError;
			if (!TryCanonicalJsonObject(
					Value,
					Resources,
					OutValue,
					StateError))
			{
				OutError = StateError;
				return false;
			}
			return true;
		}

		bool TryMakeDataLayerCreateValue(
			UWorld& World,
			IUeAgentWorldSimpleEffectReferenceResolver&
				ReferenceResolver,
			const TSharedPtr<FJsonObject>& Payload,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentDiscoveryError& OutError)
		{
			AWorldDataLayers* WorldDataLayers =
				World.GetWorldDataLayers();
			if (!WorldDataLayers)
			{
				OutError = WorldFailure(TEXT("world_simple_world_data_layers_missing"),
					TEXT("The target world has no WorldDataLayers object."), TEXT("world_data_layers"));
				OutError.TargetPath = World.GetOutermost()->GetName();
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			bool bPrivate = false;
			Payload->TryGetBoolField(
				TEXT("private"),
				bPrivate);
			UDataLayerInstance* Instance = nullptr;
			FString AssetRef;
			UDataLayerAsset* ExpectedAsset = nullptr;
			if (bPrivate)
			{
				const FString ShortName =
					Payload->GetStringField(TEXT("short_name"));
				const TArray<UDataLayerInstance*> Matches =
					UeAgentLevelAuthoring::FindDataLayerInstancesExact(
						*WorldDataLayers,
						ShortName,
						UeAgentLevelAuthoring::
							EDataLayerTargetScope::PrivateOnly);
				const int32 MatchCount = Matches.Num();
				Instance = MatchCount == 1 ? Matches[0] : nullptr;
				if (MatchCount != 1)
				{
					OutError = CountFailure(MatchCount > 1 ? TEXT("world_simple_create_identity_ambiguous") : TEXT("world_simple_created_target_unresolved"),
						TEXT("The created private data layer readback requires exactly one matching target."), TEXT("short_name"), MatchCount, 1);
					OutError.CandidateName = ShortName;
					OutError.TargetPath = World.GetOutermost()->GetName();
					return false;
				}
			}
			else
			{
				AssetRef =
					Payload->GetStringField(TEXT("asset_ref"));
				if (!TryResolveTypedReference(
						ReferenceResolver,
						World,
						AssetRef,
						ExpectedAsset,
						OutError))
				{
					return false;
				}
				Instance = const_cast<UDataLayerInstance*>(
					WorldDataLayers->GetDataLayerInstance(
						ExpectedAsset));
				if (!Instance)
				{
					OutError = WorldFailure(TEXT("world_simple_created_target_unresolved"),
						TEXT("The world has no data-layer instance for the created asset."), TEXT("asset_ref"));
					OutError.TargetPath = AssetRef;
					OutError.ActualValuePresent = false;
					OutError.ExpectedValuePresent = true;
					return false;
				}
			}

			const TSharedRef<FJsonObject> Value =
				MakeShared<FJsonObject>();
			Value->SetStringField(
				TEXT("entity_kind"),
				TEXT("data_layer_instance"));
			const bool bActualPrivate =
				Instance->GetAsset()
				&& Instance->GetAsset()->IsPrivate();
			Value->SetBoolField(
				TEXT("is_private"),
				bActualPrivate);
			if (bPrivate)
			{
				Value->SetStringField(
					TEXT("short_name"),
					Instance->GetDataLayerShortName());
			}
			else
			{
				Value->SetStringField(
					TEXT("asset_ref"),
					Instance->GetAsset() == ExpectedAsset
						? AssetRef
						: Instance->GetAsset()
							? Instance->GetAsset()->GetPathName()
							: TEXT("missing"));
			}
			FString ParentRef;
			if (Payload->TryGetStringField(
					TEXT("parent_ref"),
					ParentRef))
			{
				UDataLayerInstance* ExpectedParent = nullptr;
				if (!TryResolveTypedReference(
						ReferenceResolver,
						World,
						ParentRef,
						ExpectedParent,
						OutError))
				{
					return false;
				}
				Value->SetStringField(
					TEXT("parent_ref"),
					Instance->GetParent() == ExpectedParent
						? ParentRef
						: TEXT("mismatch"));
			}
			const TSharedPtr<FJsonObject>* Initial = nullptr;
			if (Payload->TryGetObjectField(
					TEXT("initial_state"),
					Initial)
				&& Initial)
			{
				if ((*Initial)->HasField(
						TEXT("initial_runtime_state")))
				{
					Value->SetStringField(
						TEXT("initial_runtime_state"),
						DataLayerRuntimeStateText(
							Instance
								->GetInitialRuntimeState()));
				}
				if ((*Initial)->HasField(
						TEXT("initially_visible")))
				{
					Value->SetBoolField(
						TEXT("initially_visible"),
						Instance->IsInitiallyVisible());
				}
				if ((*Initial)->HasField(
						TEXT("loaded_in_editor")))
				{
					Value->SetBoolField(
						TEXT("loaded_in_editor"),
						Instance->IsLoadedInEditor());
				}
				if ((*Initial)->HasField(TEXT("visible")))
				{
					Value->SetBoolField(
						TEXT("visible"),
						Instance->IsVisible());
				}
			}
			FUeAgentStateSemanticError StateError;
			if (!TryCanonicalJsonObject(
					Value,
					Resources,
					OutValue,
					StateError))
			{
				OutError = StateError;
				return false;
			}
			return true;
		}

		bool TryMakeStreamingCreateValue(
			UWorld& World,
			IUeAgentWorldSimpleEffectReferenceResolver&
				ReferenceResolver,
			const TSharedPtr<FJsonObject>& Payload,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentDiscoveryError& OutError)
		{
			const FString LevelRef =
				Payload->GetStringField(TEXT("level_asset_ref"));
			const FString ClassRef =
				Payload->GetStringField(
					TEXT("streaming_class_ref"));
			UWorld* LevelAsset = nullptr;
			UClass* StreamingClass = nullptr;
			if (!TryResolveTypedReference(
					ReferenceResolver,
					World,
					LevelRef,
					LevelAsset,
					OutError)
				|| !TryResolveTypedReference(
					ReferenceResolver,
					World,
					ClassRef,
					StreamingClass,
					OutError))
			{
				return false;
			}
			bool bHasTransform = false;
			FString Unit;
			FTransform ExpectedTransform;
			if (!TryReadCreatePlacement(Payload, bHasTransform, Unit, ExpectedTransform, OutError)) return false;
			int32 MatchCount = 0;
			ULevelStreaming* Relation =
				FindExactStreamingRelation(
					World,
					LevelAsset->GetOutermost()->GetName(),
					*StreamingClass,
					ExpectedTransform,
					MatchCount);
			if (!Relation || MatchCount != 1)
			{
				OutError = CountFailure(MatchCount > 1 ? TEXT("world_simple_create_identity_ambiguous") : TEXT("world_simple_created_target_unresolved"),
					TEXT("The created streaming relation readback requires exactly one matching target."), TEXT("level_asset_ref"), MatchCount, 1);
				OutError.CandidateName = LevelRef;
				OutError.TargetPath = World.GetOutermost()->GetName();
				return false;
			}
			const TSharedRef<FJsonObject> Value =
				MakeShared<FJsonObject>();
			Value->SetStringField(
				TEXT("entity_kind"),
				TEXT("streaming_relation"));
			Value->SetStringField(
				TEXT("level_asset_ref"),
				Relation->GetWorldAssetPackageName()
						== LevelAsset->GetOutermost()->GetName()
					? LevelRef
					: Relation->GetWorldAssetPackageName());
			Value->SetStringField(
				TEXT("streaming_class_ref"),
				Relation->GetClass() == StreamingClass
					? ClassRef
					: Relation->GetClass()->GetPathName());
			Value->SetBoolField(
				TEXT("has_transform"),
				bHasTransform);
			if (bHasTransform)
			{
				const double TranslationScale =
					Unit == TEXT("meter") ? 0.01 : 1.0;
				const FVector Translation =
					Relation->LevelTransform.GetLocation()
					* TranslationScale;
				FQuat Rotation =
					Relation->LevelTransform.GetRotation();
				Rotation.Normalize();
				const FVector Scale =
					Relation->LevelTransform.GetScale3D();
				Value->SetArrayField(
					TEXT("translation"),
					MakeNumberArray({
						Translation.X,
						Translation.Y,
						Translation.Z,
					}));
				Value->SetArrayField(
					TEXT("rotation_quaternion_xyzw"),
					MakeNumberArray({
						Rotation.X,
						Rotation.Y,
						Rotation.Z,
						Rotation.W,
					}));
				Value->SetArrayField(
					TEXT("scale"),
					MakeNumberArray({
						Scale.X,
						Scale.Y,
						Scale.Z,
					}));
				Value->SetStringField(TEXT("unit"), Unit);
			}
			FUeAgentStateSemanticError StateError;
			if (!TryCanonicalJsonObject(
					Value,
					Resources,
					OutValue,
					StateError))
			{
				OutError = StateError;
				return false;
			}
			return true;
		}

		bool TryMakeMembershipValue(
			const EWorldProfileMechanic Mechanic,
			const FString& SubjectRef,
			const TSharedPtr<FJsonObject>& Payload,
			const bool bReadFullState,
			const bool bSatisfied,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			const TSharedRef<FJsonObject> Value =
				MakeShared<FJsonObject>();
			const bool bActorHlod =
				Mechanic
				== EWorldProfileMechanic::ActorHlodMembership;
			Value->SetStringField(
				TEXT("membership_kind"),
				bActorHlod
					? TEXT("actor_hlod_layer")
					: TEXT("data_layer_relation"));
			Value->SetStringField(
				TEXT("subject_ref"),
				SubjectRef);
			if (bReadFullState)
			{
				Value->SetStringField(
					TEXT("mode"),
					TEXT("observed"));
				Value->SetBoolField(
					TEXT("predicate_satisfied"),
					bSatisfied);
				Value->SetStringField(
					TEXT("requested_mode"),
					Payload->GetStringField(
						TEXT("mode")));
				if (bActorHlod)
				{
					FString MemberRef;
					if (Payload->TryGetStringField(
							TEXT("collection_ref"),
							MemberRef)
						&& !MemberRef.IsEmpty())
					{
						Value->SetStringField(
							TEXT("member_ref"),
							MemberRef);
					}
				}
				else
				{
					Value->SetStringField(
						TEXT("relation_kind"),
						Payload->GetStringField(
							TEXT("relation_kind")));
					const TArray<TSharedPtr<FJsonValue>>* Refs =
						nullptr;
					if (Payload->TryGetArrayField(
							TEXT("collection_refs"),
							Refs)
						&& Refs)
					{
						Value->SetArrayField(
							TEXT("member_refs"),
							*Refs);
					}
				}
			}
			else
			{
				Value->SetStringField(
					TEXT("mode"),
					bSatisfied
						? Payload->GetStringField(
							TEXT("mode"))
						: TEXT("observed_mismatch"));
				if (bActorHlod)
				{
					FString MemberRef;
					if (Payload->TryGetStringField(
							TEXT("collection_ref"),
							MemberRef)
						&& !MemberRef.IsEmpty())
					{
						Value->SetStringField(
							TEXT("member_ref"),
							MemberRef);
					}
				}
				else
				{
					Value->SetStringField(
						TEXT("relation_kind"),
						Payload->GetStringField(
							TEXT("relation_kind")));
					const TArray<TSharedPtr<FJsonValue>>* Refs =
						nullptr;
					if (Payload->TryGetArrayField(
							TEXT("collection_refs"),
							Refs)
						&& Refs)
					{
						Value->SetArrayField(
							TEXT("member_refs"),
							*Refs);
					}
				}
			}
			return TryCanonicalJsonObject(
				Value,
				Resources,
				OutValue,
				OutError);
		}

		bool TryMakeDefaultValue(
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			return FUeAgentCanonicalValue::TryObject(
				{},
				Resources,
				OutValue,
				OutError);
		}

		FUeAgentCanonicalType WorldSimpleStateType()
		{
			return FUeAgentCanonicalType(
				TEXT("world_simple_canonical_object"),
				EUeAgentValueKind::Object,
				false,
				TEXT("object"));
		}

		FUeAgentDefaultValue WorldSimpleStateDefault(
			const FUeAgentCanonicalValueRef& Value)
		{
			return FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::Schema,
				TEXT("world-simple:canonical-object-default"),
				TEXT("independent WorldSimple UE API"),
				Value);
		}

		FUeAgentStateDetails WorldSimpleStateDetails(
			const FWorldProfileSpec& Profile)
		{
			return FUeAgentStateDetails(
				Profile.CapabilityId,
				TEXT(""),
				{
					TEXT("World"),
					TEXT("SimpleAuthoring"),
					Profile.StateKind,
				},
				true,
				TEXT("available"),
				{});
		}

		bool SameExactWorld(
			const FUeAgentWorldSimpleEffectWorldCandidate& Candidate,
			const FUeAgentCandidateBinding& Binding,
			const bool bRequireSourceInstance)
		{
			const FUeAgentCanonicalIdentity& Identity =
				Binding.GetSummary().GetIdentity();
			return Candidate.IsValid()
				&& Candidate.GetWorld()
				&& Candidate.GetWorldIdentity()
					== Identity.GetOwnerId()
				&& Candidate.GetWorld()
					->GetOutermost()
					->GetName()
					== Binding.GetPublicRef()
				&& (!bRequireSourceInstance
					|| (Candidate.GetContextRevision()
							== Identity.GetGeneration()
						&& Candidate.GetWorld()->GetUniqueID()
							== Binding.GetCatalogRevision()
								.GetSequence()));
		}

		bool TryResolveExactWorld(
			IUeAgentWorldSimpleEffectWorldCatalog& Catalog,
			const FUeAgentCandidateBinding& Binding,
			const bool bRequireSourceInstance,
			UWorld*& OutWorld,
			FUeAgentDiscoveryError& OutError)
		{
			OutWorld = nullptr;
			TArray<FUeAgentWorldSimpleEffectWorldCandidate> Candidates;
			FExecutionMutationError Error;
			if (!Catalog.TryEnumerate(Candidates, Error))
			{
				OutError = Error;
				if (OutError.IsEmpty())
					OutError = WorldFailure(TEXT("world_simple_state_catalog_unavailable"),
						TEXT("The World catalog could not enumerate worlds and returned no diagnostic."), TEXT("world_catalog"));
				return false;
			}
			for (const FUeAgentWorldSimpleEffectWorldCandidate& Candidate
				: Candidates)
			{
				if (!SameExactWorld(
						Candidate,
						Binding,
						bRequireSourceInstance))
				{
					continue;
				}
				if (OutWorld)
				{
					OutError = CountFailure(TEXT("world_simple_state_world_ambiguous"),
						TEXT("At least two worlds match the exact World binding; enumeration stopped at the second match."), TEXT("world"), 2, 1);
					OutError.SubjectId = Binding.GetBindingId();
					OutError.TargetPath = Binding.GetPublicRef();
					OutWorld = nullptr;
					return false;
				}
				OutWorld = Candidate.GetWorld();
			}
			if (!OutWorld)
			{
				OutError = CountFailure(TEXT("world_simple_state_world_unresolved"),
					TEXT("No world matches the exact World binding."), TEXT("world"), 0, 1);
				OutError.SubjectId = Binding.GetBindingId();
				OutError.TargetPath = Binding.GetPublicRef();
				return false;
			}
			return true;
		}

		class FWorldTargetResolver final
			: public IUeAgentSimpleAuthoringTargetResolver
		{
		public:
			FWorldTargetResolver(
				TSharedRef<
					IUeAgentWorldSimpleEffectWorldCatalog,
					ESPMode::ThreadSafe> InCatalog,
				TSharedRef<
					FUeAgentWorldSimpleEffectExactWorldResolver,
					ESPMode::ThreadSafe> InWorldResolver)
				: Catalog(MoveTemp(InCatalog))
				, WorldResolver(MoveTemp(InWorldResolver))
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
				const FWorldProfileSpec* Profile = FindProfile(CanonicalRequest.GetDescriptorIdentity());
				if (!Profile)
				{
					OutError = MissingProfile(CanonicalRequest.GetDescriptorIdentity());
					return false;
				}
				UWorld* ResolvedWorld = nullptr;
				FExecutionMutationError Error;
				const bool bResolved = WorldResolver->TryResolveWorld(Context, ResolvedWorld, Error);
				if (!bResolved || !ResolvedWorld || !ResolvedWorld->PersistentLevel)
				{
					OutError = Error;
					if (OutError.IsEmpty())
					{
						OutError = WorldFailure(TEXT("world_simple_target_unresolved"),
							!bResolved ? TEXT("The World resolver failed without returning a diagnostic.")
								: !ResolvedWorld ? TEXT("The World resolver returned no world.")
								: TEXT("The resolved world has no persistent level."),
							!bResolved || !ResolvedWorld ? TEXT("world") : TEXT("world/persistent_level"));
						OutError.ActualValuePresent = bResolved && ResolvedWorld && ResolvedWorld->PersistentLevel;
						OutError.ExpectedValuePresent = true;
					}
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Context.GetWorldIdentity();
					return false;
				}
				const FUeAgentCanonicalIdentity Identity =
					TargetIdentity(Context, CanonicalRequest);
				const FUeAgentCandidateBinding Binding(
					TEXT("world-simple:")
						+ CanonicalRequest.GetRequestId(),
					FUeAgentCandidateSummary(
						Identity,
						CanonicalRequest
							.GetDescriptorIdentity(),
						TEXT("world_simple"),
						CanonicalRequest
							.GetDescriptorIdentity()
							.GetDescriptorId(),
						TEXT(
							"exact WorldSimple operation target"),
						TArray<FString>{
							TEXT("World"),
							TEXT("SimpleAuthoring"),
						}),
					FUeAgentCatalogRevision(
						TEXT("world-simple-exact-context"),
						Context.GetContextRevision(),
						ResolvedWorld->GetUniqueID()),
					ResolvedWorld->GetOutermost()->GetName(),
					PersistedStatePortId,
					TArray<EUeAgentCandidateOperation>{
						Profile->Operation,
					},
					FString(RequestEvidencePrefix)
						+ CanonicalRequest
							.GetPayload()
							.GetJsonValue());
				OutTarget.Emplace(Binding);
				if (!OutTarget->IsValid())
				{
					OutError = *OutTarget->GetValidationDiagnostic();
					OutTarget.Reset();
					return false;
				}
				return true;
			}

		private:
			TSharedRef<
				IUeAgentWorldSimpleEffectWorldCatalog,
				ESPMode::ThreadSafe> Catalog;
			TSharedRef<
				FUeAgentWorldSimpleEffectExactWorldResolver,
				ESPMode::ThreadSafe> WorldResolver;
		};

		class FWorldStateReadPort final
			: public IUeAgentStateReadPort
		{
		public:
			FWorldStateReadPort(
				FString InPortId,
				const EUeAgentStatePlane InRequiredPlane,
				TSharedRef<
					IUeAgentWorldSimpleEffectWorldCatalog,
					ESPMode::ThreadSafe> InCatalog,
				TSharedRef<
					IUeAgentWorldSimpleEffectReferenceResolver,
					ESPMode::ThreadSafe> InReferenceResolver,
				const bool bInRequireSourceInstance)
				: PortId(MoveTemp(InPortId))
				, RequiredPlane(InRequiredPlane)
				, Catalog(MoveTemp(InCatalog))
				, ReferenceResolver(
					MoveTemp(InReferenceResolver))
				, bRequireSourceInstance(
					bInRequireSourceInstance)
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
				if (!Request.IsValid())
				{
					OutError = *Request.GetBinding().GetValidationDiagnostic();
					return false;
				}
				if (Request.GetStatePlane() != RequiredPlane)
				{
					OutError = WorldFailure(TEXT("world_simple_state_request_invalid"),
						TEXT("The World read port does not serve the requested state plane."), TEXT("state_plane"));
					FString Actual = Contract::LexToString(Request.GetStatePlane());
					if (Actual.IsEmpty()) Actual = FString::FromInt(static_cast<int32>(Request.GetStatePlane()));
					OutError.ActualValue = MakeShared<FJsonValueString>(Actual);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Contract::LexToString(RequiredPlane));
					OutError.TargetPath = Request.GetBinding().GetPublicRef();
					return false;
				}
				const FWorldProfileSpec* Profile = FindProfile(Request.GetBinding().GetSummary().GetDescriptorIdentity());
				if (!Profile)
				{
					OutError = MissingProfile(Request.GetBinding().GetSummary().GetDescriptorIdentity());
					return false;
				}
				if (Cancellation.IsCancelled())
				{
					OutError = WorldFailure(TEXT("world_simple_state_request_cancelled"),
						TEXT("The World state read was cancelled before resolving its target."), TEXT("cancellation"));
					OutError.TargetPath = Request.GetBinding().GetPublicRef();
					return false;
				}
				const FUeAgentCandidateBinding& Binding =
					Request.GetBinding();
				TSharedPtr<FJsonObject> Payload;
				if (!TryParseRequestEvidence(
						Binding,
						Payload,
						OutError))
				{
					return false;
				}

				UWorld* ResolvedWorld = nullptr;
				if (!TryResolveExactWorld(
						*Catalog,
						Binding,
						bRequireSourceInstance,
						ResolvedWorld,
						OutError))
				{
					return false;
				}

				FUeAgentCanonicalValuePtr DefaultValue;
				FUeAgentStateSemanticError StateError;
				if (!TryMakeDefaultValue(
						Resources,
						DefaultValue,
						StateError))
				{
					OutError = StateError;
					return false;
				}
				const FUeAgentStateRevision Revision(
					PortId,
					Request.GetBinding()
						.GetSummary()
						.GetIdentity()
						.GetGeneration(),
					ResolvedWorld->GetUniqueID());
				TArray<FUeAgentStateEntry> Entries;
				const bool bAggregateTarget =
					Binding.GetSummary()
						.GetIdentity()
						.GetTargetKind()
					== TEXT("world.simple.operation");
				const auto AddStateEntry =
					[&](
						const FString& Selector,
						const FUeAgentCanonicalValueRef&
							CurrentValue)
					{
						const FUeAgentCanonicalIdentity Identity =
							bAggregateTarget
								? StateIdentity(
									*Profile,
									Binding,
									Selector)
								: Binding.GetSummary()
									.GetIdentity();
						Entries.Emplace(
							Identity,
							WorldSimpleStateType(),
							CurrentValue,
							WorldSimpleStateDefault(
								DefaultValue.ToSharedRef()),
							WorldSimpleStateDetails(*Profile),
							Revision,
							EUeAgentAuthoredPresence::Present,
							EUeAgentOverrideState::Overridden);
					};
				if (Profile->Operation
					== EUeAgentCandidateOperation::Create)
				{
					if (bAggregateTarget)
					{
						if (!TryCheckCreateTargetAbsent(
								*Profile,
								*ResolvedWorld,
								*ReferenceResolver,
								Payload,
								OutError))
						{
							return false;
						}
					}
					else
					{
						const FString Selector =
							Binding.GetSummary()
								.GetIdentity()
								.GetSelector();
						FUeAgentCanonicalValuePtr CurrentValue;
						bool bRead = false;
						switch (Profile->Mechanic)
						{
						case EWorldProfileMechanic::ActorCreate:
							bRead = TryMakeActorCreateValue(
								*ResolvedWorld,
								*ReferenceResolver,
								Payload,
								Selector,
								Resources,
								CurrentValue,
								OutError);
							break;
						case EWorldProfileMechanic::WallCreate:
							bRead = TryMakeWallCreateValue(
								*ResolvedWorld,
								*ReferenceResolver,
								Payload,
								Selector,
								Resources,
								CurrentValue,
								OutError);
							break;
						case EWorldProfileMechanic::DataLayerCreate:
							bRead = TryMakeDataLayerCreateValue(
								*ResolvedWorld,
								*ReferenceResolver,
								Payload,
								Resources,
								CurrentValue,
								OutError);
							break;
						case EWorldProfileMechanic::
							StreamingRelationCreate:
							bRead = TryMakeStreamingCreateValue(
								*ResolvedWorld,
								*ReferenceResolver,
								Payload,
								Resources,
								CurrentValue,
								OutError);
							break;
						default:
							OutError = WorldFailure(TEXT("world_simple_create_reader_missing"),
								TEXT("The World profile has no reader for its create mechanic."), TEXT("profile/mechanic"));
							OutError.CandidateName = Profile->DescriptorId;
							OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Profile->Mechanic));
							OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("ActorCreate, WallCreate, DataLayerCreate or StreamingRelationCreate"));
							break;
						}
						if (!bRead)
						{
							return false;
						}
						AddStateEntry(
							Selector,
							CurrentValue.ToSharedRef());
					}
				}
				else if (Profile->Mechanic
					== EWorldProfileMechanic::LevelGameModeState)
				{
					FUeAgentCanonicalValuePtr CurrentValue;
					if (!TryMakeLevelGameModeValue(
							*ResolvedWorld,
							Resources,
							CurrentValue,
							StateError))
					{
						OutError = StateError;
						return false;
					}
					const FUeAgentCanonicalIdentity Identity =
						bAggregateTarget
							? StateIdentity(
								*Profile,
								Binding,
								Binding.GetSummary()
									.GetIdentity()
									.GetOwnerId())
							: Binding.GetSummary().GetIdentity();
					Entries.Emplace(
						Identity,
						WorldSimpleStateType(),
						CurrentValue.ToSharedRef(),
						WorldSimpleStateDefault(
							DefaultValue.ToSharedRef()),
						WorldSimpleStateDetails(*Profile),
						Revision,
						EUeAgentAuthoredPresence::Present,
						EUeAgentOverrideState::Overridden);
				}
				else if (Profile->Mechanic
					== EWorldProfileMechanic::
						WorldPartitionStreamingState)
				{
					UWorldPartition* WorldPartition =
						ResolvedWorld->GetWorldPartition();
					if (!WorldPartition)
					{
						OutError = WorldFailure(TEXT("world_simple_world_partition_missing"),
							TEXT("The target world has no WorldPartition object."), TEXT("world_partition"));
						OutError.TargetPath = ResolvedWorld->GetOutermost()->GetName();
						OutError.ActualValuePresent = false;
						OutError.ExpectedValuePresent = true;
						return false;
					}
					FUeAgentCanonicalValuePtr CurrentValue;
					if (!TryMakeWorldPartitionStreamingValue(
							WorldPartition
								->IsStreamingEnabledInEditor(),
							Resources,
							CurrentValue,
							StateError))
					{
						OutError = StateError;
						return false;
					}
					const FUeAgentCanonicalIdentity Identity =
						bAggregateTarget
							? StateIdentity(
								*Profile,
								Binding,
								Binding.GetSummary()
									.GetIdentity()
									.GetOwnerId())
							: Binding.GetSummary()
								.GetIdentity();
					Entries.Emplace(
						Identity,
						WorldSimpleStateType(),
						CurrentValue.ToSharedRef(),
						WorldSimpleStateDefault(
							DefaultValue.ToSharedRef()),
						WorldSimpleStateDetails(*Profile),
						Revision,
						EUeAgentAuthoredPresence::Present,
						EUeAgentOverrideState::Overridden);
				}
				else if (
					Profile->Mechanic
					== EWorldProfileMechanic::
						ActorStreamingState)
				{
					const TArray<FString> Selectors =
						bAggregateTarget
							? ReadStringArray(
								Payload,
								TEXT("target_refs"))
							: TArray<FString>{
								Binding.GetSummary()
									.GetIdentity()
									.GetSelector(),
							};
					if (Selectors.IsEmpty())
					{
						const auto Raw = Payload->Values.FindRef(TEXT("target_refs"));
						OutError = Raw.IsValid() && Raw->Type == EJson::Array && !Raw->AsArray().IsEmpty()
							? JsonValueFailure(TEXT("target_refs/0"), Raw->AsArray()[0], TEXT("nonempty reference string; no usable selectors were read"))
							: JsonFieldFailure(Payload, TEXT("target_refs"), TEXT("array containing at least one nonempty reference"));
						OutError.TargetPath = Binding.GetPublicRef();
						return false;
					}
					for (const FString& Selector : Selectors)
					{
						AActor* ResolvedActor = nullptr;
						if (!TryResolveTypedReference(*ReferenceResolver, *ResolvedWorld, Selector, ResolvedActor, OutError))
						{
							return false;
						}
						FUeAgentCanonicalValuePtr CurrentValue;
						if (!TryMakeActorStreamingPolicyValue(
								*ResolvedActor,
								Selector,
								Payload,
								bAggregateTarget,
								Resources,
								CurrentValue,
								StateError))
						{
							OutError = StateError;
							return false;
						}
						const FUeAgentCanonicalIdentity Identity =
							bAggregateTarget
								? StateIdentity(
									*Profile,
									Binding,
									Selector)
								: Binding.GetSummary()
									.GetIdentity();
						Entries.Emplace(
							Identity,
							WorldSimpleStateType(),
							CurrentValue.ToSharedRef(),
							WorldSimpleStateDefault(
								DefaultValue.ToSharedRef()),
							WorldSimpleStateDetails(*Profile),
							Revision,
							EUeAgentAuthoredPresence::Present,
							EUeAgentOverrideState::Overridden);
					}
				}
				else if (
					Profile->Mechanic
					== EWorldProfileMechanic::DataLayerState)
				{
					const TArray<FString> Selectors =
						bAggregateTarget
							? ReadStringArray(
								Payload,
								TEXT("target_refs"))
							: TArray<FString>{
								Binding.GetSummary()
									.GetIdentity()
									.GetSelector(),
							};
					if (Selectors.IsEmpty())
					{
						const auto Raw = Payload->Values.FindRef(TEXT("target_refs"));
						OutError = Raw.IsValid() && Raw->Type == EJson::Array && !Raw->AsArray().IsEmpty()
							? JsonValueFailure(TEXT("target_refs/0"), Raw->AsArray()[0], TEXT("nonempty reference string; no usable selectors were read"))
							: JsonFieldFailure(Payload, TEXT("target_refs"), TEXT("array containing at least one nonempty reference"));
						OutError.TargetPath = Binding.GetPublicRef();
						return false;
					}
					for (const FString& Selector : Selectors)
					{
						UDataLayerInstance* Instance = nullptr;
						if (!TryResolveTypedReference(
								*ReferenceResolver,
								*ResolvedWorld,
								Selector,
								Instance,
								OutError))
						{
							return false;
						}
						FUeAgentCanonicalValuePtr CurrentValue;
						if (!TryMakeDataLayerStateValue(
								*Instance,
								Selector,
								Payload,
								bAggregateTarget,
								Resources,
								CurrentValue,
								StateError))
						{
							OutError = StateError;
							return false;
						}
						AddStateEntry(
							Selector,
							CurrentValue.ToSharedRef());
					}
				}
				else if (
					Profile->Mechanic
					== EWorldProfileMechanic::
						StreamingRelationState)
				{
					TArray<FString> Selectors;
					if (bAggregateTarget)
					{
						FString TargetRef;
						if (Payload->TryGetStringField(
								TEXT("target_ref"),
								TargetRef)
							&& !TargetRef.IsEmpty())
						{
							Selectors.Add(MoveTemp(TargetRef));
						}
					}
					else
					{
						Selectors.Add(
							Binding.GetSummary()
								.GetIdentity()
								.GetSelector());
					}
					if (Selectors.IsEmpty())
					{
						OutError = JsonFieldFailure(Payload, TEXT("target_ref"), TEXT("nonempty reference string"));
						OutError.TargetPath = Binding.GetPublicRef();
						return false;
					}
					for (const FString& Selector : Selectors)
					{
						ULevelStreaming* Relation = nullptr;
						if (!TryResolveTypedReference(
								*ReferenceResolver,
								*ResolvedWorld,
								Selector,
								Relation,
								OutError))
						{
							return false;
						}
						FUeAgentCanonicalValuePtr CurrentValue;
						if (!TryMakeStreamingRelationStateValue(
								*Relation,
								Selector,
								Payload,
								bAggregateTarget,
								Resources,
								CurrentValue,
								StateError))
						{
							OutError = StateError;
							return false;
						}
						AddStateEntry(
							Selector,
							CurrentValue.ToSharedRef());
					}
				}
				else if (
					Profile->Mechanic
						== EWorldProfileMechanic::
							ActorHlodMembership
					|| Profile->Mechanic
						== EWorldProfileMechanic::
							DataLayerMembership)
				{
					const TArray<FString> Selectors =
						bAggregateTarget
							? ReadStringArray(
								Payload,
								TEXT("subject_refs"))
							: TArray<FString>{
								Binding.GetSummary()
									.GetIdentity()
									.GetSelector(),
							};
					if (Selectors.IsEmpty())
					{
						const auto Raw = Payload->Values.FindRef(TEXT("subject_refs"));
						OutError = Raw.IsValid() && Raw->Type == EJson::Array && !Raw->AsArray().IsEmpty()
							? JsonValueFailure(TEXT("subject_refs/0"), Raw->AsArray()[0], TEXT("nonempty reference string; no usable selectors were read"))
							: JsonFieldFailure(Payload, TEXT("subject_refs"), TEXT("array containing at least one nonempty reference"));
						OutError.TargetPath = Binding.GetPublicRef();
						return false;
					}
					for (const FString& Selector : Selectors)
					{
						bool bSatisfied = false;
						if (Profile->Mechanic
							== EWorldProfileMechanic::
								ActorHlodMembership)
						{
							AActor* Actor = nullptr;
							if (!TryResolveTypedReference(
									*ReferenceResolver,
									*ResolvedWorld,
									Selector,
									Actor,
									OutError))
							{
								return false;
							}
							const FString Mode =
								Payload->GetStringField(
									TEXT("mode"));
							FString MemberRef;
							Payload->TryGetStringField(
								TEXT("collection_ref"),
								MemberRef);
							if (Mode == TEXT("clear")
								|| MemberRef.IsEmpty())
							{
								bSatisfied =
									Actor->GetHLODLayer()
									== nullptr;
							}
							else
							{
								UHLODLayer* Expected = nullptr;
								if (!TryResolveTypedReference(
										*ReferenceResolver,
										*ResolvedWorld,
										MemberRef,
										Expected,
										OutError))
								{
									return false;
								}
								bSatisfied =
									Actor->GetHLODLayer()
									== Expected;
							}
						}
						else
						{
							const FString RelationKind =
								Payload->GetStringField(
									TEXT("relation_kind"));
							const FString Mode =
								Payload->GetStringField(
									TEXT("mode"));
							const TArray<FString> MemberRefs =
								ReadStringArray(
									Payload,
									TEXT("collection_refs"));
							if (RelationKind == TEXT("parent"))
							{
								UDataLayerInstance* Subject =
									nullptr;
								if (!TryResolveTypedReference(
										*ReferenceResolver,
										*ResolvedWorld,
										Selector,
										Subject,
										OutError))
								{
									return false;
								}
								UDataLayerInstance* Expected =
									nullptr;
								if (!MemberRefs.IsEmpty()
									&& !TryResolveTypedReference(
										*ReferenceResolver,
										*ResolvedWorld,
										MemberRefs[0],
										Expected,
										OutError))
								{
									return false;
								}
								if (Mode == TEXT("remove"))
								{
									bSatisfied =
										Subject->GetParent()
										!= Expected;
								}
								else if (
									Mode == TEXT("clear"))
								{
									bSatisfied =
										Subject->GetParent()
										== nullptr;
								}
								else
								{
									bSatisfied =
										Subject->GetParent()
										== Expected;
								}
							}
							else
							{
								AActor* Actor = nullptr;
								if (!TryResolveTypedReference(
										*ReferenceResolver,
										*ResolvedWorld,
										Selector,
										Actor,
										OutError))
								{
									return false;
								}
								TSet<FString> Expected;
								for (const FString& MemberRef
									: MemberRefs)
								{
									UDataLayerInstance* Member =
										nullptr;
									if (!TryResolveTypedReference(
											*ReferenceResolver,
											*ResolvedWorld,
											MemberRef,
											Member,
											OutError))
									{
										return false;
									}
									const UDataLayerAsset* Asset =
										Member->GetAsset();
									if (!Asset)
									{
										OutError = WorldFailure(TEXT("world_simple_data_layer_asset_missing"),
											TEXT("The referenced data-layer member has no asset."), TEXT("data_layer/asset"));
										OutError.TargetPath = MemberRef;
										OutError.ActualValuePresent = false;
										OutError.ExpectedValuePresent = true;
										return false;
									}
									Expected.Add(Asset->GetPathName());
								}
								// Actor membership is serialized as direct
								// DataLayerAsset references. The instance view is
								// derived through an initialized WorldPartition
								// manager and is not a persisted-state oracle.
								TSet<FString> Actual;
								for (const UDataLayerAsset* Asset
									: Actor->GetDataLayerAssets(false))
								{
									if (Asset)
									{
										Actual.Add(Asset->GetPathName());
									}
								}
								const auto ContainsAll =
									[&Actual](
										const TSet<FString>& Required)
									{
										for (const FString& Member
											: Required)
										{
											if (!Actual.Contains(
													Member))
											{
												return false;
											}
										}
										return true;
									};
								if (Mode == TEXT("clear"))
								{
									bSatisfied =
										Actual.IsEmpty();
								}
								else if (
									Mode == TEXT("replace"))
								{
									bSatisfied =
										Actual.Num()
											== Expected.Num()
										&& ContainsAll(Expected);
								}
								else if (
									Mode == TEXT("remove"))
								{
									bSatisfied = true;
									for (const FString& Member
										: Expected)
									{
										bSatisfied =
											bSatisfied
											&& !Actual.Contains(
												Member);
									}
								}
								else
								{
									bSatisfied =
										ContainsAll(Expected);
								}
							}
						}
						FUeAgentCanonicalValuePtr CurrentValue;
						if (!TryMakeMembershipValue(
								Profile->Mechanic,
								Selector,
								Payload,
								bAggregateTarget,
								bSatisfied,
								Resources,
								CurrentValue,
								StateError))
						{
							OutError = StateError;
							return false;
						}
						AddStateEntry(
							Selector,
							CurrentValue.ToSharedRef());
					}
				}
				else if (
					!bAggregateTarget
					|| Profile->Operation
						!= EUeAgentCandidateOperation::Create)
				{
					OutError = WorldFailure(TEXT("world_simple_profile_state_reader_missing"),
						TEXT("The World profile has no state reader for this target and operation."), TEXT("profile"));
					OutError.CandidateName = Profile->DescriptorId;
					OutError.TargetPath = Binding.GetPublicRef();
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetNumberField(TEXT("mechanic"), static_cast<int32>(Profile->Mechanic));
					Actual->SetNumberField(TEXT("operation"), static_cast<int32>(Profile->Operation));
					Actual->SetBoolField(TEXT("aggregate_target"), bAggregateTarget);
					OutError.ActualValue = MakeShared<FJsonValueObject>(Actual);
					return false;
				}
				FUeAgentCanonicalSnapshotPtr Snapshot;
				if (!FUeAgentCanonicalSnapshot::TryCreate(
						TEXT("snapshot:")
							+ PortId
							+ TEXT(":")
							+ Request.GetBinding()
								.GetBindingId(),
						RequiredPlane,
						Revision,
						FUeAgentTemporalPoint(
							TEXT("ue-world-simple"),
							FString::FromInt(
								ResolvedWorld
									->GetUniqueID())),
						FUeAgentStateProvenance(
							TEXT("independent_readback"),
							PortId,
							TEXT(
								"world-simple:"
								"typed-ue-state")),
						MoveTemp(Entries),
						Resources,
						Snapshot,
						StateError))
				{
					OutError = StateError;
					return false;
				}
				OutSnapshot = MoveTemp(Snapshot);
				return true;
			}

		private:
			FString PortId;
			EUeAgentStatePlane RequiredPlane =
				EUeAgentStatePlane::AuthoringMemory;
			TSharedRef<
				IUeAgentWorldSimpleEffectWorldCatalog,
				ESPMode::ThreadSafe> Catalog;
			TSharedRef<
				IUeAgentWorldSimpleEffectReferenceResolver,
				ESPMode::ThreadSafe> ReferenceResolver;
			bool bRequireSourceInstance = true;
		};


		class FPersistedWorldCatalog final
			: public IUeAgentWorldSimpleEffectWorldCatalog
		{
		public:
			FPersistedWorldCatalog(
				UWorld& InWorld,
				FString InWorldIdentity,
				FString InContextRevision)
				: World(&InWorld)
				, WorldIdentity(MoveTemp(InWorldIdentity))
				, ContextRevision(MoveTemp(InContextRevision))
			{
			}

			bool TryEnumerate(
				TArray<FUeAgentWorldSimpleEffectWorldCandidate>&
					OutCandidates,
				FExecutionMutationError& OutError) override
			{
				OutCandidates.Reset();
				OutError = FExecutionMutationError();
				if (!World.IsValid() || !World->PersistentLevel || WorldIdentity.IsEmpty() || ContextRevision.IsEmpty())
				{
					const TCHAR* Field = !World.IsValid() ? TEXT("world") : !World->PersistentLevel
						? TEXT("world/persistent_level") : WorldIdentity.IsEmpty() ? TEXT("world_identity") : TEXT("context_revision");
					OutError = WorldFailure(TEXT("world_simple_persisted_catalog_invalid"),
						FString::Printf(TEXT("The persisted World world catalog has no valid '%s'."), Field), Field);
					if (!World.IsValid() || !World->PersistentLevel)
					{
						OutError.ActualValuePresent = false;
						OutError.ExpectedValuePresent = true;
					}
					else
					{
						OutError.ActualEvidence = MakeShared<FJsonValueString>(WorldIdentity.IsEmpty() ? WorldIdentity : ContextRevision);
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty identifier"));
					}
					return false;
				}
				OutCandidates.Emplace(
					World.Get(),
					WorldIdentity,
					World->PersistentLevel->GetPathName(),
					EUeAgentExecutionWorldType::Preview,
					TOptional<int32>(),
					ContextRevision);
				return true;
			}

		private:
			TWeakObjectPtr<UWorld> World;
			FString WorldIdentity;
			FString ContextRevision;
		};

		class FPersistedWorldReferenceResolver final
			: public IUeAgentWorldSimpleEffectReferenceResolver
		{
		public:
			FPersistedWorldReferenceResolver(
				TSharedRef<
					IUeAgentWorldSimpleEffectReferenceResolver,
					ESPMode::ThreadSafe> InSourceResolver,
				FPersistedWorldReferenceMap InReferenceMap)
				: SourceResolver(MoveTemp(InSourceResolver))
				, ReferenceMap(MoveTemp(InReferenceMap))
			{
			}

			bool TryResolveReference(
				UWorld& World,
				const FString& CanonicalRef,
				UObject*& OutObject,
				FExecutionMutationError& OutError) override
			{
				FString SnapshotRef;
				if (!ReferenceMap.TryMapReference(
						CanonicalRef,
						SnapshotRef))
				{
					return SourceResolver->TryResolveReference(
						World,
						CanonicalRef,
						OutObject,
						OutError);
				}
				FExecutionMutationError ResolveError;
				if (SourceResolver->TryResolveReference(
						World,
						SnapshotRef,
						OutObject,
						ResolveError))
				{
					return true;
				}
				OutObject = nullptr;
				OutError = ResolveError;
				if (OutError.IsEmpty())
					OutError = WorldFailure(TEXT("world_simple_persisted_reference_unresolved"),
						TEXT("The persisted World reference resolver failed without returning a diagnostic."), TEXT("reference"));
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = CanonicalRef;
				return false;
			}

		private:
			TSharedRef<
				IUeAgentWorldSimpleEffectReferenceResolver,
				ESPMode::ThreadSafe> SourceResolver;
			FPersistedWorldReferenceMap ReferenceMap;
		};

		class FWorldArtifactPersistedStateOracle final
			: public IPersistedStateOracle
		{
		public:
			explicit FWorldArtifactPersistedStateOracle(
				TSharedRef<
					IUeAgentWorldSimpleEffectReferenceResolver,
					ESPMode::ThreadSafe> InReferenceResolver)
				: ReferenceResolver(MoveTemp(InReferenceResolver))
			{
			}

			const FString& GetReadPortId() const override
			{
				return ReadPortId;
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
				if (!Target.IsValid())
				{
					OutError = *Target.GetValidationDiagnostic();
					return false;
				}
				if (Artifacts.IsEmpty())
				{
					OutError = CountFailure(TEXT("world_simple_persisted_read_request_invalid"),
						TEXT("The World persisted-state read requires at least one durable artifact."), TEXT("artifacts"), 0, 1);
					return false;
				}
				if (Target.GetBinding().GetReadPortId() != ReadPortId)
				{
					OutError = WorldFailure(TEXT("world_simple_persisted_read_request_invalid"),
						TEXT("The World verification target uses a different persisted-state read port."), TEXT("read_port_id"));
					OutError.ActualValue = MakeShared<FJsonValueString>(Target.GetBinding().GetReadPortId());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(ReadPortId);
					return false;
				}
				if (Cancellation.IsCancelled())
				{
					OutError = WorldFailure(TEXT("world_simple_persisted_read_request_invalid"),
						TEXT("The World persisted-state read was cancelled before reading its evidence."), TEXT("cancellation"));
					return false;
				}

				FUeAgentCanonicalSnapshotPtr Snapshot;
				FPersistedWorldLoadError LoadError;
				const bool bObserved = TryObservePersistedWorld(
					Artifacts,
					[&](UWorld& SnapshotWorld,
						const FPersistedWorldReferenceMap& ReferenceMap,
						FPersistedWorldLoadError& ObserverError)
					{
						if (Cancellation.IsCancelled())
						{
							ObserverError = WorldFailure(TEXT("world_simple_persisted_read_cancelled"),
								TEXT("The persisted World read was cancelled after loading the snapshot world."), TEXT("cancellation"));
							ObserverError.TargetPath = Target.GetBinding().GetPublicRef();
							return false;
						}

						const FUeAgentCandidateBinding& SourceBinding =
							Target.GetBinding();
						const FString SnapshotWorldPackage =
							SnapshotWorld.GetOutermost()->GetName();
						const FString SnapshotToken =
							FGuid::NewGuid().ToString(EGuidFormats::Digits);
						const FUeAgentCandidateBinding SnapshotBinding(
							SourceBinding.GetBindingId()
								+ TEXT(":artifact:")
								+ SnapshotToken,
							SourceBinding.GetSummary(),
							SourceBinding.GetCatalogRevision(),
							SnapshotWorldPackage,
							ReadPortId,
							SourceBinding.GetSupportedOperations(),
							SourceBinding.GetBindingEvidence());
						const TSharedRef<FPersistedWorldCatalog,
							ESPMode::ThreadSafe> Catalog = MakeShared<
								FPersistedWorldCatalog,
								ESPMode::ThreadSafe>(
								SnapshotWorld,
								SourceBinding.GetSummary()
									.GetIdentity()
									.GetOwnerId(),
								SourceBinding.GetSummary()
									.GetIdentity()
									.GetGeneration());
						const TSharedRef<
							FPersistedWorldReferenceResolver,
							ESPMode::ThreadSafe> PersistedResolver = MakeShared<
							FPersistedWorldReferenceResolver,
							ESPMode::ThreadSafe>(
								ReferenceResolver,
								ReferenceMap);
						FWorldStateReadPort Reader(
							ReadPortId,
							EUeAgentStatePlane::Persisted,
							Catalog,
							PersistedResolver,
							false);
						FUeAgentDiscoveryError ReadError;
						if (!Reader.TryRead(
								FUeAgentStateReadRequest(
									SnapshotBinding,
									EUeAgentStatePlane::Persisted),
								Resources,
								Deadline,
								Cancellation,
								Snapshot,
								ReadError))
						{
							ObserverError = ReadError;
							if (ObserverError.IsEmpty())
								ObserverError = WorldFailure(TEXT("world_simple_persisted_state_read_failed"),
									TEXT("The persisted World state reader failed without returning a diagnostic."), TEXT("snapshot"));
							if (ObserverError.TargetPath.IsEmpty()) ObserverError.TargetPath = SourceBinding.GetPublicRef();
							return false;
						}
						return true;
					},
					LoadError);
				if (!bObserved)
				{
					OutError = LoadError;
					if (OutError.IsEmpty())
						OutError = WorldFailure(TEXT("world_simple_persisted_state_read_failed"),
							TEXT("The persisted World observer failed without returning a diagnostic."), TEXT("persisted_world"));
					return false;
				}

				OutSnapshot = MoveTemp(Snapshot);
				return true;
			}

		private:
			FString ReadPortId = PersistedStatePortId;
			TSharedRef<
				IUeAgentWorldSimpleEffectReferenceResolver,
				ESPMode::ThreadSafe> ReferenceResolver;
		};

		class FWorldDraftBuilder final
			: public IUeAgentSimpleAuthoringDraftBuilder
		{
		public:
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
				if (!TryBuildWorldSimpleEffectChangeSetDraft(
						CanonicalRequest,
						Context,
						PreState,
						Resources,
						OutDraft,
						Error))
				{
					OutError = Error;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = CanonicalRequest.GetRequestId();
					return false;
				}
				return true;
			}
		};

		class FWorldChangeTargetProjector final
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
				const TArray<FChangeDraft>& Changes =
					Draft.GetChanges();
				for (int32 Index = 0; Index < Changes.Num(); ++Index)
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
							TEXT("world_simple"),
							Change.GetIdentity()
								.GetSelector(),
							TEXT(
								"exact WorldSimple change target"),
							TArray<FString>{
								TEXT("World"),
								TEXT("SimpleAuthoring"),
							}),
						ResolvedTarget.GetCatalogRevision(),
						ResolvedTarget.GetPublicRef(),
						ResolvedTarget.GetReadPortId(),
						TArray<EUeAgentCandidateOperation>{
							CandidateOperation(
								Change.GetIntent()),
						},
						ResolvedTarget.GetBindingEvidence());
					if (!OutTargets.Last().IsValid())
					{
						OutError = *OutTargets.Last().GetValidationDiagnostic();
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = OutTargets.Last().GetPublicRef();
						OutTargets.Reset();
						return false;
					}
				}
				if (OutTargets.IsEmpty())
				{
					OutError = CountFailure(TEXT("world_simple_change_target_invalid"),
						TEXT("At least one World change target is required."), TEXT("change_targets"), 0, 1);
					return false;
				}
				return true;
			}
		};

		class FWorldMutationBindingProjector final
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
				if (Targets.Num() != Changes.Num())
				{
					OutError = CountFailure(TEXT("world_simple_change_targets_missing"),
						TEXT("The number of World change targets differs from the planned changes."), TEXT("change_targets"), Targets.Num(), Changes.Num());
					OutError.SubjectId = Plan->GetRequestId();
					return false;
				}
				for (int32 Index = 0; Index < Changes.Num(); ++Index)
				{
					const FPlannedChange& Change = Changes[Index];
					OutBindings.Emplace(
						Targets[Index],
						TEXT(
							"UAI.WorldSimple:"
							"CanonicalObject"),
						FUeAgentCanonicalType(
							TEXT(
								"world_simple_"
								"canonical_object"),
							EUeAgentValueKind::Object,
							false,
							TEXT("object")),
						TEXT(
							"world-simple:typed-native-binding"));
					if (Targets[Index].GetSummary().GetIdentity().GetCanonicalKey() != Change.GetIdentity().GetCanonicalKey())
					{
						OutError = WorldFailure(TEXT("world_simple_mutation_binding_invalid"),
							TEXT("The World mutation target identity differs from its planned change."),
							FString::Printf(TEXT("targets/%d/identity"), Index));
						OutError.TargetPath = Targets[Index].GetPublicRef();
						OutError.ActualEvidence = MakeShared<FJsonValueString>(Targets[Index].GetSummary().GetIdentity().GetCanonicalKey());
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Change.GetIdentity().GetCanonicalKey());
						OutBindings.Reset();
						return false;
					}
					if (!OutBindings.Last().IsValid())
					{
						OutError = OutBindings.Last().GetValidationDiagnostic();
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Targets[Index].GetPublicRef();
						OutBindings.Reset();
						return false;
					}
				}
				if (OutBindings.IsEmpty())
				{
					OutError = CountFailure(TEXT("world_simple_mutation_binding_invalid"),
						TEXT("At least one World mutation binding is required."), TEXT("mutation_bindings"), 0, 1);
					return false;
				}
				return true;
			}
		};

		class FWorldDurableTargetProjector final
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
				if (Changes.IsEmpty())
				{
					OutError = CountFailure(TEXT("world_simple_durable_target_missing"),
						TEXT("A World durable target requires at least one planned change."), TEXT("changes"), 0, 1);
					OutError.SubjectId = Target.GetBindingId();
					return false;
				}
				if (Target.GetPublicRef().IsEmpty())
				{
					OutError = WorldFailure(TEXT("world_simple_durable_target_missing"),
						TEXT("The World durable target has an empty public reference."), TEXT("target_ref"));
					OutError.ActualValue = MakeShared<FJsonValueString>(Target.GetPublicRef());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty package reference"));
					OutError.SubjectId = Target.GetBindingId();
					return false;
				}
				OutTargets.Emplace(
					Changes[0].GetIdentity(),
					Target.GetPublicRef(),
					EPackageHeaderKind::Map,
					EAssetDurableIntent::Save,
					ERollbackGrade::DurableBackup);
				if (!OutTargets[0].IsValid())
				{
					OutError = OutTargets[0].GetValidationDiagnostic();
					return false;
				}
				return true;
			}
		};

		class FWorldExpectedStateProjector final
			: public IUeAgentSimpleAuthoringExpectedStateProjector
		{
		public:
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
				const FUeAgentStateRevision Revision(
					TEXT(
						"world-simple-operation-expected"),
					Plan->GetFingerprint(),
					1);
				FUeAgentStateSemanticError StateError;
				FUeAgentCanonicalValuePtr DefaultValue;
				if (!TryMakeDefaultValue(
						Resources,
						DefaultValue,
						StateError))
				{
					OutError = StateError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Plan->GetRequestId();
					return false;
				}
				const FWorldProfileSpec* Profile =
					FindProfile(
						Plan->GetDescriptorIdentity());
				if (!Profile)
				{
					OutError = MissingProfile(Plan->GetDescriptorIdentity());
					return false;
				}
				TArray<FString> ArtifactIds;
				for (const FDurableArtifact& Artifact :
					Inventory.GetArtifacts())
				{
					ArtifactIds.Add(Artifact.GetArtifactId());
				}
				for (const FPlannedChange& Change :
					Plan->GetChangeSet().GetChanges())
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
						OutError = WorldFailure(TEXT("world_simple_verification_target_missing"),
							TEXT("No resolved World target matches the planned change to verify."), TEXT("change_targets"));
						OutError.TargetPath = ResolvedTarget.GetPublicRef();
						OutError.ActualValuePresent = false;
						OutError.ExpectedValuePresent = true;
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Change.GetIdentity().GetCanonicalKey());
						return false;
					}
					if (!Change.GetAfter().IsValid())
					{
						OutError = WorldFailure(TEXT("world_simple_expected_state_missing"),
							TEXT("The planned World change has no after-state to verify."), TEXT("change/after"));
						OutError.TargetPath = ChangeTarget->GetPublicRef();
						OutError.ActualValuePresent = false;
						OutError.ExpectedValuePresent = true;
						OutError.SubjectId = Change.GetIdentity().GetCanonicalKey();
						return false;
					}
					TArray<FUeAgentStateEntry> Entries;
					Entries.Emplace(
						Change.GetIdentity(),
						WorldSimpleStateType(),
						Change.GetAfter().ToSharedRef(),
						WorldSimpleStateDefault(
							DefaultValue.ToSharedRef()),
						WorldSimpleStateDetails(*Profile),
						Revision,
						EUeAgentAuthoredPresence::Present,
						EUeAgentOverrideState::Overridden);
					FUeAgentCanonicalSnapshotPtr Expected;
					if (!FUeAgentCanonicalSnapshot::
							TryCreateExpected(
								TEXT(
									"snapshot:world-simple:"
									"expected:")
									+ Plan->GetRequestId()
									+ TEXT(":")
									+ Change.GetIdentity()
										.GetCanonicalKey(),
								EUeAgentStatePlane::Persisted,
								Revision,
								FUeAgentTemporalPoint(
									TEXT(
										"world-simple-operation"),
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
						OutError = StateError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Change.GetIdentity()
								.GetCanonicalKey();
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
						OutError = *OutTargets.Last().GetValidationDiagnostic();
						return false;
					}
				}
				if (OutTargets.IsEmpty())
				{
					OutError = CountFailure(TEXT("world_simple_verification_target_missing"),
						TEXT("At least one World verification target is required."), TEXT("verification_targets"), 0, 1);
					return false;
				}
				return true;
			}
		};

		bool ValidateConfig(
			const FUeAgentWorldSimpleAuthoringProviderConfig& Config,
			FUeAgentWorldSimpleAuthoringError& OutError)
		{
			const TCHAR* Missing = !Config.DescriptorRegistry.IsValid() ? TEXT("descriptor_registry")
				: !Config.WorldCatalog.IsValid() ? TEXT("world_catalog")
				: !Config.ReferenceResolver.IsValid() ? TEXT("reference_resolver")
				: !Config.DurableService.IsValid() ? TEXT("durable_service")
				: !Config.AbsenceOracle.IsValid() ? TEXT("absence_oracle")
				: !Config.ReferenceOracle.IsValid() ? TEXT("reference_oracle")
				: !Config.Projection.IsSet() ? TEXT("projection") : nullptr;
			if (Missing)
			{
				OutError = WorldFailure(TEXT("world_simple_authoring_dependencies_missing"),
					FString::Printf(TEXT("The World authoring provider has no '%s'."), Missing), Missing);
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			if (!Config.Projection->IsValid())
			{
				OutError = Config.Projection->GetValidationDiagnostic();
				return false;
			}
			if (Config.OwnerLeaseId.IsEmpty())
			{
				OutError = WorldFailure(TEXT("world_simple_authoring_dependencies_missing"),
					TEXT("The World authoring provider has an empty owner lease identifier."), TEXT("owner_lease_id"));
				OutError.ActualValue = MakeShared<FJsonValueString>(TEXT(""));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty identifier"));
				return false;
			}
			for (const FWorldProfileSpec& Profile : WorldProfiles)
			{
				const FUeAgentOperationDescriptor* Descriptor =
					Config.DescriptorRegistry
						->FindByDescriptorId(
							Profile.DescriptorId);
				if (!Descriptor)
				{
					OutError = WorldFailure(TEXT("world_simple_authoring_descriptor_missing"),
						TEXT("The required World operation descriptor is not registered."), TEXT("descriptor_id"));
					OutError.CandidateName = Profile.DescriptorId;
					OutError.ActualValuePresent = false;
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Profile.DescriptorId);
					return false;
				}
				if (Descriptor->GetCapabilityId() != Profile.CapabilityId || Descriptor->GetDomainId() != TEXT("world"))
				{
					const bool bCapabilityMismatch = Descriptor->GetCapabilityId() != Profile.CapabilityId;
					OutError = WorldFailure(TEXT("world_simple_authoring_descriptor_missing"),
						TEXT("The registered World descriptor does not match its profile."),
						bCapabilityMismatch ? TEXT("capability_id") : TEXT("domain_id"));
					OutError.CandidateName = Profile.DescriptorId;
					OutError.ActualValue = MakeShared<FJsonValueString>(bCapabilityMismatch ? Descriptor->GetCapabilityId() : Descriptor->GetDomainId());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(bCapabilityMismatch ? Profile.CapabilityId : TEXT("world"));
					return false;
				}
			}
			return true;
		}
	}

	bool FUeAgentWorldSimpleAuthoringProviderFactory::TryCreate(
		const FUeAgentWorldSimpleAuthoringProviderConfig& Config,
		TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider>&
			OutProvider,
		FUeAgentWorldSimpleAuthoringError& OutError)
	{
		OutProvider.Reset();
		OutError = FUeAgentWorldSimpleAuthoringError();
		if (!ValidateConfig(Config, OutError))
		{
			return false;
		}

		const TSharedRef<
			IUeAgentWorldSimpleEffectWorldCatalog,
			ESPMode::ThreadSafe> Catalog =
				Config.WorldCatalog.ToSharedRef();
		const TSharedRef<
			IUeAgentWorldSimpleEffectWorldCatalog,
			ESPMode::ThreadSafe> PersistedCatalog =
				Config.PersistedWorldCatalog.IsValid()
					? Config.PersistedWorldCatalog.ToSharedRef()
					: Catalog;
		const TSharedRef<
			FUeAgentWorldSimpleEffectExactWorldResolver,
			ESPMode::ThreadSafe> WorldResolver =
				MakeShared<
					FUeAgentWorldSimpleEffectExactWorldResolver,
					ESPMode::ThreadSafe>(Catalog);
		TSharedPtr<IUeAgentWorldSimpleEffectNativeHook> NativeHook;
		FExecutionMutationError NativeError;
		if (!FUeAgentWorldSimpleEffectNativeHookFactory::TryCreate(
				Config.ReferenceResolver.ToSharedRef(),
				NativeHook,
				NativeError))
		{
			OutError = NativeError;
			return false;
		}

		FUeAgentSimpleAuthoringProductionProviderConfig
			ProviderConfig;
		ProviderConfig.FamilyId = TEXT("world");
		ProviderConfig.OwnerLeaseId = Config.OwnerLeaseId;
		ProviderConfig.PreStatePlane =
			EUeAgentStatePlane::AuthoringMemory;
		ProviderConfig.AtomicityMode =
			EAtomicityMode::CompensatedSaga;
		ProviderConfig.Projection = Config.Projection;
		ProviderConfig.TargetResolver =
			MakeShared<FWorldTargetResolver>(
				Catalog,
				WorldResolver);
		ProviderConfig.PreStateReadPort =
			MakeShared<FWorldStateReadPort>(
				PreStatePortId,
				EUeAgentStatePlane::AuthoringMemory,
				Catalog,
				Config.ReferenceResolver.ToSharedRef(),
				true);
		ProviderConfig.ChangeTargetProjector =
			MakeShared<FWorldChangeTargetProjector>();
		ProviderConfig.DraftBuilder =
			MakeShared<FWorldDraftBuilder>();
		ProviderConfig.MutationBindingProjector =
			MakeShared<FWorldMutationBindingProjector>();
		ProviderConfig.DurableTargetProjector =
			MakeShared<FWorldDurableTargetProjector>();
		ProviderConfig.DurableService =
			Config.DurableService;
		ProviderConfig.UeHook =
			FUeAgentWorldSimpleEffectProductionHost::Create(
				Catalog,
				NativeHook.ToSharedRef());
		ProviderConfig.ExpectedStateProjector =
			MakeShared<FWorldExpectedStateProjector>();
		if (Config.PersistedWorldCatalog.IsValid())
		{
			const TSharedRef<IUeAgentStateReadPort,
				ESPMode::ThreadSafe> PersistedStateReadPort =
					MakeShared<FWorldStateReadPort>(
						PersistedStatePortId,
						EUeAgentStatePlane::Persisted,
						PersistedCatalog,
						Config.ReferenceResolver.ToSharedRef(),
						false);
			ProviderConfig.PersistedStateOracle = MakeShared<
				FStateReadPortPersistedStateOracle,
				ESPMode::ThreadSafe>(PersistedStateReadPort);
		}
		else
		{
			ProviderConfig.PersistedStateOracle = MakeShared<
				FWorldArtifactPersistedStateOracle,
				ESPMode::ThreadSafe>(
				Config.ReferenceResolver.ToSharedRef());
		}
		ProviderConfig.AbsenceOracle = Config.AbsenceOracle;
		ProviderConfig.ReferenceOracle = Config.ReferenceOracle;

		return FUeAgentSimpleAuthoringProductionProvider::TryCreate(
			ProviderConfig, OutProvider, OutError);
	}
}
