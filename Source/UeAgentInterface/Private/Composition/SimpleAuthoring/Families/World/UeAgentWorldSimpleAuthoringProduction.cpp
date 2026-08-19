#include "Composition/SimpleAuthoring/Families/World/UeAgentWorldSimpleAuthoringProduction.h"

#include "Components/StaticMeshComponent.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
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

		void SetError(
			FUeAgentWorldSimpleAuthoringError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		void SetError(
			FUeAgentSimpleAuthoringProductionError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		bool IsSupported(
			const FUeAgentDescriptorIdentity& Identity)
		{
			return FindProfile(Identity) != nullptr;
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
				OutError.Code =
					TEXT(
						"world_simple_state_scope_missing");
				OutError.SubjectId = Binding.GetBindingId();
				return false;
			}
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(
					Evidence.RightChop(Prefix.Len()));
			if (!FJsonSerializer::Deserialize(
					Reader,
					OutPayload)
				|| !OutPayload.IsValid())
			{
				OutError.Code =
					TEXT(
						"world_simple_state_scope_invalid");
				OutError.SubjectId = Binding.GetBindingId();
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
			FUeAgentStateSemanticError& OutError)
		{
			OutValue.Reset();
			if (!JsonValue.IsValid())
			{
				OutError.Code =
					TEXT("world_simple_state_value_missing");
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
						if (!TryCanonicalJsonValue(
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
						if (!TryCanonicalJsonValue(
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
					TEXT("world_simple_state_value_kind_invalid");
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
				OutError.Code = ResolveError.GetCode().IsEmpty()
					? TEXT(
						"world_simple_state_reference_unresolved")
					: ResolveError.GetCode();
				OutError.SubjectId = Selector;
				OutError.Detail = ResolveError.GetDetail();
				return false;
			}
			OutObject = Cast<TObject>(ResolvedObject);
			if (!OutObject)
			{
				OutError.Code =
					TEXT("world_simple_state_reference_type_invalid");
				OutError.SubjectId = Selector;
				OutError.Detail = ResolvedObject
					? ResolvedObject->GetClass()->GetPathName()
					: FString();
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
			const TSharedPtr<FJsonObject>& Payload,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			const AWorldSettings* WorldSettings = World.GetWorldSettings();
			if (!WorldSettings)
			{
				OutError.Code = TEXT("world_simple_world_settings_missing");
				return false;
			}
			bool bSaveLevel = false;
			Payload->TryGetBoolField(TEXT("save_level"), bSaveLevel);
			const FString GameModeClassRef = WorldSettings->DefaultGameMode
				? WorldSettings->DefaultGameMode->GetPathName()
				: FString();
			FUeAgentCanonicalValuePtr Kind;
			FUeAgentCanonicalValuePtr ClassRef;
			FUeAgentCanonicalValuePtr SaveLevel;
			if (!FUeAgentCanonicalValue::TryString(
					TEXT("level_game_mode"),
					Resources,
					Kind,
					OutError)
				|| !FUeAgentCanonicalValue::TryString(
					GameModeClassRef,
					Resources,
					ClassRef,
					OutError)
				|| !FUeAgentCanonicalValue::TryBoolean(
					bSaveLevel,
					Resources,
					SaveLevel,
					OutError))
			{
				return false;
			}
			return FUeAgentCanonicalValue::TryObject(
				{
					{TEXT("game_mode_class_ref"), ClassRef.ToSharedRef()},
					{TEXT("save_level"), SaveLevel.ToSharedRef()},
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
			const TCHAR* Field,
			const int32 ExpectedCount,
			TArray<double>& OutValues)
		{
			OutValues.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object.IsValid()
				|| !Object->TryGetArrayField(Field, Values)
				|| !Values
				|| Values->Num() != ExpectedCount)
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				double Number = 0.0;
				if (!Value.IsValid()
					|| !Value->TryGetNumber(Number)
					|| !FMath::IsFinite(Number))
				{
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
			FTransform& OutTransform)
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
				return Payload.IsValid();
			}
			TArray<double> Translation;
			TArray<double> Rotation;
			TArray<double> Scale;
			if (!TryReadNumberArray(
					*Placement,
					TEXT("translation"),
					3,
					Translation)
				|| !TryReadNumberArray(
					*Placement,
					TEXT("rotation_quaternion_xyzw"),
					4,
					Rotation)
				|| !TryReadNumberArray(
					*Placement,
					TEXT("scale"),
					3,
					Scale)
				|| !(*Placement)->TryGetStringField(
					TEXT("unit"),
					OutUnit))
			{
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
			for (TActorIterator<AActor> It(&World); It; ++It)
			{
				if (It->GetActorLabel() == Label)
				{
					Match = *It;
					++OutMatchCount;
				}
			}
			return Match;
		}

		UDataLayerInstance* FindPrivateDataLayerByShortName(
			AWorldDataLayers& WorldDataLayers,
			const FString& ShortName,
			int32& OutMatchCount)
		{
			UDataLayerInstance* Match = nullptr;
			OutMatchCount = 0;
			WorldDataLayers.ForEachDataLayerInstance(
				[&Match, &OutMatchCount, &ShortName](
					UDataLayerInstance* Instance)
				{
					if (Instance
						&& Instance->GetAsset()
						&& Instance->GetAsset()->IsPrivate()
						&& Instance->GetDataLayerShortName()
							== ShortName)
					{
						Match = Instance;
						++OutMatchCount;
					}
					return true;
				});
			return Match;
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
				if (!Payload->TryGetStringField(
						TEXT("actor_class_ref"),
						ActorClassRef)
					|| !Payload->TryGetStringField(
						TEXT("label"),
						Label)
					|| Label.IsEmpty())
				{
					OutError.Code =
						TEXT("world_simple_create_payload_invalid");
					return false;
				}
				UClass* ActorClass = nullptr;
				if (!TryResolveTypedReference(
						ReferenceResolver,
						World,
						ActorClassRef,
						ActorClass,
						OutError)
					|| !ActorClass->IsChildOf(AActor::StaticClass()))
				{
					if (OutError.Code.IsEmpty())
					{
						OutError.Code = TEXT(
							"world_simple_create_reference_type_invalid");
					}
					return false;
				}
				bool bHasTransform = false;
				FString Unit;
				FTransform Transform;
				if (!TryReadCreatePlacement(
						Payload,
						bHasTransform,
						Unit,
						Transform)
					|| !bHasTransform)
				{
					OutError.Code =
						TEXT("world_simple_create_placement_invalid");
					return false;
				}
				int32 MatchCount = 0;
				FindActorByExactLabel(World, Label, MatchCount);
				if (MatchCount > 0)
				{
					OutError.Code =
						TEXT("world_simple_create_target_exists");
					OutError.SubjectId = Label;
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
					OutError.Code =
						TEXT("world_simple_create_payload_invalid");
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
				if (!TryResolveTypedReference(
						ReferenceResolver,
						World,
						ActorClassRef,
						ActorClass,
						OutError)
					|| !ActorClass->IsChildOf(
						AActor::StaticClass())
					|| !TryResolveTypedReference(
						ReferenceResolver,
						World,
						MeshRef,
						Mesh,
						OutError))
				{
					if (OutError.Code.IsEmpty())
					{
						OutError.Code =
							TEXT(
								"world_simple_create_reference_type_invalid");
					}
					return false;
				}
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
						OutError.Code =
							TEXT(
								"world_simple_create_target_exists");
						OutError.SubjectId = Label;
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
					OutError.Code =
						TEXT(
							"world_simple_world_data_layers_missing");
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
					int32 MatchCount = 0;
					FindPrivateDataLayerByShortName(
						*WorldDataLayers,
						ShortName,
						MatchCount);
					if (MatchCount > 0)
					{
						OutError.Code =
							TEXT(
								"world_simple_create_target_exists");
						OutError.SubjectId = ShortName;
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
					OutError.Code =
						TEXT("world_simple_create_target_exists");
					OutError.SubjectId = AssetRef;
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
					OutError)
				|| !StreamingClass->IsChildOf(
					ULevelStreaming::StaticClass()))
			{
				if (OutError.Code.IsEmpty())
				{
					OutError.Code =
						TEXT(
							"world_simple_create_reference_type_invalid");
				}
				return false;
			}
			bool bHasTransform = false;
			FString Unit;
			FTransform Transform;
			if (!TryReadCreatePlacement(
					Payload,
					bHasTransform,
					Unit,
					Transform))
			{
				OutError.Code =
					TEXT("world_simple_create_placement_invalid");
				return false;
			}
			int32 MatchCount = 0;
			FindExactStreamingRelation(
				World,
				LevelAsset->GetOutermost()->GetName(),
				*StreamingClass,
				Transform,
				MatchCount);
			if (MatchCount > 0)
			{
				OutError.Code =
					TEXT("world_simple_create_target_exists");
				OutError.SubjectId =
					LevelAsset->GetOutermost()->GetName();
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
				OutError.Code =
					TEXT("world_simple_create_identity_invalid");
				OutError.SubjectId = Selector;
				return false;
			}
			int32 MatchCount = 0;
			AActor* Actor = FindActorByExactLabel(
				World,
				Label,
				MatchCount);
			if (!Actor || MatchCount != 1)
			{
				OutError.Code =
					MatchCount > 1
						? TEXT(
							"world_simple_create_identity_ambiguous")
						: TEXT(
							"world_simple_created_target_unresolved");
				OutError.SubjectId = Label;
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
			if (!TryReadCreatePlacement(
					Payload,
					bHasTransform,
					Unit,
					ExpectedTransform)
				|| !bHasTransform)
			{
				OutError.Code =
					TEXT("world_simple_create_placement_invalid");
				return false;
			}

			const FTransform ActualTransform =
				Actor->GetActorTransform();
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
			bool bSaveLevel = false;
			Payload->TryGetBoolField(TEXT("save_level"), bSaveLevel);
			Value->SetBoolField(TEXT("save_level"), bSaveLevel);
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
				OutError.Code = StateError.Code;
				OutError.Detail = StateError.Detail;
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
				OutError.Code =
					TEXT(
						"world_simple_create_identity_invalid");
				OutError.SubjectId = Selector;
				return false;
			}
			const TSharedPtr<FJsonObject>* Segment = nullptr;
			if (!Payload->TryGetObjectField(
					TEXT("segment_template"),
					Segment)
				|| !Segment)
			{
				OutError.Code =
					TEXT("world_simple_create_payload_invalid");
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
				OutError.Code =
					MatchCount > 1
						? TEXT(
							"world_simple_create_identity_ambiguous")
						: TEXT(
							"world_simple_created_target_unresolved");
				OutError.SubjectId = Label;
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
				Actor->GetActorTransform();
			const FVector Location = Transform.GetLocation();
			const FVector Normal =
				Actor->GetActorForwardVector();
			const FVector Up = Actor->GetActorUpVector();
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
				OutError.Code = StateError.Code;
				OutError.Detail = StateError.Detail;
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
				OutError.Code =
					TEXT(
						"world_simple_world_data_layers_missing");
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
				int32 MatchCount = 0;
				Instance = FindPrivateDataLayerByShortName(
					*WorldDataLayers,
					ShortName,
					MatchCount);
				if (MatchCount != 1)
				{
					OutError.Code =
						MatchCount > 1
							? TEXT(
								"world_simple_create_identity_ambiguous")
							: TEXT(
								"world_simple_created_target_unresolved");
					OutError.SubjectId = ShortName;
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
					OutError.Code =
						TEXT(
							"world_simple_created_target_unresolved");
					OutError.SubjectId = AssetRef;
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
				OutError.Code = StateError.Code;
				OutError.Detail = StateError.Detail;
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
			if (!TryReadCreatePlacement(
					Payload,
					bHasTransform,
					Unit,
					ExpectedTransform))
			{
				OutError.Code =
					TEXT("world_simple_create_placement_invalid");
				return false;
			}
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
				OutError.Code =
					MatchCount > 1
						? TEXT(
							"world_simple_create_identity_ambiguous")
						: TEXT(
							"world_simple_created_target_unresolved");
				OutError.SubjectId = LevelRef;
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
				OutError.Code = StateError.Code;
				OutError.Detail = StateError.Detail;
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
				OutError.Code = Error.GetCode().IsEmpty()
					? TEXT(
						"world_simple_state_catalog_unavailable")
					: Error.GetCode();
				OutError.Detail = Error.GetDetail();
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
					OutError.Code =
						TEXT("world_simple_state_world_ambiguous");
					OutError.SubjectId =
						Binding.GetBindingId();
					OutWorld = nullptr;
					return false;
				}
				OutWorld = Candidate.GetWorld();
			}
			if (!OutWorld)
			{
				OutError.Code =
					TEXT("world_simple_state_world_unresolved");
				OutError.SubjectId = Binding.GetBindingId();
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
				if (!IsSupported(
						CanonicalRequest
							.GetDescriptorIdentity()))
				{
					SetError(
						OutError,
						TEXT(
							"world_simple_authoring_profile_missing"),
						CanonicalRequest
							.GetDescriptorIdentity()
							.GetDescriptorId());
					return false;
				}
				UWorld* ResolvedWorld = nullptr;
				FExecutionMutationError Error;
				if (!WorldResolver->TryResolveWorld(
						Context,
						ResolvedWorld,
						Error)
					|| !ResolvedWorld
					|| !ResolvedWorld->PersistentLevel)
				{
					SetError(
						OutError,
						Error.GetCode().IsEmpty()
							? TEXT(
								"world_simple_target_unresolved")
							: Error.GetCode(),
						Context.GetWorldIdentity(),
						Error.GetDetail());
					return false;
				}
				const FUeAgentCanonicalIdentity Identity =
					TargetIdentity(Context, CanonicalRequest);
				const FWorldProfileSpec* Profile =
					FindProfile(
						CanonicalRequest
							.GetDescriptorIdentity());
				if (!Profile)
				{
					SetError(
						OutError,
						TEXT(
							"world_simple_authoring_profile_missing"),
						CanonicalRequest
							.GetDescriptorIdentity()
							.GetDescriptorId());
					return false;
				}
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
					SetError(
						OutError,
						TEXT(
							"world_simple_target_binding_invalid"),
						CanonicalRequest.GetRequestId());
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
				if (!Request.IsValid()
					|| Request.GetStatePlane() != RequiredPlane
					|| !IsSupported(
						Request.GetBinding()
							.GetSummary()
							.GetDescriptorIdentity()))
				{
					OutError.Code =
						TEXT(
							"world_simple_state_request_invalid");
					OutError.SubjectId =
						Request.GetBinding().GetBindingId();
					return false;
				}
				if (!Deadline.CanContinue()
					|| Cancellation.IsCancelled())
				{
					OutError.Code =
						TEXT(
							"world_simple_state_request_cancelled");
					return false;
				}
				const FUeAgentCandidateBinding& Binding =
					Request.GetBinding();
				const FWorldProfileSpec* Profile =
					FindProfile(
						Binding.GetSummary()
							.GetDescriptorIdentity());
				if (!Profile)
				{
					OutError.Code =
						TEXT(
							"world_simple_state_profile_missing");
					OutError.SubjectId =
						Binding.GetBindingId();
					return false;
				}
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
					OutError.Code = StateError.Code;
					OutError.Detail = StateError.Detail;
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
							OutError.Code =
								TEXT(
									"world_simple_create_reader_missing");
							break;
						}
						if (!bRead || !CurrentValue.IsValid())
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
							Payload,
							Resources,
							CurrentValue,
							StateError))
					{
						OutError.Code = StateError.Code;
						OutError.Detail = StateError.Detail;
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
						OutError.Code =
							TEXT(
								"world_simple_"
								"world_partition_missing");
						OutError.SubjectId =
							ResolvedWorld->GetPathName();
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
						OutError.Code = StateError.Code;
						OutError.Detail = StateError.Detail;
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
						OutError.Code =
							TEXT(
								"world_simple_state_scope_empty");
						OutError.SubjectId =
							Binding.GetBindingId();
						return false;
					}
					for (const FString& Selector : Selectors)
					{
						UObject* ResolvedObject = nullptr;
						FExecutionMutationError ResolveError;
						if (!ReferenceResolver
								->TryResolveReference(
									*ResolvedWorld,
									Selector,
									ResolvedObject,
									ResolveError)
							|| !Cast<AActor>(ResolvedObject))
						{
							OutError.Code =
								ResolveError.GetCode().IsEmpty()
									? TEXT(
										"world_simple_state_"
										"reference_unresolved")
									: ResolveError.GetCode();
							OutError.SubjectId = Selector;
							OutError.Detail =
								ResolveError.GetDetail();
							return false;
						}
						FUeAgentCanonicalValuePtr CurrentValue;
						if (!TryMakeActorStreamingPolicyValue(
								*CastChecked<AActor>(
									ResolvedObject),
								Selector,
								Payload,
								bAggregateTarget,
								Resources,
								CurrentValue,
								StateError))
						{
							OutError.Code = StateError.Code;
							OutError.Detail = StateError.Detail;
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
						OutError.Code =
							TEXT(
								"world_simple_state_scope_empty");
						OutError.SubjectId =
							Binding.GetBindingId();
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
							OutError.Code = StateError.Code;
							OutError.Detail = StateError.Detail;
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
						OutError.Code =
							TEXT(
								"world_simple_state_scope_empty");
						OutError.SubjectId =
							Binding.GetBindingId();
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
							OutError.Code = StateError.Code;
							OutError.Detail = StateError.Detail;
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
						OutError.Code =
							TEXT(
								"world_simple_state_scope_empty");
						OutError.SubjectId =
							Binding.GetBindingId();
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
								TSet<
									const UDataLayerInstance*>
										Expected;
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
									Expected.Add(Member);
								}
								TSet<
									const UDataLayerInstance*>
										Actual;
								for (const UDataLayerInstance*
									Instance
									: Actor
										->GetDataLayerInstances())
								{
									Actual.Add(Instance);
								}
								const auto ContainsAll =
									[&Actual](
										const TSet<
											const UDataLayerInstance*>&
											Required)
									{
										for (const auto* Member
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
									for (const auto* Member
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
							OutError.Code = StateError.Code;
							OutError.Detail = StateError.Detail;
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
					OutError.Code =
						TEXT(
							"world_simple_profile_state_reader_missing");
					OutError.SubjectId =
						Profile->DescriptorId;
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
					OutError.Code = StateError.Code;
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
				IUeAgentWorldSimpleEffectWorldCatalog,
				ESPMode::ThreadSafe> Catalog;
			TSharedRef<
				IUeAgentWorldSimpleEffectReferenceResolver,
				ESPMode::ThreadSafe> ReferenceResolver;
			bool bRequireSourceInstance = true;
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
					SetError(
						OutError,
						Error.GetCode(),
						CanonicalRequest.GetRequestId(),
						Error.GetDetail());
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
						SetError(
							OutError,
							TEXT(
								"world_simple_change_target_"
								"invalid"),
							Change.GetIdentity()
								.GetCanonicalKey());
						OutTargets.Reset();
						return false;
					}
				}
				return !OutTargets.IsEmpty();
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
					SetError(
						OutError,
						TEXT(
							"world_simple_change_targets_missing"),
						Plan->GetRequestId());
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
					if (Targets[Index].GetSummary()
							.GetIdentity()
							.GetCanonicalKey()
							!= Change.GetIdentity()
								.GetCanonicalKey()
						|| !OutBindings.Last().IsValid())
					{
						SetError(
							OutError,
							TEXT(
								"world_simple_mutation_"
								"binding_invalid"),
							Change.GetIdentity()
								.GetCanonicalKey());
						OutBindings.Reset();
						return false;
					}
				}
				return !OutBindings.IsEmpty();
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
				if (Changes.IsEmpty()
					|| Target.GetPublicRef().IsEmpty())
				{
					SetError(
						OutError,
						TEXT(
							"world_simple_durable_target_missing"),
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
					SetError(
						OutError,
						StateError.Code,
						Plan->GetRequestId(),
						StateError.Detail);
					return false;
				}
				const FWorldProfileSpec* Profile =
					FindProfile(
						Plan->GetDescriptorIdentity());
				if (!Profile)
				{
					SetError(
						OutError,
						TEXT(
							"world_simple_expected_profile_missing"),
						Plan->GetDescriptorIdentity()
							.GetDescriptorId());
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
						SetError(
							OutError,
							TEXT(
								"world_simple_verification_"
								"target_missing"),
							Change.GetIdentity().GetCanonicalKey());
						return false;
					}
					if (!Change.GetAfter().IsValid())
					{
						SetError(
							OutError,
							TEXT(
								"world_simple_expected_"
								"state_missing"),
							Change.GetIdentity()
								.GetCanonicalKey());
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
						SetError(
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
						SetError(
							OutError,
							TEXT(
								"world_simple_verification_"
								"target_invalid"),
							Change.GetIdentity().GetCanonicalKey());
						return false;
					}
				}
				return !OutTargets.IsEmpty();
			}
		};

		bool ValidateConfig(
			const FUeAgentWorldSimpleAuthoringProviderConfig& Config,
			FUeAgentWorldSimpleAuthoringError& OutError)
		{
			if (!Config.DescriptorRegistry.IsValid()
				|| !Config.WorldCatalog.IsValid()
				|| !Config.ReferenceResolver.IsValid()
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
						"world_simple_authoring_dependencies_missing"));
				return false;
			}
			for (const FWorldProfileSpec& Profile : WorldProfiles)
			{
				const FUeAgentOperationDescriptor* Descriptor =
					Config.DescriptorRegistry
						->FindByDescriptorId(
							Profile.DescriptorId);
				if (!Descriptor
					|| Descriptor->GetCapabilityId()
						!= Profile.CapabilityId
					|| Descriptor->GetDomainId()
						!= TEXT("world"))
				{
					SetError(
						OutError,
						TEXT(
							"world_simple_authoring_"
							"descriptor_missing"),
						Profile.DescriptorId);
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
				NativeError)
			|| !NativeHook.IsValid())
		{
			SetError(
				OutError,
				NativeError.GetCode().IsEmpty()
					? TEXT(
						"world_simple_native_hook_missing")
					: NativeError.GetCode(),
				TEXT("world"),
				NativeError.GetDetail());
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
		ProviderConfig.MaxQueueItems = Config.MaxQueueItems;
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
		ProviderConfig.PersistedStateReadPort =
			MakeShared<FWorldStateReadPort>(
				PersistedStatePortId,
				EUeAgentStatePlane::Persisted,
				PersistedCatalog,
				Config.ReferenceResolver.ToSharedRef(),
				false);
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
