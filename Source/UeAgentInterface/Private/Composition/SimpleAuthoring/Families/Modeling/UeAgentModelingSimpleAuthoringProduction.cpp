#include "Composition/SimpleAuthoring/Families/Modeling/UeAgentModelingSimpleAuthoringProduction.h"
#include "Composition/SimpleAuthoring/Families/Modeling/UeAgentModelingSimpleAuthoringExistingEdit.h"

#include "Components/DynamicMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Core/AssetDurable/UeAgentPersistedPackage.h"
#include "Core/AssetDurable/UeAgentPersistedWorld.h"
#include "Core/StateSemantic/UeAgentStateRevisionJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMeshActor.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::Composition::SimpleAuthoring::Modeling
{
	namespace
	{
		using namespace AssetDurable;
		using namespace ContextResource;
		using namespace Contract;
		using namespace Discovery;
		using namespace DomainAdapters::GenericEffect;
		using namespace DomainAdapters::ModelingGeometryEffect;
		using namespace DomainAdapters::WorldSimpleEffect;
		using namespace ExecutionMutation;
		using namespace PlanningPreflight;
		using namespace StateSemantic;
		using namespace Verification;
		using UE::Geometry::FDynamicMesh3;

		constexpr const TCHAR* PreStatePortId =
			TEXT("modeling-geometry.authoring-memory.v1");
		constexpr const TCHAR* PersistedStatePortId =
			TEXT("modeling-geometry.independent-readback.v1");
		constexpr const TCHAR* RequestEvidencePrefix =
			TEXT("modeling-geometry-request-v1:");

		using FModelingProfile =
			FUeAgentModelingGeometryEffectProductionProfile;

		struct FModelingProfileCatalog
		{
			FModelingProfileCatalog()
			{
				TryGetModelingGeometryEffectProductionProfiles(
					Profiles,
					Error);
			}

			TArray<FModelingProfile> Profiles;
			FString Error;
		};

		const FModelingProfileCatalog& GetProfileCatalog()
		{
			static const FModelingProfileCatalog Catalog;
			return Catalog;
		}

		bool IsPrimitiveProfile(const FModelingProfile& Profile)
		{
			return Profile.bPrimitiveOutput
				&& Profile.NativeTargetKind
					== TEXT("geometry.primitive.create");
		}

		bool IsExistingEditProfile(const FModelingProfile& Profile)
		{
			return !Profile.bPrimitiveOutput
				&& (Profile.NativeTargetKind
						== TEXT("geometry.topology.edit")
					|| Profile.NativeTargetKind
						== TEXT("geometry.attribute.edit")
					|| Profile.NativeTargetKind
						== TEXT("geometry.collision.edit"));
		}

		FString ProfileCategory(const FModelingProfile& Profile)
		{
			if (IsPrimitiveProfile(Profile))
			{
				return TEXT("primitive");
			}
			if (Profile.NativeTargetKind
				== TEXT("geometry.attribute.edit"))
			{
				return TEXT("attribute");
			}
			if (Profile.NativeTargetKind
				== TEXT("geometry.collision.edit"))
			{
				return TEXT("collision");
			}
			return TEXT("topology");
		}

		const FModelingProfile* FindProfile(
			const FUeAgentDescriptorIdentity& Identity)
		{
			for (const FModelingProfile& Profile
				: GetProfileCatalog().Profiles)
			{
				if (Identity.GetDescriptorId()
						== Profile.DescriptorId
					&& Identity.GetVersion()
						== Profile.DescriptorVersion
					&& Identity.GetDescriptorHash()
						== Profile.DescriptorHash)
				{
					return &Profile;
				}
			}
			return nullptr;
		}

		void SetError(
			FUeAgentModelingSimpleAuthoringError& OutError,
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
			return FJsonSerializer::Deserialize(Reader, OutObject)
				&& OutObject.IsValid();
		}

		bool TrySerializeJson(
			const TSharedPtr<FJsonObject>& Object,
			FString& OutJson)
		{
			OutJson.Reset();
			if (!Object.IsValid())
			{
				return false;
			}
			const TSharedRef<TJsonWriter<>> Writer =
				TJsonWriterFactory<>::Create(&OutJson);
			return FJsonSerializer::Serialize(
				Object.ToSharedRef(),
				Writer);
		}

		bool TryReadPayloadRevision(
			const TSharedPtr<FJsonObject>& Payload,
			const TCHAR* Field,
			TOptional<FUeAgentStateRevision>& OutRevision)
		{
			OutRevision.Reset();
			const TSharedPtr<FJsonObject>* Revision = nullptr;
			return Payload.IsValid()
				&& Payload->TryGetObjectField(Field, Revision)
				&& Revision
				&& FUeAgentStateRevisionJson::TryParse(
					*Revision,
					OutRevision);
		}

		FString RevisionMismatchDetail(
			const FUeAgentStateRevision& Expected,
			const TOptional<FUeAgentStateRevision>& Actual)
		{
			if (!Actual.IsSet())
			{
				return FString::Printf(
					TEXT(
						"expected_source=%s;expected_generation=%s;"
						"expected_sequence=%lld;actual=missing"),
					*Expected.GetSourceId(),
					*Expected.GetGeneration(),
					Expected.GetSequence());
			}
			return FString::Printf(
				TEXT(
					"expected_source=%s;expected_generation=%s;"
					"expected_sequence=%lld;actual_source=%s;"
					"actual_generation=%s;actual_sequence=%lld"),
				*Expected.GetSourceId(),
				*Expected.GetGeneration(),
				Expected.GetSequence(),
				*Actual->GetSourceId(),
				*Actual->GetGeneration(),
				Actual->GetSequence());
		}

		bool TryParseRequestEvidence(
			const FUeAgentCandidateBinding& Binding,
			TSharedPtr<FJsonObject>& OutPayload,
			FUeAgentDiscoveryError& OutError)
		{
			const FString& Evidence = Binding.GetBindingEvidence();
			if (!Evidence.StartsWith(RequestEvidencePrefix)
				|| !TryParseJson(
					Evidence.RightChop(
						FCString::Strlen(RequestEvidencePrefix)),
					OutPayload))
			{
				OutError.Code =
					TEXT("modeling_geometry_request_evidence_invalid");
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
					OutValues.Reset();
					return false;
				}
				OutValues.Add(Number);
			}
			return true;
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
					TEXT("modeling_geometry_state_json_invalid");
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
					TArray<TPair<
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
					TEXT("modeling_geometry_state_json_kind_invalid");
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

		FUeAgentCanonicalType ModelingStateType()
		{
			return FUeAgentCanonicalType(
				TEXT("modeling_geometry_current_state"),
				EUeAgentValueKind::Object,
				false,
				TEXT("object"));
		}

		FUeAgentDefaultValue ModelingStateDefault(
			const FUeAgentCanonicalValueRef& Value)
		{
			return FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::Schema,
				TEXT("modeling-geometry:empty-default"),
				TEXT("Modeling geometry state has no implicit target"),
				Value);
		}

		FUeAgentStateDetails ModelingStateDetails(
			const FModelingProfile& Profile)
		{
			return FUeAgentStateDetails(
				Profile.CapabilityId,
				TEXT(""),
				{
					TEXT("Modeling"),
					TEXT("SimpleAuthoring"),
					ProfileCategory(Profile),
				},
				true,
				TEXT("available"),
				{});
		}

		FUeAgentCanonicalIdentity StateIdentity(
			const FUeAgentCandidateBinding& Binding,
			const FString& TargetRef,
			const FModelingProfile& Profile)
		{
			const FUeAgentCanonicalIdentity& Root =
				Binding.GetSummary().GetIdentity();
			return FUeAgentCanonicalIdentity(
				TEXT("uai.modeling.geometry:") + TargetRef,
				Profile.NativeTargetKind,
				Root.GetOwnerId(),
				TargetRef,
				TEXT("modeling_geometry"),
				Root.GetGeneration());
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
				&& Candidate.GetWorld()->GetOutermost()->GetName()
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
					? TEXT("modeling_geometry_state_catalog_unavailable")
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
						TEXT("modeling_geometry_state_world_ambiguous");
					OutError.SubjectId = Binding.GetBindingId();
					OutWorld = nullptr;
					return false;
				}
				OutWorld = Candidate.GetWorld();
			}
			if (!OutWorld)
			{
				OutError.Code =
					TEXT("modeling_geometry_state_world_unresolved");
				OutError.SubjectId = Binding.GetBindingId();
				return false;
			}
			return true;
		}

		ADynamicMeshActor* FindActorByLabel(
			UWorld& World,
			const FString& Label,
			bool& bOutAmbiguous)
		{
			bOutAmbiguous = false;
			ADynamicMeshActor* Match = nullptr;
			for (TActorIterator<ADynamicMeshActor> It(&World);
				It;
				++It)
			{
				if (It->GetActorLabel() != Label)
				{
					continue;
				}
				if (Match)
				{
					bOutAmbiguous = true;
					return nullptr;
				}
				Match = *It;
			}
			return Match;
		}

		ADynamicMeshActor* FindActorByGuid(
			UWorld& World,
			const FGuid& ActorGuid,
			bool& bOutAmbiguous)
		{
			bOutAmbiguous = false;
			ADynamicMeshActor* Match = nullptr;
			if (!ActorGuid.IsValid())
			{
				return nullptr;
			}
			for (TActorIterator<ADynamicMeshActor> It(&World); It; ++It)
			{
				if (It->GetActorGuid() != ActorGuid)
				{
					continue;
				}
				if (Match)
				{
					bOutAmbiguous = true;
					return nullptr;
				}
				Match = *It;
			}
			return Match;
		}

		bool TryReadBounds(
			ADynamicMeshActor& Actor,
			FBox& OutBounds,
			int32& OutTriangleCount)
		{
			OutBounds = FBox(EForceInit::ForceInit);
			OutTriangleCount = 0;
			UDynamicMeshComponent* Component =
				Actor.GetDynamicMeshComponent();
			UDynamicMesh* Mesh =
				Component ? Component->GetDynamicMesh() : nullptr;
			if (!Mesh)
			{
				return false;
			}
			Mesh->ProcessMesh(
				[&OutBounds, &OutTriangleCount](
					const FDynamicMesh3& DynamicMesh)
				{
					for (const int32 VertexId
						: DynamicMesh.VertexIndicesItr())
					{
						OutBounds += FVector(
							DynamicMesh.GetVertex(VertexId));
					}
					OutTriangleCount =
						DynamicMesh.TriangleCount();
				});
			return OutBounds.IsValid && OutTriangleCount > 0;
		}

		bool TryExpectedPrimitiveSize(
			const FModelingProfile& Profile,
			const TSharedPtr<FJsonObject>& Payload,
			FVector& OutSize)
		{
			OutSize = FVector::ZeroVector;
			TArray<double> Values;
			if (Profile.OperationKey == TEXT("cylinder"))
			{
				double Radius = 0.0;
				double Height = 0.0;
				if (!Payload->TryGetNumberField(
						TEXT("radius_cm"),
						Radius)
					|| !Payload->TryGetNumberField(
						TEXT("height_cm"),
						Height))
				{
					return false;
				}
				OutSize = FVector(
					Radius * 2.0,
					Radius * 2.0,
					Height);
				return true;
			}
			if (Profile.OperationKey == TEXT("plane"))
			{
				if (!TryReadNumberArray(
						Payload,
						TEXT("size_cm"),
						2,
						Values))
				{
					return false;
				}
				OutSize = FVector(Values[0], Values[1], 0.0);
				return true;
			}
			if (Profile.OperationKey == TEXT("sphere"))
			{
				double Radius = 0.0;
				if (!Payload->TryGetNumberField(
						TEXT("radius_cm"),
						Radius))
				{
					return false;
				}
				OutSize = FVector(Radius * 2.0);
				return true;
			}
			if (Profile.OperationKey == TEXT("stairs"))
			{
				double Count = 0.0;
				double Width = 0.0;
				double Depth = 0.0;
				double Height = 0.0;
				FString Axis;
				if (!Payload->TryGetNumberField(
						TEXT("step_count"),
						Count)
					|| !Payload->TryGetNumberField(
						TEXT("step_width_cm"),
						Width)
					|| !Payload->TryGetNumberField(
						TEXT("step_depth_cm"),
						Depth)
					|| !Payload->TryGetNumberField(
						TEXT("step_height_cm"),
						Height)
					|| !Payload->TryGetStringField(
						TEXT("rise_axis"),
						Axis))
				{
					return false;
				}
				OutSize = Axis == TEXT("y")
					? FVector(Width, Depth * Count, Height * Count)
					: FVector(Depth * Count, Width, Height * Count);
				return true;
			}
			if (!TryReadNumberArray(
					Payload,
					TEXT("size_cm"),
					3,
					Values))
			{
				return false;
			}
			OutSize = FVector(Values[0], Values[1], Values[2]);
			return true;
		}

		struct FPlacementComparison final
		{
			bool bLocationMatches = false;
			bool bRotationMatches = false;
			bool bScaleMatches = false;

			bool Matches() const
			{
				return bLocationMatches
					&& bRotationMatches
					&& bScaleMatches;
			}
		};

		TArray<TSharedPtr<FJsonValue>> PlacementVectorValues(
			const FVector& Value)
		{
			return {
				MakeShared<FJsonValueNumber>(Value.X),
				MakeShared<FJsonValueNumber>(Value.Y),
				MakeShared<FJsonValueNumber>(Value.Z),
			};
		}

		bool TryReadExpectedPlacement(
			const TSharedPtr<FJsonObject>& Payload,
			FVector& OutLocation,
			FQuat& OutRotation,
			FVector& OutScale)
		{
			OutLocation = FVector::ZeroVector;
			OutRotation = FQuat::Identity;
			OutScale = FVector::OneVector;
			const TSharedPtr<FJsonObject>* Placement = nullptr;
			if (!Payload->TryGetObjectField(
					TEXT("placement"),
					Placement)
				|| !Placement
				|| !Placement->IsValid())
			{
				return false;
			}
			TArray<double> Translation;
			TArray<double> Rotation;
			TArray<double> Scale;
			FString Unit;
			if (!(*Placement)->TryGetStringField(
					TEXT("unit"),
					Unit)
				|| !TryReadNumberArray(
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
					Scale))
			{
				return false;
			}
			const double UnitScale = Unit == TEXT("meter") ? 100.0 : 1.0;
			OutLocation = FVector(
				Translation[0] * UnitScale,
				Translation[1] * UnitScale,
				Translation[2] * UnitScale);
			OutRotation = FQuat(
				Rotation[0],
				Rotation[1],
				Rotation[2],
				Rotation[3]);
			if (OutRotation.SizeSquared() <= UE_DOUBLE_SMALL_NUMBER)
			{
				return false;
			}
			OutRotation.Normalize();
			OutScale = FVector(Scale[0], Scale[1], Scale[2]);
			return true;
		}

		FTransform ReadPrimitiveAuthoredTransform(
			const ADynamicMeshActor& Actor)
		{
			const USceneComponent* const RootComponent =
				Actor.GetRootComponent();
			if (RootComponent && !RootComponent->GetAttachParent())
			{
				return RootComponent->GetRelativeTransform();
			}
			return Actor.GetActorTransform();
		}

		FPlacementComparison ComparePlacement(
			const TSharedPtr<FJsonObject>& Payload,
			const ADynamicMeshActor& Actor)
		{
			FPlacementComparison Result;
			FVector ExpectedLocation;
			FQuat ExpectedRotation;
			FVector ExpectedScale;
			if (!TryReadExpectedPlacement(
					Payload,
					ExpectedLocation,
					ExpectedRotation,
					ExpectedScale))
			{
				return Result;
			}
			const FTransform AuthoredTransform =
				ReadPrimitiveAuthoredTransform(Actor);
			Result.bLocationMatches = AuthoredTransform.GetLocation().Equals(
				ExpectedLocation,
				0.01);
			Result.bRotationMatches = AuthoredTransform.GetRotation().Equals(
				ExpectedRotation,
				0.0001);
			Result.bScaleMatches = AuthoredTransform.GetScale3D().Equals(
				ExpectedScale,
				0.0001);
			return Result;
		}

		bool GeometryMatches(
			const FModelingProfile& Profile,
			const TSharedPtr<FJsonObject>& Payload,
			ADynamicMeshActor& Actor)
		{
			FVector ExpectedSize;
			FBox Bounds;
			int32 TriangleCount = 0;
			return TryExpectedPrimitiveSize(
					Profile,
					Payload,
					ExpectedSize)
				&& TryReadBounds(Actor, Bounds, TriangleCount)
				&& Bounds.GetSize().Equals(ExpectedSize, 0.5);
		}

		bool TryBuildPrimitiveState(
			const FModelingProfile& Profile,
			const TSharedPtr<FJsonObject>& Payload,
			ADynamicMeshActor& Actor,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentDiscoveryError& OutError)
		{
			FString TargetRef;
			if (!TryGetTargetRef(Payload, TargetRef))
			{
				OutError.Code =
					TEXT("modeling_geometry_target_ref_missing");
				return false;
			}
			const TSharedPtr<FJsonObject> State =
				MakeShared<FJsonObject>();
			const FPlacementComparison Placement = ComparePlacement(
				Payload,
				Actor);
			State->SetStringField(TEXT("target_ref"), TargetRef);
			State->SetStringField(
				TEXT("carrier_kind"),
				TEXT("dynamic_mesh_actor"));
			State->SetBoolField(
				TEXT("placement_matches"),
				Placement.Matches());
			State->SetBoolField(
				TEXT("placement_location_matches"),
				Placement.bLocationMatches);
			State->SetBoolField(
				TEXT("placement_rotation_matches"),
				Placement.bRotationMatches);
			State->SetBoolField(
				TEXT("placement_scale_matches"),
				Placement.bScaleMatches);
			const FTransform AuthoredTransform =
				ReadPrimitiveAuthoredTransform(Actor);
			State->SetArrayField(
				TEXT("placement_location_cm"),
				PlacementVectorValues(AuthoredTransform.GetLocation()));
			State->SetBoolField(
				TEXT("geometry_matches"),
				GeometryMatches(Profile, Payload, Actor));
			FUeAgentStateSemanticError StateError;
			if (!TryCanonicalObject(
					State,
					Resources,
					OutValue,
					StateError))
			{
				OutError.Code = StateError.Code;
				OutError.SubjectId = TargetRef;
				OutError.Detail = StateError.Detail;
				return false;
			}
			return true;
		}

		bool TryBuildExpectedPrimitiveState(
			const FString& TargetRef,
			const TSharedPtr<FJsonObject>& Payload,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentSimpleAuthoringProductionError& OutError)
		{
			const TSharedPtr<FJsonObject> State =
				MakeShared<FJsonObject>();
			FVector ExpectedLocation;
			FQuat ExpectedRotation;
			FVector ExpectedScale;
			if (!TryReadExpectedPlacement(
					Payload,
					ExpectedLocation,
					ExpectedRotation,
					ExpectedScale))
			{
				SetProductionError(
					OutError,
					TEXT("modeling_geometry_placement_invalid"),
					TargetRef);
				return false;
			}
			State->SetStringField(TEXT("target_ref"), TargetRef);
			State->SetStringField(
				TEXT("carrier_kind"),
				TEXT("dynamic_mesh_actor"));
			State->SetBoolField(
				TEXT("placement_matches"),
				true);
			State->SetBoolField(
				TEXT("placement_location_matches"),
				true);
			State->SetBoolField(
				TEXT("placement_rotation_matches"),
				true);
			State->SetBoolField(
				TEXT("placement_scale_matches"),
				true);
			State->SetArrayField(
				TEXT("placement_location_cm"),
				PlacementVectorValues(ExpectedLocation));
			State->SetBoolField(
				TEXT("geometry_matches"),
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
					TargetRef,
					StateError.Detail);
				return false;
			}
			return true;
		}

		class FModelingTargetResolver final
			: public IUeAgentSimpleAuthoringTargetResolver
		{
		public:
			explicit FModelingTargetResolver(
				TSharedRef<
					FUeAgentWorldSimpleEffectExactWorldResolver,
					ESPMode::ThreadSafe> InWorldResolver,
				TSharedRef<
					IUeAgentModelingGeometryEffectReferenceResolver,
					ESPMode::ThreadSafe> InReferenceResolver)
				: WorldResolver(MoveTemp(InWorldResolver))
				, ReferenceResolver(MoveTemp(InReferenceResolver))
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
				const FModelingProfile* Profile =
					FindProfile(
						CanonicalRequest.GetDescriptorIdentity());
				TSharedPtr<FJsonObject> Payload;
				FString TargetRef;
				if (!Profile
					|| !TryParseJson(
						CanonicalRequest.GetPayload()
							.GetJsonValue(),
						Payload)
					|| !TryGetTargetRef(Payload, TargetRef))
				{
					SetProductionError(
						OutError,
						TEXT("modeling_geometry_profile_missing"),
						CanonicalRequest.GetDescriptorIdentity()
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
								"modeling_geometry_target_unresolved")
							: Error.GetCode(),
						Context.GetWorldIdentity(),
						Error.GetDetail());
					return false;
				}
				FString EvidenceJson =
					CanonicalRequest.GetPayload().GetJsonValue();
				if (IsExistingEditProfile(*Profile))
				{
					TOptional<FUeAgentStateRevision> ExpectedRevision;
					ExistingEdit::FCarrierState Baseline;
					FString ErrorCode;
					FString ErrorDetail;
					if (!TryReadPayloadRevision(
							Payload,
							TEXT("target_revision"),
							ExpectedRevision))
					{
						SetProductionError(
							OutError,
							TEXT(
								"modeling_geometry_target_revision_missing"),
							TargetRef);
						return false;
					}
					if (!ExistingEdit::TryReadCarrier(
							*World,
							*ReferenceResolver,
							TargetRef,
							Baseline,
							ErrorCode,
							ErrorDetail))
					{
						SetProductionError(
							OutError,
							ErrorCode,
							TargetRef,
							ErrorDetail);
						return false;
					}
					if (!Baseline.TargetRevision.IsSet()
						|| Baseline.TargetRevision.GetValue()
							!= ExpectedRevision.GetValue())
					{
						SetProductionError(
							OutError,
							TEXT(
								"modeling_geometry_target_revision_stale"),
							TargetRef,
							RevisionMismatchDetail(
								ExpectedRevision.GetValue(),
								Baseline.TargetRevision));
						return false;
					}
					if (!ExistingEdit::TryAttachBaselineEvidence(
							*World,
							*ReferenceResolver,
							Profile->OperationKey,
							Baseline,
							Payload,
							ErrorCode,
							ErrorDetail))
					{
						SetProductionError(
							OutError,
							ErrorCode,
							TargetRef,
							ErrorDetail);
						return false;
					}
					if (!TrySerializeJson(Payload, EvidenceJson))
					{
						SetProductionError(
							OutError,
							TEXT(
								"modeling_geometry_baseline_evidence_invalid"),
							TargetRef);
						return false;
					}
				}
				const FUeAgentCanonicalIdentity Identity(
					TEXT("uai.modeling.operation:") + TargetRef,
					TEXT("modeling.geometry.operation"),
					Context.GetWorldIdentity(),
					TargetRef,
					TEXT("modeling_geometry"),
					Context.GetContextRevision());
				OutTarget.Emplace(
					TEXT("modeling-geometry:")
						+ CanonicalRequest.GetRequestId(),
					FUeAgentCandidateSummary(
						Identity,
						CanonicalRequest.GetDescriptorIdentity(),
						TEXT("modeling_geometry"),
						TargetRef,
						TEXT(
							"exact Modeling geometry operation target"),
						TArray<FString>{
							TEXT("Modeling"),
							TEXT("SimpleAuthoring"),
						}),
					FUeAgentCatalogRevision(
						TEXT("modeling-geometry-exact-context"),
						Context.GetContextRevision(),
						World->GetUniqueID()),
					World->GetOutermost()->GetName(),
					PersistedStatePortId,
					TArray<EUeAgentCandidateOperation>{
						IsPrimitiveProfile(*Profile)
							? EUeAgentCandidateOperation::Create
							: EUeAgentCandidateOperation::Update,
					},
					FString(RequestEvidencePrefix)
						+ EvidenceJson);
				if (!OutTarget->IsValid())
				{
					SetProductionError(
						OutError,
						TEXT(
							"modeling_geometry_target_binding_invalid"),
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
			TSharedRef<
				IUeAgentModelingGeometryEffectReferenceResolver,
				ESPMode::ThreadSafe> ReferenceResolver;
		};

		class FModelingStateReadPort final
			: public IUeAgentStateReadPort
		{
		public:
			FModelingStateReadPort(
				FString InPortId,
				const EUeAgentStatePlane InRequiredPlane,
				TSharedRef<
					IUeAgentWorldSimpleEffectWorldCatalog,
					ESPMode::ThreadSafe> InCatalog,
				TSharedRef<
					IUeAgentModelingGeometryEffectReferenceResolver,
					ESPMode::ThreadSafe> InReferenceResolver,
				const bool bInRequireSourceInstance,
				FString InEvidenceId = FString())
				: PortId(MoveTemp(InPortId))
				, RequiredPlane(InRequiredPlane)
				, Catalog(MoveTemp(InCatalog))
				, ReferenceResolver(
					MoveTemp(InReferenceResolver))
				, bRequireSourceInstance(
					bInRequireSourceInstance)
				, EvidenceId(MoveTemp(InEvidenceId))
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
				const FModelingProfile* Profile =
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
						TEXT("modeling_geometry_state_request_invalid");
					return false;
				}
				if (!Deadline.CanContinue()
					|| Cancellation.IsCancelled())
				{
					OutError.Code =
						TEXT(
							"modeling_geometry_state_request_cancelled");
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
					|| !TryGetTargetRef(Payload, TargetRef))
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
				auto AppendState =
					[&](
						const FString& Ref,
						const TSharedPtr<FJsonObject>& State,
						const FUeAgentCanonicalIdentity& Identity)
					{
						FUeAgentCanonicalValuePtr Value;
						if (!TryCanonicalObject(
								State,
								Resources,
								Value,
								StateError))
						{
							OutError.Code = StateError.Code;
							OutError.SubjectId = Ref;
							OutError.Detail = StateError.Detail;
							return false;
						}
						Entries.Emplace(
							Identity,
							ModelingStateType(),
							Value.ToSharedRef(),
							ModelingStateDefault(
								DefaultValue.ToSharedRef()),
							ModelingStateDetails(*Profile),
							Revision,
							EUeAgentAuthoredPresence::Present,
							EUeAgentOverrideState::Overridden);
						return true;
					};

				if (IsPrimitiveProfile(*Profile))
				{
					bool bAmbiguous = false;
					ADynamicMeshActor* Actor =
						FindActorByLabel(
							*World,
							TargetRef,
							bAmbiguous);
					if (RequiredPlane
						== EUeAgentStatePlane::AuthoringMemory)
					{
						if (Actor || bAmbiguous)
						{
							OutError.Code =
								TEXT(
									"modeling_geometry_output_already_exists");
							OutError.SubjectId = TargetRef;
							return false;
						}
					}
					else if (!Actor || bAmbiguous)
					{
						OutError.Code = bAmbiguous
							? TEXT(
								"modeling_geometry_created_target_ambiguous")
							: TEXT(
								"modeling_geometry_created_target_missing");
						OutError.SubjectId = TargetRef;
						return false;
					}
					if (Actor)
					{
						FUeAgentCanonicalValuePtr Value;
						if (!TryBuildPrimitiveState(
								*Profile,
								Payload,
								*Actor,
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
							== TEXT("modeling.geometry.operation");
						const FUeAgentCanonicalIdentity Identity =
							bAggregate
								? StateIdentity(
									Binding,
									TargetRef,
									*Profile)
								: Binding.GetSummary().GetIdentity();
						Entries.Emplace(
							Identity,
							ModelingStateType(),
							Value.ToSharedRef(),
							ModelingStateDefault(
								DefaultValue.ToSharedRef()),
							ModelingStateDetails(*Profile),
							Revision,
							EUeAgentAuthoredPresence::Present,
							EUeAgentOverrideState::Overridden);
					}
				}
				else if (RequiredPlane
					== EUeAgentStatePlane::AuthoringMemory)
				{
					ExistingEdit::FCarrierState TargetState;
					FString ErrorCode;
					FString ErrorDetail;
					if (!ExistingEdit::TryReadCarrier(
							*World,
							*ReferenceResolver,
							TargetRef,
							TargetState,
							ErrorCode,
							ErrorDetail)
						|| !AppendState(
							TargetRef,
							ExistingEdit::BuildDraftState(TargetState),
							StateIdentity(
								Binding,
								TargetRef,
								*Profile)))
					{
						if (OutError.Code.IsEmpty())
						{
							OutError.Code = ErrorCode.IsEmpty()
								? TEXT(
									"modeling_geometry_target_state_invalid")
								: ErrorCode;
							OutError.SubjectId = TargetRef;
							OutError.Detail = ErrorDetail;
						}
						return false;
					}

					FString OperandRef;
					if (Payload->TryGetStringField(
							TEXT("operand_ref"),
							OperandRef))
					{
						ExistingEdit::FCarrierState OperandState;
						if (!ExistingEdit::TryReadCarrier(
								*World,
								*ReferenceResolver,
								OperandRef,
								OperandState,
								ErrorCode,
								ErrorDetail)
							|| !AppendState(
								OperandRef,
								ExistingEdit::BuildDraftState(
									OperandState),
								StateIdentity(
									Binding,
									OperandRef,
									*Profile)))
						{
							if (OutError.Code.IsEmpty())
							{
								OutError.Code =
									ErrorCode.IsEmpty()
									? TEXT(
										"modeling_geometry_operand_state_invalid")
									: ErrorCode;
								OutError.SubjectId = OperandRef;
								OutError.Detail = ErrorDetail;
							}
							return false;
						}
					}
				}
				else
				{
					ExistingEdit::FCarrierState Baseline;
					ExistingEdit::FCarrierState Actual;
					FString ErrorCode;
					FString ErrorDetail;
					if (!ExistingEdit::TryReadBaseline(
							Payload,
							Baseline)
						|| !ExistingEdit::TryReadCarrier(
							*World,
							*ReferenceResolver,
							TargetRef,
							Actual,
							ErrorCode,
							ErrorDetail)
						|| !AppendState(
							TargetRef,
							ExistingEdit::BuildActualVerificationState(
								Profile->OperationKey,
								Payload,
								Baseline,
								Actual),
							Binding.GetSummary().GetIdentity()))
					{
						if (OutError.Code.IsEmpty())
						{
							OutError.Code = ErrorCode.IsEmpty()
								? TEXT(
									"modeling_geometry_topology_readback_invalid")
								: ErrorCode;
							OutError.SubjectId = TargetRef;
							OutError.Detail = ErrorDetail;
						}
						return false;
					}
				}
				FUeAgentCanonicalSnapshotPtr Snapshot;
				if (!FUeAgentCanonicalSnapshot::TryCreate(
						TEXT("snapshot:modeling-geometry:")
							+ PortId
							+ TEXT(":")
							+ Binding.GetBindingId(),
						RequiredPlane,
						Revision,
						FUeAgentTemporalPoint(
							TEXT("modeling-geometry-operation"),
							TEXT("1"),
							TEXT("1"),
							TEXT("1"),
							TEXT("observed")),
						FUeAgentStateProvenance(
							TEXT("independent_readback"),
							PortId,
							EvidenceId.IsEmpty()
								? TargetRef
								: EvidenceId),
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
				IUeAgentModelingGeometryEffectReferenceResolver,
				ESPMode::ThreadSafe> ReferenceResolver;
			bool bRequireSourceInstance = true;
			FString EvidenceId;
		};

		class FSnapshotModelingReferenceResolver final
			: public IUeAgentModelingGeometryEffectReferenceResolver
		{
		public:
			FSnapshotModelingReferenceResolver(
				FString InSourceRef,
				UObject& InSnapshotObject)
				: SourceRef(MoveTemp(InSourceRef))
				, SnapshotObject(&InSnapshotObject)
			{
			}

			bool TryResolveReference(
				UWorld& World,
				const FString& CanonicalRef,
				UObject*& OutObject,
				FExecutionMutationError& OutError) override
			{
				OutObject = SnapshotObject.Get();
				OutError = {};
				UWorld* OwningWorld = OutObject
					? OutObject->GetTypedOuter<UWorld>()
					: nullptr;
				if (CanonicalRef != SourceRef
					|| !IsValid(OutObject)
					|| (OwningWorld && OwningWorld != &World))
				{
					OutObject = nullptr;
					OutError = FExecutionMutationError(
						TEXT("modeling_geometry_persisted_reference_unresolved"),
						CanonicalRef);
					return false;
				}
				return true;
			}

		private:
			FString SourceRef;
			TWeakObjectPtr<UObject> SnapshotObject;
		};

		class FModelingPersistedWorldCatalog final
			: public IUeAgentWorldSimpleEffectWorldCatalog
		{
		public:
			FModelingPersistedWorldCatalog(
				UWorld& InWorld,
				FString InWorldIdentity,
				FString InContextRevision)
				: World(&InWorld)
				, WorldIdentity(MoveTemp(InWorldIdentity))
				, ContextRevision(MoveTemp(InContextRevision))
			{
			}

			bool TryEnumerate(
				TArray<FUeAgentWorldSimpleEffectWorldCandidate>&
					OutCandidates,
				FExecutionMutationError& OutError) override
			{
				OutCandidates.Reset();
				OutError = {};
				if (!World.IsValid()
					|| !World->PersistentLevel
					|| WorldIdentity.IsEmpty()
					|| ContextRevision.IsEmpty())
				{
					OutError = FExecutionMutationError(
						TEXT(
							"modeling_geometry_persisted_world_catalog_invalid"));
					return false;
				}
				OutCandidates.Emplace(
					World.Get(),
					WorldIdentity,
					World->PersistentLevel->GetPathName(),
					EUeAgentExecutionWorldType::Preview,
					TOptional<int32>(),
					ContextRevision);
				return OutCandidates.Last().IsValid();
			}

		private:
			TWeakObjectPtr<UWorld> World;
			FString WorldIdentity;
			FString ContextRevision;
		};

		class FModelingArtifactPersistedStateOracle final
			: public IPersistedStateOracle
		{
		public:
			FModelingArtifactPersistedStateOracle(
				TSharedRef<
					IUeAgentWorldSimpleEffectWorldCatalog,
					ESPMode::ThreadSafe> InCatalog)
				: Catalog(MoveTemp(InCatalog))
			{
			}

			const FString& GetReadPortId() const override
			{
				static const FString Id(PersistedStatePortId);
				return Id;
			}

			bool TryObserve(
				const FVerificationTarget& Target,
				const TArray<FDurableArtifact>& Artifacts,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				FUeAgentCanonicalSnapshotPtr& OutSnapshot,
				FVerificationError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				const FModelingProfile* Profile = Target.IsValid()
					? FindProfile(
						Target.GetBinding()
							.GetSummary()
							.GetDescriptorIdentity())
					: nullptr;
				TSharedPtr<FJsonObject> Payload;
				FString TargetRef;
				FUeAgentDiscoveryError EvidenceError;
				if (!Target.IsValid()
					|| Artifacts.IsEmpty()
					|| Target.GetBinding().GetReadPortId()
						!= PersistedStatePortId
					|| !Profile
					|| !Deadline.CanContinue()
					|| Cancellation.IsCancelled()
					|| !TryParseRequestEvidence(
						Target.GetBinding(),
						Payload,
						EvidenceError)
					|| !TryGetTargetRef(Payload, TargetRef))
				{
					OutError = FVerificationError(
						EvidenceError.Code.IsEmpty()
							? TEXT(
								"modeling_geometry_persisted_read_request_invalid")
							: EvidenceError.Code,
						EvidenceError.Detail);
					return false;
				}

				if (IsPrimitiveProfile(*Profile))
				{
					return TryObservePersistedWorldCarrier(
						Target,
						Artifacts,
						Resources,
						Deadline,
						Cancellation,
						TargetRef,
						nullptr,
						OutSnapshot,
						OutError);
				}

				ExistingEdit::FCarrierState Baseline;
				if (!IsExistingEditProfile(*Profile)
					|| !ExistingEdit::TryReadBaseline(Payload, Baseline))
				{
					OutError = FVerificationError(
						TEXT(
							"modeling_geometry_persisted_baseline_invalid"),
						TargetRef);
					return false;
				}
				if (Baseline.CarrierKind == TEXT("dynamic_mesh_actor"))
				{
					return TryObservePersistedWorldCarrier(
						Target,
						Artifacts,
						Resources,
						Deadline,
						Cancellation,
						TargetRef,
						&Baseline,
						OutSnapshot,
						OutError);
				}
				if (Baseline.CarrierKind != TEXT("static_mesh_asset"))
				{
					OutError = FVerificationError(
						TEXT(
							"modeling_geometry_persisted_carrier_unsupported"),
						Baseline.CarrierKind);
					return false;
				}

				FPersistedPackageLoadError LoadError;
				const bool bObserved = TryObservePersistedPackage(
					Artifacts,
					Baseline.DurablePackageName,
					[&](
						UPackage& SnapshotPackage,
						FPersistedPackageLoadError& ObserverError)
					{
						const FString ObjectName =
							FPackageName::ObjectPathToObjectName(
								Baseline.TargetRef);
						UStaticMesh* SnapshotMesh = Cast<UStaticMesh>(
							StaticFindObjectFast(
								UStaticMesh::StaticClass(),
								&SnapshotPackage,
								FName(*ObjectName)));
						if (!SnapshotMesh)
						{
							ObserverError.Code =
								TEXT("modeling_geometry_persisted_asset_missing");
							ObserverError.Detail = Baseline.TargetRef;
							return false;
						}
						const TSharedRef<
							FSnapshotModelingReferenceResolver,
							ESPMode::ThreadSafe> Resolver = MakeShared<
								FSnapshotModelingReferenceResolver,
								ESPMode::ThreadSafe>(
							Baseline.TargetRef,
							*SnapshotMesh);
						const FString EvidenceId =
							TEXT("modeling:persisted-package-instance:")
							+ SnapshotPackage.GetName()
							+ TEXT(":")
							+ Baseline.TargetRef;
						FModelingStateReadPort Reader(
							PersistedStatePortId,
							EUeAgentStatePlane::Persisted,
							Catalog,
							Resolver,
							false,
							EvidenceId);
						FUeAgentDiscoveryError ReadError;
						if (!Reader.TryRead(
								FUeAgentStateReadRequest(
									Target.GetBinding(),
									EUeAgentStatePlane::Persisted),
								Resources,
								Deadline,
								Cancellation,
								OutSnapshot,
								ReadError))
						{
							ObserverError.Code = ReadError.Code.IsEmpty()
								? TEXT("modeling_geometry_persisted_state_read_failed")
								: ReadError.Code;
							ObserverError.Detail = ReadError.Detail;
							return false;
						}
						return OutSnapshot.IsValid();
					},
					LoadError);
				if (!bObserved)
				{
					OutError = FVerificationError(
						LoadError.Code.IsEmpty()
							? TEXT("modeling_geometry_persisted_package_unavailable")
							: LoadError.Code,
						LoadError.Detail);
					return false;
				}
				return OutSnapshot.IsValid();
			}

		private:
			bool TryObservePersistedWorldCarrier(
				const FVerificationTarget& Target,
				const TArray<FDurableArtifact>& Artifacts,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				const FString& TargetRef,
				const ExistingEdit::FCarrierState* Baseline,
				FUeAgentCanonicalSnapshotPtr& OutSnapshot,
				FVerificationError& OutError) const
			{
				FPersistedWorldLoadError LoadError;
				const bool bObserved = TryObservePersistedWorld(
					Artifacts,
					[&](
						UWorld& SnapshotWorld,
						FPersistedWorldLoadError& ObserverError)
					{
						if (!Deadline.CanContinue()
							|| Cancellation.IsCancelled())
						{
							ObserverError.Code = TEXT(
								"modeling_geometry_persisted_world_read_cancelled");
							return false;
						}
						bool bAmbiguous = false;
						ADynamicMeshActor* SnapshotActor = Baseline
							? FindActorByGuid(
								SnapshotWorld,
								Baseline->ActorGuid,
								bAmbiguous)
							: FindActorByLabel(
								SnapshotWorld,
								TargetRef,
								bAmbiguous);
						if (!SnapshotActor || bAmbiguous)
						{
							ObserverError.Code = bAmbiguous
								? TEXT(
									"modeling_geometry_persisted_actor_ambiguous")
								: TEXT(
									"modeling_geometry_persisted_actor_missing");
							ObserverError.Detail = TargetRef;
							return false;
						}

						const FUeAgentCandidateBinding& SourceBinding =
							Target.GetBinding();
						const FUeAgentCanonicalIdentity& SourceIdentity =
							SourceBinding.GetSummary().GetIdentity();
						const FString SnapshotWorldPackage =
							SnapshotWorld.GetOutermost()->GetName();
						const FString SnapshotToken =
							FGuid::NewGuid().ToString(EGuidFormats::Digits);
						const FUeAgentCandidateBinding SnapshotBinding(
							SourceBinding.GetBindingId()
								+ TEXT(":artifact:")
								+ SnapshotToken,
							SourceBinding.GetSummary(),
							SourceBinding.GetCatalogRevision(),
							SnapshotWorldPackage,
							PersistedStatePortId,
							SourceBinding.GetSupportedOperations(),
							SourceBinding.GetBindingEvidence());
						if (!SnapshotBinding.IsValid())
						{
							ObserverError.Code = TEXT(
								"modeling_geometry_persisted_binding_invalid");
							return false;
						}
						const TSharedRef<
							FModelingPersistedWorldCatalog,
							ESPMode::ThreadSafe> SnapshotCatalog = MakeShared<
								FModelingPersistedWorldCatalog,
								ESPMode::ThreadSafe>(
							SnapshotWorld,
							SourceIdentity.GetOwnerId(),
							SourceIdentity.GetGeneration());
						const TSharedRef<
							FSnapshotModelingReferenceResolver,
							ESPMode::ThreadSafe> Resolver = MakeShared<
								FSnapshotModelingReferenceResolver,
								ESPMode::ThreadSafe>(
							TargetRef,
							*SnapshotActor);
						const FString EvidenceId =
							TEXT("modeling:persisted-world-instance:")
							+ SnapshotWorldPackage
							+ TEXT(":")
							+ TargetRef;
						FModelingStateReadPort Reader(
							PersistedStatePortId,
							EUeAgentStatePlane::Persisted,
							SnapshotCatalog,
							Resolver,
							false,
							EvidenceId);
						FUeAgentDiscoveryError ReadError;
						if (!Reader.TryRead(
								FUeAgentStateReadRequest(
									SnapshotBinding,
									EUeAgentStatePlane::Persisted),
								Resources,
								Deadline,
								Cancellation,
								OutSnapshot,
								ReadError))
						{
							ObserverError.Code = ReadError.Code.IsEmpty()
								? TEXT(
									"modeling_geometry_persisted_world_state_read_failed")
								: ReadError.Code;
							ObserverError.Detail = ReadError.Detail;
							return false;
						}
						return OutSnapshot.IsValid();
					},
					LoadError);
				if (!bObserved)
				{
					OutError = FVerificationError(
						LoadError.Code.IsEmpty()
							? TEXT(
								"modeling_geometry_persisted_world_unavailable")
							: LoadError.Code,
						LoadError.Detail);
					return false;
				}
				return OutSnapshot.IsValid();
			}

			TSharedRef<
				IUeAgentWorldSimpleEffectWorldCatalog,
				ESPMode::ThreadSafe> Catalog;
		};

		class FModelingDraftBuilder final
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
				if (!TryBuildModelingGeometryEffectChangeSetDraft(
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

		class FModelingChangeTargetProjector final
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
				const FModelingProfile* Profile =
					FindProfile(
						CanonicalRequest.GetDescriptorIdentity());
				if (!Profile)
				{
					SetProductionError(
						OutError,
						TEXT(
							"modeling_geometry_change_profile_missing"),
						CanonicalRequest.GetDescriptorIdentity()
							.GetDescriptorId());
					return false;
				}
				const TArray<FChangeDraft>& Changes =
					Draft.GetChanges();
				for (int32 Index = 0;
					Index < Changes.Num();
					++Index)
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
							TEXT("modeling_geometry"),
							Change.GetIdentity().GetSelector(),
							TEXT(
								"exact Modeling geometry change target"),
							TArray<FString>{
								TEXT("Modeling"),
								TEXT("SimpleAuthoring"),
							}),
						ResolvedTarget.GetCatalogRevision(),
						ResolvedTarget.GetPublicRef(),
						ResolvedTarget.GetReadPortId(),
						TArray<EUeAgentCandidateOperation>{
							IsPrimitiveProfile(*Profile)
								? EUeAgentCandidateOperation::Create
								: EUeAgentCandidateOperation::Update,
						},
						ResolvedTarget.GetBindingEvidence());
					if (!OutTargets.Last().IsValid())
					{
						SetProductionError(
							OutError,
							TEXT(
								"modeling_geometry_change_target_invalid"),
							Change.GetIdentity()
								.GetCanonicalKey());
						OutTargets.Reset();
						return false;
					}
				}
				return !OutTargets.IsEmpty();
			}
		};

		class FModelingMutationBindingProjector final
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
							"modeling_geometry_change_targets_missing"),
						Plan->GetRequestId());
					return false;
				}
				for (int32 Index = 0;
					Index < Changes.Num();
					++Index)
				{
					OutBindings.Emplace(
						Targets[Index],
						TEXT(
							"UAI.ModelingGeometry:CanonicalObject"),
						FUeAgentCanonicalType(
							TEXT(
								"modeling_geometry_canonical_object"),
							EUeAgentValueKind::Object,
							false,
							TEXT("object")),
						TEXT(
							"modeling-geometry:typed-native-binding"));
					if (Targets[Index].GetSummary().GetIdentity()
							!= Changes[Index].GetIdentity()
						|| !OutBindings.Last().IsValid())
					{
						SetProductionError(
							OutError,
							TEXT(
								"modeling_geometry_mutation_binding_invalid"),
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

		class FModelingDurableTargetProjector final
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
				const FModelingProfile* Profile =
					FindProfile(Plan->GetDescriptorIdentity());
				if (Changes.IsEmpty()
					|| !Profile
					|| Target.GetPublicRef().IsEmpty())
				{
					SetProductionError(
						OutError,
						TEXT(
							"modeling_geometry_durable_target_missing"),
						Target.GetBindingId());
					return false;
				}
				FString DurablePackageName = Target.GetPublicRef();
				if (IsExistingEditProfile(*Profile))
				{
					TSharedPtr<FJsonObject> Payload;
					FUeAgentDiscoveryError EvidenceError;
					if (!TryParseRequestEvidence(
							Target,
							Payload,
							EvidenceError)
						|| !ExistingEdit::
							TryReadDurablePackageNameEvidence(
								Payload,
								DurablePackageName))
					{
						SetProductionError(
							OutError,
							TEXT(
								"modeling_geometry_durable_target_evidence_invalid"),
							Target.GetBindingId(),
							EvidenceError.Detail);
						return false;
					}
				}
				OutTargets.Emplace(
					Changes[0].GetIdentity(),
					MoveTemp(DurablePackageName),
					IsPrimitiveProfile(*Profile)
						? EPackageHeaderKind::Map
						: EPackageHeaderKind::EvidenceRequired,
					EAssetDurableIntent::Save,
					ERollbackGrade::DurableBackup);
				return OutTargets[0].IsValid();
			}
		};

		class FModelingExpectedStateProjector final
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
				const FModelingProfile* Profile =
					FindProfile(Plan->GetDescriptorIdentity());
				TSharedPtr<FJsonObject> Payload;
				FString TargetRef;
				FUeAgentDiscoveryError EvidenceError;
				if (!Profile
					|| !TryParseRequestEvidence(
						ResolvedTarget,
						Payload,
						EvidenceError)
					|| !TryGetTargetRef(Payload, TargetRef))
				{
					SetProductionError(
						OutError,
						EvidenceError.Code.IsEmpty()
							? TEXT(
								"modeling_geometry_expected_profile_missing")
							: EvidenceError.Code,
						Plan->GetDescriptorIdentity()
							.GetDescriptorId(),
						EvidenceError.Detail);
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
					TEXT("modeling-geometry-operation-expected"),
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
								"modeling_geometry_verification_target_missing"),
							Change.GetIdentity()
								.GetCanonicalKey());
						return false;
					}
					FUeAgentCanonicalValuePtr ExpectedValue;
					if (IsPrimitiveProfile(*Profile))
					{
						if (!TryBuildExpectedPrimitiveState(
								TargetRef,
								Payload,
								Resources,
								ExpectedValue,
								OutError))
						{
							return false;
						}
					}
					else
					{
						ExistingEdit::FCarrierState Baseline;
						if (!ExistingEdit::TryReadBaseline(
								Payload,
								Baseline)
							|| !TryCanonicalObject(
								ExistingEdit::
									BuildExpectedVerificationState(
										Profile->OperationKey,
										Baseline),
								Resources,
								ExpectedValue,
								StateError))
						{
							SetProductionError(
								OutError,
								StateError.Code.IsEmpty()
									? TEXT(
										"modeling_geometry_topology_expected_invalid")
									: StateError.Code,
								TargetRef,
								StateError.Detail);
							return false;
						}
					}
					TArray<FUeAgentStateEntry> Entries;
					Entries.Emplace(
						Change.GetIdentity(),
						ModelingStateType(),
						ExpectedValue.ToSharedRef(),
						ModelingStateDefault(
							DefaultValue.ToSharedRef()),
						ModelingStateDetails(*Profile),
						Revision,
						EUeAgentAuthoredPresence::Present,
						EUeAgentOverrideState::Overridden);
					FUeAgentCanonicalSnapshotPtr Expected;
					if (!FUeAgentCanonicalSnapshot::
							TryCreateExpected(
								TEXT(
									"snapshot:modeling-geometry:"
									"expected:")
									+ Plan->GetRequestId()
									+ TEXT(":")
									+ Change.GetIdentity()
										.GetCanonicalKey(),
								EUeAgentStatePlane::Persisted,
								Revision,
								FUeAgentTemporalPoint(
									TEXT(
										"modeling-geometry-operation"),
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
								"modeling_geometry_verification_target_invalid"),
							Change.GetIdentity()
								.GetCanonicalKey());
						return false;
					}
				}
				return !OutTargets.IsEmpty();
			}
		};

		class FModelingUeHook final
			: public IUeAgentGenericEffectUeHook
		{
		public:
			FModelingUeHook(
				TSharedRef<
					FUeAgentWorldSimpleEffectExactWorldResolver,
					ESPMode::ThreadSafe> InWorldResolver,
				TSharedRef<
					IUeAgentModelingGeometryEffectNativeHook,
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
				IUeAgentModelingGeometryEffectNativeHook,
				ESPMode::ThreadSafe> NativeHook;
		};

		bool ValidateConfig(
			const FUeAgentModelingSimpleAuthoringProviderConfig&
				Config,
			FUeAgentModelingSimpleAuthoringError& OutError)
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
						"modeling_simple_authoring_dependencies_missing"));
				return false;
			}
			const FModelingProfileCatalog& Catalog =
				GetProfileCatalog();
			if (!Catalog.Error.IsEmpty()
				|| Catalog.Profiles.Num() != 31)
			{
				SetError(
					OutError,
					TEXT(
						"modeling_simple_authoring_profile_catalog_invalid"),
					TEXT("mesh"),
					Catalog.Error);
				return false;
			}
			int32 PrimitiveCount = 0;
			int32 TopologyCount = 0;
			int32 AttributeCount = 0;
			int32 CollisionCount = 0;
			for (const FModelingProfile& Profile : Catalog.Profiles)
			{
				PrimitiveCount += IsPrimitiveProfile(Profile) ? 1 : 0;
				TopologyCount +=
					Profile.NativeTargetKind
							== TEXT("geometry.topology.edit")
						? 1
						: 0;
				AttributeCount +=
					Profile.NativeTargetKind
							== TEXT("geometry.attribute.edit")
						? 1
						: 0;
				CollisionCount +=
					Profile.NativeTargetKind
							== TEXT("geometry.collision.edit")
						? 1
						: 0;
				const FUeAgentOperationDescriptor* Descriptor =
					Config.DescriptorRegistry
						->FindByDescriptorId(Profile.DescriptorId);
				if (!Descriptor
					|| Descriptor->GetCapabilityId()
						!= Profile.CapabilityId
					|| Descriptor->GetDomainId()
						!= TEXT("mesh")
					|| Descriptor->GetIdentity().GetVersion()
						!= Profile.DescriptorVersion
					|| Descriptor->GetIdentity().GetDescriptorHash()
						!= Profile.DescriptorHash)
				{
					SetError(
						OutError,
						TEXT(
							"modeling_simple_authoring_descriptor_missing"),
						Profile.DescriptorId);
					return false;
				}
			}
			if (PrimitiveCount != 7
				|| TopologyCount != 17
				|| AttributeCount != 5
				|| CollisionCount != 2)
			{
				SetError(
					OutError,
					TEXT(
						"modeling_simple_authoring_profile_distribution_invalid"),
					TEXT("mesh"));
				return false;
			}
			return true;
		}
	}

	bool FUeAgentModelingSimpleAuthoringProviderFactory::TryCreate(
		const FUeAgentModelingSimpleAuthoringProviderConfig& Config,
		TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider>&
			OutProvider,
		FUeAgentModelingSimpleAuthoringError& OutError)
	{
		OutProvider.Reset();
		OutError = FUeAgentModelingSimpleAuthoringError();
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
			IUeAgentModelingGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe> ReferenceResolver =
				Config.ReferenceResolver.ToSharedRef();
		const TSharedRef<
			FUeAgentWorldSimpleEffectExactWorldResolver,
			ESPMode::ThreadSafe> WorldResolver =
				MakeShared<
					FUeAgentWorldSimpleEffectExactWorldResolver,
					ESPMode::ThreadSafe>(Catalog);
		TSharedPtr<IUeAgentModelingGeometryEffectNativeHook>
			NativeHook;
		FExecutionMutationError NativeError;
		if (!FUeAgentModelingGeometryEffectNativeHookFactory::
				TryCreate(
					ReferenceResolver,
					NativeHook,
					NativeError)
			|| !NativeHook.IsValid())
		{
			SetError(
				OutError,
				NativeError.GetCode().IsEmpty()
					? TEXT("modeling_geometry_native_hook_missing")
					: NativeError.GetCode(),
				TEXT("mesh"),
				NativeError.GetDetail());
			return false;
		}

		FUeAgentSimpleAuthoringProductionProviderConfig ProviderConfig;
		ProviderConfig.FamilyId = TEXT("mesh");
		ProviderConfig.OwnerLeaseId = Config.OwnerLeaseId;
		ProviderConfig.PreStatePlane =
			EUeAgentStatePlane::AuthoringMemory;
		ProviderConfig.AtomicityMode =
			EAtomicityMode::CompensatedSaga;
		ProviderConfig.MaxQueueItems = Config.MaxQueueItems;
		ProviderConfig.Projection = Config.Projection;
		ProviderConfig.TargetResolver =
			MakeShared<FModelingTargetResolver>(
				WorldResolver,
				ReferenceResolver);
		ProviderConfig.PreStateReadPort =
			MakeShared<FModelingStateReadPort>(
				PreStatePortId,
				EUeAgentStatePlane::AuthoringMemory,
				Catalog,
				ReferenceResolver,
				true);
		ProviderConfig.ChangeTargetProjector =
			MakeShared<FModelingChangeTargetProjector>();
		ProviderConfig.DraftBuilder =
			MakeShared<FModelingDraftBuilder>();
		ProviderConfig.MutationBindingProjector =
			MakeShared<FModelingMutationBindingProjector>();
		ProviderConfig.DurableTargetProjector =
			MakeShared<FModelingDurableTargetProjector>();
		ProviderConfig.DurableService = Config.DurableService;
		ProviderConfig.UeHook =
			MakeShared<FModelingUeHook>(
				WorldResolver,
				NativeHook.ToSharedRef());
		ProviderConfig.ExpectedStateProjector =
			MakeShared<FModelingExpectedStateProjector>();
		if (Config.PersistedReferenceResolver.IsValid())
		{
			const TSharedRef<IUeAgentStateReadPort, ESPMode::ThreadSafe>
				PersistedStateReadPort = MakeShared<FModelingStateReadPort>(
					PersistedStatePortId,
					EUeAgentStatePlane::Persisted,
					PersistedCatalog,
					Config.PersistedReferenceResolver.ToSharedRef(),
					false);
			ProviderConfig.PersistedStateOracle = MakeShared<
				FStateReadPortPersistedStateOracle,
				ESPMode::ThreadSafe>(PersistedStateReadPort);
		}
		else
		{
			ProviderConfig.PersistedStateOracle = MakeShared<
				FModelingArtifactPersistedStateOracle,
				ESPMode::ThreadSafe>(
					PersistedCatalog);
		}
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
