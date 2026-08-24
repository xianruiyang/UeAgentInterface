#include "Composition/SimpleAuthoring/Families/Landscape/UeAgentLandscapeSimpleAuthoringProduction.h"

#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/Landscape/UeAgentLandscapeHeightState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Landscape.h"
#include "LandscapeDataAccess.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Composition::SimpleAuthoring::Landscape
{
	namespace
	{
		using namespace AssetDurable;
		using namespace ContextResource;
		using namespace Contract;
		using namespace Discovery;
		using namespace DomainAdapters::GenericEffect;
		using namespace
			DomainAdapters::LandscapeGeometryEffect;
		using namespace DomainAdapters::WorldSimpleEffect;
		using namespace ExecutionMutation;
		using namespace PlanningPreflight;
		using namespace StateSemantic;
		using namespace Verification;

		constexpr const TCHAR* PreStatePortId =
			TEXT("landscape-geometry.authoring-memory.v1");
		constexpr const TCHAR* PersistedStatePortId =
			TEXT("landscape-geometry.independent-readback.v1");
		constexpr const TCHAR* RequestEvidencePrefix =
			TEXT("landscape-geometry-request-v1:");

		enum class ELandscapeProfileMechanic : uint8
		{
			Create,
			HeightDelta,
		};

		struct FLandscapeProfileSpec
		{
			const TCHAR* DescriptorId;
			const TCHAR* CapabilityId;
			const TCHAR* NativeTargetKind;
			const TCHAR* StateKind;
			ELandscapeProfileMechanic Mechanic;
			EUeAgentCandidateOperation Operation;
		};

		const FLandscapeProfileSpec LandscapeProfiles[] = {
			{
				TEXT("uai.operation.landscape.create"),
				TEXT("landscape.create"),
				TEXT("landscape.geometry.create"),
				TEXT("landscape_geometry"),
				ELandscapeProfileMechanic::Create,
				EUeAgentCandidateOperation::Create,
			},
			{
				TEXT("uai.operation.landscape.height_delta"),
				TEXT("landscape.height_delta"),
				TEXT("landscape.geometry.height_delta"),
				TEXT("landscape_height_region"),
				ELandscapeProfileMechanic::HeightDelta,
				EUeAgentCandidateOperation::Update,
			},
		};

		const FLandscapeProfileSpec* FindProfile(
			const FUeAgentDescriptorIdentity& Identity)
		{
			for (const FLandscapeProfileSpec& Profile
				: LandscapeProfiles)
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
			FUeAgentLandscapeSimpleAuthoringError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		void SetProductionError(
			FUeAgentSimpleAuthoringProductionError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		bool TryParseJson(
			const FString& Json,
			TSharedPtr<FJsonObject>& OutObject)
		{
			OutObject.Reset();
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Json);
			return FJsonSerializer::Deserialize(
					Reader,
					OutObject)
				&& OutObject.IsValid();
		}

		bool TryParseRequestEvidence(
			const FUeAgentCandidateBinding& Binding,
			TSharedPtr<FJsonObject>& OutPayload,
			FUeAgentDiscoveryError& OutError)
		{
			const FString& Evidence =
				Binding.GetBindingEvidence();
			if (!Evidence.StartsWith(RequestEvidencePrefix)
				|| !TryParseJson(
					Evidence.RightChop(
						FCString::Strlen(
							RequestEvidencePrefix)),
					OutPayload))
			{
				OutError.Code =
					TEXT(
						"landscape_geometry_request_evidence_invalid");
				OutError.SubjectId = Binding.GetBindingId();
				return false;
			}
			return true;
		}

		bool TryGetTargetRef(
			const TSharedPtr<FJsonObject>& Payload,
			FString& OutTargetRef)
		{
			OutTargetRef.Reset();
			return Payload.IsValid()
				&& Payload->TryGetStringField(
					TEXT("target_ref"),
					OutTargetRef)
				&& !OutTargetRef.IsEmpty();
		}

		bool TryReadNumberArray(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Field,
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

		bool TryReadIntegerArray(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Field,
			const int32 ExpectedCount,
			TArray<int32>& OutValues)
		{
			TArray<double> Numbers;
			if (!TryReadNumberArray(
					Object,
					Field,
					ExpectedCount,
					Numbers))
			{
				return false;
			}
			OutValues.Reset();
			for (const double Number : Numbers)
			{
				const int32 Integer = FMath::RoundToInt(Number);
				if (!FMath::IsNearlyEqual(
						Number,
						static_cast<double>(Integer),
						UE_DOUBLE_SMALL_NUMBER))
				{
					return false;
				}
				OutValues.Add(Integer);
			}
			return true;
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

		TArray<TSharedPtr<FJsonValue>> IntegerArray(
			const TArray<int32>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Values.Num());
			for (const int32 Value : Values)
			{
				Result.Add(MakeShared<FJsonValueNumber>(Value));
			}
			return Result;
		}

		bool TryCanonicalValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			OutValue.Reset();
			if (!JsonValue.IsValid())
			{
				OutError.Code =
					TEXT(
						"landscape_geometry_state_json_invalid");
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
						if (!TryCanonicalValue(
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
						if (!TryCanonicalValue(
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
					TEXT(
						"landscape_geometry_state_json_kind_invalid");
				return false;
			}
		}

		bool TryCanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			return TryCanonicalValue(
				MakeShared<FJsonValueObject>(Object),
				Resources,
				OutValue,
				OutError);
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

		bool ReadCanonicalNumbers(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			TArray<double>& OutValues)
		{
			OutValues.Reset();
			const FUeAgentCanonicalValuePtr Array =
				FindField(Object, Field);
			if (!Array.IsValid()
				|| Array->GetKind() != EUeAgentValueKind::Array)
			{
				return false;
			}
			for (const FUeAgentCanonicalValueRef& Item
				: Array->GetArrayItems())
			{
				if (Item->GetKind() != EUeAgentValueKind::Decimal
					&& Item->GetKind()
						!= EUeAgentValueKind::Integer)
				{
					return false;
				}
				const double Value =
					FCString::Atod(*Item->GetLexeme());
				if (!FMath::IsFinite(Value))
				{
					return false;
				}
				OutValues.Add(Value);
			}
			return true;
		}

		bool ReadCanonicalIntegers(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			TArray<int32>& OutValues)
		{
			TArray<double> Numbers;
			if (!ReadCanonicalNumbers(Object, Field, Numbers))
			{
				return false;
			}
			OutValues.Reset();
			for (const double Number : Numbers)
			{
				const int32 Integer = FMath::RoundToInt(Number);
				if (!FMath::IsNearlyEqual(
						Number,
						static_cast<double>(Integer),
						UE_DOUBLE_SMALL_NUMBER))
				{
					return false;
				}
				OutValues.Add(Integer);
			}
			return true;
		}

		bool ReadCanonicalString(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			FString& OutValue)
		{
			OutValue.Reset();
			const FUeAgentCanonicalValuePtr Value =
				FindField(Object, Field);
			if (!Value.IsValid()
				|| Value->GetKind() != EUeAgentValueKind::String)
			{
				return false;
			}
			OutValue = Value->GetText();
			return !OutValue.IsEmpty();
		}

		FUeAgentCanonicalType LandscapeStateType()
		{
			return FUeAgentCanonicalType(
				TEXT("landscape_geometry_current_state"),
				EUeAgentValueKind::Object,
				false,
				TEXT("object"));
		}

		FUeAgentDefaultValue LandscapeStateDefault(
			const FUeAgentCanonicalValueRef& Value)
		{
			return FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::Schema,
				TEXT(
					"landscape-geometry:empty-default"),
				TEXT(
					"Landscape geometry state has no implicit target"),
				Value);
		}

		FUeAgentStateDetails LandscapeStateDetails(
			const FLandscapeProfileSpec& Profile)
		{
			return FUeAgentStateDetails(
				Profile.CapabilityId,
				TEXT(""),
				{
					TEXT("Landscape"),
					TEXT("SimpleAuthoring"),
					Profile.StateKind,
				},
				true,
				TEXT("available"),
				{});
		}

		FUeAgentCanonicalIdentity StateIdentity(
			const FLandscapeProfileSpec& Profile,
			const FUeAgentCandidateBinding& Binding,
			const FString& TargetRef)
		{
			const FUeAgentCanonicalIdentity& Root =
				Binding.GetSummary().GetIdentity();
			return FUeAgentCanonicalIdentity(
				TEXT("uai.landscape.geometry:") + TargetRef,
				Profile.NativeTargetKind,
				Root.GetOwnerId(),
				TargetRef,
				TEXT("landscape_geometry"),
				Root.GetGeneration());
		}

		EUeAgentCandidateOperation CandidateOperation(
			const EChangeIntent Intent)
		{
			switch (Intent)
			{
			case EChangeIntent::Create:
				return EUeAgentCandidateOperation::Create;
			case EChangeIntent::Delete:
				return EUeAgentCandidateOperation::Delete;
			case EChangeIntent::Update:
			default:
				return EUeAgentCandidateOperation::Update;
			}
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
						"landscape_geometry_state_catalog_unavailable")
					: Error.GetCode();
				OutError.Detail = Error.GetDetail();
				return false;
			}
			for (const FUeAgentWorldSimpleEffectWorldCandidate&
				Candidate : Candidates)
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
						TEXT(
							"landscape_geometry_state_world_ambiguous");
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
					TEXT(
						"landscape_geometry_state_world_unresolved");
				OutError.SubjectId = Binding.GetBindingId();
				return false;
			}
			return true;
		}

		TSharedPtr<FJsonObject> MakeRegion(
			const TArray<int32>& Min,
			const TArray<int32>& Max)
		{
			const TSharedPtr<FJsonObject> Region =
				MakeShared<FJsonObject>();
			Region->SetArrayField(
				TEXT("min_inclusive"),
				IntegerArray(Min));
			Region->SetArrayField(
				TEXT("max_inclusive"),
				IntegerArray(Max));
			return Region;
		}

		bool TryBuildLiveState(
			const FLandscapeProfileSpec& Profile,
			const TSharedPtr<FJsonObject>& Payload,
			ALandscape& Landscape,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentDiscoveryError& OutError)
		{
			FString TargetRef;
			const TSharedPtr<FJsonObject>* Region = nullptr;
			const FString RegionField =
				Profile.Mechanic
					== ELandscapeProfileMechanic::Create
				? TEXT("sample_region")
				: TEXT("region");
			if (!TryGetTargetRef(Payload, TargetRef)
				|| !Payload->TryGetObjectField(
					RegionField,
					Region)
				|| !Region)
			{
				OutError.Code =
					TEXT(
						"landscape_geometry_state_scope_invalid");
				OutError.SubjectId = TargetRef;
				return false;
			}
			TArray<int32> RegionMin;
			TArray<int32> RegionMax;
			if (!TryReadIntegerArray(
					*Region,
					TEXT("min_inclusive"),
					2,
					RegionMin)
				|| !TryReadIntegerArray(
					*Region,
					TEXT("max_inclusive"),
					2,
					RegionMax))
			{
				OutError.Code =
					TEXT(
						"landscape_geometry_state_read_failed");
				OutError.SubjectId = TargetRef;
				return false;
			}

			DomainAdapters::Landscape::FLandscapeHeightState
				HeightState;
			DomainAdapters::Landscape::FLandscapeHeightStateError
				HeightError;
			if (!DomainAdapters::Landscape::
				TryReadLandscapeHeightState(
					Landscape,
					FIntPoint(RegionMin[0], RegionMin[1]),
					FIntPoint(RegionMax[0], RegionMax[1]),
					MAX_int32,
					HeightState,
					HeightError))
			{
				OutError.Code = TEXT("landscape_geometry_state_read_failed");
				OutError.SubjectId = TargetRef;
				OutError.Detail = HeightError.Code + TEXT(":")
					+ HeightError.Detail;
				return false;
			}

			const FVector Scale = HeightState.Scale;
			const TSharedPtr<FJsonObject> State =
				MakeShared<FJsonObject>();
			State->SetStringField(TEXT("target_ref"), TargetRef);
			State->SetBoolField(
				TEXT("identity_bound"),
				true);
			State->SetObjectField(
				TEXT("extent"),
				MakeRegion(
					{
						HeightState.ExtentMinInclusive.X,
						HeightState.ExtentMinInclusive.Y,
					},
					{
						HeightState.ExtentMaxInclusive.X,
						HeightState.ExtentMaxInclusive.Y,
					}));
			State->SetObjectField(
				TEXT("region"),
				MakeRegion(RegionMin, RegionMax));
			State->SetArrayField(
				TEXT("scale"),
				NumberArray({Scale.X, Scale.Y, Scale.Z}));
			State->SetStringField(
				TEXT("height_encoding"),
				TEXT("landscape_local_height"));
			State->SetArrayField(
				TEXT("heights"),
				NumberArray(HeightState.Heights));

			if (Profile.Mechanic
				== ELandscapeProfileMechanic::Create)
			{
				State->SetStringField(
					TEXT("managed_id"),
					UeAgentLevelAuthoring::GetManagedActorId(
						Landscape));
				State->SetStringField(
					TEXT("managed_folder"),
					UeAgentLevelAuthoring::GetActorFolderPathText(
						Landscape));
				State->SetBoolField(
					TEXT("managed_tag_present"),
					Landscape.ActorHasTag(
						UeAgentLevelAuthoring::
							GetManagedActorMarkerTag()));
				const TSharedPtr<FJsonObject>* Placement = nullptr;
				TArray<double> Translation;
				TArray<double> Rotation;
				TArray<double> ExpectedScale;
				FString Unit;
				bool bPlacementMatches = false;
				if (Payload->TryGetObjectField(
						TEXT("placement"),
						Placement)
					&& Placement
					&& (*Placement)->TryGetStringField(
						TEXT("unit"),
						Unit)
					&& TryReadNumberArray(
						*Placement,
						TEXT("translation"),
						3,
						Translation)
					&& TryReadNumberArray(
						*Placement,
						TEXT(
							"rotation_quaternion_xyzw"),
						4,
						Rotation)
					&& TryReadNumberArray(
						*Placement,
						TEXT("scale"),
						3,
						ExpectedScale))
				{
					const double UnitScale =
						Unit == TEXT("meter") ? 100.0 : 1.0;
					const FVector ExpectedTranslation(
						Translation[0] * UnitScale,
						Translation[1] * UnitScale,
						Translation[2] * UnitScale);
					const FQuat ExpectedRotation(
						Rotation[0],
						Rotation[1],
						Rotation[2],
						Rotation[3]);
					bPlacementMatches =
						Landscape.GetActorLocation().Equals(
							ExpectedTranslation,
							UE_KINDA_SMALL_NUMBER)
						&& Landscape.GetActorQuat().Equals(
							ExpectedRotation,
							UE_KINDA_SMALL_NUMBER)
						&& Scale.Equals(
							FVector(
								ExpectedScale[0],
								ExpectedScale[1],
								ExpectedScale[2]),
							UE_KINDA_SMALL_NUMBER);
				}
				State->SetBoolField(
					TEXT("placement_matches"),
					bPlacementMatches);
			}

			FUeAgentStateSemanticError StateError;
			if (!TryCanonicalObject(
					State,
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

		const FUeAgentStateEntry* FindPreStateEntry(
			const FUeAgentCanonicalSnapshotRef& PreState,
			const FUeAgentCanonicalIdentity& Identity)
		{
			for (const FUeAgentStateEntry& Entry
				: PreState->GetEntries())
			{
				if (Entry.GetIdentity() == Identity)
				{
					return &Entry;
				}
			}
			return nullptr;
		}

		bool TryBuildExpectedCreateState(
			const FPlannedChange& Change,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentSimpleAuthoringProductionError& OutError)
		{
			const FUeAgentCanonicalValuePtr& After =
				Change.GetAfter();
			FString TargetRef;
			TArray<int32> RegionMin;
			TArray<int32> RegionMax;
			TArray<double> Scale;
			double InitialHeight = 0.0;
			const FUeAgentCanonicalValuePtr Initial =
				FindField(
					After,
					TEXT("initial_local_height"));
			if (!ReadCanonicalString(
					After,
					TEXT("target_ref"),
					TargetRef)
				|| !ReadCanonicalIntegers(
					After,
					TEXT("sample_min_inclusive"),
					RegionMin)
				|| !ReadCanonicalIntegers(
					After,
					TEXT("sample_max_inclusive"),
					RegionMax)
				|| !ReadCanonicalNumbers(
					After,
					TEXT("scale"),
					Scale)
				|| !Initial.IsValid()
				|| (Initial->GetKind()
						!= EUeAgentValueKind::Decimal
					&& Initial->GetKind()
						!= EUeAgentValueKind::Integer))
			{
				SetProductionError(
					OutError,
					TEXT(
						"landscape_geometry_expected_create_invalid"),
					Change.GetIdentity().GetCanonicalKey());
				return false;
			}
			InitialHeight =
				FCString::Atod(*Initial->GetLexeme());
			const int32 Width =
				RegionMax[0] - RegionMin[0] + 1;
			const int32 Height =
				RegionMax[1] - RegionMin[1] + 1;
			TArray<double> Heights;
			Heights.Init(
				InitialHeight,
				Width * Height);
			const TSharedPtr<FJsonObject> State =
				MakeShared<FJsonObject>();
			State->SetStringField(
				TEXT("target_ref"),
				TargetRef);
			State->SetBoolField(
				TEXT("identity_bound"),
				true);
			State->SetObjectField(
				TEXT("extent"),
				MakeRegion(RegionMin, RegionMax));
			State->SetObjectField(
				TEXT("region"),
				MakeRegion(RegionMin, RegionMax));
			State->SetArrayField(
				TEXT("scale"),
				NumberArray(Scale));
			State->SetStringField(
				TEXT("height_encoding"),
				TEXT("landscape_local_height"));
			State->SetArrayField(
				TEXT("heights"),
				NumberArray(Heights));
			State->SetBoolField(
				TEXT("placement_matches"),
				true);
			State->SetStringField(
				TEXT("managed_id"),
				TargetRef);
			State->SetStringField(
				TEXT("managed_folder"),
				UeAgentLevelAuthoring::MakeManagedActorFolderPath(
					TEXT("landscape")));
			State->SetBoolField(
				TEXT("managed_tag_present"),
				true);
			FUeAgentStateSemanticError StateError;
			if (!TryCanonicalObject(
					State,
					Resources,
					OutValue,
					StateError))
			{
				SetProductionError(
					OutError,
					StateError.Code,
					Change.GetIdentity().GetCanonicalKey(),
					StateError.Detail);
				return false;
			}
			return true;
		}

		bool TryBuildExpectedDeltaState(
			const FPlannedChange& Change,
			const FUeAgentCanonicalSnapshotRef& PreState,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentSimpleAuthoringProductionError& OutError)
		{
			const FUeAgentStateEntry* Before =
				FindPreStateEntry(
					PreState,
					Change.GetIdentity());
			if (!Before)
			{
				SetProductionError(
					OutError,
					TEXT(
						"landscape_geometry_expected_prestate_missing"),
					Change.GetIdentity().GetCanonicalKey());
				return false;
			}
			const FUeAgentCanonicalValuePtr BeforeValue(
				Before->GetValue());
			FString TargetRef;
			TArray<double> Scale;
			TArray<double> Heights;
			TArray<double> Delta;
			TArray<int32> ExtentMin;
			TArray<int32> ExtentMax;
			TArray<int32> RegionMin;
			TArray<int32> RegionMax;
			const FUeAgentCanonicalValuePtr Extent =
				FindField(BeforeValue, TEXT("extent"));
			const FUeAgentCanonicalValuePtr Region =
				FindField(BeforeValue, TEXT("region"));
			if (!ReadCanonicalString(
					BeforeValue,
					TEXT("target_ref"),
					TargetRef)
				|| !ReadCanonicalNumbers(
					BeforeValue,
					TEXT("scale"),
					Scale)
				|| !ReadCanonicalNumbers(
					BeforeValue,
					TEXT("heights"),
					Heights)
				|| !ReadCanonicalIntegers(
					Extent,
					TEXT("min_inclusive"),
					ExtentMin)
				|| !ReadCanonicalIntegers(
					Extent,
					TEXT("max_inclusive"),
					ExtentMax)
				|| !ReadCanonicalIntegers(
					Region,
					TEXT("min_inclusive"),
					RegionMin)
				|| !ReadCanonicalIntegers(
					Region,
					TEXT("max_inclusive"),
					RegionMax)
				|| !ReadCanonicalNumbers(
					Change.GetAfter(),
					TEXT("delta_local_heights"),
					Delta)
				|| Heights.Num() != Delta.Num())
			{
				SetProductionError(
					OutError,
					TEXT(
						"landscape_geometry_expected_delta_invalid"),
					Change.GetIdentity().GetCanonicalKey());
				return false;
			}
			for (int32 Index = 0; Index < Heights.Num(); ++Index)
			{
				const float Quantized =
					LandscapeDataAccess::GetLocalHeight(
						LandscapeDataAccess::GetTexHeight(
							static_cast<float>(
								Heights[Index]
									+ Delta[Index])));
				Heights[Index] = Quantized;
			}
			const TSharedPtr<FJsonObject> State =
				MakeShared<FJsonObject>();
			State->SetStringField(
				TEXT("target_ref"),
				TargetRef);
			State->SetBoolField(
				TEXT("identity_bound"),
				true);
			State->SetObjectField(
				TEXT("extent"),
				MakeRegion(ExtentMin, ExtentMax));
			State->SetObjectField(
				TEXT("region"),
				MakeRegion(RegionMin, RegionMax));
			State->SetArrayField(
				TEXT("scale"),
				NumberArray(Scale));
			State->SetStringField(
				TEXT("height_encoding"),
				TEXT("landscape_local_height"));
			State->SetArrayField(
				TEXT("heights"),
				NumberArray(Heights));
			FUeAgentStateSemanticError StateError;
			if (!TryCanonicalObject(
					State,
					Resources,
					OutValue,
					StateError))
			{
				SetProductionError(
					OutError,
					StateError.Code,
					Change.GetIdentity().GetCanonicalKey(),
					StateError.Detail);
				return false;
			}
			return true;
		}

		class FLandscapeTargetResolver final
			: public IUeAgentSimpleAuthoringTargetResolver
		{
		public:
			explicit FLandscapeTargetResolver(
				TSharedRef<
					FUeAgentWorldSimpleEffectExactWorldResolver,
					ESPMode::ThreadSafe> InWorldResolver)
				: WorldResolver(MoveTemp(InWorldResolver))
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
				const FLandscapeProfileSpec* Profile =
					FindProfile(
						CanonicalRequest
							.GetDescriptorIdentity());
				TSharedPtr<FJsonObject> Payload;
				FString TargetRef;
				if (!Profile
					|| !TryParseJson(
						CanonicalRequest
							.GetPayload()
							.GetJsonValue(),
						Payload)
					|| !TryGetTargetRef(
						Payload,
						TargetRef))
				{
					SetProductionError(
						OutError,
						TEXT(
							"landscape_geometry_profile_missing"),
						CanonicalRequest
							.GetDescriptorIdentity()
							.GetDescriptorId());
					return false;
				}
				UWorld* World = nullptr;
				FExecutionMutationError Error;
				if (!Deadline.CanContinue()
					|| Cancellation.IsCancelled()
					|| !WorldResolver->TryResolveWorld(
						Context,
						World,
						Error)
					|| !World
					|| !World->PersistentLevel)
				{
					SetProductionError(
						OutError,
						Error.GetCode().IsEmpty()
							? TEXT(
								"landscape_geometry_target_unresolved")
							: Error.GetCode(),
						Context.GetWorldIdentity(),
						Error.GetDetail());
					return false;
				}
				const FUeAgentCanonicalIdentity Identity(
					TEXT("uai.landscape.operation:")
						+ TargetRef,
					TEXT("landscape.geometry.operation"),
					Context.GetWorldIdentity(),
					TargetRef,
					TEXT("landscape_geometry"),
					Context.GetContextRevision());
				OutTarget.Emplace(
					TEXT("landscape-geometry:")
						+ CanonicalRequest.GetRequestId(),
					FUeAgentCandidateSummary(
						Identity,
						CanonicalRequest
							.GetDescriptorIdentity(),
						TEXT("landscape_geometry"),
						TargetRef,
						TEXT(
							"exact Landscape geometry operation target"),
						TArray<FString>{
							TEXT("Landscape"),
							TEXT("SimpleAuthoring"),
						}),
					FUeAgentCatalogRevision(
						TEXT(
							"landscape-geometry-exact-context"),
						Context.GetContextRevision(),
						World->GetUniqueID()),
					World->GetOutermost()->GetName(),
					PersistedStatePortId,
					TArray<EUeAgentCandidateOperation>{
						Profile->Operation,
					},
					FString(RequestEvidencePrefix)
						+ CanonicalRequest
							.GetPayload()
							.GetJsonValue());
				if (!OutTarget->IsValid())
				{
					SetProductionError(
						OutError,
						TEXT(
							"landscape_geometry_target_binding_invalid"),
						CanonicalRequest.GetRequestId());
					OutTarget.Reset();
					return false;
				}
				return true;
			}

		private:
			TSharedRef<
				FUeAgentWorldSimpleEffectExactWorldResolver,
				ESPMode::ThreadSafe> WorldResolver;
		};

		class FLandscapeStateReadPort final
			: public IUeAgentStateReadPort
		{
		public:
			FLandscapeStateReadPort(
				FString InPortId,
				const EUeAgentStatePlane InRequiredPlane,
				TSharedRef<
					IUeAgentWorldSimpleEffectWorldCatalog,
					ESPMode::ThreadSafe> InCatalog,
				TSharedRef<
					IUeAgentLandscapeGeometryEffectReferenceResolver,
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
				const FLandscapeProfileSpec* Profile =
					Request.IsValid()
					? FindProfile(
						Request.GetBinding()
							.GetSummary()
							.GetDescriptorIdentity())
					: nullptr;
				if (!Profile
					|| Request.GetStatePlane() != RequiredPlane)
				{
					OutError.Code =
						TEXT(
							"landscape_geometry_state_request_invalid");
					return false;
				}
				if (!Deadline.CanContinue()
					|| Cancellation.IsCancelled())
				{
					OutError.Code =
						TEXT(
							"landscape_geometry_state_request_cancelled");
					return false;
				}
				const FUeAgentCandidateBinding& Binding =
					Request.GetBinding();
				TSharedPtr<FJsonObject> Payload;
				FString TargetRef;
				if (!TryParseRequestEvidence(
						Binding,
						Payload,
						OutError)
					|| !TryGetTargetRef(
						Payload,
						TargetRef))
				{
					return false;
				}
				UWorld* World = nullptr;
				if (!TryResolveExactWorld(
						*Catalog,
						Binding,
						bRequireSourceInstance,
						World,
						OutError))
				{
					return false;
				}

				ALandscape* Landscape = nullptr;
				DomainAdapters::Landscape::
					FLandscapeHeightStateError TargetError;
				const bool bTargetResolved =
					DomainAdapters::Landscape::
						TryResolveLandscapeHeightTarget(
							*World,
							TargetRef,
							&ReferenceResolver.Get(),
							Landscape,
							TargetError);
				if (Profile->Mechanic
					== ELandscapeProfileMechanic::Create)
				{
					if (RequiredPlane
						== EUeAgentStatePlane::AuthoringMemory)
					{
						if (bTargetResolved)
						{
							OutError.Code =
								TEXT(
									"landscape_geometry_create_conflict");
							OutError.SubjectId = TargetRef;
							return false;
						}
						if (TargetError.Code
							!= TEXT("landscape_height_target_not_found"))
						{
							OutError.Code = TargetError.Code;
							OutError.SubjectId = TargetRef;
							OutError.Detail = TargetError.Detail;
							return false;
						}
						Landscape = nullptr;
					}
					else if (!bTargetResolved)
					{
						OutError.Code = TargetError.Code
							== TEXT("landscape_height_target_not_found")
							? FString(TEXT(
								"landscape_geometry_created_target_missing"))
							: TargetError.Code;
						OutError.SubjectId = TargetRef;
						OutError.Detail = TargetError.Detail;
						return false;
					}
				}
				else if (!bTargetResolved)
				{
					OutError.Code = TargetError.Code;
					OutError.SubjectId = TargetRef;
					OutError.Detail = TargetError.Detail;
					return false;
				}

				FUeAgentCanonicalValuePtr DefaultValue;
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryObject(
						{},
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
					Binding.GetSummary()
						.GetIdentity()
						.GetGeneration(),
					World->GetUniqueID());
				TArray<FUeAgentStateEntry> Entries;
				if (Landscape)
				{
					FUeAgentCanonicalValuePtr Value;
					if (!TryBuildLiveState(
							*Profile,
							Payload,
							*Landscape,
							Resources,
							Value,
							OutError))
					{
						return false;
					}
					const bool bAggregate =
						Binding.GetSummary()
							.GetIdentity()
							.GetTargetKind()
						== TEXT(
							"landscape.geometry.operation");
					const FUeAgentCanonicalIdentity Identity =
						bAggregate
							? StateIdentity(
								*Profile,
								Binding,
								TargetRef)
							: Binding.GetSummary()
								.GetIdentity();
					Entries.Emplace(
						Identity,
						LandscapeStateType(),
						Value.ToSharedRef(),
						LandscapeStateDefault(
							DefaultValue.ToSharedRef()),
						LandscapeStateDetails(*Profile),
						Revision,
						EUeAgentAuthoredPresence::Present,
						EUeAgentOverrideState::Overridden);
				}

				FUeAgentCanonicalSnapshotPtr Snapshot;
				if (!FUeAgentCanonicalSnapshot::TryCreate(
						TEXT(
							"snapshot:landscape-geometry:")
							+ PortId
							+ TEXT(":")
							+ Binding.GetBindingId(),
						RequiredPlane,
						Revision,
						FUeAgentTemporalPoint(
							TEXT(
								"landscape-geometry-operation"),
							TEXT("1"),
							TEXT("1"),
							TEXT("1"),
							TEXT("observed")),
						FUeAgentStateProvenance(
							TEXT("independent_readback"),
							PortId,
							TargetRef),
						MoveTemp(Entries),
						Resources,
						Snapshot,
						StateError))
				{
					OutError.Code = StateError.Code;
					OutError.SubjectId = TargetRef;
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
				IUeAgentLandscapeGeometryEffectReferenceResolver,
				ESPMode::ThreadSafe> ReferenceResolver;
			bool bRequireSourceInstance = true;
		};

		class FLandscapeDraftBuilder final
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
				if (!TryBuildLandscapeGeometryEffectChangeSetDraft(
						CanonicalRequest,
						Context,
						PreState,
						Resources,
						OutDraft,
						Error))
				{
					SetProductionError(
						OutError,
						Error.GetCode(),
						CanonicalRequest.GetRequestId(),
						Error.GetDetail());
					return false;
				}
				return true;
			}
		};

		class FLandscapeChangeTargetProjector final
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
							TEXT("landscape_geometry"),
							Change.GetIdentity().GetSelector(),
							TEXT(
								"exact Landscape geometry change target"),
							TArray<FString>{
								TEXT("Landscape"),
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
						SetProductionError(
							OutError,
							TEXT(
								"landscape_geometry_change_target_invalid"),
							Change.GetIdentity()
								.GetCanonicalKey());
						OutTargets.Reset();
						return false;
					}
				}
				return !OutTargets.IsEmpty();
			}
		};

		class FLandscapeMutationBindingProjector final
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
				if (Changes.Num() != Targets.Num())
				{
					SetProductionError(
						OutError,
						TEXT(
							"landscape_geometry_change_targets_missing"),
						Plan->GetRequestId());
					return false;
				}
				for (int32 Index = 0; Index < Changes.Num(); ++Index)
				{
					OutBindings.Emplace(
						Targets[Index],
						TEXT(
							"UAI.LandscapeGeometry:CanonicalObject"),
						FUeAgentCanonicalType(
							TEXT(
								"landscape_geometry_canonical_object"),
							EUeAgentValueKind::Object,
							false,
							TEXT("object")),
						TEXT(
							"landscape-geometry:typed-native-binding"));
					if (Targets[Index].GetSummary()
							.GetIdentity()
						!= Changes[Index].GetIdentity()
						|| !OutBindings.Last().IsValid())
					{
						SetProductionError(
							OutError,
							TEXT(
								"landscape_geometry_mutation_binding_invalid"),
							Changes[Index]
								.GetIdentity()
								.GetCanonicalKey());
						OutBindings.Reset();
						return false;
					}
				}
				return !OutBindings.IsEmpty();
			}
		};

		class FLandscapeDurableTargetProjector final
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
					SetProductionError(
						OutError,
						TEXT(
							"landscape_geometry_durable_target_missing"),
						Target.GetBindingId());
					return false;
				}
				OutTargets.Emplace(
					Changes[0].GetIdentity(),
					Target.GetPublicRef(),
					EPackageHeaderKind::Map,
					EAssetDurableIntent::Save,
					ERollbackGrade::DurableBackup);
				return OutTargets[0].IsValid();
			}
		};

		class FLandscapeExpectedStateProjector final
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
				const FLandscapeProfileSpec* Profile =
					FindProfile(
						Plan->GetDescriptorIdentity());
				if (!Profile)
				{
					SetProductionError(
						OutError,
						TEXT(
							"landscape_geometry_expected_profile_missing"),
						Plan->GetDescriptorIdentity()
							.GetDescriptorId());
					return false;
				}
				FUeAgentCanonicalValuePtr DefaultValue;
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryObject(
						{},
						Resources,
						DefaultValue,
						StateError))
				{
					SetProductionError(
						OutError,
						StateError.Code,
						Plan->GetRequestId(),
						StateError.Detail);
					return false;
				}
				const FUeAgentStateRevision Revision(
					TEXT(
						"landscape-geometry-operation-expected"),
					Plan->GetFingerprint(),
					1);
				TArray<FString> ArtifactIds;
				for (const FDurableArtifact& Artifact
					: Inventory.GetArtifacts())
				{
					ArtifactIds.Add(Artifact.GetArtifactId());
				}
				for (const FPlannedChange& Change
					: Plan->GetChangeSet().GetChanges())
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
						SetProductionError(
							OutError,
							TEXT(
								"landscape_geometry_verification_target_missing"),
							Change.GetIdentity()
								.GetCanonicalKey());
						return false;
					}
					FUeAgentCanonicalValuePtr ExpectedValue;
					const bool bBuilt =
						Profile->Mechanic
							== ELandscapeProfileMechanic::Create
						? TryBuildExpectedCreateState(
							Change,
							Resources,
							ExpectedValue,
							OutError)
						: TryBuildExpectedDeltaState(
							Change,
							PreState,
							Resources,
							ExpectedValue,
							OutError);
					if (!bBuilt)
					{
						return false;
					}
					TArray<FUeAgentStateEntry> Entries;
					Entries.Emplace(
						Change.GetIdentity(),
						LandscapeStateType(),
						ExpectedValue.ToSharedRef(),
						LandscapeStateDefault(
							DefaultValue.ToSharedRef()),
						LandscapeStateDetails(*Profile),
						Revision,
						EUeAgentAuthoredPresence::Present,
						EUeAgentOverrideState::Overridden);
					FUeAgentCanonicalSnapshotPtr Expected;
					if (!FUeAgentCanonicalSnapshot::
							TryCreateExpected(
								TEXT(
									"snapshot:landscape-geometry:"
									"expected:")
									+ Plan->GetRequestId()
									+ TEXT(":")
									+ Change.GetIdentity()
										.GetCanonicalKey(),
								EUeAgentStatePlane::Persisted,
								Revision,
								FUeAgentTemporalPoint(
									TEXT(
										"landscape-geometry-operation"),
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
						SetProductionError(
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
						SetProductionError(
							OutError,
							TEXT(
								"landscape_geometry_verification_target_invalid"),
							Change.GetIdentity()
								.GetCanonicalKey());
						return false;
					}
				}
				return !OutTargets.IsEmpty();
			}
		};

		class FLandscapeUeHook final
			: public IUeAgentGenericEffectUeHook
		{
		public:
			FLandscapeUeHook(
				TSharedRef<
					FUeAgentWorldSimpleEffectExactWorldResolver,
					ESPMode::ThreadSafe> InWorldResolver,
				TSharedRef<
					IUeAgentLandscapeGeometryEffectNativeHook,
					ESPMode::ThreadSafe> InNativeHook)
				: WorldResolver(MoveTemp(InWorldResolver))
				, NativeHook(MoveTemp(InNativeHook))
			{
			}

			bool TryApply(
				const FExecutionMutationRequest& Request,
				const FLockFenceLease& Lease,
				const TArray<FUeAgentGenericEffectBinding>&
					Bindings,
				FUeAgentResourceTracker& Resources,
				FUeAgentGenericEffectApplyResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				UWorld* World = nullptr;
				if (!WorldResolver->TryResolveWorld(
						Request.GetExecutionContext(),
						World,
						OutError)
					|| !World)
				{
					return false;
				}
				return NativeHook->TryApply(
					*World,
					Request,
					Lease,
					Bindings,
					Resources,
					OutResult,
					OutError);
			}

			bool TryRollback(
				const FExecutionMutationRequest& Request,
				const FUeAgentGenericEffectApplyResult& Applied,
				FUeAgentResourceTracker& Resources,
				bool& bOutConfirmed,
				FExecutionMutationError& OutError) override
			{
				UWorld* World = nullptr;
				if (!WorldResolver->TryResolveWorld(
						Request.GetExecutionContext(),
						World,
						OutError)
					|| !World)
				{
					return false;
				}
				return NativeHook->TryRollback(
					*World,
					Request,
					Applied,
					Resources,
					bOutConfirmed,
					OutError);
			}

			void ReleaseVerifiedOperation(
				const FString& OperationId) override
			{
				NativeHook->ReleaseVerifiedOperation(OperationId);
			}

			bool TryWaitForSettle(
				const FBarrierRequest& Request,
				FUeAgentResourceTracker& Resources,
				FBarrierResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				return NativeHook->TryWaitForSettle(
					Request,
					Resources,
					OutResult,
					OutError);
			}

		private:
			TSharedRef<
				FUeAgentWorldSimpleEffectExactWorldResolver,
				ESPMode::ThreadSafe> WorldResolver;
			TSharedRef<
				IUeAgentLandscapeGeometryEffectNativeHook,
				ESPMode::ThreadSafe> NativeHook;
		};

		bool ValidateConfig(
			const FUeAgentLandscapeSimpleAuthoringProviderConfig&
				Config,
			FUeAgentLandscapeSimpleAuthoringError& OutError)
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
						"landscape_simple_authoring_dependencies_missing"));
				return false;
			}
			for (const FLandscapeProfileSpec& Profile
				: LandscapeProfiles)
			{
				const FUeAgentOperationDescriptor* Descriptor =
					Config.DescriptorRegistry
						->FindByDescriptorId(
							Profile.DescriptorId);
				if (!Descriptor
					|| Descriptor->GetCapabilityId()
						!= Profile.CapabilityId
					|| Descriptor->GetDomainId()
						!= TEXT("landscape"))
				{
					SetError(
						OutError,
						TEXT(
							"landscape_simple_authoring_descriptor_missing"),
						Profile.DescriptorId);
					return false;
				}
			}
			return true;
		}
	}

	bool FUeAgentLandscapeSimpleAuthoringProviderFactory::TryCreate(
		const FUeAgentLandscapeSimpleAuthoringProviderConfig& Config,
		TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider>&
			OutProvider,
		FUeAgentLandscapeSimpleAuthoringError& OutError)
	{
		OutProvider.Reset();
		OutError = FUeAgentLandscapeSimpleAuthoringError();
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
			IUeAgentLandscapeGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe> PersistedReferenceResolver =
				Config.PersistedReferenceResolver.IsValid()
					? Config.PersistedReferenceResolver
						.ToSharedRef()
					: Config.ReferenceResolver.ToSharedRef();
		const TSharedRef<
			FUeAgentWorldSimpleEffectExactWorldResolver,
			ESPMode::ThreadSafe> WorldResolver =
				MakeShared<
					FUeAgentWorldSimpleEffectExactWorldResolver,
					ESPMode::ThreadSafe>(Catalog);
		TSharedPtr<IUeAgentLandscapeGeometryEffectNativeHook>
			NativeHook;
		FExecutionMutationError NativeError;
		if (!FUeAgentLandscapeGeometryEffectNativeHookFactory::
				TryCreate(
					Config.ReferenceResolver.ToSharedRef(),
					NativeHook,
					NativeError)
			|| !NativeHook.IsValid())
		{
			SetError(
				OutError,
				NativeError.GetCode().IsEmpty()
					? TEXT(
						"landscape_geometry_native_hook_missing")
					: NativeError.GetCode(),
				TEXT("landscape"),
				NativeError.GetDetail());
			return false;
		}

		FUeAgentSimpleAuthoringProductionProviderConfig
			ProviderConfig;
		ProviderConfig.FamilyId = TEXT("landscape");
		ProviderConfig.OwnerLeaseId = Config.OwnerLeaseId;
		ProviderConfig.PreStatePlane =
			EUeAgentStatePlane::AuthoringMemory;
		ProviderConfig.AtomicityMode =
			EAtomicityMode::CompensatedSaga;
		ProviderConfig.MaxQueueItems = Config.MaxQueueItems;
		ProviderConfig.Projection = Config.Projection;
		ProviderConfig.TargetResolver =
			MakeShared<FLandscapeTargetResolver>(
				WorldResolver);
		ProviderConfig.PreStateReadPort =
			MakeShared<FLandscapeStateReadPort>(
				PreStatePortId,
				EUeAgentStatePlane::AuthoringMemory,
				Catalog,
				Config.ReferenceResolver.ToSharedRef(),
				true);
		ProviderConfig.ChangeTargetProjector =
			MakeShared<FLandscapeChangeTargetProjector>();
		ProviderConfig.DraftBuilder =
			MakeShared<FLandscapeDraftBuilder>();
		ProviderConfig.MutationBindingProjector =
			MakeShared<FLandscapeMutationBindingProjector>();
		ProviderConfig.DurableTargetProjector =
			MakeShared<FLandscapeDurableTargetProjector>();
		ProviderConfig.DurableService = Config.DurableService;
		ProviderConfig.UeHook =
			MakeShared<FLandscapeUeHook>(
				WorldResolver,
				NativeHook.ToSharedRef());
		ProviderConfig.ExpectedStateProjector =
			MakeShared<FLandscapeExpectedStateProjector>();
		const TSharedRef<IUeAgentStateReadPort, ESPMode::ThreadSafe>
			PersistedStateReadPort = MakeShared<FLandscapeStateReadPort>(
				PersistedStatePortId,
				EUeAgentStatePlane::Persisted,
				PersistedCatalog,
				PersistedReferenceResolver,
				false);
		ProviderConfig.PersistedStateOracle = MakeShared<
			FStateReadPortPersistedStateOracle,
			ESPMode::ThreadSafe>(PersistedStateReadPort);
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
