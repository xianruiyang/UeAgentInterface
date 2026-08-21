// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Modeling.h"
#include "UeAgentHttpServer_ModelingOps.h"

#include "Components/DynamicMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UDynamicMesh.h"

namespace UeAgentModelingSpatialInfo
{
	static constexpr double ZeroSizeTolerance = KINDA_SMALL_NUMBER;

	struct FResolvedMeshSpatialInfo
	{
		FString TargetType;
		FString BoundsSpace;
		TSharedPtr<FJsonObject> TargetJson;
		FBox BoundsBox = FBox(ForceInit);
	};

	TSharedPtr<FJsonObject> MakeAabbJson(const FBox& BoundsBox)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("min"), UeAgentModelingOps::MakeVectorJson(BoundsBox.Min));
		Obj->SetObjectField(TEXT("max"), UeAgentModelingOps::MakeVectorJson(BoundsBox.Max));
		return Obj;
	}

	static void SetRatioComponent(
		const TSharedPtr<FJsonObject>& Ratio,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const TCHAR* Axis,
		double PivotValue,
		double MinValue,
		double MaxValue)
	{
		const double Size = MaxValue - MinValue;
		if (FMath::Abs(Size) <= ZeroSizeTolerance)
		{
			Ratio->SetField(Axis, MakeShared<FJsonValueNull>());
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("code"), TEXT("zero_size_axis"));
			Issue->SetStringField(TEXT("axis"), Axis);
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
			return;
		}
		Ratio->SetNumberField(Axis, (PivotValue - MinValue) / Size);
	}

	TSharedPtr<FJsonObject> MakePivotInAabbJson(const FBox& BoundsBox, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		TSharedPtr<FJsonObject> Ratio = MakeShared<FJsonObject>();
		SetRatioComponent(Ratio, Issues, TEXT("x"), 0.0, BoundsBox.Min.X, BoundsBox.Max.X);
		SetRatioComponent(Ratio, Issues, TEXT("y"), 0.0, BoundsBox.Min.Y, BoundsBox.Max.Y);
		SetRatioComponent(Ratio, Issues, TEXT("z"), 0.0, BoundsBox.Min.Z, BoundsBox.Max.Z);

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("ratio"), Ratio);
		return Obj;
	}

	TSharedPtr<FJsonObject> MakeFrameJson(const FString& BoundsSpace)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("bounds_space"), BoundsSpace);
		Obj->SetStringField(TEXT("bounds_source"), TEXT("visual_mesh"));
		Obj->SetStringField(TEXT("unit"), TEXT("cm"));
		return Obj;
	}

	FString GetComponentTargetType(const UPrimitiveComponent* Component)
	{
		if (Component && Component->IsA<UStaticMeshComponent>())
		{
			return TEXT("static_mesh_component");
		}
		if (Component && Component->IsA<USkeletalMeshComponent>())
		{
			return TEXT("skeletal_mesh_component");
		}
		if (Component && Component->IsA<UDynamicMeshComponent>())
		{
			return TEXT("dynamic_mesh_component");
		}
		return FString();
	}

	static bool IsSupportedMeshComponent(UPrimitiveComponent* Component)
	{
		if (UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component))
		{
			return StaticMeshComponent->GetStaticMesh() != nullptr;
		}
		if (USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(Component))
		{
			return SkeletalMeshComponent->GetSkeletalMeshAsset() != nullptr;
		}
		if (UDynamicMeshComponent* DynamicMeshComponent = Cast<UDynamicMeshComponent>(Component))
		{
			return DynamicMeshComponent->GetDynamicMesh() != nullptr;
		}
		return false;
	}

	static bool ComponentMatchesName(const UPrimitiveComponent* Component, const FString& ComponentName)
	{
		if (!Component || ComponentName.IsEmpty())
		{
			return true;
		}
		return Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)
			|| Component->GetFName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase)
			|| Component->GetPathName(Component->GetOwner()).Equals(ComponentName, ESearchCase::IgnoreCase)
			|| Component->GetPathName().Equals(ComponentName, ESearchCase::IgnoreCase);
	}

	bool ResolveActorFromParams(
		UWorld* World,
		const TSharedPtr<FJsonObject>& Params,
		AActor*& OutActor,
		FString& OutError)
	{
		FString ActorKey;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("actor_id"), ActorKey);
			if (ActorKey.IsEmpty())
			{
				Params->TryGetStringField(TEXT("actor_label"), ActorKey);
			}
			if (ActorKey.IsEmpty())
			{
				Params->TryGetStringField(TEXT("actor_path"), ActorKey);
			}
			if (ActorKey.IsEmpty())
			{
				Params->TryGetStringField(TEXT("actor"), ActorKey);
			}
		}

		ActorKey = ActorKey.TrimStartAndEnd();
		if (ActorKey.IsEmpty())
		{
			OutError = TEXT("missing_asset_path_or_actor");
			return false;
		}

		OutActor = UeAgentModelingOps::FindActorByNameOrLabelLoose(World, ActorKey);
		if (!OutActor)
		{
			OutError = TEXT("actor_not_found");
			return false;
		}
		return true;
	}

	bool ResolveMeshComponent(
		AActor* Actor,
		const FString& ComponentName,
		UPrimitiveComponent*& OutComponent,
		FString& OutError)
	{
		if (!Actor)
		{
			OutError = TEXT("actor_not_found");
			return false;
		}

		TArray<UPrimitiveComponent*> Matches;
		TArray<UActorComponent*> Components;
		Actor->GetComponents(UPrimitiveComponent::StaticClass(), Components);
		for (UActorComponent* RawComponent : Components)
		{
			UPrimitiveComponent* Component = Cast<UPrimitiveComponent>(RawComponent);
			if (!IsSupportedMeshComponent(Component) || !ComponentMatchesName(Component, ComponentName))
			{
				continue;
			}
			Matches.Add(Component);
		}

		if (Matches.Num() == 0)
		{
			OutError = ComponentName.IsEmpty() ? TEXT("no_supported_mesh_component") : TEXT("mesh_component_not_found");
			return false;
		}
		if (Matches.Num() > 1)
		{
			OutError = TEXT("ambiguous_mesh_component");
			return false;
		}

		OutComponent = Matches[0];
		return true;
	}

	TSharedPtr<FJsonObject> MakeAssetTargetJson(const UObject* Asset, const FString& TargetType)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("target_type"), TargetType);
		Obj->SetStringField(TEXT("asset_path"), Asset && Asset->GetOutermost() ? Asset->GetOutermost()->GetName() : TEXT(""));
		Obj->SetStringField(TEXT("object_path"), Asset ? Asset->GetPathName() : TEXT(""));
		return Obj;
	}

	TSharedPtr<FJsonObject> MakeComponentTargetJson(const AActor* Actor, const UPrimitiveComponent* Component, const FString& TargetType)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("target_type"), TargetType);
		Obj->SetStringField(TEXT("actor_id"), Actor ? Actor->GetName() : TEXT(""));
		Obj->SetStringField(TEXT("actor_label"), Actor ? Actor->GetActorLabel() : TEXT(""));
		Obj->SetStringField(TEXT("actor_path"), Actor ? Actor->GetPathName() : TEXT(""));
		Obj->SetStringField(TEXT("component_name"), Component ? Component->GetName() : TEXT(""));
		Obj->SetStringField(TEXT("component_class"), (Component && Component->GetClass()) ? Component->GetClass()->GetPathName() : TEXT(""));
		return Obj;
	}

	static bool ResolveAssetSpatialInfo(
		const FString& AssetPath,
		const FString& RequestedBoundsSpace,
		FResolvedMeshSpatialInfo& OutInfo,
		FString& OutError)
	{
		if (!RequestedBoundsSpace.IsEmpty() && !RequestedBoundsSpace.Equals(TEXT("asset_local"), ESearchCase::IgnoreCase))
		{
			OutError = TEXT("unsupported_bounds_space");
			return false;
		}

		UObject* Asset = UeAgentModelingOps::LoadAssetObject(AssetPath);
		if (!Asset)
		{
			OutError = TEXT("mesh_asset_not_found");
			return false;
		}

		if (const UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
		{
			const FBoxSphereBounds Bounds = StaticMesh->GetBounds();
			OutInfo.TargetType = TEXT("static_mesh_asset");
			OutInfo.BoundsSpace = TEXT("asset_local");
			OutInfo.TargetJson = MakeAssetTargetJson(StaticMesh, OutInfo.TargetType);
			OutInfo.BoundsBox = FBox(Bounds.Origin - Bounds.BoxExtent, Bounds.Origin + Bounds.BoxExtent);
			return true;
		}
		if (const USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset))
		{
			const FBoxSphereBounds Bounds = SkeletalMesh->GetBounds();
			OutInfo.TargetType = TEXT("skeletal_mesh_asset");
			OutInfo.BoundsSpace = TEXT("asset_local");
			OutInfo.TargetJson = MakeAssetTargetJson(SkeletalMesh, OutInfo.TargetType);
			OutInfo.BoundsBox = FBox(Bounds.Origin - Bounds.BoxExtent, Bounds.Origin + Bounds.BoxExtent);
			return true;
		}

		OutError = TEXT("unsupported_mesh_asset");
		return false;
	}

	static bool ResolveComponentSpatialInfo(
		UWorld* World,
		const TSharedPtr<FJsonObject>& Params,
		const FString& RequestedBoundsSpace,
		FResolvedMeshSpatialInfo& OutInfo,
		FString& OutError)
	{
		if (!RequestedBoundsSpace.IsEmpty() && !RequestedBoundsSpace.Equals(TEXT("component_local"), ESearchCase::IgnoreCase))
		{
			OutError = TEXT("unsupported_bounds_space");
			return false;
		}

		AActor* Actor = nullptr;
		if (!ResolveActorFromParams(World, Params, Actor, OutError))
		{
			return false;
		}

		FString ComponentName;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("component_name"), ComponentName);
			if (ComponentName.IsEmpty())
			{
				Params->TryGetStringField(TEXT("component"), ComponentName);
			}
		}
		ComponentName = ComponentName.TrimStartAndEnd();

		UPrimitiveComponent* Component = nullptr;
		if (!ResolveMeshComponent(Actor, ComponentName, Component, OutError))
		{
			return false;
		}

		const FBoxSphereBounds Bounds = Component->CalcBounds(FTransform::Identity);
		OutInfo.TargetType = GetComponentTargetType(Component);
		OutInfo.BoundsSpace = TEXT("component_local");
		OutInfo.TargetJson = MakeComponentTargetJson(Actor, Component, OutInfo.TargetType);
		OutInfo.BoundsBox = FBox(Bounds.Origin - Bounds.BoxExtent, Bounds.Origin + Bounds.BoxExtent);
		return true;
	}
}

bool FUeAgentLegacyMechanics::CmdModelingGetMeshSpatialInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingSpatialInfo;

	OutData = MakeShared<FJsonObject>();
	FString BoundsSpace;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("bounds_space"), BoundsSpace);
	}
	BoundsSpace = BoundsSpace.TrimStartAndEnd();

	FResolvedMeshSpatialInfo Info;
	FString AssetPath;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	AssetPath = AssetPath.TrimStartAndEnd();

	if (!AssetPath.IsEmpty())
	{
		if (!ResolveAssetSpatialInfo(AssetPath, BoundsSpace, Info, OutError))
		{
			return false;
		}
	}
	else
	{
		UWorld* World = GetEditorWorld(OutError);
		if (!World)
		{
			return false;
		}
		if (!ResolveComponentSpatialInfo(World, Ctx.Params, BoundsSpace, Info, OutError))
		{
			return false;
		}
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	OutData->SetObjectField(TEXT("target"), Info.TargetJson);
	OutData->SetObjectField(TEXT("frame"), MakeFrameJson(Info.BoundsSpace));
	OutData->SetObjectField(TEXT("aabb"), MakeAabbJson(Info.BoundsBox));
	OutData->SetObjectField(TEXT("pivot_in_aabb"), MakePivotInAabbJson(Info.BoundsBox, Issues));
	if (Issues.Num() > 0)
	{
		OutData->SetArrayField(TEXT("issues"), Issues);
	}
	return true;
}
