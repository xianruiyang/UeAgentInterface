#include "Misc/AutomationTest.h"

#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/StateSemantic/UeAgentTemporalContract.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialContract.h"

#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialPort.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT(
				"UeAgentInterface/Source/UeAgentInterfaceTests/Private/"
				"DomainAdapters/RigSimulationSpatial/Fixtures/"
				"rig_simulation_spatial_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load RigSimulationSpatial contract fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse RigSimulationSpatial contract fixture"),
				FJsonSerializer::Deserialize(Reader, Fixture)
					&& Fixture.IsValid()))
		{
			return nullptr;
		}
		return Fixture;
	}

	TArray<FString> ReadStrings(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field)
	{
		TArray<FString> Result;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid()
			|| !Object->TryGetArrayField(Field, Values)
			|| Values == nullptr)
		{
			return Result;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			Result.Add(Value->AsString());
		}
		return Result;
	}

	TArray<int32> ReadInts(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field)
	{
		TArray<int32> Result;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid()
			|| !Object->TryGetArrayField(Field, Values)
			|| Values == nullptr)
		{
			return Result;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			Result.Add(static_cast<int32>(Value->AsNumber()));
		}
		return Result;
	}

	bool HasExactStrings(TArray<FString> Left, TArray<FString> Right)
	{
		Left.Sort();
		Right.Sort();
		return Left == Right;
	}

	FUeAgentResourceTracker MakeTracker()
	{
		return FUeAgentResourceTracker();
	}

	FUeAgentSpatialFrame MakeFrame(
		const FString& FrameId,
		const TArray<FString>& Origin,
		const FString& Evidence,
		const FUeAgentTemporalPoint& ObservedAt)
	{
		return FUeAgentSpatialFrame(
			FrameId,
			TEXT("fixture_world_basis"),
			3,
			{{TEXT("1"), TEXT("0"), TEXT("0")},
			 {TEXT("0"), TEXT("1"), TEXT("0")},
			 {TEXT("0"), TEXT("0"), TEXT("1")}},
			Origin,
			TEXT("centimeter"),
			Evidence,
			ObservedAt);
	}

	FUeAgentSpatialConversion MakeTranslationConversion(
		const FString& ConversionId,
		const FString& FromFrame,
		const FString& ToFrame,
		const TArray<FString>& Translation,
		const FString& Evidence,
		const FUeAgentTemporalPoint& ObservedAt)
	{
		return FUeAgentSpatialConversion(
			ConversionId,
			FromFrame,
			ToFrame,
			3,
			3,
			{{TEXT("1"), TEXT("0"), TEXT("0")},
			 {TEXT("0"), TEXT("1"), TEXT("0")},
			 {TEXT("0"), TEXT("0"), TEXT("1")}},
			Translation,
			ObservedAt,
			Evidence);
	}

	class FRecordingRigSimulationSpatialNativePort final
		: public UE::UAI::DomainAdapters::RigSimulationSpatial::
			IUeAgentRigSimulationSpatialNativePort
	{
	public:
		bool TryExecute(
			const UE::UAI::DomainAdapters::RigSimulationSpatial::
				FUeAgentRigSimulationSpatialPortRequest& Request,
			FUeAgentResourceTracker& Resources,
			UE::UAI::DomainAdapters::RigSimulationSpatial::
				FUeAgentRigSimulationSpatialPortObservation& OutObservation,
			UE::UAI::DomainAdapters::RigSimulationSpatial::
				FUeAgentRigSimulationSpatialPortError& OutError) override
		{
			++ExecuteCalls;
			OutObservation = NextObservation;
			return true;
		}

		int32 ExecuteCalls = 0;
		UE::UAI::DomainAdapters::RigSimulationSpatial::
			FUeAgentRigSimulationSpatialPortObservation NextObservation;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentRigSimulationSpatialContractTest,
	"UeAgentInterface.DomainAdapters.RigSimulationSpatial.E0.Contract."
		"CurrentSpatialLifecycleAndProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentRigSimulationSpatialContractTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}

	TestEqual(
		TEXT("fixture schema"),
		Fixture->GetStringField(TEXT("schema")),
		FString(TEXT(
			"uai.domain_adapter.rig_simulation_spatial.test_fixture.v1")));
	TestEqual(
		TEXT("shared temporal contract"),
		Fixture->GetStringField(TEXT("temporal_contract_id")),
		FString(TEXT("uai.temporal_contract.v1")));
	TestEqual(
		TEXT("shared spatial contract"),
		Fixture->GetStringField(TEXT("space_contract_id")),
		FString(TEXT("uai.space_contract.v1")));

	const TArray<TSharedPtr<FJsonValue>>& FamilyValues =
		Fixture->GetArrayField(TEXT("port_families"));
	const TArray<TSharedPtr<FJsonValue>>& CapabilityValues =
		Fixture->GetArrayField(TEXT("capabilities"));
	const TArray<TSharedPtr<FJsonValue>>& ScenarioValues =
		Fixture->GetArrayField(TEXT("scenario_contracts"));
	const TSet<FString> ExpectedFamilies = {
		TEXT("skeletal_attachment"),
		TEXT("rig_bake"),
		TEXT("ik_retarget"),
		TEXT("simulation_definition"),
		TEXT("simulation_observation"),
	};
	TSet<FString> FamilyIds;
	TMap<FString, TSharedPtr<FJsonObject>> FamilyById;
	for (const TSharedPtr<FJsonValue>& Value : FamilyValues)
	{
		const TSharedPtr<FJsonObject> Family = Value->AsObject();
		const FString FamilyId =
			Family->GetStringField(TEXT("family_id"));
		TestFalse(
			TEXT("family id is unique: ") + FamilyId,
			FamilyIds.Contains(FamilyId));
		FamilyIds.Add(FamilyId);
		FamilyById.Add(FamilyId, Family);
		TestTrue(
			TEXT("family declares exact 3D dimension: ") + FamilyId,
			ReadInts(Family, TEXT("dimensions")) == TArray<int32>({3}));
		TestTrue(
			TEXT("family declares accepted time states: ") + FamilyId,
			!ReadStrings(Family, TEXT("accepted_time_states")).IsEmpty());
		TestTrue(
			TEXT("family freezes subject: ") + FamilyId,
			!Family->GetStringField(TEXT("subject_policy")).IsEmpty());
		TestTrue(
			TEXT("family freezes Fsemantic: ") + FamilyId,
			!Family->GetStringField(
				TEXT("semantic_frame_policy")).IsEmpty());
		TestTrue(
			TEXT("family freezes Fwrite: ") + FamilyId,
			!Family->GetStringField(
				TEXT("write_frame_policy")).IsEmpty());
		TestTrue(
			TEXT("family freezes Fread: ") + FamilyId,
			!Family->GetStringField(
				TEXT("read_frame_policy")).IsEmpty());
		TestTrue(
			TEXT("family freezes Fverify: ") + FamilyId,
			!Family->GetStringField(
				TEXT("verify_frame_policy")).IsEmpty());
		TestTrue(
			TEXT("family freezes conversion chain: ") + FamilyId,
			!Family->GetStringField(TEXT("conversion_chain")).IsEmpty());
		TestTrue(
			TEXT("family requires frozen context: ") + FamilyId,
			!ReadStrings(Family, TEXT("required_context_facets")).IsEmpty());
		TestTrue(
			TEXT("family rejects implicit context: ") + FamilyId,
			!ReadStrings(Family, TEXT("forbidden_implicit_facets")).IsEmpty());
		TestTrue(
			TEXT("family has independent oracle: ") + FamilyId,
			!Family->GetStringField(TEXT("oracle_kind")).IsEmpty());
	}
	for (const FString& ExpectedFamily : ExpectedFamilies)
	{
		TestTrue(TEXT("required typed port family exists: ")
			+ ExpectedFamily,
			FamilyIds.Contains(ExpectedFamily));
	}
	TestTrue(
		TEXT("simulation observation accepts explicit simulation time"),
		ReadStrings(
			FamilyById[TEXT("simulation_observation")],
			TEXT("accepted_time_states"))
			.Contains(TEXT("simulation")));
	TestTrue(
		TEXT("captured time cannot become simulation time"),
		ReadStrings(
			FamilyById[TEXT("simulation_observation")],
			TEXT("forbidden_implicit_facets"))
			.Contains(TEXT("captured_time_as_simulation_time")));

	TSet<FString> CapabilityIds;
	TSet<FString> DescriptorIds;
	TMap<FString, TSharedPtr<FJsonObject>> CapabilityById;
	for (const TSharedPtr<FJsonValue>& Value : CapabilityValues)
	{
		const TSharedPtr<FJsonObject> Capability = Value->AsObject();
		const FString CapabilityId =
			Capability->GetStringField(TEXT("capability_id"));
		const FString DescriptorId =
			Capability->GetStringField(TEXT("descriptor_id"));
		TestFalse(
			TEXT("capability identity is unique: ") + CapabilityId,
			CapabilityIds.Contains(CapabilityId));
		TestFalse(
			TEXT("descriptor identity is unique: ") + DescriptorId,
			DescriptorIds.Contains(DescriptorId));
		TestFalse(
			TEXT("runtime identity is not migration placeholder: ")
				+ CapabilityId,
			CapabilityId.StartsWith(TEXT("uai.migration."))
				|| DescriptorId.StartsWith(TEXT("uai.migration.")));
		CapabilityIds.Add(CapabilityId);
		DescriptorIds.Add(DescriptorId);
		CapabilityById.Add(CapabilityId, Capability);
		const FString OperationKind =
			Capability->GetStringField(TEXT("operation_kind"));
		const FString EffectClass =
			Capability->GetStringField(TEXT("effect_class"));
		TestTrue(
			TEXT("operation/effect pairing is valid: ") + CapabilityId,
			(OperationKind == TEXT("query")
				&& EffectClass == TEXT("read_only"))
				|| (OperationKind == TEXT("operation")
					&& (EffectClass == TEXT("write")
						|| EffectClass == TEXT("session_control"))));
		TestTrue(
			TEXT("capability references typed port: ") + CapabilityId,
			FamilyIds.Contains(Capability->GetStringField(
				TEXT("port_family_id"))));
	}
	TestEqual(
		TEXT("physics probe is effectful session control"),
		CapabilityById[TEXT("physics.simulation.probe")]
			->GetStringField(TEXT("effect_class")),
		FString(TEXT("session_control")));

	const TSharedPtr<FJsonObject> Lifecycle =
		Fixture->GetObjectField(TEXT("lifecycle_contract"));
	TestEqual(TEXT("validation runs before mutation"),
		Lifecycle->GetStringField(TEXT("validation_stage")),
		FString(TEXT("pre_mutation")));
	TestEqual(TEXT("validation belongs to the same HJSON import plan"),
		Lifecycle->GetStringField(TEXT("validation_scope")),
		FString(TEXT("same_hjson_import_plan")));
	TestFalse(TEXT("validation is not a standalone capability"),
		Lifecycle->GetBoolField(
			TEXT("standalone_validation_capability")));

	const TSet<FString> ExpectedScenarios = {
		TEXT("skeleton_socket_parent_chain"),
		TEXT("control_rig_bake_authored_range"),
		TEXT("ik_source_target_pose"),
		TEXT("physics_constraint_body_pair"),
		TEXT("deformer_geometry_evaluation"),
		TEXT("pcg_generated_point_observation"),
	};
	TSet<FString> ScenarioIds;
	for (const TSharedPtr<FJsonValue>& Value : ScenarioValues)
	{
		const TSharedPtr<FJsonObject> Scenario = Value->AsObject();
		const FString ScenarioId =
			Scenario->GetStringField(TEXT("scenario_id"));
		TestFalse(
			TEXT("scenario id is unique: ") + ScenarioId,
			ScenarioIds.Contains(ScenarioId));
		ScenarioIds.Add(ScenarioId);
		TestEqual(
			TEXT("scenario is exact 3D: ") + ScenarioId,
			static_cast<int32>(Scenario->GetNumberField(TEXT("dimension"))),
			3);
		for (const TCHAR* Field : {
			TEXT("subject"),
			TEXT("semantic_frame"),
			TEXT("write_frame"),
			TEXT("read_frame"),
			TEXT("verify_frame"),
			TEXT("temporal_state"),
			TEXT("conversion_chain")})
		{
			TestTrue(
				FString(TEXT("scenario declares ")) + Field
					+ TEXT(": ") + ScenarioId,
				!Scenario->GetStringField(Field).IsEmpty());
		}
		TestTrue(
			TEXT("scenario has frame/time evidence: ") + ScenarioId,
			!ReadStrings(Scenario, TEXT("required_evidence")).IsEmpty());
	}
	for (const FString& ExpectedScenario : ExpectedScenarios)
	{
		TestTrue(TEXT("required scenario contract exists: ")
			+ ExpectedScenario,
			ScenarioIds.Contains(ExpectedScenario));
	}

	const TArray<FString> ForbiddenOracles =
		ReadStrings(Fixture, TEXT("forbidden_oracles"));
	TestTrue(
		TEXT("screenshot alone is not an oracle"),
		ForbiddenOracles.Contains(TEXT("screenshot_only")));
	TestTrue(
		TEXT("current editor state is not an oracle"),
		ForbiddenOracles.Contains(TEXT("current_editor_state")));

	const FUeAgentTemporalPoint CapturedAt(
		TEXT("fixture_observation_clock"),
		TEXT("100"),
		TEXT("1"),
		TEXT("1"),
		TEXT("captured"));
	const FUeAgentSpatialFrame SocketFrame = MakeFrame(
		TEXT("socket_local:/Game/Test/SK_Test:Grip"),
		{TEXT("12"), TEXT("23"), TEXT("34")},
		TEXT("socket_frame_readback:revision-7"),
		CapturedAt);
	const FUeAgentSpatialFrame ParentBoneFrame = MakeFrame(
		TEXT("bone_local:/Game/Test/SK_Test:hand_r"),
		{TEXT("10"), TEXT("20"), TEXT("30")},
		TEXT("parent_bone_frame_readback:revision-7"),
		CapturedAt);
	const FUeAgentSpatialFrame ComponentFrame = MakeFrame(
		TEXT("skeletal_component:/Game/Test/SK_Test:preview"),
		{TEXT("0"), TEXT("0"), TEXT("0")},
		TEXT("component_frame_readback:revision-7"),
		CapturedAt);
	const FUeAgentSpatialConversion SocketToBone =
		MakeTranslationConversion(
			TEXT("socket_to_parent_bone"),
			SocketFrame.GetFrameId(),
			ParentBoneFrame.GetFrameId(),
			{TEXT("2"), TEXT("3"), TEXT("4")},
			TEXT("socket_parent_readback:revision-7"),
			CapturedAt);
	const FUeAgentSpatialConversion BoneToComponent =
		MakeTranslationConversion(
			TEXT("parent_bone_to_component"),
			ParentBoneFrame.GetFrameId(),
			ComponentFrame.GetFrameId(),
			{TEXT("10"), TEXT("20"), TEXT("30")},
			TEXT("bone_component_readback:revision-7"),
			CapturedAt);
	const FUeAgentSpatialValue SocketOrigin(
		TEXT("socket:/Game/Test/SK_Test:Grip"),
		EUeAgentSpatialQuantityKind::Point,
		SocketFrame,
		{TEXT("0"), TEXT("0"), TEXT("0")},
		CapturedAt);
	FUeAgentResourceTracker SpatialTracker = MakeTracker();
	FUeAgentSpatialValuePtr InBone;
	FUeAgentSpatialValuePtr InComponent;
	FUeAgentStateSemanticError SpatialError;
	TestTrue(
		TEXT("socket converts to exact parent-bone frame"),
		TryConvertSpatialValue(
			SocketOrigin,
			ParentBoneFrame,
			SocketToBone,
			SpatialTracker,
			InBone,
			SpatialError));
	TestTrue(
		TEXT("parent-bone result converts to component frame"),
		InBone.IsValid()
			&& TryConvertSpatialValue(
				*InBone,
				ComponentFrame,
				BoneToComponent,
				SpatialTracker,
				InComponent,
				SpatialError));
	const FUeAgentSpatialValue ExpectedComponentPoint(
		SocketOrigin.GetSubject(),
		EUeAgentSpatialQuantityKind::Point,
		ComponentFrame,
		{TEXT("12"), TEXT("23"), TEXT("34")},
		CapturedAt,
		{SocketToBone.GetEvidence(), BoneToComponent.GetEvidence()});
	TestTrue(
		TEXT("two-stage skeletal frame chain reaches Fverify"),
		InComponent.IsValid()
			&& InComponent->SemanticallyEquals(ExpectedComponentPoint));
	const FUeAgentSpatialValue OtherSubjectSameNumbers(
		TEXT("socket:/Game/Test/SK_Test:Other"),
		EUeAgentSpatialQuantityKind::Point,
		ComponentFrame,
		{TEXT("12"), TEXT("23"), TEXT("34")},
		CapturedAt,
		{SocketToBone.GetEvidence(), BoneToComponent.GetEvidence()});
	TestFalse(
		TEXT("same numeric transform on another subject is not equivalent"),
		ExpectedComponentPoint.SemanticallyEquals(OtherSubjectSameNumbers));

	const FUeAgentTemporalPoint SequencePoint(
		TEXT("sequence:/Game/Test/LS_Rig"),
		TEXT("24"),
		TEXT("24"),
		TEXT("1"),
		TEXT("authored"));
	const FUeAgentTemporalConversion SequenceToBakeSample(
		TEXT("sequence_to_bake_sample"),
		TEXT("sequence:/Game/Test/LS_Rig"),
		TEXT("bake_sample:/Game/Test/A_Baked"),
		FUeAgentTemporalRate(TEXT("24"), TEXT("1")),
		FUeAgentTemporalRate(TEXT("48"), TEXT("1")),
		TEXT("1"),
		TEXT("1"),
		TEXT("0"),
		TEXT("1"),
		TEXT("sequence_tick_resolution_and_target_sample_rate"));
	FUeAgentResourceTracker TemporalTracker = MakeTracker();
	TOptional<FUeAgentTemporalPoint> BakePoint;
	FUeAgentStateSemanticError TemporalError;
	TestTrue(
		TEXT("rig bake uses exact C4 clock conversion"),
		TryConvertTemporalPoint(
			SequencePoint,
			SequenceToBakeSample,
			EUeAgentTemporalRoundingPolicy::Exact,
			TemporalTracker,
			BakePoint,
			TemporalError));
	TestTrue(
		TEXT("one second at 24fps becomes exact tick 48 at 48fps"),
		BakePoint.IsSet()
			&& BakePoint->GetTick() == TEXT("48")
			&& BakePoint->GetState() == TEXT("authored"));

	using namespace UE::UAI::DomainAdapters::RigSimulationSpatial;
	TestEqual(
		TEXT("production RigSimulationSpatial contract version"),
		UAI_RIG_SIMULATION_SPATIAL_CONTRACT_V1,
		1);
	const auto& ProductionFamilies =
		GetRigSimulationSpatialPortFamilyContracts();
	const auto& ProductionCapabilities =
		GetRigSimulationSpatialCapabilityContracts();
	for (const auto& ProductionFamily : ProductionFamilies)
	{
		const FString FamilyId = ProductionFamily.GetFamilyId();
		TestTrue(
			TEXT("production family exists in fixture: ") + FamilyId,
			FamilyById.Contains(FamilyId));
		if (!FamilyById.Contains(FamilyId))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Expected = FamilyById[FamilyId];
		TestEqual(
			TEXT("production temporal contract: ") + FamilyId,
			ProductionFamily.GetTemporalContractId(),
			Fixture->GetStringField(TEXT("temporal_contract_id")));
		TestEqual(
			TEXT("production spatial contract: ") + FamilyId,
			ProductionFamily.GetSpaceContractId(),
			Fixture->GetStringField(TEXT("space_contract_id")));
		TestTrue(
			TEXT("production dimensions: ") + FamilyId,
			ProductionFamily.GetDimensions()
				== ReadInts(Expected, TEXT("dimensions")));
		TestTrue(
			TEXT("production time states: ") + FamilyId,
			HasExactStrings(
				ProductionFamily.GetAcceptedTimeStates(),
				ReadStrings(Expected, TEXT("accepted_time_states"))));
		TestEqual(
			TEXT("production subject policy: ") + FamilyId,
			ProductionFamily.GetSubjectPolicy(),
			Expected->GetStringField(TEXT("subject_policy")));
		TestEqual(
			TEXT("production Fsemantic policy: ") + FamilyId,
			ProductionFamily.GetSemanticFramePolicy(),
			Expected->GetStringField(TEXT("semantic_frame_policy")));
		TestEqual(
			TEXT("production Fwrite policy: ") + FamilyId,
			ProductionFamily.GetWriteFramePolicy(),
			Expected->GetStringField(TEXT("write_frame_policy")));
		TestEqual(
			TEXT("production Fread policy: ") + FamilyId,
			ProductionFamily.GetReadFramePolicy(),
			Expected->GetStringField(TEXT("read_frame_policy")));
		TestEqual(
			TEXT("production Fverify policy: ") + FamilyId,
			ProductionFamily.GetVerifyFramePolicy(),
			Expected->GetStringField(TEXT("verify_frame_policy")));
		TestEqual(
			TEXT("production conversion chain: ") + FamilyId,
			ProductionFamily.GetConversionChain(),
			Expected->GetStringField(TEXT("conversion_chain")));
		TestTrue(
			TEXT("production context facets: ") + FamilyId,
			HasExactStrings(
				ProductionFamily.GetRequiredContextFacets(),
				ReadStrings(Expected, TEXT("required_context_facets"))));
		TestTrue(
			TEXT("production forbidden implicit facets: ") + FamilyId,
			HasExactStrings(
				ProductionFamily.GetForbiddenImplicitFacets(),
				ReadStrings(Expected, TEXT("forbidden_implicit_facets"))));
		TestEqual(
			TEXT("production independent oracle: ") + FamilyId,
			ProductionFamily.GetOracleKind(),
			Expected->GetStringField(TEXT("oracle_kind")));
	}
	for (const auto& ProductionCapability : ProductionCapabilities)
	{
		const FString CapabilityId =
			ProductionCapability.GetCapabilityId();
		TestTrue(
			TEXT("production capability exists in fixture: ")
				+ CapabilityId,
			CapabilityById.Contains(CapabilityId));
		if (!CapabilityById.Contains(CapabilityId))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Expected =
			CapabilityById[CapabilityId];
		TestEqual(
			TEXT("production descriptor id: ") + CapabilityId,
			ProductionCapability.GetDescriptorId(),
			Expected->GetStringField(TEXT("descriptor_id")));
		TestEqual(
			TEXT("production descriptor version: ") + CapabilityId,
			ProductionCapability.GetVersion(),
			static_cast<int32>(Expected->GetNumberField(TEXT("version"))));
		TestEqual(
			TEXT("production operation kind: ") + CapabilityId,
			ProductionCapability.GetOperationKind(),
			Expected->GetStringField(TEXT("operation_kind")));
		TestEqual(
			TEXT("production effect class: ") + CapabilityId,
			ProductionCapability.GetEffectClass(),
			Expected->GetStringField(TEXT("effect_class")));
		TestEqual(
			TEXT("production port family: ") + CapabilityId,
			ProductionCapability.GetPortFamilyId(),
			Expected->GetStringField(TEXT("port_family_id")));
		TestEqual(
			TEXT("production capability temporal contract: ")
				+ CapabilityId,
			ProductionCapability.GetTemporalContractId(),
			Fixture->GetStringField(TEXT("temporal_contract_id")));
		TestEqual(
			TEXT("production capability spatial contract: ")
				+ CapabilityId,
			ProductionCapability.GetSpaceContractId(),
			Fixture->GetStringField(TEXT("space_contract_id")));
	}

	using namespace UE::UAI::DomainAdapters::RigSimulationSpatial;
	TestEqual(
		TEXT("production RigSimulationSpatial typed-port version"),
		UAI_RIG_SIMULATION_SPATIAL_TYPED_PORT_V1,
		1);
	const FUeAgentRigSimulationSpatialPortRequest MissingFrameChain(
		EUeAgentRigSimulationSpatialPortFamily::SkeletalAttachment,
		{SequencePoint},
		{},
		{MakeShared<FUeAgentSpatialValue>(SocketOrigin)},
		{},
		{TEXT("skeleton_ref"), TEXT("source_revision"),
		 TEXT("stable_bone_refs"), TEXT("space_contract")});
	FRecordingRigSimulationSpatialNativePort NativePort;
	FUeAgentRigSimulationSpatialPortObservation Observation;
	FUeAgentRigSimulationSpatialPortError PortError;
	FUeAgentResourceTracker MissingChainTracker = MakeTracker();
	TestFalse(
		TEXT("missing skeletal conversion chain is rejected"),
		TryExecuteRigSimulationSpatialPort(
			MissingFrameChain,
			NativePort,
			MissingChainTracker,
			Observation,
			PortError));
	TestEqual(
		TEXT("missing conversion has stable error"),
		PortError.Code,
		FString(TEXT("rig_simulation_spatial_conversion_required")));
	TestEqual(
		TEXT("invalid request never reaches native port"),
		NativePort.ExecuteCalls,
		0);

	const FUeAgentRigSimulationSpatialPortRequest MissingContext(
		EUeAgentRigSimulationSpatialPortFamily::SkeletalAttachment,
		{SequencePoint},
		{},
		{MakeShared<FUeAgentSpatialValue>(SocketOrigin)},
		{SocketToBone, BoneToComponent},
		{TEXT("skeleton_ref"), TEXT("source_revision"),
		 TEXT("stable_bone_refs")});
	FUeAgentResourceTracker MissingContextTracker = MakeTracker();
	TestFalse(
		TEXT("missing frozen context is rejected"),
		TryExecuteRigSimulationSpatialPort(
			MissingContext,
			NativePort,
			MissingContextTracker,
			Observation,
			PortError));
	TestEqual(
		TEXT("missing context has stable error"),
		PortError.Code,
		FString(TEXT("rig_simulation_spatial_context_facet_missing")));
	TestEqual(
		TEXT("missing context rejection is pre-native"),
		NativePort.ExecuteCalls,
		0);

	const FUeAgentTemporalPoint CapturedAsBakeTime(
		TEXT("sequence:/Game/Test/LS_Rig"),
		TEXT("24"), TEXT("24"), TEXT("1"), TEXT("captured"));
	const FUeAgentRigSimulationSpatialPortRequest CapturedBakeRequest(
		EUeAgentRigSimulationSpatialPortFamily::RigBake,
		{CapturedAsBakeTime},
		{},
		{MakeShared<FUeAgentSpatialValue>(SocketOrigin)},
		{SocketToBone, BoneToComponent},
		{TEXT("sequence_ref"), TEXT("binding_ref"), TEXT("rig_ref"),
		 TEXT("target_ref"), TEXT("source_revision"),
		 TEXT("temporal_domain"), TEXT("space_contract")});
	FUeAgentResourceTracker CapturedBakeTracker = MakeTracker();
	TestFalse(
		TEXT("captured observation time cannot become authored bake time"),
		TryExecuteRigSimulationSpatialPort(
			CapturedBakeRequest,
			NativePort,
			CapturedBakeTracker,
			Observation,
			PortError));
	TestEqual(
		TEXT("captured/authored confusion has stable error"),
		PortError.Code,
		FString(TEXT("rig_simulation_spatial_authored_time_required")));
	TestEqual(
		TEXT("captured time rejection is pre-native"),
		NativePort.ExecuteCalls,
		0);

	const FUeAgentRigSimulationSpatialPortRequest CapturedSimulationRequest(
		EUeAgentRigSimulationSpatialPortFamily::SimulationObservation,
		{CapturedAt},
		{},
		{InComponent},
		{SocketToBone, BoneToComponent},
		{TEXT("subject_refs"), TEXT("source_revision"),
		 TEXT("temporal_domain"), TEXT("space_contract")});
	FUeAgentResourceTracker CapturedSimulationTracker = MakeTracker();
	TestFalse(
		TEXT("captured readback time cannot become simulation time"),
		TryExecuteRigSimulationSpatialPort(
			CapturedSimulationRequest,
			NativePort,
			CapturedSimulationTracker,
			Observation,
			PortError));
	TestEqual(
		TEXT("captured/simulation confusion has stable error"),
		PortError.Code,
		FString(TEXT(
			"rig_simulation_spatial_temporal_state_not_allowed")));
	TestEqual(
		TEXT("captured simulation rejection is pre-native"),
		NativePort.ExecuteCalls,
		0);

	const FUeAgentTemporalPoint SimulationPoint(
		TEXT("simulation:owned-physics-session"),
		TEXT("6"), TEXT("60"), TEXT("1"), TEXT("simulation"));
	const FUeAgentRigSimulationSpatialPortRequest ValidSimulationProbe(
		EUeAgentRigSimulationSpatialPortFamily::SimulationObservation,
		{SimulationPoint},
		{},
		{InComponent},
		{SocketToBone, BoneToComponent},
		{TEXT("subject_refs"), TEXT("source_revision"),
		 TEXT("temporal_domain"), TEXT("space_contract")});
	FRecordingRigSimulationSpatialNativePort MissingEvidencePort;
	MissingEvidencePort.NextObservation =
		FUeAgentRigSimulationSpatialPortObservation(
			{SimulationPoint},
			{InComponent},
			{});
	FUeAgentResourceTracker MissingEvidenceTracker = MakeTracker();
	TestFalse(
		TEXT("native success without independent readback is rejected"),
		TryExecuteRigSimulationSpatialPort(
			ValidSimulationProbe,
			MissingEvidencePort,
			MissingEvidenceTracker,
			Observation,
			PortError));
	TestEqual(
		TEXT("missing readback has stable error"),
		PortError.Code,
		FString(TEXT(
			"rig_simulation_spatial_readback_evidence_required")));
	TestEqual(
		TEXT("valid request reaches native exactly once"),
		MissingEvidencePort.ExecuteCalls,
		1);

	FRecordingRigSimulationSpatialNativePort MissingTimePort;
	MissingTimePort.NextObservation =
		FUeAgentRigSimulationSpatialPortObservation(
			{},
			{InComponent},
			{TEXT("physics_component_readback:revision-9")});
	FUeAgentResourceTracker MissingTimeTracker = MakeTracker();
	TestFalse(
		TEXT("readback without semantic time is rejected"),
		TryExecuteRigSimulationSpatialPort(
			ValidSimulationProbe,
			MissingTimePort,
			MissingTimeTracker,
			Observation,
			PortError));
	TestEqual(
		TEXT("missing observation time has stable error"),
		PortError.Code,
		FString(TEXT(
			"rig_simulation_spatial_observation_time_required")));
	TestEqual(
		TEXT("missing observation time is post-native"),
		MissingTimePort.ExecuteCalls,
		1);

	const FUeAgentSpatialValue CrossSubjectWithoutEvidence(
		TEXT("socket:/Game/Test/SK_Test:Other"),
		EUeAgentSpatialQuantityKind::Point,
		ComponentFrame,
		{TEXT("12"), TEXT("23"), TEXT("34")},
		CapturedAt);
	FRecordingRigSimulationSpatialNativePort CrossSubjectPort;
	CrossSubjectPort.NextObservation =
		FUeAgentRigSimulationSpatialPortObservation(
			{SimulationPoint},
			{MakeShared<FUeAgentSpatialValue>(
				CrossSubjectWithoutEvidence)},
			{TEXT("physics_component_readback:revision-10")});
	FUeAgentResourceTracker CrossSubjectTracker = MakeTracker();
	TestFalse(
		TEXT("cross-subject readback without conversion evidence is rejected"),
		TryExecuteRigSimulationSpatialPort(
			ValidSimulationProbe,
			CrossSubjectPort,
			CrossSubjectTracker,
			Observation,
			PortError));
	TestEqual(
		TEXT("cross-subject readback has stable error"),
		PortError.Code,
		FString(TEXT(
			"rig_simulation_spatial_observation_subject_frame_mismatch")));
	TestEqual(
		TEXT("cross-subject rejection is post-native"),
		CrossSubjectPort.ExecuteCalls,
		1);

	FRecordingRigSimulationSpatialNativePort ValidPort;
	ValidPort.NextObservation =
		FUeAgentRigSimulationSpatialPortObservation(
			{SimulationPoint},
			{InComponent},
			{TEXT("physics_component_readback:revision-11")});
	FUeAgentResourceTracker ValidTracker = MakeTracker();
	TestTrue(
		TEXT("valid typed request and independent readback pass"),
		TryExecuteRigSimulationSpatialPort(
			ValidSimulationProbe,
			ValidPort,
			ValidTracker,
			Observation,
			PortError));
	TestEqual(
		TEXT("successful request reaches native exactly once"),
		ValidPort.ExecuteCalls,
		1);
	return true;
}
