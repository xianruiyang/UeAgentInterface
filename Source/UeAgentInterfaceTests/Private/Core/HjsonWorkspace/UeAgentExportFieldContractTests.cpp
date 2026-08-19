#include "Misc/AutomationTest.h"
#include "Core/HjsonWorkspace/UeAgentExportFieldContract.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	static FUeAgentResourceTracker MakeTracker()
	{
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 1024 * 1024},
			{EUeAgentResourceDimension::OutputBytes, 1024 * 1024},
			{EUeAgentResourceDimension::StringBytes, 256 * 1024},
			{EUeAgentResourceDimension::Nodes, 4096},
			{EUeAgentResourceDimension::Items, 4096},
			{EUeAgentResourceDimension::Files, 32},
			{EUeAgentResourceDimension::ReferenceEdges, 4096},
			{EUeAgentResourceDimension::WorkUnits, 32768},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 4 * 1024 * 1024},
		}));
	}

	static FExportFieldContract MakeField(
		const FString& ContractId,
		const FString& FieldId,
		const FString& ExternalName,
		const FString& ValueContract,
		const FString& SemanticRole,
		EExportFieldEmissionPolicy EmissionPolicy,
		EUeAgentDefaultSourceKind DefaultSource,
		bool bDefaultReproducible,
		bool bPresenceSensitive,
		const FString& ValueKind)
	{
		return FExportFieldContract(
			ContractId,
			1,
			FieldId,
			ExternalName,
			TEXT("fixture:blueprint:cdo"),
			ValueContract,
			SemanticRole,
			TEXT("conditional"),
			{TEXT("state"), TEXT("edit_bundle"), TEXT("readback")},
			bPresenceSensitive,
			EmissionPolicy,
			DefaultSource,
			bDefaultReproducible,
			TEXT("class_defaults.core"),
			TEXT("HX-002 independent fixture"),
			ExternalName,
			TEXT("static description"),
			ValueKind);
	}

	static FUeAgentStateEntry MakeEntry(
		const FString& FieldId,
		const FString& ExternalName,
		const FString& ValueLexeme,
		const FString& DefaultLexeme,
		EUeAgentDefaultSourceKind DefaultSource,
		EUeAgentAuthoredPresence AuthoredPresence,
		EUeAgentOverrideState OverrideState,
		const FUeAgentStateRevision& Revision,
		FUeAgentResourceTracker& Resources,
		FAutomationTestBase* Test)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentCanonicalValuePtr Default;
		FUeAgentStateSemanticError Error;
		Test->TestTrue(
			TEXT("create exact field value"),
			FUeAgentCanonicalValue::TryInteger(
				ValueLexeme,
				Resources,
				Value,
				Error));
		Test->TestTrue(
			TEXT("create exact field default"),
			FUeAgentCanonicalValue::TryInteger(
				DefaultLexeme,
				Resources,
				Default,
				Error));
		return FUeAgentStateEntry(
			FUeAgentCanonicalIdentity(
				FieldId,
				TEXT("blueprint_class_default"),
				TEXT("fixture:blueprint:cdo"),
				ExternalName,
				TEXT("editable_property"),
				TEXT("hx002")),
			FUeAgentCanonicalType(
				TEXT("ue.int32.v1"),
				EUeAgentValueKind::Integer,
				false,
				TEXT("scalar")),
			Value.ToSharedRef(),
			FUeAgentDefaultValue(
				DefaultSource,
				TEXT("fixture:default"),
				TEXT("fixture:independent-default"),
				Default.ToSharedRef()),
			FUeAgentStateDetails(
				ExternalName,
				TEXT("static description"),
				{TEXT("Fixture")},
				true,
				TEXT("available"),
				{}),
			Revision,
			AuthoredPresence,
			OverrideState);
	}

	static FUeAgentCanonicalSnapshotPtr MakeSnapshot(
		const TArray<FUeAgentStateEntry>& Entries,
		const FUeAgentStateRevision& Revision,
		FUeAgentResourceTracker& Resources,
		FAutomationTestBase* Test)
	{
		FUeAgentCanonicalSnapshotPtr Snapshot;
		FUeAgentStateSemanticError Error;
		Test->TestTrue(
			TEXT("create independent field snapshot"),
			FUeAgentCanonicalSnapshot::TryCreate(
				TEXT("fixture:hx002:field-snapshot"),
				EUeAgentStatePlane::AuthoringMemory,
				Revision,
				FUeAgentTemporalPoint(TEXT("fixture"), TEXT("1")),
				FUeAgentStateProvenance(
					TEXT("independent_readback"),
					TEXT("fixture:hx002"),
					TEXT("fixture:hx002:independent")),
				Entries,
				Resources,
				Snapshot,
				Error));
		return Snapshot;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentExportFieldOwnerConflictTest,
	"UeAgentInterface.Core.HjsonWorkspace.ExportFieldContract.OwnerConflict",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentExportFieldOwnerConflictTest::RunTest(
	const FString& Parameters)
{
	const FExportFieldContract Field = MakeField(
		TEXT("fixture.field.actor_label"),
		TEXT("ue.actor.label"),
		TEXT("ActorLabel"),
		TEXT("ue.string.v1"),
		TEXT("editable"),
		EExportFieldEmissionPolicy::WhenNonDefault,
		EUeAgentDefaultSourceKind::ClassDefaultObject,
		true,
		false,
		TEXT("string"));
	const TArray<FExportFieldRegistration> Registrations = {
		FExportFieldRegistration(
			Field,
			EExportFieldOwnerSource::Handwritten,
			TEXT("handwritten:actor_core:label")),
		FExportFieldRegistration(
			Field,
			EExportFieldOwnerSource::Reflection,
			TEXT("reflection:AActor:ActorLabel")),
	};
	FExportFieldRegistryPtr Registry;
	FWorkspaceError Error;

	TestFalse(
		TEXT("same semantic field cannot have handwritten and reflection owners"),
		TryCompileExportFieldRegistry(
			Registrations,
			TEXT("fixture.export_fields"),
			1,
			Registry,
			Error));
	TestEqual(
		TEXT("owner conflict is typed"),
		Error.GetCode(),
		FString(TEXT("field_owner_conflict")));
	TestTrue(
		TEXT("owner conflict reports both sources"),
		Error.GetSourceUnit().Contains(TEXT("handwritten"))
			&& Error.GetSourceUnit().Contains(TEXT("reflection")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentExportFieldPresenceProjectionTest,
	"UeAgentInterface.Core.HjsonWorkspace.ExportFieldContract.PresenceAndCompactProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentExportFieldPresenceProjectionTest::RunTest(
	const FString& Parameters)
{
	const FExportFieldContract Inherited = MakeField(
		TEXT("fixture.field.input_priority"),
		TEXT("ue.blueprint.input_priority"),
		TEXT("InputPriority"),
		TEXT("ue.int32.v1"),
		TEXT("override"),
		EExportFieldEmissionPolicy::WhenOverridden,
		EUeAgentDefaultSourceKind::Parent,
		true,
		true,
		TEXT("integer"));
	const FExportFieldContract Dynamic = MakeField(
		TEXT("fixture.field.dynamic_seed"),
		TEXT("ue.fixture.dynamic_seed"),
		TEXT("DynamicSeed"),
		TEXT("ue.int32.v1"),
		TEXT("editable"),
		EExportFieldEmissionPolicy::WhenNonDefault,
		EUeAgentDefaultSourceKind::PostCreateReadback,
		false,
		false,
		TEXT("integer"));
	const TArray<FExportFieldRegistration> Registrations = {
		FExportFieldRegistration(
			Inherited,
			EExportFieldOwnerSource::Reflection,
			TEXT("reflection:AActor:InputPriority")),
		FExportFieldRegistration(
			Dynamic,
			EExportFieldOwnerSource::Reflection,
			TEXT("reflection:UFixture:DynamicSeed")),
	};
	FExportFieldRegistryPtr Registry;
	FWorkspaceError Error;
	if (!TestTrue(
		TEXT("compile unique field registry"),
		TryCompileExportFieldRegistry(
			Registrations,
			TEXT("fixture.export_fields"),
			1,
			Registry,
			Error)))
	{
		return false;
	}

	FUeAgentResourceTracker Resources = MakeTracker();
	const FUeAgentStateRevision Revision(
		TEXT("fixture:hx002"),
		TEXT("hx002"),
		1);
	const TArray<FUeAgentStateEntry> Entries = {
		MakeEntry(
			TEXT("ue.blueprint.input_priority"),
			TEXT("InputPriority"),
			TEXT("2"),
			TEXT("2"),
			EUeAgentDefaultSourceKind::Parent,
			EUeAgentAuthoredPresence::Present,
			EUeAgentOverrideState::Overridden,
			Revision,
			Resources,
			this),
		MakeEntry(
			TEXT("ue.fixture.dynamic_seed"),
			TEXT("DynamicSeed"),
			TEXT("7"),
			TEXT("7"),
			EUeAgentDefaultSourceKind::PostCreateReadback,
			EUeAgentAuthoredPresence::Absent,
			EUeAgentOverrideState::None,
			Revision,
			Resources,
			this),
	};
	const FUeAgentCanonicalSnapshotPtr Snapshot =
		MakeSnapshot(Entries, Revision, Resources, this);
	if (!Snapshot.IsValid())
	{
		return false;
	}

	FPropertySetProjectionPtr Projection;
	TestTrue(
		TEXT("project compact property set"),
		TryProjectPropertySet(
			Registry.ToSharedRef(),
			Snapshot.ToSharedRef(),
			TEXT("fixture:blueprint:cdo"),
			TEXT("class_defaults.core"),
			Resources,
			Projection,
			Error));
	if (!Projection.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("two required values"), Projection->GetValues().Num(), 2);
	TestTrue(
		TEXT("authored override equal to base is preserved"),
		Projection->GetValues().Contains(TEXT("InputPriority")));
	TestTrue(
		TEXT("unreproducible equal default is preserved"),
		Projection->GetValues().Contains(TEXT("DynamicSeed")));
	TestEqual(
		TEXT("instance projection carries no repeated descriptions"),
		Projection->GetDescriptions().Num(),
		0);
	FExportFieldDescription Description;
	TestTrue(
		TEXT("static metadata is available on demand"),
		TryDescribeExportField(
			Registry.ToSharedRef(),
			TEXT("ue.blueprint.input_priority"),
			Description,
			Error));
	TestEqual(
		TEXT("field description binds stable external name"),
		Description.GetExternalName(),
		FString(TEXT("InputPriority")));
	const FExportFieldRegistration* ApplyField =
		Registry->FindFieldByExternalName(
			TEXT("fixture:blueprint:cdo"),
			TEXT("class_defaults.core"),
			TEXT("InputPriority"));
	if (!TestNotNull(
		TEXT("Apply resolves HJSON external name through the registry"),
		ApplyField))
	{
		return false;
	}
	const FString SharedFieldId =
		TEXT("ue.blueprint.input_priority");
	TestEqual(
		TEXT("Query uses the shared field id"),
		Description.GetFieldId(),
		SharedFieldId);
	TestTrue(
		TEXT("HJSON projection carries the shared field id"),
		Projection->GetEmittedFieldIds().Contains(SharedFieldId));
	TestEqual(
		TEXT("Apply resolves the shared field id"),
		ApplyField->GetField().GetFieldId(),
		SharedFieldId);
	bool bVerificationUsesSharedFieldId = false;
	for (const FUeAgentStateEntry& Entry : Snapshot->GetEntries())
	{
		if (Entry.GetIdentity().GetSemanticId() == SharedFieldId)
		{
			bVerificationUsesSharedFieldId = true;
			break;
		}
	}
	TestTrue(
		TEXT("Verification snapshot uses the shared field id"),
		bVerificationUsesSharedFieldId);
	return true;
}
