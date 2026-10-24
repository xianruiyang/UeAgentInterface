#include "QueryOwners/Pcg/UeAgentPcgDomainQueryOwner.h"

#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Projection/UeAgentProjection.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Domains/PCG/UeAgentPCGDomainBackend.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Grid/PCGPartitionActor.h"
#include "PCGComponent.h"
#include "PCGSubsystem.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::QueryOwners::Pcg
{
	namespace
	{
		bool Reject(UE::UAI::Contract::FDiagnostic& Diagnostic, const TCHAR* Error, const FString& Path, const FString& Message)
		{
			Diagnostic = Contract::MakeInputDiagnostic(Error, Path, TEXT("pcg_current_state"), Message);
			return false;
		}

		TSharedPtr<FJsonObject> Vector(const FVector& Value)
		{
			auto Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("x"), Value.X);
			Result->SetNumberField(TEXT("y"), Value.Y);
			Result->SetNumberField(TEXT("z"), Value.Z);
			return Result;
		}

		bool Matches(const UObject& Object, const FString& Selector)
		{
			return Selector.IsEmpty() || Selector == Object.GetName() || Selector == Object.GetPathName();
		}

		void Collect(UObject& Target, TArray<UPCGComponent*>& Components)
		{
			if (auto* Component = Cast<UPCGComponent>(&Target))
			{
				Components.Add(Component);
				return;
			}
			auto AddActor = [&Components](AActor* Actor)
			{
				if (!IsValid(Actor)) return;
				TInlineComponentArray<UPCGComponent*> Owned;
				Actor->GetComponents(Owned);
				Components.Append(Owned);
			};
			if (auto* Actor = Cast<AActor>(&Target)) AddActor(Actor);
			else if (auto* Level = Cast<ULevel>(&Target))
			{
				for (AActor* LevelActor : Level->Actors) AddActor(LevelActor);
			}
			else if (auto* World = Cast<UWorld>(&Target))
			{
				for (TActorIterator<AActor> It(World); It; ++It) AddActor(*It);
			}
		}

		TArray<APCGPartitionActor*> Partitions(UPCGComponent& Component)
		{
			TArray<APCGPartitionActor*> Result;
			UPCGComponent* Original = Component.GetOriginalComponent();
			if (!Original) Original = &Component;
			UPCGSubsystem* Subsystem = UPCGSubsystem::GetInstance(Component.GetWorld());
			if (Original->IsPartitioned() && Subsystem)
			{
				for (APCGPartitionActor* Actor : Subsystem->GetPCGComponentPartitionActorMappings(Original))
					if (IsValid(Actor)) Result.Add(Actor);
			}
			Result.Sort([](const APCGPartitionActor& A, const APCGPartitionActor& B) { return A.GetPathName() < B.GetPathName(); });
			return Result;
		}
	}

	bool IsCurrentSpatialQuery(const FString& CommandId)
	{
		return CommandId == TEXT("pcg.generated_spatial_data.read") || CommandId == TEXT("pcg.partition.spatial_state.read");
	}

	bool TryReadCurrentSpatialState(
		const FString& CommandId, const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutPayload, UE::UAI::Contract::FDiagnostic& OutDiagnostic)
	{
		OutPayload.Reset();
		OutDiagnostic.Reset();
		FString Target;
		const TSharedPtr<FJsonObject>* Input = nullptr;
		const TSharedPtr<FJsonObject>* Context = nullptr;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("target"), Target)
			|| !Params->TryGetObjectField(TEXT("input"), Input) || !Params->TryGetObjectField(TEXT("context"), Context))
			return Reject(OutDiagnostic, TEXT("pcg_spatial_input_invalid"), TEXT("/"), TEXT("target, input and context are required; empty input/context objects are valid."));
		if (!Target.Contains(TEXT("."))) Target += TEXT(".") + FPackageName::GetLongPackageAssetName(Target);
		UObject* Object = StaticFindObject(UObject::StaticClass(), nullptr, *Target);
		if (!IsValid(Object))
			return Reject(OutDiagnostic, TEXT("pcg_spatial_target_unresolved"), TEXT("/target"), TEXT("Use the exact currently loaded world, level, actor or PCG component reference returned by public readback."));
		if (!Object->IsA<UWorld>() && !Object->IsA<ULevel>() && !Object->IsA<AActor>() && !Object->IsA<UPCGComponent>())
			return Reject(OutDiagnostic, TEXT("pcg_spatial_target_type_unsupported"), TEXT("/target"), TEXT("PCG Graph and DataAsset are definitions; current generated state belongs to a world, level, actor or PCG component."));
		UWorld* World = Object->GetWorld();
		if (!World) World = Cast<UWorld>(Object);
		if (!World || Object->IsTemplate())
			return Reject(OutDiagnostic, TEXT("pcg_spatial_target_world_unavailable"), TEXT("/target"), TEXT("Current generation belongs to an actual world instance, not an asset component template."));
		ULevel* TargetLevel = Cast<ULevel>(Object);
		if (AActor* Actor = Cast<AActor>(Object)) TargetLevel = Actor->GetLevel();
		if (UPCGComponent* Component = Cast<UPCGComponent>(Object))
			TargetLevel = Component->GetOwner() ? Component->GetOwner()->GetLevel() : nullptr;
		if (!TargetLevel && World) TargetLevel = World->PersistentLevel;
		for (const TCHAR* Field : {TEXT("world"), TEXT("level")})
		{
			if (!(*Context)->HasField(Field)) continue;
			FString Reference;
			if (!(*Context)->TryGetStringField(Field, Reference))
				return Reject(OutDiagnostic, TEXT("pcg_spatial_context_invalid"), FString(TEXT("/context/")) + Field, TEXT("Expected a readable object reference."));
			UObject* Scope = FString(Field) == TEXT("world") ? static_cast<UObject*>(World) : static_cast<UObject*>(TargetLevel);
			if (!Scope || !Matches(*Scope, Reference))
				return Reject(OutDiagnostic, TEXT("pcg_spatial_context_mismatch"), FString(TEXT("/context/")) + Field, TEXT("Context must identify the target's actual loaded world/level."));
		}
		for (const auto& Field : (*Input)->Values)
		{
			if (Field.Key != TEXT("actor") && Field.Key != TEXT("component") && Field.Key != TEXT("max_points_per_data"))
				return Reject(OutDiagnostic, TEXT("pcg_spatial_input_invalid"), TEXT("/input/") + Field.Key, TEXT("Current native reads accept actor, component and max_points_per_data. Generation identities and fixture frame selectors belong to the retired query contract."));
		}
		FString ActorSelector, ComponentSelector;
		for (auto Pair : {TPair<const TCHAR*, FString*>(TEXT("actor"), &ActorSelector), TPair<const TCHAR*, FString*>(TEXT("component"), &ComponentSelector)})
		{
			if ((*Input)->HasField(Pair.Key) && !(*Input)->TryGetStringField(Pair.Key, *Pair.Value))
				return Reject(OutDiagnostic, TEXT("pcg_spatial_input_invalid"), FString(TEXT("/input/")) + Pair.Key, TEXT("Expected exact readable name or object path."));
		}
		int32 PointLimit = -1;
		if ((*Input)->HasField(TEXT("max_points_per_data")))
		{
			int64 Limit = 0;
			UE::UAI::Contract::FDiagnostic NumberError;
			if (!UeAgentPropertyValueCodec::TryReadIntegerNumber((*Input)->TryGetField(TEXT("max_points_per_data")), -1, MAX_int32, Limit, NumberError))
				{
				Reject(OutDiagnostic, TEXT("pcg_spatial_input_invalid"), TEXT("/input/max_points_per_data"), TEXT("Use -1 for all points, 0 for counts only, or a nonnegative integer for an explicit sample."));
				if (!NumberError.IsEmpty()) OutDiagnostic.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(NumberError);
				return false;
			}
			PointLimit = static_cast<int32>(Limit);
		}
		bool bDetails = false;
		const TSharedPtr<FJsonObject>* ProjectionObject = nullptr;
		if (Params->TryGetObjectField(TEXT("projection"), ProjectionObject))
		{
			for (const auto& Field : (*ProjectionObject)->Values)
			{
				const bool bProfile = Field.Key == TEXT("profile_id");
				const bool bFlag = Field.Key == TEXT("include_defaults") || Field.Key == TEXT("include_details") || Field.Key == TEXT("include_provenance");
				if ((!bProfile && !bFlag) || !Field.Value.IsValid() || Field.Value->Type != (bProfile ? EJson::String : EJson::Boolean))
					return Reject(OutDiagnostic, TEXT("pcg_spatial_projection_invalid"), TEXT("/projection/") + Field.Key, TEXT("profile_id is a string; include_defaults, include_details and include_provenance are booleans."));
			}
			FString ProfileId = TEXT("compact");
			bool bDefaults = false, bProvenance = false;
			(*ProjectionObject)->TryGetStringField(TEXT("profile_id"), ProfileId);
			(*ProjectionObject)->TryGetBoolField(TEXT("include_defaults"), bDefaults);
			(*ProjectionObject)->TryGetBoolField(TEXT("include_details"), bDetails);
			(*ProjectionObject)->TryGetBoolField(TEXT("include_provenance"), bProvenance);
			if (!Projection::FUeAgentProjectionProfile(ProfileId, bDefaults, bDetails, bProvenance).IsValid())
				return Reject(OutDiagnostic, TEXT("pcg_spatial_projection_invalid"), TEXT("/projection"), TEXT("Use the public projection profile fields."));
		}
		TArray<UPCGComponent*> Components;
		Collect(*Object, Components);
		Components.RemoveAll([](UPCGComponent* Component) { return !IsValid(Component) || !IsValid(Component->GetOwner()); });
		// An omitted selector keeps the complete collection. An explicit selector
		// must identify one native object in this target's scope, never a first match.
		for (const bool bActor : {true, false})
		{
			const FString& Selector = bActor ? ActorSelector : ComponentSelector;
			if (Selector.IsEmpty()) continue;
			TArray<UObject*> Scope;
			for (UPCGComponent* Component : Components)
				Scope.AddUnique(bActor ? static_cast<UObject*>(Component->GetOwner()) : Component);
			UObject* Selected = nullptr;
			if (!DomainAdapters::ContentDiscovery::TryResolveScopedObjectReference(Scope, Selector, Selected, OutDiagnostic))
			{
				OutDiagnostic.Source.JsonPath = bActor ? TEXT("/input/actor") : TEXT("/input/component");
				OutDiagnostic.Source.bCommandInput = true;
				return false;
			}
			Components.RemoveAll([&](UPCGComponent* Component)
			{
				return (bActor ? static_cast<UObject*>(Component->GetOwner()) : Component) != Selected;
			});
		}
		const bool bGeneratedData = CommandId == TEXT("pcg.generated_spatial_data.read");
		if (bGeneratedData)
		{
			const TArray<UPCGComponent*> Originals = Components;
			for (UPCGComponent* Component : Originals)
			{
				if (Component->GetOriginalComponent() != Component) continue;
				for (APCGPartitionActor* Partition : Partitions(*Component))
					if (UPCGComponent* Local = Partition->GetLocalComponent(Component)) Components.AddUnique(Local);
			}
		}
		Components.Sort([](const UPCGComponent& A, const UPCGComponent& B) { return A.GetPathName() < B.GetPathName(); });
		TArray<TSharedPtr<FJsonValue>> Entries;
		for (UPCGComponent* Component : Components)
		{
			auto Entry = UeAgentPCG::ComponentToJson(Component, bDetails);
			if (bGeneratedData)
			{
				Entry->SetObjectField(TEXT("output"), UeAgentPCG::PCGDataCollectionToJson(Component->GetGeneratedGraphOutput(), PointLimit));
				Entry->SetStringField(TEXT("point_transform_space"), TEXT("native_pcg_data"));
				Entry->SetStringField(TEXT("point_bounds_space"), TEXT("point_local"));
				if (UPCGComponent* Original = Component->GetOriginalComponent(); Original && Original != Component)
					Entry->SetStringField(TEXT("original_component"), Original->GetPathName());
			}
			else
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				for (APCGPartitionActor* Partition : Partitions(*Component))
				{
					auto Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("actor_path"), Partition->GetPathName());
					const FIntVector Coord = Partition->GetGridCoord();
					Item->SetObjectField(TEXT("grid_coord"), Vector(FVector(Coord)));
					Item->SetNumberField(TEXT("grid_size"), Partition->GetPCGGridSize());
					Item->SetBoolField(TEXT("use_2d_grid"), Partition->IsUsing2DGrid());
					Item->SetBoolField(TEXT("runtime_generated"), Partition->IsRuntimeGenerated());
					const FBox Bounds = Partition->GetFixedBounds();
					Item->SetObjectField(TEXT("bounds_min"), Vector(Bounds.Min));
					Item->SetObjectField(TEXT("bounds_max"), Vector(Bounds.Max));
					Item->SetStringField(TEXT("bounds_space"), TEXT("world"));
					if (UPCGComponent* Local = Partition->GetLocalComponent(Component->GetOriginalComponent() ? Component->GetOriginalComponent() : Component)) Item->SetObjectField(TEXT("local_component"), UeAgentPCG::ComponentToJson(Local, bDetails));
					Items.Add(MakeShared<FJsonValueObject>(Item));
				}
				Entry->SetArrayField(TEXT("partitions"), MoveTemp(Items));
			}
			Entries.Add(MakeShared<FJsonValueObject>(Entry));
		}
		OutPayload = MakeShared<FJsonObject>();
		OutPayload->SetStringField(TEXT("schema"), TEXT("uai.pcg.current_spatial_state/1"));
		OutPayload->SetStringField(TEXT("target"), Object->GetPathName());
		OutPayload->SetArrayField(TEXT("components"), MoveTemp(Entries));
		return true;
	}

	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}
