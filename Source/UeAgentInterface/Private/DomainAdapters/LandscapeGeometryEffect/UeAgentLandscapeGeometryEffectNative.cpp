#include "DomainAdapters/LandscapeGeometryEffect/UeAgentLandscapeGeometryEffectCapabilities.h"

#include "DomainAdapters/Landscape/UeAgentLandscapeHeightState.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeDataAccess.h"
#include "LandscapeEdit.h"
#include "LandscapeInfo.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"

namespace UE::UAI::DomainAdapters::LandscapeGeometryEffect
{
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace GenericEffect;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{
		enum class ELandscapeGeometryActionKind : uint8
		{
			Create,
			HeightDelta,
		};

		struct FLandscapeGeometryAction
		{
			ELandscapeGeometryActionKind Kind =
				ELandscapeGeometryActionKind::Create;
			FString CanonicalKey;
			FString TargetRef;
			int32 QuadsPerSection = 0;
			int32 SectionsPerComponent = 0;
			int32 ComponentCountX = 0;
			int32 ComponentCountY = 0;
			int32 MinX = 0;
			int32 MinY = 0;
			int32 MaxX = 0;
			int32 MaxY = 0;
			int32 RowStride = 0;
			FVector Translation = FVector::ZeroVector;
			FQuat Rotation = FQuat::Identity;
			FVector Scale = FVector::OneVector;
			float InitialLocalHeight = 0.0f;
			TArray<float> DeltaLocalHeights;
			TArray<uint16> OriginalEncodedHeights;
			TWeakObjectPtr<ALandscape> Landscape;
			TWeakObjectPtr<UPackage> Package;
			bool bPackageWasDirty = false;
		};

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
			FExecutionMutationError& OutError)
		{
			OutValue.Reset();
			const FUeAgentCanonicalValuePtr Value =
				FindField(Object, Field);
			if (!Value.IsValid()
				|| Value->GetKind() != EUeAgentValueKind::String)
			{
				return Fail(
					OutError,
					TEXT("landscape_geometry_internal_field_type"),
					Field);
			}
			OutValue = Value->GetText();
			return !OutValue.IsEmpty()
				|| Fail(
					OutError,
					TEXT("landscape_geometry_internal_field_empty"),
					Field);
		}

		bool ReadNumber(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			double& OutValue,
			FExecutionMutationError& OutError)
		{
			OutValue = 0.0;
			const FUeAgentCanonicalValuePtr Value =
				FindField(Object, Field);
			if (!Value.IsValid()
				|| (Value->GetKind() != EUeAgentValueKind::Decimal
					&& Value->GetKind()
						!= EUeAgentValueKind::Integer))
			{
				return Fail(
					OutError,
					TEXT("landscape_geometry_internal_field_type"),
					Field);
			}
			OutValue = FCString::Atod(*Value->GetLexeme());
			return FMath::IsFinite(OutValue)
				|| Fail(
					OutError,
					TEXT("landscape_geometry_internal_number_invalid"),
					Field);
		}

		bool ReadInteger(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			int32& OutValue,
			FExecutionMutationError& OutError)
		{
			double Number = 0.0;
			if (!ReadNumber(Object, Field, Number, OutError)
				|| Number < static_cast<double>(MIN_int32)
				|| Number > static_cast<double>(MAX_int32))
			{
				return false;
			}
			OutValue = FMath::RoundToInt(Number);
			return FMath::IsNearlyEqual(
					Number,
					static_cast<double>(OutValue),
					UE_DOUBLE_SMALL_NUMBER)
				|| Fail(
					OutError,
					TEXT("landscape_geometry_internal_integer_invalid"),
					Field);
		}

		bool ReadNumberArray(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			const int32 ExpectedCount,
			TArray<double>& OutValues,
			FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			const FUeAgentCanonicalValuePtr Value =
				FindField(Object, Field);
			if (!Value.IsValid()
				|| Value->GetKind() != EUeAgentValueKind::Array
				|| (ExpectedCount >= 0
					&& Value->GetArrayItems().Num()
						!= ExpectedCount))
			{
				return Fail(
					OutError,
					TEXT("landscape_geometry_internal_field_type"),
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
							"landscape_geometry_internal_number_invalid"),
						Field);
				}
				const double Number =
					FCString::Atod(*Item->GetLexeme());
				if (!FMath::IsFinite(Number))
				{
					return Fail(
						OutError,
						TEXT(
							"landscape_geometry_internal_number_invalid"),
						Field);
				}
				OutValues.Add(Number);
			}
			return true;
		}

		bool ReadIntegerArray(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			const int32 ExpectedCount,
			TArray<int32>& OutValues,
			FExecutionMutationError& OutError)
		{
			TArray<double> Numbers;
			if (!ReadNumberArray(
					Object,
					Field,
					ExpectedCount,
					Numbers,
					OutError))
			{
				return false;
			}
			OutValues.Reset();
			for (const double Number : Numbers)
			{
				if (Number < static_cast<double>(MIN_int32)
					|| Number > static_cast<double>(MAX_int32))
				{
					return Fail(
						OutError,
						TEXT(
							"landscape_geometry_internal_integer_invalid"),
						Field);
				}
				const int32 Integer = FMath::RoundToInt(Number);
				if (!FMath::IsNearlyEqual(
						Number,
						static_cast<double>(Integer),
						UE_DOUBLE_SMALL_NUMBER))
				{
					return Fail(
						OutError,
						TEXT(
							"landscape_geometry_internal_integer_invalid"),
						Field);
				}
				OutValues.Add(Integer);
			}
			return true;
		}

		bool IsLandscapeLocalHeightRepresentable(
			const double LocalHeight)
		{
			return FMath::IsFinite(LocalHeight)
				&& LocalHeight >= -256.0
				&& LocalHeight <= 255.9921875;
		}

		bool ReadLiveExtent(
			ALandscape& Landscape,
			int32& OutMinX,
			int32& OutMinY,
			int32& OutMaxX,
			int32& OutMaxY)
		{
			ULandscapeInfo* Info = Landscape.GetLandscapeInfo();
			return Info
				&& Info->GetLandscapeExtent(
					OutMinX,
					OutMinY,
					OutMaxX,
					OutMaxY);
		}

		bool PrepareCreate(
			const FPlannedChange& Change,
			FLandscapeGeometryAction& OutAction,
			FExecutionMutationError& OutError)
		{
			if (Change.GetIntent() != EChangeIntent::Create)
			{
				return Fail(
					OutError,
					TEXT("landscape_geometry_create_intent_invalid"));
			}
			const FUeAgentCanonicalValuePtr& After =
				Change.GetAfter();
			FString WriteFrame;
			FString ConversionEvidence;
			TArray<int32> ComponentCount;
			TArray<int32> MinInclusive;
			TArray<int32> MaxInclusive;
			TArray<double> Translation;
			TArray<double> Rotation;
			TArray<double> Scale;
			double InitialLocalHeight = 0.0;
			if (!ReadString(
					After,
					TEXT("target_ref"),
					OutAction.TargetRef,
					OutError)
				|| !ReadString(
					After,
					TEXT("write_frame_id"),
					WriteFrame,
					OutError)
				|| !ReadString(
					After,
					TEXT("conversion_evidence"),
					ConversionEvidence,
					OutError)
				|| WriteFrame
					!= TEXT(
						"ue.landscape.actor_and_local_sample_grid")
				|| !ReadInteger(
					After,
					TEXT("quads_per_section"),
					OutAction.QuadsPerSection,
					OutError)
				|| !ReadInteger(
					After,
					TEXT("sections_per_component"),
					OutAction.SectionsPerComponent,
					OutError)
				|| !ReadIntegerArray(
					After,
					TEXT("component_count"),
					2,
					ComponentCount,
					OutError)
				|| !ReadIntegerArray(
					After,
					TEXT("sample_min_inclusive"),
					2,
					MinInclusive,
					OutError)
				|| !ReadIntegerArray(
					After,
					TEXT("sample_max_inclusive"),
					2,
					MaxInclusive,
					OutError)
				|| !ReadNumberArray(
					After,
					TEXT("translation_cm"),
					3,
					Translation,
					OutError)
				|| !ReadNumberArray(
					After,
					TEXT("rotation_quaternion_xyzw"),
					4,
					Rotation,
					OutError)
				|| !ReadNumberArray(
					After,
					TEXT("scale"),
					3,
					Scale,
					OutError)
				|| !ReadNumber(
					After,
					TEXT("initial_local_height"),
					InitialLocalHeight,
					OutError))
			{
				return false;
			}

			OutAction.Kind = ELandscapeGeometryActionKind::Create;
			OutAction.CanonicalKey =
				Change.GetIdentity().GetCanonicalKey();
			OutAction.ComponentCountX = ComponentCount[0];
			OutAction.ComponentCountY = ComponentCount[1];
			OutAction.MinX = MinInclusive[0];
			OutAction.MinY = MinInclusive[1];
			OutAction.MaxX = MaxInclusive[0];
			OutAction.MaxY = MaxInclusive[1];
			OutAction.Translation = FVector(
				Translation[0],
				Translation[1],
				Translation[2]);
			OutAction.Rotation = FQuat(
				Rotation[0],
				Rotation[1],
				Rotation[2],
				Rotation[3]);
			OutAction.Scale =
				FVector(Scale[0], Scale[1], Scale[2]);
			OutAction.InitialLocalHeight =
				static_cast<float>(InitialLocalHeight);

			const int64 ExpectedQuadsX =
				static_cast<int64>(OutAction.QuadsPerSection)
				* OutAction.SectionsPerComponent
				* OutAction.ComponentCountX;
			const int64 ExpectedQuadsY =
				static_cast<int64>(OutAction.QuadsPerSection)
				* OutAction.SectionsPerComponent
				* OutAction.ComponentCountY;
			if (OutAction.QuadsPerSection <= 0
				|| (OutAction.SectionsPerComponent != 1
					&& OutAction.SectionsPerComponent != 2)
				|| OutAction.ComponentCountX <= 0
				|| OutAction.ComponentCountY <= 0
				|| static_cast<int64>(
					OutAction.MaxX - OutAction.MinX)
					!= ExpectedQuadsX
				|| static_cast<int64>(
					OutAction.MaxY - OutAction.MinY)
					!= ExpectedQuadsY
				|| OutAction.Scale.X <= 0.0
				|| OutAction.Scale.Y <= 0.0
				|| OutAction.Scale.Z <= 0.0
				|| !OutAction.Rotation.IsNormalized()
				|| !IsLandscapeLocalHeightRepresentable(
					InitialLocalHeight))
			{
				return Fail(
					OutError,
					TEXT("landscape_geometry_create_preflight_invalid"),
					OutAction.TargetRef);
			}
			return true;
		}

		bool PrepareHeightDelta(
			UWorld& World,
			IUeAgentLandscapeGeometryEffectReferenceResolver&
				Resolver,
			const FPlannedChange& Change,
			FLandscapeGeometryAction& OutAction,
			FExecutionMutationError& OutError)
		{
			if (Change.GetIntent() != EChangeIntent::Update)
			{
				return Fail(
					OutError,
					TEXT(
						"landscape_geometry_height_delta_intent_invalid"));
			}
			const FUeAgentCanonicalValuePtr& After =
				Change.GetAfter();
			FString WriteFrame;
			FString ConversionEvidence;
			TArray<double> ExpectedScale;
			TArray<int32> FrozenMin;
			TArray<int32> FrozenMax;
			TArray<int32> RegionMin;
			TArray<int32> RegionMax;
			TArray<double> DeltaValues;
			if (!ReadString(
					After,
					TEXT("target_ref"),
					OutAction.TargetRef,
					OutError)
				|| !ReadString(
					After,
					TEXT("write_frame_id"),
					WriteFrame,
					OutError)
				|| !ReadString(
					After,
					TEXT("conversion_evidence"),
					ConversionEvidence,
					OutError)
				|| WriteFrame
					!= TEXT("ue.landscape.local_sample_grid")
				|| !ReadNumberArray(
					After,
					TEXT("expected_scale"),
					3,
					ExpectedScale,
					OutError)
				|| !ReadIntegerArray(
					After,
					TEXT("frozen_extent_min_inclusive"),
					2,
					FrozenMin,
					OutError)
				|| !ReadIntegerArray(
					After,
					TEXT("frozen_extent_max_inclusive"),
					2,
					FrozenMax,
					OutError)
				|| !ReadIntegerArray(
					After,
					TEXT("region_min_inclusive"),
					2,
					RegionMin,
					OutError)
				|| !ReadIntegerArray(
					After,
					TEXT("region_max_inclusive"),
					2,
					RegionMax,
					OutError)
				|| !ReadInteger(
					After,
					TEXT("row_stride"),
					OutAction.RowStride,
					OutError)
				|| !ReadNumberArray(
					After,
					TEXT("delta_local_heights"),
					-1,
					DeltaValues,
					OutError))
			{
				return false;
			}

			ALandscape* Landscape = nullptr;
			DomainAdapters::Landscape::
				FLandscapeHeightStateError TargetError;
			if (!DomainAdapters::Landscape::
					TryResolveLandscapeHeightTarget(
						World,
						OutAction.TargetRef,
						&Resolver,
						Landscape,
						TargetError))
			{
				return Fail(
					OutError,
					TargetError.Code,
					TargetError.SubjectId
						+ (TargetError.Detail.IsEmpty()
							? FString()
							: TEXT(":") + TargetError.Detail));
			}

			int32 LiveMinX = 0;
			int32 LiveMinY = 0;
			int32 LiveMaxX = 0;
			int32 LiveMaxY = 0;
			const FVector LiveScale = Landscape->GetActorScale3D();
			if (!ReadLiveExtent(
					*Landscape,
					LiveMinX,
					LiveMinY,
					LiveMaxX,
					LiveMaxY)
				|| LiveMinX != FrozenMin[0]
				|| LiveMinY != FrozenMin[1]
				|| LiveMaxX != FrozenMax[0]
				|| LiveMaxY != FrozenMax[1]
				|| !LiveScale.Equals(
					FVector(
						ExpectedScale[0],
						ExpectedScale[1],
						ExpectedScale[2]),
					UE_KINDA_SMALL_NUMBER))
			{
				return Fail(
					OutError,
					TEXT(
						"landscape_geometry_live_state_stale"),
					OutAction.TargetRef);
			}

			OutAction.Kind =
				ELandscapeGeometryActionKind::HeightDelta;
			OutAction.CanonicalKey =
				Change.GetIdentity().GetCanonicalKey();
			OutAction.MinX = RegionMin[0];
			OutAction.MinY = RegionMin[1];
			OutAction.MaxX = RegionMax[0];
			OutAction.MaxY = RegionMax[1];
			OutAction.Scale = LiveScale;
			OutAction.Landscape = Landscape;
			OutAction.Package = Landscape->GetOutermost();
			OutAction.bPackageWasDirty =
				OutAction.Package.IsValid()
					&& OutAction.Package->IsDirty();

			const int32 Width =
				OutAction.MaxX - OutAction.MinX + 1;
			const int32 Height =
				OutAction.MaxY - OutAction.MinY + 1;
			if (Width <= 0
				|| Height <= 0
				|| OutAction.RowStride != Width
				|| OutAction.MinX < LiveMinX
				|| OutAction.MinY < LiveMinY
				|| OutAction.MaxX > LiveMaxX
				|| OutAction.MaxY > LiveMaxY
				|| static_cast<int64>(DeltaValues.Num())
					!= static_cast<int64>(Width) * Height)
			{
				return Fail(
					OutError,
					TEXT(
						"landscape_geometry_height_delta_preflight_invalid"),
					OutAction.TargetRef);
			}

			ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
			if (!Info)
			{
				return Fail(
					OutError,
					TEXT("landscape_geometry_info_missing"),
					OutAction.TargetRef);
			}
			OutAction.OriginalEncodedHeights.SetNumUninitialized(
				Width * Height);
			FLandscapeEditDataInterface Edit(Info);
			Edit.GetHeightDataFast(
				OutAction.MinX,
				OutAction.MinY,
				OutAction.MaxX,
				OutAction.MaxY,
				OutAction.OriginalEncodedHeights.GetData(),
				Width,
				nullptr);
			OutAction.DeltaLocalHeights.Reserve(
				DeltaValues.Num());
			for (int32 Index = 0;
				Index < DeltaValues.Num();
				++Index)
			{
				const double ResultHeight =
					LandscapeDataAccess::GetLocalHeight(
						OutAction.OriginalEncodedHeights[Index])
					+ DeltaValues[Index];
				if (!IsLandscapeLocalHeightRepresentable(
						ResultHeight))
				{
					return Fail(
						OutError,
						TEXT(
							"landscape_geometry_height_delta_overflow"),
						OutAction.TargetRef);
				}
				OutAction.DeltaLocalHeights.Add(
					static_cast<float>(DeltaValues[Index]));
			}
			return true;
		}

		bool ApplyCreate(
			UWorld& World,
			FLandscapeGeometryAction& Action,
			FExecutionMutationError& OutError)
		{
			const FString TargetLabel =
				FPaths::GetBaseFilename(Action.TargetRef);
			for (TActorIterator<ALandscape> It(&World); It; ++It)
			{
				if (It->GetActorLabel() == TargetLabel)
				{
					return Fail(
						OutError,
						TEXT(
							"landscape_geometry_create_conflict"),
						Action.TargetRef);
				}
			}

			UPackage* Package = World.PersistentLevel
				? World.PersistentLevel->GetOutermost()
				: World.GetOutermost();
			Action.Package = Package;
			Action.bPackageWasDirty =
				Package && Package->IsDirty();

			FActorSpawnParameters SpawnParameters;
			SpawnParameters.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnParameters.ObjectFlags |= RF_Transactional;
			ALandscape* Landscape =
				World.SpawnActor<ALandscape>(
					Action.Translation,
					Action.Rotation.Rotator(),
					SpawnParameters);
			if (!Landscape)
			{
				return Fail(
					OutError,
					TEXT("landscape_geometry_spawn_failed"),
					Action.TargetRef);
			}
			Action.Landscape = Landscape;
			Landscape->SetActorScale3D(Action.Scale);
			Landscape->SetActorLabel(TargetLabel);
			FString OwnershipError;
			if (!UeAgentLevelAuthoring::TryBindManagedActorOwnership(
					*Landscape,
					Action.TargetRef,
					TEXT("landscape"),
					OwnershipError))
			{
				World.DestroyActor(Landscape);
				Action.Landscape.Reset();
				if (Package)
				{
					Package->SetDirtyFlag(Action.bPackageWasDirty);
				}
				return Fail(
					OutError,
					TEXT(
						"landscape_geometry_managed_ownership_failed"),
					OwnershipError);
			}

			const int64 Width =
				static_cast<int64>(Action.MaxX)
					- Action.MinX + 1;
			const int64 Height =
				static_cast<int64>(Action.MaxY)
					- Action.MinY + 1;
			if (Width <= 0
				|| Height <= 0
				|| Width * Height > MAX_int32)
			{
				World.DestroyActor(Landscape);
				Action.Landscape.Reset();
				if (Package)
				{
					Package->SetDirtyFlag(
						Action.bPackageWasDirty);
				}
				return Fail(
					OutError,
					TEXT(
						"landscape_geometry_create_size_invalid"),
					Action.TargetRef);
			}

			TArray<uint16> Heights;
			Heights.Init(
				LandscapeDataAccess::GetTexHeight(
					Action.InitialLocalHeight),
				static_cast<int32>(Width * Height));
			TMap<FGuid, TArray<uint16>> HeightsByLayer;
			HeightsByLayer.Add(FGuid(), MoveTemp(Heights));
			TMap<
				FGuid,
				TArray<FLandscapeImportLayerInfo>>
				MaterialLayersByLayer;
			MaterialLayersByLayer.Add(
				FGuid(),
				TArray<FLandscapeImportLayerInfo>());
			const TArrayView<const FLandscapeLayer> EmptyLayers;
			Landscape->Import(
				FGuid::NewGuid(),
				Action.MinX,
				Action.MinY,
				Action.MaxX,
				Action.MaxY,
				Action.SectionsPerComponent,
				Action.QuadsPerSection,
				HeightsByLayer,
				TEXT(""),
				MaterialLayersByLayer,
				ELandscapeImportAlphamapType::Additive,
				EmptyLayers);
			Landscape->CreateLandscapeInfo();
			Landscape->PostEditChange();

			int32 LiveMinX = 0;
			int32 LiveMinY = 0;
			int32 LiveMaxX = 0;
			int32 LiveMaxY = 0;
			if (!ReadLiveExtent(
					*Landscape,
					LiveMinX,
					LiveMinY,
					LiveMaxX,
					LiveMaxY)
				|| LiveMinX != Action.MinX
				|| LiveMinY != Action.MinY
				|| LiveMaxX != Action.MaxX
				|| LiveMaxY != Action.MaxY)
			{
				World.DestroyActor(Landscape);
				Action.Landscape.Reset();
				if (Package)
				{
					Package->SetDirtyFlag(
						Action.bPackageWasDirty);
				}
				return Fail(
					OutError,
					TEXT(
						"landscape_geometry_create_readback_failed"),
					Action.TargetRef);
			}
			return true;
		}

		bool ApplyHeightDelta(
			FLandscapeGeometryAction& Action,
			FExecutionMutationError& OutError)
		{
			ALandscape* Landscape = Action.Landscape.Get();
			ULandscapeInfo* Info =
				Landscape ? Landscape->GetLandscapeInfo() : nullptr;
			if (!Landscape || !Info)
			{
				return Fail(
					OutError,
					TEXT(
						"landscape_geometry_height_target_lost"),
					Action.TargetRef);
			}
			TArray<uint16> Updated =
				Action.OriginalEncodedHeights;
			for (int32 Index = 0;
				Index < Updated.Num();
				++Index)
			{
				const float CurrentLocalHeight =
					LandscapeDataAccess::GetLocalHeight(
						Updated[Index]);
				Updated[Index] =
					LandscapeDataAccess::GetTexHeight(
						CurrentLocalHeight
							+ Action.DeltaLocalHeights[Index]);
			}
			FLandscapeEditDataInterface Edit(Info);
			Edit.SetHeightData(
				Action.MinX,
				Action.MinY,
				Action.MaxX,
				Action.MaxY,
				Updated.GetData(),
				Action.RowStride,
				true);
			Landscape->PostEditChange();
			Landscape->MarkPackageDirty();
			return true;
		}

		bool RollbackAction(
			UWorld& World,
			const FLandscapeGeometryAction& Action)
		{
			UPackage* Package = Action.Package.Get();
			if (Action.Kind
				== ELandscapeGeometryActionKind::Create)
			{
				if (ALandscape* Landscape =
					Action.Landscape.Get())
				{
					if (Landscape->GetWorld() != &World
						|| !World.DestroyActor(Landscape))
					{
						return false;
					}
				}
				if (Package)
				{
					Package->SetDirtyFlag(
						Action.bPackageWasDirty);
				}
				return true;
			}

			ALandscape* Landscape = Action.Landscape.Get();
			ULandscapeInfo* Info =
				Landscape ? Landscape->GetLandscapeInfo() : nullptr;
			if (!Landscape
				|| Landscape->GetWorld() != &World
				|| !Info
				|| Action.OriginalEncodedHeights.IsEmpty())
			{
				return false;
			}
			FLandscapeEditDataInterface Edit(Info);
			Edit.SetHeightData(
				Action.MinX,
				Action.MinY,
				Action.MaxX,
				Action.MaxY,
				Action.OriginalEncodedHeights.GetData(),
				Action.RowStride,
				true);
			Landscape->PostEditChange();
			if (Package)
			{
				Package->SetDirtyFlag(Action.bPackageWasDirty);
			}
			return true;
		}

		bool RollbackAll(
			UWorld& World,
			const TArray<FLandscapeGeometryAction>& Actions)
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

		class FLandscapeGeometryEffectNativeHook final
			: public IUeAgentLandscapeGeometryEffectNativeHook
		{
		public:
			explicit FLandscapeGeometryEffectNativeHook(
				TSharedRef<
					IUeAgentLandscapeGeometryEffectReferenceResolver,
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
							"landscape_geometry_game_thread_required"));
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
							"landscape_geometry_exact_world_context_mismatch"));
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
							"landscape_geometry_change_binding_mismatch"));
				}

				TArray<FLandscapeGeometryAction> Actions;
				Actions.Reserve(Changes.Num());
				for (const FPlannedChange& Change : Changes)
				{
					FLandscapeGeometryAction Action;
					const FString& TargetKind =
						Change.GetIdentity().GetTargetKind();
					const bool bPrepared =
						TargetKind
							== TEXT("landscape.geometry.create")
						? PrepareCreate(
							Change,
							Action,
							OutError)
						: TargetKind
								== TEXT(
									"landscape.geometry.height_delta")
							? PrepareHeightDelta(
								World,
								*Resolver,
								Change,
								Action,
								OutError)
							: Fail(
								OutError,
								TEXT(
									"landscape_geometry_target_kind_unsupported"),
								TargetKind);
					if (!bPrepared)
					{
						return false;
					}
					Actions.Add(MoveTemp(Action));
				}

				TArray<FLandscapeGeometryAction> AppliedActions;
				AppliedActions.Reserve(Actions.Num());
				TArray<FString> MutatedKeys;
				for (FLandscapeGeometryAction& Action : Actions)
				{
					const bool bApplied =
						Action.Kind
							== ELandscapeGeometryActionKind::Create
						? ApplyCreate(World, Action, OutError)
						: ApplyHeightDelta(Action, OutError);
					if (!bApplied)
					{
						RollbackAll(World, AppliedActions);
						return false;
					}
					MutatedKeys.Add(Action.CanonicalKey);
					AppliedActions.Add(MoveTemp(Action));
				}

				const FString Token = FString::Printf(
					TEXT("landscape-geometry:%s:%lld"),
					*Request.GetOperationId(),
					Lease.GetFencingToken());
				{
					FScopeLock Lock(&StateMutex);
					RollbackByToken.Add(Token, AppliedActions);
					TokenByOperation.Add(
						Request.GetOperationId(),
						Token);
				}
				OutResult = FUeAgentGenericEffectApplyResult(
					Lease.GetFencingToken(),
					MoveTemp(MutatedKeys),
					{TEXT("landscape.authoring.memory")},
					true,
					Token,
					true);
				return OutResult.IsValid()
					|| Fail(
						OutError,
						TEXT(
							"landscape_geometry_apply_result_invalid"));
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
							"landscape_geometry_rollback_request_invalid"));
				}
				TArray<FLandscapeGeometryAction> Actions;
				{
					FScopeLock Lock(&StateMutex);
					const TArray<FLandscapeGeometryAction>* Found =
						RollbackByToken.Find(
							Applied.GetRollbackToken());
					if (!Found)
					{
						return Fail(
							OutError,
							TEXT(
								"landscape_geometry_rollback_token_unknown"));
					}
					Actions = *Found;
				}
				bOutConfirmed = RollbackAll(World, Actions);
				if (!bOutConfirmed)
				{
					return Fail(
						OutError,
						TEXT(
							"landscape_geometry_rollback_unconfirmed"));
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
							"landscape_geometry_barrier_request_invalid"));
				}
				OutResult = FBarrierResult(
					Request.GetSubjectGeneration(),
					Request.GetRequiredSignals(),
					FString::Printf(
						TEXT("landscape-geometry:%lld"),
						Request.GetSubjectGeneration()),
					true);
				return OutResult.IsValid()
					|| Fail(
						OutError,
						TEXT(
							"landscape_geometry_barrier_result_invalid"));
			}

		private:
			TSharedRef<
				IUeAgentLandscapeGeometryEffectReferenceResolver,
				ESPMode::ThreadSafe> Resolver;
			FCriticalSection StateMutex;
			TMap<
				FString,
				TArray<FLandscapeGeometryAction>> RollbackByToken;
			TMap<FString, FString> TokenByOperation;
		};
	}

	bool FUeAgentLandscapeGeometryEffectNativeHookFactory::TryCreate(
		TSharedRef<
			IUeAgentLandscapeGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe> Resolver,
		TSharedPtr<
			IUeAgentLandscapeGeometryEffectNativeHook>& OutHook,
		FExecutionMutationError& OutError)
	{
		OutHook =
			MakeShared<FLandscapeGeometryEffectNativeHook>(
				MoveTemp(Resolver));
		OutError = FExecutionMutationError();
		return OutHook.IsValid();
	}
}
