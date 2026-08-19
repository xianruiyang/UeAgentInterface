#include "Misc/AutomationTest.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialRoutes.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace
{
	using namespace UE::UAI::Composition::GraphCompound;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker MakeTracker()
	{
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 8 * 1024 * 1024},
			{EUeAgentResourceDimension::OutputBytes, 8 * 1024 * 1024},
			{EUeAgentResourceDimension::StringBytes, 8 * 1024 * 1024},
			{EUeAgentResourceDimension::Nodes, 65536},
			{EUeAgentResourceDimension::Items, 65536},
			{EUeAgentResourceDimension::Files, 128},
			{EUeAgentResourceDimension::ReferenceEdges, 131072},
			{EUeAgentResourceDimension::WorkUnits, 524288},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 32 * 1024 * 1024},
		}));
	}

	const TArray<FString>& ExpectedCapabilities()
	{
		static const TArray<FString> Values = {
			TEXT("blueprint.asset.create"),
			TEXT("blueprint.authoring.apply"),
			TEXT("blueprint.component.events.discover"),
			TEXT("umg.asset.create"),
			TEXT("umg.authoring.apply"),
			TEXT("umg.widget.events.discover"),
			TEXT("material.asset.create"),
			TEXT("material.graph.apply"),
			TEXT("material.function.graph.apply"),
			TEXT("material_instance.asset.create"),
			TEXT("material_instance.parameters.apply"),
			TEXT("niagara.emitter.asset.create"),
			TEXT("niagara.system.asset.create"),
			TEXT("niagara.asset.duplicate"),
			TEXT("niagara.stack.reorder"),
			TEXT("niagara.stack.issue_fix"),
			TEXT("niagara.compound.apply"),
			TEXT("niagara.renderer_event_parameter.apply"),
			TEXT("niagara.compile_refresh_settle"),
		};
		return Values;
	}

	FUeAgentCanonicalValuePtr MakeString(
		const FString& Text,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(Text, Resources, Value, Error));
		return Value;
	}

	FUeAgentCanonicalSnapshotRef MakeSnapshot(
		FUeAgentResourceTracker& Resources,
		const TArray<FUeAgentCanonicalIdentity>& Identities)
	{
		const FUeAgentStateRevision Revision(
			TEXT("gc015:persisted"), TEXT("generation:gc015"), 15);
		TArray<FUeAgentStateEntry> Entries;
		for (const FUeAgentCanonicalIdentity& Identity : Identities)
		{
			Entries.Emplace(
				Identity,
				FUeAgentCanonicalType(
					TEXT("string"), EUeAgentValueKind::String, false, TEXT("scalar")),
				MakeString(TEXT("before"), Resources).ToSharedRef(),
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::Schema,
					TEXT("gc015:string-default"),
					TEXT("fixture:gc015"),
					MakeString(TEXT(""), Resources).ToSharedRef()),
				FUeAgentStateDetails(
					TEXT("Graph element"), TEXT(""), {TEXT("GC-015")},
					true, TEXT("available"), {}),
				Revision,
				EUeAgentAuthoredPresence::Present,
				EUeAgentOverrideState::Overridden);
		}
		FUeAgentCanonicalSnapshotPtr Snapshot;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalSnapshot::TryCreate(
			TEXT("snapshot:gc015"),
			EUeAgentStatePlane::Persisted,
			Revision,
			FUeAgentTemporalPoint(TEXT("fixture"), TEXT("15")),
			FUeAgentStateProvenance(
				TEXT("independent_readback"),
				TEXT("fixture:gc015"),
				TEXT("evidence:gc015:persisted")),
			Entries,
			Resources,
			Snapshot,
			Error));
		return Snapshot.ToSharedRef();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundBlueprintMaterialExactContractTest,
	"UeAgentInterface.Composition.GraphCompound.BlueprintMaterial.ExactContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundBlueprintMaterialExactContractTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const TArray<FGraphCompoundDomainRouteProfile>& Profiles =
		GetBlueprintMaterialGraphRouteProfiles();
	TestEqual(TEXT("exactly 19 routes are owned"), Profiles.Num(), 19);

	int32 QueryCount = 0;
	int32 TypedOperationCount = 0;
	int32 HjsonImportCount = 0;
	TSet<FString> Capabilities;
	TSet<FString> DescriptorIds;
	for (const FGraphCompoundDomainRouteProfile& Profile : Profiles)
	{
		Capabilities.Add(Profile.GetCapabilityId());
		DescriptorIds.Add(Profile.GetDescriptorId());
		switch (Profile.GetRouteKind())
		{
		case EGraphCompoundDomainRouteKind::Query: ++QueryCount; break;
		case EGraphCompoundDomainRouteKind::TypedOperation:
			++TypedOperationCount; break;
		case EGraphCompoundDomainRouteKind::HjsonImport:
			++HjsonImportCount; break;
		default: AddError(TEXT("invalid route kind")); break;
		}
		TestTrue(
			*FString::Printf(TEXT("%s execution contract is valid"),
				*Profile.GetCapabilityId()),
			Profile.GetExecutionContract().IsValid());
	}
	TestEqual(TEXT("two routes are C3Q"), QueryCount, 2);
	TestEqual(TEXT("fifteen routes are typed C3O"), TypedOperationCount, 15);
	TestEqual(TEXT("two routes compose the single C5 owner"), HjsonImportCount, 2);
	TestEqual(TEXT("capabilities are unique"), Capabilities.Num(), 19);
	TestEqual(TEXT("descriptor ids are unique"), DescriptorIds.Num(), 19);
	for (const FString& Capability : ExpectedCapabilities())
	{
		TestTrue(*FString::Printf(TEXT("%s is present"), *Capability),
			Capabilities.Contains(Capability));
	}

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString Error;
	TestTrue(
		TEXT("the immutable exact descriptor registry compiles"),
		TryCreateBlueprintMaterialGraphDescriptorRegistry(Registry, Error));
	TestTrue(TEXT("registry is valid"), Registry.IsValid());
	if (Registry.IsValid())
	{
		TestEqual(TEXT("registry owns 19 descriptors"),
			Registry->GetDescriptors().Num(), 19);
		TSet<FString> Hashes;
		for (const FUeAgentOperationDescriptor& Descriptor
			: Registry->GetDescriptors())
		{
			Hashes.Add(Descriptor.GetIdentity().GetDescriptorHash());
			TestTrue(TEXT("input schema closes additional properties"),
				Descriptor.GetInputSchema().GetJsonValue().Contains(
					TEXT("\"additionalProperties\":false")));
			const FUeAgentContractSchema* Schema =
				Registry->FindCompiledInputSchemaExact(Descriptor.GetIdentity());
			TestNotNull(TEXT("compiled input schema exists"), Schema);
			if (Schema)
			{
				TOptional<FUeAgentContractValue> Canonical;
				FUeAgentContractSchemaError SchemaError;
				TestFalse(
					TEXT("an unknown field fails closed"),
					Schema->TryValidateAndCanonicalize(
						FUeAgentContractValue(TEXT("{\"unexpected\":true}")),
						[](int64, const FString&, FUeAgentContractSchemaError&)
						{
							return true;
						},
						Canonical, SchemaError));
			}
		}
		TestEqual(TEXT("descriptor hashes are exact and unique"), Hashes.Num(), 19);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundBlueprintMaterialHjsonRouteTest,
	"UeAgentInterface.Composition.GraphCompound.BlueprintMaterial.HjsonRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundBlueprintMaterialHjsonRouteTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FUeAgentResourceTracker Resources = MakeTracker();
	const FGraphCompoundDomainRouteProfile* Blueprint =
		FindBlueprintMaterialGraphRouteProfile(TEXT("blueprint.authoring.apply"));
	TestNotNull(TEXT("blueprint HJSON route exists"), Blueprint);
	if (!Blueprint)
	{
		return false;
	}

	FGraphCompoundHjsonSourceRequest Request;
	FGraphCompoundRouteError Error;
	TestTrue(
		TEXT("directory source decodes into a C5 request"),
		TryDecodeGraphCompoundHjsonSourceRequest(
			*Blueprint,
			FUeAgentContractValue(TEXT(
				"{\"composition_kind\":\"ue.blueprint.asset\","
				"\"dry_run\":false,\"expected_revision\":\"revision:15\","
				"\"intent\":\"update\",\"source_kind\":\"directory\","
				"\"source_locator\":\"D:/workspace/BP\","
				"\"target_ref\":\"/Game/UAI/BP\"}")),
			Resources,
			Request,
			Error));
	TestEqual(TEXT("source kind is retained"), Request.GetSourceKind(),
		EGraphCompoundHjsonSourceKind::Directory);
	TestEqual(TEXT("target is retained"), Request.GetTargetRef(),
		FString(TEXT("/Game/UAI/BP")));

	FGraphCompoundHjsonSourceRequest WrongComposition;
	TestFalse(
		TEXT("a Blueprint route cannot silently import a Niagara composition"),
		TryDecodeGraphCompoundHjsonSourceRequest(
			*Blueprint,
			FUeAgentContractValue(TEXT(
				"{\"composition_kind\":\"ue.niagara_system.asset\","
				"\"dry_run\":false,\"expected_revision\":\"revision:15\","
				"\"intent\":\"update\",\"source_kind\":\"inline\","
				"\"inline_hjson\":\"schema: x\","
				"\"target_ref\":\"/Game/UAI/NS\"}")),
			Resources,
			WrongComposition,
			Error));
	TestTrue(TEXT("composition mismatch fails with a stable error"),
		Error.IsValid());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundBlueprintMaterialTypedMappingTest,
	"UeAgentInterface.Composition.GraphCompound.BlueprintMaterial.TypedMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundBlueprintMaterialTypedMappingTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FUeAgentResourceTracker Resources = MakeTracker();
	const FUeAgentCanonicalSnapshotRef Empty = MakeSnapshot(Resources, {});
	const FGraphCompoundDomainRouteProfile* Material =
		FindBlueprintMaterialGraphRouteProfile(TEXT("material.graph.apply"));
	TestNotNull(TEXT("material graph route exists"), Material);
	if (!Material)
	{
		return false;
	}

	FGraphCompoundCanonicalPayload Payload(TEXT("invalid"), {});
	FGraphCompoundRouteError Error;
	const FUeAgentContractValue Input(TEXT(
		"{\"edits\":[{\"depends_on\":[],\"element_kind\":\"property\","
		"\"identity\":\"property:roughness\",\"intent\":\"create\","
		"\"value\":{\"nested\":[1,true,\"x\"]}}],"
		"\"target_ref\":\"/Game/UAI/M_GC015\"}"));
	TestTrue(
		TEXT("typed graph fields decode to canonical edits"),
		TryDecodeGraphCompoundTypedOperation(
			*Material, Input, Empty, Resources, Payload, Error));
	TestEqual(TEXT("one identity edit is produced"), Payload.GetEdits().Num(), 1);
	if (Payload.GetEdits().Num() == 1)
	{
		TestEqual(TEXT("identity is retained"),
			Payload.GetEdits()[0].GetIdentity().GetSemanticId(),
			FString(TEXT("property:roughness")));
		TestEqual(TEXT("nested value remains typed canonical data"),
			Payload.GetEdits()[0].GetProposedAfter()->GetKind(),
			EUeAgentValueKind::Object);
	}

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	TestTrue(TEXT("registry builds for draft mapping"),
		TryCreateBlueprintMaterialGraphDescriptorRegistry(Registry, RegistryError));
	if (Registry.IsValid())
	{
		const FUeAgentOperationDescriptor* Descriptor =
			Registry->FindByDescriptorId(Material->GetDescriptorId());
		TestNotNull(TEXT("material descriptor exists"), Descriptor);
		if (Descriptor)
		{
			TOptional<FChangeSetDraft> Draft;
			TestTrue(
				TEXT("typed route builds the shared per-identity C6 draft"),
				TryBuildGraphCompoundTypedOperationDraft(
					*Material, Descriptor->GetIdentity(), Input, Empty,
					TEXT("request:gc015"), TEXT("idempotency:gc015"),
					Resources, Draft, Error));
			TestTrue(TEXT("draft exists"), Draft.IsSet());
			if (Draft.IsSet())
			{
				TestEqual(TEXT("one edit becomes one change"),
					Draft->GetChanges().Num(), 1);
				TestEqual(TEXT("whole public payload is not stored as the value"),
					Draft->GetChanges()[0].GetProposedAfter()->GetKind(),
					EUeAgentValueKind::Object);
			}
		}
	}

	FGraphCompoundEventQueryRequest Query;
	const FGraphCompoundDomainRouteProfile* QueryProfile =
		FindBlueprintMaterialGraphRouteProfile(
			TEXT("blueprint.component.events.discover"));
	TestNotNull(TEXT("query route exists"), QueryProfile);
	if (QueryProfile)
	{
		TestTrue(TEXT("event query decodes only its typed fields"),
			TryDecodeGraphCompoundEventQueryRequest(
				*QueryProfile,
				FUeAgentContractValue(TEXT(
					"{\"asset_path\":\"/Game/UAI/BP\"," 
					"\"compile_before_query\":false,"
					"\"component_name\":\"Mesh\","
					"\"include_hidden\":true,\"limit\":25,"
					"\"only_unbound\":true,\"query\":\"hit\"}")),
				Resources, Query, Error));
		TestEqual(TEXT("asset path is the exact target"),
			Query.GetTargetRef(), FString(TEXT("/Game/UAI/BP")));
		TestEqual(TEXT("component name becomes the exact owner ref"),
			Query.GetOwnerItemRef(), FString(TEXT("component:Mesh")));
		TestEqual(TEXT("query filter is preserved"),
			Query.GetQuery(), FString(TEXT("hit")));
		TestEqual(TEXT("limit is preserved"), Query.GetLimit(), 25);
		TestTrue(TEXT("only-unbound filter is preserved"),
			Query.IsOnlyUnbound());
		TestTrue(TEXT("hidden-event filter is preserved"),
			Query.IsIncludeHidden());
		TestFalse(TEXT("compile-before-query is preserved"),
			Query.IsCompileBeforeQuery());
		TestEqual(TEXT("query remains C3Q"),
			QueryProfile->GetExecutionContract().GetLane(),
			EGraphCompoundExecutionLane::Query);
		TestFalse(TEXT("query does not request native mutation"),
			QueryProfile->GetExecutionContract().RequiresNativeMutation());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundNiagaraTypedIdentityContractTest,
	"UeAgentInterface.Composition.GraphCompound.BlueprintMaterial."
		"NiagaraTypedIdentityContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundNiagaraTypedIdentityContractTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString EmitterHandleId =
		TEXT("11111111-1111-1111-1111-111111111111");
	const TMap<FString, FString> MissingEmitterInputs = {
		{TEXT("niagara.stack.reorder"),
			TEXT("{\"item_ref\":\"module:solve\",\"predecessor_ref\":\"\","
				"\"stage_ref\":\"particle_spawn\","
				"\"target_ref\":\"/Game/UAI/NS_GC015\"}")},
		{TEXT("niagara.stack.issue_fix"),
			TEXT("{\"fix_id\":\"fix:remove_duplicate\","
				"\"issue_id\":\"issue:duplicate\","
				"\"target_ref\":\"/Game/UAI/NS_GC015\"}")},
		{TEXT("niagara.compound.apply"),
			TEXT("{\"input_name\":\"Particles.Value\","
				"\"module_ref\":\"module:solve\","
				"\"stage_ref\":\"particle_spawn\","
				"\"target_ref\":\"/Game/UAI/NS_GC015\","
				"\"value\":{\"kind\":\"niagara_input\","
				"\"input_name\":\"Particles.Value\",\"type_id\":\"float\","
				"\"mode\":\"literal\",\"literal\":\"1.0\"}}")},
		{TEXT("niagara.renderer_event_parameter.apply"),
			TEXT("{\"parameter_name\":\"position\","
				"\"renderer_ref\":\"renderer:sprite\","
				"\"target_ref\":\"/Game/UAI/NS_GC015\","
				"\"type_ref\":\"Position\","
				"\"value\":\"Particles.Position\"}")},
	};
	for (const TPair<FString, FString>& Case : MissingEmitterInputs)
	{
		FUeAgentResourceTracker Resources = MakeTracker();
		const FUeAgentCanonicalSnapshotRef Empty = MakeSnapshot(Resources, {});
		const FGraphCompoundDomainRouteProfile* Profile =
			FindBlueprintMaterialGraphRouteProfile(Case.Key);
		TestNotNull(*FString::Printf(TEXT("%s profile exists"), *Case.Key),
			Profile);
		if (!Profile)
		{
			continue;
		}
		FGraphCompoundCanonicalPayload Payload(
			GetGraphCompoundCanonicalSchemaId(), {});
		FGraphCompoundRouteError Error;
		TestFalse(
			*FString::Printf(
				TEXT("%s rejects an emitter-ambiguous selector"), *Case.Key),
			TryDecodeGraphCompoundTypedOperation(
				*Profile, FUeAgentContractValue(Case.Value), Empty,
				Resources, Payload, Error));
		TestTrue(TEXT("missing emitter identity fails with a stable error"),
			Error.IsValid());
	}

	FUeAgentResourceTracker Resources = MakeTracker();
	const FUeAgentCanonicalSnapshotRef Empty = MakeSnapshot(Resources, {});
	const FGraphCompoundDomainRouteProfile* Renderer =
		FindBlueprintMaterialGraphRouteProfile(
			TEXT("niagara.renderer_event_parameter.apply"));
	TestNotNull(TEXT("renderer parameter route exists"), Renderer);
	if (!Renderer)
	{
		return false;
	}
	const FString InputPrefix = FString::Printf(
		TEXT("{\"emitter_handle_id\":\"%s\","
			"\"parameter_name\":\"position\","
			"\"renderer_ref\":\"renderer:sprite\","
			"\"target_ref\":\"/Game/UAI/NS_GC015\","
			"\"type_ref\":\"Position\",\"value\":"),
		*EmitterHandleId);
	FGraphCompoundCanonicalPayload First(GetGraphCompoundCanonicalSchemaId(), {});
	FGraphCompoundCanonicalPayload Second(GetGraphCompoundCanonicalSchemaId(), {});
	FGraphCompoundRouteError Error;
	TestTrue(TEXT("first renderer value decodes"),
		TryDecodeGraphCompoundTypedOperation(
			*Renderer,
			FUeAgentContractValue(InputPrefix + TEXT("\"Particles.Position\"}")),
			Empty, Resources, First, Error));
	TestTrue(TEXT("second renderer value decodes"),
		TryDecodeGraphCompoundTypedOperation(
			*Renderer,
			FUeAgentContractValue(
				InputPrefix + TEXT("\"Particles.Initial.Position\"}")),
			Empty, Resources, Second, Error));
	if (First.GetEdits().Num() == 1 && Second.GetEdits().Num() == 1)
	{
		const FString ExpectedIdentity =
			TEXT("binding:") + EmitterHandleId
			+ TEXT(":renderer:sprite:position:position");
		TestEqual(TEXT("renderer identity uses only stable selector fields"),
			First.GetEdits()[0].GetIdentity().GetSemanticId(), ExpectedIdentity);
		TestEqual(TEXT("changing the desired binding preserves identity"),
			Second.GetEdits()[0].GetIdentity().GetSemanticId(), ExpectedIdentity);
		TestNotEqual(TEXT("changing the desired binding changes only after value"),
			First.GetEdits()[0].GetProposedAfter()->GetText(),
			Second.GetEdits()[0].GetProposedAfter()->GetText());
	}
	else
	{
		AddError(TEXT("renderer identity contract did not produce one edit"));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundBlueprintMaterialAllTypedRoutesTest,
	"UeAgentInterface.Composition.GraphCompound.BlueprintMaterial.AllTypedRoutes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundBlueprintMaterialAllTypedRoutesTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const TMap<FString, FString> Inputs = {
		{TEXT("blueprint.asset.create"), TEXT("{\"asset_name\":\"BP_GC015\",\"package_path\":\"/Game/UAI\",\"parent_class_ref\":\"/Script/Engine.Actor\"}")},
		{TEXT("umg.asset.create"), TEXT("{\"asset_name\":\"WBP_GC015\",\"package_path\":\"/Game/UAI\"}")},
		{TEXT("material.asset.create"), TEXT("{\"asset_name\":\"M_GC015\",\"package_path\":\"/Game/UAI\"}")},
		{TEXT("material.graph.apply"), TEXT("{\"edits\":[{\"depends_on\":[],\"element_kind\":\"item\",\"identity\":\"expression:new\",\"intent\":\"create\",\"value\":{\"class_ref\":\"Constant3Vector\"}}],\"target_ref\":\"/Game/UAI/M_GC015\"}")},
		{TEXT("material.function.graph.apply"), TEXT("{\"edits\":[{\"depends_on\":[],\"element_kind\":\"item\",\"identity\":\"expression:new\",\"intent\":\"create\",\"value\":{\"class_ref\":\"FunctionInput\"}}],\"target_ref\":\"/Game/UAI/MF_GC015\"}")},
		{TEXT("material_instance.asset.create"), TEXT("{\"asset_name\":\"MI_GC015\",\"package_path\":\"/Game/UAI\",\"parent_material_ref\":\"/Game/UAI/M_Parent\"}")},
		{TEXT("material_instance.parameters.apply"), TEXT("{\"parameters\":[{\"association\":\"global\",\"index\":-1,\"name\":\"Tint\",\"type\":\"vector\",\"value\":[1,0.5,0,1]}],\"target_ref\":\"/Game/UAI/MI_GC015\"}")},
		{TEXT("niagara.emitter.asset.create"), TEXT("{\"asset_name\":\"NE_GC015\",\"package_path\":\"/Game/UAI\",\"source_ref\":\"/Niagara/DefaultEmitter\"}")},
		{TEXT("niagara.system.asset.create"), TEXT("{\"asset_name\":\"NS_GC015\",\"package_path\":\"/Game/UAI\",\"source_ref\":\"/Niagara/DefaultSystem\"}")},
		{TEXT("niagara.asset.duplicate"), TEXT("{\"source_ref\":\"/Game/UAI/NS_Source\",\"target_ref\":\"/Game/UAI/NS_Copy\"}")},
		{TEXT("niagara.stack.reorder"), TEXT("{\"emitter_handle_id\":\"11111111-1111-1111-1111-111111111111\",\"item_ref\":\"module:22222222-2222-2222-2222-222222222222\",\"predecessor_ref\":\"module:33333333-3333-3333-3333-333333333333\",\"stage_ref\":\"particle_spawn\",\"target_ref\":\"/Game/UAI/NS_GC015\"}")},
		{TEXT("niagara.stack.issue_fix"), TEXT("{\"emitter_handle_id\":\"11111111-1111-1111-1111-111111111111\",\"fix_id\":\"fix:add_dependency\",\"issue_id\":\"issue:missing_dependency\",\"target_ref\":\"/Game/UAI/NS_GC015\"}")},
		{TEXT("niagara.compound.apply"), TEXT("{\"emitter_handle_id\":\"11111111-1111-1111-1111-111111111111\",\"input_name\":\"Color\",\"module_ref\":\"module:22222222-2222-2222-2222-222222222222\",\"stage_ref\":\"particle_spawn\",\"target_ref\":\"/Game/UAI/NS_GC015\",\"value\":{\"mode\":\"linked\",\"value\":\"Particles.Color\"}}")},
		{TEXT("niagara.renderer_event_parameter.apply"), TEXT("{\"emitter_handle_id\":\"11111111-1111-1111-1111-111111111111\",\"parameter_name\":\"position\",\"renderer_ref\":\"renderer:sprite\",\"target_ref\":\"/Game/UAI/NS_GC015\",\"type_ref\":\"Position\",\"value\":\"Particles.Position\"}")},
		{TEXT("niagara.compile_refresh_settle"), TEXT("{\"target_ref\":\"/Game/UAI/NS_GC015\"}")},
	};
	TestEqual(TEXT("all fifteen typed routes have owned examples"), Inputs.Num(), 15);

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	TestTrue(TEXT("registry builds"),
		TryCreateBlueprintMaterialGraphDescriptorRegistry(Registry, RegistryError));
	if (!Registry.IsValid())
	{
		return false;
	}

	for (const TPair<FString, FString>& Case : Inputs)
	{
		FUeAgentResourceTracker Resources = MakeTracker();
		const FUeAgentCanonicalSnapshotRef Empty = MakeSnapshot(Resources, {});
		const FGraphCompoundDomainRouteProfile* Profile =
			FindBlueprintMaterialGraphRouteProfile(Case.Key);
		TestNotNull(*FString::Printf(TEXT("%s profile exists"), *Case.Key), Profile);
		if (!Profile)
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s is typed"), *Case.Key),
			Profile->GetRouteKind(),
			EGraphCompoundDomainRouteKind::TypedOperation);
		FGraphCompoundCanonicalPayload Payload(
			GetGraphCompoundCanonicalSchemaId(), {});
		FGraphCompoundRouteError Error;
		TestTrue(*FString::Printf(TEXT("%s decodes"), *Case.Key),
			TryDecodeGraphCompoundTypedOperation(
				*Profile, FUeAgentContractValue(Case.Value), Empty,
				Resources, Payload, Error));
		if (Payload.GetEdits().IsEmpty())
		{
			AddError(FString::Printf(TEXT("%s produced no identity edits"), *Case.Key));
			continue;
		}

		TArray<FUeAgentCanonicalIdentity> Existing;
		TSet<FString> ExistingKeys;
		for (const FGraphCompoundCanonicalEdit& Edit : Payload.GetEdits())
		{
			if (Edit.GetIntent() != EChangeIntent::Create
				&& !ExistingKeys.Contains(Edit.GetIdentity().GetCanonicalKey()))
			{
				Existing.Add(Edit.GetIdentity());
				ExistingKeys.Add(Edit.GetIdentity().GetCanonicalKey());
			}
			for (const FUeAgentCanonicalIdentity& Dependency
				: Edit.GetDependencies())
			{
				if (!ExistingKeys.Contains(Dependency.GetCanonicalKey()))
				{
					Existing.Add(Dependency);
					ExistingKeys.Add(Dependency.GetCanonicalKey());
				}
			}
		}
		const FUeAgentCanonicalSnapshotRef Before =
			MakeSnapshot(Resources, Existing);
		const FUeAgentOperationDescriptor* Descriptor =
			Registry->FindByDescriptorId(Profile->GetDescriptorId());
		TestNotNull(*FString::Printf(TEXT("%s descriptor exists"), *Case.Key),
			Descriptor);
		if (!Descriptor)
		{
			continue;
		}
		TOptional<FChangeSetDraft> Draft;
		TestTrue(*FString::Printf(TEXT("%s builds a C6 draft"), *Case.Key),
			TryBuildGraphCompoundTypedOperationDraft(
				*Profile, Descriptor->GetIdentity(),
				FUeAgentContractValue(Case.Value), Before,
				TEXT("request:gc015:all"),
				TEXT("idempotency:gc015:all"),
				Resources, Draft, Error));
		TestTrue(*FString::Printf(TEXT("%s draft is present"), *Case.Key),
			Draft.IsSet());
		if (Draft.IsSet())
		{
			TestEqual(
				*FString::Printf(TEXT("%s preserves per-identity cardinality"), *Case.Key),
				Draft->GetChanges().Num(), Payload.GetEdits().Num());
		}
	}
	return !HasAnyErrors();
}
