#include "Misc/AutomationTest.h"

#include "EngineUtils.h"
#include "Engine/World.h"
#include "Landscape.h"
#include "LandscapeDataAccess.h"
#include "LandscapeEdit.h"
#include "LandscapeInfo.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"

#include "DomainAdapters/LandscapeGeometryEffect/UeAgentLandscapeGeometryEffectCapabilities.h"


#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"


namespace
{
	constexpr int32 LandscapeMin = 0;
	constexpr int32 LandscapeMax = 7;
	constexpr int32 LandscapeSize = 8;

	class FOwnedLandscapeWorld final
	{
	public:
		explicit FOwnedLandscapeWorld(
			const bool bCreateBaseline)
		{
			const FString Suffix =
				FGuid::NewGuid().ToString(
					EGuidFormats::Digits);
			Package = CreatePackage(
				*(TEXT("/Temp/UAI/LandscapeGeometry/")
					+ Suffix));
			World = UWorld::CreateWorld(
				EWorldType::EditorPreview,
				false,
				FName(TEXT("LandscapeGeometryOwnedWorld")),
				Package);
			if (!World || !World->PersistentLevel)
			{
				return;
			}
			World->SetCurrentLevel(World->PersistentLevel);
			if (bCreateBaseline)
			{
				Landscape = CreateLandscape(
					TEXT("LandscapeGeometryOwned"),
					FVector(1000.0, 2000.0, 300.0),
					FRotator(0.0, 90.0, 0.0),
					FVector(100.0, 200.0, 50.0));
			}
			Package->SetDirtyFlag(false);
		}

		~FOwnedLandscapeWorld()
		{
			if (Package)
			{
				Package->SetDirtyFlag(false);
			}
			if (World)
			{
				World->DestroyWorld(false);
				World->MarkAsGarbage();
			}
			if (Package)
			{
				Package->MarkAsGarbage();
			}
		}

		ALandscape* CreateLandscape(
			const FString& Label,
			const FVector& Translation,
			const FRotator& Rotation,
			const FVector& Scale)
		{
			if (!World)
			{
				return nullptr;
			}

			FActorSpawnParameters SpawnParameters;
			SpawnParameters.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnParameters.ObjectFlags |= RF_Transactional;
			ALandscape* Created =
				World->SpawnActor<ALandscape>(
					Translation,
					Rotation,
					SpawnParameters);
			if (!Created)
			{
				return nullptr;
			}
			Created->SetActorScale3D(Scale);
			Created->SetActorLabel(Label);

			TArray<uint16> Heights;
			Heights.Init(
				LandscapeDataAccess::GetTexHeight(0.0f),
				LandscapeSize * LandscapeSize);
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

			Created->Import(
				FGuid::NewGuid(),
				LandscapeMin,
				LandscapeMin,
				LandscapeMax,
				LandscapeMax,
				1,
				7,
				HeightsByLayer,
				TEXT(""),
				MaterialLayersByLayer,
				ELandscapeImportAlphamapType::Additive,
				EmptyLayers);
			Created->CreateLandscapeInfo();
			Created->PostEditChange();
			return Created;
		}

		bool ReadExtent(
			ALandscape& Target,
			int32& OutMinX,
			int32& OutMinY,
			int32& OutMaxX,
			int32& OutMaxY) const
		{
			ULandscapeInfo* Info =
				Target.GetLandscapeInfo();
			return Info
				&& Info->GetLandscapeExtent(
					OutMinX,
					OutMinY,
					OutMaxX,
					OutMaxY);
		}

		bool ReadLocalHeights(
			ALandscape& Target,
			TArray<float>& OutHeights) const
		{
			int32 MinX = 0;
			int32 MinY = 0;
			int32 MaxX = 0;
			int32 MaxY = 0;
			ULandscapeInfo* Info =
				Target.GetLandscapeInfo();
			if (!Info
				|| !ReadExtent(
					Target,
					MinX,
					MinY,
					MaxX,
					MaxY))
			{
				return false;
			}

			const int32 Width = MaxX - MinX + 1;
			const int32 Height = MaxY - MinY + 1;
			TArray<uint16> Encoded;
			Encoded.SetNumUninitialized(Width * Height);
			FLandscapeEditDataInterface Edit(Info);
			Edit.GetHeightDataFast(
				MinX,
				MinY,
				MaxX,
				MaxY,
				Encoded.GetData(),
				Width,
				nullptr);

			OutHeights.Reset(Encoded.Num());
			for (const uint16 Value : Encoded)
			{
				OutHeights.Add(
					LandscapeDataAccess::GetLocalHeight(Value));
			}
			return true;
		}

		int32 CountLandscapes() const
		{
			int32 Count = 0;
			if (!World)
			{
				return Count;
			}
			for (TActorIterator<ALandscape> It(World);
				It;
				++It)
			{
				++Count;
			}
			return Count;
		}

		ALandscape* FindOnlyLandscape() const
		{
			ALandscape* Result = nullptr;
			if (!World)
			{
				return nullptr;
			}
			for (TActorIterator<ALandscape> It(World);
				It;
				++It)
			{
				if (Result)
				{
					return nullptr;
				}
				Result = *It;
			}
			return Result;
		}

		bool IsValid(const bool bExpectLandscape) const
		{
			return Package
				&& World
				&& World->PersistentLevel
				&& (!bExpectLandscape
					|| (Landscape
						&& Landscape->GetLandscapeInfo()));
		}

		UPackage* Package = nullptr;
		UWorld* World = nullptr;
		ALandscape* Landscape = nullptr;
	};

	bool ValidateOwnedFixture(
		FAutomationTestBase* Test,
		const FOwnedLandscapeWorld& Owner)
	{
		if (!Test->TestTrue(
				TEXT("owned Landscape fixture is valid"),
				Owner.IsValid(true)))
		{
			return false;
		}

		int32 MinX = 0;
		int32 MinY = 0;
		int32 MaxX = 0;
		int32 MaxY = 0;
		if (!Test->TestTrue(
				TEXT(
					"owned Landscape extent reads independently"),
				Owner.ReadExtent(
					*Owner.Landscape,
					MinX,
					MinY,
					MaxX,
					MaxY)))
		{
			return false;
		}
		Test->TestEqual(TEXT("fixture extent min x"), MinX, 0);
		Test->TestEqual(TEXT("fixture extent min y"), MinY, 0);
		Test->TestEqual(TEXT("fixture extent max x"), MaxX, 7);
		Test->TestEqual(TEXT("fixture extent max y"), MaxY, 7);
		Test->TestTrue(
			TEXT("fixture uses non-uniform XY/Z scale"),
			Owner.Landscape->GetActorScale3D().Equals(
				FVector(100.0, 200.0, 50.0)));
		Test->TestTrue(
			TEXT("fixture uses a rotated world placement"),
			Owner.Landscape->GetActorRotation().Equals(
				FRotator(0.0, 90.0, 0.0),
				0.01));

		TArray<float> Heights;
		if (!Test->TestTrue(
				TEXT(
					"owned Landscape heights read independently"),
				Owner.ReadLocalHeights(
					*Owner.Landscape,
					Heights)))
		{
			return false;
		}
		Test->TestEqual(
			TEXT("fixture has sixty-four samples"),
			Heights.Num(),
			64);
		for (const float Height : Heights)
		{
			Test->TestTrue(
				TEXT("fixture begins at local height zero"),
				FMath::IsNearlyZero(Height, 0.01f));
		}
		return !Test->HasAnyErrors();
	}
}


namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::GenericEffect;
	using namespace
		UE::UAI::DomainAdapters::LandscapeGeometryEffect;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	const TCHAR* CreateInput = TEXT(
		"{\"initial_height\":{\"encoding\":"
		"\"landscape_local_height\",\"value\":0},"
		"\"placement\":{\"basis_evidence\":"
		"\"explicit UE world frame and sample-origin placement\","
		"\"frame_id\":\"ue.world\",\"rotation_quaternion_xyzw\":"
		"[0,0,0.7071067811865476,0.7071067811865476],"
		"\"scale\":[100,200,50],\"subject_point\":"
		"\"sample_origin\",\"translation\":[1000,2000,300],"
		"\"unit\":\"centimeter\"},"
		"\"sample_region\":{\"basis_evidence\":"
		"\"one component, one section, seven quads produces an "
		"eight by eight sample grid\",\"frame_id\":"
		"\"ue.landscape.local_sample_grid\","
		"\"max_inclusive\":[7,7],\"min_inclusive\":[0,0]},"
		"\"target_ref\":\"uai://landscape/new-owned\","
		"\"topology\":{\"component_count\":[1,1],"
		"\"quads_per_section\":7,\"sections_per_component\":1}}");

	const TCHAR* HeightDeltaInput = TEXT(
		"{\"delta\":{\"encoding\":"
		"\"landscape_local_height_delta\",\"row_stride\":3,"
		"\"values\":[1,2,3,4,5,6]},"
		"\"region\":{\"frame_id\":"
		"\"ue.landscape.local_sample_grid\","
		"\"max_inclusive\":[4,2],\"min_inclusive\":[2,1]},"
		"\"space_contract\":{\"basis_evidence\":"
		"\"independent Landscape transform, scale, extent, and "
		"revision\",\"conversion_chain\":\"UE world intent -> "
		"observed Landscape transform -> local sample grid -> "
		"Landscape edit API -> independent readback -> UE world\","
		"\"expected_scale\":[100,200,50],"
		"\"read_frame_id\":\"ue.landscape.local_sample_grid\","
		"\"semantic_frame_id\":\"ue.world\","
		"\"unit\":\"centimeter\","
		"\"verify_frame_id\":\"ue.world\","
		"\"write_frame_id\":"
		"\"ue.landscape.local_sample_grid\"},"
		"\"target_ref\":\"uai://landscape/owned\"}");

	class FOwnedReferenceResolver final
		: public
			IUeAgentLandscapeGeometryEffectReferenceResolver
	{
	public:
		bool TryResolveReference(
			UWorld& World,
			const FString& CanonicalRef,
			UObject*& OutObject,
			FExecutionMutationError& OutError) override
		{
			++ResolveCalls;
			OutObject = Objects.FindRef(CanonicalRef);
			if (!IsValid(OutObject)
				|| OutObject->GetWorld() != &World)
			{
				OutError = FExecutionMutationError(
					TEXT(
						"landscape_geometry_reference_unresolved"),
					CanonicalRef);
				return false;
			}
			return true;
		}

		void Bind(
			const FString& CanonicalRef,
			UObject* Object)
		{
			Objects.Add(CanonicalRef, Object);
		}

		TMap<FString, UObject*> Objects;
		int32 ResolveCalls = 0;
	};

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
	MakeResources()
	{
		return MakeShared<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1048576},
				{EUeAgentResourceDimension::OutputBytes, 1048576},
				{EUeAgentResourceDimension::StringBytes, 1048576},
				{EUeAgentResourceDimension::Nodes, 16384},
				{EUeAgentResourceDimension::Items, 16384},
				{EUeAgentResourceDimension::Files, 64},
				{EUeAgentResourceDimension::ReferenceEdges, 16384},
				{EUeAgentResourceDimension::WorkUnits, 262144},
				{
					EUeAgentResourceDimension::
						EstimatedMemoryBytes,
					33554432,
				},
			}));
	}

	bool TryCanonicalValue(
		const TSharedPtr<FJsonValue>& JsonValue,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError)
	{
		OutValue.Reset();
		OutError = {};
		if (!JsonValue.IsValid())
		{
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
					TPair<FString, FUeAgentCanonicalValueRef>>
					Items;
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
			return false;
		}
	}

	FUeAgentCanonicalValuePtr CanonicalInput(
		FAutomationTestBase* Test,
		const FString& Json,
		FUeAgentResourceTracker& Resources)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Json);
		if (!Test->TestTrue(
				TEXT("parse Landscape canonical input"),
				FJsonSerializer::Deserialize(Reader, Object)
					&& Object.IsValid()))
		{
			return nullptr;
		}
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		Test->TestTrue(
			TEXT("convert Landscape input to C4 canonical value"),
			TryCanonicalValue(
				MakeShared<FJsonValueObject>(Object),
				Resources,
				Value,
				Error));
		return Value;
	}

	FUeAgentCanonicalSnapshotPtr MakeSnapshot(
		FAutomationTestBase* Test,
		const FUeAgentOperationDescriptor& Descriptor,
		UWorld& World,
		const ALandscape* Landscape,
		FUeAgentResourceTracker& Resources)
	{
		const FUeAgentStateRevision Revision(
			TEXT("landscape-geometry-e2"),
			TEXT("owned-worker:r1"),
			1);
		TArray<FUeAgentStateEntry> Entries;
		if (Landscape)
		{
			const FString CurrentJson = FString::Printf(
				TEXT(
					"{\"extent\":{\"max_inclusive\":[7,7],"
					"\"min_inclusive\":[0,0]},"
					"\"height_encoding\":"
					"\"landscape_local_height\","
					"\"heights\":[0,0,0,0,0,0,0,0,"
					"0,0,0,0,0,0,0,0,"
					"0,0,0,0,0,0,0,0,"
					"0,0,0,0,0,0,0,0,"
					"0,0,0,0,0,0,0,0,"
					"0,0,0,0,0,0,0,0,"
					"0,0,0,0,0,0,0,0,"
					"0,0,0,0,0,0,0,0],"
					"\"scale\":[100,200,50],"
					"\"target_ref\":\"uai://landscape/owned\","
					"\"transform\":{\"rotation_quaternion_xyzw\":"
					"[0,0,0.7071067811865476,"
					"0.7071067811865476],"
					"\"translation\":[1000,2000,300]}}"));
			FUeAgentCanonicalValuePtr Current =
				CanonicalInput(
					Test,
					CurrentJson,
					Resources);
			FUeAgentCanonicalValuePtr Default;
			FUeAgentStateSemanticError ValueError;
			if (!Current.IsValid()
				|| !FUeAgentCanonicalValue::TryObject(
					{},
					Resources,
					Default,
					ValueError))
			{
				return nullptr;
			}
			Entries.Emplace(
				FUeAgentCanonicalIdentity(
					TEXT("uai.landscape.current:")
						+ Descriptor.GetCapabilityId(),
					TEXT(
						"landscape.geometry.height_delta"),
					World.GetPathName(),
					TEXT("uai://landscape/owned"),
					TEXT("height_field"),
					TEXT("owned-worker:r1")),
				FUeAgentCanonicalType(
					TEXT("landscape_geometry_current_state"),
					EUeAgentValueKind::Object,
					false,
					TEXT("object")),
				Current.ToSharedRef(),
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::Schema,
					TEXT(
						"landscape-geometry:e2:empty-default"),
					TEXT(
						"test-only default; before is independent readback"),
					Default.ToSharedRef()),
				FUeAgentStateDetails(
					TEXT("uai://landscape/owned"),
					TEXT(""),
					{TEXT("LandscapeGeometryEffect")},
					true,
					TEXT("available"),
					{}),
				Revision);
		}

		FUeAgentCanonicalSnapshotPtr Snapshot;
		FUeAgentStateSemanticError Error;
		if (!Test->TestTrue(
				TEXT("create independent Landscape snapshot"),
				FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("snapshot:landscape-geometry-e2:")
						+ Descriptor.GetCapabilityId(),
					EUeAgentStatePlane::AuthoringMemory,
					Revision,
					FUeAgentTemporalPoint(
						TEXT("owned-worker-clock"),
						TEXT("1")),
					FUeAgentStateProvenance(
						TEXT("independent_readback"),
						TEXT("owned-landscape-native-api"),
						TEXT(
							"landscape-geometry-e2:prestate-readback")),
					Entries,
					Resources,
					Snapshot,
					Error)))
		{
			Test->AddError(
				TEXT("landscape_snapshot_failed:")
				+ Error.Code);
			return nullptr;
		}
		return Snapshot;
	}

	FUeAgentExecutionContextRefOwner MakeExecutionContext(
		const FUeAgentDescriptorIdentity& Descriptor,
		UWorld& World)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId =
			TEXT("project:landscape-geometry-e2");
		Input.EditorInstanceId =
			TEXT("editor:owned-worker");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = EUeAgentExecutionWorldType::Preview;
		Input.WorldIdentity = World.GetPathName();
		Input.LevelIdentity =
			World.PersistentLevel->GetPathName();
		Input.ContextRevision = FString::Printf(
			TEXT("owned-world:%u"),
			World.GetUniqueID());
		Input.bResolvedFromCurrent = false;

		FUeAgentExecutionContextRefPtr Context;
		FUeAgentExecutionContextError Error;
		check(FUeAgentExecutionContextRef::TryCreate(
			Input,
			Descriptor,
			EUeAgentRequestKind::Operation,
			FUeAgentExecutionContextPolicy(
				{EUeAgentStatePlane::AuthoringMemory},
				{
					EUeAgentExecutionContextFacet::Level,
					EUeAgentExecutionContextFacet::World,
				},
				EUeAgentContextResolutionPolicy::ExplicitOnly,
				EUeAgentTransientContextPolicy::
					TemporaryMutationAllowed),
			Context,
			Error));
		return Context.ToSharedRef();
	}

	bool HasCanonicalObjectField(
		const FUeAgentCanonicalValuePtr& Value,
		const FString& Field)
	{
		if (!Value.IsValid()
			|| Value->GetKind() != EUeAgentValueKind::Object)
		{
			return false;
		}
		for (const TPair<
			FString,
			FUeAgentCanonicalValueRef>& Item
			: Value->GetObjectItems())
		{
			if (Item.Key == Field)
			{
				return true;
			}
		}
		return false;
	}

	bool BuildDraft(
		FAutomationTestBase* Test,
		const FUeAgentOperationDescriptor& Descriptor,
		UWorld& World,
		const FString& InputJson,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		FUeAgentResourceTracker& Resources,
		TOptional<FChangeSetDraft>& OutDraft,
		FExecutionMutationError& OutError)
	{
		TSharedPtr<const FUeAgentContractSchema> Compiled;
		FUeAgentContractSchemaError SchemaError;
		if (!Test->TestTrue(
				*(Descriptor.GetIdentity().GetDescriptorId()
					+ TEXT(" input schema compiles")),
				FUeAgentContractSchema::TryCompile(
					Descriptor.GetInputSchema(),
					Compiled,
					SchemaError))
			|| !Compiled.IsValid())
		{
			return false;
		}

		TOptional<FUeAgentContractValue> CanonicalPayload;
		if (!Test->TestTrue(
				*(Descriptor.GetIdentity().GetDescriptorId()
					+ TEXT(" input canonicalizes once in C0")),
				Compiled->TryValidateAndCanonicalize(
					FUeAgentContractValue(InputJson),
					[](int64,
						const FString&,
						FUeAgentContractSchemaError&)
					{
						return true;
					},
					CanonicalPayload,
					SchemaError))
			|| !CanonicalPayload.IsSet())
		{
			return false;
		}

		const FUeAgentExecutionContextRefOwner Context =
			MakeExecutionContext(
				Descriptor.GetIdentity(),
				World);
		return TryBuildLandscapeGeometryEffectChangeSetDraft(
			FUeAgentOperationRequest(
				Descriptor.GetIdentity(),
				TEXT("request:landscape-geometry-e2"),
				TEXT("idempotency:landscape-geometry-e2"),
				CanonicalPayload.GetValue()),
			*Context,
			Snapshot,
			Resources,
			OutDraft,
			OutError);
	}

	struct FNativeRequest
	{
		FNativeRequest(
			TSharedRef<
				FUeAgentResourceTracker,
				ESPMode::ThreadSafe> InResources,
			FPlanningPreflightPlanRef InPlan,
			FUeAgentExecutionContextRefOwner InContext)
			: Resources(MoveTemp(InResources))
			, Cancellation([this]() { return Now; })
			, Plan(MoveTemp(InPlan))
			, Context(MoveTemp(InContext))
		{
		}

		FExecutionMutationRequest MakeRequest()
		{
			return FExecutionMutationRequest(
				TEXT("operation:landscape-geometry-e2"),
				TEXT("owner:landscape-geometry-e2"),
				Plan,
				Context,
				Resources,
				FUeAgentOperationDeadline::FromTimeout(
					30.0,
					[this]() { return Now; }),
				Cancellation.GetToken(),
				EExecutionThreadAffinity::GameThread,
				EExecutionReentrancyPolicy::RejectSameTarget);
		}

		double Now = FPlatformTime::Seconds();
		TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			Resources;
		FUeAgentCancellationSource Cancellation;
		FPlanningPreflightPlanRef Plan;
		FUeAgentExecutionContextRefOwner Context;
	};

	FUeAgentGenericEffectBinding MakeBinding(
		const FUeAgentOperationDescriptor& Descriptor,
		const FUeAgentCanonicalIdentity& Identity)
	{
		return FUeAgentGenericEffectBinding(
			FUeAgentCandidateBinding(
				TEXT("binding:")
					+ Descriptor.GetCapabilityId(),
				FUeAgentCandidateSummary(
					Identity,
					Descriptor.GetIdentity(),
					TEXT("landscape_geometry"),
					Descriptor.GetCapabilityId(),
					TEXT("owned Landscape geometry target"),
					{
						TEXT("Landscape"),
						TEXT("Geometry"),
					}),
				FUeAgentCatalogRevision(
					TEXT("landscape-geometry-e2"),
					TEXT("owned-worker:r1"),
					1),
				TEXT("uai://landscape/")
					+ Descriptor.GetCapabilityId(),
				TEXT("landscape-read"),
				{
					EUeAgentCandidateOperation::Create,
					EUeAgentCandidateOperation::Update,
				},
				TEXT(
					"fixture:landscape-geometry-e2:binding")),
			TEXT("UAI.LandscapeGeometry:CanonicalObject"),
			FUeAgentCanonicalType(
				TEXT("landscape_geometry_canonical_object"),
				EUeAgentValueKind::Object,
				false,
				TEXT("object")),
			TEXT(
				"fixture:landscape-geometry-e2:type-binding"));
	}

	struct FAppliedCapability
	{
		TSharedPtr<
			IUeAgentLandscapeGeometryEffectNativeHook> Hook;
		TUniquePtr<FNativeRequest> Fixture;
		TOptional<FExecutionMutationRequest> Request;
		FUeAgentGenericEffectApplyResult Result;
	};

	bool ApplyCapability(
		FAutomationTestBase* Test,
		const TSharedRef<
			FOwnedReferenceResolver,
			ESPMode::ThreadSafe>& Resolver,
		const TSharedRef<const FUeAgentDescriptorRegistry>& Registry,
		UWorld& World,
		const FString& DescriptorId,
		const FString& InputJson,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		FAppliedCapability& OutApplied,
		const bool bExpectApply = true)
	{
		const FUeAgentOperationDescriptor* Descriptor =
			Registry->FindByDescriptorId(DescriptorId);
		if (!Test->TestNotNull(
				*(DescriptorId + TEXT(" descriptor exists")),
				Descriptor))
		{
			return false;
		}

		const TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> Resources =
				MakeResources();
		TOptional<FChangeSetDraft> Draft;
		FExecutionMutationError Error;
		if (!BuildDraft(
				Test,
				*Descriptor,
				World,
				InputJson,
				Snapshot,
				*Resources,
				Draft,
				Error)
			|| !Draft.IsSet()
			|| !Draft->IsValid())
		{
			Test->AddError(
				DescriptorId + TEXT(":")
				+ Error.GetCode());
			return false;
		}

		FPlanningResult PlanningResult;
		FPlanningPreflightError PlanningError;
		if (!Test->TestTrue(
				*(DescriptorId + TEXT(" freezes through C6")),
				TryPlanChanges(
					FPlanningRequest(
						Draft.GetValue(),
						FUeAgentCallerContext(
							TEXT(
								"agent:landscape-geometry-e2"),
							{TEXT("uai:landscape:write")},
							TEXT(
								"correlation:landscape-geometry-e2")),
						false),
					*Resources,
					PlanningResult,
					PlanningError))
			|| !PlanningResult.IsReady()
			|| !PlanningResult.GetPlan().IsValid())
		{
			return false;
		}

		TSharedPtr<
			IUeAgentLandscapeGeometryEffectNativeHook> Hook;
		if (!Test->TestTrue(
				*(DescriptorId + TEXT(" native hook creates")),
				FUeAgentLandscapeGeometryEffectNativeHookFactory::
					TryCreate(
						Resolver,
						Hook,
						Error))
			|| !Hook.IsValid())
		{
			return false;
		}

		const FUeAgentExecutionContextRefOwner Context =
			MakeExecutionContext(
				Descriptor->GetIdentity(),
				World);
		OutApplied.Fixture = MakeUnique<FNativeRequest>(
			Resources,
			PlanningResult.GetPlan().ToSharedRef(),
			Context);
		OutApplied.Request.Emplace(
			OutApplied.Fixture->MakeRequest());
		OutApplied.Hook = MoveTemp(Hook);

		TArray<FUeAgentGenericEffectBinding> Bindings;
		for (const FPlannedChange& Change
			: OutApplied.Fixture->Plan
				->GetChangeSet()
				.GetChanges())
		{
			Bindings.Add(
				MakeBinding(
					*Descriptor,
					Change.GetIdentity()));
		}

		const bool bApplied = OutApplied.Hook->TryApply(
			World,
			OutApplied.Request.GetValue(),
			FLockFenceLease(
				TEXT("owner:landscape-geometry-e2"),
				{World.GetPathName()},
				1),
			Bindings,
			*OutApplied.Fixture->Resources,
			OutApplied.Result,
			Error);
		Test->TestEqual(
			*(DescriptorId
				+ TEXT(" native apply matches expectation")),
			bApplied,
			bExpectApply);
		Test->TestEqual(
			*(DescriptorId
				+ TEXT(" mutation fact matches expectation")),
			OutApplied.Result.DidMutationStart(),
			bExpectApply);
		if (bApplied != bExpectApply)
		{
			Test->AddError(
				DescriptorId + TEXT(":")
				+ Error.GetCode());
		}
		return bApplied;
	}

	void RollbackCapability(
		FAutomationTestBase* Test,
		UWorld& World,
		const FString& DescriptorId,
		FAppliedCapability& Applied)
	{
		bool bConfirmed = false;
		FExecutionMutationError Error;
		Test->TestTrue(
			*(DescriptorId + TEXT(" rollback executes")),
			Applied.Hook->TryRollback(
				World,
				Applied.Request.GetValue(),
				Applied.Result,
				*Applied.Fixture->Resources,
				bConfirmed,
				Error));
		Test->TestTrue(
			*(DescriptorId + TEXT(" rollback is confirmed")),
			bConfirmed);
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLandscapeGeometryEffectTypedPlanningTest,
	"UeAgentInterface.DomainAdapters.LandscapeGeometryEffect.E1."
		"TypedPlanningAndPreflight",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentLandscapeGeometryEffectTypedPlanningTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	if (!TestTrue(
			TEXT("create LandscapeGeometryEffect registry"),
			TryCreateLandscapeGeometryEffectDescriptorRegistry(
				Registry,
				RegistryError))
		|| !Registry.IsValid())
	{
		return false;
	}

	FOwnedLandscapeWorld EmptyOwner(false);
	FOwnedLandscapeWorld ExistingOwner(true);
	if (!TestTrue(
			TEXT("empty owned world is valid"),
			EmptyOwner.IsValid(false))
		|| !ValidateOwnedFixture(this, ExistingOwner))
	{
		return false;
	}

	struct FPlanningCase
	{
		FString DescriptorId;
		FString Input;
		UWorld* World = nullptr;
		ALandscape* CurrentLandscape = nullptr;
		FString ExpectedTargetKind;
		EChangeIntent ExpectedIntent = EChangeIntent::Create;
	};
	const TArray<FPlanningCase> Cases = {
		{
			TEXT("uai.operation.landscape.create"),
			CreateInput,
			EmptyOwner.World,
			nullptr,
			TEXT("landscape.geometry.create"),
			EChangeIntent::Create,
		},
		{
			TEXT("uai.operation.landscape.height_delta"),
			HeightDeltaInput,
			ExistingOwner.World,
			ExistingOwner.Landscape,
			TEXT("landscape.geometry.height_delta"),
			EChangeIntent::Update,
		},
	};

	for (const FPlanningCase& Case : Cases)
	{
		const FUeAgentOperationDescriptor* Descriptor =
			Registry->FindByDescriptorId(Case.DescriptorId);
		if (!TestNotNull(
				*(Case.DescriptorId + TEXT(" exists")),
				Descriptor))
		{
			continue;
		}

		const TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> Resources =
				MakeResources();
		const FUeAgentCanonicalSnapshotPtr Snapshot =
			MakeSnapshot(
				this,
				*Descriptor,
				*Case.World,
				Case.CurrentLandscape,
				*Resources);
		if (!Snapshot.IsValid())
		{
			continue;
		}

		TOptional<FChangeSetDraft> Draft;
		FExecutionMutationError Error;
		TestTrue(
			*(Case.DescriptorId
				+ TEXT(" builds a typed draft")),
			BuildDraft(
				this,
				*Descriptor,
				*Case.World,
				Case.Input,
				Snapshot.ToSharedRef(),
				*Resources,
				Draft,
				Error));
		if (!Draft.IsSet() || !Draft->IsValid())
		{
			AddError(
				Case.DescriptorId + TEXT(":")
				+ Error.GetCode());
			continue;
		}

		TestEqual(
			*(Case.DescriptorId
				+ TEXT(" produces one atomic change")),
			Draft->GetChanges().Num(),
			1);
		const FChangeDraft& Change =
			Draft->GetChanges()[0];
		TestEqual(
			*(Case.DescriptorId
				+ TEXT(" uses a Landscape-only target kind")),
			Change.GetIdentity().GetTargetKind(),
			Case.ExpectedTargetKind);
		TestEqual(
			*(Case.DescriptorId
				+ TEXT(" preserves CRUD intent")),
			Change.GetIntent(),
			Case.ExpectedIntent);
		TestEqual(
			*(Case.DescriptorId
				+ TEXT(" before-state presence is exact")),
			Change.GetExpectedBefore().IsValid(),
			Case.ExpectedIntent != EChangeIntent::Create);
		for (const FString& PublicField
			: TArray<FString>{
				TEXT("delta"),
				TEXT("initial_height"),
				TEXT("placement"),
				TEXT("sample_region"),
				TEXT("space_contract"),
				TEXT("topology"),
			})
		{
			TestFalse(
				*(Case.DescriptorId
					+ TEXT(
						" removes public envelope before native")),
				HasCanonicalObjectField(
					Change.GetProposedAfter(),
					PublicField));
		}
		for (const FString& NativeField
			: TArray<FString>{
				TEXT("target_ref"),
				TEXT("write_frame_id"),
				TEXT("conversion_evidence"),
			})
		{
			TestTrue(
				*(Case.DescriptorId
					+ TEXT(" freezes native spatial evidence")),
				HasCanonicalObjectField(
					Change.GetProposedAfter(),
					NativeField));
		}
	}

	const FUeAgentOperationDescriptor* CreateDescriptor =
		Registry->FindByDescriptorId(
			TEXT("uai.operation.landscape.create"));
	const FUeAgentOperationDescriptor* DeltaDescriptor =
		Registry->FindByDescriptorId(
			TEXT("uai.operation.landscape.height_delta"));
	if (!CreateDescriptor || !DeltaDescriptor)
	{
		return false;
	}

	const TSharedRef<
		FUeAgentResourceTracker,
		ESPMode::ThreadSafe> InvalidResources =
			MakeResources();
	const FUeAgentCanonicalSnapshotPtr EmptySnapshot =
		MakeSnapshot(
			this,
			*CreateDescriptor,
			*EmptyOwner.World,
			nullptr,
			*InvalidResources);
	const FUeAgentCanonicalSnapshotPtr ExistingSnapshot =
		MakeSnapshot(
			this,
			*DeltaDescriptor,
			*ExistingOwner.World,
			ExistingOwner.Landscape,
			*InvalidResources);
	if (!EmptySnapshot.IsValid()
		|| !ExistingSnapshot.IsValid())
	{
		return false;
	}

	struct FNegativeCase
	{
		const FUeAgentOperationDescriptor* Descriptor = nullptr;
		UWorld* World = nullptr;
		FUeAgentCanonicalSnapshotRef Snapshot;
		FString Input;
		FString ExpectedError;
	};
	const TArray<FNegativeCase> NegativeCases = {
		{
			CreateDescriptor,
			EmptyOwner.World,
			EmptySnapshot.ToSharedRef(),
			FString(CreateInput).Replace(
				TEXT("\"max_inclusive\":[7,7]"),
				TEXT("\"max_inclusive\":[6,7]")),
			TEXT("landscape_create_region_topology_mismatch"),
		},
		{
			DeltaDescriptor,
			ExistingOwner.World,
			ExistingSnapshot.ToSharedRef(),
			FString(HeightDeltaInput).Replace(
				TEXT("\"values\":[1,2,3,4,5,6]"),
				TEXT("\"values\":[1,2,3,4,5]")),
			TEXT(
				"landscape_height_delta_sample_count_mismatch"),
		},
		{
			DeltaDescriptor,
			ExistingOwner.World,
			ExistingSnapshot.ToSharedRef(),
			FString(HeightDeltaInput).Replace(
				TEXT("\"expected_scale\":[100,200,50]"),
				TEXT("\"expected_scale\":[100,100,50]")),
			TEXT("landscape_height_delta_scale_stale"),
		},
		{
			DeltaDescriptor,
			ExistingOwner.World,
			ExistingSnapshot.ToSharedRef(),
			FString(HeightDeltaInput).Replace(
				TEXT("\"max_inclusive\":[4,2]"),
				TEXT("\"max_inclusive\":[8,2]")),
			TEXT(
				"landscape_height_delta_region_out_of_bounds"),
		},
	};

	for (const FNegativeCase& Case : NegativeCases)
	{
		TOptional<FChangeSetDraft> Draft;
		FExecutionMutationError Error;
		TestFalse(
			*(Case.ExpectedError
				+ TEXT(" fails before C6 and mutation")),
			BuildDraft(
				this,
				*Case.Descriptor,
				*Case.World,
				Case.Input,
				Case.Snapshot,
				*InvalidResources,
				Draft,
				Error));
		TestEqual(
			*(Case.ExpectedError
				+ TEXT(" is a stable failure")),
			Error.GetCode(),
			Case.ExpectedError);
		TestFalse(
			*(Case.ExpectedError
				+ TEXT(" produces no draft")),
			Draft.IsSet());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLandscapeGeometryEffectOwnedWorldTest,
	"UeAgentInterface.DomainAdapters.LandscapeGeometryEffect.E2."
		"OwnedWorldReadbackRollback",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentLandscapeGeometryEffectOwnedWorldTest::RunTest(
	const FString& Parameters)
{
	if (!TestTrue(
			TEXT("Landscape native test owns the game thread"),
			IsInGameThread()))
	{
		return false;
	}

	FOwnedLandscapeWorld ExistingOwner(true);
	if (!ValidateOwnedFixture(this, ExistingOwner))
	{
		return false;
	}

	FOwnedLandscapeWorld EmptyOwner(false);
	if (!TestTrue(
			TEXT("empty owned Landscape world is valid"),
			EmptyOwner.IsValid(false)))
	{
		return false;
	}

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	if (!TestTrue(
			TEXT("create LandscapeGeometryEffect registry"),
			TryCreateLandscapeGeometryEffectDescriptorRegistry(
				Registry,
				RegistryError))
		|| !Registry.IsValid())
	{
		return false;
	}

	const TSharedRef<
		FOwnedReferenceResolver,
		ESPMode::ThreadSafe> Resolver =
			MakeShared<
				FOwnedReferenceResolver,
				ESPMode::ThreadSafe>();
	Resolver->Bind(
		TEXT("uai://landscape/owned"),
		ExistingOwner.Landscape);

	const FUeAgentOperationDescriptor* CreateDescriptor =
		Registry->FindByDescriptorId(
			TEXT("uai.operation.landscape.create"));
	const FUeAgentOperationDescriptor* DeltaDescriptor =
		Registry->FindByDescriptorId(
			TEXT("uai.operation.landscape.height_delta"));
	if (!TestNotNull(
			TEXT("create descriptor exists"),
			CreateDescriptor)
		|| !TestNotNull(
			TEXT("height-delta descriptor exists"),
			DeltaDescriptor))
	{
		return false;
	}

	const TSharedRef<
		FUeAgentResourceTracker,
		ESPMode::ThreadSafe> SnapshotResources =
			MakeResources();
	const FUeAgentCanonicalSnapshotPtr EmptySnapshot =
		MakeSnapshot(
			this,
			*CreateDescriptor,
			*EmptyOwner.World,
			nullptr,
			*SnapshotResources);
	const FUeAgentCanonicalSnapshotPtr ExistingSnapshot =
		MakeSnapshot(
			this,
			*DeltaDescriptor,
			*ExistingOwner.World,
			ExistingOwner.Landscape,
			*SnapshotResources);
	if (!EmptySnapshot.IsValid()
		|| !ExistingSnapshot.IsValid())
	{
		return false;
	}

	const bool bCreateDirtyBefore =
		EmptyOwner.Package->IsDirty();
	FAppliedCapability Applied;
	if (ApplyCapability(
			this,
			Resolver,
			Registry.ToSharedRef(),
			*EmptyOwner.World,
			TEXT("uai.operation.landscape.create"),
			CreateInput,
			EmptySnapshot.ToSharedRef(),
			Applied))
	{
		TestEqual(
			TEXT("create produces one exact Landscape"),
			EmptyOwner.CountLandscapes(),
			1);
		ALandscape* Created =
			EmptyOwner.FindOnlyLandscape();
		if (TestNotNull(
				TEXT("created Landscape reads independently"),
				Created))
		{
			TestTrue(
				TEXT("create placement translation reads back"),
				Created->GetActorLocation().Equals(
					FVector(1000.0, 2000.0, 300.0)));
			TestTrue(
				TEXT("create placement rotation reads back"),
				Created->GetActorRotation().Equals(
					FRotator(0.0, 90.0, 0.0),
					0.01));
			TestTrue(
				TEXT("create non-uniform scale reads back"),
				Created->GetActorScale3D().Equals(
					FVector(100.0, 200.0, 50.0)));
			int32 MinX = 0;
			int32 MinY = 0;
			int32 MaxX = 0;
			int32 MaxY = 0;
			TestTrue(
				TEXT("create sample extent reads independently"),
				EmptyOwner.ReadExtent(
					*Created,
					MinX,
					MinY,
					MaxX,
					MaxY));
			TestEqual(TEXT("created min x"), MinX, 0);
			TestEqual(TEXT("created min y"), MinY, 0);
			TestEqual(TEXT("created max x"), MaxX, 7);
			TestEqual(TEXT("created max y"), MaxY, 7);
		}
		RollbackCapability(
			this,
			*EmptyOwner.World,
			TEXT("uai.operation.landscape.create"),
			Applied);
		TestEqual(
			TEXT("create rollback removes Landscape"),
			EmptyOwner.CountLandscapes(),
			0);
		TestEqual(
			TEXT("rollback restores package dirty state"),
			EmptyOwner.Package->IsDirty(),
			bCreateDirtyBefore);
	}

	TArray<float> HeightsBefore;
	if (!TestTrue(
			TEXT("height-delta prestate reads independently"),
			ExistingOwner.ReadLocalHeights(
				*ExistingOwner.Landscape,
				HeightsBefore)))
	{
		return false;
	}

	Applied = {};
	if (ApplyCapability(
			this,
			Resolver,
			Registry.ToSharedRef(),
			*ExistingOwner.World,
			TEXT("uai.operation.landscape.height_delta"),
			HeightDeltaInput,
			ExistingSnapshot.ToSharedRef(),
			Applied))
	{
		TArray<float> HeightsAfter;
		TestTrue(
			TEXT("height-delta reads through Landscape API"),
			ExistingOwner.ReadLocalHeights(
				*ExistingOwner.Landscape,
				HeightsAfter));
		const TArray<float> ExpectedDelta = {
			1.0f,
			2.0f,
			3.0f,
			4.0f,
			5.0f,
			6.0f,
		};
		int32 DeltaIndex = 0;
		for (int32 Y = 0; Y < LandscapeSize; ++Y)
		{
			for (int32 X = 0; X < LandscapeSize; ++X)
			{
				const int32 Index =
					Y * LandscapeSize + X;
				const bool bInside =
					X >= 2 && X <= 4
					&& Y >= 1 && Y <= 2;
				const float Expected = bInside
					? HeightsBefore[Index]
						+ ExpectedDelta[DeltaIndex++]
					: HeightsBefore[Index];
				TestTrue(
					*FString::Printf(
						TEXT(
							"height sample %d,%d matches independent oracle"),
						X,
						Y),
					FMath::IsNearlyEqual(
						HeightsAfter[Index],
						Expected,
						0.01f));
			}
		}

		RollbackCapability(
			this,
			*ExistingOwner.World,
			TEXT(
				"uai.operation.landscape.height_delta"),
			Applied);
		TArray<float> HeightsRolledBack;
		TestTrue(
			TEXT("height rollback reads independently"),
			ExistingOwner.ReadLocalHeights(
				*ExistingOwner.Landscape,
				HeightsRolledBack));
		TestEqual(
			TEXT("height rollback restores sample count"),
			HeightsRolledBack.Num(),
			HeightsBefore.Num());
		for (int32 Index = 0;
			Index < HeightsBefore.Num()
				&& Index < HeightsRolledBack.Num();
			++Index)
		{
			TestTrue(
				*FString::Printf(
					TEXT("height rollback restores sample %d"),
					Index),
				FMath::IsNearlyEqual(
					HeightsRolledBack[Index],
					HeightsBefore[Index],
					0.01f));
		}
	}

	const TSharedRef<
		FOwnedReferenceResolver,
		ESPMode::ThreadSafe> MissingResolver =
			MakeShared<
				FOwnedReferenceResolver,
				ESPMode::ThreadSafe>();
	TArray<float> HeightsBeforeRejected;
	ExistingOwner.ReadLocalHeights(
		*ExistingOwner.Landscape,
		HeightsBeforeRejected);
	Applied = {};
	ApplyCapability(
		this,
		MissingResolver,
		Registry.ToSharedRef(),
		*ExistingOwner.World,
		TEXT("uai.operation.landscape.height_delta"),
		HeightDeltaInput,
		ExistingSnapshot.ToSharedRef(),
		Applied,
		false);
	TArray<float> HeightsAfterRejected;
	ExistingOwner.ReadLocalHeights(
		*ExistingOwner.Landscape,
		HeightsAfterRejected);
	TestEqual(
		TEXT("unresolved reference preserves sample count"),
		HeightsAfterRejected.Num(),
		HeightsBeforeRejected.Num());
	for (int32 Index = 0;
		Index < HeightsBeforeRejected.Num()
			&& Index < HeightsAfterRejected.Num();
		++Index)
	{
		TestTrue(
			*FString::Printf(
				TEXT(
					"unresolved reference performs zero height mutation %d"),
				Index),
			FMath::IsNearlyEqual(
				HeightsAfterRejected[Index],
				HeightsBeforeRejected[Index],
				0.01f));
	}

	return !HasAnyErrors();
}
