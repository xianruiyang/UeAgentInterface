#include "Misc/AutomationTest.h"

// RSP-006 is test-first. The substantive test compiles only after the
// independent read-only Deformer/PCG native family exists.
#include "Animation/AnimSequence.h"
#include "Components/SceneComponent.h"
#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"
#include "Core/BindingGateway/Query/UeAgentQueryBindingError.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialDeformerPcgNative.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialProductionRuntime.h"
#include "GeometryCache.h"
#include "GeometryCacheCodecV1.h"
#include "GeometryCacheMeshData.h"
#include "GeometryCacheTrackStreamable.h"
#include "Grid/PCGGridDescriptor.h"
#include "Grid/PCGPartitionActor.h"
#include "PCGComponent.h"
#include "PCGDataAsset.h"
#include "PCGPoint.h"
#include "Data/PCGPointData.h"
#include "PreviewScene.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/Package.h"

namespace
{
	using namespace UE::UAI::BindingGateway::Query;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::RigSimulationSpatial;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTrackerOwner MakeResources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 4 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 8 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 200000},
				{EUeAgentResourceDimension::Items, 200000},
				{EUeAgentResourceDimension::StringBytes, 8 * 1024 * 1024},
				{EUeAgentResourceDimension::WorkUnits, 300000},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 32 * 1024 * 1024},
			}));
	}

	TArray<TSharedPtr<FJsonValue>> Strings(
		std::initializer_list<const TCHAR*> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const TCHAR* Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> FrameJson(const FString& FrameId)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("frame_id"), FrameId);
		Result->SetStringField(TEXT("basis_frame_id"), TEXT("frame://basis"));
		Result->SetNumberField(TEXT("dimension"), 3);
		Result->SetStringField(TEXT("unit"), TEXT("cm"));
		Result->SetStringField(TEXT("basis_evidence_id"), TEXT("basis://rsp006"));
		return Result;
	}

	TSharedPtr<FJsonObject> PointJson()
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("clock_id"), TEXT("clock://authored"));
		Result->SetStringField(TEXT("tick"), TEXT("15"));
		const TSharedPtr<FJsonObject> Rate = MakeShared<FJsonObject>();
		Rate->SetStringField(TEXT("numerator"), TEXT("30"));
		Rate->SetStringField(TEXT("denominator"), TEXT("1"));
		Result->SetObjectField(TEXT("rate"), Rate);
		Result->SetStringField(TEXT("state"), TEXT("evaluation"));
		return Result;
	}

	TSharedPtr<FJsonObject> RangeJson()
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("clock_id"), TEXT("clock://authored"));
		Result->SetStringField(TEXT("lower_tick"), TEXT("15"));
		Result->SetStringField(TEXT("upper_tick"), TEXT("30"));
		const TSharedPtr<FJsonObject> Rate = MakeShared<FJsonObject>();
		Rate->SetStringField(TEXT("numerator"), TEXT("30"));
		Rate->SetStringField(TEXT("denominator"), TEXT("1"));
		Result->SetObjectField(TEXT("rate"), Rate);
		Result->SetStringField(TEXT("state"), TEXT("evaluation"));
		return Result;
	}

	TSharedPtr<FJsonObject> SelectorJson(
		const TCHAR* Mode = TEXT("range"),
		const int32 Start = 1)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("mode"), Mode);
		Result->SetArrayField(TEXT("refs"), {});
		if (FString(Mode) == TEXT("range"))
		{
			Result->SetNumberField(TEXT("start"), Start);
			Result->SetNumberField(TEXT("count"), 1);
		}
		return Result;
	}

	TSharedPtr<FJsonObject> RevisionJson(
		const FRigSimulationSpatialRevision& Revision)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("source_id"), Revision.SourceId);
		Result->SetStringField(TEXT("generation"), Revision.Generation);
		Result->SetStringField(TEXT("sequence"), LexToString(Revision.Sequence));
		return Result;
	}

	FUeAgentContractValue JsonInput(const TSharedRef<FJsonObject>& Root)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Root, Writer);
		return FUeAgentContractValue(MoveTemp(Text));
	}

	FUeAgentContractValue QueryInput(
		const FString& Capability,
		const FRigSimulationSpatialGenerationIdentity& GenerationIdentity)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		if (Capability == TEXT("deformer.geometry.sample"))
		{
			Root->SetStringField(TEXT("geometry_cache_ref"), TEXT("geometry://rsp006"));
			Root->SetObjectField(TEXT("sample_time"), PointJson());
			Root->SetObjectField(TEXT("output_frame"), FrameJson(TEXT("frame://output")));
			Root->SetObjectField(TEXT("vertex_selector"), SelectorJson());
		}
		else if (Capability == TEXT("deformer.training_inputs.validate"))
		{
			Root->SetArrayField(TEXT("training_input_refs"), Strings({
				TEXT("training://skeletal"), TEXT("training://animation"),
				TEXT("training://geometry")}));
			TArray<TSharedPtr<FJsonValue>> Ranges;
			TArray<TSharedPtr<FJsonValue>> Mappings;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				Ranges.Add(MakeShared<FJsonValueObject>(RangeJson()));
				const TSharedPtr<FJsonObject> Mapping = MakeShared<FJsonObject>();
				Mapping->SetObjectField(TEXT("source_frame"),
					FrameJson(TEXT("frame://geometry-native")));
				Mapping->SetObjectField(TEXT("target_frame"),
					FrameJson(TEXT("frame://output")));
				Mapping->SetArrayField(TEXT("conversion_evidence_ids"),
					Strings({TEXT("conversion-evidence://geometry-output")}));
				Mappings.Add(MakeShared<FJsonValueObject>(Mapping));
			}
			Root->SetArrayField(TEXT("temporal_ranges"), Ranges);
			Root->SetArrayField(TEXT("frame_mappings"), Mappings);
		}
		else if (Capability == TEXT("deformer.evaluation.sample"))
		{
			Root->SetStringField(TEXT("deformer_ref"), TEXT("deformer://rsp006"));
			Root->SetStringField(TEXT("subject_ref"), TEXT("subject://rsp006"));
			Root->SetObjectField(TEXT("sample_time"), PointJson());
			Root->SetObjectField(TEXT("output_frame"), FrameJson(TEXT("frame://output")));
			Root->SetObjectField(TEXT("vertex_selector"), SelectorJson());
		}
		else
		{
			const TSharedPtr<FJsonObject> Identity = MakeShared<FJsonObject>();
			Identity->SetStringField(TEXT("source_ref"), GenerationIdentity.SourceRef);
			Identity->SetStringField(TEXT("generation"), GenerationIdentity.Generation);
			Identity->SetObjectField(TEXT("revision"),
				RevisionJson(GenerationIdentity.Revision));
			Root->SetObjectField(TEXT("generation_identity"), Identity);
			Root->SetObjectField(
				Capability.Contains(TEXT("partition"))
					? TEXT("partition_selector") : TEXT("data_selector"),
				SelectorJson(TEXT("range"),
					Capability.Contains(TEXT("partition")) ? 0 : 1));
			Root->SetObjectField(TEXT("output_frame"), FrameJson(TEXT("frame://output")));
		}
		return JsonInput(Root);
	}

	TSharedPtr<FUeAgentOperationContext> MakeContext(
		FAutomationTestBase& Test,
		const FUeAgentOperationDescriptor& Descriptor,
		const FString& RequestId,
		const FUeAgentResourceTrackerOwner& Resources)
	{
		const TArray<EUeAgentStatePlane>& Planes =
			Descriptor.GetExecutionContextPolicy().GetAllowedStatePlanes();
		if (!Test.TestTrue(TEXT("query descriptor state plane is explicit"),
			!Planes.IsEmpty())) return nullptr;
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("rsp006-project");
		Input.EditorInstanceId = TEXT("rsp006-editor");
		Input.StatePlane = Planes[0];
		Input.WorldType = EUeAgentExecutionWorldType::Preview;
		Input.WorldIdentity = TEXT("world://rsp006-owned");
		Input.AssetEditorIdentity = TEXT("asset-editor://none");
		Input.ViewportIdentity = TEXT("viewport://none");
		Input.ContextRevision = TEXT("rsp006-context-1");
		Input.bResolvedFromCurrent = false;
		FUeAgentExecutionContextRefPtr Execution;
		FUeAgentExecutionContextError ExecutionError;
		if (!Test.TestTrue(TEXT("freeze explicit Deformer/PCG context"),
			FUeAgentExecutionContextRef::TryCreate(Input, Descriptor.GetIdentity(),
				EUeAgentRequestKind::Query, Descriptor.GetExecutionContextPolicy(),
				Execution, ExecutionError))) return nullptr;
		const FUeAgentCallerContext Caller(TEXT("rsp006-native-e2"),
			{TEXT("rig-simulation-spatial:read")}, TEXT("corr-") + RequestId);
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(60.0, [](){ return 100.0; });
		const FUeAgentCancellationSource Cancellation([](){ return 100.0; });
		const FUeAgentQueryRequest Request(Descriptor.GetIdentity(), RequestId,
			FUeAgentContractValue(TEXT("{}")));
		TSharedPtr<FUeAgentOperationContext> Result;
		FUeAgentOperationContextError Error;
		Test.TestTrue(TEXT("create query context"),
			FUeAgentOperationContext::TryCreate(Request, Caller, Execution.ToSharedRef(),
				Resources, Deadline, Cancellation.GetToken(), Result, Error));
		return Result;
	}

	const FUeAgentCanonicalValue* Field(
		const FUeAgentCanonicalValue& Object,
		const FString& Name)
	{
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
			: Object.GetObjectItems())
		{
			if (Item.Key == Name) return &Item.Value.Get();
		}
		return nullptr;
	}

	const FUeAgentCanonicalValue* ResultRoot(
		const FUeAgentProjectionSourceValue& Source)
	{
		const auto& Snapshot = Source.GetSnapshot();
		return Snapshot.IsValid() && !Snapshot->GetEntries().IsEmpty()
			? &Snapshot->GetEntries()[0].GetValue().Get() : nullptr;
	}

	bool Query(
		FAutomationTestBase& Test,
		const FUeAgentRigSimulationSpatialProductionRuntime& Runtime,
		const FString& Capability,
		const FUeAgentContractValue& Input,
		const FUeAgentResourceTrackerOwner& Resources,
		TSharedPtr<const FUeAgentProjectionSourceValue>& OutSource,
		FString* OutFailureCode = nullptr)
	{
		const FUeAgentOperationDescriptor* Operation =
			Runtime.GetDescriptorRegistry()->GetDescriptors().FindByPredicate(
				[&](const auto& Item){ return Item.GetCapabilityId() == Capability; });
		if (!Test.TestNotNull(TEXT("query operation descriptor: ") + Capability,
			Operation)) return false;
		const TOptional<FUeAgentQueryDescriptor> Descriptor =
			Runtime.GetDescriptorRegistry()->FindQueryExact(Operation->GetIdentity());
		if (!Test.TestTrue(TEXT("exact query descriptor: ") + Capability,
			Descriptor.IsSet())) return false;
		const TSharedPtr<IUeAgentDomainQueryProvider> Provider =
			Runtime.GetQueryProviderRegistry()->FindExact(
				Descriptor->GetProviderBinding());
		if (!Test.TestTrue(TEXT("exact production query provider: ") + Capability,
			Provider.IsValid())) return false;
		const TSharedPtr<FUeAgentOperationContext> Context = MakeContext(
			Test, *Operation, TEXT("query-") + Capability, Resources);
		if (!Context.IsValid()) return false;
		FUeAgentQueryBindingError Error;
		if (!Provider->TryQuery(*Descriptor, Input, {}, *Context, OutSource, Error))
		{
			if (OutFailureCode) *OutFailureCode = Error.Code;
			else Test.AddError(Capability + TEXT(":") + Error.Code
				+ TEXT(":") + Error.Detail);
			return false;
		}
		return Test.TestTrue(TEXT("query returns independent snapshot: ") + Capability,
			OutSource.IsValid() && OutSource->GetSnapshot().IsValid());
	}

	UGeometryCache* MakeGeometryCache(
		const TCHAR* PackageName,
		const FVector3f& First,
		const FVector3f& Second)
	{
		UPackage* Package = CreatePackage(PackageName);
		UGeometryCache* Cache = NewObject<UGeometryCache>(Package,
			TEXT("Cache"), RF_Public | RF_Standalone | RF_Transactional);
		UGeometryCacheTrackStreamable* Track =
			NewObject<UGeometryCacheTrackStreamable>(Cache);
		UGeometryCacheCodecV1* Codec = NewObject<UGeometryCacheCodecV1>(Track);
		Codec->InitializeEncoder(0.01f, 10);
		Track->BeginCoding(Codec, false, false, false);
		FGeometryCacheMeshData Sample;
		Sample.Positions = {First, Second, FVector3f(0.0f, 25.0f, 0.0f)};
		Sample.Indices = {0, 1, 2};
		Sample.ImportedVertexNumbers = {0, 1, 2};
		Sample.VertexInfo.bHasImportedVertexNumbers = true;
		Sample.BoundingBox = FBox3f(Sample.Positions);
		Track->AddMeshSample(Sample, 0.5f, true);
		Sample.Positions[1] += FVector3f(5.0f, 0.0f, 0.0f);
		Sample.BoundingBox = FBox3f(Sample.Positions);
		Track->AddMeshSample(Sample, 1.0f, true);
		Track->EndCoding();
		Track->SetDuration(1.0f);
		Cache->AddTrack(Track);
		Cache->SetFrameStartEnd(15, 30);
		return Cache;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentRigSimulationSpatialDeformerPcgNativeTest,
	"UeAgentInterface.DomainAdapters.RigSimulationSpatial.E2.ProductionNative.DeformerPcgQueries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentRigSimulationSpatialDeformerPcgNativeTest::RunTest(
	const FString& Parameters)
{
	UGeometryCache* GeometryCache = MakeGeometryCache(
		TEXT("/Game/__UAI_RSP006/Geometry"), FVector3f(1.0f, 2.0f, 3.0f),
		FVector3f(20.0f, 4.0f, 5.0f));
	UGeometryCache* EvaluationCache = MakeGeometryCache(
		TEXT("/Game/__UAI_RSP006/Evaluation"), FVector3f(11.0f, 12.0f, 13.0f),
		FVector3f(120.0f, 14.0f, 15.0f));
	USkeletalMesh* SkeletalMesh = LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	UPackage* AnimationPackage = CreatePackage(TEXT("/Game/__UAI_RSP006/Animation"));
	UAnimSequence* Animation = NewObject<UAnimSequence>(AnimationPackage,
		TEXT("Animation"), RF_Public | RF_Standalone | RF_Transactional);
	if (SkeletalMesh && Animation) Animation->SetSkeleton(SkeletalMesh->GetSkeleton());
	USceneComponent* Subject = NewObject<USceneComponent>(GetTransientPackage(),
		TEXT("RSP006EvaluationSubject"), RF_Transactional);

	TUniquePtr<FPreviewScene> PreviewScene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(false).SetForceMipsResident(false)
			.SetTransactional(false));
	UWorld* World = PreviewScene->GetWorld();
	AActor* SourceActor = World ? World->SpawnActor<AActor>() : nullptr;
	UPCGComponent* PcgComponent = SourceActor
		? NewObject<UPCGComponent>(SourceActor, TEXT("RSP006PCG"), RF_Transactional)
		: nullptr;
	if (SourceActor && PcgComponent)
	{
		SourceActor->AddInstanceComponent(PcgComponent);
		PcgComponent->RegisterComponent();
	}
	UPCGDataAsset* DataAsset = NewObject<UPCGDataAsset>(GetTransientPackage(),
		TEXT("RSP006GenerationData"), RF_Transactional);
	UPCGPointData* PointData = NewObject<UPCGPointData>(DataAsset,
		TEXT("RSP006Points"), RF_Transactional);
	if (PointData && DataAsset)
	{
		FPCGPoint A(FTransform(FVector(10.0, 20.0, 30.0)), 0.5f, 7);
		A.BoundsMin = FVector(-1.0, -2.0, -3.0);
		A.BoundsMax = FVector(1.0, 2.0, 3.0);
		FPCGPoint B(FTransform(FVector(100.0, 200.0, 300.0)), 0.75f, 11);
		B.BoundsMin = FVector(-4.0, -5.0, -6.0);
		B.BoundsMax = FVector(4.0, 5.0, 6.0);
		PointData->SetPoints({A, B});
		FPCGTaggedData Tagged;
		Tagged.Data = PointData;
		Tagged.Pin = TEXT("Out");
		Tagged.Tags = {TEXT("rsp006")};
		DataAsset->Data.TaggedData.Add(MoveTemp(Tagged));
	}
	APCGPartitionActor* Partition = nullptr;
	if (World)
	{
		Partition = World->SpawnActor<APCGPartitionActor>(
			FVector(400.0, 500.0, 600.0), FRotator::ZeroRotator);
		if (Partition)
		{
			Partition->SetToRuntimeGenerated();
			Partition->PostCreation(FPCGGridDescriptor().SetGridSize(200)
				.SetIs2DGrid(false).SetIsRuntime(true));
		}
	}
	if (!TestNotNull(TEXT("real Geometry Cache fixture"), GeometryCache)
		|| !TestNotNull(TEXT("real evaluation Geometry Cache fixture"), EvaluationCache)
		|| !TestNotNull(TEXT("real skeletal training input"), SkeletalMesh)
		|| !TestNotNull(TEXT("real animation training input"), Animation)
		|| !TestNotNull(TEXT("real PCG component"), PcgComponent)
		|| !TestNotNull(TEXT("real PCG generated data asset"), DataAsset)
		|| !TestNotNull(TEXT("real PCG point data"), PointData)
		|| !TestNotNull(TEXT("real PCG partition actor"), Partition)) return false;

	const FUeAgentTemporalPoint NativeTime(TEXT("clock://observation-native"),
		TEXT("30"), TEXT("60"), TEXT("1"), TEXT("evaluation"));
	const TArray<TArray<FString>> Basis = {
		{TEXT("1"), TEXT("0"), TEXT("0")},
		{TEXT("0"), TEXT("1"), TEXT("0")},
		{TEXT("0"), TEXT("0"), TEXT("1")}};
	auto MakeFrame = [&](const FString& Id)
	{
		const TArray<FString> Origin = Id == TEXT("frame://output")
			? TArray<FString>{TEXT("-10"), TEXT("0"), TEXT("0")}
			: TArray<FString>{TEXT("0"), TEXT("0"), TEXT("0")};
		return FUeAgentSpatialFrame(Id, TEXT("frame://basis"), 3, Basis,
			Origin, TEXT("cm"),
			TEXT("basis://rsp006"), NativeTime);
	};

	FRigSimulationSpatialGenerationIdentity GenerationIdentity;
	GenerationIdentity.SourceRef = TEXT("pcg://rsp006");
	GenerationIdentity.Generation = TEXT("pcg-generation://rsp006-1");
	GenerationIdentity.Revision.SourceId = GenerationIdentity.SourceRef;
	GenerationIdentity.Revision.Generation = TEXT("rsp006-generation");
	uint32 RevisionHash = GetTypeHash(PcgComponent->GetPathName());
	RevisionHash = HashCombine(RevisionHash,
		GetTypeHash(PcgComponent->GetClass()->GetPathName()));
	RevisionHash = HashCombine(RevisionHash,
		PcgComponent->GetOutermost()->IsDirty() ? 1u : 0u);
	GenerationIdentity.Revision.Sequence = static_cast<int64>(RevisionHash);

	FUeAgentRigSimulationSpatialProductionRuntimeConfig Config;
	Config.ProjectInstanceId = TEXT("rsp006-project");
	Config.EditorInstanceId = TEXT("rsp006-editor");
	Config.ServerSessionId = TEXT("rsp006-session");
	Config.RuntimeGeneration = TEXT("rsp006-generation");
	Config.RegistryEpoch = 1;
	Config.ObjectBindings = {
		{TEXT("geometry://rsp006"), GeometryCache},
		{TEXT("deformer://rsp006"), EvaluationCache},
		{TEXT("subject://rsp006"), Subject},
		{TEXT("training://skeletal"), SkeletalMesh},
		{TEXT("training://animation"), Animation},
		{TEXT("training://geometry"), GeometryCache},
		{TEXT("pcg://rsp006"), PcgComponent}};
	Config.ClockBindings.Add({TEXT("clock://authored"),
		TEXT("clock://observation-native"), TEXT("clock-evidence://rsp006")});
	for (const TPair<FString, bool>& Frame : {
		TPair<FString, bool>(TEXT("frame://geometry-native"), true),
		TPair<FString, bool>(TEXT("frame://evaluation-native"), true),
		TPair<FString, bool>(TEXT("frame://pcg-native"), true),
		TPair<FString, bool>(TEXT("frame://output"), false)})
	{
		Config.FrameBindings.Add(FUeAgentRigSimulationSpatialFrameBinding(
			MakeFrame(Frame.Key), Frame.Value));
	}
	for (const FString& NativeFrame : {TEXT("frame://geometry-native"),
		TEXT("frame://evaluation-native"), TEXT("frame://pcg-native")})
	{
		Config.SpatialConversions.Add(FUeAgentSpatialConversion(
			TEXT("conversion://") + NativeFrame + TEXT("-output"), NativeFrame,
			TEXT("frame://output"), 3, 3, Basis,
			{TEXT("10"), TEXT("0"), TEXT("0")}, NativeTime,
			TEXT("conversion-evidence://") + NativeFrame + TEXT("-output")));
		Config.SpatialConversions.Add(FUeAgentSpatialConversion(
			TEXT("conversion://output-") + NativeFrame, TEXT("frame://output"),
			NativeFrame, 3, 3, Basis, {TEXT("-10"), TEXT("0"), TEXT("0")},
			NativeTime, TEXT("conversion-evidence://output-") + NativeFrame));
	}
	Config.TemporalConversions.Add(FUeAgentTemporalConversion(
		TEXT("conversion://authored-observation"), TEXT("clock://authored"),
		TEXT("clock://observation-native"), FUeAgentTemporalRate(TEXT("30"), TEXT("1")),
		FUeAgentTemporalRate(TEXT("60"), TEXT("1")), TEXT("1"), TEXT("1"),
		TEXT("0"), TEXT("1"), TEXT("conversion-evidence://rsp006-clock")));

	FUeAgentRigSimulationSpatialDeformerGeometryBinding GeometryBinding;
	GeometryBinding.Identity = TEXT("geometry://rsp006");
	GeometryBinding.GeometryCache = GeometryCache;
	GeometryBinding.NativeClockId = TEXT("clock://observation-native");
	GeometryBinding.NativeFrame = MakeFrame(TEXT("frame://geometry-native"));
	Config.DeformerGeometryBindings.Add(GeometryBinding);
	for (const TPair<FString, UObject*>& Input : {
		TPair<FString, UObject*>(TEXT("training://skeletal"), SkeletalMesh),
		TPair<FString, UObject*>(TEXT("training://animation"), Animation),
		TPair<FString, UObject*>(TEXT("training://geometry"), GeometryCache)})
	{
		FUeAgentRigSimulationSpatialDeformerTrainingInputBinding Binding;
		Binding.Identity = Input.Key;
		Binding.Input = Input.Value;
		Binding.NativeClockId = TEXT("clock://observation-native");
		Binding.NativeFrame = MakeFrame(TEXT("frame://geometry-native"));
		Config.DeformerTrainingInputBindings.Add(Binding);
	}
	FUeAgentRigSimulationSpatialDeformerEvaluationBinding EvaluationBinding;
	EvaluationBinding.Identity = TEXT("deformer://rsp006");
	EvaluationBinding.SubjectIdentity = TEXT("subject://rsp006");
	EvaluationBinding.EvaluationGeometry = EvaluationCache;
	EvaluationBinding.Subject = Subject;
	EvaluationBinding.NativeClockId = TEXT("clock://observation-native");
	EvaluationBinding.NativeFrame = MakeFrame(TEXT("frame://evaluation-native"));
	Config.DeformerEvaluationBindings.Add(EvaluationBinding);
	FUeAgentRigSimulationSpatialPcgGenerationBinding PcgBinding;
	PcgBinding.Identity = GenerationIdentity;
	PcgBinding.SourceComponent = PcgComponent;
	PcgBinding.GeneratedData = DataAsset;
	PcgBinding.Partitions = {Partition};
	PcgBinding.GeneratedAt = NativeTime;
	PcgBinding.NativeFrame = MakeFrame(TEXT("frame://pcg-native"));
	Config.PcgGenerationBindings.Add(PcgBinding);

	FUeAgentRigSimulationSpatialProductionRuntimeConfig MissingDeformerClock = Config;
	MissingDeformerClock.DeformerGeometryBindings[0].NativeClockId =
		TEXT("clock://missing-deformer-native");
	TSharedPtr<FUeAgentRigSimulationSpatialProductionRuntime> InvalidRuntime;
	FUeAgentRigSimulationSpatialExecutionError InvalidError;
	TestFalse(TEXT("runtime rejects unregistered Deformer native clock"),
		FUeAgentRigSimulationSpatialProductionRuntime::TryCreate(
			MissingDeformerClock, InvalidRuntime, InvalidError));
	TestEqual(TEXT("unregistered Deformer clock has stable config failure"),
		InvalidError.Code,
		FString(TEXT("rig_simulation_spatial_runtime_config_invalid")));

	FUeAgentRigSimulationSpatialProductionRuntimeConfig MissingPcgClock = Config;
	MissingPcgClock.PcgGenerationBindings[0].GeneratedAt = FUeAgentTemporalPoint(
		TEXT("clock://missing-pcg-native"), TEXT("30"), TEXT("60"), TEXT("1"),
		TEXT("evaluation"));
	InvalidRuntime.Reset();
	InvalidError = {};
	TestFalse(TEXT("runtime rejects unregistered PCG generation clock"),
		FUeAgentRigSimulationSpatialProductionRuntime::TryCreate(
			MissingPcgClock, InvalidRuntime, InvalidError));
	TestEqual(TEXT("unregistered PCG clock has stable config failure"),
		InvalidError.Code,
		FString(TEXT("rig_simulation_spatial_runtime_config_invalid")));

	TSharedPtr<FUeAgentRigSimulationSpatialProductionRuntime> Runtime;
	FUeAgentRigSimulationSpatialExecutionError Error;
	if (!TestTrue(TEXT("compose Deformer/PCG Query production runtime"),
		FUeAgentRigSimulationSpatialProductionRuntime::TryCreate(Config, Runtime, Error))
		|| !Runtime.IsValid())
	{
		AddError(Error.Code + TEXT(":") + Error.Detail);
		return false;
	}

	const bool bGeometryDirty = GeometryCache->GetOutermost()->IsDirty();
	const bool bEvaluationDirty = EvaluationCache->GetOutermost()->IsDirty();
	const bool bAnimationDirty = Animation->GetOutermost()->IsDirty();
	const bool bPcgPackageDirty = PcgComponent->GetOutermost()->IsDirty();
	const FTransform SubjectTransform = Subject->GetComponentTransform();
	const int32 DataCount = DataAsset->Data.TaggedData.Num();
	const FVector PartitionLocation = Partition->GetActorLocation();
	const FUeAgentResourceTrackerOwner Resources = MakeResources();

	for (const FString& Capability : {
		TEXT("deformer.geometry.sample"),
		TEXT("deformer.training_inputs.validate"),
		TEXT("deformer.evaluation.sample"),
		TEXT("pcg.generated_spatial_data.read"),
		TEXT("pcg.partition.spatial_state.read")})
	{
		TSharedPtr<const FUeAgentProjectionSourceValue> Source;
		if (!Query(*this, *Runtime, Capability,
			QueryInput(Capability, GenerationIdentity), Resources, Source)) return false;
		const FUeAgentCanonicalValue* Root = ResultRoot(*Source);
		const FUeAgentCanonicalValue* CapabilityField = Root
			? Field(*Root, TEXT("capability_id")) : nullptr;
		TestTrue(TEXT("structured result root: ") + Capability, Root != nullptr);
		TestTrue(TEXT("result retains exact capability: ") + Capability,
			CapabilityField && CapabilityField->GetLexeme() == Capability);
		const FUeAgentCanonicalValue* Evidence = Root
			? Field(*Root, TEXT("evidence")) : nullptr;
		TestTrue(TEXT("result has non-empty real evidence: ") + Capability,
			Evidence && !Evidence->GetArrayItems().IsEmpty());
		if (Capability.Contains(TEXT("sample")))
		{
			const FUeAgentCanonicalValue* Sample = Root
				? Field(*Root, TEXT("sample")) : nullptr;
			TestNotNull(TEXT("sample result is structured"), Sample);
			const FUeAgentCanonicalValue* Time = Sample
				? Field(*Sample, TEXT("time")) : nullptr;
			const FUeAgentCanonicalValue* Frame = Sample
				? Field(*Sample, TEXT("frame")) : nullptr;
			TestTrue(TEXT("sample retains exact converted native clock"),
				Time && Field(*Time, TEXT("clock_id"))
					&& Field(*Time, TEXT("clock_id"))->GetLexeme()
						== TEXT("clock://observation-native"));
			TestTrue(TEXT("sample retains exact converted native tick"),
				Time && Field(*Time, TEXT("tick"))
					&& Field(*Time, TEXT("tick"))->GetLexeme() == TEXT("30"));
			TestTrue(TEXT("sample retains exact requested output frame"),
				Frame && Field(*Frame, TEXT("frame_id"))
					&& Field(*Frame, TEXT("frame_id"))->GetLexeme()
						== TEXT("frame://output"));
			const FUeAgentCanonicalValue* Vertices = Sample
				? Field(*Sample, TEXT("vertices")) : nullptr;
			TestTrue(TEXT("sample returns selected real vertex, not bounds alias"),
				Vertices && Vertices->GetArrayItems().Num() == 1);
			if (Vertices && Vertices->GetArrayItems().Num() == 1)
			{
				const FUeAgentCanonicalValue& Vertex =
					Vertices->GetArrayItems()[0].Get();
				const FUeAgentCanonicalValue* Components =
					Field(Vertex, TEXT("components"));
				const double ExpectedX = Capability == TEXT("deformer.geometry.sample")
					? 30.0 : 130.0;
				double ActualX = 0.0;
				const FString ActualXLexeme = Components
					&& !Components->GetArrayItems().IsEmpty()
					? Components->GetArrayItems()[0]->GetLexeme() : TEXT("<missing>");
				const bool bHasExactComponent = Components
					&& Components->GetArrayItems().Num() == 3
					&& LexTryParseString(ActualX, *ActualXLexeme);
				TestTrue(FString::Printf(TEXT("sample contains converted real vertex within codec precision (actual=%s expected=%.2f)"),
					*ActualXLexeme, ExpectedX),
					bHasExactComponent && FMath::IsNearlyEqual(ActualX, ExpectedX, 0.02));
			}
			if (Capability == TEXT("deformer.evaluation.sample"))
			{
				TestTrue(TEXT("evaluation retains exact subject identity"),
					Field(*Root, TEXT("subject_ref"))
						&& Field(*Root, TEXT("subject_ref"))->GetLexeme()
							== TEXT("subject://rsp006"));
			}
		}
		else if (Capability == TEXT("deformer.training_inputs.validate"))
		{
			const FUeAgentCanonicalValue* Inputs = Field(*Root, TEXT("inputs"));
			TestTrue(TEXT("training validation reports every explicit input"),
				Inputs && Inputs->GetArrayItems().Num() == 3);
			if (Inputs)
			{
				for (const FUeAgentCanonicalValueRef& Item : Inputs->GetArrayItems())
				{
					TestNotNull(TEXT("training item retains identity"),
						Field(Item.Get(), TEXT("input_ref")));
					TestNotNull(TEXT("training item retains incompatibility reasons"),
						Field(Item.Get(), TEXT("reasons")));
					TestNotNull(TEXT("training item retains exact temporal range"),
						Field(Item.Get(), TEXT("temporal_range")));
					TestNotNull(TEXT("training item retains C4 frame mapping"),
						Field(Item.Get(), TEXT("frame_mapping")));
				}
			}
		}
		else
		{
			const FUeAgentCanonicalValue* Identity =
				Field(*Root, TEXT("generation_identity"));
			TestTrue(TEXT("PCG result retains exact generation source"),
				Identity && Field(*Identity, TEXT("source_ref"))
					&& Field(*Identity, TEXT("source_ref"))->GetLexeme()
						== GenerationIdentity.SourceRef);
			TestTrue(TEXT("PCG result retains exact generation id"),
				Identity && Field(*Identity, TEXT("generation"))
					&& Field(*Identity, TEXT("generation"))->GetLexeme()
						== GenerationIdentity.Generation);
			const FUeAgentCanonicalValue* Time = Field(*Root, TEXT("time"));
			TestTrue(TEXT("PCG result retains exact generation time"),
				Time && Field(*Time, TEXT("clock_id"))
					&& Field(*Time, TEXT("clock_id"))->GetLexeme()
						== TEXT("clock://observation-native")
					&& Field(*Time, TEXT("tick"))
					&& Field(*Time, TEXT("tick"))->GetLexeme() == TEXT("30"));
			const FUeAgentCanonicalValue* Frame = Field(*Root, TEXT("frame"));
			TestTrue(TEXT("PCG result retains exact requested output frame"),
				Frame && Field(*Frame, TEXT("frame_id"))
					&& Field(*Frame, TEXT("frame_id"))->GetLexeme()
						== TEXT("frame://output"));
			const FUeAgentCanonicalValue* Items = Field(*Root, TEXT("items"));
			TestTrue(TEXT("PCG selector returns one real item"),
				Items && Items->GetArrayItems().Num() == 1);
			if (Items && Items->GetArrayItems().Num() == 1)
			{
				const FUeAgentCanonicalValue& Item = Items->GetArrayItems()[0].Get();
				const TCHAR* IdentityField = Capability.Contains(TEXT("partition"))
					? TEXT("partition_ref") : TEXT("data_ref");
				const FUeAgentCanonicalValue* ItemIdentity = Field(Item, IdentityField);
				TestTrue(TEXT("PCG selector retains real item identity"),
					ItemIdentity && (Capability.Contains(TEXT("partition"))
						? ItemIdentity->GetLexeme().StartsWith(TEXT("partition:0:"))
						: ItemIdentity->GetLexeme() == TEXT("data:0/point:1")));
			}
		}
	}

	FRigSimulationSpatialGenerationIdentity WrongGeneration = GenerationIdentity;
	WrongGeneration.Generation = TEXT("pcg-generation://wrong");
	TSharedPtr<const FUeAgentProjectionSourceValue> FailedSource;
	FString FailureCode;
	TestFalse(TEXT("generation mismatch fails closed"), Query(*this, *Runtime,
		TEXT("pcg.generated_spatial_data.read"),
		QueryInput(TEXT("pcg.generated_spatial_data.read"), WrongGeneration),
		Resources, FailedSource, &FailureCode));
	TestEqual(TEXT("generation mismatch has exact failure"), FailureCode,
		FString(TEXT("rig_simulation_spatial_pcg_generation_identity_mismatch")));
	TestEqual(TEXT("five successful queries plus one rejected read reach family once each"),
		Runtime->GetFamilyDispatchCallCount(), 6);

	TestEqual(TEXT("Geometry Cache package dirty state is unchanged"),
		GeometryCache->GetOutermost()->IsDirty(), bGeometryDirty);
	TestEqual(TEXT("evaluation package dirty state is unchanged"),
		EvaluationCache->GetOutermost()->IsDirty(), bEvaluationDirty);
	TestEqual(TEXT("animation package dirty state is unchanged"),
		Animation->GetOutermost()->IsDirty(), bAnimationDirty);
	TestTrue(TEXT("evaluation subject transform is unchanged"),
		Subject->GetComponentTransform().Equals(SubjectTransform));
	TestEqual(TEXT("PCG source component package dirty state is unchanged"),
		PcgComponent->GetOutermost()->IsDirty(), bPcgPackageDirty);
	TestEqual(TEXT("PCG generated data collection is unchanged"),
		DataAsset->Data.TaggedData.Num(), DataCount);
	TestTrue(TEXT("PCG partition location is unchanged"),
		Partition->GetActorLocation().Equals(PartitionLocation));
	return !HasAnyErrors();
}
