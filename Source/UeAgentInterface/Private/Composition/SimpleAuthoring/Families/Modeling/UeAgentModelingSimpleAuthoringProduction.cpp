#include "Composition/SimpleAuthoring/Families/Modeling/UeAgentModelingSimpleAuthoringProduction.h"
#include "Core/Contract/UeAgentContractJson.h"
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
#include "Serialization/JsonWriter.h"
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
			UE::UAI::Contract::FDiagnostic Error;
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


		void SetProductionError(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
        { OutError = MoveTemp(Diagnostic); if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;  }


		FDiagnostic ModelingFailure(const FString& Code, const FString& Reason, const FString& Field)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("simple_authoring"));
			Error.Schema = TEXT("uai.runtime.diagnostic/1");
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			return Error;
		}

		FDiagnostic JsonValueFailure(const FString& Path, const TSharedPtr<FJsonValue>& Actual, const FString& Expected)
		{
			auto Error = ModelingFailure(TEXT("modeling_geometry_state_field_invalid"),
				TEXT("The Modeling state field does not satisfy the required value."), Path);
			Error.ActualValuePresent = Actual.IsValid();
			if (Actual.IsValid() && (Actual->Type == EJson::Array || Actual->Type == EJson::Object))
			{
				auto Summary = MakeShared<FJsonObject>();
				Summary->SetStringField(TEXT("kind"), Actual->Type == EJson::Array ? TEXT("array") : TEXT("object"));
				if (Actual->Type == EJson::Array) Summary->SetNumberField(TEXT("count"), Actual->AsArray().Num());
				else if (Actual->AsObject().IsValid()) Summary->SetNumberField(TEXT("count"), Actual->AsObject()->Values.Num());
				Error.ActualValue = MakeShared<FJsonValueObject>(Summary);
			}
			else Error.ActualValue = Actual;
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return Error;
		}

		FDiagnostic JsonFieldFailure(const TSharedPtr<FJsonObject>& Object, const FString& Field,
			const FString& Expected, const FString& Path = FString())
		{
			return JsonValueFailure(Path.IsEmpty() ? Field : Path,
				Object.IsValid() ? Object->Values.FindRef(Field) : nullptr, Expected);
		}

		FDiagnostic MissingProfile(const FUeAgentDescriptorIdentity& Identity)
		{
			const auto& Catalog = GetProfileCatalog();
			if (!Catalog.Error.IsEmpty()) return Catalog.Error;
			const auto* SameId = Catalog.Profiles.FindByPredicate([&](const FModelingProfile& Profile)
				{ return Profile.DescriptorId == Identity.GetDescriptorId(); });
			const auto* SameVersion = Catalog.Profiles.FindByPredicate([&](const FModelingProfile& Profile)
				{ return Profile.DescriptorId == Identity.GetDescriptorId() && Profile.DescriptorVersion == Identity.GetVersion(); });
			auto Error = ModelingFailure(TEXT("modeling_geometry_profile_missing"),
				!SameId ? TEXT("No Modeling profile is registered for the requested descriptor.")
					: !SameVersion ? TEXT("No registered Modeling profile matches the requested descriptor version.")
					: TEXT("The Modeling descriptor hash differs from the registered profile."),
				!SameId ? TEXT("descriptor_id") : !SameVersion ? TEXT("descriptor_version") : TEXT("descriptor_hash"));
			Error.CandidateName = Identity.GetDescriptorId();
			if (!SameId)
			{
				Error.ActualValue = MakeShared<FJsonValueString>(Identity.GetDescriptorId());
				TArray<TSharedPtr<FJsonValue>> Supported;
				for (const auto& Profile : Catalog.Profiles) Supported.Add(MakeShared<FJsonValueString>(Profile.DescriptorId));
				Error.ExpectedValue = MakeShared<FJsonValueArray>(Supported);
			}
			else if (!SameVersion)
			{
				Error.ActualValue = MakeShared<FJsonValueNumber>(Identity.GetVersion());
				TArray<TSharedPtr<FJsonValue>> Versions;
				for (const auto& Profile : Catalog.Profiles)
					if (Profile.DescriptorId == Identity.GetDescriptorId()) Versions.Add(MakeShared<FJsonValueNumber>(Profile.DescriptorVersion));
				Error.ExpectedValue = MakeShared<FJsonValueArray>(Versions);
			}
			else
			{
				Error.ActualEvidence = MakeShared<FJsonValueString>(Identity.GetDescriptorHash());
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(SameVersion->DescriptorHash);
			}
			return Error;
		}

		FDiagnostic CountFailure(const FString& Code, const FString& Reason, const FString& Field, const int32 Actual, const int32 Expected)
		{
			auto Error = ModelingFailure(Code, Reason, Field);
			Error.ActualValue = MakeShared<FJsonValueNumber>(Actual);
			Error.ExpectedValue = MakeShared<FJsonValueNumber>(Expected);
			return Error;
		}

		bool TrySerializeJson(const TSharedPtr<FJsonObject>& Object, FString& OutJson, FDiagnostic& OutError)
		{
			OutJson.Reset();
			if (!Object.IsValid())
			{
				OutError = ModelingFailure(TEXT("modeling_geometry_baseline_evidence_invalid"),
					TEXT("The Modeling baseline evidence object is missing."), TEXT("binding_evidence"));
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
			if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
			{
				OutError = ModelingFailure(TEXT("modeling_geometry_baseline_evidence_invalid"),
					TEXT("The Modeling baseline evidence object could not be serialized to JSON."), TEXT("binding_evidence"));
				return false;
			}
			return true;
		}

		bool TryParseRequestEvidence(
			const FUeAgentCandidateBinding& Binding,
			TSharedPtr<FJsonObject>& OutPayload,
			FUeAgentDiscoveryError& OutError)
		{
			const FString& Evidence = Binding.GetBindingEvidence();
			if (!Evidence.StartsWith(RequestEvidencePrefix))
			{
				OutError = ModelingFailure(TEXT("modeling_geometry_request_evidence_invalid"),
					TEXT("The Modeling binding evidence has an unsupported format prefix."), TEXT("binding_evidence/prefix"));
				OutError.SubjectId = Binding.GetBindingId();
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Evidence.Left(FCString::Strlen(RequestEvidencePrefix)));
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(RequestEvidencePrefix);
				return false;
			}
			if (!Contract::Json::TryParseObject(Evidence.RightChop(FCString::Strlen(RequestEvidencePrefix)), OutPayload, OutError))
			{
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("binding_evidence");
				OutError.SubjectId = Binding.GetBindingId();
				return false;
			}
			return true;
		}

		bool TryGetTargetRef(const TSharedPtr<FJsonObject>& Payload, FString& OutTargetRef, FDiagnostic& OutError,
			const FString& SourceKind = TEXT("native_result"))
		{
			OutTargetRef.Reset();
			if (Payload.IsValid() && Payload->TryGetStringField(TEXT("target_ref"), OutTargetRef) && !OutTargetRef.IsEmpty())
				return true;
			OutError = JsonFieldFailure(Payload, TEXT("target_ref"), TEXT("nonempty string"));
			OutError.SourceKind = SourceKind;
			if (SourceKind == TEXT("command_input")) OutError.Source.JsonPath = TEXT("/target_ref");
			return false;
		}

		bool TryReadNumberArray(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Field,
			const int32 ExpectedCount,
			TArray<double>& OutValues,
			FDiagnostic& OutError,
			const FString& ParentPath = FString())
		{
			OutValues.Reset();
			const FString Path = ParentPath.IsEmpty() ? Field : ParentPath + TEXT("/") + Field;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values)
			{
				OutError = JsonFieldFailure(Object, Field, TEXT("number array"), Path);
				return false;
			}
			if (Values->Num() != ExpectedCount)
			{
				OutError = CountFailure(TEXT("modeling_geometry_state_array_size_invalid"),
					TEXT("The Modeling state array has the wrong number of elements."), Path, Values->Num(), ExpectedCount);
				return false;
			}
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
				double Number = 0.0;
				if (!Value.IsValid() || !Value->TryGetNumber(Number) || !FMath::IsFinite(Number))
				{
					OutValues.Reset();
					OutError = JsonValueFailure(Path + TEXT("/") + FString::FromInt(Index), Value, TEXT("finite number"));
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
			FUeAgentStateSemanticError& OutError,
			const FString& Path = TEXT("state"))
		{
			OutValue.Reset();
			if (!JsonValue.IsValid())
			{
				OutError = ModelingFailure(TEXT("modeling_geometry_state_json_invalid"),
					TEXT("The generated Modeling state contains a missing JSON value."), Path);
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			const auto Complete = [&](const bool bSucceeded)
			{
				if (!bSucceeded)
				{
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = OutError.PropertyPath.IsEmpty()
						? Path : Path + (OutError.PropertyPath.StartsWith(TEXT("/")) ? OutError.PropertyPath : TEXT("/") + OutError.PropertyPath);
				}
				return bSucceeded;
			};
			switch (JsonValue->Type)
			{
			case EJson::Null:
				return Complete(FUeAgentCanonicalValue::TryNull(
					Resources,
					OutValue,
					OutError));
			case EJson::Boolean:
				return Complete(FUeAgentCanonicalValue::TryBoolean(
					JsonValue->AsBool(),
					Resources,
					OutValue,
					OutError));
			case EJson::String:
				return Complete(FUeAgentCanonicalValue::TryString(
					JsonValue->AsString(),
					Resources,
					OutValue,
					OutError));
			case EJson::Number:
				return Complete(FUeAgentCanonicalValue::TryDecimal(
					FString::SanitizeFloat(
						JsonValue->AsNumber()),
					Resources,
					OutValue,
					OutError));
			case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					const auto& Values = JsonValue->AsArray();
					for (int32 Index = 0; Index < Values.Num(); ++Index)
					{
						FUeAgentCanonicalValuePtr CanonicalItem;
						if (!TryCanonicalValue(
								Values[Index],
								Resources,
								CanonicalItem,
								OutError,
								Path + TEXT("/") + FString::FromInt(Index)))
						{
							return false;
						}
						Items.Add(CanonicalItem.ToSharedRef());
					}
					return Complete(FUeAgentCanonicalValue::TryArray(
						Items,
						Resources,
						OutValue,
						OutError));
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
								OutError,
								Path + TEXT("/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))))
						{
							return false;
						}
						Items.Emplace(
							Pair.Key,
							CanonicalItem.ToSharedRef());
					}
					return Complete(FUeAgentCanonicalValue::TryObject(
						Items,
						Resources,
						OutValue,
						OutError));
				}
			default:
				OutError = ModelingFailure(TEXT("modeling_geometry_state_json_kind_invalid"),
					TEXT("The generated Modeling state contains an unsupported JSON kind."), Path);
				OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(JsonValue->Type));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("null, boolean, string, number, array or object"));
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
				OutError = Error;
				if (OutError.IsEmpty())
					OutError = ModelingFailure(TEXT("modeling_geometry_state_catalog_unavailable"),
						TEXT("The Modeling world catalog failed to enumerate worlds without a diagnostic."), TEXT("world_catalog"));
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
					OutError = CountFailure(TEXT("modeling_geometry_state_world_ambiguous"),
						TEXT("More than one loaded world matches the Modeling binding."), TEXT("world"), 2, 1);
					OutError.TargetPath = Binding.GetPublicRef();
					OutError.SubjectId = Binding.GetBindingId();
					OutError.StoppedEarly = true;
					OutWorld = nullptr;
					return false;
				}
				OutWorld = Candidate.GetWorld();
			}
			if (!OutWorld)
			{
				OutError = CountFailure(TEXT("modeling_geometry_state_world_unresolved"),
					TEXT("No loaded world matches the Modeling binding."), TEXT("world"), 0, 1);
				OutError.TargetPath = Binding.GetPublicRef();
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
			FDiagnostic ProbeError;
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
						Values, ProbeError))
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
					Values, ProbeError))
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
			FVector& OutScale,
			FDiagnostic& OutError)
		{
			OutLocation = FVector::ZeroVector;
			OutRotation = FQuat::Identity;
			OutScale = FVector::OneVector;
			const TSharedPtr<FJsonObject>* Placement = nullptr;
			if (!Payload->TryGetObjectField(TEXT("placement"), Placement) || !Placement || !Placement->IsValid())
			{
				OutError = JsonFieldFailure(Payload, TEXT("placement"), TEXT("object"));
				return false;
			}
			TArray<double> Translation, Rotation, Scale;
			FString Unit;
			if (!(*Placement)->TryGetStringField(TEXT("unit"), Unit))
			{
				OutError = JsonFieldFailure(*Placement, TEXT("unit"), TEXT("string"), TEXT("placement/unit"));
				return false;
			}
			if (!TryReadNumberArray(*Placement, TEXT("translation"), 3, Translation, OutError, TEXT("placement"))
				|| !TryReadNumberArray(*Placement, TEXT("rotation_quaternion_xyzw"), 4, Rotation, OutError, TEXT("placement"))
				|| !TryReadNumberArray(*Placement, TEXT("scale"), 3, Scale, OutError, TEXT("placement"))) return false;
			const double UnitScale = Unit == TEXT("meter") ? 100.0 : 1.0;
			OutLocation = FVector(Translation[0] * UnitScale, Translation[1] * UnitScale, Translation[2] * UnitScale);
			OutRotation = FQuat(Rotation[0], Rotation[1], Rotation[2], Rotation[3]);
			if (OutRotation.SizeSquared() <= UE_DOUBLE_SMALL_NUMBER)
			{
				OutError = ModelingFailure(TEXT("modeling_geometry_placement_invalid"),
					TEXT("The Modeling placement quaternion is too small to normalize."), TEXT("placement/rotation_quaternion_xyzw"));
				OutError.ActualValue = (*Placement)->TryGetField(TEXT("rotation_quaternion_xyzw"));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(
					FString::Printf(TEXT("quaternion squared norm greater than %.17g"), UE_DOUBLE_SMALL_NUMBER));
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
			FDiagnostic ProbeError;
			if (!TryReadExpectedPlacement(
					Payload,
					ExpectedLocation,
					ExpectedRotation,
					ExpectedScale, ProbeError))
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
			if (!TryGetTargetRef(Payload, TargetRef, OutError)) return false;
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
				OutError = StateError;
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
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
					ExpectedScale, OutError))
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
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
				SetProductionError(OutError, StateError);
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
				if (!Profile)
				{
					OutError = MissingProfile(CanonicalRequest.GetDescriptorIdentity());
					return false;
				}
				if (!Contract::Json::TryParseObject(CanonicalRequest.GetPayload().GetJsonValue(), Payload, OutError))
				{
					if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("command_input");
					return false;
				}
				if (!TryGetTargetRef(Payload, TargetRef, OutError, TEXT("command_input"))) return false;
				UWorld* World = nullptr;
				FExecutionMutationError Error;
				if (Cancellation.IsCancelled())
				{
					OutError = ModelingFailure(TEXT("modeling_geometry_target_unresolved"),
						TEXT("Modeling target resolution was cancelled before resolving the world."), TEXT("cancellation"));
					OutError.TargetPath = TargetRef;
					return false;
				}
				const bool bResolved = WorldResolver->TryResolveWorld(Context, World, Error);
				if (!bResolved || !World || !World->PersistentLevel)
				{
					OutError = Error;
					if (OutError.IsEmpty())
					{
						OutError = ModelingFailure(TEXT("modeling_geometry_target_unresolved"),
							!bResolved ? TEXT("The Modeling world resolver failed without a diagnostic.")
								: !World ? TEXT("The Modeling world resolver returned no world.")
								: TEXT("The resolved Modeling world has no persistent level."),
							World ? TEXT("world/persistent_level") : TEXT("world"));
						OutError.ActualValuePresent = false;
						OutError.ExpectedValuePresent = true;
						OutError.SubjectId = Context.GetWorldIdentity();
					}
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					return false;
				}
				FString EvidenceJson =
					CanonicalRequest.GetPayload().GetJsonValue();
				if (IsExistingEditProfile(*Profile))
				{
					ExistingEdit::FCarrierState Baseline;
					UE::UAI::Contract::FDiagnostic CarrierError;
					if (!ExistingEdit::TryReadCarrier(
							*World,
							*ReferenceResolver,
							TargetRef,
							Baseline,
							CarrierError))
					{
						SetProductionError(OutError, CarrierError, TargetRef);
						return false;
					}
					if (!Baseline.TargetRevision.IsSet()
						|| !Baseline.TargetRevision->IsValid())
					{
						OutError = Baseline.TargetRevision.IsSet()
							? Baseline.TargetRevision->GetValidationDiagnostic()
							: ModelingFailure(TEXT("modeling_geometry_target_revision_unavailable"),
								TEXT("The captured Modeling carrier has no target revision."), TEXT("target_revision"));
						if (!Baseline.TargetRevision.IsSet())
						{
							OutError.ActualValuePresent = false;
							OutError.ExpectedValuePresent = true;
						}
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
						return false;
					}
					if (!ExistingEdit::TryAttachBaselineEvidence(
							*World,
							*ReferenceResolver,
							Profile->OperationKey,
							Baseline,
							Payload,
							CarrierError))
					{
						SetProductionError(OutError, CarrierError, TargetRef);
						return false;
					}
					if (!TrySerializeJson(Payload, EvidenceJson, OutError))
					{
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
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
					OutError = *OutTarget->GetValidationDiagnostic();
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
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
				if (!Profile)
				{
					OutError = !Request.IsValid() ? *Request.GetBinding().GetValidationDiagnostic()
						: MissingProfile(Request.GetBinding().GetSummary().GetDescriptorIdentity());
					return false;
				}
				if (Request.GetStatePlane() != RequiredPlane)
				{
					OutError = ModelingFailure(TEXT("modeling_geometry_state_request_invalid"),
						TEXT("The Modeling read port does not serve the requested state plane."), TEXT("state_plane"));
					FString Actual = Contract::LexToString(Request.GetStatePlane());
					if (Actual.IsEmpty()) Actual = FString::FromInt(static_cast<int32>(Request.GetStatePlane()));
					OutError.ActualValue = MakeShared<FJsonValueString>(Actual);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Contract::LexToString(RequiredPlane));
					OutError.TargetPath = Request.GetBinding().GetPublicRef();
					return false;
				}
				if (Cancellation.IsCancelled())
				{
					OutError = ModelingFailure(TEXT("modeling_geometry_state_request_cancelled"),
						TEXT("The Modeling state read was cancelled before resolving its target."), TEXT("cancellation"));
					OutError.TargetPath = Request.GetBinding().GetPublicRef();
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
					|| !TryGetTargetRef(Payload, TargetRef, OutError))
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
					OutError = StateError;
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
							OutError = StateError;
							OutError.SubjectId = Ref;
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
							OutError = CountFailure(TEXT("modeling_geometry_output_already_exists"),
								TEXT("The Modeling primitive create target already exists in the current world."),
								TEXT("target_ref"), bAmbiguous ? 2 : 1, 0);
							OutError.TargetPath = TargetRef;
							if (bAmbiguous) OutError.StoppedEarly = true;
							return false;
						}
					}
					else if (!Actor || bAmbiguous)
					{
						OutError = CountFailure(bAmbiguous ? TEXT("modeling_geometry_created_target_ambiguous") : TEXT("modeling_geometry_created_target_missing"),
							bAmbiguous ? TEXT("Multiple dynamic mesh actors match the created Modeling target.") : TEXT("The created Modeling target is missing from readback."),
							TEXT("target_ref"), bAmbiguous ? 2 : 0, 1);
						OutError.TargetPath = TargetRef;
						if (bAmbiguous) OutError.StoppedEarly = true;
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
					UE::UAI::Contract::FDiagnostic CarrierError;
					if (!ExistingEdit::TryReadCarrier(
							*World,
							*ReferenceResolver,
							TargetRef,
							TargetState,
							CarrierError)
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
							OutError = CarrierError;
							if (OutError.IsEmpty()) OutError = ModelingFailure(TEXT("modeling_geometry_target_state_invalid"),
								TEXT("The Modeling carrier read failed without a diagnostic."), TEXT("carrier"));
							if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
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
								CarrierError)
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
								OutError = CarrierError;
								if (OutError.IsEmpty()) OutError = ModelingFailure(TEXT("modeling_geometry_operand_state_invalid"),
									TEXT("The Modeling carrier read failed without a diagnostic."), TEXT("carrier"));
								if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = OperandRef;
							}
							return false;
						}
					}
				}
				else
				{
					ExistingEdit::FCarrierState Baseline;
					ExistingEdit::FCarrierState Actual;
					UE::UAI::Contract::FDiagnostic CarrierError;
					if (!ExistingEdit::TryReadBaseline(
							Payload,
							Baseline, CarrierError)
						|| !ExistingEdit::TryReadCarrier(
							*World,
							*ReferenceResolver,
							TargetRef,
							Actual,
							CarrierError)
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
							OutError = CarrierError;
							if (OutError.IsEmpty())
							{
								OutError = FDiagnostic::Failure(TEXT("modeling_geometry_topology_readback_invalid"),
									TEXT("Reading the Modeling carrier failed without a diagnostic."), TEXT("modeling_capture"));
								OutError.SourceKind = TEXT("native_result");
							}
							if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
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
					OutError = StateError;
					OutError.SubjectId = TargetRef;
					return false;
				}
				OutSnapshot = MoveTemp(Snapshot);
				return true;
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
				if (CanonicalRef != SourceRef || !IsValid(OutObject) || (OwningWorld && OwningWorld != &World))
				{
					const bool bRefMismatch = CanonicalRef != SourceRef;
					const bool bObjectMissing = !IsValid(OutObject);
					OutError = ModelingFailure(TEXT("modeling_geometry_persisted_reference_unresolved"),
						bRefMismatch ? TEXT("The requested Modeling reference differs from the persisted snapshot reference.")
							: bObjectMissing ? TEXT("The Modeling snapshot object is no longer valid.")
							: TEXT("The Modeling snapshot object belongs to a different world."),
						bRefMismatch ? TEXT("target_ref") : bObjectMissing ? TEXT("snapshot_object") : TEXT("world"));
					OutError.TargetPath = CanonicalRef;
					if (bRefMismatch)
					{
						OutError.ActualValue = MakeShared<FJsonValueString>(CanonicalRef);
						OutError.ExpectedValue = MakeShared<FJsonValueString>(SourceRef);
					}
					else if (bObjectMissing)
					{
						OutError.ActualValuePresent = false;
						OutError.ExpectedValuePresent = true;
					}
					else
					{
						OutError.ActualEvidence = MakeShared<FJsonValueString>(OwningWorld->GetPathName());
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(World.GetPathName());
					}
					OutObject = nullptr;
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
				if (!World.IsValid() || !World->PersistentLevel || WorldIdentity.IsEmpty() || ContextRevision.IsEmpty())
				{
					const TCHAR* Field = !World.IsValid() ? TEXT("world") : !World->PersistentLevel
						? TEXT("world/persistent_level") : WorldIdentity.IsEmpty() ? TEXT("world_identity") : TEXT("context_revision");
					OutError = ModelingFailure(TEXT("modeling_geometry_persisted_world_catalog_invalid"),
						FString::Printf(TEXT("The persisted Modeling world catalog has no valid '%s'."), Field), Field);
					if (!World.IsValid() || !World->PersistentLevel)
					{
						OutError.ActualValuePresent = false;
						OutError.ExpectedValuePresent = true;
					}
					else
					{
						OutError.ActualEvidence = MakeShared<FJsonValueString>(WorldIdentity.IsEmpty() ? WorldIdentity : ContextRevision);
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty identifier"));
					}
					return false;
				}
				OutCandidates.Emplace(
					World.Get(),
					WorldIdentity,
					World->PersistentLevel->GetPathName(),
					EUeAgentExecutionWorldType::Preview,
					TOptional<int32>(),
					ContextRevision);
				return true;
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
				if (!Target.IsValid())
				{
					OutError = *Target.GetValidationDiagnostic();
					return false;
				}
				if (Artifacts.IsEmpty())
				{
					OutError = CountFailure(TEXT("modeling_geometry_persisted_read_request_invalid"),
						TEXT("The Modeling persisted-state read requires at least one durable artifact."), TEXT("artifacts"), 0, 1);
					return false;
				}
				if (Target.GetBinding().GetReadPortId() != PersistedStatePortId)
				{
					OutError = ModelingFailure(TEXT("modeling_geometry_persisted_read_request_invalid"),
						TEXT("The Modeling verification target uses a different persisted-state read port."), TEXT("read_port_id"));
					OutError.ActualValue = MakeShared<FJsonValueString>(Target.GetBinding().GetReadPortId());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(PersistedStatePortId);
					return false;
				}
				if (!Profile)
				{
					OutError = MissingProfile(Target.GetBinding().GetSummary().GetDescriptorIdentity());
					return false;
				}
				if (Cancellation.IsCancelled())
				{
					OutError = ModelingFailure(TEXT("modeling_geometry_persisted_read_request_invalid"),
						TEXT("The Modeling persisted-state read was cancelled before reading its evidence."), TEXT("cancellation"));
					return false;
				}
				if (!TryParseRequestEvidence(Target.GetBinding(), Payload, OutError) || !TryGetTargetRef(Payload, TargetRef, OutError))
					return false;

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
				if (!IsExistingEditProfile(*Profile))
				{
					OutError = FVerificationError::Failure(TEXT("modeling_geometry_persisted_baseline_invalid"),
						TEXT("The Modeling profile does not support existing-edit baseline verification."), TEXT("verification"));
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = TEXT("native_target_kind");
					OutError.ActualValue = MakeShared<FJsonValueString>(Profile->NativeTargetKind);
					OutError.TargetPath = TargetRef;
					return false;
				}
				if (!ExistingEdit::TryReadBaseline(Payload, Baseline, OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
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
					OutError = ModelingFailure(TEXT("modeling_geometry_persisted_carrier_unsupported"),
						TEXT("The Modeling persisted-state reader does not support this carrier kind."), TEXT("carrier_kind"));
					OutError.TargetPath = TargetRef;
					OutError.ActualValue = MakeShared<FJsonValueString>(Baseline.CarrierKind);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("dynamic_mesh_actor or static_mesh_asset"));
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
							ObserverError = ModelingFailure(TEXT("modeling_geometry_persisted_asset_missing"),
								TEXT("The captured package does not contain the expected static mesh asset."), TEXT("static_mesh"));
							ObserverError.TargetPath = Baseline.TargetRef;
							ObserverError.ActualValuePresent = false;
							ObserverError.ExpectedValuePresent = true;
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
							ObserverError = ReadError;
							if (ObserverError.IsEmpty()) ObserverError = ModelingFailure(TEXT("modeling_geometry_persisted_state_read_failed"),
								TEXT("The Modeling persisted-state reader failed without a diagnostic."), TEXT("persisted_state"));
							if (ObserverError.TargetPath.IsEmpty()) ObserverError.TargetPath = TargetRef;
							return false;
						}
						return true;
					},
					LoadError);
				if (!bObserved)
				{
					OutError = LoadError;
					if (OutError.IsEmpty()) OutError = ModelingFailure(TEXT("modeling_geometry_persisted_package_unavailable"),
						TEXT("The Modeling persisted snapshot observer failed without a diagnostic."), TEXT("persisted_snapshot"));
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					return false;
				}
				return true;
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
						const FPersistedWorldReferenceMap&,
						FPersistedWorldLoadError& ObserverError)
					{
						if (Cancellation.IsCancelled())
						{
							ObserverError = ModelingFailure(TEXT("modeling_geometry_persisted_world_read_cancelled"),
								TEXT("The Modeling persisted-world read was cancelled before locating its actor."), TEXT("cancellation"));
							ObserverError.TargetPath = TargetRef;
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
							ObserverError = CountFailure(bAmbiguous ? TEXT("modeling_geometry_persisted_actor_ambiguous") : TEXT("modeling_geometry_persisted_actor_missing"),
								bAmbiguous ? TEXT("Multiple actors match the Modeling target in the persisted world.") : TEXT("No actor matches the Modeling target in the persisted world."),
								TEXT("actor"), bAmbiguous ? 2 : 0, 1);
							ObserverError.TargetPath = TargetRef;
							if (bAmbiguous) ObserverError.StoppedEarly = true;
							if (Baseline) ObserverError.ExpectedEvidence = MakeShared<FJsonValueString>(Baseline->ActorGuid.ToString());
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
							ObserverError = *SnapshotBinding.GetValidationDiagnostic();
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
							ObserverError = ReadError;
							if (ObserverError.IsEmpty()) ObserverError = ModelingFailure(TEXT("modeling_geometry_persisted_world_state_read_failed"),
								TEXT("The Modeling persisted-state reader failed without a diagnostic."), TEXT("persisted_state"));
							if (ObserverError.TargetPath.IsEmpty()) ObserverError.TargetPath = TargetRef;
							return false;
						}
						return true;
					},
					LoadError);
				if (!bObserved)
				{
					OutError = LoadError;
					if (OutError.IsEmpty()) OutError = ModelingFailure(TEXT("modeling_geometry_persisted_world_unavailable"),
						TEXT("The Modeling persisted snapshot observer failed without a diagnostic."), TEXT("persisted_snapshot"));
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					return false;
				}
				return true;
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
					SetProductionError(OutError, Error);
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
					OutError = MissingProfile(CanonicalRequest.GetDescriptorIdentity());
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
						OutError = *OutTargets.Last().GetValidationDiagnostic();
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = OutTargets.Last().GetPublicRef();
						OutTargets.Reset();
						return false;
					}
				}
				if (OutTargets.IsEmpty())
				{
					OutError = CountFailure(TEXT("modeling_geometry_change_target_invalid"),
						TEXT("At least one Modeling change target is required."), TEXT("change_targets"), 0, 1);
					return false;
				}
				return true;
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
					OutError = CountFailure(TEXT("modeling_geometry_change_targets_missing"),
						TEXT("The number of Modeling change targets differs from the planned changes."),
						TEXT("change_targets"), Targets.Num(), Changes.Num());
					OutError.SubjectId = Plan->GetRequestId();
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
					if (Targets[Index].GetSummary().GetIdentity() != Changes[Index].GetIdentity())
					{
						OutError = ModelingFailure(TEXT("modeling_geometry_mutation_binding_invalid"),
							TEXT("The Modeling mutation target identity differs from its planned change."),
							FString::Printf(TEXT("targets/%d/identity"), Index));
						OutError.TargetPath = Targets[Index].GetPublicRef();
						OutError.ActualEvidence = MakeShared<FJsonValueString>(Targets[Index].GetSummary().GetIdentity().GetCanonicalKey());
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Changes[Index].GetIdentity().GetCanonicalKey());
						OutBindings.Reset();
						return false;
					}
					if (!OutBindings.Last().IsValid())
					{
						OutError = OutBindings.Last().GetValidationDiagnostic();
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Targets[Index].GetPublicRef();
						OutBindings.Reset();
						return false;
					}
				}
				if (OutBindings.IsEmpty())
				{
					OutError = CountFailure(TEXT("modeling_geometry_mutation_binding_invalid"),
						TEXT("At least one Modeling mutation binding is required."), TEXT("mutation_bindings"), 0, 1);
					return false;
				}
				return true;
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
				if (Changes.IsEmpty())
				{
					OutError = CountFailure(TEXT("modeling_geometry_durable_target_missing"),
						TEXT("A Modeling durable target requires at least one planned change."), TEXT("changes"), 0, 1);
					OutError.SubjectId = Target.GetBindingId();
					return false;
				}
				if (!Profile)
				{
					OutError = MissingProfile(Plan->GetDescriptorIdentity());
					return false;
				}
				if (Target.GetPublicRef().IsEmpty())
				{
					OutError = ModelingFailure(TEXT("modeling_geometry_durable_target_missing"),
						TEXT("The Modeling durable target has an empty public reference."), TEXT("target_ref"));
					OutError.ActualValue = MakeShared<FJsonValueString>(Target.GetPublicRef());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty package reference"));
					OutError.SubjectId = Target.GetBindingId();
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
								DurablePackageName, EvidenceError))
					{
						OutError = EvidenceError;
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target.GetPublicRef();
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
				if (!OutTargets[0].IsValid())
				{
					OutError = OutTargets[0].GetValidationDiagnostic();
					return false;
				}
				return true;
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
				if (!Profile)
				{
					OutError = MissingProfile(Plan->GetDescriptorIdentity());
					return false;
				}
				if (!TryParseRequestEvidence(ResolvedTarget, Payload, OutError)
					|| !TryGetTargetRef(Payload, TargetRef, OutError))
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
					SetProductionError(OutError, StateError);
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
						OutError = ModelingFailure(TEXT("modeling_geometry_verification_target_missing"),
							TEXT("No resolved target matches the Modeling change to verify."), TEXT("change_targets"));
						OutError.TargetPath = TargetRef;
						OutError.ActualValuePresent = false;
						OutError.ExpectedValuePresent = true;
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Change.GetIdentity().GetCanonicalKey());
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
						if (!ExistingEdit::TryReadBaseline(Payload, Baseline, OutError))
						{
							if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
							return false;
						}
						if (!TryCanonicalObject(
								ExistingEdit::BuildExpectedVerificationState(Profile->OperationKey, Baseline, Payload),
								Resources, ExpectedValue, StateError))
						{
							OutError = StateError;
							if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
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
						SetProductionError(OutError, StateError);
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
						OutError = *OutTargets.Last().GetValidationDiagnostic();
						return false;
					}
				}
				if (OutTargets.IsEmpty())
				{
					OutError = CountFailure(TEXT("modeling_geometry_verification_target_missing"),
						TEXT("At least one Modeling verification target is required."), TEXT("verification_targets"), 0, 1);
					return false;
				}
				return true;
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
			const TCHAR* Missing = !Config.DescriptorRegistry.IsValid() ? TEXT("descriptor_registry")
				: !Config.WorldCatalog.IsValid() ? TEXT("world_catalog")
				: !Config.ReferenceResolver.IsValid() ? TEXT("reference_resolver")
				: !Config.DurableService.IsValid() ? TEXT("durable_service")
				: !Config.AbsenceOracle.IsValid() ? TEXT("absence_oracle")
				: !Config.ReferenceOracle.IsValid() ? TEXT("reference_oracle")
				: !Config.Projection.IsSet() ? TEXT("projection") : nullptr;
			if (Missing)
			{
				OutError = ModelingFailure(TEXT("modeling_simple_authoring_dependencies_missing"),
					FString::Printf(TEXT("The Modeling authoring provider has no '%s'."), Missing), Missing);
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			if (!Config.Projection->IsValid())
			{
				OutError = Config.Projection->GetValidationDiagnostic();
				return false;
			}
			if (Config.OwnerLeaseId.IsEmpty())
			{
				OutError = ModelingFailure(TEXT("modeling_simple_authoring_dependencies_missing"),
					TEXT("The Modeling authoring provider has an empty owner lease identifier."), TEXT("owner_lease_id"));
				OutError.ActualValue = MakeShared<FJsonValueString>(TEXT(""));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty identifier"));
				return false;
			}
			const FModelingProfileCatalog& Catalog =
				GetProfileCatalog();
			if (!Catalog.Error.IsEmpty())
			{
				OutError = Catalog.Error;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TEXT("mesh");
				return false;
			}
			for (const FModelingProfile& Profile : Catalog.Profiles)
			{
				const FUeAgentOperationDescriptor* Descriptor =
					Config.DescriptorRegistry
						->FindByDescriptorId(Profile.DescriptorId);
				if (!Descriptor)
				{
					OutError = ModelingFailure(TEXT("modeling_simple_authoring_descriptor_missing"),
						TEXT("The required Modeling operation descriptor is not registered."), TEXT("descriptor_id"));
					OutError.CandidateName = Profile.DescriptorId;
					OutError.ActualValuePresent = false;
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Profile.DescriptorId);
					return false;
				}
				if (Descriptor->GetCapabilityId() != Profile.CapabilityId || Descriptor->GetDomainId() != TEXT("mesh"))
				{
					const bool bCapabilityMismatch = Descriptor->GetCapabilityId() != Profile.CapabilityId;
					OutError = ModelingFailure(TEXT("modeling_simple_authoring_descriptor_missing"),
						TEXT("The registered Modeling descriptor does not match its profile."),
						bCapabilityMismatch ? TEXT("capability_id") : TEXT("domain_id"));
					OutError.CandidateName = Profile.DescriptorId;
					OutError.ActualValue = MakeShared<FJsonValueString>(bCapabilityMismatch ? Descriptor->GetCapabilityId() : Descriptor->GetDomainId());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(bCapabilityMismatch ? Profile.CapabilityId : TEXT("mesh"));
					return false;
				}
				if (Descriptor->GetIdentity().GetVersion() != Profile.DescriptorVersion)
				{
					OutError = ModelingFailure(TEXT("modeling_simple_authoring_descriptor_missing"),
						TEXT("The registered Modeling descriptor version differs from its profile."), TEXT("descriptor_version"));
					OutError.CandidateName = Profile.DescriptorId;
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Descriptor->GetIdentity().GetVersion());
					OutError.ExpectedValue = MakeShared<FJsonValueNumber>(Profile.DescriptorVersion);
					return false;
				}
				if (Descriptor->GetIdentity().GetDescriptorHash() != Profile.DescriptorHash)
				{
					OutError = ModelingFailure(TEXT("modeling_simple_authoring_descriptor_missing"),
						TEXT("The registered Modeling descriptor hash differs from its profile."), TEXT("descriptor_hash"));
					OutError.CandidateName = Profile.DescriptorId;
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Descriptor->GetIdentity().GetDescriptorHash());
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Profile.DescriptorHash);
					return false;
				}
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
					NativeError))
		{
			OutError = NativeError;
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TEXT("mesh");
			return false;
		}

		FUeAgentSimpleAuthoringProductionProviderConfig ProviderConfig;
		ProviderConfig.FamilyId = TEXT("mesh");
		ProviderConfig.OwnerLeaseId = Config.OwnerLeaseId;
		ProviderConfig.PreStatePlane =
			EUeAgentStatePlane::AuthoringMemory;
		ProviderConfig.AtomicityMode =
			EAtomicityMode::CompensatedSaga;
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

		return FUeAgentSimpleAuthoringProductionProvider::TryCreate(
			ProviderConfig, OutProvider, OutError);
	}
}
