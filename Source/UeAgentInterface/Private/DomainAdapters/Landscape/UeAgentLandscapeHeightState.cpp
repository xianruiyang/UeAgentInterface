#include "DomainAdapters/Landscape/UeAgentLandscapeHeightState.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DomainAdapters/LandscapeGeometryEffect/UeAgentLandscapeGeometryEffectCapabilities.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeDataAccess.h"
#include "LandscapeInfo.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::Landscape
{
	using LandscapeGeometryEffect::
		IUeAgentLandscapeGeometryEffectReferenceResolver;
	using ExecutionMutation::FExecutionMutationError;

	namespace
	{
		bool Fail(
			FLandscapeHeightStateError& OutError,
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

		TArray<TSharedPtr<FJsonValue>> IntegerPoint(
			const FIntPoint& Point)
		{
			return {
				MakeShared<FJsonValueNumber>(Point.X),
				MakeShared<FJsonValueNumber>(Point.Y),
			};
		}

		TArray<TSharedPtr<FJsonValue>> NumberArray(
			const TArray<double>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Values.Num());
			for (const double Value : Values)
			{
				Result.Add(MakeShared<FJsonValueNumber>(Value));
			}
			return Result;
		}

		TSharedRef<FJsonObject> RegionObject(
			const FIntPoint& MinInclusive,
			const FIntPoint& MaxInclusive)
		{
			const TSharedRef<FJsonObject> Result =
				MakeShared<FJsonObject>();
			Result->SetStringField(
				TEXT("frame_id"),
				TEXT("ue.landscape.local_sample_grid"));
			Result->SetArrayField(
				TEXT("min_inclusive"),
				IntegerPoint(MinInclusive));
			Result->SetArrayField(
				TEXT("max_inclusive"),
				IntegerPoint(MaxInclusive));
			return Result;
		}
	}

	int64 FLandscapeHeightState::GetSampleCount() const
	{
		return Heights.Num();
	}

	bool TryResolveLandscapeHeightTarget(
		UWorld& World,
		const FString& ExactTarget,
		IUeAgentLandscapeGeometryEffectReferenceResolver*
			OperationReferenceResolver,
		ALandscape*& OutLandscape,
		FLandscapeHeightStateError& OutError)
	{
		OutLandscape = nullptr;
		OutError = {};
		const FString Target = ExactTarget.TrimStartAndEnd();
		if (Target.IsEmpty() || Target != ExactTarget)
		{
			return Fail(
				OutError,
				TEXT("landscape_height_target_invalid"),
				ExactTarget);
		}

		AActor* ManagedActor = nullptr;
		FString ManagedError;
		const FString ManagedFolder =
			UeAgentLevelAuthoring::MakeManagedActorFolderPath(
				TEXT("landscape"));
		if (UeAgentLevelAuthoring::TryFindManagedActorExact(
				World,
				Target,
				ManagedFolder,
				ManagedActor,
				ManagedError))
		{
			OutLandscape = Cast<ALandscape>(ManagedActor);
			if (!OutLandscape || OutLandscape->GetWorld() != &World)
			{
				OutLandscape = nullptr;
				return Fail(
					OutError,
					TEXT("landscape_height_target_type_invalid"),
					Target,
					ManagedActor
						? ManagedActor->GetClass()->GetPathName()
						: FString());
			}
			return true;
		}
		if (!ManagedError.StartsWith(
				TEXT("level_managed_actor_not_found:"),
				ESearchCase::CaseSensitive))
		{
			return Fail(
				OutError,
				TEXT("landscape_height_managed_identity_invalid"),
				Target,
				ManagedError);
		}

		UObject* Object = nullptr;
		if (OperationReferenceResolver)
		{
			FExecutionMutationError ResolverError;
			if (OperationReferenceResolver->TryResolveReference(
					World,
					Target,
					Object,
					ResolverError))
			{
				OutLandscape = Cast<ALandscape>(Object);
				if (!OutLandscape || OutLandscape->GetWorld() != &World)
				{
					OutLandscape = nullptr;
					return Fail(
						OutError,
						TEXT("landscape_height_target_type_invalid"),
						Target,
						IsValid(Object)
							? Object->GetClass()->GetPathName()
							: FString());
				}
				return true;
			}
		}

		Object = StaticFindObject(
			UObject::StaticClass(),
			nullptr,
			*Target);
		if (!Object)
		{
			Object = StaticLoadObject(
				UObject::StaticClass(),
				nullptr,
				*Target,
				nullptr,
				LOAD_NoWarn);
		}
		if (!IsValid(Object) || Object->GetPathName() != Target)
		{
			return Fail(
				OutError,
				TEXT("landscape_height_target_not_found"),
				Target);
		}
		OutLandscape = Cast<ALandscape>(Object);
		if (!OutLandscape || OutLandscape->GetWorld() != &World)
		{
			OutLandscape = nullptr;
			return Fail(
				OutError,
				TEXT("landscape_height_target_type_invalid"),
				Target,
				Object->GetClass()->GetPathName());
		}
		return true;
	}

	bool TryReadLandscapeHeightState(
		ALandscape& Landscape,
		const FIntPoint& RegionMinInclusive,
		const FIntPoint& RegionMaxInclusive,
		const int64 MaxSamples,
		FLandscapeHeightState& OutState,
		FLandscapeHeightStateError& OutError)
	{
		OutState = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError,
				TEXT("landscape_height_thread_invalid"),
				Landscape.GetPathName());
		}
		ULandscapeInfo* Info = Landscape.GetLandscapeInfo();
		int32 MinX = 0;
		int32 MinY = 0;
		int32 MaxX = 0;
		int32 MaxY = 0;
		if (!Info
			|| !Info->GetLandscapeExtent(
				MinX,
				MinY,
				MaxX,
				MaxY))
		{
			return Fail(
				OutError,
				TEXT("landscape_height_extent_unavailable"),
				Landscape.GetPathName());
		}

		const int64 Width =
			static_cast<int64>(RegionMaxInclusive.X)
				- RegionMinInclusive.X + 1;
		const int64 Height =
			static_cast<int64>(RegionMaxInclusive.Y)
				- RegionMinInclusive.Y + 1;
		if (Width <= 0
			|| Height <= 0
			|| RegionMinInclusive.X < MinX
			|| RegionMinInclusive.Y < MinY
			|| RegionMaxInclusive.X > MaxX
			|| RegionMaxInclusive.Y > MaxY)
		{
			return Fail(
				OutError,
				TEXT("landscape_height_region_out_of_bounds"),
				Landscape.GetPathName());
		}
		if (MaxSamples <= 0
			|| Width > MAX_int32
			|| Height > MAX_int32
			|| Width > MaxSamples / Height
			|| Width * Height > MAX_int32)
		{
			return Fail(
				OutError,
				TEXT("landscape_height_sample_limit_exceeded"),
				Landscape.GetPathName(),
				FString::Printf(
					TEXT("width=%lld,height=%lld,max_samples=%lld"),
					Width,
					Height,
					MaxSamples));
		}

		const int32 SampleCount = static_cast<int32>(Width * Height);
		TArray<uint16> Encoded;
		Encoded.SetNumZeroed(SampleCount);
		TBitArray<> SamplePresent(false, SampleCount);
		TArray<ULandscapeComponent*> Components;
		for (const TPair<FIntPoint, ULandscapeComponent*>& Pair
			: Info->XYtoComponentMap)
		{
			ULandscapeComponent* Component = Pair.Value;
			if (!IsValid(Component))
			{
				continue;
			}
			const FIntPoint ComponentMin =
				Component->GetSectionBase();
			const FIntPoint ComponentMax = ComponentMin
				+ FIntPoint(
					Component->ComponentSizeQuads,
					Component->ComponentSizeQuads);
			if (ComponentMax.X < RegionMinInclusive.X
				|| ComponentMax.Y < RegionMinInclusive.Y
				|| ComponentMin.X > RegionMaxInclusive.X
				|| ComponentMin.Y > RegionMaxInclusive.Y)
			{
				continue;
			}
			Components.Add(Component);
		}
		Components.Sort(
			[](const ULandscapeComponent& Left,
				const ULandscapeComponent& Right)
			{
				const FIntPoint LeftBase = Left.GetSectionBase();
				const FIntPoint RightBase = Right.GetSectionBase();
				return LeftBase.Y != RightBase.Y
					? LeftBase.Y < RightBase.Y
					: LeftBase.X != RightBase.X
						? LeftBase.X < RightBase.X
						: Left.GetPathName() < Right.GetPathName();
			});

		for (ULandscapeComponent* Component : Components)
		{
			const FIntPoint ComponentMin =
				Component->GetSectionBase();
			const FIntPoint ComponentMax = ComponentMin
				+ FIntPoint(
					Component->ComponentSizeQuads,
					Component->ComponentSizeQuads);
			const FIntPoint ReadMin(
				FMath::Max(ComponentMin.X, RegionMinInclusive.X),
				FMath::Max(ComponentMin.Y, RegionMinInclusive.Y));
			const FIntPoint ReadMax(
				FMath::Min(ComponentMax.X, RegionMaxInclusive.X),
				FMath::Min(ComponentMax.Y, RegionMaxInclusive.Y));
			FLandscapeComponentDataInterface ComponentData(
				Component,
				0,
				true);
			if (!ComponentData.GetRawHeightData())
			{
				return Fail(
					OutError,
					TEXT("landscape_height_data_unavailable"),
					Component->GetPathName());
			}
			for (int32 Y = ReadMin.Y; Y <= ReadMax.Y; ++Y)
			{
				for (int32 X = ReadMin.X; X <= ReadMax.X; ++X)
				{
					const int32 Index =
						(Y - RegionMinInclusive.Y)
							* static_cast<int32>(Width)
						+ (X - RegionMinInclusive.X);
					if (SamplePresent[Index])
					{
						continue;
					}
					Encoded[Index] = ComponentData.GetHeight(
						X - ComponentMin.X,
						Y - ComponentMin.Y);
					SamplePresent[Index] = true;
				}
			}
		}
		if (SamplePresent.CountSetBits() != SampleCount)
		{
			return Fail(
				OutError,
				TEXT("landscape_height_region_unavailable"),
				Landscape.GetPathName(),
				FString::Printf(
					TEXT("available_samples=%d,required_samples=%d"),
					SamplePresent.CountSetBits(),
					SampleCount));
		}

		OutState.ActorPath = Landscape.GetPathName();
		OutState.ManagedId =
			UeAgentLevelAuthoring::GetManagedActorId(Landscape);
		OutState.ManagedFolder =
			UeAgentLevelAuthoring::GetActorFolderPathText(Landscape);
		OutState.bManagedTagPresent = Landscape.ActorHasTag(
			UeAgentLevelAuthoring::GetManagedActorMarkerTag());
		OutState.ExtentMinInclusive = FIntPoint(MinX, MinY);
		OutState.ExtentMaxInclusive = FIntPoint(MaxX, MaxY);
		OutState.RegionMinInclusive = RegionMinInclusive;
		OutState.RegionMaxInclusive = RegionMaxInclusive;
		OutState.Scale = Landscape.GetActorScale3D();
		OutState.Heights.Reserve(Encoded.Num());
		for (const uint16 Value : Encoded)
		{
			OutState.Heights.Add(
				LandscapeDataAccess::GetLocalHeight(Value));
		}
		return true;
	}

	TSharedRef<FJsonObject> BuildLandscapeHeightStateObject(
		const FString& ExactTarget,
		const FLandscapeHeightState& State)
	{
		const TSharedRef<FJsonObject> Result =
			MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("target_ref"), ExactTarget);
		Result->SetStringField(TEXT("actor_path"), State.ActorPath);
		Result->SetStringField(TEXT("managed_id"), State.ManagedId);
		Result->SetStringField(
			TEXT("managed_folder"),
			State.ManagedFolder);
		Result->SetBoolField(
			TEXT("managed_tag_present"),
			State.bManagedTagPresent);
		Result->SetObjectField(
			TEXT("extent"),
			RegionObject(
				State.ExtentMinInclusive,
				State.ExtentMaxInclusive));
		Result->SetObjectField(
			TEXT("region"),
			RegionObject(
				State.RegionMinInclusive,
				State.RegionMaxInclusive));
		Result->SetArrayField(
			TEXT("scale"),
			NumberArray({
				State.Scale.X,
				State.Scale.Y,
				State.Scale.Z,
			}));
		Result->SetStringField(
			TEXT("height_encoding"),
			TEXT("landscape_local_height"));
		Result->SetStringField(
			TEXT("height_order"),
			TEXT("row_major_x_fastest"));
		Result->SetNumberField(
			TEXT("sample_count"),
			State.GetSampleCount());
		Result->SetArrayField(
			TEXT("heights"),
			NumberArray(State.Heights));
		return Result;
	}
}
