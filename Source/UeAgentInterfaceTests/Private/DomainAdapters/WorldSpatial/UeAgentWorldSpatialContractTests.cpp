#include "Misc/AutomationTest.h"

#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialContract.h"

#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialPort.h"

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
				"DomainAdapters/WorldSpatial/Fixtures/"
				"world_spatial_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load WorldSpatial contract fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse WorldSpatial contract fixture"),
				FJsonSerializer::Deserialize(Reader, Fixture)
					&& Fixture.IsValid()))
		{
			return nullptr;
		}
		return Fixture;
	}

	TArray<FString> ReadStringArray(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field)
	{
		TArray<FString> Result;
		for (const TSharedPtr<FJsonValue>& Value
			: Object->GetArrayField(Field))
		{
			Result.Add(Value->AsString());
		}
		return Result;
	}

	TArray<int32> ReadIntArray(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field)
	{
		TArray<int32> Result;
		for (const TSharedPtr<FJsonValue>& Value
			: Object->GetArrayField(Field))
		{
			Result.Add(static_cast<int32>(Value->AsNumber()));
		}
		return Result;
	}

	FUeAgentResourceTracker MakeSpatialTracker()
	{
		return FUeAgentResourceTracker();
	}

	bool HasExactStrings(
		TArray<FString> Left,
		TArray<FString> Right)
	{
		Left.Sort();
		Right.Sort();
		return Left == Right;
	}

	bool HasExactInts(TArray<int32> Left, TArray<int32> Right)
	{
		Left.Sort();
		Right.Sort();
		return Left == Right;
	}

	class FRecordingWorldSpatialNativePort final
		: public UE::UAI::DomainAdapters::WorldSpatial::
			IUeAgentWorldSpatialNativePort
	{
	public:
		bool TryExecute(
			const UE::UAI::DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialPortRequest& Request,
			FUeAgentResourceTracker& Resources,
			UE::UAI::DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialPortObservation& OutObservation,
			UE::UAI::DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialPortError& OutError) override
		{
			++ExecuteCalls;
			OutObservation = NextObservation;
			return true;
		}

		int32 ExecuteCalls = 0;
		UE::UAI::DomainAdapters::WorldSpatial::
			FUeAgentWorldSpatialPortObservation NextObservation;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialContractTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E0.Contract."
		"CurrentExplicitSpaceBridgeAndReadbackContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialContractTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}

	const FString SpaceContractId =
		Fixture->GetStringField(TEXT("space_contract_id"));
	TestEqual(
		TEXT("fixture schema"),
		Fixture->GetStringField(TEXT("schema")),
		FString(TEXT("uai.domain_adapter.world_spatial.test_fixture.v1")));
	TestEqual(
		TEXT("shared space contract"),
		SpaceContractId,
		FString(TEXT("uai.space_contract.v1")));

	const TArray<TSharedPtr<FJsonValue>>& FamilyValues =
		Fixture->GetArrayField(TEXT("port_families"));
	const TArray<TSharedPtr<FJsonValue>>& CapabilityValues =
		Fixture->GetArrayField(TEXT("capabilities"));

	const TSet<FString> ExpectedFamilies = {
		TEXT("graph_view"),
		TEXT("graph_layout"),
		TEXT("object_transform"),
		TEXT("viewport_projection"),
		TEXT("viewport_session_state"),
	};
	TSet<FString> FamilyIds;
	TMap<FString, TSharedPtr<FJsonObject>> FamilyById;
	for (const TSharedPtr<FJsonValue>& Value : FamilyValues)
	{
		const TSharedPtr<FJsonObject> Family = Value->AsObject();
		const FString FamilyId =
			Family->GetStringField(TEXT("family_id"));
		TestFalse(
			TEXT("port family id is unique: ") + FamilyId,
			FamilyIds.Contains(FamilyId));
		FamilyIds.Add(FamilyId);
		FamilyById.Add(FamilyId, Family);

		const TArray<int32> Dimensions =
			ReadIntArray(Family, TEXT("dimensions"));
		TestTrue(
			TEXT("port family declares dimension: ") + FamilyId,
			!Dimensions.IsEmpty());
		for (const int32 Dimension : Dimensions)
		{
			TestTrue(
				TEXT("port family dimension is 2D or 3D: ") + FamilyId,
				Dimension == 2 || Dimension == 3);
		}
		TestTrue(
			TEXT("port family freezes subject: ") + FamilyId,
			!Family->GetStringField(TEXT("subject_policy")).IsEmpty());
		TestTrue(
			TEXT("port family freezes Fsemantic: ") + FamilyId,
			!Family->GetStringField(
				TEXT("semantic_frame_policy")).IsEmpty());
		TestTrue(
			TEXT("port family freezes Fwrite: ") + FamilyId,
			!Family->GetStringField(
				TEXT("write_frame_policy")).IsEmpty());
		TestTrue(
			TEXT("port family freezes Fread: ") + FamilyId,
			!Family->GetStringField(
				TEXT("read_frame_policy")).IsEmpty());
		TestTrue(
			TEXT("port family freezes Fverify: ") + FamilyId,
			!Family->GetStringField(
				TEXT("verify_frame_policy")).IsEmpty());
		TestTrue(
			TEXT("port family freezes conversion chain: ") + FamilyId,
			!Family->GetStringField(
				TEXT("conversion_chain")).IsEmpty());
		TestTrue(
			TEXT("port family requires explicit context: ") + FamilyId,
			!Family->GetArrayField(
				TEXT("required_context_facets")).IsEmpty());
		TestTrue(
			TEXT("port family rejects implicit context: ") + FamilyId,
			!Family->GetArrayField(
				TEXT("forbidden_implicit_facets")).IsEmpty());
	}
	for (const FString& ExpectedFamily : ExpectedFamilies)
	{
		TestTrue(TEXT("required current port family exists: ")
			+ ExpectedFamily,
			FamilyIds.Contains(ExpectedFamily));
	}
	TestEqual(
		TEXT("2D to 3D uses explicit camera projection bridge"),
		FamilyById[TEXT("viewport_projection")]
			->GetStringField(TEXT("dimension_bridge")),
		FString(TEXT("camera_deprojection")));
	TestTrue(
		TEXT("projection family keeps both dimensions explicit"),
		HasExactInts(
			ReadIntArray(
				FamilyById[TEXT("viewport_projection")],
				TEXT("dimensions")),
			{2, 3}));
	TestEqual(
		TEXT("viewport session keeps 2D and 3D values separate"),
		FamilyById[TEXT("viewport_session_state")]
			->GetStringField(TEXT("dimension_bridge")),
		FString(TEXT("separate_values_only")));

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
			TEXT("capability id is unique: ") + CapabilityId,
			CapabilityIds.Contains(CapabilityId));
		TestFalse(
			TEXT("descriptor id is unique: ") + DescriptorId,
			DescriptorIds.Contains(DescriptorId));
		TestFalse(
			TEXT("runtime identity is not a migration placeholder: ")
				+ DescriptorId,
			DescriptorId.StartsWith(TEXT("uai.migration."))
				|| CapabilityId.StartsWith(TEXT("uai.migration.")));
		CapabilityIds.Add(CapabilityId);
		DescriptorIds.Add(DescriptorId);
		CapabilityById.Add(CapabilityId, Capability);

		const FString OperationKind =
			Capability->GetStringField(TEXT("operation_kind"));
		const FString EffectClass =
			Capability->GetStringField(TEXT("effect_class"));
		TestTrue(
			TEXT("operation kind is query or operation: ") + CapabilityId,
			OperationKind == TEXT("query")
				|| OperationKind == TEXT("operation"));
		TestTrue(
			TEXT("effect class matches operation kind: ") + CapabilityId,
			(OperationKind == TEXT("query")
				&& EffectClass == TEXT("read_only"))
				|| (OperationKind == TEXT("operation")
					&& (EffectClass == TEXT("write")
						|| EffectClass == TEXT("session_control"))));
		TestTrue(
			TEXT("capability references a typed port: ") + CapabilityId,
			FamilyIds.Contains(Capability->GetStringField(
				TEXT("port_family_id"))));
		TestEqual(
			TEXT("capability uses shared space contract: ") + CapabilityId,
			Capability->GetStringField(TEXT("space_contract_id")),
			SpaceContractId);
		TestTrue(
			TEXT("capability has independent oracle: ") + CapabilityId,
			!Capability->GetStringField(TEXT("oracle_kind")).IsEmpty());
	}

	TSet<FString> ForbiddenOracles;
	for (const FString& ForbiddenOracle
		: ReadStringArray(Fixture, TEXT("forbidden_oracles")))
	{
		ForbiddenOracles.Add(ForbiddenOracle);
	}
	TestTrue(
		TEXT("screenshot alone is not an oracle"),
		ForbiddenOracles.Contains(TEXT("screenshot_only")));

	const FUeAgentTemporalPoint ObservedAt(
		TEXT("world_spatial_contract_clock"),
		TEXT("1"));
	const FUeAgentSpatialFrame ComponentFrame(
		TEXT("component_local:/Game/Test.Actor.Mesh"),
		TEXT("ue_world_basis"),
		3,
		{{TEXT("1"), TEXT("0"), TEXT("0")},
		 {TEXT("0"), TEXT("1"), TEXT("0")},
		 {TEXT("0"), TEXT("0"), TEXT("1")}},
		{TEXT("10"), TEXT("20"), TEXT("30")},
		TEXT("centimeter"),
		TEXT("component_transform_readback"),
		ObservedAt);
	const FUeAgentSpatialFrame WorldFrame(
		TEXT("ue_world"),
		TEXT("ue_world_basis"),
		3,
		{{TEXT("1"), TEXT("0"), TEXT("0")},
		 {TEXT("0"), TEXT("1"), TEXT("0")},
		 {TEXT("0"), TEXT("0"), TEXT("1")}},
		{TEXT("0"), TEXT("0"), TEXT("0")},
		TEXT("centimeter"),
		TEXT("world_frame_readback"),
		ObservedAt);
	const FUeAgentSpatialConversion ComponentToWorld(
		TEXT("component_to_world:/Game/Test.Actor.Mesh"),
		ComponentFrame.GetFrameId(),
		WorldFrame.GetFrameId(),
		3,
		3,
		{{TEXT("1"), TEXT("0"), TEXT("0")},
		 {TEXT("0"), TEXT("1"), TEXT("0")},
		 {TEXT("0"), TEXT("0"), TEXT("1")}},
		{TEXT("10"), TEXT("20"), TEXT("30")},
		ObservedAt,
		TEXT("component_transform_readback"));
	const FUeAgentSpatialValue ComponentPoint(
		TEXT("component:/Game/Test.Actor.Mesh"),
		EUeAgentSpatialQuantityKind::Point,
		ComponentFrame,
		{TEXT("1"), TEXT("2"), TEXT("3")},
		ObservedAt);
	const FUeAgentSpatialValue ExpectedWorldPoint(
		TEXT("component:/Game/Test.Actor.Mesh"),
		EUeAgentSpatialQuantityKind::Point,
		WorldFrame,
		{TEXT("11"), TEXT("22"), TEXT("33")},
		ObservedAt,
		{ComponentToWorld.GetEvidence()});
	FUeAgentResourceTracker Tracker = MakeSpatialTracker();
	FUeAgentSpatialValuePtr Converted;
	FUeAgentStateSemanticError SpatialError;
	TestTrue(
		TEXT("explicit same-dimension frame conversion succeeds"),
		TryConvertSpatialValue(
			ComponentPoint,
			WorldFrame,
			ComponentToWorld,
			Tracker,
			Converted,
			SpatialError));
	TestTrue(
		TEXT("frame-converted value is independently readable in Fverify"),
		Converted.IsValid()
			&& Converted->SemanticallyEquals(ExpectedWorldPoint));

	const FUeAgentSpatialValue OtherSubject(
		TEXT("component:/Game/Test.Other.Mesh"),
		EUeAgentSpatialQuantityKind::Point,
		WorldFrame,
		{TEXT("11"), TEXT("22"), TEXT("33")},
		ObservedAt,
		{ComponentToWorld.GetEvidence()});
	TestFalse(
		TEXT("different subjects are not directly comparable"),
		ExpectedWorldPoint.SemanticallyEquals(OtherSubject));

	const FUeAgentSpatialFrame GraphFrame(
		TEXT("graph_canvas:/Game/Test.BP:EventGraph"),
		TEXT("graph_canvas_basis"),
		2,
		{{TEXT("1"), TEXT("0")},
		 {TEXT("0"), TEXT("1")}},
		{TEXT("0"), TEXT("0")},
		TEXT("graph_unit"),
		TEXT("graph_node_readback"),
		ObservedAt);
	const FUeAgentSpatialValue GraphPoint(
		ExpectedWorldPoint.GetSubject(),
		EUeAgentSpatialQuantityKind::Point,
		GraphFrame,
		{TEXT("11"), TEXT("22")},
		ObservedAt);
	TestFalse(
		TEXT("different dimensions are not directly comparable"),
		ExpectedWorldPoint.SemanticallyEquals(GraphPoint));

	using namespace UE::UAI::DomainAdapters::WorldSpatial;
	TestEqual(
		TEXT("production WorldSpatial contract version"),
		UAI_WORLD_SPATIAL_CONTRACT_V1,
		1);
	const auto& ProductionFamilies =
		GetWorldSpatialPortFamilyContracts();
	const auto& ProductionCapabilities =
		GetWorldSpatialCapabilityContracts();
	for (const auto& ProductionFamily : ProductionFamilies)
	{
		const FString FamilyId = ProductionFamily.GetFamilyId();
		TestTrue(
			TEXT("production family exists in contract fixture: ") + FamilyId,
			FamilyById.Contains(FamilyId));
		if (!FamilyById.Contains(FamilyId))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Expected = FamilyById[FamilyId];
		TestEqual(TEXT("production family space contract: ") + FamilyId,
			ProductionFamily.GetSpaceContractId(), SpaceContractId);
		TestTrue(TEXT("production family dimensions: ") + FamilyId,
			ProductionFamily.GetDimensions()
				== ReadIntArray(Expected, TEXT("dimensions")));
		TestEqual(TEXT("production family subject policy: ") + FamilyId,
			ProductionFamily.GetSubjectPolicy(),
			Expected->GetStringField(TEXT("subject_policy")));
		TestEqual(TEXT("production family Fsemantic policy: ") + FamilyId,
			ProductionFamily.GetSemanticFramePolicy(),
			Expected->GetStringField(TEXT("semantic_frame_policy")));
		TestEqual(TEXT("production family Fwrite policy: ") + FamilyId,
			ProductionFamily.GetWriteFramePolicy(),
			Expected->GetStringField(TEXT("write_frame_policy")));
		TestEqual(TEXT("production family Fread policy: ") + FamilyId,
			ProductionFamily.GetReadFramePolicy(),
			Expected->GetStringField(TEXT("read_frame_policy")));
		TestEqual(TEXT("production family Fverify policy: ") + FamilyId,
			ProductionFamily.GetVerifyFramePolicy(),
			Expected->GetStringField(TEXT("verify_frame_policy")));
		TestEqual(TEXT("production family dimension bridge: ") + FamilyId,
			ProductionFamily.GetDimensionBridge(),
			Expected->GetStringField(TEXT("dimension_bridge")));
		TestEqual(TEXT("production family conversion chain: ") + FamilyId,
			ProductionFamily.GetConversionChain(),
			Expected->GetStringField(TEXT("conversion_chain")));
		TestTrue(TEXT("production family context facets: ") + FamilyId,
			HasExactStrings(
				ProductionFamily.GetRequiredContextFacets(),
				ReadStringArray(
					Expected,
					TEXT("required_context_facets"))));
	}
	for (const auto& ProductionCapability : ProductionCapabilities)
	{
		const FString CapabilityId =
			ProductionCapability.GetCapabilityId();
		TestTrue(
			TEXT("production capability exists in contract fixture: ")
				+ CapabilityId,
			CapabilityById.Contains(CapabilityId));
		if (!CapabilityById.Contains(CapabilityId))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Expected =
			CapabilityById[CapabilityId];
		TestEqual(TEXT("production descriptor id: ") + CapabilityId,
			ProductionCapability.GetDescriptorId(),
			Expected->GetStringField(TEXT("descriptor_id")));
		TestEqual(TEXT("production descriptor version: ") + CapabilityId,
			ProductionCapability.GetVersion(),
			static_cast<int32>(Expected->GetNumberField(TEXT("version"))));
		TestEqual(TEXT("production operation kind: ") + CapabilityId,
			ProductionCapability.GetOperationKind(),
			Expected->GetStringField(TEXT("operation_kind")));
		TestEqual(TEXT("production effect class: ") + CapabilityId,
			ProductionCapability.GetEffectClass(),
			Expected->GetStringField(TEXT("effect_class")));
		TestEqual(TEXT("production typed port: ") + CapabilityId,
			ProductionCapability.GetPortFamilyId(),
			Expected->GetStringField(TEXT("port_family_id")));
		TestEqual(TEXT("production space contract: ") + CapabilityId,
			ProductionCapability.GetSpaceContractId(),
			SpaceContractId);
	}

	using namespace UE::UAI::DomainAdapters::WorldSpatial;
	TestEqual(
		TEXT("production WorldSpatial typed-port version"),
		UAI_WORLD_SPATIAL_TYPED_PORT_V1,
		1);
	const FUeAgentWorldSpatialPortRequest InvalidDimensionRequest(
		EUeAgentWorldSpatialPortFamily::ObjectTransform,
		{MakeShared<FUeAgentSpatialValue>(GraphPoint)},
		{TEXT("world"), TEXT("target_refs")});
	FRecordingWorldSpatialNativePort NativePort;
	NativePort.NextObservation = FUeAgentWorldSpatialPortObservation(
		{MakeShared<FUeAgentSpatialValue>(ExpectedWorldPoint)},
		FString());
	FUeAgentWorldSpatialPortObservation PortObservation;
	FUeAgentWorldSpatialPortError PortError;
	FUeAgentResourceTracker PortTracker = MakeSpatialTracker();
	TestFalse(
		TEXT("invalid dimension is rejected before native execution"),
		TryExecuteWorldSpatialPort(
			InvalidDimensionRequest,
			NativePort,
			PortTracker,
			PortObservation,
			PortError));
	TestEqual(
		TEXT("invalid dimension has stable error"),
		PortError.Code,
		FString(TEXT("world_spatial_input_dimension_mismatch")));
	TestEqual(
		TEXT("invalid request never reaches native port"),
		NativePort.ExecuteCalls,
		0);

	const FUeAgentWorldSpatialPortRequest MissingContextRequest(
		EUeAgentWorldSpatialPortFamily::ObjectTransform,
		{MakeShared<FUeAgentSpatialValue>(ComponentPoint)},
		{TEXT("world")});
	FUeAgentResourceTracker MissingContextTracker = MakeSpatialTracker();
	TestFalse(
		TEXT("missing frozen target context is rejected"),
		TryExecuteWorldSpatialPort(
			MissingContextRequest,
			NativePort,
			MissingContextTracker,
			PortObservation,
			PortError));
	TestEqual(
		TEXT("missing context has stable error"),
		PortError.Code,
		FString(TEXT("world_spatial_context_facet_missing")));
	TestEqual(
		TEXT("missing context never reaches native port"),
		NativePort.ExecuteCalls,
		0);

	const FUeAgentSpatialFrame ScreenFrame(
		TEXT("viewport_pixel:viewport-1"),
		TEXT("viewport_pixel_basis:viewport-1"),
		2,
		{{TEXT("1"), TEXT("0")},
		 {TEXT("0"), TEXT("1")}},
		{TEXT("0"), TEXT("0")},
		TEXT("pixel"),
		TEXT("viewport_size_readback:1920x1080"),
		ObservedAt);
	const FUeAgentSpatialValue ScreenPoint(
		TEXT("screen_point:viewport-1:960:540"),
		EUeAgentSpatialQuantityKind::Point,
		ScreenFrame,
		{TEXT("960"), TEXT("540")},
		ObservedAt);
	const FUeAgentSpatialValue WorldRayDirection(
		TEXT("world_ray:viewport-1:960:540"),
		EUeAgentSpatialQuantityKind::Direction,
		WorldFrame,
		{TEXT("1"), TEXT("0"), TEXT("0")},
		ObservedAt,
		{TEXT("camera_state_readback:viewport-1")});
	const FUeAgentWorldSpatialPortRequest ProjectionRequest(
		EUeAgentWorldSpatialPortFamily::ViewportProjection,
		{MakeShared<FUeAgentSpatialValue>(ScreenPoint)},
		{TEXT("viewport"), TEXT("camera")});
	NativePort.NextObservation = FUeAgentWorldSpatialPortObservation(
		{MakeShared<FUeAgentSpatialValue>(WorldRayDirection)},
		FString());
	FUeAgentResourceTracker MissingBridgeTracker = MakeSpatialTracker();
	TestFalse(
		TEXT("2D to 3D observation requires bridge evidence"),
		TryExecuteWorldSpatialPort(
			ProjectionRequest,
			NativePort,
			MissingBridgeTracker,
			PortObservation,
			PortError));
	TestEqual(
		TEXT("missing bridge evidence has stable error"),
		PortError.Code,
		FString(TEXT(
			"world_spatial_dimension_bridge_evidence_required")));
	TestEqual(
		TEXT("projection native hook ran once"),
		NativePort.ExecuteCalls,
		1);

	NativePort.NextObservation = FUeAgentWorldSpatialPortObservation(
		{MakeShared<FUeAgentSpatialValue>(WorldRayDirection)},
		TEXT("camera_deprojection:viewport-1:projection-revision-7"));
	FUeAgentResourceTracker ValidBridgeTracker = MakeSpatialTracker();
	TestTrue(
		TEXT("explicit camera deprojection bridge is accepted"),
		TryExecuteWorldSpatialPort(
			ProjectionRequest,
			NativePort,
			ValidBridgeTracker,
			PortObservation,
			PortError));
	TestEqual(
		TEXT("valid projection reaches native hook once"),
		NativePort.ExecuteCalls,
		2);
	return true;
}
