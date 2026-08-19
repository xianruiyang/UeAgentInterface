#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectCapabilities.h"

#include "DataLayer/DataLayerEditorSubsystem.h"
#include "Editor.h"
#include "EditorLevelUtils.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/ScopeLock.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerInstanceWithAsset.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/DataLayer/DataLayerUtils.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/WorldPartition.h"

namespace UE::UAI::DomainAdapters::WorldSimpleEffect
{
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace PlanningPreflight;
	using namespace StateSemantic;
	using namespace GenericEffect;

	namespace
	{
		enum class ERollbackKind : uint8
		{
			DestroyActor,
			RemoveDataLayerInstance,
			RestoreActorDataLayers,
			RestoreDataLayerParent,
			RestoreActorHlod,
			RestoreDataLayerState,
			RestoreActorStreamingPolicy,
			RemoveStreamingRelation,
			RestoreStreamingState,
			RestoreWorldPartitionStreaming,
			RestoreLevelGameMode,
		};

		struct FRollbackAction
		{
			ERollbackKind Kind = ERollbackKind::DestroyActor;
			TWeakObjectPtr<UObject> Primary;
			TWeakObjectPtr<UObject> Secondary;
			TArray<TWeakObjectPtr<UObject>> Objects;
			FTransform Transform = FTransform::Identity;
			FName Name;
			FString Text;
			int32 Integer = 0;
			bool bValueA = false;
			bool bValueB = false;
			bool bValueC = false;
		};

		using FResolvedReferenceMap =
			TMap<FString, TWeakObjectPtr<UObject>>;

		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				MoveTemp(Code),
				MoveTemp(Detail));
			return false;
		}

		FUeAgentCanonicalValuePtr FindField(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field)
		{
			if (!Object.IsValid()
				|| Object->GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<
				FString,
				FUeAgentCanonicalValueRef>& Item
				: Object->GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return FUeAgentCanonicalValuePtr(Item.Value);
				}
			}
			return nullptr;
		}

		bool ReadString(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			FString& OutValue,
			const bool bRequired,
			FExecutionMutationError& OutError)
		{
			OutValue.Reset();
			const FUeAgentCanonicalValuePtr Value =
				FindField(Object, Field);
			if (!Value.IsValid())
			{
				return !bRequired
					|| Fail(
						OutError,
						TEXT(
							"world_simple_internal_field_missing"),
						Field);
			}
			if (Value->GetKind() != EUeAgentValueKind::String)
			{
				return Fail(
					OutError,
					TEXT("world_simple_internal_field_type"),
					Field);
			}
			OutValue = Value->GetText();
			return !OutValue.IsEmpty()
				|| !bRequired
				|| Fail(
					OutError,
					TEXT("world_simple_internal_field_empty"),
					Field);
		}

		bool ReadBool(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			bool& OutValue,
			bool& bOutPresent,
			FExecutionMutationError& OutError)
		{
			bOutPresent = false;
			const FUeAgentCanonicalValuePtr Value =
				FindField(Object, Field);
			if (!Value.IsValid())
			{
				return true;
			}
			if (Value->GetKind() != EUeAgentValueKind::Boolean)
			{
				return Fail(
					OutError,
					TEXT("world_simple_internal_field_type"),
					Field);
			}
			OutValue = Value->GetBoolean();
			bOutPresent = true;
			return true;
		}

		bool ReadNumberArray(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			const int32 ExpectedCount,
			TArray<double>& OutValues,
			const bool bRequired,
			FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			const FUeAgentCanonicalValuePtr Value =
				FindField(Object, Field);
			if (!Value.IsValid())
			{
				return !bRequired
					|| Fail(
						OutError,
						TEXT(
							"world_simple_internal_field_missing"),
						Field);
			}
			if (Value->GetKind() != EUeAgentValueKind::Array
				|| Value->GetArrayItems().Num() != ExpectedCount)
			{
				return Fail(
					OutError,
					TEXT("world_simple_internal_field_type"),
					Field);
			}
			for (const FUeAgentCanonicalValueRef& Item
				: Value->GetArrayItems())
			{
				if (Item->GetKind() != EUeAgentValueKind::Decimal
					&& Item->GetKind()
						!= EUeAgentValueKind::Integer)
				{
					return Fail(
						OutError,
						TEXT(
							"world_simple_internal_number_invalid"),
						Field);
				}
				const double Number =
					FCString::Atod(*Item->GetLexeme());
				if (!FMath::IsFinite(Number))
				{
					return Fail(
						OutError,
						TEXT(
							"world_simple_internal_number_invalid"),
						Field);
				}
				OutValues.Add(Number);
			}
			return true;
		}

		TArray<FString> ReadStringArray(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			FExecutionMutationError& OutError)
		{
			TArray<FString> Result;
			const FUeAgentCanonicalValuePtr Value =
				FindField(Object, Field);
			if (!Value.IsValid())
			{
				return Result;
			}
			if (Value->GetKind() != EUeAgentValueKind::Array)
			{
				Fail(
					OutError,
					TEXT("world_simple_internal_field_type"),
					Field);
				return {};
			}
			for (const FUeAgentCanonicalValueRef& Item
				: Value->GetArrayItems())
			{
				if (Item->GetKind() != EUeAgentValueKind::String
					|| Item->GetText().IsEmpty())
				{
					Fail(
						OutError,
						TEXT("world_simple_internal_ref_invalid"),
						Field);
					return {};
				}
				Result.Add(Item->GetText());
			}
			return Result;
		}

		UObject* FindResolved(
			const FResolvedReferenceMap& Resolved,
			const FString& CanonicalRef)
		{
			return Resolved.FindRef(CanonicalRef).Get();
		}

		bool AddReference(
			TSet<FString>& OutReferences,
			const FString& CanonicalRef,
			const FString& Field,
			FExecutionMutationError& OutError)
		{
			if (CanonicalRef.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("world_simple_internal_ref_missing"),
					Field);
			}
			OutReferences.Add(CanonicalRef);
			return true;
		}

		bool CollectReferences(
			const FPlannedChange& Change,
			TSet<FString>& OutReferences,
			FExecutionMutationError& OutError)
		{
			const FUeAgentCanonicalValuePtr& After =
				Change.GetAfter();
			const FString& TargetKind =
				Change.GetIdentity().GetTargetKind();
			FString InternalKind;
			if (TargetKind == TEXT("world.entity.create"))
			{
				if (!ReadString(
						After,
						TEXT("entity_kind"),
						InternalKind,
						true,
						OutError))
				{
					return false;
				}
				if (InternalKind == TEXT("wall_segment"))
				{
					for (const TCHAR* Field : {
						TEXT("actor_class_ref"),
						TEXT("mesh_ref")})
					{
						FString Ref;
						if (!ReadString(
								After,
								Field,
								Ref,
								true,
								OutError)
							|| !AddReference(
								OutReferences,
								Ref,
								Field,
								OutError))
						{
							return false;
						}
					}
					return true;
				}
				if (InternalKind == TEXT("actor"))
				{
					FString ClassRef;
					return ReadString(
							After,
							TEXT("actor_class_ref"),
							ClassRef,
							true,
							OutError)
						&& AddReference(
							OutReferences,
							ClassRef,
							TEXT("actor_class_ref"),
							OutError);
				}
				if (InternalKind == TEXT("data_layer_instance"))
				{
					bool bPrivate = false;
					bool bPrivatePresent = false;
					if (!ReadBool(
							After,
							TEXT("is_private"),
							bPrivate,
							bPrivatePresent,
							OutError)
						|| !bPrivatePresent)
					{
						return false;
					}
					if (!bPrivate)
					{
						FString AssetRef;
						if (!ReadString(
								After,
								TEXT("asset_ref"),
								AssetRef,
								true,
								OutError)
							|| !AddReference(
								OutReferences,
								AssetRef,
								TEXT("asset_ref"),
								OutError))
						{
							return false;
						}
					}
					FString ParentRef;
					if (!ReadString(
							After,
							TEXT("parent_ref"),
							ParentRef,
							false,
							OutError))
					{
						return false;
					}
					return ParentRef.IsEmpty()
						|| AddReference(
							OutReferences,
							ParentRef,
							TEXT("parent_ref"),
							OutError);
				}
				if (InternalKind == TEXT("streaming_relation"))
				{
					for (const TCHAR* Field : {
						TEXT("level_asset_ref"),
						TEXT("streaming_class_ref")})
					{
						FString Ref;
						if (!ReadString(
								After,
								Field,
								Ref,
								true,
								OutError)
							|| !AddReference(
								OutReferences,
								Ref,
								Field,
								OutError))
						{
							return false;
						}
					}
					return true;
				}
				return Fail(
					OutError,
					TEXT("world_simple_entity_kind_unknown"),
					InternalKind);
			}
			if (TargetKind == TEXT("world.membership.update"))
			{
				if (!ReadString(
						After,
						TEXT("membership_kind"),
						InternalKind,
						true,
						OutError))
				{
					return false;
				}
				FString SubjectRef;
				if (!ReadString(
						After,
						TEXT("subject_ref"),
						SubjectRef,
						true,
						OutError)
					|| !AddReference(
						OutReferences,
						SubjectRef,
						TEXT("subject_ref"),
						OutError))
				{
					return false;
				}
				if (InternalKind == TEXT("data_layer_relation"))
				{
					const TArray<FString> Refs =
						ReadStringArray(
							After,
							TEXT("member_refs"),
							OutError);
					if (OutError.IsPresent())
					{
						return false;
					}
					for (const FString& Ref : Refs)
					{
						if (!AddReference(
								OutReferences,
								Ref,
								TEXT("member_refs"),
								OutError))
						{
							return false;
						}
					}
					return true;
				}
				if (InternalKind == TEXT("actor_hlod_layer"))
				{
					FString Ref;
					if (!ReadString(
							After,
							TEXT("member_ref"),
							Ref,
							false,
							OutError))
					{
						return false;
					}
					return Ref.IsEmpty()
						|| AddReference(
							OutReferences,
							Ref,
							TEXT("member_ref"),
							OutError);
				}
				return Fail(
					OutError,
					TEXT("world_simple_membership_kind_unknown"),
					InternalKind);
			}
			if (TargetKind == TEXT("world.state.update"))
			{
				if (!ReadString(
						After,
						TEXT("state_kind"),
						InternalKind,
						true,
						OutError))
				{
					return false;
				}
				if (InternalKind
					== TEXT("world_partition_streaming"))
				{
					return true;
				}
				if (InternalKind == TEXT("level_game_mode"))
				{
					FString GameModeClassRef;
					return ReadString(
							After,
							TEXT("game_mode_class_ref"),
							GameModeClassRef,
							true,
							OutError)
						&& AddReference(
							OutReferences,
							GameModeClassRef,
							TEXT("game_mode_class_ref"),
							OutError);
				}
				if (InternalKind != TEXT("data_layer")
					&& InternalKind
						!= TEXT("actor_streaming_policy")
					&& InternalKind
						!= TEXT("streaming_relation"))
				{
					return Fail(
						OutError,
						TEXT("world_simple_state_kind_unknown"),
						InternalKind);
				}
				FString TargetRef;
				return ReadString(
						After,
						TEXT("target_ref"),
						TargetRef,
						true,
						OutError)
					&& AddReference(
						OutReferences,
						TargetRef,
						TEXT("target_ref"),
						OutError);
			}
			return Fail(
				OutError,
				TEXT("world_simple_native_target_kind_unknown"),
				TargetKind);
		}

		bool ResolveAllReferences(
			UWorld& World,
			IUeAgentWorldSimpleEffectReferenceResolver& Resolver,
			const TSet<FString>& References,
			FResolvedReferenceMap& OutResolved,
			FExecutionMutationError& OutError)
		{
			OutResolved.Reset();
			for (const FString& CanonicalRef : References)
			{
				UObject* Object = nullptr;
				if (!Resolver.TryResolveReference(
						World,
						CanonicalRef,
						Object,
						OutError)
					|| !IsValid(Object))
				{
					if (!OutError.IsPresent())
					{
						Fail(
							OutError,
							TEXT(
								"world_simple_reference_unresolved"),
							CanonicalRef);
					}
					return false;
				}
				OutResolved.Add(CanonicalRef, Object);
			}
			return true;
		}

		bool ValidateResolvedTypes(
			UWorld& World,
			const FPlannedChange& Change,
			const FResolvedReferenceMap& Resolved,
			FExecutionMutationError& OutError)
		{
			const FUeAgentCanonicalValuePtr& After =
				Change.GetAfter();
			FString Kind;
			if (Change.GetIdentity().GetTargetKind()
				== TEXT("world.entity.create"))
			{
				if (!ReadString(
						After,
						TEXT("entity_kind"),
						Kind,
						true,
						OutError))
				{
					return false;
				}
				if (Kind == TEXT("wall_segment"))
				{
					FString ClassRef;
					FString MeshRef;
					ReadString(
						After,
						TEXT("actor_class_ref"),
						ClassRef,
						true,
						OutError);
					ReadString(
						After,
						TEXT("mesh_ref"),
						MeshRef,
						true,
						OutError);
					UClass* ActorClass =
						Cast<UClass>(
							FindResolved(Resolved, ClassRef));
					return (ActorClass
							&& ActorClass->IsChildOf(
								AActor::StaticClass())
							&& Cast<UStaticMesh>(
								FindResolved(
									Resolved,
									MeshRef)))
						|| Fail(
							OutError,
							TEXT(
								"world_simple_reference_type_invalid"),
							Kind);
				}
				if (Kind == TEXT("actor"))
				{
					FString ClassRef;
					if (!ReadString(
							After,
							TEXT("actor_class_ref"),
							ClassRef,
							true,
							OutError))
					{
						return false;
					}
					UClass* ActorClass = Cast<UClass>(
						FindResolved(Resolved, ClassRef));
					return (ActorClass
							&& ActorClass->IsChildOf(AActor::StaticClass()))
						|| Fail(
							OutError,
							TEXT("world_simple_reference_type_invalid"),
							Kind);
				}
				if (Kind == TEXT("data_layer_instance"))
				{
					if (!World.GetWorldDataLayers())
					{
						return Fail(
							OutError,
							TEXT(
								"world_simple_world_data_layers_missing"));
					}
					bool bPrivate = false;
					bool bPrivatePresent = false;
					if (!ReadBool(
							After,
							TEXT("is_private"),
							bPrivate,
							bPrivatePresent,
							OutError)
						|| !bPrivatePresent)
					{
						return false;
					}
					if (bPrivate)
					{
						FString ShortName;
						return ReadString(
								After,
								TEXT("short_name"),
								ShortName,
								true,
								OutError)
							&& !ShortName.IsEmpty();
					}
					FString AssetRef;
					ReadString(
						After,
						TEXT("asset_ref"),
						AssetRef,
						true,
						OutError);
					return Cast<UDataLayerAsset>(
							FindResolved(Resolved, AssetRef))
						|| Fail(
							OutError,
							TEXT(
								"world_simple_reference_type_invalid"),
							Kind);
				}
				FString LevelRef;
				FString ClassRef;
				ReadString(
					After,
					TEXT("level_asset_ref"),
					LevelRef,
					true,
					OutError);
				ReadString(
					After,
					TEXT("streaming_class_ref"),
					ClassRef,
					true,
					OutError);
				UClass* StreamingClass =
					Cast<UClass>(
						FindResolved(Resolved, ClassRef));
				return (Cast<UWorld>(
							FindResolved(Resolved, LevelRef))
						&& StreamingClass
						&& StreamingClass->IsChildOf(
							ULevelStreaming::StaticClass()))
					|| Fail(
						OutError,
						TEXT(
							"world_simple_reference_type_invalid"),
						Kind);
			}
			if (Change.GetIdentity().GetTargetKind()
				== TEXT("world.membership.update"))
			{
				FString SubjectRef;
				ReadString(
					After,
					TEXT("subject_ref"),
					SubjectRef,
					true,
					OutError);
				ReadString(
					After,
					TEXT("membership_kind"),
					Kind,
					true,
					OutError);
				UObject* Subject =
					FindResolved(Resolved, SubjectRef);
				if (Kind == TEXT("data_layer_relation"))
				{
					FString RelationKind;
					ReadString(
						After,
						TEXT("relation_kind"),
						RelationKind,
						true,
						OutError);
					if (RelationKind == TEXT("actor_membership"))
					{
						if (!Cast<AActor>(Subject))
						{
							return Fail(
								OutError,
								TEXT(
									"world_simple_reference_type_invalid"),
								SubjectRef);
						}
					}
					else if (
						RelationKind == TEXT("parent"))
					{
						if (!Cast<UDataLayerInstance>(Subject))
						{
							return Fail(
								OutError,
								TEXT(
									"world_simple_reference_type_invalid"),
								SubjectRef);
						}
					}
					else
					{
						return Fail(
							OutError,
							TEXT(
								"world_simple_relation_kind_unknown"),
							RelationKind);
					}
					for (const FString& Ref : ReadStringArray(
						After,
						TEXT("member_refs"),
						OutError))
					{
						if (!Cast<UDataLayerInstance>(
							FindResolved(Resolved, Ref)))
						{
							return Fail(
								OutError,
								TEXT(
									"world_simple_reference_type_invalid"),
								Ref);
						}
					}
					return !OutError.IsPresent();
				}
				if (!Cast<AActor>(Subject))
				{
					return Fail(
						OutError,
						TEXT(
							"world_simple_reference_type_invalid"),
						SubjectRef);
				}
				FString MemberRef;
				ReadString(
					After,
					TEXT("member_ref"),
					MemberRef,
					false,
					OutError);
				return MemberRef.IsEmpty()
					|| Cast<UHLODLayer>(
						FindResolved(Resolved, MemberRef))
					|| Fail(
						OutError,
						TEXT(
							"world_simple_reference_type_invalid"),
						MemberRef);
			}

			ReadString(
				After,
				TEXT("state_kind"),
				Kind,
				true,
				OutError);
			if (Kind == TEXT("world_partition_streaming"))
			{
				return World.GetWorldPartition()
					|| Fail(
						OutError,
						TEXT(
							"world_simple_world_partition_missing"));
			}
			if (Kind == TEXT("level_game_mode"))
			{
				FString GameModeClassRef;
				if (!ReadString(
						After,
						TEXT("game_mode_class_ref"),
						GameModeClassRef,
						true,
						OutError))
				{
					return false;
				}
				UClass* GameModeClass = Cast<UClass>(
					FindResolved(Resolved, GameModeClassRef));
				return (GameModeClass
						&& GameModeClass->IsChildOf(
							AGameModeBase::StaticClass()))
					|| Fail(
						OutError,
						TEXT("world_simple_reference_type_invalid"),
						GameModeClassRef);
			}
			FString TargetRef;
			ReadString(
				After,
				TEXT("target_ref"),
				TargetRef,
				true,
				OutError);
			UObject* Target =
				FindResolved(Resolved, TargetRef);
			const bool bTypeValid =
				(Kind == TEXT("data_layer")
					&& Cast<UDataLayerInstance>(Target))
				|| (Kind == TEXT("actor_streaming_policy")
					&& Cast<AActor>(Target))
				|| (Kind == TEXT("streaming_relation")
					&& Cast<ULevelStreaming>(Target));
			return bTypeValid
				|| Fail(
					OutError,
					TEXT("world_simple_reference_type_invalid"),
					TargetRef);
		}

		EDataLayerRuntimeState ParseRuntimeState(
			const FString& Value)
		{
			if (Value == TEXT("activated"))
			{
				return EDataLayerRuntimeState::Activated;
			}
			if (Value == TEXT("loaded"))
			{
				return EDataLayerRuntimeState::Loaded;
			}
			return EDataLayerRuntimeState::Unloaded;
		}

		FTransform ReadTransform(
			const FUeAgentCanonicalValuePtr& After,
			FExecutionMutationError& OutError)
		{
			TArray<double> Translation;
			TArray<double> Rotation;
			TArray<double> Scale;
			if (!ReadNumberArray(
					After,
					TEXT("translation"),
					3,
					Translation,
					true,
					OutError)
				|| !ReadNumberArray(
					After,
					TEXT("rotation_quaternion_xyzw"),
					4,
					Rotation,
					true,
					OutError)
				|| !ReadNumberArray(
					After,
					TEXT("scale"),
					3,
					Scale,
					true,
					OutError))
			{
				return FTransform::Identity;
			}
			FString Unit;
			ReadString(
				After,
				TEXT("unit"),
				Unit,
				false,
				OutError);
			const double UnitScale =
				Unit == TEXT("meter") ? 100.0 : 1.0;
			FQuat Quat(
				Rotation[0],
				Rotation[1],
				Rotation[2],
				Rotation[3]);
			Quat.Normalize();
			return FTransform(
				Quat,
				FVector(
					Translation[0] * UnitScale,
					Translation[1] * UnitScale,
					Translation[2] * UnitScale),
				FVector(Scale[0], Scale[1], Scale[2]));
		}

		bool ApplyWallSegment(
			UWorld& World,
			const FUeAgentCanonicalValuePtr& After,
			const FResolvedReferenceMap& Resolved,
			TArray<FRollbackAction>& OutRollback,
			FExecutionMutationError& OutError)
		{
			FString ClassRef;
			FString MeshRef;
			FString Label;
			FString Folder;
			ReadString(
				After,
				TEXT("actor_class_ref"),
				ClassRef,
				true,
				OutError);
			ReadString(
				After,
				TEXT("mesh_ref"),
				MeshRef,
				true,
				OutError);
			ReadString(
				After,
				TEXT("label"),
				Label,
				true,
				OutError);
			ReadString(
				After,
				TEXT("folder_path"),
				Folder,
				false,
				OutError);
			TArray<double> Position;
			TArray<double> Normal;
			TArray<double> Up;
			TArray<double> Dimensions;
			if (!ReadNumberArray(
					After,
					TEXT("position"),
					3,
					Position,
					true,
					OutError)
				|| !ReadNumberArray(
					After,
					TEXT("basis_normal"),
					3,
					Normal,
					true,
					OutError)
				|| !ReadNumberArray(
					After,
					TEXT("basis_up"),
					3,
					Up,
					true,
					OutError)
				|| !ReadNumberArray(
					After,
					TEXT("dimensions_cm"),
					3,
					Dimensions,
					true,
					OutError))
			{
				return false;
			}
			const FVector N(
				Normal[0],
				Normal[1],
				Normal[2]);
			const FVector U(Up[0], Up[1], Up[2]);
			const FQuat Rotation =
				FRotationMatrix::MakeFromXZ(N, U).ToQuat();
			const FTransform Transform(
				Rotation,
				FVector(
					Position[0],
					Position[1],
					Position[2]),
				FVector(
					Dimensions[0] / 100.0,
					Dimensions[1] / 100.0,
					Dimensions[2] / 100.0));
			UClass* ActorClass =
				CastChecked<UClass>(
					FindResolved(Resolved, ClassRef));
			AActor* Actor = World.SpawnActor<AActor>(
				ActorClass,
				Transform);
			if (!Actor)
			{
				return Fail(
					OutError,
					TEXT("world_simple_wall_spawn_failed"));
			}
			if (AStaticMeshActor* MeshActor =
				Cast<AStaticMeshActor>(Actor))
			{
				MeshActor->GetStaticMeshComponent()->SetStaticMesh(
					CastChecked<UStaticMesh>(
						FindResolved(Resolved, MeshRef)));
			}
			Actor->SetActorLabel(Label);
			if (!Folder.IsEmpty())
			{
				Actor->SetFolderPath(FName(*Folder));
			}
			OutRollback.Add({
				ERollbackKind::DestroyActor,
				Actor,
			});
			return true;
		}

		void ApplyDataLayerStateFields(
			UDataLayerInstance& Instance,
			const FUeAgentCanonicalValuePtr& After)
		{
			bool Value = false;
			bool bPresent = false;
			FExecutionMutationError Ignored;
			if (ReadBool(
					After,
					TEXT("visible"),
					Value,
					bPresent,
					Ignored)
				&& bPresent)
			{
				Instance.SetVisible(Value);
			}
			if (ReadBool(
					After,
					TEXT("loaded_in_editor"),
					Value,
					bPresent,
					Ignored)
				&& bPresent)
			{
				Instance.SetIsLoadedInEditor(Value, true);
			}
			if (ReadBool(
					After,
					TEXT("initially_visible"),
					Value,
					bPresent,
					Ignored)
				&& bPresent)
			{
				Instance.SetIsInitiallyVisible(Value);
			}
			FString RuntimeState;
			if (ReadString(
					After,
					TEXT("initial_runtime_state"),
					RuntimeState,
					false,
					Ignored)
				&& !RuntimeState.IsEmpty())
			{
				Instance.SetInitialRuntimeState(
					ParseRuntimeState(RuntimeState));
			}
			FString ShortName;
			if (ReadString(
					After,
					TEXT("short_name"),
					ShortName,
					false,
					Ignored)
				&& !ShortName.IsEmpty()
				&& Instance.CanEditDataLayerShortName())
			{
				FDataLayerUtils::SetDataLayerShortName(
					&Instance,
					ShortName);
			}
		}

		bool ApplyEntityCreate(
			UWorld& World,
			const FUeAgentCanonicalValuePtr& After,
			const FResolvedReferenceMap& Resolved,
			TArray<FRollbackAction>& OutRollback,
			FExecutionMutationError& OutError)
		{
			FString Kind;
			ReadString(
				After,
				TEXT("entity_kind"),
				Kind,
				true,
				OutError);
			if (Kind == TEXT("wall_segment"))
			{
				return ApplyWallSegment(
					World,
					After,
					Resolved,
					OutRollback,
					OutError);
			}
			if (Kind == TEXT("actor"))
			{
				FString ClassRef;
				FString Label;
				FString Folder;
				if (!ReadString(After, TEXT("actor_class_ref"), ClassRef, true, OutError)
					|| !ReadString(After, TEXT("label"), Label, true, OutError)
					|| !ReadString(After, TEXT("folder_path"), Folder, false, OutError))
				{
					return false;
				}
				const TArray<FString> Tags = ReadStringArray(
					After, TEXT("tags"), OutError);
				if (OutError.IsPresent())
				{
					return false;
				}
				bool bSaveLevel = false;
				bool bSaveLevelPresent = false;
				if (!ReadBool(
						After,
						TEXT("save_level"),
						bSaveLevel,
						bSaveLevelPresent,
						OutError)
					|| !bSaveLevelPresent)
				{
					return Fail(
						OutError,
						TEXT("world_simple_actor_save_level_missing"));
				}
				const FTransform Transform = ReadTransform(After, OutError);
				if (OutError.IsPresent())
				{
					return false;
				}
				FActorSpawnParameters SpawnParameters;
				SpawnParameters.OverrideLevel = World.GetCurrentLevel();
				SpawnParameters.ObjectFlags |= RF_Transactional;
				SpawnParameters.SpawnCollisionHandlingOverride =
					ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				AActor* Actor = World.SpawnActor<AActor>(
					CastChecked<UClass>(FindResolved(Resolved, ClassRef)),
					Transform,
					SpawnParameters);
				if (!Actor)
				{
					return Fail(OutError, TEXT("world_simple_actor_spawn_failed"));
				}
				Actor->Modify();
				Actor->SetActorLabel(Label);
				if (!Folder.IsEmpty())
				{
					Actor->SetFolderPath(FName(*Folder));
				}
				for (const FString& Tag : Tags)
				{
					Actor->Tags.AddUnique(FName(*Tag));
				}
				Actor->PostEditMove(true);
				Actor->MarkPackageDirty();
				FRollbackAction Rollback;
				Rollback.Kind = ERollbackKind::DestroyActor;
				Rollback.Primary = Actor;
				Rollback.bValueA = bSaveLevel;
				OutRollback.Add(Rollback);
				if (bSaveLevel
					&& !UEditorLoadingAndSavingUtils::SaveMap(
						&World,
						World.GetOutermost()->GetName()))
				{
					OutRollback.Pop();
					World.DestroyActor(Actor);
					return Fail(
						OutError,
						TEXT("world_simple_actor_save_failed"),
						World.GetOutermost()->GetName());
				}
				return true;
			}
			if (Kind == TEXT("data_layer_instance"))
			{
				FString ParentRef;
				ReadString(
					After,
					TEXT("parent_ref"),
					ParentRef,
					false,
					OutError);
				bool bPrivate = false;
				bool bPrivatePresent = false;
				ReadBool(
					After,
					TEXT("is_private"),
					bPrivate,
					bPrivatePresent,
					OutError);
				AWorldDataLayers* WorldDataLayers =
					World.GetWorldDataLayers();
				UDataLayerAsset* Asset = nullptr;
				if (!bPrivate)
				{
					FString AssetRef;
					ReadString(
						After,
						TEXT("asset_ref"),
						AssetRef,
						true,
						OutError);
					Asset = CastChecked<UDataLayerAsset>(
						FindResolved(Resolved, AssetRef));
				}
				UDataLayerInstance* Instance = nullptr;
				if (UDataLayerEditorSubsystem* Subsystem =
					GEditor
						? GEditor->GetEditorSubsystem<
							UDataLayerEditorSubsystem>()
						: nullptr)
				{
					FDataLayerCreationParameters Parameters;
					Parameters.DataLayerAsset = Asset;
					Parameters.WorldDataLayers = WorldDataLayers;
					Parameters.bIsPrivate =
						bPrivatePresent && bPrivate;
					Instance =
						Subsystem->CreateDataLayerInstance(
							Parameters);
				}
				if (!Instance)
				{
					if (bPrivatePresent && bPrivate)
					{
						return Fail(
							OutError,
							TEXT(
								"world_simple_private_data_layer_create_failed"));
					}
					Instance = WorldDataLayers
						->CreateDataLayer<
							UDataLayerInstanceWithAsset>(Asset);
				}
				if (!Instance)
				{
					return Fail(
						OutError,
						TEXT(
							"world_simple_data_layer_create_failed"));
				}
				if (!ParentRef.IsEmpty()
					&& !Instance->SetParent(
						CastChecked<UDataLayerInstance>(
							FindResolved(
								Resolved,
								ParentRef))))
				{
					WorldDataLayers->RemoveDataLayer(Instance);
					return Fail(
						OutError,
						TEXT(
							"world_simple_data_layer_parent_failed"));
				}
				ApplyDataLayerStateFields(*Instance, After);
				OutRollback.Add({
					ERollbackKind::RemoveDataLayerInstance,
					Instance,
					WorldDataLayers,
				});
				return true;
			}

			FString LevelRef;
			FString ClassRef;
			ReadString(
				After,
				TEXT("level_asset_ref"),
				LevelRef,
				true,
				OutError);
			ReadString(
				After,
				TEXT("streaming_class_ref"),
				ClassRef,
				true,
				OutError);
			bool bHasTransform = false;
			bool bPresent = false;
			ReadBool(
				After,
				TEXT("has_transform"),
				bHasTransform,
				bPresent,
				OutError);
			FTransform Transform = FTransform::Identity;
			if (bPresent && bHasTransform)
			{
				Transform = ReadTransform(After, OutError);
				if (OutError.IsPresent())
				{
					return false;
				}
			}
			UWorld* LevelAsset =
				CastChecked<UWorld>(
					FindResolved(Resolved, LevelRef));
			UClass* StreamingClass =
				CastChecked<UClass>(
					FindResolved(Resolved, ClassRef));
			ULevelStreaming* Relation =
				UEditorLevelUtils::AddLevelToWorld(
					&World,
					*LevelAsset->GetOutermost()->GetName(),
					StreamingClass,
					Transform);
			if (!Relation)
			{
				return Fail(
					OutError,
					TEXT(
						"world_simple_streaming_relation_create_failed"));
			}
			OutRollback.Add({
				ERollbackKind::RemoveStreamingRelation,
				Relation,
			});
			return true;
		}

		bool ApplyMembership(
			const FUeAgentCanonicalValuePtr& After,
			const FResolvedReferenceMap& Resolved,
			TArray<FRollbackAction>& OutRollback,
			FExecutionMutationError& OutError)
		{
			FString Kind;
			FString SubjectRef;
			FString Mode;
			ReadString(
				After,
				TEXT("membership_kind"),
				Kind,
				true,
				OutError);
			ReadString(
				After,
				TEXT("subject_ref"),
				SubjectRef,
				true,
				OutError);
			ReadString(
				After,
				TEXT("mode"),
				Mode,
				true,
				OutError);
			if (Kind == TEXT("actor_hlod_layer"))
			{
				AActor* Actor =
					CastChecked<AActor>(
						FindResolved(Resolved, SubjectRef));
				FRollbackAction Rollback;
				Rollback.Kind = ERollbackKind::RestoreActorHlod;
				Rollback.Primary = Actor;
				Rollback.Secondary = Actor->GetHLODLayer();
				FString MemberRef;
				ReadString(
					After,
					TEXT("member_ref"),
					MemberRef,
					false,
					OutError);
				Actor->Modify();
				Actor->SetHLODLayer(
					MemberRef.IsEmpty()
						? nullptr
						: CastChecked<UHLODLayer>(
							FindResolved(
								Resolved,
								MemberRef)));
				OutRollback.Add(MoveTemp(Rollback));
				return true;
			}

			FString RelationKind;
			ReadString(
				After,
				TEXT("relation_kind"),
				RelationKind,
				true,
				OutError);
			const TArray<FString> MemberRefs =
				ReadStringArray(
					After,
					TEXT("member_refs"),
					OutError);
			if (RelationKind == TEXT("parent"))
			{
				UDataLayerInstance* Subject =
					CastChecked<UDataLayerInstance>(
						FindResolved(Resolved, SubjectRef));
				FRollbackAction Rollback;
				Rollback.Kind =
					ERollbackKind::RestoreDataLayerParent;
				Rollback.Primary = Subject;
				Rollback.Secondary = Subject->GetParent();
				UDataLayerInstance* Parent =
					MemberRefs.IsEmpty()
						? nullptr
						: CastChecked<UDataLayerInstance>(
							FindResolved(
								Resolved,
								MemberRefs[0]));
				if (!Subject->SetParent(Parent))
				{
					return Fail(
						OutError,
						TEXT(
							"world_simple_data_layer_parent_failed"));
				}
				OutRollback.Add(MoveTemp(Rollback));
				return true;
			}

			AActor* Actor =
				CastChecked<AActor>(
					FindResolved(Resolved, SubjectRef));
			FRollbackAction Rollback;
			Rollback.Kind =
				ERollbackKind::RestoreActorDataLayers;
			Rollback.Primary = Actor;
			for (const UDataLayerInstance* Existing
				: Actor->GetDataLayerInstances())
			{
				Rollback.Objects.Add(
					const_cast<UDataLayerInstance*>(Existing));
			}
			TArray<UDataLayerInstance*> Members;
			for (const FString& MemberRef : MemberRefs)
			{
				Members.Add(
					CastChecked<UDataLayerInstance>(
						FindResolved(Resolved, MemberRef)));
			}
			UDataLayerManager* DataLayerManager =
				UDataLayerManager::GetDataLayerManager(Actor);
			AWorldDataLayers* ActorWorldDataLayers =
				Actor->GetLevel()
					? Actor->GetLevel()->GetWorldDataLayers()
					: nullptr;
			if (!DataLayerManager || !ActorWorldDataLayers)
			{
				return Fail(
					OutError,
					TEXT(
						"world_simple_data_layer_manager_missing"));
			}
			for (const UDataLayerInstance* Member : Members)
			{
				const UDataLayerAsset* Asset =
					Member ? Member->GetAsset() : nullptr;
				if (!Member
					|| Member->GetOuterWorldDataLayers()
						!= ActorWorldDataLayers
					|| !Asset
					|| DataLayerManager
							->GetDataLayerInstance(Asset)
						!= Member)
				{
					return Fail(
						OutError,
						TEXT(
							"world_simple_data_layer_scope_mismatch"));
				}
			}
			Actor->Modify();
			if (Mode == TEXT("replace")
				|| Mode == TEXT("clear"))
			{
				Actor->RemoveAllDataLayers();
			}
			if (Mode != TEXT("clear"))
			{
				for (UDataLayerInstance* Member : Members)
				{
					if (Mode == TEXT("remove"))
					{
						Actor->RemoveDataLayer(Member);
					}
					else
					{
						Actor->AddDataLayer(Member);
					}
				}
			}
			OutRollback.Add(MoveTemp(Rollback));
			return true;
		}

		bool ApplyState(
			UWorld& World,
			const FUeAgentCanonicalValuePtr& After,
			const FResolvedReferenceMap& Resolved,
			TArray<FRollbackAction>& OutRollback,
			FExecutionMutationError& OutError)
		{
			FString Kind;
			ReadString(
				After,
				TEXT("state_kind"),
				Kind,
				true,
				OutError);
			if (Kind == TEXT("world_partition_streaming"))
			{
				UWorldPartition* WorldPartition =
					World.GetWorldPartition();
				bool bEnabled = false;
				bool bPresent = false;
				if (!ReadBool(
						After,
						TEXT("enabled"),
						bEnabled,
						bPresent,
						OutError)
					|| !bPresent)
				{
					return Fail(
						OutError,
						TEXT(
							"world_simple_internal_field_missing"),
						TEXT("enabled"));
				}
				FRollbackAction Rollback;
				Rollback.Kind =
					ERollbackKind::
						RestoreWorldPartitionStreaming;
				Rollback.Primary = WorldPartition;
				Rollback.bValueA =
					WorldPartition->IsStreamingEnabledInEditor();
				WorldPartition->SetEnableStreaming(bEnabled);
				OutRollback.Add(MoveTemp(Rollback));
				return true;
			}
			if (Kind == TEXT("level_game_mode"))
			{
				FString GameModeClassRef;
				bool bSaveLevel = false;
				bool bSaveLevelPresent = false;
				if (!ReadString(
						After,
						TEXT("game_mode_class_ref"),
						GameModeClassRef,
						true,
						OutError)
					|| !ReadBool(
						After,
						TEXT("save_level"),
						bSaveLevel,
						bSaveLevelPresent,
						OutError)
					|| !bSaveLevelPresent)
				{
					return Fail(
						OutError,
						TEXT("world_simple_level_game_mode_payload_invalid"));
				}
				AWorldSettings* WorldSettings = World.GetWorldSettings();
				UClass* GameModeClass = CastChecked<UClass>(
					FindResolved(Resolved, GameModeClassRef));
				if (!WorldSettings)
				{
					return Fail(
						OutError,
						TEXT("world_simple_world_settings_missing"));
				}
				FRollbackAction Rollback;
				Rollback.Kind = ERollbackKind::RestoreLevelGameMode;
				Rollback.Primary = WorldSettings;
				Rollback.Secondary = WorldSettings->DefaultGameMode.Get();
				Rollback.bValueA = bSaveLevel;
				Rollback.bValueB = World.GetOutermost()->IsDirty();
				WorldSettings->DefaultGameMode = GameModeClass;
				WorldSettings->MarkPackageDirty();
				OutRollback.Add(Rollback);
				if (bSaveLevel
					&& !UEditorLoadingAndSavingUtils::SaveMap(
						&World,
						World.GetOutermost()->GetName()))
				{
					OutRollback.Pop();
					WorldSettings->DefaultGameMode = Cast<UClass>(
						Rollback.Secondary.Get());
					World.GetOutermost()->SetDirtyFlag(
						Rollback.bValueB);
					return Fail(
						OutError,
						TEXT("world_simple_level_game_mode_save_failed"),
						World.GetOutermost()->GetName());
				}
				return true;
			}

			FString TargetRef;
			ReadString(
				After,
				TEXT("target_ref"),
				TargetRef,
				true,
				OutError);
			if (Kind == TEXT("data_layer"))
			{
				UDataLayerInstance* Instance =
					CastChecked<UDataLayerInstance>(
						FindResolved(Resolved, TargetRef));
				FRollbackAction Rollback;
				Rollback.Kind =
					ERollbackKind::RestoreDataLayerState;
				Rollback.Primary = Instance;
				Rollback.Integer = static_cast<int32>(
					Instance->GetInitialRuntimeState());
				Rollback.bValueA = Instance->IsVisible();
				Rollback.bValueB =
					Instance->IsLoadedInEditor();
				Rollback.bValueC =
					Instance->IsInitiallyVisible();
				Rollback.Text =
					Instance->GetDataLayerShortName();
				Instance->Modify();
				ApplyDataLayerStateFields(*Instance, After);
				OutRollback.Add(MoveTemp(Rollback));
				return true;
			}
			if (Kind == TEXT("actor_streaming_policy"))
			{
				AActor* Actor =
					CastChecked<AActor>(
						FindResolved(Resolved, TargetRef));
				FRollbackAction Rollback;
				Rollback.Kind =
					ERollbackKind::
						RestoreActorStreamingPolicy;
				Rollback.Primary = Actor;
				Rollback.Name = Actor->GetRuntimeGrid();
				Rollback.bValueA =
					Actor->GetIsSpatiallyLoaded();
				FString GridIntent;
				FString GridValue;
				ReadString(
					After,
					TEXT("runtime_grid_intent"),
					GridIntent,
					false,
					OutError);
				ReadString(
					After,
					TEXT("runtime_grid_value"),
					GridValue,
					false,
					OutError);
				bool bSpatial = false;
				bool bSpatialPresent = false;
				ReadBool(
					After,
					TEXT("is_spatially_loaded"),
					bSpatial,
					bSpatialPresent,
					OutError);
				Actor->Modify();
				if (GridIntent == TEXT("set"))
				{
					Actor->SetRuntimeGrid(FName(*GridValue));
				}
				else if (GridIntent == TEXT("clear"))
				{
					Actor->SetRuntimeGrid(NAME_None);
				}
				if (bSpatialPresent)
				{
					Actor->SetIsSpatiallyLoaded(bSpatial);
				}
				OutRollback.Add(MoveTemp(Rollback));
				return true;
			}

			ULevelStreaming* Relation =
				CastChecked<ULevelStreaming>(
					FindResolved(Resolved, TargetRef));
			FRollbackAction Rollback;
			Rollback.Kind =
				ERollbackKind::RestoreStreamingState;
			Rollback.Primary = Relation;
			Rollback.bValueA = Relation->ShouldBeLoaded();
			Rollback.bValueB = Relation->ShouldBeVisible();
			Rollback.Transform = Relation->LevelTransform;
			bool Value = false;
			bool bPresent = false;
			if (ReadBool(
					After,
					TEXT("loaded"),
					Value,
					bPresent,
					OutError)
				&& bPresent)
			{
				Relation->SetShouldBeLoaded(Value);
			}
			if (ReadBool(
					After,
					TEXT("visible"),
					Value,
					bPresent,
					OutError)
				&& bPresent)
			{
				Relation->SetShouldBeVisible(Value);
			}
			bool bHasTransform = false;
			if (ReadBool(
					After,
					TEXT("has_transform"),
					bHasTransform,
					bPresent,
					OutError)
				&& bPresent
				&& bHasTransform)
			{
				Relation->LevelTransform =
					ReadTransform(After, OutError);
			}
			Relation->Modify();
			OutRollback.Add(MoveTemp(Rollback));
			return !OutError.IsPresent();
		}

		bool ApplyChange(
			UWorld& World,
			const FPlannedChange& Change,
			const FResolvedReferenceMap& Resolved,
			TArray<FRollbackAction>& OutRollback,
			FExecutionMutationError& OutError)
		{
			const FString& TargetKind =
				Change.GetIdentity().GetTargetKind();
			if (TargetKind == TEXT("world.entity.create"))
			{
				return ApplyEntityCreate(
					World,
					Change.GetAfter(),
					Resolved,
					OutRollback,
					OutError);
			}
			if (TargetKind == TEXT("world.membership.update"))
			{
				return ApplyMembership(
					Change.GetAfter(),
					Resolved,
					OutRollback,
					OutError);
			}
			if (TargetKind == TEXT("world.state.update"))
			{
				return ApplyState(
					World,
					Change.GetAfter(),
					Resolved,
					OutRollback,
					OutError);
			}
			return Fail(
				OutError,
				TEXT("world_simple_native_target_kind_unknown"),
				TargetKind);
		}

		bool RollbackAction(
			UWorld& World,
			const FRollbackAction& Action)
		{
			switch (Action.Kind)
			{
			case ERollbackKind::DestroyActor:
				if (AActor* Actor =
					Cast<AActor>(Action.Primary.Get()))
				{
					const bool bDestroyed = World.DestroyActor(Actor);
					if (!bDestroyed)
					{
						return false;
					}
					return !Action.bValueA
						|| UEditorLoadingAndSavingUtils::SaveMap(
							&World,
							World.GetOutermost()->GetName());
				}
				return true;
			case ERollbackKind::RemoveDataLayerInstance:
				if (AWorldDataLayers* WorldDataLayers =
					Cast<AWorldDataLayers>(
						Action.Secondary.Get()))
				{
					if (UDataLayerInstance* Instance =
						Cast<UDataLayerInstance>(
							Action.Primary.Get()))
					{
						return WorldDataLayers->RemoveDataLayer(
							Instance);
					}
				}
				return true;
			case ERollbackKind::RestoreActorDataLayers:
				if (AActor* Actor =
					Cast<AActor>(Action.Primary.Get()))
				{
					Actor->RemoveAllDataLayers();
					for (const TWeakObjectPtr<UObject>& Object
						: Action.Objects)
					{
						if (UDataLayerInstance* Instance =
							Cast<UDataLayerInstance>(
								Object.Get()))
						{
							Actor->AddDataLayer(Instance);
						}
					}
				}
				return true;
			case ERollbackKind::RestoreDataLayerParent:
				if (UDataLayerInstance* Instance =
					Cast<UDataLayerInstance>(
						Action.Primary.Get()))
				{
					return Instance->SetParent(
						Cast<UDataLayerInstance>(
							Action.Secondary.Get()));
				}
				return true;
			case ERollbackKind::RestoreActorHlod:
				if (AActor* Actor =
					Cast<AActor>(Action.Primary.Get()))
				{
					Actor->SetHLODLayer(
						Cast<UHLODLayer>(
							Action.Secondary.Get()));
				}
				return true;
			case ERollbackKind::RestoreDataLayerState:
				if (UDataLayerInstance* Instance =
					Cast<UDataLayerInstance>(
						Action.Primary.Get()))
				{
					Instance->SetInitialRuntimeState(
						static_cast<EDataLayerRuntimeState>(
							Action.Integer));
					Instance->SetVisible(Action.bValueA);
					Instance->SetIsLoadedInEditor(
						Action.bValueB,
						true);
					Instance->SetIsInitiallyVisible(
						Action.bValueC);
					if (Instance->CanEditDataLayerShortName())
					{
						FDataLayerUtils::SetDataLayerShortName(
							Instance,
							Action.Text);
					}
				}
				return true;
			case ERollbackKind::RestoreActorStreamingPolicy:
				if (AActor* Actor =
					Cast<AActor>(Action.Primary.Get()))
				{
					Actor->SetRuntimeGrid(Action.Name);
					Actor->SetIsSpatiallyLoaded(
						Action.bValueA);
				}
				return true;
			case ERollbackKind::RemoveStreamingRelation:
				if (ULevelStreaming* Relation =
					Cast<ULevelStreaming>(
						Action.Primary.Get()))
				{
					if (ULevel* Level =
						Relation->GetLoadedLevel())
					{
						return UEditorLevelUtils::
							RemoveLevelFromWorld(
								Level,
								true,
								false);
					}
				}
				return true;
			case ERollbackKind::RestoreStreamingState:
				if (ULevelStreaming* Relation =
					Cast<ULevelStreaming>(
						Action.Primary.Get()))
				{
					Relation->SetShouldBeLoaded(
						Action.bValueA);
					Relation->SetShouldBeVisible(
						Action.bValueB);
					Relation->LevelTransform =
						Action.Transform;
				}
				return true;
			case ERollbackKind::
				RestoreWorldPartitionStreaming:
				if (UWorldPartition* WorldPartition =
					Cast<UWorldPartition>(
						Action.Primary.Get()))
				{
					WorldPartition->SetEnableStreaming(
						Action.bValueA);
				}
				return true;
			case ERollbackKind::RestoreLevelGameMode:
				if (AWorldSettings* WorldSettings =
					Cast<AWorldSettings>(Action.Primary.Get()))
				{
					WorldSettings->DefaultGameMode = Cast<UClass>(
						Action.Secondary.Get());
					WorldSettings->MarkPackageDirty();
					if (Action.bValueA)
					{
						return UEditorLoadingAndSavingUtils::SaveMap(
							&World,
							World.GetOutermost()->GetName());
					}
					World.GetOutermost()->SetDirtyFlag(Action.bValueB);
				}
				return true;
			default:
				return false;
			}
		}

		bool RollbackAll(
			UWorld& World,
			const TArray<FRollbackAction>& Actions)
		{
			bool bConfirmed = true;
			for (int32 Index = Actions.Num() - 1;
				Index >= 0;
				--Index)
			{
				bConfirmed =
					RollbackAction(World, Actions[Index])
					&& bConfirmed;
			}
			return bConfirmed;
		}

		class FWorldSimpleEffectNativeHook final
			: public IUeAgentWorldSimpleEffectNativeHook
		{
		public:
			explicit FWorldSimpleEffectNativeHook(
				TSharedRef<
					IUeAgentWorldSimpleEffectReferenceResolver,
					ESPMode::ThreadSafe> InResolver)
				: Resolver(MoveTemp(InResolver))
			{
			}

			bool TryApply(
				UWorld& World,
				const FExecutionMutationRequest& Request,
				const FLockFenceLease& Lease,
				const TArray<FUeAgentGenericEffectBinding>& Bindings,
				FUeAgentResourceTracker& Resources,
				FUeAgentGenericEffectApplyResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				OutResult = FUeAgentGenericEffectApplyResult();
				OutError = FExecutionMutationError();
				if (!IsInGameThread())
				{
					return Fail(
						OutError,
						TEXT(
							"world_simple_game_thread_required"));
				}
				if (!Request.IsValid()
					|| !Lease.IsValid()
					|| Request.GetExecutionContext()
							.GetWorldIdentity()
						!= World.GetPathName())
				{
					return Fail(
						OutError,
						TEXT(
							"world_simple_exact_world_context_mismatch"));
				}
				const TArray<FPlannedChange>& Changes =
					Request.GetPlan()
						->GetChangeSet()
						.GetChanges();
				if (Changes.IsEmpty()
					|| Bindings.Num() != Changes.Num())
				{
					return Fail(
						OutError,
						TEXT(
							"world_simple_change_binding_mismatch"));
				}

				TSet<FString> References;
				for (const FPlannedChange& Change : Changes)
				{
					if (!CollectReferences(
							Change,
							References,
							OutError))
					{
						return false;
					}
				}
				FResolvedReferenceMap Resolved;
				if (!ResolveAllReferences(
						World,
						*Resolver,
						References,
						Resolved,
						OutError))
				{
					return false;
				}
				for (const FPlannedChange& Change : Changes)
				{
					if (!ValidateResolvedTypes(
							World,
							Change,
							Resolved,
							OutError))
					{
						return false;
					}
				}

				TArray<FRollbackAction> Rollback;
				TArray<FString> MutatedKeys;
				for (const FPlannedChange& Change : Changes)
				{
					if (!ApplyChange(
							World,
							Change,
							Resolved,
							Rollback,
							OutError))
					{
						RollbackAll(World, Rollback);
						return false;
					}
					MutatedKeys.Add(
						Change.GetIdentity().GetCanonicalKey());
				}
				const FString Token = FString::Printf(
					TEXT("world-simple:%s:%lld"),
					*Request.GetOperationId(),
					Lease.GetFencingToken());
				{
					FScopeLock Lock(&StateMutex);
					RollbackByToken.Add(Token, Rollback);
					TokenByOperation.Add(
						Request.GetOperationId(),
						Token);
				}
				OutResult = FUeAgentGenericEffectApplyResult(
					Lease.GetFencingToken(),
					MoveTemp(MutatedKeys),
					{TEXT("world.authoring.memory")},
					true,
					Token,
					true);
				return OutResult.IsValid()
					|| Fail(
						OutError,
						TEXT(
							"world_simple_apply_result_invalid"));
			}

			bool TryRollback(
				UWorld& World,
				const FExecutionMutationRequest& Request,
				const FUeAgentGenericEffectApplyResult& Applied,
				FUeAgentResourceTracker& Resources,
				bool& bOutConfirmed,
				FExecutionMutationError& OutError) override
			{
				bOutConfirmed = false;
				OutError = FExecutionMutationError();
				if (!IsInGameThread()
					|| !Applied.IsValid()
					|| Request.GetExecutionContext()
							.GetWorldIdentity()
						!= World.GetPathName())
				{
					return Fail(
						OutError,
						TEXT(
							"world_simple_rollback_request_invalid"));
				}
				TArray<FRollbackAction> Actions;
				{
					FScopeLock Lock(&StateMutex);
					TArray<FRollbackAction>* Found =
						RollbackByToken.Find(
							Applied.GetRollbackToken());
					if (!Found)
					{
						return Fail(
							OutError,
							TEXT(
								"world_simple_rollback_token_unknown"));
					}
					Actions = *Found;
				}
				bOutConfirmed = RollbackAll(World, Actions);
				if (!bOutConfirmed)
				{
					return Fail(
						OutError,
						TEXT(
							"world_simple_rollback_unconfirmed"));
				}
				{
					FScopeLock Lock(&StateMutex);
					RollbackByToken.Remove(
						Applied.GetRollbackToken());
					TokenByOperation.Remove(
						Request.GetOperationId());
				}
				return true;
			}

			void ReleaseVerifiedOperation(
				const FString& OperationId) override
			{
				FScopeLock Lock(&StateMutex);
				if (const FString* Token =
					TokenByOperation.Find(OperationId))
				{
					RollbackByToken.Remove(*Token);
				}
				TokenByOperation.Remove(OperationId);
			}

			bool TryWaitForSettle(
				const FBarrierRequest& Request,
				FUeAgentResourceTracker& Resources,
				FBarrierResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				OutError = FExecutionMutationError();
				if (!Request.IsValid())
				{
					return Fail(
						OutError,
						TEXT(
							"world_simple_barrier_request_invalid"));
				}
				OutResult = FBarrierResult(
					Request.GetSubjectGeneration(),
					Request.GetRequiredSignals(),
					FString::Printf(
						TEXT("world-simple:%lld"),
						Request.GetSubjectGeneration()),
					true);
				return OutResult.IsValid()
					|| Fail(
						OutError,
						TEXT(
							"world_simple_barrier_result_invalid"));
			}

		private:
			TSharedRef<
				IUeAgentWorldSimpleEffectReferenceResolver,
				ESPMode::ThreadSafe> Resolver;
			FCriticalSection StateMutex;
			TMap<FString, TArray<FRollbackAction>>
				RollbackByToken;
			TMap<FString, FString> TokenByOperation;
		};
	}

	bool FUeAgentWorldSimpleEffectNativeHookFactory::TryCreate(
		TSharedRef<
			IUeAgentWorldSimpleEffectReferenceResolver,
			ESPMode::ThreadSafe> Resolver,
		TSharedPtr<IUeAgentWorldSimpleEffectNativeHook>& OutHook,
		FExecutionMutationError& OutError)
	{
		OutHook = MakeShared<FWorldSimpleEffectNativeHook>(
			MoveTemp(Resolver));
		OutError = FExecutionMutationError();
		return OutHook.IsValid();
	}
}
