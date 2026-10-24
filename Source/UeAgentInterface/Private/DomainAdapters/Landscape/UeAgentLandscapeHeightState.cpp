#include "DomainAdapters/Landscape/UeAgentLandscapeHeightState.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

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
		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Target = TEXT(""))
		{
			OutError = MoveTemp(Diagnostic);
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target;
			return false;
		}

		FLandscapeHeightStateError HeightFailure(FString Code, FString Reason, FString Target, FString Field,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = FLandscapeHeightStateError::Failure(MoveTemp(Code), MoveTemp(Reason), TEXT("landscape_state"));
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = MoveTemp(Target);
			Error.PropertyPath = MoveTemp(Field);
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValuePresent = Error.ExpectedValue.IsValid();
			return Error;
		}

		FLandscapeHeightStateError HeightConditionFailure(FString Code, FString Reason, FString Target, FString Field)
		{
			return HeightFailure(MoveTemp(Code), MoveTemp(Reason), MoveTemp(Target), MoveTemp(Field),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		}

		FLandscapeHeightStateError LandscapeTypeFailure(UObject* Object, const UWorld& World, const FString& Target)
		{
			if (!IsValid(Object))
				return HeightConditionFailure(TEXT("landscape_height_target_type_invalid"),
					TEXT("The resolved Landscape object is missing or no longer valid."), Target, TEXT("/resolved_object_valid"));
			if (!Cast<ALandscape>(Object))
				return HeightFailure(TEXT("landscape_height_target_type_invalid"),
					TEXT("The resolved target is not a Landscape actor."), Target, TEXT("/native_class"),
					MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()), MakeShared<FJsonValueString>(ALandscape::StaticClass()->GetPathName()));
			auto Error = HeightConditionFailure(TEXT("landscape_height_target_type_invalid"),
				TEXT("The resolved Landscape actor belongs to a different world."), Target, TEXT("/world_matches_requested"));
			Error.ActualEvidence = MakeShared<FJsonValueString>(Object->GetWorld() ? Object->GetWorld()->GetPathName() : FString());
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(World.GetPathName());
			return Error;
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

		TSharedRef<FJsonObject> ScaleObject(const FVector& Scale)
		{
			const TSharedRef<FJsonObject> Result =
				MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("x"), Scale.X);
			Result->SetNumberField(TEXT("y"), Scale.Y);
			Result->SetNumberField(TEXT("z"), Scale.Z);
			return Result;
		}
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
			return Fail(OutError, HeightFailure(TEXT("landscape_height_target_invalid"),
				Target.IsEmpty() ? TEXT("The exact Landscape target is empty.") : TEXT("The exact Landscape target contains leading or trailing whitespace."),
				ExactTarget, TEXT("/target"), MakeShared<FJsonValueString>(ExactTarget),
				MakeShared<FJsonValueString>(Target.IsEmpty() ? FString(TEXT("non-empty exact target")) : Target)));
		}

		AActor* ManagedActor = nullptr;
		UE::UAI::Contract::FDiagnostic ManagedError;
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
				return Fail(OutError, LandscapeTypeFailure(ManagedActor, World, Target));
			}
			return true;
		}
		if (ManagedError.Code != TEXT("level_managed_actor_not_found"))
		{
			return Fail(OutError, ManagedError, Target);
		}

		UObject* Object = nullptr;
		FExecutionMutationError ReferenceError;
		if (OperationReferenceResolver)
		{
			if (OperationReferenceResolver->TryResolveReference(
					World,
					Target,
					Object,
					ReferenceError))
			{
				OutLandscape = Cast<ALandscape>(Object);
				if (!OutLandscape || OutLandscape->GetWorld() != &World)
				{
					OutLandscape = nullptr;
					return Fail(OutError, LandscapeTypeFailure(Object, World, Target));
				}
				return true;
			}
		}

		FString ObjectPath = Target;
		UE::UAI::ContentDiscovery::FContentDiscoveryError ResolveError;
		if (!ContentDiscovery::TryResolveEditorContentReference(ObjectPath, FString(), ResolveError))
		{
			ResolveError.Append(ReferenceError);
			return Fail(OutError, MoveTemp(ResolveError), Target);
		}
		Object = StaticFindObject(
			UObject::StaticClass(),
			nullptr,
			*ObjectPath);
		if (!Object)
		{
			Object = StaticLoadObject(
				UObject::StaticClass(),
				nullptr,
				*ObjectPath,
				nullptr,
				LOAD_NoWarn);
		}
		if (!IsValid(Object) || Object->GetPathName() != ObjectPath)
		{
			auto Error = HeightConditionFailure(TEXT("landscape_height_target_not_found"),
				!IsValid(Object) ? TEXT("No valid native object was found for the exact Landscape target.")
					: TEXT("The resolved native object does not match the exact Landscape reference."), Target,
				!IsValid(Object) ? TEXT("/resolved_object_valid") : TEXT("/resolved_object_matches_reference"));
			if (IsValid(Object)) Error.ActualEvidence = MakeShared<FJsonValueString>(Object->GetPathName());
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(ObjectPath);
			Error.Append(ReferenceError);
			return Fail(OutError, MoveTemp(Error));
		}
		OutLandscape = Cast<ALandscape>(Object);
		if (!OutLandscape || OutLandscape->GetWorld() != &World)
		{
			OutLandscape = nullptr;
			auto Error = LandscapeTypeFailure(Object, World, Target);
			Error.Append(ReferenceError);
			return Fail(OutError, MoveTemp(Error));
		}
		return true;
	}

	bool TryReadLandscapeHeightState(
		ALandscape& Landscape,
		const FIntPoint& RegionMinInclusive,
		const FIntPoint& RegionMaxInclusive,
		FLandscapeHeightState& OutState,
		FLandscapeHeightStateError& OutError)
	{
		OutState = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(OutError, HeightConditionFailure(TEXT("landscape_height_thread_invalid"),
				TEXT("Landscape height reads must run on GameThread."), Landscape.GetPathName(), TEXT("/game_thread")));
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
			return Fail(OutError, HeightConditionFailure(TEXT("landscape_height_extent_unavailable"),
				!Info ? TEXT("The Landscape has no native LandscapeInfo.") : TEXT("The native Landscape extent could not be read."),
				Landscape.GetPathName(), !Info ? TEXT("/landscape_info_present") : TEXT("/extent_available")));
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
			return Fail(OutError, HeightFailure(TEXT("landscape_height_region_out_of_bounds"),
				Width <= 0 || Height <= 0 ? TEXT("The region minimum must not exceed its inclusive maximum.")
					: TEXT("The requested height region extends outside the native Landscape extent."),
				Landscape.GetPathName(), TEXT("/region"),
				MakeShared<FJsonValueObject>(RegionObject(RegionMinInclusive, RegionMaxInclusive)),
				MakeShared<FJsonValueObject>(RegionObject(FIntPoint(MinX, MinY), FIntPoint(MaxX, MaxY)))));
		}
		if (Width > MAX_int32
			|| Height > MAX_int32
			|| Width * Height > MAX_int32)
		{
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetNumberField(TEXT("width"), Width);
			Actual->SetNumberField(TEXT("height"), Height);
			if (Width <= MAX_int32 && Height <= MAX_int32)
				Actual->SetStringField(TEXT("sample_count"), LexToString(Width * Height));
			return Fail(OutError, HeightFailure(TEXT("landscape_height_sample_limit_exceeded"),
				TEXT("The region width, height and total sample count must each fit the native 32-bit sample limit."),
				Landscape.GetPathName(), TEXT("/region/sample_limit"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueNumber>(MAX_int32)));
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
				return Fail(OutError, HeightConditionFailure(TEXT("landscape_height_data_unavailable"),
					TEXT("The native Landscape component has no readable raw height data."), Component->GetPathName(), TEXT("/raw_height_data_present")));
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
		const int32 AvailableSamples = SamplePresent.CountSetBits();
		if (AvailableSamples != SampleCount)
		{
			return Fail(OutError, HeightFailure(TEXT("landscape_height_region_unavailable"),
				TEXT("The Landscape components did not provide every height sample in the requested region."),
				Landscape.GetPathName(), TEXT("/region/available_samples"), MakeShared<FJsonValueNumber>(AvailableSamples), MakeShared<FJsonValueNumber>(SampleCount)));
		}

		OutState.ActorPath = Landscape.GetPathName();
		OutState.ExtentMinInclusive = FIntPoint(MinX, MinY);
		OutState.ExtentMaxInclusive = FIntPoint(MaxX, MaxY);
		OutState.Scale = Landscape.GetActorScale3D();
		OutState.Heights.Reserve(Encoded.Num());
		for (const uint16 Value : Encoded)
		{
			OutState.Heights.Add(
				LandscapeDataAccess::GetLocalHeight(Value));
		}
		return true;
	}

	TSharedRef<FJsonObject> BuildLandscapeHeightQueryObject(
		const FLandscapeHeightState& State)
	{
		const TSharedRef<FJsonObject> Result =
			MakeShared<FJsonObject>();
		Result->SetObjectField(
			TEXT("extent"),
			RegionObject(
				State.ExtentMinInclusive,
				State.ExtentMaxInclusive));
		Result->SetObjectField(
			TEXT("scale_cm_per_local_unit"),
			ScaleObject(State.Scale));
		Result->SetStringField(
			TEXT("height_encoding"),
			TEXT("landscape_local_height"));
		Result->SetStringField(
			TEXT("height_order"),
			TEXT("row_major_x_fastest"));
		Result->SetArrayField(
			TEXT("heights"),
			NumberArray(State.Heights));
		return Result;
	}
}
