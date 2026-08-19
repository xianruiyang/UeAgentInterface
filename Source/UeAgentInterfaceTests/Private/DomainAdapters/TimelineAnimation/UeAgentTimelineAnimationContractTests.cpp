#include "Misc/AutomationTest.h"

#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationContract.h"

#include "Core/StateSemantic/UeAgentTemporalContract.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationPort.h"

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
				"DomainAdapters/TimelineAnimation/Fixtures/"
				"timeline_animation_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load TimelineAnimation contract fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse TimelineAnimation contract fixture"),
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
		for (const TSharedPtr<FJsonValue>& Value
			: Object->GetArrayField(Field))
		{
			Result.Add(Value->AsString());
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
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::Nodes, 128},
			{EUeAgentResourceDimension::Items, 1024},
			{EUeAgentResourceDimension::StringBytes, 65536},
			{EUeAgentResourceDimension::WorkUnits, 65536},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 1048576},
		}));
	}

	class FRecordingTimelineAnimationNativePort final
		: public UE::UAI::DomainAdapters::TimelineAnimation::
			IUeAgentTimelineAnimationNativePort
	{
	public:
		bool TryExecute(
			const UE::UAI::DomainAdapters::TimelineAnimation::
				FUeAgentTimelineAnimationPortRequest& Request,
			FUeAgentResourceTracker& Resources,
			UE::UAI::DomainAdapters::TimelineAnimation::
				FUeAgentTimelineAnimationPortObservation& OutObservation,
			UE::UAI::DomainAdapters::TimelineAnimation::
				FUeAgentTimelineAnimationPortError& OutError) override
		{
			++ExecuteCalls;
			OutObservation = NextObservation;
			return true;
		}

		int32 ExecuteCalls = 0;
		UE::UAI::DomainAdapters::TimelineAnimation::
			FUeAgentTimelineAnimationPortObservation NextObservation;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentTimelineAnimationContractTest,
	"UeAgentInterface.DomainAdapters.TimelineAnimation.E0.Contract."
		"TypedPortsAndLegacyBoundary",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentTimelineAnimationContractTest::RunTest(
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
			"uai.domain_adapter.timeline_animation.test_fixture.v1")));
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
	const TArray<TSharedPtr<FJsonValue>>& MappingValues =
		Fixture->GetArrayField(TEXT("legacy_mappings"));
	const TArray<TSharedPtr<FJsonValue>>& RetiredValues =
		Fixture->GetArrayField(TEXT("retired_legacy_ids"));
	TestEqual(
		TEXT("five typed port families"),
		FamilyValues.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("port_family_count"))));
	TestEqual(
		TEXT("nineteen canonical capabilities"),
		CapabilityValues.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("capability_count"))));
	TestEqual(
		TEXT("twenty seven mapped legacy inputs"),
		MappingValues.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("mapped_legacy_count"))));
	TestEqual(
		TEXT("forty five retired legacy inputs"),
		RetiredValues.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("retired_legacy_count"))));
	TestEqual(
		TEXT("all seventy two legacy candidates adjudicated"),
		MappingValues.Num() + RetiredValues.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("legacy_candidate_count"))));

	const TSet<FString> ExpectedFamilies = {
		TEXT("temporal_read"),
		TEXT("animation_keyed_data"),
		TEXT("timeline_structure"),
		TEXT("subject_animation_state"),
		TEXT("timeline_rig_candidate"),
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
			TEXT("family freezes context: ") + FamilyId,
			!Family->GetArrayField(
				TEXT("required_context_facets")).IsEmpty());
		TestEqual(
			TEXT("authored time is distinct from captured time: ")
				+ FamilyId,
			Family->GetArrayField(TEXT("accepted_time_states"))[0]
				->AsString(),
			FString(TEXT("authored")));
		TestTrue(
			TEXT("family declares clock policy: ") + FamilyId,
			!Family->GetStringField(TEXT("clock_policy")).IsEmpty());
		TestTrue(
			TEXT("family declares range policy: ") + FamilyId,
			!Family->GetStringField(TEXT("range_policy")).IsEmpty());
		TestTrue(
			TEXT("family declares spatial boundary: ") + FamilyId,
			!Family->GetStringField(TEXT("spatial_policy")).IsEmpty());
		TestTrue(
			TEXT("family has independent oracle: ") + FamilyId,
			!Family->GetStringField(TEXT("oracle_kind")).IsEmpty());
	}
	bool bExactFamilies = FamilyIds.Num() == ExpectedFamilies.Num();
	for (const FString& ExpectedFamily : ExpectedFamilies)
	{
		bExactFamilies =
			bExactFamilies && FamilyIds.Contains(ExpectedFamily);
	}
	TestTrue(TEXT("exact typed port family set"), bExactFamilies);

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

	TSet<FString> ExternalTargets;
	for (const FString& Target
		: ReadStrings(Fixture, TEXT("external_capability_targets")))
	{
		ExternalTargets.Add(Target);
	}
	TSet<FString> CandidateIds;
	for (const TSharedPtr<FJsonValue>& Value : MappingValues)
	{
		const TSharedPtr<FJsonObject> Mapping = Value->AsObject();
		const FString LegacyId =
			Mapping->GetStringField(TEXT("legacy_id"));
		const FString Disposition =
			Mapping->GetStringField(TEXT("disposition"));
		const FString Target =
			Mapping->GetStringField(TEXT("capability_id"));
		TestFalse(
			TEXT("mapped legacy id is unique: ") + LegacyId,
			CandidateIds.Contains(LegacyId));
		CandidateIds.Add(LegacyId);
		TestTrue(
			TEXT("mapped disposition is allowed: ") + LegacyId,
			Disposition == TEXT("adapt")
				|| Disposition == TEXT("compose")
				|| Disposition == TEXT("split"));
		TestTrue(
			TEXT("mapped target is canonical or existing shared capability: ")
				+ LegacyId,
			CapabilityIds.Contains(Target)
				|| ExternalTargets.Contains(Target));
	}
	for (const TSharedPtr<FJsonValue>& Value : RetiredValues)
	{
		const FString LegacyId = Value->AsString();
		TestFalse(
			TEXT("retired legacy id is unique: ") + LegacyId,
			CandidateIds.Contains(LegacyId));
		CandidateIds.Add(LegacyId);
	}
	TestEqual(
		TEXT("candidate union is exact"),
		CandidateIds.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("legacy_candidate_count"))));
	TestTrue(
		TEXT("Montage editor UI entry is deleted"),
		CandidateIds.Contains(TEXT("montage_open_editor"))
			&& !MappingValues.ContainsByPredicate(
				[](const TSharedPtr<FJsonValue>& Value)
				{
					return Value->AsObject()->GetStringField(
						TEXT("legacy_id")) == TEXT("montage_open_editor");
				}));
	TestTrue(
		TEXT("Sequence editor UI entry is deleted"),
		CandidateIds.Contains(TEXT("sequence_open_level_sequence"))
			&& !MappingValues.ContainsByPredicate(
				[](const TSharedPtr<FJsonValue>& Value)
				{
					return Value->AsObject()->GetStringField(
						TEXT("legacy_id"))
						== TEXT("sequence_open_level_sequence");
				}));

	for (const FString& Reallocated
		: ReadStrings(Fixture, TEXT("reallocated_legacy_ids")))
	{
		TestTrue(
			TEXT("reallocated scenario has exactly one terminal mapping: ")
				+ Reallocated,
			MappingValues.ContainsByPredicate(
				[&Reallocated](const TSharedPtr<FJsonValue>& Value)
				{
					return Value->AsObject()->GetStringField(
						TEXT("legacy_id")) == Reallocated;
				}));
	}
	TestEqual(
		TEXT("pose is session animation state"),
		CapabilityById[TEXT("animation.subject.pose.update")]
			->GetStringField(TEXT("effect_class")),
		FString(TEXT("session_control")));
	TestEqual(
		TEXT("morph is session animation state"),
		CapabilityById[TEXT("animation.subject.morph_weights.update")]
			->GetStringField(TEXT("effect_class")),
		FString(TEXT("session_control")));
	TestEqual(
		TEXT("IK candidate is a typed timeline/rig operation"),
		CapabilityById[TEXT("animation.ik_probe_candidate.apply")]
			->GetStringField(TEXT("port_family_id")),
		FString(TEXT("timeline_rig_candidate")));

	const TArray<FString> ForbiddenOracles =
		ReadStrings(Fixture, TEXT("forbidden_oracles"));
	TestTrue(
		TEXT("legacy success is not an oracle"),
		ForbiddenOracles.Contains(TEXT("legacy_command_success")));
	TestTrue(
		TEXT("handler echo is not an oracle"),
		ForbiddenOracles.Contains(TEXT("legacy_handler_echo")));
	TestTrue(
		TEXT("export echo is not an oracle"),
		ForbiddenOracles.Contains(TEXT("exported_input_echo")));
	TestTrue(
		TEXT("mutation cache is not an oracle"),
		ForbiddenOracles.Contains(TEXT("mutation_cache")));

	using namespace UE::UAI::DomainAdapters::TimelineAnimation;
	TestEqual(
		TEXT("production TimelineAnimation contract version"),
		UAI_TIMELINE_ANIMATION_CONTRACT_V1,
		1);
	const auto& ProductionFamilies =
		GetTimelineAnimationPortFamilyContracts();
	const auto& ProductionCapabilities =
		GetTimelineAnimationCapabilityContracts();
	TestEqual(
		TEXT("production exposes five family contracts"),
		ProductionFamilies.Num(),
		FamilyValues.Num());
	TestEqual(
		TEXT("production exposes nineteen capabilities"),
		ProductionCapabilities.Num(),
		CapabilityValues.Num());
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
			TEXT("production context facets: ") + FamilyId,
			HasExactStrings(
				ProductionFamily.GetRequiredContextFacets(),
				ReadStrings(Expected, TEXT("required_context_facets"))));
		TestEqual(
			TEXT("production clock policy: ") + FamilyId,
			ProductionFamily.GetClockPolicy(),
			Expected->GetStringField(TEXT("clock_policy")));
		TestEqual(
			TEXT("production range policy: ") + FamilyId,
			ProductionFamily.GetRangePolicy(),
			Expected->GetStringField(TEXT("range_policy")));
		TestEqual(
			TEXT("production spatial policy: ") + FamilyId,
			ProductionFamily.GetSpatialPolicy(),
			Expected->GetStringField(TEXT("spatial_policy")));
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
	}

	using namespace UE::UAI::DomainAdapters::TimelineAnimation;
	TestEqual(
		TEXT("production TimelineAnimation typed-port version"),
		UAI_TIMELINE_ANIMATION_TYPED_PORT_V1,
		1);
	const FUeAgentTemporalPoint AuthoredPoint(
		TEXT("sequence:/Game/Test/LS_Rig"),
		TEXT("24"), TEXT("24"), TEXT("1"), TEXT("authored"));
	const FUeAgentTemporalRange AuthoredRange(
		TEXT("sequence:/Game/Test/LS_Rig"),
		TEXT("authored"),
		FUeAgentTemporalBound::Inclusive(AuthoredPoint),
		FUeAgentTemporalBound::Exclusive(FUeAgentTemporalPoint(
			TEXT("sequence:/Game/Test/LS_Rig"),
			TEXT("48"), TEXT("24"), TEXT("1"), TEXT("authored"))));
	const FUeAgentTemporalPoint ObservedAt(
		TEXT("timeline-port-observation"),
		TEXT("1"), TEXT("1"), TEXT("1"), TEXT("captured"));
	const FUeAgentSpatialFrame ControlFrame(
		TEXT("control:/Game/Test/LS_Rig:CR_Test:Goal"),
		TEXT("ue_world_basis"),
		3,
		{{TEXT("1"), TEXT("0"), TEXT("0")},
		 {TEXT("0"), TEXT("1"), TEXT("0")},
		 {TEXT("0"), TEXT("0"), TEXT("1")}},
		{TEXT("0"), TEXT("0"), TEXT("0")},
		TEXT("centimeter"),
		TEXT("control_frame_readback:revision-3"),
		ObservedAt);
	const FUeAgentSpatialValue CandidateTransform(
		TEXT("rig_goal:/Game/Test/LS_Rig:CR_Test:Goal"),
		EUeAgentSpatialQuantityKind::Transform,
		ControlFrame,
		{TEXT("1"), TEXT("0"), TEXT("0"), TEXT("10"),
		 TEXT("0"), TEXT("1"), TEXT("0"), TEXT("20"),
		 TEXT("0"), TEXT("0"), TEXT("1"), TEXT("30"),
		 TEXT("0"), TEXT("0"), TEXT("0"), TEXT("1")},
		ObservedAt);

	const FUeAgentTimelineAnimationPortRequest MissingProbeSession(
		EUeAgentTimelineAnimationPortFamily::TimelineRigCandidate,
		{AuthoredPoint},
		{AuthoredRange},
		{MakeShared<FUeAgentSpatialValue>(CandidateTransform)},
		{TEXT("sequence"), TEXT("rig"), TEXT("goal"),
		 TEXT("source_revision"), TEXT("temporal_domain"),
		 TEXT("space_contract")});
	FRecordingTimelineAnimationNativePort NativePort;
	FUeAgentTimelineAnimationPortObservation Observation;
	FUeAgentTimelineAnimationPortError PortError;
	FUeAgentResourceTracker MissingFacetTracker = MakeTracker();
	TestFalse(
		TEXT("missing frozen probe session is rejected"),
		TryExecuteTimelineAnimationPort(
			MissingProbeSession,
			NativePort,
			MissingFacetTracker,
			Observation,
			PortError));
	TestEqual(
		TEXT("missing context has stable error"),
		PortError.Code,
		FString(TEXT("timeline_animation_context_facet_missing")));
	TestEqual(
		TEXT("invalid request never reaches native port"),
		NativePort.ExecuteCalls,
		0);

	const FUeAgentTemporalPoint CapturedAsAuthored(
		TEXT("animation:/Game/Test/A_Test"),
		TEXT("1"), TEXT("30"), TEXT("1"), TEXT("captured"));
	const FUeAgentTimelineAnimationPortRequest CapturedTimeRequest(
		EUeAgentTimelineAnimationPortFamily::AnimationKeyedData,
		{CapturedAsAuthored},
		{},
		{},
		{TEXT("asset_ref"), TEXT("source_revision"),
		 TEXT("temporal_domain"), TEXT("stable_item_refs")});
	FUeAgentResourceTracker CapturedTimeTracker = MakeTracker();
	TestFalse(
		TEXT("captured observation time cannot become authored key time"),
		TryExecuteTimelineAnimationPort(
			CapturedTimeRequest,
			NativePort,
			CapturedTimeTracker,
			Observation,
			PortError));
	TestEqual(
		TEXT("captured/authored confusion has stable error"),
		PortError.Code,
		FString(TEXT("timeline_animation_authored_time_required")));
	TestEqual(
		TEXT("captured time rejection is pre-native"),
		NativePort.ExecuteCalls,
		0);

	const FUeAgentTimelineAnimationPortRequest ValidRigCandidate(
		EUeAgentTimelineAnimationPortFamily::TimelineRigCandidate,
		{AuthoredPoint},
		{AuthoredRange},
		{MakeShared<FUeAgentSpatialValue>(CandidateTransform)},
		{TEXT("sequence"), TEXT("rig"), TEXT("goal"),
		 TEXT("probe_session"), TEXT("source_revision"),
		 TEXT("temporal_domain"), TEXT("space_contract")});
	FRecordingTimelineAnimationNativePort MissingEvidencePort;
	MissingEvidencePort.NextObservation =
		FUeAgentTimelineAnimationPortObservation(
			{AuthoredPoint},
			{AuthoredRange},
			{MakeShared<FUeAgentSpatialValue>(CandidateTransform)},
			{});
	FUeAgentResourceTracker MissingEvidenceTracker = MakeTracker();
	TestFalse(
		TEXT("native success without independent evidence is rejected"),
		TryExecuteTimelineAnimationPort(
			ValidRigCandidate,
			MissingEvidencePort,
			MissingEvidenceTracker,
			Observation,
			PortError));
	TestEqual(
		TEXT("missing readback evidence has stable error"),
		PortError.Code,
		FString(TEXT("timeline_animation_readback_evidence_required")));
	TestEqual(
		TEXT("invalid observation follows exactly one native call"),
		MissingEvidencePort.ExecuteCalls,
		1);

	NativePort.NextObservation = FUeAgentTimelineAnimationPortObservation(
		{AuthoredPoint},
		{AuthoredRange},
		{MakeShared<FUeAgentSpatialValue>(CandidateTransform)},
		{TEXT("timeline_readback:sequence-revision-11"),
		 TEXT("rig_control_readback:revision-4")});
	FUeAgentResourceTracker ValidTracker = MakeTracker();
	TestTrue(
		TEXT("valid rig candidate reaches one native operation"),
		TryExecuteTimelineAnimationPort(
			ValidRigCandidate,
			NativePort,
			ValidTracker,
			Observation,
			PortError));
	TestEqual(
		TEXT("valid request reaches native port exactly once"),
		NativePort.ExecuteCalls,
		1);
	return true;
}
