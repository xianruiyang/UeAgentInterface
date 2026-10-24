#include "Misc/AutomationTest.h"

#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/Landscape/UeAgentLandscapeHeightQuery.h"
#include "DomainAdapters/Landscape/UeAgentLandscapeHeightState.h"
#include "DomainAdapters/WorldRead/UeAgentWorldReadNative.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Landscape.h"
#include "LandscapeDataAccess.h"
#include "LandscapeInfo.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::Landscape;
	using namespace UE::UAI::DomainAdapters::WorldRead;
	using namespace UE::UAI::StateSemantic;

	const FString ManagedTarget(
		TEXT("uai://landscape/height-query-owned"));

	class FOwnedLandscapeHeightWorld final
	{
	public:
		FOwnedLandscapeHeightWorld()
		{
			Package = CreatePackage(
				*(TEXT("/Temp/UAI/LandscapeHeightRead/")
					+ FGuid::NewGuid().ToString(
						EGuidFormats::Digits)));
			World = UWorld::CreateWorld(
				EWorldType::EditorPreview,
				false,
				FName(TEXT("LandscapeHeightReadOwnedWorld")),
				Package);
			if (!World || !World->PersistentLevel)
			{
				return;
			}
			World->SetCurrentLevel(World->PersistentLevel);
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnParameters.ObjectFlags |= RF_Transactional;
			Landscape = World->SpawnActor<ALandscape>(
				FVector(1000.0, 2000.0, 300.0),
				FRotator(0.0, 90.0, 0.0),
				SpawnParameters);
			if (!Landscape)
			{
				return;
			}
			Landscape->SetActorScale3D(
				FVector(100.0, 200.0, 50.0));
			Landscape->SetActorLabel(
				TEXT("height-query-owned"));
			FString OwnershipError;
			if (!UeAgentLevelAuthoring::TryBindManagedActorOwnership(
					*Landscape,
					ManagedTarget,
					TEXT("landscape"),
					OwnershipError))
			{
				return;
			}

			TArray<uint16> Heights;
			Heights.Init(
				LandscapeDataAccess::GetTexHeight(3.0f),
				64);
			TMap<FGuid, TArray<uint16>> HeightsByLayer;
			HeightsByLayer.Add(FGuid(), MoveTemp(Heights));
			TMap<FGuid, TArray<FLandscapeImportLayerInfo>>
				MaterialLayersByLayer;
			MaterialLayersByLayer.Add(
				FGuid(),
				TArray<FLandscapeImportLayerInfo>());
			const TArrayView<const FLandscapeLayer> EmptyLayers;
			Landscape->Import(
				FGuid::NewGuid(),
				0,
				0,
				7,
				7,
				1,
				7,
				HeightsByLayer,
				TEXT(""),
				MaterialLayersByLayer,
				ELandscapeImportAlphamapType::Additive,
				EmptyLayers);
			Landscape->CreateLandscapeInfo();
			Landscape->PostEditChange();
			Package->SetDirtyFlag(false);
		}

		~FOwnedLandscapeHeightWorld()
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

		bool IsValid() const
		{
			return Package
				&& World
				&& Landscape
				&& Landscape->GetLandscapeInfo();
		}

		UPackage* Package = nullptr;
		UWorld* World = nullptr;
		ALandscape* Landscape = nullptr;
	};

	class FOwnedLandscapeHeightEnvironment final
		: public IUeAgentWorldReadEnvironment
	{
	public:
		explicit FOwnedLandscapeHeightEnvironment(
			FOwnedLandscapeHeightWorld& InOwner)
			: Owner(InOwner)
		{
		}

		bool TryResolveWorld(
			const FUeAgentExecutionContextRef& Context,
			UWorld*& OutWorld,
			FUeAgentWorldReadError& OutError) override
		{
			OutWorld = Context.GetWorldIdentity()
					== Owner.World->GetPathName()
				? Owner.World
				: nullptr;
			if (OutWorld)
			{
				return true;
			}
			OutError.Code = TEXT("landscape_height_fixture_world_mismatch");
			OutError.SubjectId = Context.GetWorldIdentity();
			return false;
		}

		bool TryResolveTarget(
			const FUeAgentCandidateBinding& Target,
			UObject*& OutObject,
			FUeAgentWorldReadError& OutError) override
		{
			OutObject = nullptr;
			OutError.Code = TEXT("landscape_height_parallel_target_resolver_used");
			OutError.SubjectId = Target.GetPublicRef();
			return false;
		}

	private:
		FOwnedLandscapeHeightWorld& Owner;
	};

	FUeAgentDescriptorIdentity LandscapeHeightIdentity()
	{
		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString Error;
		if (TryCreateLandscapeHeightReadDescriptorRegistry(Registry, Error)
			&& Registry.IsValid())
		{
			const FUeAgentOperationDescriptor* Descriptor =
				Registry->FindByDescriptorId(
					TEXT("uai.query.landscape.height.read"));
			if (Descriptor)
			{
				return Descriptor->GetIdentity();
			}
		}
		return FUeAgentDescriptorIdentity(FString(), 0, FString());
	}

	FUeAgentResourceTrackerOwner LandscapeHeightResources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
	}

	TSharedPtr<FUeAgentOperationContext> LandscapeHeightContext(
		FAutomationTestBase* Test,
		UWorld& World,
		const FUeAgentResourceTrackerOwner& Resources)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("fixture-project");
		Input.EditorInstanceId = TEXT("fixture-editor");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = EUeAgentExecutionWorldType::Preview;
		Input.WorldIdentity = World.GetPathName();
		Input.LevelIdentity = World.PersistentLevel->GetPathName();
		Input.ContextRevision = TEXT("landscape-height-owned-revision");

		const FUeAgentExecutionContextPolicy Policy(
			{EUeAgentStatePlane::AuthoringMemory},
			{EUeAgentExecutionContextFacet::World},
			EUeAgentContextResolutionPolicy::ResolveCurrentThenFreeze,
			EUeAgentTransientContextPolicy::ReadOnly);
		FUeAgentExecutionContextRefPtr Frozen;
		FUeAgentExecutionContextError ContextError;
		Test->TestTrue(
			TEXT("Landscape height fixture context freezes"),
			FUeAgentExecutionContextRef::TryCreate(
				Input,
				LandscapeHeightIdentity(),
				EUeAgentRequestKind::Query,
				Policy,
				Frozen,
				ContextError));
		if (!Frozen.IsValid())
		{
			return nullptr;
		}

		const FUeAgentQueryRequest Request(
			LandscapeHeightIdentity(),
			TEXT("request-landscape-height-native"),
			FUeAgentContractValue(TEXT("{}")));
		const FUeAgentCallerContext Caller(
			TEXT("fixture-agent"),
			{TEXT("uai:query")},
			TEXT("corr-landscape-height-native"));
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(
				30.0,
				[]() { return 100.0; });
		const FUeAgentCancellationSource Cancellation(
			[]() { return 100.0; });
		TSharedPtr<FUeAgentOperationContext> Context;
		FUeAgentOperationContextError OperationError;
		Test->TestTrue(
			TEXT("Landscape height fixture operation context creates"),
			FUeAgentOperationContext::TryCreate(
				Request,
				Caller,
				Frozen.ToSharedRef(),
				Resources,
				Deadline,
				Cancellation.GetToken(),
				Context,
				OperationError));
		return Context;
	}

	FUeAgentCandidateBinding LandscapeHeightTarget()
	{
		return FUeAgentCandidateBinding(
			TEXT("landscape-height-owned-target"),
			FUeAgentCandidateSummary(
				FUeAgentCanonicalIdentity(
					TEXT("landscape.height.target:owned"),
					TEXT("landscape"),
					TEXT("owned-world"),
					TEXT("height"),
					TEXT("target"),
					TEXT("landscape-height-owned-revision")),
				LandscapeHeightIdentity(),
				TEXT("landscape_height"),
				TEXT("height-query-owned"),
				TEXT("owned Landscape height target"),
				{TEXT("Landscape"), TEXT("Query")}),
			FUeAgentCatalogRevision(
				TEXT("landscape-height-owned-catalog"),
				TEXT("landscape-height-owned-revision"),
				1),
			ManagedTarget,
			TEXT("landscape-height-read"),
			{EUeAgentCandidateOperation::Read},
			TEXT("exact managed Landscape target"));
	}

	const TCHAR* ValidInput = TEXT(
		"{\"region\":{\"frame_id\":"
		"\"ue.landscape.local_sample_grid\","
		"\"max_inclusive\":[4,2],\"min_inclusive\":[2,1]},"
		"\"space_contract\":{\"basis_evidence\":"
		"\"owned Landscape extent and sample-grid contract\","
		"\"frame_id\":\"ue.landscape.local_sample_grid\","
		"\"unit\":\"sample\"}}" );

	bool CaptureLandscapeHeight(
		const TSharedRef<IUeAgentWorldReadEnvironment>& Environment,
		const FUeAgentOperationContext& Context,
		const FString& Input,
		TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
		FUeAgentWorldReadError& OutError)
	{
		TSharedPtr<IUeAgentWorldReadHook> Hook;
		return FUeAgentWorldReadNativeHookFactory::TryCreate(
				GetLandscapeHeightReadProviderBinding(),
				Environment,
				Hook,
				OutError)
			&& Hook.IsValid()
			&& Hook->TryCapture(
				FUeAgentContractValue(Input),
				LandscapeHeightTarget(),
				Context,
				OutSnapshot,
				OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLandscapeHeightReadOwnedWorldTest,
	"UeAgentInterface.DomainAdapters.Landscape.E2."
		"HeightReadOwnedWorld",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentLandscapeHeightReadOwnedWorldTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<const FUeAgentDescriptorRegistry> Descriptors;
	FString DescriptorError;
	if (!TestTrue(
			TEXT("Landscape height descriptor freezes with its canonical hash"),
			TryCreateLandscapeHeightReadDescriptorRegistry(
				Descriptors,
				DescriptorError))
		|| !Descriptors.IsValid())
	{
		AddError(DescriptorError);
		return false;
	}
	TestNotNull(
		TEXT("public route-derived Landscape descriptor identity exists"),
		Descriptors->FindByDescriptorId(
			TEXT("uai.query.landscape.height.read")));
	TestNull(
		TEXT("pre-route Landscape descriptor identity is absent"),
		Descriptors->FindByDescriptorId(
			TEXT("uai.query.landscape.height")));

	TArray<UE::UAI::BindingGateway::Query::
		FUeAgentDomainQueryProviderRegistration> ProviderRegistrations;
	FString ProviderError;
	TestTrue(
		TEXT("Landscape domain creates one production provider contribution"),
		TryCreateLandscapeHeightReadProviderRegistrations(
			ProviderRegistrations,
			ProviderError));
	TestEqual(
		TEXT("Landscape domain contributes exactly one provider"),
		ProviderRegistrations.Num(),
		1);

	UE::UAI::DomainCapability::FDomainCapabilityAuthorities Authorities;
	FString AuthorityError;
	TestTrue(
		TEXT("frozen DomainCapability builds the unique provider authority"),
		UE::UAI::DomainCapability::
			TryGetOrBuildProcessDomainCapabilityAuthorities(
				Authorities,
				AuthorityError));
	TestTrue(
		TEXT("frozen provider authority contains Landscape height"),
		Authorities.GetQueryProviderRegistry().IsValid()
			&& Authorities.GetQueryProviderRegistry()->FindExact(
				GetLandscapeHeightReadProviderBinding()).IsValid());

	FOwnedLandscapeHeightWorld Owner;
	if (!TestTrue(
			TEXT("owned Landscape height world is valid"),
			Owner.IsValid()))
	{
		return false;
	}

	ALandscape* Resolved = nullptr;
	FLandscapeHeightStateError StateError;
	TestTrue(
		TEXT("shared reader resolves the exact managed identity"),
		TryResolveLandscapeHeightTarget(
			*Owner.World,
			ManagedTarget,
			nullptr,
			Resolved,
			StateError));
	TestTrue(
		TEXT("exact managed identity resolves the owned Landscape"),
		Resolved == Owner.Landscape);

	FLandscapeHeightState DirectState;
	TestTrue(
		TEXT("Operation/Query shared reader captures the bounded region"),
		TryReadLandscapeHeightState(
			*Owner.Landscape,
			FIntPoint(2, 1),
			FIntPoint(4, 2),
			DirectState,
			StateError));
	TestEqual(
		TEXT("shared reader preserves row-major sample count"),
		DirectState.Heights.Num(),
		6);
	for (const double Height : DirectState.Heights)
	{
		TestTrue(
			TEXT("shared reader observes the real local height"),
			FMath::IsNearlyEqual(Height, 3.0));
	}
	TestFalse(
		TEXT("shared Landscape reader leaves the owned package clean"),
		Owner.Package->IsDirty());

	const FUeAgentResourceTrackerOwner Resources =
		LandscapeHeightResources();
	const TSharedPtr<FUeAgentOperationContext> Context =
		LandscapeHeightContext(this, *Owner.World, Resources);
	if (!Context.IsValid())
	{
		return false;
	}
	const TSharedRef<FOwnedLandscapeHeightEnvironment> Environment =
		MakeShared<FOwnedLandscapeHeightEnvironment>(Owner);
	const FTransform TransformBefore =
		Owner.Landscape->GetActorTransform();
	TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
	FUeAgentWorldReadError QueryError;
	const bool bCaptured = CaptureLandscapeHeight(
		Environment,
		*Context,
		ValidInput,
		Snapshot,
		QueryError);
	TestTrue(
		TEXT("public Query hook reads through the shared native owner"),
		bCaptured);
	if (!bCaptured)
	{
		AddError(
			TEXT("landscape_height_query_failed:")
				+ QueryError.Code + TEXT(":")
				+ QueryError.SubjectId + TEXT(":")
				+ QueryError.Detail);
	}
	if (Snapshot.IsValid() && Snapshot->GetEntries().Num() == 1)
	{
		TSharedPtr<FJsonValue> Json;
		FString JsonError;
		TestTrue(
			TEXT("Landscape Query state converts back to typed JSON"),
			CanonicalJson::TryToJsonValue(
				*Snapshot->GetEntries()[0].GetValue(),
				Json,
				JsonError));
		const TSharedPtr<FJsonObject> Object =
			Json.IsValid() ? Json->AsObject() : nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Heights = nullptr;
		TestTrue(
			TEXT("Landscape Query publishes the exact height array"),
			Object.IsValid()
				&& Object->Values.Num() == 5
				&& Object->HasTypedField<EJson::Object>(TEXT("extent"))
				&& Object->HasTypedField<EJson::Object>(TEXT("scale_cm_per_local_unit"))
				&& Object->GetStringField(TEXT("height_encoding"))
					== TEXT("landscape_local_height")
				&& Object->GetStringField(TEXT("height_order"))
					== TEXT("row_major_x_fastest")
				&& Object->TryGetArrayField(TEXT("heights"), Heights)
				&& Heights
				&& Heights->Num() == DirectState.Heights.Num()
				&& !Object->HasField(TEXT("target_ref"))
				&& !Object->HasField(TEXT("actor_path"))
				&& !Object->HasField(TEXT("managed_id"))
				&& !Object->HasField(TEXT("managed_folder"))
				&& !Object->HasField(TEXT("managed_tag_present"))
				&& !Object->HasField(TEXT("region"))
				&& !Object->HasField(TEXT("sample_count")));
		if (Heights)
		{
			for (int32 Index = 0;
				Index < Heights->Num()
					&& Index < DirectState.Heights.Num();
				++Index)
			{
				TestTrue(
					TEXT("Operation and Query consume identical native samples"),
					FMath::IsNearlyEqual(
						(*Heights)[Index]->AsNumber(),
						DirectState.Heights[Index]));
			}
		}
	}
	else
	{
		AddError(TEXT("Landscape Query snapshot is missing its state entry"));
	}
	TestFalse(
		TEXT("Landscape Query leaves the owned package clean"),
		Owner.Package->IsDirty());
	TestTrue(
		TEXT("Landscape Query leaves the actor transform unchanged"),
		Owner.Landscape->GetActorTransform().Equals(TransformBefore));

	Snapshot.Reset();
	QueryError = {};
	FString InvalidFrame(ValidInput);
	InvalidFrame.ReplaceInline(
		TEXT("ue.landscape.local_sample_grid"),
		TEXT("ue.world"));
	TestFalse(
		TEXT("wrong Landscape frame fails before reading"),
		CaptureLandscapeHeight(
			Environment,
			*Context,
			InvalidFrame,
			Snapshot,
			QueryError));
	TestEqual(
		TEXT("wrong frame keeps a stable typed error"),
		QueryError.Code,
		FString(TEXT("landscape_height_space_contract_invalid")));

	Snapshot.Reset();
	QueryError = {};
	const FString OutOfBounds = TEXT(
		"{\"region\":{\"frame_id\":"
		"\"ue.landscape.local_sample_grid\","
		"\"max_inclusive\":[8,7],\"min_inclusive\":[0,0]},"
		"\"space_contract\":{\"basis_evidence\":\"owned extent\","
		"\"frame_id\":\"ue.landscape.local_sample_grid\","
		"\"unit\":\"sample\"}}" );
	TestFalse(
		TEXT("out-of-bounds Landscape region fails closed"),
		CaptureLandscapeHeight(
			Environment,
			*Context,
			OutOfBounds,
			Snapshot,
			QueryError));
	TestEqual(
		TEXT("out-of-bounds region keeps a stable typed error"),
		QueryError.Code,
		FString(TEXT("landscape_height_region_out_of_bounds")));

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* WrongType = Owner.World->SpawnActor<AActor>(
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		SpawnParameters);
	FString OwnershipError;
	TestTrue(
		TEXT("owned wrong-type fixture receives a valid managed identity"),
		WrongType
			&& UeAgentLevelAuthoring::TryBindManagedActorOwnership(
				*WrongType,
				TEXT("uai://landscape/wrong-type"),
				TEXT("landscape"),
				OwnershipError));
	Resolved = nullptr;
	StateError = {};
	TestFalse(
		TEXT("managed non-Landscape identity fails closed"),
		TryResolveLandscapeHeightTarget(
			*Owner.World,
			TEXT("uai://landscape/wrong-type"),
			nullptr,
			Resolved,
			StateError));
	TestEqual(
		TEXT("wrong target type remains typed"),
		StateError.Code,
		FString(TEXT("landscape_height_target_type_invalid")));

	AActor* Duplicate = Owner.World->SpawnActor<AActor>(
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		SpawnParameters);
	if (Duplicate)
	{
		Duplicate->Tags.AddUnique(
			UeAgentLevelAuthoring::GetManagedActorMarkerTag());
		Duplicate->Tags.AddUnique(FName(
			*UeAgentLevelAuthoring::MakeManagedActorTag(
				ManagedTarget)));
		Duplicate->SetFolderPath(FName(
			*UeAgentLevelAuthoring::MakeManagedActorFolderPath(
				TEXT("landscape"))));
	}
	Resolved = nullptr;
	StateError = {};
	TestFalse(
		TEXT("duplicate managed Landscape identity fails closed"),
		TryResolveLandscapeHeightTarget(
			*Owner.World,
			ManagedTarget,
			nullptr,
			Resolved,
			StateError));
	TestEqual(
		TEXT("duplicate managed identity keeps the authority error"),
		StateError.Code,
		FString(TEXT("landscape_height_managed_identity_invalid")));
	Owner.Package->SetDirtyFlag(false);
	return !HasAnyErrors();
}
