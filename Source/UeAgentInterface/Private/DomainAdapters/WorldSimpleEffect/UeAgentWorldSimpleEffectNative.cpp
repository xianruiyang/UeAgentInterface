#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectCapabilities.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"

#include "ActorFactories/ActorFactory.h"
#include "Builders/CubeBuilder.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "Editor.h"
#include "EditorLevelUtils.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/Volume.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"
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
	using namespace AssetDurable;
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
			TArray<TWeakObjectPtr<UObject>> DurableDependencies;
		};

		using FResolvedReferenceMap =
			TMap<FString, TWeakObjectPtr<UObject>>;

		bool Fail(FExecutionMutationError& OutError, FExecutionMutationError Diagnostic)
		{
			OutError = MoveTemp(Diagnostic);
			return false;
		}

		FExecutionMutationError NativeFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target = FString())
		{
			auto Error = FExecutionMutationError::Failure(Code, Reason, TEXT("execution"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.TargetPath = Target;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		TSharedPtr<FJsonValue> NativeValue(const FUeAgentCanonicalValuePtr& Value)
		{
			if (!Value) return MakeShared<FJsonValueString>(TEXT("<missing>"));
			switch (Value->GetKind())
			{
			case EUeAgentValueKind::Null: return MakeShared<FJsonValueNull>();
			case EUeAgentValueKind::Boolean: return MakeShared<FJsonValueBoolean>(Value->GetBoolean());
			case EUeAgentValueKind::String: return MakeShared<FJsonValueString>(Value->GetText());
			case EUeAgentValueKind::Integer:
			case EUeAgentValueKind::Decimal: return MakeShared<FJsonValueString>(Value->GetLexeme());
			case EUeAgentValueKind::Array: return MakeShared<FJsonValueString>(TEXT("array"));
			case EUeAgentValueKind::Object: return MakeShared<FJsonValueString>(TEXT("object"));
			default: return MakeShared<FJsonValueString>(FString::Printf(TEXT("value kind %d"), static_cast<int32>(Value->GetKind())));
			}
		}

		FExecutionMutationError RequiredField(const FString& Field)
		{
			return NativeFact(TEXT("world_simple_internal_field_missing"),
				TEXT("Planned world change is missing a required field."), TEXT("/after/") + Field,
				MakeShared<FJsonValueString>(TEXT("<missing>")), MakeShared<FJsonValueString>(TEXT("required value")));
		}

		FExecutionMutationError ReferenceType(const FString& Ref, const FString& Field, UObject* Object, const FString& Expected)
		{
			const UClass* Class = Cast<UClass>(Object);
			return NativeFact(TEXT("world_simple_reference_type_invalid"),
				TEXT("Resolved reference has an incompatible type."), TEXT("/after/") + Field,
				MakeShared<FJsonValueString>(Class ? Class->GetPathName() : Object ? Object->GetClass()->GetPathName() : TEXT("<unresolved>")),
				MakeShared<FJsonValueString>(Expected), Ref);
		}

		void SupplementNative(FExecutionMutationError& Error, const FString& Target)
		{
			if (Error.IsEmpty()) return;
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target;
			if (Error.SourceKind.IsEmpty() && Error.Source.SourceFile.IsEmpty()) Error.SourceKind = TEXT("native_result");
			for (auto& Child : Error.Diagnostics) SupplementNative(Child, Target);
			if (Error.PublicCause)
			{
				auto Cause = MakeShared<FExecutionMutationError>(*Error.PublicCause);
				SupplementNative(*Cause, Target);
				Error.PublicCause = MoveTemp(Cause);
			}
		}

		void PrefixPlannedChange(FExecutionMutationError& Error, const int32 Index)
		{
			if (Error.IsEmpty()) return;
			Error.PropertyPath = TEXT("/changes/") + FString::FromInt(Index)
				+ (Error.PropertyPath.IsEmpty() || Error.PropertyPath.StartsWith(TEXT("/")) ? TEXT("") : TEXT("/")) + Error.PropertyPath;
			for (auto& Child : Error.Diagnostics) PrefixPlannedChange(Child, Index);
			if (Error.PublicCause)
			{
				auto Cause = MakeShared<FExecutionMutationError>(*Error.PublicCause);
				PrefixPlannedChange(*Cause, Index);
				Error.PublicCause = MoveTemp(Cause);
			}
		}

		bool AppendDurableTargets(
			const FPlannedChange& Change,
			const TArray<FRollbackAction>& Rollback,
			const int32 FirstAction,
			TArray<FAssetDurableTarget>& OutTargets,
			FExecutionMutationError& OutError)
		{
			const EAssetDurableIntent Intent =
				Change.GetIntent() == EChangeIntent::Delete
					? EAssetDurableIntent::Delete
					: EAssetDurableIntent::Save;
			for (int32 Index = FirstAction;
				Index < Rollback.Num();
				++Index)
			{
				TArray<TWeakObjectPtr<UObject>> DurableObjects;
				DurableObjects.Add(Rollback[Index].Primary);
				DurableObjects.Append(
					Rollback[Index].DurableDependencies);
				for (const TWeakObjectPtr<UObject>& WeakObject
					: DurableObjects)
				{
					UObject* const DurableObject = WeakObject.Get();
					if (!DurableObject)
					{
						continue;
					}
					UPackage* Package =
						DurableObject->GetExternalPackage();
					if (!Package)
					{
						Package = DurableObject->GetOutermost();
					}
					if (!Package
						|| Package == GetTransientPackage()
						|| !FPackageName::IsValidLongPackageName(
							Package->GetName()))
					{
						return Fail(OutError, NativeFact(TEXT("world_simple_durable_package_invalid"),
							TEXT("Changed object must belong to a non-transient package with a valid long package name."), TEXT("/package"),
							MakeShared<FJsonValueString>(Package ? Package->GetName() : TEXT("<missing>")),
							MakeShared<FJsonValueString>(TEXT("persistent package with a valid long name")), DurableObject->GetPathName()));
					}
					UWorld* const ObjectWorld = DurableObject->GetWorld();
					const bool bMapPackage = Package->ContainsMap()
						|| UWorld::FindWorldInPackage(Package) != nullptr
						|| (ObjectWorld
							&& ObjectWorld->GetOutermost() == Package);
					const FAssetDurableTarget Candidate(
						Change.GetIdentity(),
						Package->GetName(),
						bMapPackage
							? EPackageHeaderKind::Map
							: EPackageHeaderKind::EvidenceRequired,
						Intent,
						ERollbackGrade::DurableBackup);
					const FString CandidateKey =
						Candidate.GetDurableTargetKey();
					const FAssetDurableTarget* Existing =
						OutTargets.FindByPredicate(
							[&CandidateKey](
								const FAssetDurableTarget& Target)
							{
								return Target.GetDurableTargetKey()
									== CandidateKey;
							});
					if (Existing)
					{
						if (Existing->GetIntent()
							!= Candidate.GetIntent())
						{
							return Fail(OutError, NativeFact(TEXT("world_simple_durable_package_conflict"),
								TEXT("The same package has conflicting durable mutation intents."), TEXT("/package/intent"),
								MakeShared<FJsonValueNumber>(static_cast<int32>(Candidate.GetIntent())),
								MakeShared<FJsonValueNumber>(static_cast<int32>(Existing->GetIntent())), Package->GetName()));
						}
						continue;
					}
					OutTargets.Add(Candidate);
				}
			}
			return true;
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

		bool ReadString(const FUeAgentCanonicalValuePtr& Object, const FString& Field, FString& OutValue,
			const bool bRequired, FExecutionMutationError& OutError)
		{
			OutValue.Reset();
			const FUeAgentCanonicalValuePtr Value = FindField(Object, Field);
			if (!Value.IsValid()) return !bRequired || Fail(OutError, RequiredField(Field));
			if (Value->GetKind() != EUeAgentValueKind::String)
				return Fail(OutError, NativeFact(TEXT("world_simple_internal_field_type"),
					TEXT("Planned world-change field must be a string."), TEXT("/after/") + Field,
					NativeValue(Value), MakeShared<FJsonValueString>(TEXT("string"))));
			OutValue = Value->GetText();
			return !OutValue.IsEmpty() || !bRequired || Fail(OutError, NativeFact(TEXT("world_simple_internal_field_empty"),
				TEXT("Required planned world-change string cannot be empty."), TEXT("/after/") + Field,
				MakeShared<FJsonValueString>(OutValue), MakeShared<FJsonValueString>(TEXT("non-empty string"))));
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
				return Fail(OutError, NativeFact(TEXT("world_simple_internal_field_type"),
					TEXT("Planned world-change field must be boolean."), TEXT("/after/") + Field,
					NativeValue(Value), MakeShared<FJsonValueString>(TEXT("boolean"))));
			}
			OutValue = Value->GetBoolean();
			bOutPresent = true;
			return true;
		}

		bool ReadNumberArray(const FUeAgentCanonicalValuePtr& Object, const FString& Field, const int32 ExpectedCount,
			TArray<double>& OutValues, const bool bRequired, FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			const FUeAgentCanonicalValuePtr Value = FindField(Object, Field);
			if (!Value.IsValid()) return !bRequired || Fail(OutError, RequiredField(Field));
			if (Value->GetKind() != EUeAgentValueKind::Array)
				return Fail(OutError, NativeFact(TEXT("world_simple_internal_field_type"),
					TEXT("Planned spatial field must be an array."), TEXT("/after/") + Field,
					NativeValue(Value), MakeShared<FJsonValueString>(TEXT("numeric array"))));
			if (Value->GetArrayItems().Num() != ExpectedCount)
				return Fail(OutError, NativeFact(TEXT("world_simple_internal_field_type"),
					TEXT("Planned spatial field has the wrong component count."), TEXT("/after/") + Field,
					MakeShared<FJsonValueNumber>(Value->GetArrayItems().Num()), MakeShared<FJsonValueNumber>(ExpectedCount)));
			for (int32 Index = 0; Index < Value->GetArrayItems().Num(); ++Index)
			{
				const auto& Item = Value->GetArrayItems()[Index];
				const FString Path = TEXT("/after/") + Field + TEXT("/") + FString::FromInt(Index);
				if (Item->GetKind() != EUeAgentValueKind::Decimal && Item->GetKind() != EUeAgentValueKind::Integer)
					return Fail(OutError, NativeFact(TEXT("world_simple_internal_number_invalid"),
						TEXT("Planned spatial component must be numeric."), Path, NativeValue(FUeAgentCanonicalValuePtr(Item)),
						MakeShared<FJsonValueString>(TEXT("finite number"))));
				const double Number = FCString::Atod(*Item->GetLexeme());
				if (!FMath::IsFinite(Number))
					return Fail(OutError, NativeFact(TEXT("world_simple_internal_number_invalid"),
						TEXT("Planned spatial component is outside the finite numeric range."), Path,
						MakeShared<FJsonValueString>(Item->GetLexeme()), MakeShared<FJsonValueString>(TEXT("finite number"))));
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
				Fail(OutError, NativeFact(TEXT("world_simple_internal_field_type"),
					TEXT("Planned reference field must be an array."), TEXT("/after/") + Field,
					NativeValue(Value), MakeShared<FJsonValueString>(TEXT("array of non-empty strings"))));
				return {};
			}
			for (int32 Index = 0; Index < Value->GetArrayItems().Num(); ++Index)
			{
				const auto& Item = Value->GetArrayItems()[Index];
				if (Item->GetKind() != EUeAgentValueKind::String
					|| Item->GetText().IsEmpty())
				{
					Fail(OutError, NativeFact(TEXT("world_simple_internal_ref_invalid"),
						TEXT("Each planned reference must be a non-empty string."), TEXT("/after/") + Field + TEXT("/") + FString::FromInt(Index),
						NativeValue(FUeAgentCanonicalValuePtr(Item)), MakeShared<FJsonValueString>(TEXT("non-empty reference string"))));
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
				return Fail(OutError, NativeFact(TEXT("world_simple_internal_ref_missing"),
					TEXT("Planned reference cannot be empty."), TEXT("/after/") + Field,
					MakeShared<FJsonValueString>(CanonicalRef), MakeShared<FJsonValueString>(TEXT("non-empty reference"))));
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
					if (!ReadBool(After, TEXT("is_private"), bPrivate, bPrivatePresent, OutError)) return false;
					if (!bPrivatePresent) return Fail(OutError, RequiredField(TEXT("is_private")));
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
				return Fail(OutError, NativeFact(TEXT("world_simple_entity_kind_unknown"),
					TEXT("Planned world change uses an unsupported entity_kind."), TEXT("/after/entity_kind"),
					MakeShared<FJsonValueString>(InternalKind), MakeShared<FJsonValueString>(TEXT("wall_segment, actor, data_layer_instance or streaming_relation"))));
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
					if ((!OutError.IsEmpty()))
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
				return Fail(OutError, NativeFact(TEXT("world_simple_membership_kind_unknown"),
					TEXT("Planned world change uses an unsupported membership_kind."), TEXT("/after/membership_kind"),
					MakeShared<FJsonValueString>(InternalKind), MakeShared<FJsonValueString>(TEXT("data_layer_relation or actor_hlod_layer"))));
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
					return Fail(OutError, NativeFact(TEXT("world_simple_state_kind_unknown"),
						TEXT("Planned world change uses an unsupported state_kind."), TEXT("/after/state_kind"),
						MakeShared<FJsonValueString>(InternalKind), MakeShared<FJsonValueString>(TEXT("world_partition_streaming, level_game_mode, data_layer, actor_streaming_policy or streaming_relation"))));
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
			return Fail(OutError, NativeFact(TEXT("world_simple_native_target_kind_unknown"),
				TEXT("Planned world change uses an unsupported target_kind."), TEXT("/target_kind"),
				MakeShared<FJsonValueString>(TargetKind), MakeShared<FJsonValueString>(TEXT("world.entity.create, world.membership.update or world.state.update"))));
		}

		bool ResolveAllReferences(UWorld& World, IUeAgentWorldSimpleEffectReferenceResolver& Resolver,
			const TSet<FString>& References, FResolvedReferenceMap& OutResolved, FExecutionMutationError& OutError)
		{
			OutResolved.Reset();
			for (const FString& CanonicalRef : References)
			{
				UObject* Object = nullptr;
				const bool bResolved = Resolver.TryResolveReference(World, CanonicalRef, Object, OutError);
				if (!bResolved || !IsValid(Object))
				{
					if (OutError.IsEmpty())
					{
						const auto Actual = MakeShared<FJsonObject>();
						Actual->SetBoolField(TEXT("resolved"), bResolved);
						Actual->SetBoolField(TEXT("object_valid"), IsValid(Object));
						Fail(OutError, NativeFact(TEXT("world_simple_reference_unresolved"),
							bResolved ? TEXT("Reference resolver succeeded without a valid object.") : TEXT("Reference resolution failed without a diagnostic."),
							TEXT("/reference"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("resolved valid object")), CanonicalRef));
					}
					SupplementNative(OutError, CanonicalRef);
					return false;
				}
				OutResolved.Add(CanonicalRef, Object);
			}
			return true;
		}

		bool ValidateResolvedTypes(UWorld& World, const FPlannedChange& Change,
			const FResolvedReferenceMap& Resolved, FExecutionMutationError& OutError)
		{
			const FUeAgentCanonicalValuePtr& After = Change.GetAfter();
			FString Kind;
			if (Change.GetIdentity().GetTargetKind() == TEXT("world.entity.create"))
			{
				if (!ReadString(After, TEXT("entity_kind"), Kind, true, OutError)) return false;
				if (Kind == TEXT("wall_segment"))
				{
					FString ClassRef, MeshRef;
					if (!ReadString(After, TEXT("actor_class_ref"), ClassRef, true, OutError)
						|| !ReadString(After, TEXT("mesh_ref"), MeshRef, true, OutError)) return false;
					UClass* ActorClass = Cast<UClass>(FindResolved(Resolved, ClassRef));
					if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass()))
						return Fail(OutError, ReferenceType(ClassRef, TEXT("actor_class_ref"), FindResolved(Resolved, ClassRef), TEXT("Actor class")));
					return Cast<UStaticMesh>(FindResolved(Resolved, MeshRef))
						|| Fail(OutError, ReferenceType(MeshRef, TEXT("mesh_ref"), FindResolved(Resolved, MeshRef), TEXT("StaticMesh")));
				}
				if (Kind == TEXT("actor"))
				{
					FString ClassRef;
					if (!ReadString(After, TEXT("actor_class_ref"), ClassRef, true, OutError)) return false;
					UClass* ActorClass = Cast<UClass>(FindResolved(Resolved, ClassRef));
					return (ActorClass && ActorClass->IsChildOf(AActor::StaticClass()))
						|| Fail(OutError, ReferenceType(ClassRef, TEXT("actor_class_ref"), FindResolved(Resolved, ClassRef), TEXT("Actor class")));
				}
				if (Kind == TEXT("data_layer_instance"))
				{
					if (!World.GetWorldDataLayers())
						return Fail(OutError, NativeFact(TEXT("world_simple_world_data_layers_missing"),
							TEXT("World has no WorldDataLayers object."), TEXT("/world_data_layers"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), World.GetPathName()));
					bool bPrivate = false, bPrivatePresent = false;
					if (!ReadBool(After, TEXT("is_private"), bPrivate, bPrivatePresent, OutError)) return false;
					if (!bPrivatePresent) return Fail(OutError, RequiredField(TEXT("is_private")));
					if (bPrivate)
					{
						FString ShortName;
						if (!ReadString(After, TEXT("short_name"), ShortName, true, OutError)) return false;
					}
					else
					{
						FString AssetRef;
						if (!ReadString(After, TEXT("asset_ref"), AssetRef, true, OutError)) return false;
						if (!Cast<UDataLayerAsset>(FindResolved(Resolved, AssetRef)))
							return Fail(OutError, ReferenceType(AssetRef, TEXT("asset_ref"), FindResolved(Resolved, AssetRef), TEXT("DataLayerAsset")));
					}
					FString ParentRef;
					if (!ReadString(After, TEXT("parent_ref"), ParentRef, false, OutError)) return false;
					return ParentRef.IsEmpty() || Cast<UDataLayerInstance>(FindResolved(Resolved, ParentRef))
						|| Fail(OutError, ReferenceType(ParentRef, TEXT("parent_ref"), FindResolved(Resolved, ParentRef), TEXT("DataLayerInstance")));
				}
				FString LevelRef, ClassRef;
				if (!ReadString(After, TEXT("level_asset_ref"), LevelRef, true, OutError)
					|| !ReadString(After, TEXT("streaming_class_ref"), ClassRef, true, OutError)) return false;
				if (!Cast<UWorld>(FindResolved(Resolved, LevelRef)))
					return Fail(OutError, ReferenceType(LevelRef, TEXT("level_asset_ref"), FindResolved(Resolved, LevelRef), TEXT("World asset")));
				UClass* StreamingClass = Cast<UClass>(FindResolved(Resolved, ClassRef));
				return (StreamingClass && StreamingClass->IsChildOf(ULevelStreaming::StaticClass()))
					|| Fail(OutError, ReferenceType(ClassRef, TEXT("streaming_class_ref"), FindResolved(Resolved, ClassRef), TEXT("LevelStreaming class")));
			}
			if (Change.GetIdentity().GetTargetKind() == TEXT("world.membership.update"))
			{
				FString SubjectRef;
				if (!ReadString(After, TEXT("subject_ref"), SubjectRef, true, OutError)
					|| !ReadString(After, TEXT("membership_kind"), Kind, true, OutError)) return false;
				UObject* Subject = FindResolved(Resolved, SubjectRef);
				if (Kind == TEXT("data_layer_relation"))
				{
					FString RelationKind;
					if (!ReadString(After, TEXT("relation_kind"), RelationKind, true, OutError)) return false;
					if (RelationKind == TEXT("actor_membership"))
					{
						if (!Cast<AActor>(Subject)) return Fail(OutError, ReferenceType(SubjectRef, TEXT("subject_ref"), Subject, TEXT("Actor")));
					}
					else if (RelationKind == TEXT("parent"))
					{
						if (!Cast<UDataLayerInstance>(Subject)) return Fail(OutError, ReferenceType(SubjectRef, TEXT("subject_ref"), Subject, TEXT("DataLayerInstance")));
					}
					else return Fail(OutError, NativeFact(TEXT("world_simple_relation_kind_unknown"),
						TEXT("Planned data-layer relation kind is unsupported."), TEXT("/after/relation_kind"),
						MakeShared<FJsonValueString>(RelationKind), MakeShared<FJsonValueString>(TEXT("actor_membership or parent"))));
					const auto Refs = ReadStringArray(After, TEXT("member_refs"), OutError);
					if (!OutError.IsEmpty()) return false;
					for (const FString& Ref : Refs)
					{
						if (!Cast<UDataLayerInstance>(FindResolved(Resolved, Ref)))
							return Fail(OutError, ReferenceType(Ref, TEXT("member_refs"), FindResolved(Resolved, Ref), TEXT("DataLayerInstance")));
					}
					return true;
				}
				if (!Cast<AActor>(Subject)) return Fail(OutError, ReferenceType(SubjectRef, TEXT("subject_ref"), Subject, TEXT("Actor")));
				FString MemberRef;
				if (!ReadString(After, TEXT("member_ref"), MemberRef, false, OutError)) return false;
				return MemberRef.IsEmpty() || Cast<UHLODLayer>(FindResolved(Resolved, MemberRef))
					|| Fail(OutError, ReferenceType(MemberRef, TEXT("member_ref"), FindResolved(Resolved, MemberRef), TEXT("HLODLayer")));
			}
			if (!ReadString(After, TEXT("state_kind"), Kind, true, OutError)) return false;
			if (Kind == TEXT("world_partition_streaming"))
				return World.GetWorldPartition() || Fail(OutError, NativeFact(TEXT("world_simple_world_partition_missing"),
					TEXT("World has no WorldPartition."), TEXT("/world_partition"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), World.GetPathName()));
			if (Kind == TEXT("level_game_mode"))
			{
				FString ClassRef;
				if (!ReadString(After, TEXT("game_mode_class_ref"), ClassRef, true, OutError)) return false;
				UClass* GameModeClass = Cast<UClass>(FindResolved(Resolved, ClassRef));
				return (GameModeClass && GameModeClass->IsChildOf(AGameModeBase::StaticClass()))
					|| Fail(OutError, ReferenceType(ClassRef, TEXT("game_mode_class_ref"), FindResolved(Resolved, ClassRef), TEXT("GameModeBase class")));
			}
			FString TargetRef;
			if (!ReadString(After, TEXT("target_ref"), TargetRef, true, OutError)) return false;
			UObject* Target = FindResolved(Resolved, TargetRef);
			const bool bTypeValid = (Kind == TEXT("data_layer") && Cast<UDataLayerInstance>(Target))
				|| (Kind == TEXT("actor_streaming_policy") && Cast<AActor>(Target))
				|| (Kind == TEXT("streaming_relation") && Cast<ULevelStreaming>(Target));
			const FString Expected = Kind == TEXT("data_layer") ? TEXT("DataLayerInstance")
				: Kind == TEXT("actor_streaming_policy") ? TEXT("Actor") : Kind == TEXT("streaming_relation") ? TEXT("LevelStreaming") : TEXT("known state kind and matching target type");
			return bTypeValid || Fail(OutError, ReferenceType(TargetRef, TEXT("target_ref"), Target, Expected));
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
			if (!ReadString(After, TEXT("unit"), Unit, false, OutError)) return FTransform::Identity;
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
			if (!ReadString(After, TEXT("actor_class_ref"), ClassRef, true, OutError)
				|| !ReadString(After, TEXT("mesh_ref"), MeshRef, true, OutError)
				|| !ReadString(After, TEXT("label"), Label, true, OutError)
				|| !ReadString(After, TEXT("folder_path"), Folder, false, OutError)) return false;
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
				const auto Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("actor_class_ref"), ClassRef);
				Actual->SetStringField(TEXT("mesh_ref"), MeshRef);
				Actual->SetStringField(TEXT("label"), Label);
				Actual->SetBoolField(TEXT("spawned"), false);
				return Fail(OutError, NativeFact(TEXT("world_simple_wall_spawn_failed"),
					TEXT("World did not spawn the requested wall-segment Actor."), TEXT("/spawn"),
					MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("spawned Actor")), World.GetPathName()));
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

		bool ApplyDataLayerStateFields(UDataLayerInstance& Instance, const FUeAgentCanonicalValuePtr& After,
			FExecutionMutationError& OutError)
		{
			ON_SCOPE_EXIT { SupplementNative(OutError, Instance.GetPathName()); };
			bool Value = false, bPresent = false;
			if (!ReadBool(After, TEXT("visible"), Value, bPresent, OutError)) return false;
			if (bPresent) Instance.SetVisible(Value);
			if (!ReadBool(After, TEXT("loaded_in_editor"), Value, bPresent, OutError)) return false;
			if (bPresent) Instance.SetIsLoadedInEditor(Value, true);
			if (!ReadBool(After, TEXT("initially_visible"), Value, bPresent, OutError)) return false;
			if (bPresent) Instance.SetIsInitiallyVisible(Value);
			FString RuntimeState;
			if (!ReadString(After, TEXT("initial_runtime_state"), RuntimeState, false, OutError)) return false;
			if (!RuntimeState.IsEmpty()) Instance.SetInitialRuntimeState(ParseRuntimeState(RuntimeState));
			FString ShortName;
			if (!ReadString(After, TEXT("short_name"), ShortName, false, OutError)) return false;
			if (!ShortName.IsEmpty() && Instance.CanEditDataLayerShortName())
				FDataLayerUtils::SetDataLayerShortName(&Instance, ShortName);
			return true;
		}

		bool ApplyEntityCreate(
			UWorld& World,
			const FUeAgentCanonicalValuePtr& After,
			const FResolvedReferenceMap& Resolved,
			TArray<FRollbackAction>& OutRollback,
			FExecutionMutationError& OutError)
		{
			FString Kind;
			if (!ReadString(After, TEXT("entity_kind"), Kind, true, OutError)) return false;
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
				if ((!OutError.IsEmpty()))
				{
					return false;
				}
				const FTransform Transform = ReadTransform(After, OutError);
				if ((!OutError.IsEmpty()))
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
					const auto Actual = MakeShared<FJsonObject>();
					Actual->SetStringField(TEXT("actor_class_ref"), ClassRef);
					Actual->SetStringField(TEXT("label"), Label);
					Actual->SetBoolField(TEXT("spawned"), false);
					return Fail(OutError, NativeFact(TEXT("world_simple_actor_spawn_failed"),
						TEXT("World did not spawn the requested Actor."), TEXT("/spawn"),
						MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("spawned Actor")), World.GetPathName()));
				}
				if (AVolume* Volume = Cast<AVolume>(Actor))
				{
					UCubeBuilder* Builder = NewObject<UCubeBuilder>(
						GetTransientPackage(),
						NAME_None,
						RF_Transactional);
					if (!Builder)
					{
						const FString ActorPath = Actor->GetPathName();
						const bool bDestroyed = World.DestroyActor(Actor);
						auto Error = NativeFact(TEXT("world_simple_volume_brush_builder_unavailable"), TEXT("Could not create the cube brush builder for the volume Actor."), TEXT("/brush_builder"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), ActorPath);
						if (!bDestroyed) Error.Append(NativeFact(TEXT("world_simple_rollback_destroy_failed"),
							TEXT("Editor did not confirm destroying the newly created volume Actor."), TEXT("/destroy_confirmed"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), ActorPath));
						return Fail(OutError, MoveTemp(Error));
					}
					UActorFactory::CreateBrushForVolumeActor(
						Volume,
						Builder);
					const FBox Bounds =
						Volume->GetComponentsBoundingBox(true);
					if (!Bounds.IsValid
						|| Bounds.GetExtent().GetMin()
							<= UE_KINDA_SMALL_NUMBER)
					{
						const auto Actual = MakeShared<FJsonObject>();
						Actual->SetBoolField(TEXT("valid"), Bounds.IsValid != 0);
						if (Bounds.IsValid) Actual->SetNumberField(TEXT("minimum_extent_cm"), Bounds.GetExtent().GetMin());
						const auto Expected = MakeShared<FJsonObject>();
						Expected->SetBoolField(TEXT("valid"), true);
						Expected->SetNumberField(TEXT("minimum_extent_cm_greater_than"), UE_KINDA_SMALL_NUMBER);
						const FString ActorPath = Actor->GetPathName();
						const bool bDestroyed = World.DestroyActor(Actor);
						auto Error = NativeFact(TEXT("world_simple_volume_brush_initialization_failed"), TEXT("Volume brush initialization did not produce valid bounds with sufficient extent."), TEXT("/brush_bounds"),
							MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected), ActorPath);
						if (!bDestroyed) Error.Append(NativeFact(TEXT("world_simple_rollback_destroy_failed"),
							TEXT("Editor did not confirm destroying the newly created volume Actor."), TEXT("/destroy_confirmed"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), ActorPath));
						return Fail(OutError, MoveTemp(Error));
					}
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
				OutRollback.Add(Rollback);
				return true;
			}
			if (Kind == TEXT("data_layer_instance"))
			{
				FString ParentRef;
				if (!ReadString(After, TEXT("parent_ref"), ParentRef, false, OutError)) return false;
				bool bPrivate = false;
				bool bPrivatePresent = false;
				if (!ReadBool(After, TEXT("is_private"), bPrivate, bPrivatePresent, OutError)) return false;
				AWorldDataLayers* WorldDataLayers =
					World.GetWorldDataLayers();
				UDataLayerAsset* Asset = nullptr;
				if (!bPrivate)
				{
					FString AssetRef;
					if (!ReadString(After, TEXT("asset_ref"), AssetRef, true, OutError)) return false;
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
						return Fail(OutError, NativeFact(TEXT("world_simple_private_data_layer_create_failed"),
							TEXT("Data-layer subsystem did not create the requested private instance."), TEXT("/data_layer_instance"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), World.GetPathName()));
					}
					Instance = WorldDataLayers
						->CreateDataLayer<
							UDataLayerInstanceWithAsset>(Asset);
				}
				if (!Instance)
				{
					return Fail(OutError, NativeFact(TEXT("world_simple_data_layer_create_failed"),
						TEXT("Neither data-layer creation path produced an instance for the asset."), TEXT("/data_layer_instance"),
						MakeShared<FJsonValueString>(Asset ? Asset->GetPathName() : TEXT("<missing asset>")),
						MakeShared<FJsonValueString>(TEXT("created DataLayerInstance")), World.GetPathName()));
				}
				if (!ParentRef.IsEmpty()
					&& !Instance->SetParent(
						CastChecked<UDataLayerInstance>(
							FindResolved(
								Resolved,
								ParentRef))))
				{
					const FString InstancePath = Instance->GetPathName();
					const FString ActualParent = Instance->GetParent() ? Instance->GetParent()->GetPathName() : TEXT("<none>");
					const bool bRemoved = WorldDataLayers->RemoveDataLayer(Instance);
					auto Error = NativeFact(TEXT("world_simple_data_layer_parent_failed"),
						TEXT("Created data-layer instance rejected the requested parent."), TEXT("/parent"),
						MakeShared<FJsonValueString>(ActualParent), MakeShared<FJsonValueString>(ParentRef), InstancePath);
					if (!bRemoved) Error.Append(NativeFact(TEXT("world_simple_rollback_remove_data_layer_failed"),
						TEXT("WorldDataLayers did not confirm removing the newly created instance."), TEXT("/remove_confirmed"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), InstancePath));
					return Fail(OutError, MoveTemp(Error));
				}
				OutRollback.Add({
					ERollbackKind::RemoveDataLayerInstance,
					Instance,
					WorldDataLayers,
				});
				return ApplyDataLayerStateFields(*Instance, After, OutError);
			}

			FString LevelRef;
			FString ClassRef;
			if (!ReadString(After, TEXT("level_asset_ref"), LevelRef, true, OutError)
				|| !ReadString(After, TEXT("streaming_class_ref"), ClassRef, true, OutError)) return false;
			bool bHasTransform = false;
			bool bPresent = false;
			if (!ReadBool(After, TEXT("has_transform"), bHasTransform, bPresent, OutError)) return false;
			FTransform Transform = FTransform::Identity;
			if (bPresent && bHasTransform)
			{
				Transform = ReadTransform(After, OutError);
				if ((!OutError.IsEmpty()))
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
				const auto Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("level_asset_ref"), LevelRef);
				Actual->SetStringField(TEXT("streaming_class_ref"), ClassRef);
				Actual->SetBoolField(TEXT("created"), false);
				return Fail(OutError, NativeFact(TEXT("world_simple_streaming_relation_create_failed"),
					TEXT("Editor did not add the requested streaming level to the world."), TEXT("/streaming_relation"),
					MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("created LevelStreaming relation")), World.GetPathName()));
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
			if (!ReadString(After, TEXT("membership_kind"), Kind, true, OutError)
				|| !ReadString(After, TEXT("subject_ref"), SubjectRef, true, OutError)
				|| !ReadString(After, TEXT("mode"), Mode, true, OutError)) return false;
			ON_SCOPE_EXIT { SupplementNative(OutError, SubjectRef); };
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
				if (!ReadString(After, TEXT("member_ref"), MemberRef, false, OutError)) return false;
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
			if (!ReadString(After, TEXT("relation_kind"), RelationKind, true, OutError)) return false;
			const TArray<FString> MemberRefs =
				ReadStringArray(
					After,
					TEXT("member_refs"),
					OutError);
			if (!OutError.IsEmpty()) return false;
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
					return Fail(OutError, NativeFact(TEXT("world_simple_data_layer_parent_failed"),
						TEXT("Data-layer instance rejected the requested parent."), TEXT("/parent"),
						MakeShared<FJsonValueString>(Subject->GetParent() ? Subject->GetParent()->GetPathName() : TEXT("<none>")),
						MakeShared<FJsonValueString>(Parent ? Parent->GetPathName() : TEXT("<none>")), Subject->GetPathName()));
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
			if (!DataLayerManager)
				return Fail(OutError, NativeFact(TEXT("world_simple_data_layer_manager_missing"),
					TEXT("Actor has no data-layer manager."), TEXT("/data_layer_manager"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Actor->GetPathName()));
			if (!ActorWorldDataLayers)
				return Fail(OutError, NativeFact(TEXT("world_simple_data_layer_manager_missing"),
					TEXT("Actor level has no WorldDataLayers object."), TEXT("/world_data_layers"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Actor->GetPathName()));
			Rollback.DurableDependencies.Add(
				ActorWorldDataLayers);
			for (int32 Index = 0; Index < Members.Num(); ++Index)
			{
				const UDataLayerInstance* Member = Members[Index];
				const UDataLayerAsset* Asset = Member ? Member->GetAsset() : nullptr;
				if (!Member)
					return Fail(OutError, NativeFact(TEXT("world_simple_data_layer_scope_mismatch"),
						TEXT("Membership references a missing data-layer instance."), TEXT("/after/member_refs/") + FString::FromInt(Index),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), MemberRefs[Index]));
				const AWorldDataLayers* Outer = Member->GetOuterWorldDataLayers();
				if (Outer != ActorWorldDataLayers)
					return Fail(OutError, NativeFact(TEXT("world_simple_data_layer_scope_mismatch"),
						TEXT("Data-layer instance belongs to a different WorldDataLayers scope."), TEXT("/outer_world_data_layers"),
						MakeShared<FJsonValueString>(Outer ? Outer->GetPathName() : TEXT("<missing>")),
						MakeShared<FJsonValueString>(ActorWorldDataLayers->GetPathName()), Member->GetPathName()));
				if (!Asset)
					return Fail(OutError, NativeFact(TEXT("world_simple_data_layer_scope_mismatch"),
						TEXT("Actor membership requires a data-layer instance with an asset."), TEXT("/asset"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Member->GetPathName()));
				const UDataLayerInstance* Managed = DataLayerManager->GetDataLayerInstance(Asset);
				if (Managed != Member)
					return Fail(OutError, NativeFact(TEXT("world_simple_data_layer_scope_mismatch"),
						TEXT("Actor data-layer manager resolves the asset to a different instance."), TEXT("/data_layer_manager/instance"),
						MakeShared<FJsonValueString>(Managed ? Managed->GetPathName() : TEXT("<missing>")),
						MakeShared<FJsonValueString>(Member->GetPathName()), Actor->GetPathName()));
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
			if (!ReadString(After, TEXT("state_kind"), Kind, true, OutError)) return false;
			if (Kind == TEXT("world_partition_streaming"))
			{
				UWorldPartition* WorldPartition =
					World.GetWorldPartition();
				bool bEnabled = false;
				bool bPresent = false;
				if (!ReadBool(After, TEXT("enabled"), bEnabled, bPresent, OutError)) return false;
				if (!bPresent) return Fail(OutError, RequiredField(TEXT("enabled")));
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
				if (!ReadString(
						After,
						TEXT("game_mode_class_ref"),
						GameModeClassRef,
						true,
						OutError))
				{
					return false;
				}
				AWorldSettings* WorldSettings = World.GetWorldSettings();
				UClass* GameModeClass = CastChecked<UClass>(
					FindResolved(Resolved, GameModeClassRef));
				if (!WorldSettings)
				{
					return Fail(OutError, NativeFact(TEXT("world_simple_world_settings_missing"),
						TEXT("World has no WorldSettings object to update its game mode."), TEXT("/world_settings"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), World.GetPathName()));
				}
				FRollbackAction Rollback;
				Rollback.Kind = ERollbackKind::RestoreLevelGameMode;
				Rollback.Primary = WorldSettings;
				Rollback.Secondary = WorldSettings->DefaultGameMode.Get();
				Rollback.bValueB = World.GetOutermost()->IsDirty();
				WorldSettings->DefaultGameMode = GameModeClass;
				WorldSettings->MarkPackageDirty();
				OutRollback.Add(Rollback);
				return true;
			}

			FString TargetRef;
			if (!ReadString(After, TEXT("target_ref"), TargetRef, true, OutError)) return false;
			ON_SCOPE_EXIT { SupplementNative(OutError, TargetRef); };
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
				OutRollback.Add(MoveTemp(Rollback));
				return ApplyDataLayerStateFields(*Instance, After, OutError);
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
				if (!ReadString(After, TEXT("runtime_grid_intent"), GridIntent, false, OutError)
					|| !ReadString(After, TEXT("runtime_grid_value"), GridValue, false, OutError)) return false;
				bool bSpatial = false;
				bool bSpatialPresent = false;
				if (!ReadBool(After, TEXT("is_spatially_loaded"), bSpatial, bSpatialPresent, OutError)) return false;
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
			OutRollback.Add(MoveTemp(Rollback));
			bool Value = false, bPresent = false;
			if (!ReadBool(After, TEXT("loaded"), Value, bPresent, OutError)) return false;
			if (bPresent) Relation->SetShouldBeLoaded(Value);
			if (!ReadBool(After, TEXT("visible"), Value, bPresent, OutError)) return false;
			if (bPresent) Relation->SetShouldBeVisible(Value);
			bool bHasTransform = false;
			if (!ReadBool(After, TEXT("has_transform"), bHasTransform, bPresent, OutError)) return false;
			if (bPresent && bHasTransform)
			{
				const FTransform Transform = ReadTransform(After, OutError);
				if (!OutError.IsEmpty()) return false;
				Relation->LevelTransform = Transform;
			}
			Relation->Modify();
			return true;
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
			return Fail(OutError, NativeFact(TEXT("world_simple_native_target_kind_unknown"),
				TEXT("Planned change has no native world-simple dispatcher."), TEXT("/target_kind"),
				MakeShared<FJsonValueString>(TargetKind),
				MakeShared<FJsonValueString>(TEXT("world.entity.create, world.membership.update or world.state.update")), World.GetPathName()));
		}

		bool RollbackAction(
			UWorld& World,
			const FRollbackAction& Action,
			FExecutionMutationError& OutError)
		{
			switch (Action.Kind)
			{
			case ERollbackKind::DestroyActor:
				if (AActor* Actor =
					Cast<AActor>(Action.Primary.Get()))
				{
					const FString ActorPath = Actor->GetPathName();
					const bool bDestroyed = World.DestroyActor(Actor);
					if (!bDestroyed)
					{
						return Fail(OutError, NativeFact(TEXT("world_simple_rollback_destroy_failed"),
							TEXT("Editor did not confirm destroying the created Actor during rollback."), TEXT("/destroy_confirmed"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), ActorPath));
					}
					return true;
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
						const FString InstancePath = Instance->GetPathName();
						return WorldDataLayers->RemoveDataLayer(Instance) || Fail(OutError, NativeFact(TEXT("world_simple_rollback_remove_data_layer_failed"),
							TEXT("WorldDataLayers did not confirm removing the created instance during rollback."), TEXT("/remove_confirmed"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), InstancePath));
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
					UDataLayerInstance* Parent = Cast<UDataLayerInstance>(Action.Secondary.Get());
					if (Instance->SetParent(Parent)) return true;
					return Fail(OutError, NativeFact(TEXT("world_simple_rollback_restore_parent_failed"),
						TEXT("Data-layer instance rejected its saved parent during rollback."), TEXT("/parent"),
						MakeShared<FJsonValueString>(Instance->GetParent() ? Instance->GetParent()->GetPathName() : TEXT("<none>")),
						MakeShared<FJsonValueString>(Parent ? Parent->GetPathName() : TEXT("<none>")), Instance->GetPathName()));
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
						const FString LevelPath = Level->GetPathName();
						return UEditorLevelUtils::RemoveLevelFromWorld(Level, true, false)
							|| Fail(OutError, NativeFact(TEXT("world_simple_rollback_remove_streaming_level_failed"),
								TEXT("Editor did not confirm removing the created streaming level during rollback."), TEXT("/remove_confirmed"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), LevelPath));
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
					World.GetOutermost()->SetDirtyFlag(Action.bValueB);
				}
				return true;
			default:
				return Fail(OutError, NativeFact(TEXT("world_simple_rollback_kind_unknown"),
					TEXT("Rollback action kind is unsupported."), TEXT("/kind"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Action.Kind)), MakeShared<FJsonValueString>(TEXT("registered world-simple rollback action")), World.GetPathName()));
			}
		}

		bool RollbackAll(UWorld& World, const TArray<FRollbackAction>& Actions, FExecutionMutationError& OutError)
		{
			OutError.Reset();
			bool bConfirmed = true;
			for (int32 Index = Actions.Num() - 1; Index >= 0; --Index)
			{
				FExecutionMutationError ActionError;
				if (!RollbackAction(World, Actions[Index], ActionError))
				{
					ActionError.PropertyPath = TEXT("/rollback/actions/") + FString::FromInt(Index) + ActionError.PropertyPath;
					OutError.Append(ActionError);
					bConfirmed = false;
				}
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
				OutResult = FUeAgentGenericEffectApplyResult::NoMutation();
				OutError = FExecutionMutationError();
				if (!IsInGameThread())
				{
					return Fail(OutError, NativeFact(TEXT("world_simple_game_thread_required"),
						TEXT("World-simple mutations must execute on the game thread."), TEXT("/game_thread"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
				}
				ON_SCOPE_EXIT { SupplementNative(OutError, World.GetPathName()); };
				if (!Request.IsValid()) return Fail(OutError, Request.GetValidationDiagnostic());
				if (!Lease.IsValid()) return Fail(OutError, Lease.GetValidationDiagnostic());
				if (Request.GetExecutionContext().GetWorldIdentity() != World.GetPathName())
				{
					auto Error = NativeFact(TEXT("world_simple_exact_world_context_mismatch"),
						TEXT("Actual world does not match the request's bound world."), TEXT("/execution_context/world"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), World.GetPathName());
					Error.ActualEvidence = MakeShared<FJsonValueString>(World.GetPathName());
					Error.ExpectedEvidence = MakeShared<FJsonValueString>(Request.GetExecutionContext().GetWorldIdentity());
					return Fail(OutError, MoveTemp(Error));
				}
				const TArray<FPlannedChange>& Changes =
					Request.GetPlan()
						->GetChangeSet()
						.GetChanges();
				if (Changes.IsEmpty()) return Fail(OutError, NativeFact(TEXT("world_simple_change_binding_mismatch"),
					TEXT("World mutation plan contains no changes."), TEXT("/changes"),
					MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one change"))));
				if (Bindings.Num() != Changes.Num()) return Fail(OutError, NativeFact(TEXT("world_simple_change_binding_mismatch"),
					TEXT("World mutation binding count must match its planned change count."), TEXT("/bindings"),
					MakeShared<FJsonValueNumber>(Bindings.Num()), MakeShared<FJsonValueNumber>(Changes.Num())));

				TSet<FString> References;
				for (int32 ChangeIndex = 0; ChangeIndex < Changes.Num(); ++ChangeIndex)
				{
					const FPlannedChange& Change = Changes[ChangeIndex];
					if (!CollectReferences(
							Change,
							References,
							OutError))
					{
						PrefixPlannedChange(OutError, ChangeIndex);
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
				for (int32 ChangeIndex = 0; ChangeIndex < Changes.Num(); ++ChangeIndex)
				{
					const FPlannedChange& Change = Changes[ChangeIndex];
					if (!ValidateResolvedTypes(
							World,
							Change,
							Resolved,
							OutError))
					{
						PrefixPlannedChange(OutError, ChangeIndex);
						return false;
					}
				}

				TArray<FRollbackAction> Rollback;
				TArray<FString> MutatedKeys;
				TArray<FAssetDurableTarget> DurableTargets;
				for (int32 ChangeIndex = 0; ChangeIndex < Changes.Num(); ++ChangeIndex)
				{
					const FPlannedChange& Change = Changes[ChangeIndex];
					const int32 FirstRollbackAction =
						Rollback.Num();
					OutResult = FUeAgentGenericEffectApplyResult();
					if (!ApplyChange(
							World,
							Change,
							Resolved,
							Rollback,
							OutError))
					{
						PrefixPlannedChange(OutError, ChangeIndex);
						FExecutionMutationError RollbackError;
						if (!RollbackAll(World, Rollback, RollbackError)) OutError.Append(RollbackError);
						return false;
					}
					if (!AppendDurableTargets(
							Change,
							Rollback,
							FirstRollbackAction,
							DurableTargets,
							OutError))
					{
						PrefixPlannedChange(OutError, ChangeIndex);
						FExecutionMutationError RollbackError;
						if (!RollbackAll(World, Rollback, RollbackError)) OutError.Append(RollbackError);
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
					true,
					MoveTemp(DurableTargets));
				return OutResult.IsValid() || Fail(OutError, OutResult.GetValidationDiagnostic());
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
				if (!IsInGameThread()) return Fail(OutError, NativeFact(TEXT("world_simple_rollback_request_invalid"),
					TEXT("World-simple rollback must execute on the game thread."), TEXT("/game_thread"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
				ON_SCOPE_EXIT { SupplementNative(OutError, World.GetPathName()); };
				if (!Applied.IsValid()) return Fail(OutError, Applied.GetValidationDiagnostic());
				if (Request.GetExecutionContext().GetWorldIdentity() != World.GetPathName())
				{
					auto Error = NativeFact(TEXT("world_simple_rollback_request_invalid"),
						TEXT("Rollback world does not match the request's bound world."), TEXT("/execution_context/world"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), World.GetPathName());
					Error.ActualEvidence = MakeShared<FJsonValueString>(World.GetPathName());
					Error.ExpectedEvidence = MakeShared<FJsonValueString>(Request.GetExecutionContext().GetWorldIdentity());
					return Fail(OutError, MoveTemp(Error));
				}
				TArray<FRollbackAction> Actions;
				{
					FScopeLock Lock(&StateMutex);
					TArray<FRollbackAction>* Found =
						RollbackByToken.Find(
							Applied.GetRollbackToken());
					if (!Found)
					{
						auto Error = NativeFact(TEXT("world_simple_rollback_token_unknown"),
							TEXT("No retained rollback state matches the applied operation."), TEXT("/rollback_state"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						Error.SubjectId = Applied.GetRollbackToken();
						return Fail(OutError, MoveTemp(Error));
					}
					Actions = *Found;
				}
				bOutConfirmed = RollbackAll(World, Actions, OutError);
				if (!bOutConfirmed) return false;
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
					return Fail(OutError, NativeFact(TEXT("world_simple_barrier_request_invalid"),
						TEXT("Barrier request subject generation must be non-negative."), TEXT("/subject_generation"),
						MakeShared<FJsonValueNumber>(Request.GetSubjectGeneration()), MakeShared<FJsonValueString>(TEXT("integer >= 0"))));
				}
				OutResult = FBarrierResult(
					Request.GetSubjectGeneration(),
					Request.GetRequiredSignals(),
					FString::Printf(
						TEXT("world-simple:%lld"),
						Request.GetSubjectGeneration()),
					true);
				return OutResult.IsValid() || Fail(OutError, OutResult.GetValidationDiagnostic());
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
