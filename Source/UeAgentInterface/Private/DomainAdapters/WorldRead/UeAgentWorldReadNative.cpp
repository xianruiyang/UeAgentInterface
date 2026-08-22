#include "DomainAdapters/WorldRead/UeAgentWorldReadNative.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/Landscape/UeAgentLandscapeHeightState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/LevelStreaming.h"
#include "Engine/EngineTypes.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Landscape.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerInstanceWithAsset.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/ErrorHandling/WorldPartitionStreamingGenerationErrorHandler.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstanceViewInterface.h"

namespace UE::UAI::DomainAdapters::WorldRead
{
	using UE::UAI::ContextResource::FUeAgentOperationContext;
	using UE::UAI::Contract::FUeAgentContractValue;
	using UE::UAI::Discovery::FUeAgentCandidateBinding;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		bool Fail(
			FUeAgentWorldReadError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		FString Number(const double Value)
		{
			return FString::SanitizeFloat(Value, 0);
		}

		bool ParseInput(
			const FUeAgentContractValue& Input,
			TSharedPtr<FJsonObject>& OutObject,
			FUeAgentWorldReadError& OutError)
		{
			OutObject.Reset();
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Input.GetJsonValue());
			if (!FJsonSerializer::Deserialize(Reader, OutObject)
				|| !OutObject.IsValid())
			{
				return Fail(
					OutError,
					TEXT("world_read_input_invalid"),
					FString(),
					TEXT("canonical input is not a JSON object"));
			}
			return true;
		}

		bool ReadVector(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			FVector& OutVector,
			FUeAgentWorldReadError& OutError)
		{
			const TSharedPtr<FJsonObject>* VectorObject = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
			if (!Object.IsValid()
				|| !Object->TryGetObjectField(Field, VectorObject)
				|| !VectorObject
				|| !VectorObject->IsValid()
				|| !(*VectorObject)->TryGetArrayField(
					TEXT("components"),
					Components)
				|| !Components
				|| Components->Num() != 3)
			{
				return Fail(
					OutError,
					TEXT("world_read_spatial_input_invalid"),
					Field);
			}
			OutVector = FVector(
				(*Components)[0]->AsNumber(),
				(*Components)[1]->AsNumber(),
				(*Components)[2]->AsNumber());
			return true;
		}

		bool ReadNumberArray3(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			FVector& OutVector)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object.IsValid()
				|| !Object->TryGetArrayField(Field, Values)
				|| !Values
				|| Values->Num() != 3)
			{
				return false;
			}
			OutVector = FVector(
				(*Values)[0]->AsNumber(),
				(*Values)[1]->AsNumber(),
				(*Values)[2]->AsNumber());
			return true;
		}

		bool ReadVectorArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TArray<FVector>& OutVectors,
			FUeAgentWorldReadError& OutError)
		{
			OutVectors.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object.IsValid()
				|| !Object->TryGetArrayField(Field, Values)
				|| !Values)
			{
				return Fail(
					OutError,
					TEXT("world_read_spatial_input_invalid"),
					Field);
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				const TSharedPtr<FJsonObject> Point = Value->AsObject();
				FVector Vector;
				if (!ReadVector(Point, TEXT("point"), Vector, OutError))
				{
					// Array elements already are vector objects; wrap their
					// components without accepting a second representation.
					OutError = {};
					const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
					if (!Point.IsValid()
						|| !Point->TryGetArrayField(TEXT("components"), Components)
						|| !Components
						|| Components->Num() != 3)
					{
						return Fail(
							OutError,
							TEXT("world_read_spatial_input_invalid"),
							Field);
					}
					Vector = FVector(
						(*Components)[0]->AsNumber(),
						(*Components)[1]->AsNumber(),
						(*Components)[2]->AsNumber());
				}
				OutVectors.Add(Vector);
			}
			return true;
		}

		TArray<FString> ReadStringArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field)
		{
			TArray<FString> Result;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (Object.IsValid()
				&& Object->TryGetArrayField(Field, Values)
				&& Values)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Values)
				{
					Result.Add(Value->AsString());
				}
			}
			return Result;
		}

		ECollisionChannel ReadTraceChannel(
			const TSharedPtr<FJsonObject>& Input)
		{
			FString Channel;
			Input->TryGetStringField(TEXT("trace_channel"), Channel);
			Channel.ToLowerInline();
			if (Channel == TEXT("camera"))
			{
				return ECC_Camera;
			}
			if (Channel == TEXT("pawn"))
			{
				return ECC_Pawn;
			}
			if (Channel == TEXT("world_static"))
			{
				return ECC_WorldStatic;
			}
			if (Channel == TEXT("world_dynamic"))
			{
				return ECC_WorldDynamic;
			}
			return ECC_Visibility;
		}

		FUeAgentTemporalPoint ObservedAt(
			const FUeAgentOperationContext& Context)
		{
			return FUeAgentTemporalPoint(
				TEXT("ue.operation_context:")
					+ Context.GetExecutionContext()
						.GetContextRevision(),
				TEXT("1"));
		}

		FUeAgentStateRevision Revision(
			const FUeAgentOperationContext& Context)
		{
			return FUeAgentStateRevision(
				TEXT("world-read"),
				Context.GetExecutionContext().GetContextRevision(),
				1);
		}

		FUeAgentStateDetails Details(const FString& DisplayName)
		{
			return FUeAgentStateDetails(
				DisplayName,
				TEXT("Fact captured directly from the resolved UE world"),
				{TEXT("WorldRead")},
				false,
				TEXT("available"),
				{});
		}

		bool AddValueEntry(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			const FString& TypeId,
			EUeAgentValueKind Kind,
			const FUeAgentCanonicalValuePtr& Value,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentWorldReadError& OutError)
		{
			if (!Value.IsValid())
			{
				return Fail(
					OutError,
					TEXT("world_read_value_invalid"),
					OwnerId,
					Selector);
			}
			const FUeAgentStateRevision StateRevision =
				Revision(Context);
			const FUeAgentStateEntry Entry(
				FUeAgentCanonicalIdentity(
					SemanticId,
					TargetKind,
					OwnerId,
					Selector,
					TEXT("state"),
					StateRevision.GetGeneration()),
				FUeAgentCanonicalType(
					TypeId,
					Kind,
					false,
					Kind == EUeAgentValueKind::Object
						? TEXT("object")
						: Kind == EUeAgentValueKind::Array
							? TEXT("array")
							: TEXT("scalar")),
				Value.ToSharedRef(),
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::DomainNative,
					TEXT("ue-native"),
					TEXT("world-read"),
					Value.ToSharedRef()),
				Details(Selector),
				StateRevision);
			if (!Entry.IsValid())
			{
				return Fail(
					OutError,
					TEXT("world_read_entry_invalid"),
					OwnerId,
					Selector);
			}
			OutEntries.Add(Entry);
			return true;
		}

		bool AddString(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			const FString& Text,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentWorldReadError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryString(
					Text,
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
			}
			return AddValueEntry(
				SemanticId,
				TargetKind,
				OwnerId,
				Selector,
				TEXT("core.string"),
				EUeAgentValueKind::String,
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool AddBoolean(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			const bool bValue,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentWorldReadError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryBoolean(
					bValue,
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
			}
			return AddValueEntry(
				SemanticId,
				TargetKind,
				OwnerId,
				Selector,
				TEXT("core.boolean"),
				EUeAgentValueKind::Boolean,
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool AddInteger(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			const int64 NumberValue,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentWorldReadError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryInteger(
					LexToString(NumberValue),
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
			}
			return AddValueEntry(
				SemanticId,
				TargetKind,
				OwnerId,
				Selector,
				TEXT("core.integer"),
				EUeAgentValueKind::Integer,
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool AddDecimal(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			const double NumberValue,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentWorldReadError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryDecimal(
					Number(NumberValue),
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
			}
			return AddValueEntry(
				SemanticId,
				TargetKind,
				OwnerId,
				Selector,
				TEXT("core.decimal"),
				EUeAgentValueKind::Decimal,
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool AddSpatial(
			const FString& SemanticId,
			const FString& OwnerId,
			const FString& Selector,
			const FString& Subject,
			const FVector& Point,
			const TArray<FString>& ConversionEvidence,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentWorldReadError& OutError)
		{
			const FUeAgentTemporalPoint Time = ObservedAt(Context);
			const FUeAgentSpatialFrame Frame(
				TEXT("ue.world"),
				TEXT("ue.world"),
				3,
				{
					{TEXT("1"), TEXT("0"), TEXT("0")},
					{TEXT("0"), TEXT("1"), TEXT("0")},
					{TEXT("0"), TEXT("0"), TEXT("1")},
				},
				{TEXT("0"), TEXT("0"), TEXT("0")},
				TEXT("centimeter"),
				TEXT("UE world coordinate basis"),
				Time);
			const FUeAgentSpatialValue Spatial(
				Subject,
				EUeAgentSpatialQuantityKind::Point,
				Frame,
				{Number(Point.X), Number(Point.Y), Number(Point.Z)},
				Time,
				ConversionEvidence);
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TrySpatial(
					Spatial,
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
			}
			return AddValueEntry(
				SemanticId,
				TEXT("world_spatial"),
				OwnerId,
				Selector,
				TEXT("ue.spatial.point3"),
				EUeAgentValueKind::Spatial,
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool BuildSnapshot(
			const FString& ProviderBinding,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry> Entries,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentWorldReadError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					FString::Printf(
						TEXT("%s:%s"),
						*ProviderBinding,
						*Context.GetRequestId()),
					Context.GetExecutionContext().GetStatePlane(),
					Revision(Context),
					ObservedAt(Context),
					FUeAgentStateProvenance(
						TEXT("independent_readback"),
						ProviderBinding,
						Context.GetExecutionContext()
							.GetWorldIdentity()),
					Entries,
					Context.GetResourceTracker(),
					OutSnapshot,
					StateError))
			{
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
			}
			return true;
		}

		class FWorldReadHookBase : public IUeAgentWorldReadHook
		{
		public:
			FWorldReadHookBase(
				FString InProviderBinding,
				TSharedRef<IUeAgentWorldReadEnvironment>
					InEnvironment)
				: ProviderBinding(MoveTemp(InProviderBinding))
				, Environment(MoveTemp(InEnvironment))
			{
			}

		protected:
			bool ResolveWorld(
				const FUeAgentOperationContext& Context,
				UWorld*& OutWorld,
				FUeAgentWorldReadError& OutError) const
			{
				OutWorld = nullptr;
				if (!Environment->TryResolveWorld(
						Context.GetExecutionContext(),
						OutWorld,
						OutError))
				{
					return false;
				}
				if (!OutWorld)
				{
					return Fail(
						OutError,
						TEXT("world_read_world_unresolved"),
						Context.GetExecutionContext()
							.GetWorldIdentity());
				}
				return true;
			}

			bool ResolveTarget(
				const TOptional<FUeAgentCandidateBinding>& Target,
				UObject*& OutObject,
				FUeAgentWorldReadError& OutError) const
			{
				OutObject = nullptr;
				if (!Target.IsSet())
				{
					return Fail(
						OutError,
						TEXT("world_read_target_required"),
						ProviderBinding);
				}
				return Environment->TryResolveTarget(
					Target.GetValue(),
					OutObject,
					OutError);
			}

			const FString ProviderBinding;
			const TSharedRef<IUeAgentWorldReadEnvironment>
				Environment;
		};

		class FObjectHook final : public FWorldReadHookBase
		{
		public:
			using FWorldReadHookBase::FWorldReadHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				UWorld* World = nullptr;
				if (!ResolveWorld(Context, World, OutError))
				{
					return false;
				}
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}

				TArray<FUeAgentStateEntry> Entries;
				if (ProviderBinding == TEXT("world.read.summary"))
				{
					const FString OwnerId = World->GetPathName();
					if (!AddString(
							TEXT("world.summary:path"),
							TEXT("world"),
							OwnerId,
							TEXT("path"),
							OwnerId,
							Context,
							Entries,
							OutError)
						|| !AddBoolean(
							TEXT("world.summary:dirty"),
							TEXT("world"),
							OwnerId,
							TEXT("package_dirty"),
							World->GetOutermost()->IsDirty(),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else if (ProviderBinding
					== TEXT("world.read.selection"))
				{
					const TArray<FString>& Selection =
						Context.GetExecutionContext()
							.GetSelectionIdentities();
					for (const FString& Identity : Selection)
					{
						if (!AddString(
								TEXT("world.selection:identity"),
								TEXT("object"),
								Identity,
								TEXT("selected_identity"),
								Identity,
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
				}
				else if (ProviderBinding
					== TEXT("world.read.actors"))
				{
					const TArray<FString> ClassPaths =
						ReadStringArray(Input, TEXT("class_paths"));
					const TArray<FString> Tags =
						ReadStringArray(Input, TEXT("tags"));
					FString FolderPrefix;
					Input->TryGetStringField(
						TEXT("folder_path_prefix"),
						FolderPrefix);
					for (TActorIterator<AActor> It(World); It; ++It)
					{
						const AActor* Actor = *It;
						if (!ClassPaths.IsEmpty()
							&& !ClassPaths.Contains(
								Actor->GetClass()->GetPathName()))
						{
							continue;
						}
						bool bHasAllTags = true;
						for (const FString& Tag : Tags)
						{
							bHasAllTags &= Actor->ActorHasTag(FName(*Tag));
						}
						if (!bHasAllTags)
						{
							continue;
						}
#if WITH_EDITOR
						if (!FolderPrefix.IsEmpty()
							&& !Actor->GetFolderPath().ToString()
								.StartsWith(FolderPrefix))
						{
							continue;
						}
#endif
						const FString OwnerId = It->GetPathName();
						if (!AddString(
								TEXT("world.actor:class"),
								TEXT("actor"),
								OwnerId,
								TEXT("class_path"),
								It->GetClass()->GetPathName(),
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
				}
				else
				{
					UObject* Object = nullptr;
					if (!ResolveTarget(Target, Object, OutError))
					{
						return false;
					}
					if (ProviderBinding
						== TEXT("world.read.actor_components"))
					{
						const AActor* Actor = Cast<AActor>(Object);
						if (!Actor)
						{
							return Fail(
								OutError,
								TEXT("world_read_target_kind_mismatch"),
								Object->GetPathName(),
								TEXT("actor required"));
						}
						TInlineComponentArray<UActorComponent*>
							Components(Actor);
						for (const UActorComponent* Component
							: Components)
						{
							if (!Component)
							{
								continue;
							}
							if (!AddString(
									TEXT("world.component:class"),
									TEXT("component"),
									Component->GetPathName(),
									TEXT("class_path"),
									Component->GetClass()->GetPathName(),
									Context,
									Entries,
									OutError))
							{
								return false;
							}
						}
					}
					else
					{
						FString PropertyPath;
						if (!Input->TryGetStringField(
								TEXT("property_path"),
								PropertyPath))
						{
							return Fail(
								OutError,
								TEXT("world_read_property_path_missing"),
								Object->GetPathName());
						}
						const FProperty* Property =
							FindFProperty<FProperty>(
								Object->GetClass(),
								*PropertyPath);
						FString Text;
						if (Property)
						{
							Property->ExportText_InContainer(
								0,
								Text,
								Object,
								Object,
								Object,
								PPF_None);
						}
						else if (PropertyPath == TEXT("Bounds"))
						{
							const UPrimitiveComponent* Primitive =
								Cast<UPrimitiveComponent>(Object);
							if (!Primitive)
							{
								return Fail(
									OutError,
									TEXT("world_read_property_not_found"),
									Object->GetPathName(),
									PropertyPath);
							}
							const FBoxSphereBounds& Bounds = Primitive->Bounds;
							Text = FString::Printf(
								TEXT("origin=(%s,%s,%s);extent=(%s,%s,%s);sphere_radius=%s"),
								*Number(Bounds.Origin.X),
								*Number(Bounds.Origin.Y),
								*Number(Bounds.Origin.Z),
								*Number(Bounds.BoxExtent.X),
								*Number(Bounds.BoxExtent.Y),
								*Number(Bounds.BoxExtent.Z),
								*Number(Bounds.SphereRadius));
						}
						else
						{
							return Fail(
								OutError,
								TEXT("world_read_property_not_found"),
								Object->GetPathName(),
								PropertyPath);
						}
						if (!AddString(
								TEXT("world.property:value"),
								TEXT("object"),
								Object->GetPathName(),
								PropertyPath,
								Text,
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FContentHook final : public FWorldReadHookBase
		{
		public:
			using FWorldReadHookBase::FWorldReadHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				UWorld* World = nullptr;
				if (!ResolveWorld(Context, World, OutError))
				{
					return false;
				}
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				bool bIncludeComponents = false;
				Input->TryGetBoolField(
					TEXT("include_components"),
					bIncludeComponents);

				TArray<FUeAgentStateEntry> Entries;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					const AActor* Actor = *It;
					if (!AddString(
							TEXT("world.content:actor"),
							TEXT("actor"),
							Actor->GetPathName(),
							TEXT("class_path"),
							Actor->GetClass()->GetPathName(),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
					if (!bIncludeComponents)
					{
						continue;
					}
					TInlineComponentArray<UActorComponent*>
						Components(Actor);
					for (const UActorComponent* Component : Components)
					{
						if (Component
							&& !AddString(
								TEXT("world.content:component"),
								TEXT("component"),
								Component->GetPathName(),
								TEXT("class_path"),
								Component->GetClass()->GetPathName(),
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FSpatialHook final : public FWorldReadHookBase
		{
		public:
			using FWorldReadHookBase::FWorldReadHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				UWorld* World = nullptr;
				if (!ResolveWorld(Context, World, OutError))
				{
					return false;
				}
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}

				TArray<FUeAgentStateEntry> Entries;
				if (ProviderBinding == TEXT("world.read.ray_trace"))
				{
					FVector Start;
					FVector Direction;
					double Distance = 0.0;
					if (!ReadVector(
							Input,
							TEXT("start"),
							Start,
							OutError)
						|| !ReadVector(
							Input,
							TEXT("direction"),
							Direction,
							OutError)
						|| !Input->TryGetNumberField(
							TEXT("distance_cm"),
							Distance)
						|| Direction.IsNearlyZero()
						|| Distance <= 0.0)
					{
						return Fail(
							OutError,
							TEXT("world_read_ray_input_invalid"),
							ProviderBinding);
					}
					FHitResult Hit;
					FCollisionQueryParams Params(
						SCENE_QUERY_STAT(UAIWorldReadRay),
						false);
					bool bTraceComplex = false;
					Input->TryGetBoolField(
						TEXT("trace_complex"),
						bTraceComplex);
					Params.bTraceComplex = bTraceComplex;
					if (!World->LineTraceSingleByChannel(
							Hit,
							Start,
							Start + Direction.GetSafeNormal()
								* Distance,
							ReadTraceChannel(Input),
							Params))
					{
						return Fail(
							OutError,
							TEXT("world_read_ray_no_hit"),
							World->GetPathName());
					}
					const FString OwnerId = Hit.GetComponent()
						? Hit.GetComponent()->GetPathName()
						: World->GetPathName();
					if (!AddSpatial(
							TEXT("world.ray_trace:impact_point"),
							OwnerId,
							TEXT("impact_point"),
							OwnerId,
							Hit.ImpactPoint,
							{},
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else if (ProviderBinding == TEXT("world.read.overlap"))
				{
					FVector Center;
					if (!ReadVector(
							Input,
							TEXT("center"),
							Center,
							OutError))
					{
						return false;
					}
					FString ShapeName;
					Input->TryGetStringField(TEXT("shape"), ShapeName);
					FCollisionShape Shape;
					if (ShapeName == TEXT("box"))
					{
						FVector Extents;
						if (!ReadNumberArray3(
								Input,
								TEXT("half_extents_cm"),
								Extents))
						{
							return Fail(
								OutError,
								TEXT("world_read_overlap_shape_invalid"),
								ShapeName);
						}
						Shape = FCollisionShape::MakeBox(Extents);
					}
					else
					{
						double Radius = 0.0;
						Input->TryGetNumberField(TEXT("radius_cm"), Radius);
						if (Radius <= 0.0)
						{
							return Fail(
								OutError,
								TEXT("world_read_overlap_shape_invalid"),
								ShapeName);
						}
						if (ShapeName == TEXT("capsule"))
						{
							double HalfHeight = 0.0;
							Input->TryGetNumberField(
								TEXT("half_height_cm"),
								HalfHeight);
							Shape = FCollisionShape::MakeCapsule(
								Radius,
								HalfHeight);
						}
						else
						{
							Shape = FCollisionShape::MakeSphere(Radius);
						}
					}
					FCollisionQueryParams Params(
						SCENE_QUERY_STAT(UAIWorldReadOverlap),
						false);
					TArray<FOverlapResult> Results;
					World->OverlapMultiByChannel(
						Results,
						Center,
						FQuat::Identity,
						ReadTraceChannel(Input),
						Shape,
						Params);
					for (const FOverlapResult& Result : Results)
					{
						const UPrimitiveComponent* Component =
							Result.Component.Get();
						const FString OwnerId = Component
							? Component->GetPathName()
							: World->GetPathName();
						if (!AddString(
								TEXT("world.overlap:component"),
								TEXT("primitive_component"),
								OwnerId,
								TEXT("overlap_component"),
								OwnerId,
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
					if (!AddInteger(
							TEXT("world.overlap:count"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("overlap_count"),
							Results.Num(),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else if (ProviderBinding
					== TEXT("world.read.capsule_sweep"))
				{
					FVector Start;
					FVector End;
					double Radius = 0.0;
					double HalfHeight = 0.0;
					if (!ReadVector(Input, TEXT("start"), Start, OutError)
						|| !ReadVector(Input, TEXT("end"), End, OutError)
						|| !Input->TryGetNumberField(TEXT("radius_cm"), Radius)
						|| !Input->TryGetNumberField(TEXT("half_height_cm"), HalfHeight)
						|| Radius <= 0.0
						|| HalfHeight < Radius)
					{
						return Fail(
							OutError,
							TEXT("world_read_capsule_sweep_input_invalid"),
							ProviderBinding);
					}
					FCollisionQueryParams Params(
						SCENE_QUERY_STAT(UAIWorldReadCapsuleSweep),
						false);
					bool bTraceComplex = false;
					bool bFindInitialOverlaps = false;
					Input->TryGetBoolField(TEXT("trace_complex"), bTraceComplex);
					Input->TryGetBoolField(
						TEXT("find_initial_overlaps"),
						bFindInitialOverlaps);
					Params.bTraceComplex = bTraceComplex;
					Params.bFindInitialOverlaps = bFindInitialOverlaps;
					TArray<FHitResult> Hits;
					World->SweepMultiByChannel(
						Hits,
						Start,
						End,
						FQuat::Identity,
						ReadTraceChannel(Input),
						FCollisionShape::MakeCapsule(Radius, HalfHeight),
						Params);
					for (int32 Index = 0; Index < Hits.Num(); ++Index)
					{
						const FHitResult& Hit = Hits[Index];
						const FString OwnerId = Hit.GetComponent()
							? Hit.GetComponent()->GetPathName()
							: World->GetPathName();
						if (!AddSpatial(
								FString::Printf(TEXT("world.capsule_sweep:impact:%d"), Index),
								OwnerId,
								TEXT("impact_point"),
								OwnerId,
								Hit.ImpactPoint,
								{},
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
					if (!AddInteger(
							TEXT("world.capsule_sweep:hit_count"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("hit_count"),
							Hits.Num(),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else if (ProviderBinding
					== TEXT("world.read.capsule_path"))
				{
					TArray<FVector> Points;
					double Radius = 0.0;
					double HalfHeight = 0.0;
					double Step = 0.0;
					if (!ReadVectorArray(Input, TEXT("points"), Points, OutError)
						|| Points.Num() < 2
						|| !Input->TryGetNumberField(TEXT("radius_cm"), Radius)
						|| !Input->TryGetNumberField(TEXT("half_height_cm"), HalfHeight)
						|| !Input->TryGetNumberField(TEXT("step_cm"), Step)
						|| Radius <= 0.0
						|| HalfHeight < Radius
						|| Step <= 0.0)
					{
						return Fail(
							OutError,
							TEXT("world_read_capsule_path_input_invalid"),
							ProviderBinding);
					}
					int32 BlockedSegments = 0;
					for (int32 Segment = 0; Segment + 1 < Points.Num(); ++Segment)
					{
						const FVector Delta = Points[Segment + 1] - Points[Segment];
						const int32 Steps = FMath::Max(
							1,
							FMath::CeilToInt(Delta.Length() / Step));
						for (int32 Index = 0; Index < Steps; ++Index)
						{
							const FVector Start = Points[Segment]
								+ Delta * (static_cast<double>(Index) / Steps);
							const FVector End = Points[Segment]
								+ Delta * (static_cast<double>(Index + 1) / Steps);
							FHitResult Hit;
							FCollisionQueryParams Params(
								SCENE_QUERY_STAT(UAIWorldReadCapsulePath),
								false);
							if (World->SweepSingleByChannel(
									Hit,
									Start,
									End,
									FQuat::Identity,
									ReadTraceChannel(Input),
									FCollisionShape::MakeCapsule(Radius, HalfHeight),
									Params))
							{
								++BlockedSegments;
								break;
							}
						}
					}
					if (!AddInteger(
							TEXT("world.capsule_path:blocked_segments"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("blocked_segments"),
							BlockedSegments,
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else
				{
					FVector Center;
					if (!ReadVector(Input, TEXT("center"), Center, OutError))
					{
						return false;
					}
					FVector HalfExtents;
					double Radius = 0.0;
					const bool bHasExtents = ReadNumberArray3(
						Input,
						TEXT("half_extents_cm"),
						HalfExtents);
					Input->TryGetNumberField(TEXT("radius_cm"), Radius);
					if (!bHasExtents && Radius <= 0.0)
					{
						return Fail(
							OutError,
							TEXT("world_read_nearby_shape_extent_missing"),
							ProviderBinding);
					}
					FString FolderPrefix;
					FString IgnoreFolderPrefix;
					Input->TryGetStringField(TEXT("folder_path_prefix"), FolderPrefix);
					Input->TryGetStringField(
						TEXT("ignore_folder_path_prefix"),
						IgnoreFolderPrefix);
					int32 Count = 0;
					for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
					{
						const UPrimitiveComponent* Component = *It;
						if (!IsValid(Component)
							|| Component->GetWorld() != World)
						{
							continue;
						}
						const AActor* Actor = Component->GetOwner();
#if WITH_EDITOR
						const FString Folder = Actor
							? Actor->GetFolderPath().ToString()
							: FString();
						if ((!FolderPrefix.IsEmpty() && !Folder.StartsWith(FolderPrefix))
							|| (!IgnoreFolderPrefix.IsEmpty()
								&& Folder.StartsWith(IgnoreFolderPrefix)))
						{
							continue;
						}
#endif
						const FVector Delta = Component->Bounds.Origin - Center;
						const bool bNearby = bHasExtents
							? FMath::Abs(Delta.X) <= HalfExtents.X + Component->Bounds.BoxExtent.X
								&& FMath::Abs(Delta.Y) <= HalfExtents.Y + Component->Bounds.BoxExtent.Y
								&& FMath::Abs(Delta.Z) <= HalfExtents.Z + Component->Bounds.BoxExtent.Z
							: Delta.Length() <= Radius + Component->Bounds.SphereRadius;
						if (!bNearby)
						{
							continue;
						}
						++Count;
						if (!AddSpatial(
								TEXT("world.nearby_shape:center:") + Component->GetPathName(),
								Component->GetPathName(),
								TEXT("bounds_center"),
								Component->GetPathName(),
								Component->Bounds.Origin,
								{},
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
					if (!AddInteger(
							TEXT("world.nearby_shapes:count"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("nearby_shape_count"),
							Count,
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FMeshHook final : public FWorldReadHookBase
		{
		public:
			using FWorldReadHookBase::FWorldReadHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				UWorld* World = nullptr;
				if (!ResolveWorld(Context, World, OutError))
				{
					return false;
				}
				UObject* Object = nullptr;
				if (!ResolveTarget(Target, Object, OutError))
				{
					return false;
				}
				const UStaticMeshComponent* Component =
					Cast<UStaticMeshComponent>(Object);
				const UStaticMesh* Mesh =
					Component ? Component->GetStaticMesh() : nullptr;
				const FMeshDescription* Description =
					Mesh ? Mesh->GetMeshDescription(0) : nullptr;
				if (!Component || !Description)
				{
					return Fail(
						OutError,
						TEXT("world_read_mesh_unavailable"),
						Object ? Object->GetPathName() : FString());
				}

				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				const FStaticMeshConstAttributes Attributes(
					*Description);
				const TVertexAttributesConstRef<FVector3f> Positions =
					Attributes.GetVertexPositions();
				TArray<FVertexID> VertexIds;
				for (const FVertexID VertexId
					: Description->Vertices().GetElementIDs())
				{
					VertexIds.Add(VertexId);
				}
				VertexIds.Sort(
					[](const FVertexID Left, const FVertexID Right)
					{
						return Left.GetValue() < Right.GetValue();
					});
				if (VertexIds.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("world_read_mesh_has_no_vertices"),
						Component->GetPathName());
				}

				int32 ChosenIndex = 0;
				if (ProviderBinding
					== TEXT("world.read.mesh_vertex_position"))
				{
					double RequestedIndex = 0.0;
					if (!Input->TryGetNumberField(
							TEXT("vertex_index"),
							RequestedIndex)
						|| RequestedIndex < 0.0
						|| RequestedIndex
							>= static_cast<double>(VertexIds.Num()))
					{
						return Fail(
							OutError,
							TEXT("world_read_mesh_vertex_out_of_range"),
							Component->GetPathName());
					}
					ChosenIndex = static_cast<int32>(RequestedIndex);
				}
				else
				{
					FVector WorldPoint;
					if (!ReadVector(
							Input,
							TEXT("world_point"),
							WorldPoint,
							OutError))
					{
						return false;
					}
					double BestDistanceSquared =
						TNumericLimits<double>::Max();
					for (int32 Index = 0;
						Index < VertexIds.Num();
						++Index)
					{
						const FVector Local(
							Positions[VertexIds[Index]]);
						const double DistanceSquared =
							FVector::DistSquared(
								Component->GetComponentTransform()
									.TransformPosition(Local),
								WorldPoint);
						if (DistanceSquared < BestDistanceSquared)
						{
							BestDistanceSquared = DistanceSquared;
							ChosenIndex = Index;
						}
					}
				}

				const FVector Local(Positions[VertexIds[ChosenIndex]]);
				const FVector WorldPosition =
					Component->GetComponentTransform()
						.TransformPosition(Local);
				TArray<FUeAgentStateEntry> Entries;
				if (!AddSpatial(
						TEXT("world.mesh:vertex_position"),
						Component->GetPathName(),
						TEXT("vertex_position"),
						Component->GetPathName(),
						WorldPosition,
						{
							TEXT("mesh.local"),
							TEXT("component.transform"),
							TEXT("ue.world"),
						},
						Context,
						Entries,
						OutError)
					|| !AddInteger(
						TEXT("world.mesh:vertex_index"),
						TEXT("static_mesh_component"),
						Component->GetPathName(),
						TEXT("vertex_index"),
						ChosenIndex,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FNavigationHook final : public FWorldReadHookBase
		{
		public:
			using FWorldReadHookBase::FWorldReadHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				UWorld* World = nullptr;
				if (!ResolveWorld(Context, World, OutError))
				{
					return false;
				}
				UNavigationSystemV1* Navigation =
					FNavigationSystem::GetCurrent<UNavigationSystemV1>(
						World);
				if (!Navigation
					|| !Navigation->GetDefaultNavDataInstance(
						FNavigationSystem::DontCreate))
				{
					return Fail(
						OutError,
						TEXT("world_read_navigation_unavailable"),
						World->GetPathName());
				}

				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				FVector QueryExtent = INVALID_NAVEXTENT;
				const bool bHasQueryExtent = ReadNumberArray3(
					Input, TEXT("query_extent_cm"), QueryExtent);
				auto TryProjectPoint = [&](const FVector& Point,
					FVector& OutPoint)
				{
					FNavLocation Projected;
					const bool bProjected = Navigation->ProjectPointToNavigation(
						Point,
						Projected,
						bHasQueryExtent ? QueryExtent : INVALID_NAVEXTENT);
					OutPoint = bProjected ? Projected.Location : Point;
					return bProjected;
				};
				TArray<FUeAgentStateEntry> Entries;
				if (ProviderBinding
					== TEXT("world.read.nav_project_point"))
				{
					FVector Point;
					if (!ReadVector(Input, TEXT("point"), Point, OutError))
					{
						return false;
					}
					FVector Projected;
					if (!TryProjectPoint(Point, Projected))
					{
						return Fail(
							OutError,
							TEXT("world_read_navigation_projection_failed"),
							World->GetPathName());
					}
					if (!AddSpatial(
							TEXT("world.navigation:projected_point"),
							World->GetPathName(),
							TEXT("projected_point"),
							ProviderBinding,
							Projected,
							{},
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else
				{
					TArray<FVector> Points;
					TArray<TPair<int32, int32>> Pairs;
					const bool bFindPath = ProviderBinding
						== TEXT("world.read.nav_find_path");
					if (bFindPath)
					{
						FVector Start;
						FVector End;
						if (!ReadVector(Input, TEXT("start"), Start, OutError)
							|| !ReadVector(Input, TEXT("end"), End, OutError))
						{
							return false;
						}
						Points = {Start, End};
						Pairs.Add({0, 1});
					}
					else
					{
						if (!ReadVectorArray(Input, TEXT("points"), Points, OutError)
							|| Points.Num() < 2)
						{
							return Fail(
								OutError,
								TEXT("world_read_navigation_points_invalid"),
								ProviderBinding);
						}
						const TArray<TSharedPtr<FJsonValue>>* PairValues = nullptr;
						if (Input->TryGetArrayField(TEXT("pairs"), PairValues)
							&& PairValues)
						{
							for (const TSharedPtr<FJsonValue>& PairValue : *PairValues)
							{
								const TArray<TSharedPtr<FJsonValue>>* Indices = nullptr;
								if (!PairValue->TryGetArray(Indices)
									|| !Indices
									|| Indices->Num() != 2)
								{
									return Fail(
										OutError,
										TEXT("world_read_navigation_pairs_invalid"),
										ProviderBinding);
								}
								Pairs.Add({
									static_cast<int32>((*Indices)[0]->AsNumber()),
									static_cast<int32>((*Indices)[1]->AsNumber()),
								});
							}
						}
						else
						{
							for (int32 Index = 0; Index + 1 < Points.Num(); ++Index)
							{
								Pairs.Add({Index, Index + 1});
							}
						}
					}
					bool bProjectToNav = false;
					Input->TryGetBoolField(
						TEXT("project_to_nav"), bProjectToNav);
					bool bAllowProjectionFailure = false;
					Input->TryGetBoolField(
						TEXT("allow_projection_failure"),
						bAllowProjectionFailure);
					if (!bProjectToNav
						&& (bHasQueryExtent || bAllowProjectionFailure))
					{
						return Fail(
							OutError,
							TEXT("world_read_navigation_projection_policy_invalid"),
							ProviderBinding,
							TEXT("projection selectors require project_to_nav=true"));
					}
					if (bProjectToNav)
					{
						for (int32 PointIndex = 0;
							PointIndex < Points.Num(); ++PointIndex)
						{
							FVector ProjectedPoint;
							const bool bProjected = TryProjectPoint(
								Points[PointIndex], ProjectedPoint);
							if (!bProjected && !bAllowProjectionFailure)
							{
								return Fail(
									OutError,
									TEXT("world_read_navigation_projection_failed"),
									FString::Printf(
										TEXT("%s:point_%d"),
										*World->GetPathName(), PointIndex));
							}
							Points[PointIndex] = ProjectedPoint;
							if (!AddBoolean(
									FString::Printf(
										TEXT("world.navigation:projection:%d"),
										PointIndex),
									TEXT("world"),
									World->GetPathName(),
									FString::Printf(
										TEXT("point_%d_projection_succeeded"),
										PointIndex),
									bProjected,
									Context,
									Entries,
									OutError))
							{
								return false;
							}
						}
					}
					bool bAllowPartial = false;
					Input->TryGetBoolField(TEXT("allow_partial"), bAllowPartial);
					bool bIncludePathPoints = bFindPath;
					if (!bFindPath)
					{
						Input->TryGetBoolField(
							TEXT("include_path_points"),
							bIncludePathPoints);
					}
					for (int32 PairIndex = 0; PairIndex < Pairs.Num(); ++PairIndex)
					{
						const TPair<int32, int32>& Pair = Pairs[PairIndex];
						if (!Points.IsValidIndex(Pair.Key)
							|| !Points.IsValidIndex(Pair.Value))
						{
							return Fail(
								OutError,
								TEXT("world_read_navigation_pair_out_of_range"),
								LexToString(PairIndex));
						}
						UNavigationPath* Path =
							UNavigationSystemV1::FindPathToLocationSynchronously(
								World,
								Points[Pair.Key],
								Points[Pair.Value]);
						const bool bReachable = IsValid(Path)
							&& Path->IsValid()
							&& (bAllowPartial || !Path->IsPartial());
						if (!AddBoolean(
								FString::Printf(TEXT("world.navigation:reachable:%d"), PairIndex),
								TEXT("world"),
								World->GetPathName(),
								FString::Printf(TEXT("pair_%d_reachable"), PairIndex),
								bReachable,
								Context,
								Entries,
								OutError))
						{
							return false;
						}
						if (bIncludePathPoints && IsValid(Path))
						{
							for (int32 PointIndex = 0;
								PointIndex < Path->PathPoints.Num();
								++PointIndex)
							{
								if (!AddSpatial(
										FString::Printf(
											TEXT("world.navigation:path:%d:%d"),
											PairIndex,
											PointIndex),
										World->GetPathName(),
										TEXT("path_point"),
										ProviderBinding,
										Path->PathPoints[PointIndex],
										{},
										Context,
										Entries,
										OutError))
								{
									return false;
								}
							}
						}
					}
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FTopologyHook final : public FWorldReadHookBase
		{
		public:
			using FWorldReadHookBase::FWorldReadHookBase;

			class FPartitionIssueCollector final
				: public IStreamingGenerationErrorHandler
			{
			public:
				struct FIssue final
				{
					FString Code;
					FString Subject;
					FString Detail;
				};

				void Add(FString Code, FString Subject, FString Detail)
				{
					Issues.Add({MoveTemp(Code), MoveTemp(Subject), MoveTemp(Detail)});
				}

				FString ActorName(
					const IWorldPartitionActorDescInstanceView& Actor) const
				{
					return IStreamingGenerationErrorHandler::GetFullActorName(Actor);
				}

				void OnInvalidRuntimeGrid(
					const IWorldPartitionActorDescInstanceView& Actor,
					FName Grid) override
				{
					Add(TEXT("invalid_runtime_grid"), ActorName(Actor), Grid.ToString());
				}

				void OnInvalidReference(
					const IWorldPartitionActorDescInstanceView& Actor,
					const FGuid& ReferenceGuid,
					IWorldPartitionActorDescInstanceView*) override
				{
					Add(TEXT("invalid_reference"), ActorName(Actor),
						ReferenceGuid.ToString(EGuidFormats::DigitsWithHyphens));
				}

				void OnInvalidReferenceGridPlacement(
					const IWorldPartitionActorDescInstanceView& Actor,
					const IWorldPartitionActorDescInstanceView& Reference) override
				{
					Add(TEXT("invalid_reference_grid_placement"),
						ActorName(Actor), ActorName(Reference));
				}

				void OnInvalidReferenceDataLayers(
					const IWorldPartitionActorDescInstanceView& Actor,
					const IWorldPartitionActorDescInstanceView& Reference,
					EDataLayerInvalidReason) override
				{
					Add(TEXT("invalid_reference_data_layers"),
						ActorName(Actor), ActorName(Reference));
				}

				void OnInvalidReferenceRuntimeGrid(
					const IWorldPartitionActorDescInstanceView& Actor,
					const IWorldPartitionActorDescInstanceView& Reference) override
				{
					Add(TEXT("invalid_reference_runtime_grid"),
						ActorName(Actor), ActorName(Reference));
				}

				void OnDataLayersLoadFilterMismatch(
					const IWorldPartitionActorDescInstanceView& Actor) override
				{
					Add(TEXT("data_layers_load_filter_mismatch"), ActorName(Actor), {});
				}

				void OnInvalidWorldReference(
					const IWorldPartitionActorDescInstanceView& Actor,
					EWorldReferenceInvalidReason) override
				{
					Add(TEXT("invalid_world_reference"), ActorName(Actor), {});
				}

				void OnInvalidReferenceDataLayerAsset(
					const UDataLayerInstanceWithAsset* Instance) override
				{
					Add(TEXT("invalid_reference_data_layer_asset"),
						Instance ? Instance->GetPathName() : FString(), {});
				}

				void OnDataLayerHierarchyTypeMismatch(
					const UDataLayerInstance* Instance,
					const UDataLayerInstance*,
					EDataLayerHierarchyInvalidReason) override
				{
					Add(TEXT("data_layer_hierarchy_type_mismatch"),
						Instance ? Instance->GetPathName() : FString(), {});
				}

				void OnInvalidWorldDataLayersReference(
					const AWorldDataLayers* WorldDataLayers,
					const UDataLayerInstance* Instance,
					const FText& Reason) override
				{
					Add(TEXT("invalid_world_data_layers_reference"),
						WorldDataLayers ? WorldDataLayers->GetPathName() : FString(),
						FString::Printf(TEXT("%s %s"),
							Instance ? *Instance->GetPathName() : TEXT(""),
							*Reason.ToString()));
				}

				void OnDataLayerAssetConflict(
					const UDataLayerInstanceWithAsset* Instance,
					const UDataLayerInstanceWithAsset* Conflicting) override
				{
					Add(TEXT("data_layer_asset_conflict"),
						Instance ? Instance->GetPathName() : FString(),
						Conflicting ? Conflicting->GetPathName() : FString());
				}

				void OnInvalidDataLayerAssetType(
					const UDataLayerInstanceWithAsset* Instance,
					const UDataLayerAsset* Asset) override
				{
					Add(TEXT("invalid_data_layer_asset_type"),
						Instance ? Instance->GetPathName() : FString(),
						Asset ? Asset->GetPathName() : FString());
				}

				void OnActorNeedsResave(
					const IWorldPartitionActorDescInstanceView& Actor) override
				{
					Add(TEXT("actor_needs_resave"), ActorName(Actor), {});
				}

				void OnLevelInstanceInvalidWorldAsset(
					const IWorldPartitionActorDescInstanceView& Actor,
					FName WorldAsset,
					ELevelInstanceInvalidReason) override
				{
					Add(TEXT("level_instance_invalid_world_asset"),
						ActorName(Actor), WorldAsset.ToString());
				}

				void OnInvalidActorFilterReference(
					const IWorldPartitionActorDescInstanceView& Actor,
					const IWorldPartitionActorDescInstanceView& Reference) override
				{
					Add(TEXT("invalid_actor_filter_reference"),
						ActorName(Actor), ActorName(Reference));
				}

				void OnInvalidHLODLayer(
					const IWorldPartitionActorDescInstanceView& Actor) override
				{
					Add(TEXT("invalid_hlod_layer"), ActorName(Actor), {});
				}

				TArray<FIssue> Issues;
			};

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				UWorld* World = nullptr;
				if (!ResolveWorld(Context, World, OutError))
				{
					return false;
				}
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				TArray<FUeAgentStateEntry> Entries;
				if (ProviderBinding
					== TEXT("world.read.hlod_assignments"))
				{
					FString FolderPrefix;
					FString ManagedTag;
					Input->TryGetStringField(
						TEXT("folder_path_prefix"),
						FolderPrefix);
					Input->TryGetStringField(TEXT("managed_tag"), ManagedTag);
					for (TActorIterator<AActor> It(World); It; ++It)
					{
						const AActor* Actor = *It;
#if WITH_EDITOR
						if ((!FolderPrefix.IsEmpty()
								&& !Actor->GetFolderPath().ToString().StartsWith(FolderPrefix))
							|| (!ManagedTag.IsEmpty()
								&& !Actor->ActorHasTag(FName(*ManagedTag))))
						{
							continue;
						}
						const UObject* Layer = Actor->GetHLODLayer();
						const FString LayerPath = Layer
							? Layer->GetPathName()
							: FString();
						if (!AddString(
								TEXT("world.hlod:layer:") + Actor->GetPathName(),
								TEXT("actor"),
								Actor->GetPathName(),
								TEXT("hlod_layer"),
								LayerPath,
								Context,
								Entries,
								OutError))
						{
							return false;
						}
#endif
					}
					return BuildSnapshot(
						ProviderBinding,
						Context,
						MoveTemp(Entries),
						OutSnapshot,
						OutError);
				}
				if (ProviderBinding == TEXT("world.read.partition")
					|| ProviderBinding == TEXT("world.read.partition_health"))
				{
					UWorldPartition* Partition = World->GetWorldPartition();
					const bool bPartitionEnabled = Partition != nullptr;
					if (!AddBoolean(
							TEXT("world.partition:partition_enabled"),
							TEXT("world"), World->GetPathName(),
							TEXT("partition_enabled"), bPartitionEnabled,
							Context, Entries, OutError))
					{
						return false;
					}

					if (ProviderBinding == TEXT("world.read.partition_health"))
					{
						FPartitionIssueCollector Collector;
						if (Partition)
						{
							bool bEnableStreaming =
								Partition->IsStreamingEnabledInEditor();
							Input->TryGetBoolField(
								TEXT("enable_streaming"), bEnableStreaming);
							UWorldPartition::FCheckForErrorsParams Params;
							Params.SetErrorHandler(&Collector);
							Params.SetActorDescContainerInstanceCollection(Partition);
							Params.SetEnableStreaming(bEnableStreaming);
							UWorldPartition::CheckForErrors(Params);
						}
						if (!AddString(
								TEXT("world.partition_health:health_status"),
								TEXT("world"), World->GetPathName(),
								TEXT("health_status"),
								!Partition ? TEXT("not_enabled")
									: (Collector.Issues.IsEmpty()
										? TEXT("valid") : TEXT("invalid")),
								Context, Entries, OutError)
							|| !AddInteger(
								TEXT("world.partition_health:error_count"),
								TEXT("world"), World->GetPathName(),
								TEXT("error_count"), Collector.Issues.Num(),
								Context, Entries, OutError))
						{
							return false;
						}
						for (int32 Index = 0; Index < Collector.Issues.Num(); ++Index)
						{
							const FPartitionIssueCollector::FIssue& Issue =
								Collector.Issues[Index];
							if (!AddString(
									FString::Printf(TEXT("world.partition_health:issue:%d"), Index),
									TEXT("world_partition_issue"), Issue.Subject,
									TEXT("code"), Issue.Code,
									Context, Entries, OutError)
								|| (!Issue.Detail.IsEmpty() && !AddString(
									FString::Printf(TEXT("world.partition_health:issue_detail:%d"), Index),
									TEXT("world_partition_issue"), Issue.Subject,
									TEXT("detail"), Issue.Detail,
									Context, Entries, OutError)))
							{
								return false;
							}
						}
						return BuildSnapshot(
							ProviderBinding, Context, MoveTemp(Entries),
							OutSnapshot, OutError);
					}

					const bool bStreamingEnabled = Partition
						&& Partition->IsStreamingEnabledInEditor();
					const int32 RegionCount = Partition
						? Partition->GetUserLoadedEditorRegions().Num() : 0;
					if (!AddBoolean(
							TEXT("world.partition:streaming_enabled"),
							TEXT("world"), World->GetPathName(),
							TEXT("streaming_enabled"), bStreamingEnabled,
							Context, Entries, OutError)
						|| !AddInteger(
							TEXT("world.partition:user_loaded_region_count"),
							TEXT("world"), World->GetPathName(),
							TEXT("user_loaded_region_count"), RegionCount,
							Context, Entries, OutError))
					{
						return false;
					}
					if (Partition)
					{
						if (!AddString(
								TEXT("world.partition:editor_name"),
								TEXT("world_partition"), Partition->GetPathName(),
								TEXT("editor_name"),
								Partition->GetWorldPartitionEditorName().ToString(),
								Context, Entries, OutError))
						{
							return false;
						}
						int32 RegionIndex = 0;
						for (const FBox& Region : Partition->GetUserLoadedEditorRegions())
						{
							if (!AddSpatial(
									FString::Printf(TEXT("world.partition:user_region_center:%d"), RegionIndex),
									Partition->GetPathName(), TEXT("user_region_center"),
									FString::Printf(TEXT("user-loaded-region:%d:center"), RegionIndex),
									Region.GetCenter(), {}, Context, Entries, OutError)
								|| !AddSpatial(
									FString::Printf(TEXT("world.partition:user_region_extent:%d"), RegionIndex),
									Partition->GetPathName(), TEXT("user_region_extent"),
									FString::Printf(TEXT("user-loaded-region:%d:extent"), RegionIndex),
									Region.GetExtent(), {}, Context, Entries, OutError))
							{
								return false;
							}
							++RegionIndex;
						}
					}
					return BuildSnapshot(
						ProviderBinding, Context, MoveTemp(Entries),
						OutSnapshot, OutError);
				}
				if (ProviderBinding == TEXT("world.read.data_layers"))
				{
					FString Scope = TEXT("world");
					Input->TryGetStringField(TEXT("scope"), Scope);
					if (Scope != TEXT("world") && Scope != TEXT("selection"))
					{
						return Fail(
							OutError,
							TEXT("world_read_data_layer_scope_invalid"),
							Scope);
					}
					TArray<AActor*> ActorsInScope;
					const TArray<FString>& SelectionIdentities =
						Context.GetExecutionContext().GetSelectionIdentities();
					for (TActorIterator<AActor> It(World); It; ++It)
					{
						AActor* Actor = *It;
						if (Scope == TEXT("world")
							|| SelectionIdentities.Contains(Actor->GetPathName()))
						{
							ActorsInScope.Add(Actor);
						}
					}

					TArray<const UDataLayerInstance*> Layers;
					if (Scope == TEXT("selection"))
					{
						TSet<const UDataLayerInstance*> UniqueLayers;
						for (const AActor* Actor : ActorsInScope)
						{
							UniqueLayers.Append(Actor->GetDataLayerInstances());
						}
						Layers = UniqueLayers.Array();
					}
					else if (const AWorldDataLayers* WorldDataLayers =
						World->GetWorldDataLayers())
					{
						WorldDataLayers->ForEachDataLayerInstance(
							[&Layers](UDataLayerInstance* Layer)
							{
								if (Layer)
								{
									Layers.Add(Layer);
								}
								return true;
							});
					}
					Layers.Sort(
						[](const UDataLayerInstance& Left,
							const UDataLayerInstance& Right)
						{
							return Left.GetDataLayerFullName()
								< Right.GetDataLayerFullName();
						});
					if (!AddInteger(
							TEXT("world.data_layers:count"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("data_layer_count"),
							Layers.Num(),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
					bool bIncludeMembership = false;
					Input->TryGetBoolField(
						TEXT("include_actor_membership"),
						bIncludeMembership);
					for (const UDataLayerInstance* Layer : Layers)
					{
						if (!Layer)
						{
							continue;
						}
						const FString Owner = Layer->GetPathName();
						const UDataLayerAsset* Asset = Layer->GetAsset();
						if (!AddString(
								TEXT("world.data_layers:short_name:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_short_name"),
								Layer->GetDataLayerShortName(),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.data_layers:full_name:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_full_name"),
								Layer->GetDataLayerFullName(),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.data_layers:asset:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_asset_path"),
								Asset ? Asset->GetPathName() : FString(),
								Context, Entries, OutError)
							|| !AddBoolean(
								TEXT("world.data_layers:runtime:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_runtime"), Layer->IsRuntime(),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.data_layers:runtime_state:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_runtime_state"),
								LexToString(static_cast<int32>(Layer->GetRuntimeState())),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.data_layers:effective_runtime_state:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_effective_runtime_state"),
								LexToString(static_cast<int32>(
									Layer->GetEffectiveRuntimeState())),
								Context, Entries, OutError))
						{
							return false;
						}
#if WITH_EDITOR
						if (!AddBoolean(
								TEXT("world.data_layers:loaded:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_loaded_in_editor"),
								Layer->IsLoadedInEditor(),
								Context, Entries, OutError)
							|| !AddBoolean(
								TEXT("world.data_layers:effective_loaded:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_effective_loaded_in_editor"),
								Layer->IsEffectiveLoadedInEditor(),
								Context, Entries, OutError)
							|| !AddBoolean(
								TEXT("world.data_layers:visible:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_visible"), Layer->IsVisible(),
								Context, Entries, OutError)
							|| !AddBoolean(
								TEXT("world.data_layers:effective_visible:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_effective_visible"),
								Layer->IsEffectiveVisible(),
								Context, Entries, OutError))
						{
							return false;
						}
#endif
						if (bIncludeMembership)
						{
							int64 MembershipCount = 0;
							for (const AActor* Actor : ActorsInScope)
							{
								if (!Actor->ContainsDataLayer(Layer))
								{
									continue;
								}
								++MembershipCount;
								if (!AddString(
										TEXT("world.data_layers:actor:")
											+ Owner + TEXT(":")
											+ Actor->GetPathName(),
										TEXT("data_layer"), Owner,
										TEXT("data_layer_actor_member"),
										Actor->GetPathName(),
										Context, Entries, OutError))
								{
									return false;
								}
							}
							if (!AddInteger(
									TEXT("world.data_layers:membership_count:") + Owner,
									TEXT("data_layer"), Owner,
									TEXT("data_layer_actor_membership_count"),
									MembershipCount,
									Context, Entries, OutError))
							{
								return false;
							}
						}
					}
					return BuildSnapshot(
						ProviderBinding, Context, MoveTemp(Entries),
						OutSnapshot, OutError);
				}
				const TArray<ULevelStreaming*>& StreamingLevels =
					World->GetStreamingLevels();
				for (const ULevelStreaming* Level : StreamingLevels)
				{
					if (Level
						&& !AddString(
							TEXT("world.topology:streaming_level"),
							TEXT("streaming_level"),
							Level->GetPathName(),
							TEXT("package_name"),
							Level->GetWorldAssetPackageName(),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				if (ProviderBinding
					!= TEXT("world.read.streaming_levels"))
				{
					if (!AddInteger(
							TEXT("world.topology:streaming_level_count"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("streaming_level_count"),
							StreamingLevels.Num(),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FPhysicsHook final : public FWorldReadHookBase
		{
		public:
			using FWorldReadHookBase::FWorldReadHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				UWorld* World = nullptr;
				if (!ResolveWorld(Context, World, OutError))
				{
					return false;
				}
				UObject* Object = nullptr;
				if (!ResolveTarget(Target, Object, OutError))
				{
					return false;
				}
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				TArray<FUeAgentStateEntry> Entries;
				if (ProviderBinding == TEXT("world.read.physics_constraint"))
				{
					UPhysicsConstraintComponent* Constraint =
						Cast<UPhysicsConstraintComponent>(Object);
					if (!Constraint)
					{
						return Fail(
							OutError,
							TEXT("world_read_target_kind_mismatch"),
							Object->GetPathName(),
							ProviderBinding);
					}
					const FString Owner = Constraint->GetPathName();
					if (!AddString(
							TEXT("world.physics_constraint:path"),
							TEXT("physics_constraint_component"), Owner,
							TEXT("constraint_path"), Owner,
							Context, Entries, OutError))
					{
						return false;
					}
					bool bIncludeEndpoints = false;
					bool bIncludeLimits = false;
					bool bIncludeRuntimeState = false;
					Input->TryGetBoolField(
						TEXT("include_endpoints"), bIncludeEndpoints);
					Input->TryGetBoolField(
						TEXT("include_limits"), bIncludeLimits);
					Input->TryGetBoolField(
						TEXT("include_runtime_state"), bIncludeRuntimeState);
					if (bIncludeEndpoints)
					{
						UPrimitiveComponent* Component1 = nullptr;
						UPrimitiveComponent* Component2 = nullptr;
						FName Bone1;
						FName Bone2;
						Constraint->GetConstrainedComponents(
							Component1, Bone1, Component2, Bone2);
						if (!AddString(
								TEXT("world.physics_constraint:component_1"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("constraint_component_1"),
								Component1 ? Component1->GetPathName() : FString(),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.physics_constraint:bone_1"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("constraint_bone_1"), Bone1.ToString(),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.physics_constraint:component_2"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("constraint_component_2"),
								Component2 ? Component2->GetPathName() : FString(),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.physics_constraint:bone_2"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("constraint_bone_2"), Bone2.ToString(),
								Context, Entries, OutError))
						{
							return false;
						}
					}
					const FConstraintInstance& Instance =
						Constraint->ConstraintInstance;
					if (bIncludeLimits
						&& (!AddDecimal(
								TEXT("world.physics_constraint:linear_limit"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("linear_limit_cm"), Instance.GetLinearLimit(),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.physics_constraint:linear_x_motion"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("linear_x_motion"),
								LexToString(static_cast<int32>(Instance.GetLinearXMotion())),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.physics_constraint:linear_y_motion"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("linear_y_motion"),
								LexToString(static_cast<int32>(Instance.GetLinearYMotion())),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.physics_constraint:linear_z_motion"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("linear_z_motion"),
								LexToString(static_cast<int32>(Instance.GetLinearZMotion())),
								Context, Entries, OutError)
							|| !AddDecimal(
								TEXT("world.physics_constraint:swing_1_limit"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("angular_swing_1_limit_degrees"),
								Instance.GetAngularSwing1Limit(),
								Context, Entries, OutError)
							|| !AddDecimal(
								TEXT("world.physics_constraint:swing_2_limit"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("angular_swing_2_limit_degrees"),
								Instance.GetAngularSwing2Limit(),
								Context, Entries, OutError)
							|| !AddDecimal(
								TEXT("world.physics_constraint:twist_limit"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("angular_twist_limit_degrees"),
								Instance.GetAngularTwistLimit(),
								Context, Entries, OutError)))
					{
						return false;
					}
					if (bIncludeRuntimeState
						&& (!AddBoolean(
								TEXT("world.physics_constraint:broken"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("constraint_broken"), Constraint->IsBroken(),
								Context, Entries, OutError)
							|| !AddDecimal(
								TEXT("world.physics_constraint:current_twist"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("current_twist_degrees"),
								Constraint->GetCurrentTwist(),
								Context, Entries, OutError)
							|| !AddDecimal(
								TEXT("world.physics_constraint:current_swing_1"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("current_swing_1_degrees"),
								Constraint->GetCurrentSwing1(),
								Context, Entries, OutError)
							|| !AddDecimal(
								TEXT("world.physics_constraint:current_swing_2"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("current_swing_2_degrees"),
								Constraint->GetCurrentSwing2(),
								Context, Entries, OutError)))
					{
						return false;
					}
				}
				else
				{
					const UPrimitiveComponent* Primitive =
						Cast<UPrimitiveComponent>(Object);
					if (!Primitive)
					{
						return Fail(
							OutError,
							TEXT("world_read_target_kind_mismatch"),
							Object->GetPathName(),
							ProviderBinding);
					}
					const FString Owner = Primitive->GetPathName();
					if (!AddString(
							TEXT("world.physics:collision_enabled"),
							TEXT("primitive_component"), Owner,
							TEXT("collision_enabled"),
							LexToString(static_cast<int32>(
								Primitive->GetCollisionEnabled())),
							Context, Entries, OutError))
					{
						return false;
					}
					bool bIncludeCollisionResponses = false;
					bool bIncludeMaterial = false;
					bool bIncludeRuntimeState = false;
					Input->TryGetBoolField(
						TEXT("include_collision_responses"),
						bIncludeCollisionResponses);
					Input->TryGetBoolField(
						TEXT("include_material"), bIncludeMaterial);
					Input->TryGetBoolField(
						TEXT("include_runtime_state"), bIncludeRuntimeState);
					if (bIncludeCollisionResponses)
					{
						for (int32 Channel = 0; Channel < ECC_MAX; ++Channel)
						{
							if (!AddString(
									FString::Printf(
										TEXT("world.physics:response:%d"), Channel),
									TEXT("primitive_component"), Owner,
									FString::Printf(
										TEXT("collision_response_%d"), Channel),
									LexToString(static_cast<int32>(
										Primitive->GetCollisionResponseToChannel(
											static_cast<ECollisionChannel>(Channel)))),
									Context, Entries, OutError))
							{
								return false;
							}
						}
					}
					if (bIncludeMaterial)
					{
						const UMaterialInterface* Material =
							Primitive->GetMaterial(0);
						if (!AddString(
								TEXT("world.physics:material"),
								TEXT("primitive_component"), Owner,
								TEXT("material_path"),
								Material ? Material->GetPathName() : FString(),
								Context, Entries, OutError))
						{
							return false;
						}
					}
					if (bIncludeRuntimeState
						&& (!AddBoolean(
								TEXT("world.physics:gravity_enabled"),
								TEXT("primitive_component"), Owner,
								TEXT("gravity_enabled"),
								Primitive->IsGravityEnabled(),
								Context, Entries, OutError)
							|| !AddBoolean(
								TEXT("world.physics:simulating"),
								TEXT("primitive_component"), Owner,
								TEXT("simulating_physics"),
								Primitive->IsSimulatingPhysics(),
								Context, Entries, OutError)))
					{
						return false;
					}
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FLandscapeHeightHook final : public FWorldReadHookBase
		{
		public:
			using FWorldReadHookBase::FWorldReadHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				UWorld* World = nullptr;
				if (!ResolveWorld(Context, World, OutError))
				{
					return false;
				}
				if (!Target.IsSet())
				{
					return Fail(
						OutError,
						TEXT("landscape_height_target_required"),
						ProviderBinding);
				}

				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				const TSharedPtr<FJsonObject>* Region = nullptr;
				const TSharedPtr<FJsonObject>* SpaceContract = nullptr;
				if (!Input->TryGetObjectField(TEXT("region"), Region)
					|| !Region
					|| !Region->IsValid()
					|| !Input->TryGetObjectField(
						TEXT("space_contract"),
						SpaceContract)
					|| !SpaceContract
					|| !SpaceContract->IsValid())
				{
					return Fail(
						OutError,
						TEXT("landscape_height_space_contract_invalid"),
						Target->GetPublicRef());
				}
				FString RegionFrame;
				FString SpaceFrame;
				FString Unit;
				FString BasisEvidence;
				(*Region)->TryGetStringField(
					TEXT("frame_id"),
					RegionFrame);
				(*SpaceContract)->TryGetStringField(
					TEXT("frame_id"),
					SpaceFrame);
				(*SpaceContract)->TryGetStringField(
					TEXT("unit"),
					Unit);
				(*SpaceContract)->TryGetStringField(
					TEXT("basis_evidence"),
					BasisEvidence);
				if (RegionFrame
						!= TEXT("ue.landscape.local_sample_grid")
					|| SpaceFrame
						!= TEXT("ue.landscape.local_sample_grid")
					|| Unit != TEXT("sample")
					|| BasisEvidence.TrimStartAndEnd().IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("landscape_height_space_contract_invalid"),
						Target->GetPublicRef());
				}

				FIntPoint MinInclusive;
				FIntPoint MaxInclusive;
				if (!TryReadIntegerPoint(
						*Region,
						TEXT("min_inclusive"),
						MinInclusive)
					|| !TryReadIntegerPoint(
						*Region,
						TEXT("max_inclusive"),
						MaxInclusive))
				{
					return Fail(
						OutError,
						TEXT("landscape_height_region_invalid"),
						Target->GetPublicRef());
				}

				ALandscape* Landscape = nullptr;
				DomainAdapters::Landscape::
					FLandscapeHeightStateError StateError;
				if (!DomainAdapters::Landscape::
					TryResolveLandscapeHeightTarget(
						*World,
						Target->GetPublicRef(),
						nullptr,
						Landscape,
						StateError)
					|| !Landscape)
				{
					return Fail(
						OutError,
						StateError.Code.IsEmpty()
							? FString(TEXT("landscape_height_target_unresolved"))
							: StateError.Code,
						StateError.SubjectId,
						StateError.Detail);
				}

				DomainAdapters::Landscape::FLandscapeHeightState State;
				if (!DomainAdapters::Landscape::
					TryReadLandscapeHeightState(
						*Landscape,
						MinInclusive,
						MaxInclusive,
						16384,
						State,
						StateError))
				{
					return Fail(
						OutError,
						StateError.Code,
						StateError.SubjectId,
						StateError.Detail);
				}

				FUeAgentCanonicalValuePtr Value;
				FString CanonicalError;
				if (!CanonicalJson::TryFromJsonValue(
						MakeShared<FJsonValueObject>(
							DomainAdapters::Landscape::
								BuildLandscapeHeightStateObject(
									Target->GetPublicRef(),
									State)),
						Context.GetResourceTracker(),
						Value,
						CanonicalError)
					|| !Value.IsValid())
				{
					return Fail(
						OutError,
						TEXT("landscape_height_state_encoding_failed"),
						Target->GetPublicRef(),
						CanonicalError);
				}
				TArray<FUeAgentStateEntry> Entries;
				if (!AddValueEntry(
						TEXT("landscape.height.read:")
							+ State.ActorPath,
						TEXT("landscape_height_state"),
						State.ActorPath,
						TEXT("height_state"),
						TEXT("ue.landscape.height_state"),
						EUeAgentValueKind::Object,
						Value,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}

		private:
			static bool TryReadIntegerPoint(
				const TSharedPtr<FJsonObject>& Object,
				const TCHAR* Field,
				FIntPoint& OutPoint)
			{
				const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
				if (!Object.IsValid()
					|| !Object->TryGetArrayField(Field, Values)
					|| !Values
					|| Values->Num() != 2)
				{
					return false;
				}
				int32 Components[2] = {};
				for (int32 Index = 0; Index < 2; ++Index)
				{
					const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
					if (!Value.IsValid() || Value->Type != EJson::Number)
					{
						return false;
					}
					const double NumberValue = Value->AsNumber();
					if (!FMath::IsFinite(NumberValue)
						|| NumberValue != FMath::FloorToDouble(NumberValue)
						|| NumberValue < MIN_int32
						|| NumberValue > MAX_int32)
					{
						return false;
					}
					Components[Index] = static_cast<int32>(NumberValue);
				}
				OutPoint = FIntPoint(Components[0], Components[1]);
				return true;
			}
		};

		bool IsOneOf(
			const FString& Value,
			std::initializer_list<const TCHAR*> Candidates)
		{
			for (const TCHAR* Candidate : Candidates)
			{
				if (Value == Candidate)
				{
					return true;
				}
			}
			return false;
		}
	}

	bool FUeAgentWorldReadNativeHookFactory::TryCreate(
		const FString& ProviderBinding,
		TSharedRef<IUeAgentWorldReadEnvironment> Environment,
		TSharedPtr<IUeAgentWorldReadHook>& OutHook,
		FUeAgentWorldReadError& OutError)
	{
		OutHook.Reset();
		OutError = {};
		if (IsOneOf(
				ProviderBinding,
				{
					TEXT("world.read.summary"),
					TEXT("world.read.actors"),
					TEXT("world.read.actor_components"),
					TEXT("world.read.selection"),
					TEXT("world.read.property"),
				}))
		{
			OutHook =
				MakeShared<FObjectHook>(
					ProviderBinding,
					MoveTemp(Environment));
		}
		else if (ProviderBinding == TEXT("world.read.content"))
		{
			OutHook =
				MakeShared<FContentHook>(
					ProviderBinding,
					MoveTemp(Environment));
		}
		else if (IsOneOf(
				ProviderBinding,
				{
					TEXT("world.read.ray_trace"),
					TEXT("world.read.overlap"),
					TEXT("world.read.capsule_sweep"),
					TEXT("world.read.capsule_path"),
					TEXT("world.read.nearby_shapes"),
				}))
		{
			OutHook =
				MakeShared<FSpatialHook>(
					ProviderBinding,
					MoveTemp(Environment));
		}
		else if (IsOneOf(
				ProviderBinding,
				{
					TEXT("world.read.mesh_closest_vertex"),
					TEXT("world.read.mesh_vertex_position"),
				}))
		{
			OutHook =
				MakeShared<FMeshHook>(
					ProviderBinding,
					MoveTemp(Environment));
		}
		else if (IsOneOf(
				ProviderBinding,
				{
					TEXT("world.read.nav_project_point"),
					TEXT("world.read.nav_find_path"),
					TEXT("world.read.nav_connectivity"),
				}))
		{
			OutHook =
				MakeShared<FNavigationHook>(
					ProviderBinding,
					MoveTemp(Environment));
		}
		else if (IsOneOf(
				ProviderBinding,
				{
					TEXT("world.read.data_layers"),
					TEXT("world.read.hlod_assignments"),
					TEXT("world.read.streaming_levels"),
					TEXT("world.read.partition"),
					TEXT("world.read.partition_health"),
				}))
		{
			OutHook =
				MakeShared<FTopologyHook>(
					ProviderBinding,
					MoveTemp(Environment));
		}
		else if (IsOneOf(
				ProviderBinding,
				{
					TEXT("world.read.physics_component"),
					TEXT("world.read.physics_constraint"),
				}))
		{
			OutHook =
				MakeShared<FPhysicsHook>(
					ProviderBinding,
					MoveTemp(Environment));
		}
		else if (ProviderBinding == TEXT("landscape.read.height"))
		{
			OutHook =
				MakeShared<FLandscapeHeightHook>(
					ProviderBinding,
					MoveTemp(Environment));
		}
		else
		{
			return Fail(
				OutError,
				TEXT("world_read_provider_unknown"),
				ProviderBinding);
		}
		return true;
	}
}
