#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentReflectedPropertyRows.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	FUeAgentCanonicalValueRef StringValue(
		const FString& Text,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(Text, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef ObjectValue(
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryObject(Fields, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef ArrayValue(
		TArray<FUeAgentCanonicalValueRef> Items,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryArray(Items, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	const FUeAgentCanonicalValue* FindField(
		const FUeAgentCanonicalValue& Object,
		const FString& Name)
	{
		for (const auto& Field : Object.GetObjectItems())
		{
			if (Field.Key == Name)
			{
				return &Field.Value.Get();
			}
		}
		return nullptr;
	}

	FString ReadString(
		const FUeAgentCanonicalValue& Object,
		const FString& Name)
	{
		const FUeAgentCanonicalValue* Value = FindField(Object, Name);
		return Value != nullptr ? Value->GetText() : FString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentKeyedMapOperationRecordsTest,
	"UeAgentInterface.Core.HjsonWorkspace.KeyedItem.MapOperationRecords",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentKeyedMapOperationRecordsTest::RunTest(const FString&)
{
	auto Resources = MakeSemanticWorkspaceResourceTracker(); FWorkspaceError Error;
	auto Text = [&](const TCHAR* V) { return StringValue(V, Resources); };
	auto Row = [&](TArray<TPair<FString, FUeAgentCanonicalValueRef>> V) { return ObjectValue(MoveTemp(V), Resources); };
	{
		const auto& Modules = *GetKeyedItemProjectionContracts().FindByPredicate(
			[](const auto& C) { return C.RootField == TEXT("modules"); });
		const auto Direct = Row({{TEXT("module_kind"), Text(TEXT("assignment"))},
			{TEXT("assignment_targets"), Row({{TEXT("Particles.Lifetime"), Text(TEXT("float"))}})}});
		const auto Requested = Row({{TEXT("Assign"), Direct},
			{TEXT("Script"), Row({{TEXT("name"), Text(TEXT("Shape Location"))}})}});
		const auto Empty = Row({}); FUeAgentCanonicalValuePtr ResolvedCreation; bool CreationChanged = false;
		if (!TestTrue(TEXT("one collection accepts native arguments and candidate-backed creation"),
			TryResolveKeyedItemDesiredState(Requested.Get(), Empty.Get(), TEXT("modules"), Modules, nullptr,
				Resources, ResolvedCreation, CreationChanged, Error))) return false;
		const auto* Assign = FindField(*ResolvedCreation, TEXT("Assign"));
		const auto* Script = FindField(*ResolvedCreation, TEXT("Script"));
		TestEqual(TEXT("direct creation keeps normal operation derivation"), ReadString(*Assign, TEXT("operation")), FString(TEXT("create")));
		TestNull(TEXT("direct creation does not invent a candidate"), FindField(*Assign, TEXT("candidate")));
		TestNotNull(TEXT("direct creation preserves native arguments"), FindField(*Assign, TEXT("assignment_targets")));
		TestEqual(TEXT("candidate-backed creation keeps its selector"), ReadString(*FindField(*Script, TEXT("candidate")), TEXT("name")), FString(TEXT("Shape Location")));
		auto CandidateOnly = Modules; CandidateOnly.DirectCreationFields.Empty();
		TestFalse(TEXT("unregistered direct creation does not weaken other contracts"),
			TryResolveKeyedItemDesiredState(Requested.Get(), Empty.Get(), TEXT("modules"), CandidateOnly, nullptr,
				Resources, ResolvedCreation, CreationChanged, Error));
		TestEqual(TEXT("candidate-only contract retains diagnosis"), Error.GetCode(), FString(TEXT("keyed_item_candidate_missing")));
	}
	FKeyedItemProjectionContract Child; Child.RootField = TEXT("children"); Child.bIdentityFromMapKey = true;
	Child.VisibleFieldPolicy = {{TEXT("value")}};
	FKeyedItemProjectionContract Contract; Contract.RootField = TEXT("items"); Contract.bIdentityFromMapKey = true;
	Contract.bEmitOperationRecords = true; Contract.bRequiresCreationCandidate = false;
	Contract.VisibleFieldPolicy = {{TEXT("label"), TEXT("children")}}; Contract.CollectionFieldContracts.Add(TEXT("children"), &Child);
	const auto Captured = Row({
		{TEXT("Keep"), Row({{TEXT("unit_id"), Text(TEXT("existing-identity"))}, {TEXT("label"), Text(TEXT("before"))},
			{TEXT("children"), Row({{TEXT("Child"), Row({{TEXT("native_id"), Text(TEXT("child-identity"))}, {TEXT("value"), Text(TEXT("one"))}})}})}})},
		{TEXT("Remove"), Row({{TEXT("unit_id"), Text(TEXT("removed-identity"))}})}});
	FKeyedItemProjectionContract ArrayProjection = Contract;
	ArrayProjection.CapturedRecordIdentityField = TEXT("binding_ref");
	FUeAgentCanonicalValuePtr Reprojected;
	if (!TestTrue(TEXT("explicit projected-map stage preserves readable keys without reinterpreting array identity"),
		TryProjectKeyedItemVisibleValue(TEXT("items"), Captured.Get(), ArrayProjection, Resources, Reprojected, Error,
			EKeyedItemProjectionInputKind::ProjectedMap))) return false;
	TestEqual(TEXT("projected-map stage retains ordinary fields"), ReadString(*FindField(*Reprojected, TEXT("Keep")), TEXT("label")), FString(TEXT("before")));
	TestNull(TEXT("projected-map stage still filters machine fields"), FindField(*FindField(*Reprojected, TEXT("Keep")), TEXT("unit_id")));
	const auto* ProjectedChild = FindField(*FindField(*FindField(*Reprojected, TEXT("Keep")), TEXT("children")), TEXT("Child"));
	TestNull(TEXT("nested projected fields remain filtered"), FindField(*ProjectedChild, TEXT("native_id")));
	TestEqual(TEXT("nested readable values survive re-projection"), ReadString(*ProjectedChild, TEXT("value")), FString(TEXT("one")));
	TestFalse(TEXT("UMG shared binding contract does not enable local Sequence tags"), GetLevelSequenceBindingProjectionContract(false).VisibleFieldPolicy.Allows(TEXT("tags")));
	const auto Visible = Row({{TEXT("Keep"), Row({{TEXT("label"), Text(TEXT("after"))},
		{TEXT("children"), Row({{TEXT("Child"), Row({{TEXT("value"), Text(TEXT("two"))}})}})}})},
		{TEXT("Added"), Row({{TEXT("label"), Text(TEXT("new"))}})}});
	FUeAgentCanonicalValuePtr Resolved; bool Changed = false;
	if (!TestTrue(TEXT("map records resolve before common mutation instructions"), TryResolveKeyedItemDesiredState(
		Visible.Get(), Captured.Get(), TEXT("items"), Contract, nullptr, Resources, Resolved, Changed, Error))) return false;
	TestTrue(TEXT("new and omitted items constitute a change"), Changed);
	TestEqual(TEXT("existing update"), ReadString(*FindField(*Resolved, TEXT("Keep")), TEXT("operation")), FString(TEXT("update")));
	TestEqual(TEXT("omission means remove"), ReadString(*FindField(*Resolved, TEXT("Remove")), TEXT("operation")), FString(TEXT("remove")));
	TestEqual(TEXT("new fixed-type item means create"), ReadString(*FindField(*Resolved, TEXT("Added")), TEXT("operation")), FString(TEXT("create")));
	TestNull(TEXT("new item has no invented unit identity"), FindField(*FindField(*Resolved, TEXT("Added")), TEXT("unit_id")));
	const auto* Nested = FindField(*FindField(*FindField(*Resolved, TEXT("Keep")), TEXT("children")), TEXT("Child"));
	TestEqual(TEXT("nested native identity retained"), ReadString(*Nested, TEXT("native_id")), FString(TEXT("child-identity")));
	TestEqual(TEXT("nested value changed"), ReadString(*Nested, TEXT("value")), FString(TEXT("two")));
	TestNull(TEXT("nested complete desired records do not gain mutation instructions"), FindField(*Nested, TEXT("operation")));
	for (const TCHAR* Operation : {TEXT("create"), TEXT("update"), TEXT("remove")})
	{
		const auto Forged = Row({{TEXT("Keep"), Row({{TEXT("operation"), Text(Operation)}})}});
		TestFalse(TEXT("ordinary map input cannot supply machine operation"), TryResolveKeyedItemDesiredState(
			Forged.Get(), Captured.Get(), TEXT("items"), Contract, nullptr, Resources, Resolved, Changed, Error));
		TestEqual(TEXT("ordinary operation has machine-field error"), Error.GetCode(), FString(TEXT("keyed_item_machine_field_forbidden")));
		TestFalse(TEXT("rejected operation produces no resolved state"), Resolved.IsValid());
	}
	// The workspace resolver's non-map collection path produces these records;
	// composition must explicitly identify that input, never infer it from a field.
	FKeyedItemProjectionContract WorkspaceContract;
	WorkspaceContract.bRequiresCreationCandidate = false;
	WorkspaceContract.VisibleFieldPolicy = {{TEXT("label")}};
	const auto SimpleCapture = Row({{TEXT("Keep"), Row({{TEXT("label"), Text(TEXT("before"))}})},
		{TEXT("Remove"), Row({{TEXT("label"), Text(TEXT("gone"))}})}});
	const auto SimpleVisible = Row({{TEXT("Keep"), Row({{TEXT("label"), Text(TEXT("after"))}})}});
	FUeAgentCanonicalValuePtr WorkspaceRecords;
	if (!TestTrue(TEXT("workspace owner derives operation records"), TryResolveKeyedItemDesiredState(
		SimpleVisible.Get(), SimpleCapture.Get(), TEXT("items"), WorkspaceContract, nullptr,
		Resources, WorkspaceRecords, Changed, Error))) return false;
	FKeyedItemProjectionContract DocumentContract;
	DocumentContract.bIdentityFromMapKey = true; DocumentContract.VisibleFieldPolicy = {{TEXT("label")}};
	if (!TestTrue(TEXT("explicit internal stage consumes derived operation records"), TryResolveKeyedItemDesiredState(
		*WorkspaceRecords, SimpleCapture.Get(), TEXT("items"), DocumentContract, nullptr,
		Resources, Resolved, Changed, Error, nullptr, EKeyedItemInputKind::ResolvedOperationRecords))) return false;
	TestNull(TEXT("internal removal stays removed after composition"), FindField(*Resolved, TEXT("Remove")));
	TestEqual(TEXT("internal update retains desired business value"), ReadString(*FindField(*Resolved, TEXT("Keep")), TEXT("label")), FString(TEXT("after")));
	WorkspaceContract.bRetainUnchangedOperationRecords = true;
	if (!TestTrue(TEXT("set reconstruction retains unchanged native context"), TryResolveKeyedItemDesiredState(
		SimpleCapture.Get(), SimpleCapture.Get(), TEXT("items"), WorkspaceContract, nullptr,
		Resources, WorkspaceRecords, Changed, Error))) return false;
	TestFalse(TEXT("retained context is not an authoring change"), Changed);
	TestEqual(TEXT("complete operation set keeps both captured rows"), WorkspaceRecords->GetObjectItems().Num(), 2);
	TestEqual(TEXT("unchanged captured item retains update binding"), ReadString(*FindField(*WorkspaceRecords, TEXT("Remove")), TEXT("operation")), FString(TEXT("update")));
	for (const TCHAR* Root : {TEXT("tree_items"), TEXT("blackboard_keys"), TEXT("smart_object_items")})
	{
		FKeyedItemProjectionContract FullSet;
		TestTrue(TEXT("registered reconstruction contract exists"), TryGetKeyedItemProjectionContract(Root, FullSet));
		TestTrue(TEXT("registered reconstruction consumer requires complete context"), FullSet.bRetainUnchangedOperationRecords);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentGraphDeclarationsTest,
	"UeAgentInterface.Core.HjsonWorkspace.KeyedItem.GraphDeclarations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentGraphDeclarationsTest::RunTest(const FString&)
{
	auto Resources = MakeSemanticWorkspaceResourceTracker(); FWorkspaceError Error;
	FKeyedItemProjectionContract Contract;
	if (!TestTrue(TEXT("graph declaration contract registered"), TryGetKeyedItemProjectionContract(TEXT("graph_declarations"), Contract))) return false;
	auto Text = [&](const TCHAR* V) { return StringValue(V, Resources); };
	auto Row = [&](TArray<TPair<FString, FUeAgentCanonicalValueRef>> V) { return ObjectValue(MoveTemp(V), Resources); };
	const auto Captured = Row({{TEXT("Existing"), Row({{TEXT("name"), Text(TEXT("Existing"))},
		{TEXT("graph_kind"), Text(TEXT("function_graph"))}, {TEXT("graph_guid"), Text(TEXT("native-guid"))}})}});
	FUeAgentCanonicalValuePtr Visible, Resolved; bool Changed = false;
	if (!TryProjectKeyedItemVisibleValue(TEXT("declarations"), Captured.Get(), Contract, Resources, Visible, Error)) { AddError(TEXT("project: ") + Error.GetCode()); return false; }
	TestNull(TEXT("declaration hides native GUID"), FindField(*FindField(*Visible, TEXT("Existing")), TEXT("graph_guid")));
	TestNull(TEXT("unchanged business name is omitted"), FindField(*FindField(*Visible, TEXT("Existing")), TEXT("name")));
	if (!TryResolveKeyedItemDesiredState(*Visible, Captured.Get(), TEXT("declarations"), Contract, nullptr, Resources, Resolved, Changed, Error)) { AddError(TEXT("roundtrip: ") + Error.GetCode()); return false; }
	TestFalse(TEXT("visible declaration roundtrip is unchanged"), Changed);
	const auto Renamed = Row({{TEXT("Existing"), Row({{TEXT("name"), Text(TEXT("Renamed"))},
		{TEXT("graph_kind"), Text(TEXT("function_graph"))}})}});
	if (!TestTrue(TEXT("optional business name resolves against existing key"), TryResolveKeyedItemDesiredState(
		Renamed.Get(), Captured.Get(), TEXT("declarations"), Contract, nullptr, Resources, Resolved, Changed, Error))) return false;
	TestTrue(TEXT("rename is an update of the frozen identity"), Changed
		&& ReadString(*FindField(*Resolved, TEXT("Existing")), TEXT("graph_guid")) == TEXT("native-guid")
		&& ReadString(*FindField(*Resolved, TEXT("Existing")), TEXT("operation")) == TEXT("update")
		&& ReadString(*FindField(*Resolved, TEXT("Existing")), TEXT("name")) == TEXT("Renamed"));
	const auto Added = Row({{TEXT("Existing"), Row({{TEXT("graph_kind"), Text(TEXT("function_graph"))}})},
		{TEXT("Added"), Row({{TEXT("graph_kind"), Text(TEXT("function_graph"))}})}});
	if (!TryResolveKeyedItemDesiredState(Added.Get(), Captured.Get(), TEXT("declarations"), Contract, nullptr, Resources, Resolved, Changed, Error)) { AddError(TEXT("addition: ") + Error.GetCode()); return false; }
	TestTrue(TEXT("new declaration produces common create intent"), Changed && ReadString(*FindField(*Resolved, TEXT("Added")), TEXT("operation")) == TEXT("create"));
	TestNull(TEXT("new declaration has no invented GUID"), FindField(*FindField(*Resolved, TEXT("Added")), TEXT("graph_guid")));
	TestTrue(TEXT("omitting existing graph resolves removal"), TryResolveKeyedItemDesiredState(Row({}).Get(), Captured.Get(), TEXT("declarations"), Contract, nullptr, Resources, Resolved, Changed, Error));
	TestEqual(TEXT("removal preserves captured identity"), ReadString(*FindField(*Resolved, TEXT("Existing")), TEXT("graph_guid")), FString(TEXT("native-guid")));
	TestEqual(TEXT("omission emits remove"), ReadString(*FindField(*Resolved, TEXT("Existing")), TEXT("operation")), FString(TEXT("remove")));
	TestTrue(TEXT("map-key replacement resolves remove plus create"), TryResolveKeyedItemDesiredState(
		Row({{TEXT("Replacement"), Row({{TEXT("graph_kind"), Text(TEXT("function_graph"))}})}}).Get(),
		Captured.Get(), TEXT("declarations"), Contract, nullptr, Resources, Resolved, Changed, Error));
	TestEqual(TEXT("old key removed"), ReadString(*FindField(*Resolved, TEXT("Existing")), TEXT("operation")), FString(TEXT("remove")));
	TestEqual(TEXT("new key created"), ReadString(*FindField(*Resolved, TEXT("Replacement")), TEXT("operation")), FString(TEXT("create")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentKeyedItemReferencesTest,
	"UeAgentInterface.Core.HjsonWorkspace.KeyedItem.References",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentKeyedItemReferencesTest::RunTest(const FString& Parameters)
{
	auto Resources = MakeSemanticWorkspaceResourceTracker();
	FWorkspaceError Error;
	FKeyedItemProjectionContract Contract;
	Contract.RootField = TEXT("items");
	Contract.VisibleFieldPolicy.VisibleTopLevelFields = {TEXT("label"), TEXT("parent"), TEXT("target")};
	Contract.ReferenceIdentityField = TEXT("id");
	Contract.ReferenceFields = {{TEXT("parent"), {}}, {TEXT("target"), {}}};
	auto Text = [&](const TCHAR* Value) { return StringValue(Value, Resources); };
	auto Row = [&](TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields) { return ObjectValue(MoveTemp(Fields), Resources); };
	FString First, Second;
	if (!TryFormatReadableKeyedItemAlias(TEXT("State"), 0, First, Error)
		|| !TryFormatReadableKeyedItemAlias(TEXT("State"), 1, Second, Error)) return false;
	const auto Captured = Row({
		{First, Row({{TEXT("id"), Text(TEXT("first"))}, {TEXT("label"), Text(TEXT("State"))}})},
		{Second, Row({{TEXT("id"), Text(TEXT("second"))}, {TEXT("label"), Text(TEXT("State"))}, {TEXT("parent"), Text(TEXT("first"))}})},
		{TEXT("Transition"), Row({{TEXT("id"), Text(TEXT("transition"))}, {TEXT("target"), Text(TEXT("second"))}})}});
	FUeAgentCanonicalValuePtr Visible, Resolved;
	bool bChanged = false;
	if (!TestTrue(TEXT("references project through the same repeated-name keys"),
		TryProjectKeyedItemVisibleValue(TEXT("tree"), Captured.Get(), Contract, Resources, Visible, Error))) return false;
	TestEqual(TEXT("parent targets first same-name item"), ReadString(*FindField(*Visible, Second), TEXT("parent")), First);
	TestEqual(TEXT("transition targets second same-name item"), ReadString(*FindField(*Visible, TEXT("Transition")), TEXT("target")), Second);
	if (!TestTrue(TEXT("unchanged readable references resolve"), TryResolveKeyedItemDesiredState(*Visible,
		Captured.Get(), TEXT("tree"), Contract, nullptr, Resources, Resolved, bChanged, Error))) return false;
	TestFalse(TEXT("alias projection does not manufacture an edit"), bChanged);
	TestEqual(TEXT("unchanged references emit no native updates"), Resolved->GetObjectItems().Num(), 0);

	const auto NewInput = Row({{First, Row({})}, {Second, Row({})},
		{TEXT("Transition"), Row({{TEXT("target"), Text(TEXT("NewState"))}})},
		{TEXT("NewState"), Row({{TEXT("name"), Text(TEXT("Native Candidate"))}, {TEXT("parent"), StringValue(Second, Resources)}})}});
	if (!TestTrue(TEXT("new sibling references resolve independently of row order"), TryResolveKeyedItemDesiredState(NewInput.Get(),
		Captured.Get(), TEXT("tree"), Contract, nullptr, Resources, Resolved, bChanged, Error))) return false;
	FString NewKey;
	TestTrue(TEXT("new target remains an explicit allocation-time reference"), TryGetNewKeyedItemReference(
		*FindField(*FindField(*Resolved, TEXT("Transition")), TEXT("target")), NewKey));
	TestEqual(TEXT("new target key"), NewKey, FString(TEXT("NewState")));
	TestEqual(TEXT("new child resolves existing parent identity"), ReadString(*FindField(*Resolved, TEXT("NewState")), TEXT("parent")), FString(TEXT("second")));
	TestTrue(TEXT("omitted existing parent produces no redundant update"), FindField(*Resolved, Second) == nullptr);

	const auto DeletedTarget = Row({{First, Row({})}, {TEXT("Transition"), Row({{TEXT("label"), Text(TEXT("Edited transition"))}})}});
	if (!TestTrue(TEXT("captured reference reaches native removal handling"), TryResolveKeyedItemDesiredState(DeletedTarget.Get(),
		Captured.Get(), TEXT("tree"), Contract, nullptr, Resources, Resolved, bChanged, Error))) return false;
	TestEqual(TEXT("captured target identity is preserved for native handling"), ReadString(*FindField(*Resolved, TEXT("Transition")), TEXT("target")), FString(TEXT("second")));
	const auto RawIdentity = Row({{First, Row({})}, {Second, Row({})},
		{TEXT("Transition"), Row({{TEXT("target"), Text(TEXT("second"))}})}});
	TestFalse(TEXT("machine identity cannot substitute for a readable key"), TryResolveKeyedItemDesiredState(RawIdentity.Get(),
		Captured.Get(), TEXT("tree"), Contract, nullptr, Resources, Resolved, bChanged, Error));
	Contract.ReferenceFields.Reset();
	if (!TestTrue(TEXT("unselected consumers retain their existing field semantics"),
		TryProjectKeyedItemVisibleValue(TEXT("other"), Captured.Get(), Contract, Resources, Visible, Error))) return false;
	TestEqual(TEXT("unselected parent is not remapped"), ReadString(*FindField(*Visible, Second), TEXT("parent")), FString(TEXT("first")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentKeyedCrossCollectionReferencesTest,
	"UeAgentInterface.Core.HjsonWorkspace.KeyedItem.CrossCollectionReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentKeyedCrossCollectionReferencesTest::RunTest(const FString&)
{
	auto Resources = MakeSemanticWorkspaceResourceTracker(); FWorkspaceError Error;
	auto Text = [&](const TCHAR* V) { return StringValue(V, Resources); };
	auto Row = [&](TArray<TPair<FString, FUeAgentCanonicalValueRef>> V) { return ObjectValue(MoveTemp(V), Resources); };
	auto Set = [&](TArray<FUeAgentCanonicalValueRef> V) { return ArrayValue(MoveTemp(V), Resources); };
	FKeyedItemProjectionContract Source, Target;
	Source.RootField = TEXT("owners"); Source.ReferenceIdentityField = TEXT("id");
	Source.VisibleFieldPolicy.VisibleTopLevelFields = {TEXT("one"), TEXT("many")};
	Source.ReferenceFields = {{TEXT("one"), {TEXT("targets"), EKeyedItemReferenceShape::Single}},
		{TEXT("many"), {TEXT("targets"), EKeyedItemReferenceShape::Set}}};
	Target.RootField = TEXT("targets"); Target.ReferenceIdentityField = TEXT("id");
	const auto CapturedTargets = Row({{TEXT("Target"), Row({{TEXT("id"), Text(TEXT("native-target"))}})}});
	const auto CapturedSource = Row({{TEXT("Owner"), Row({{TEXT("id"), Text(TEXT("native-owner"))},
		{TEXT("one"), Text(TEXT("native-target"))}, {TEXT("many"), Set({Text(TEXT("native-target"))})}})}});
	FKeyedItemReferenceContext Context;
	Context.Add(TEXT("targets"), {&Target, &CapturedTargets.Get(), nullptr, false});
	FUeAgentCanonicalValuePtr Visible, Resolved; bool Changed = false;
	if (!TestTrue(TEXT("cross collection single and set export"), TryProjectKeyedItemVisibleValue(TEXT("owners"), CapturedSource.Get(),
		Source, Resources, Visible, Error, EKeyedItemProjectionInputKind::CapturedItems, &Context))) return false;
	TestEqual(TEXT("single uses target readable key"), ReadString(*FindField(*Visible, TEXT("Owner")), TEXT("one")), FString(TEXT("Target")));
	const auto DesiredTargets = Row({{TEXT("Target"), Row({})}, {TEXT("New"), Row({})}});
	Context.FindChecked(TEXT("targets")).DesiredItems = &DesiredTargets.Get();
	const auto Desired = Row({{TEXT("Owner"), Row({{TEXT("one"), Text(TEXT("New"))}})}});
	if (!TestTrue(TEXT("new cross target and omitted set resolve"), TryResolveKeyedItemDesiredState(Desired.Get(), CapturedSource.Get(), TEXT("owners"),
		Source, nullptr, Resources, Resolved, Changed, Error, nullptr, EKeyedItemInputKind::VisibleDesiredState, &Context))) return false;
	FString NewKey;
	TestTrue(TEXT("cross new target remains allocation reference"), TryGetNewKeyedItemReference(*FindField(*FindField(*Resolved, TEXT("Owner")), TEXT("one")), NewKey) && NewKey == TEXT("New"));
	TestEqual(TEXT("omitted set restores captured native member"), FindField(*FindField(*Resolved, TEXT("Owner")), TEXT("many"))->GetArrayItems()[0]->GetText(), FString(TEXT("native-target")));
	const auto EmptyTargets = Row({}); Context.FindChecked(TEXT("targets")).DesiredItems = &EmptyTargets.Get();
	const auto Unchanged = Row({{TEXT("Owner"), Row({})}});
	TestTrue(TEXT("captured cross references await native relation handling"), TryResolveKeyedItemDesiredState(Unchanged.Get(), CapturedSource.Get(), TEXT("owners"),
		Source, nullptr, Resources, Resolved, Changed, Error, nullptr, EKeyedItemInputKind::VisibleDesiredState, &Context));
	for (const auto& Invalid : {
		Row({{TEXT("Owner"), Row({{TEXT("many"), Set({Text(TEXT("NeverExisted"))})}})}}),
		Row({{TEXT("Owner"), Row({})}, {TEXT("NewOwner"), Row({{TEXT("many"), Set({Text(TEXT("Target"))})}})}})})
		TestFalse(TEXT("new or rewritten dangling containment rejected"), TryResolveKeyedItemDesiredState(Invalid.Get(), CapturedSource.Get(), TEXT("owners"),
			Source, nullptr, Resources, Resolved, Changed, Error, nullptr, EKeyedItemInputKind::VisibleDesiredState, &Context));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentOrderedOwnedItemBindingTest,
	"UeAgentInterface.Core.HjsonWorkspace.KeyedItem.OrderedOwnedBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentOrderedOwnedItemBindingTest::RunTest(const FString& Parameters)
{
	auto Resources = MakeSemanticWorkspaceResourceTracker();
	FWorkspaceError Error;
	auto Definition = [&](const FString& Path) {
		return ObjectValue({{TEXT("builtin"), StringValue(TEXT("owned_object"), Resources)},
			{TEXT("instance_name"), StringValue(TEXT("Rule"), Resources)},
			{TEXT("owner_relative_path"), StringValue(Path, Resources)}}, Resources);
	};
	const auto NullDefinition = ObjectValue({{TEXT("builtin"), StringValue(TEXT("owned_object"), Resources)}}, Resources);
	const auto Definitions = ArrayValue({Definition(TEXT("OuterA.RandomName")), NullDefinition,
		Definition(TEXT("OuterB.RandomName"))}, Resources);
	FUeAgentCanonicalValuePtr Null;
	FUeAgentStateSemanticError ValueError;
	if (!FUeAgentCanonicalValue::TryNull(Resources, Null, ValueError)) return false;
	const auto NullItem = ObjectValue({{TEXT("path"), Null.ToSharedRef()}}, Resources);
	auto Item = [&](const FString& Key) {
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
		if (!Key.IsEmpty()) Fields.Emplace(TEXT("instance"), StringValue(Key, Resources));
		else Fields.Emplace(TEXT("name"), StringValue(TEXT("Rule"), Resources));
		return ObjectValue({{TEXT("owned_object"), ObjectValue(Fields, Resources)}}, Resources);
	};
	TArray<FOrderedOwnedItemBinding> Bound;
	if (!TestTrue(TEXT("nullable ordered input resolves by semantic instance key"),
		TryResolveOrderedOwnedItems(ArrayValue({Item(TEXT("Rule#2")), NullItem, Item(TEXT("Rule")), Item(FString())}, Resources).Get(),
			Definitions.Get(), Resources, Bound, Error))) return false;
	TestEqual(TEXT("array order is retained"), Bound.Num(), 4);
	TestEqual(TEXT("second same-class instance keeps its original index"), Bound[0].CapturedIndex, 2);
	TestEqual(TEXT("same native names in different owners remain distinct"), Bound[0].RelativePath, FString(TEXT("OuterB.RandomName")));
	TestEqual(TEXT("the first instance is not selected by new index"), Bound[2].CapturedIndex, 0);
	TestEqual(TEXT("null has no captured object identity"), Bound[1].CapturedIndex, INDEX_NONE);
	TestEqual(TEXT("new owned input remains distinct from existing instances"), Bound[3].CapturedIndex, INDEX_NONE);
	TestFalse(TEXT("unknown instance keys are rejected"), TryResolveOrderedOwnedItems(
		ArrayValue({Item(TEXT("Rule#3"))}, Resources).Get(), Definitions.Get(), Resources, Bound, Error));
	TestEqual(TEXT("unknown keys have a bounded public error"), Error.GetCode(), FString(TEXT("owned_item_key_unknown")));
	for (bool bExisting : {false, true})
	{
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Legacy{{TEXT("class_path"), StringValue(TEXT("/Script/Test.Rule"), Resources)}};
		if (bExisting) Legacy.Emplace(TEXT("instance"), StringValue(TEXT("Rule"), Resources));
		TestFalse(TEXT("ordinary class paths cannot select either new or existing objects"), TryResolveOrderedOwnedItems(
			ArrayValue({ObjectValue({{TEXT("owned_object"), ObjectValue(Legacy, Resources)}}, Resources)}, Resources).Get(),
			Definitions.Get(), Resources, Bound, Error));
		TestEqual(TEXT("legacy class selection is rejected by the ordinary boundary"), Error.GetCode(), FString(TEXT("owned_item_visible_field_forbidden")));
	}
	TestFalse(TEXT("an existing instance cannot reselect its class by name"), TryResolveOrderedOwnedItems(
		ArrayValue({ObjectValue({{TEXT("owned_object"), ObjectValue({{TEXT("instance"), StringValue(TEXT("Rule"), Resources)},
			{TEXT("name"), StringValue(TEXT("Other Rule"), Resources)}}, Resources)}}, Resources)}, Resources).Get(),
		Definitions.Get(), Resources, Bound, Error));
	TestEqual(TEXT("existing selector has an explicit error"), Error.GetCode(), FString(TEXT("owned_item_existing_selector_forbidden")));
	TestFalse(TEXT("ordinary new owned properties cannot use the internal untyped descriptor list"), TryResolveOrderedOwnedItems(
		ArrayValue({ObjectValue({{TEXT("owned_object"), ObjectValue({{TEXT("name"), StringValue(TEXT("Rule"), Resources)},
			{TEXT("properties"), ArrayValue({}, Resources)}}, Resources)}}, Resources)}, Resources).Get(),
		Definitions.Get(), Resources, Bound, Error));
	TestEqual(TEXT("legacy descriptor lists are rejected before native preparation"), Error.GetCode(), FString(TEXT("owned_item_properties_invalid")));

	auto TextDefinition = MakeShared<FJsonObject>(); TextDefinition->SetStringField(TEXT("builtin"), TEXT("text"));
	auto Fields = MakeShared<FJsonObject>(); Fields->SetObjectField(TEXT("value"), TextDefinition);
	auto Owned = MakeShared<FJsonObject>();
	Owned->SetStringField(TEXT("builtin"), TEXT("owned_object"));
	Owned->SetStringField(TEXT("owner_relative_path"), TEXT("Hidden.Owner"));
	Owned->SetStringField(TEXT("instance_name"), TEXT("Rule"));
	Owned->SetObjectField(TEXT("fields"), Fields);
	const auto Public = ProjectPublicOwnedItemDefinition(MakeShared<FJsonValueObject>(Owned));
	if (!TestTrue(TEXT("public projection retains nested value contracts"), Public.IsValid())) return false;
	TestFalse(TEXT("public definitions do not expose owner binding"), Public->AsObject()->HasField(TEXT("owner_relative_path")));
	TestFalse(TEXT("public definitions do not expose binding naming metadata"), Public->AsObject()->HasField(TEXT("instance_name")));
	TestEqual(TEXT("nested text definition remains exact"), Public->AsObject()->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("value"))->GetStringField(TEXT("builtin")), FString(TEXT("text")));
	auto EmptyDefinitions = MakeShared<FJsonObject>();
	EmptyDefinitions->SetArrayField(TEXT("items"), {MakeShared<FJsonValueNull>(), MakeShared<FJsonValueNull>()});
	TestFalse(TEXT("null definition placeholders do not create a public contract"),
		ProjectPublicOwnedItemDefinition(MakeShared<FJsonValueObject>(EmptyDefinitions)).IsValid());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentOrderedStructuredItemBindingTest,
	"UeAgentInterface.Core.HjsonWorkspace.KeyedItem.OrderedStructuredBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentOrderedStructuredItemBindingTest::RunTest(const FString&)
{
	auto Resources = MakeSemanticWorkspaceResourceTracker(); FWorkspaceError Error;
	auto Text = [&](const TCHAR* Value) { return StringValue(Value, Resources); };
	auto Fields = [&](const TCHAR* Business, const TCHAR* NestedAlias) {
		return ObjectValue({{TEXT("business"), Text(Business)}, {TEXT("nested"), Text(NestedAlias)}}, Resources);
	};
	auto Item = [&](const TCHAR* Instance, const TCHAR* Business) {
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Members{{TEXT("fields"), Fields(Business, TEXT("SameAlias"))}};
		if (Instance && *Instance) Members.Emplace(TEXT("instance"), Text(Instance));
		return ObjectValue(MoveTemp(Members), Resources);
	};
	const auto Definitions = ArrayValue({ObjectValue({}, Resources), ObjectValue({}, Resources), ObjectValue({}, Resources)}, Resources);
	TArray<FOrderedStructuredItemBinding> Bound;
	if (!TestTrue(TEXT("structured items resolve exact local keys while retaining target order"),
		TryResolveOrderedStructuredItems(ArrayValue({Item(TEXT("Entry2"), TEXT("edited-b")),
			Item(TEXT("Entry1"), TEXT("edited-a")), Item(TEXT(""), TEXT("new"))}, Resources).Get(),
			Definitions.Get(), Resources, Bound, Error))) return false;
	TestEqual(TEXT("reordered second capture remains second identity"), Bound[0].CapturedIndex, 1);
	TestEqual(TEXT("reordered first capture remains first identity"), Bound[1].CapturedIndex, 0);
	TestEqual(TEXT("new complete item has no captured identity"), Bound[2].CapturedIndex, INDEX_NONE);
	TestEqual(TEXT("business edits do not participate in identity"),
		ReadString(*FindField(*Bound[0].Value, TEXT("fields")), TEXT("business")), FString(TEXT("edited-b")));
	TestEqual(TEXT("identical nested aliases are scoped below each structured item"),
		ReadString(*FindField(*Bound[0].Value, TEXT("fields")), TEXT("nested")), FString(TEXT("SameAlias")));
	TestTrue(TEXT("omitting Entry3 removes it without rebinding survivors"), Bound.Num() == 3
		&& !Bound.ContainsByPredicate([](const auto& ItemBinding) { return ItemBinding.CapturedIndex == 2; }));
	TestNull(TEXT("public instance metadata is stripped before recursive overlay"), FindField(*Bound[0].Value, TEXT("instance")));
	TestFalse(TEXT("unknown structured key rejects"), TryResolveOrderedStructuredItems(
		ArrayValue({Item(TEXT("Entry4"), TEXT("x"))}, Resources).Get(), Definitions.Get(), Resources, Bound, Error));
	TestEqual(TEXT("unknown structured key error"), Error.GetCode(), FString(TEXT("structured_item_key_unknown")));
	TestFalse(TEXT("duplicate structured key rejects"), TryResolveOrderedStructuredItems(
		ArrayValue({Item(TEXT("Entry1"), TEXT("x")), Item(TEXT("Entry1"), TEXT("y"))}, Resources).Get(),
		Definitions.Get(), Resources, Bound, Error));
	TestEqual(TEXT("duplicate structured key error"), Error.GetCode(), FString(TEXT("structured_item_key_duplicate")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentReflectedPropertyRowsTest,
	"UeAgentInterface.Core.HjsonWorkspace.ReflectedPropertyRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentReflectedPropertyRowsTest::RunTest(const FString& Parameters)
{
	auto Resources = MakeSemanticWorkspaceResourceTracker();
	FUeAgentStateSemanticError Error;
	TArray<FResolvedReadableProperty> Rows;
	const TArray<FReadablePropertySchema> Schema{{TEXT("Count"), TEXT("Limits"), TEXT("integer")},
		{TEXT("Operator"), TEXT("Condition"), TEXT("EMovieSceneGroupConditionOperator")}};
	auto Proposed = [&](const FString& Type, const FString& Category) {
		return ObjectValue({{Category, ObjectValue({{TEXT("Operator"), ObjectValue({
			{TEXT("type"), StringValue(Type, Resources)}, {TEXT("value"), StringValue(TEXT("Xor"), Resources)}}, Resources)}}, Resources)}}, Resources);
	};
	TestTrue(TEXT("a sparse categorized row binds the schema by field and category rather than position"),
		TryReadReadablePropertyRows(Proposed(Schema[1].Type, TEXT("Condition")).Get(), Schema, Rows, Error));
	if (!TestEqual(TEXT("omitted rows are not synthesized"), Rows.Num(), 1)) return false;
	TestEqual(TEXT("sparse row retains exact schema correspondence"), Rows[0].SchemaIndex, 1);
	TestEqual(TEXT("enum name remains an ordinary value for native codec"), Rows[0].Value->GetText(), FString(TEXT("Xor")));
	TestFalse(TEXT("declared type cannot replace captured schema"), TryReadReadablePropertyRows(Proposed(TEXT("string"), TEXT("Condition")).Get(), Schema, Rows, Error));
	TestEqual(TEXT("wrong type has the common property-row error"), Error.Code, FString(TEXT("semantic_workspace_property_type_mismatch")));
	TestFalse(TEXT("an unknown category cannot redirect a property"), TryReadReadablePropertyRows(Proposed(Schema[1].Type, TEXT("Other")).Get(), Schema, Rows, Error));
	TestFalse(TEXT("missing schema cannot infer an owned property type"), TryReadReadablePropertyRows(Proposed(Schema[1].Type, TEXT("Condition")).Get(), {}, Rows, Error));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentReadableKeyedItemAliasTest,
	"UeAgentInterface.Core.HjsonWorkspace.KeyedItem.ReadableAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentReadableKeyedItemAliasTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::HjsonWorkspace;
	FWorkspaceError Error;
	FString Alias;
	TestTrue(TEXT("first item keeps its semantic name"),
		TryFormatReadableKeyedItemAlias(TEXT("Player Start"), 0, Alias, Error));
	TestEqual(TEXT("first readable key is unsuffixed"), Alias,
		FString(TEXT("Player Start")));
	TestTrue(TEXT("same-name item formats an occurrence"),
		TryFormatReadableKeyedItemAlias(TEXT("Player Start"), 1, Alias, Error));
	TestEqual(TEXT("occurrence suffix is continuous"), Alias,
		FString(TEXT("Player Start#2")));
	TMap<FString, int32> Occurrences;
	TSet<FString> Used;
	const TSet<FString> Reserved{TEXT("x"), TEXT("x#2")};
	TestTrue(TEXT("first distinct identity keeps base"),
		TryAllocateReadableKeyedItemAlias(TEXT("X"), Occurrences, Used, Alias, Error, &Reserved));
	TestEqual(TEXT("first has no #1"), Alias, FString(TEXT("X")));
	TestTrue(TEXT("duplicate skips a genuine reserved name"),
		TryAllocateReadableKeyedItemAlias(TEXT("X"), Occurrences, Used, Alias, Error, &Reserved));
	TestEqual(TEXT("suffix allocated within the original base"), Alias, FString(TEXT("X#3")));
	TestTrue(TEXT("genuine name is preserved"),
		TryAllocateReadableKeyedItemAlias(TEXT("X#2"), Occurrences, Used, Alias, Error, &Reserved));
	TestEqual(TEXT("genuine suffix not stripped or renumbered"), Alias, FString(TEXT("X#2")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentKeyedDefaultAlternativesTest,
	"UeAgentInterface.Core.HjsonWorkspace.KeyedItem.DefaultValueAlternatives",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentKeyedDefaultAlternativesTest::RunTest(const FString&)
{
	auto Resources = MakeSemanticWorkspaceResourceTracker(); FWorkspaceError Error;
	FUeAgentCanonicalValuePtr True, False; FUeAgentStateSemanticError SemanticError;
	if (!FUeAgentCanonicalValue::TryBoolean(true, Resources, True, SemanticError) || !FUeAgentCanonicalValue::TryBoolean(false, Resources, False, SemanticError)) return false;
	auto Text = [&](const TCHAR* Value) { return StringValue(Value, Resources); };
	auto Row = [&](TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields) { return ObjectValue(MoveTemp(Fields), Resources); };
	const auto Empty = Row({}); const auto Unset = Row({{TEXT("is_set"), False.ToSharedRef()}});
	const auto Captured = ArrayValue({Row({{TEXT("channel_ref"), Text(TEXT("native-channel"))}, {TEXT("name"), Text(TEXT("Value"))},
		{TEXT("channel_kind"), Text(TEXT("object_path"))}, {TEXT("default"), Unset}, {TEXT("keys"), ArrayValue({}, Resources)}, {TEXT("default_key"), Empty}})}, Resources);
	const auto& Contract = GetLevelSequenceScalarChannelProjectionContract(); FUeAgentCanonicalValuePtr Resolved; bool bChanged = false;
	auto Resolve = [&](const FUeAgentCanonicalValueRef& Default, const FUeAgentCanonicalValue& Before) {
		const auto Desired = Row({{TEXT("Value"), Row({{TEXT("default"), Default}})}});
		return TryResolveKeyedItemDesiredState(Desired.Get(), Before, TEXT("channels"), Contract, nullptr, Resources, Resolved, bChanged, Error); };
	const auto Reference = Row({{TEXT("path"), Text(TEXT("/Game/Example.Example"))}});
	const auto SetObject = Row({{TEXT("is_set"), True.ToSharedRef()}, {TEXT("value"), Reference}});
	if (!TestTrue(TEXT("reference default branch accepts exactly its value field"), Resolve(SetObject, Captured.Get()))) return false;
	const auto WithObject = Resolved;
	if (!TestTrue(TEXT("clear exits the reference branch value"), Resolve(Unset, *WithObject))) return false;
	TestNull(TEXT("cleared default no longer contains reference value"), FindField(*FindField(Resolved->GetArrayItems()[0].Get(), TEXT("default")), TEXT("value")));
	TestTrue(TEXT("scalar default remains an alternative"), Resolve(Row({{TEXT("is_set"), True.ToSharedRef()}, {TEXT("value_lexeme"), Text(TEXT(""))}}), Captured.Get()));
	TestFalse(TEXT("both value owners are forbidden"), Resolve(Row({{TEXT("is_set"), True.ToSharedRef()}, {TEXT("value"), Reference}, {TEXT("value_lexeme"), Text(TEXT("0"))}}), Captured.Get()));
	TestFalse(TEXT("set requires one value owner"), Resolve(Row({{TEXT("is_set"), True.ToSharedRef()}}), Captured.Get()));
	TestFalse(TEXT("clear rejects explicit inactive value"), Resolve(Row({{TEXT("is_set"), False.ToSharedRef()}, {TEXT("value"), Reference}}), Captured.Get()));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentKeyedItemVisibleProjectionTest,
	"UeAgentInterface.Core.HjsonWorkspace.KeyedItem.VisibleProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentKeyedItemVisibleProjectionTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> CapturedActor = MakeShared<FJsonObject>();
	CapturedActor->SetStringField(TEXT("managed_id"), TEXT("actor:machine"));
	CapturedActor->SetStringField(TEXT("actor_guid"), TEXT("machine-guid"));
	CapturedActor->SetStringField(TEXT("object_path"), TEXT("PersistentLevel.Actor_0"));
	CapturedActor->SetStringField(TEXT("revision"), TEXT("revision:1"));
	CapturedActor->SetStringField(TEXT("hash"), TEXT("hash:1"));
	CapturedActor->SetStringField(TEXT("operation"), TEXT("update"));
	CapturedActor->SetStringField(TEXT("label"), TEXT("Readable Actor"));
	CapturedActor->SetStringField(TEXT("type"), TEXT("/Script/Engine.Actor"));
	TSharedRef<FJsonObject> Attachment = MakeShared<FJsonObject>();
	Attachment->SetStringField(TEXT("parent_managed_id"), TEXT("parent-machine"));
	CapturedActor->SetObjectField(TEXT("attachment"), Attachment);
	TSharedRef<FJsonObject> CarrierRevision = MakeShared<FJsonObject>();
	CarrierRevision->SetStringField(TEXT("revision"), TEXT("nested-revision"));
	CapturedActor->SetObjectField(TEXT("carrier_revision"), CarrierRevision);
	CapturedActor->SetArrayField(TEXT("data_layer_refs"),
		{MakeShared<FJsonValueString>(TEXT("machine-layer"))});
	TSharedRef<FJsonObject> Component = MakeShared<FJsonObject>();
	Component->SetStringField(TEXT("object_path"), TEXT("machine-component"));
	CapturedActor->SetArrayField(TEXT("components"),
		{MakeShared<FJsonValueObject>(Component)});
	TSharedRef<FJsonObject> Captured = MakeShared<FJsonObject>();
	Captured->SetObjectField(TEXT("ReadableActor1"), CapturedActor);

	TSharedRef<FJsonObject> Visible = MakeShared<FJsonObject>();
	FWorkspaceError Error;
	auto Resources = MakeSemanticWorkspaceResourceTracker();
	FUeAgentCanonicalValuePtr CapturedValue, VisibleValue;
	FString ProjectionError;
	FKeyedItemProjectionContract Contract;
	TSharedPtr<FJsonValue> JsonVisible;
	if (!TestTrue(TEXT("keyed item projection succeeds"),
		CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueObject>(Captured), Resources, CapturedValue, ProjectionError)
		&& TryGetKeyedItemProjectionContract(TEXT("actors"), Contract)
		&& TryProjectKeyedItemVisibleValue(TEXT("level"), *CapturedValue, Contract, Resources, VisibleValue, Error)
		&& CanonicalJson::TryToJsonValue(*VisibleValue, JsonVisible, ProjectionError))) return false;
	Visible = JsonVisible->AsObject().ToSharedRef();
	const TSharedPtr<FJsonObject>* Actor = nullptr;
	TestTrue(TEXT("readable actor key remains visible"),
		Visible->TryGetObjectField(TEXT("ReadableActor1"), Actor));
	TestEqual(TEXT("ordinary label remains visible"),
		(*Actor)->GetStringField(TEXT("label")), FString(TEXT("Readable Actor")));
	TestEqual(TEXT("read-only type remains visible"),
		(*Actor)->GetStringField(TEXT("type")), FString(TEXT("/Script/Engine.Actor")));
	for (const TCHAR* HiddenCarrier : {
		TEXT("attachment"), TEXT("carrier_revision"), TEXT("data_layer_refs"),
		TEXT("components")})
	{
		TestFalse(TEXT("unapproved nested carrier is not projected"),
			(*Actor)->HasField(HiddenCarrier));
	}
	for (const TCHAR* MachineField : {
		TEXT("managed_id"), TEXT("actor_guid"), TEXT("object_path"),
		TEXT("revision"), TEXT("hash"), TEXT("operation")})
	{
		TestFalse(TEXT("machine field is hidden"), (*Actor)->HasField(MachineField));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLevelSequenceKeyedCollectionTest,
	"UeAgentInterface.Core.HjsonWorkspace.KeyedItem.LevelSequenceScalarKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLevelSequenceKeyedCollectionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	auto Resources = MakeSemanticWorkspaceResourceTracker();
	const auto Time = [](const FString& Tick, FUeAgentResourceTracker& Tracker)
	{
		return ObjectValue({
			{TEXT("clock_id"), StringValue(TEXT("level_sequence:/Game/Test.Test"), Tracker)},
			{TEXT("tick"), StringValue(Tick, Tracker)},
			{TEXT("rate"), ObjectValue({
				{TEXT("numerator"), StringValue(TEXT("24000"), Tracker)},
				{TEXT("denominator"), StringValue(TEXT("1"), Tracker)}}, Tracker)},
			{TEXT("state"), StringValue(TEXT("authored"), Tracker)}}, Tracker);
	};
	const auto Captured = ArrayValue({
		ObjectValue({
			{TEXT("key_ref"), StringValue(TEXT("machine:key:0"), Resources)},
			{TEXT("time"), Time(TEXT("0"), Resources)},
			{TEXT("value_type"), StringValue(TEXT("decimal"), Resources)},
			{TEXT("value_lexeme"), StringValue(TEXT("1.25"), Resources)}}, Resources),
		ObjectValue({
			{TEXT("key_ref"), StringValue(TEXT("machine:key:1"), Resources)},
			{TEXT("time"), Time(TEXT("12000"), Resources)},
			{TEXT("value_type"), StringValue(TEXT("decimal"), Resources)},
			{TEXT("value_lexeme"), StringValue(TEXT("2.5"), Resources)}}, Resources)}, Resources);
	const auto& Contract = GetLevelSequenceScalarKeyProjectionContract();
	FUeAgentCanonicalValuePtr Visible;
	FWorkspaceError Error;
	if (!TestTrue(TEXT("captured keys project to readable map"),
		TryProjectKeyedItemVisibleValue(TEXT("section/keys"), Captured.Get(), Contract,
			Resources, Visible, Error))) return false;
	TestTrue(TEXT("first readable key exists"), FindField(*Visible, TEXT("Key1")) != nullptr);
	TestTrue(TEXT("second readable key exists"), FindField(*Visible, TEXT("Key2")) != nullptr);
	TestTrue(TEXT("machine key identity is hidden"),
		FindField(*FindField(*Visible, TEXT("Key1")), TEXT("key_ref")) == nullptr);
	TestTrue(TEXT("owning clock identity is hidden"),
		FindField(*FindField(*FindField(*Visible, TEXT("Key1")), TEXT("time")), TEXT("clock_id")) == nullptr);
	const auto Defaults = ObjectValue({{TEXT("time"), Time(TEXT("0"), Resources)}}, Resources);

	const auto Desired = ObjectValue({
		{TEXT("Key1"), ObjectValue({
			{TEXT("time"), ObjectValue({{TEXT("tick"), StringValue(TEXT("6000"), Resources)},
				{TEXT("rate"), ObjectValue({{TEXT("numerator"), StringValue(TEXT("24000"), Resources)}}, Resources)}}, Resources)},
			{TEXT("value_lexeme"), StringValue(TEXT("7.75"), Resources)}}, Resources)},
		{TEXT("Later"), ObjectValue({
			{TEXT("time"), ObjectValue({{TEXT("tick"), StringValue(TEXT("18000"), Resources)}}, Resources)},
			{TEXT("value_type"), StringValue(TEXT("decimal"), Resources)},
			{TEXT("value_lexeme"), StringValue(TEXT("9.5"), Resources)}}, Resources)}}, Resources);
	FUeAgentCanonicalValuePtr Resolved;
	bool bChanged = false;
	if (!TestTrue(TEXT("complete desired key collection resolves delete update add"),
		TryResolveKeyedItemDesiredState(Desired.Get(), Captured.Get(), TEXT("section/keys"),
			Contract, &Defaults.Get(), Resources, Resolved, bChanged, Error))) return false;
	TestTrue(TEXT("collection change is reported"), bChanged);
	TestEqual(TEXT("resolved collection has intended count"), Resolved->GetArrayItems().Num(), 2);
	TestEqual(TEXT("existing key restores hidden identity"),
		ReadString(Resolved->GetArrayItems()[0].Get(), TEXT("key_ref")), FString(TEXT("machine:key:0")));
	TestEqual(TEXT("omitted existing value field is preserved except explicit overlay"),
		ReadString(Resolved->GetArrayItems()[0].Get(), TEXT("value_lexeme")), FString(TEXT("7.75")));
	TestEqual(TEXT("new readable key becomes request-local native identity"),
		ReadString(Resolved->GetArrayItems()[1].Get(), TEXT("key_ref")), FString(TEXT("Later")));
	for (const auto& Record : Resolved->GetArrayItems())
	{
		const auto* ResolvedTime = FindField(Record.Get(), TEXT("time"));
		TestEqual(TEXT("sparse time restores owning clock"), ReadString(*ResolvedTime, TEXT("clock_id")),
			FString(TEXT("level_sequence:/Game/Test.Test")));
		TestEqual(TEXT("sparse time preserves omitted state"), ReadString(*ResolvedTime, TEXT("state")), FString(TEXT("authored")));
		TestEqual(TEXT("sparse time preserves omitted rate"), ReadString(*FindField(*ResolvedTime, TEXT("rate")), TEXT("numerator")), FString(TEXT("24000")));
		TestEqual(TEXT("sparse nested rate preserves omitted denominator"), ReadString(*FindField(*ResolvedTime, TEXT("rate")), TEXT("denominator")), FString(TEXT("1")));
	}

	const auto Empty = ObjectValue({}, Resources);
	Resolved.Reset();
	TestTrue(TEXT("explicit empty key map clears the channel"),
		TryResolveKeyedItemDesiredState(Empty.Get(), Captured.Get(), TEXT("section/keys"),
			Contract, nullptr, Resources, Resolved, bChanged, Error));
	TestEqual(TEXT("empty desired key map resolves empty array"), Resolved->GetArrayItems().Num(), 0);
	const auto& SectionsContract = GetLevelSequenceScalarSectionProjectionContract();
	const auto Bound = ObjectValue({{TEXT("kind"), StringValue(TEXT("inclusive"), Resources)},
		{TEXT("point"), Time(TEXT("0"), Resources)}}, Resources);
	const auto Range = ObjectValue({{TEXT("clock_id"), StringValue(TEXT("level_sequence:/Game/Test.Test"), Resources)},
		{TEXT("lower"), Bound}, {TEXT("upper"), Bound}, {TEXT("state"), StringValue(TEXT("authored"), Resources)}}, Resources);
	auto Channels = [&](const FUeAgentCanonicalValueRef& Keys, const TCHAR* Kind = TEXT("float")) {
		return ArrayValue({ObjectValue({{TEXT("channel_ref"), StringValue(TEXT("channel:0"), Resources)},
			{TEXT("name"), StringValue(TEXT("Value"), Resources)}, {TEXT("channel_kind"), StringValue(Kind, Resources)},
			{TEXT("default_key"), ObjectValue({{TEXT("properties"), ObjectValue({{TEXT("fields"), Empty}}, Resources)}}, Resources)},
			{TEXT("keys"), Keys}}, Resources)}, Resources);
	};
	auto VisibleChannels = [&](const FUeAgentCanonicalValueRef& Keys) {
		return ObjectValue({{TEXT("Value"), ObjectValue({{TEXT("keys"), Keys}}, Resources)}}, Resources);
	};
	const auto DefaultChannels = Channels(ArrayValue({}, Resources));
	const auto SectionDefaults = ObjectValue({{TEXT("section_kind"), StringValue(TEXT("float"), Resources)},
		{TEXT("range"), Range}, {TEXT("channels"), DefaultChannels}}, Resources);
	const auto NestedDefaults = ObjectValue({{TEXT("channels"), ObjectValue({{TEXT("nested_record_defaults"),
		ObjectValue({{TEXT("keys"), Defaults}}, Resources)}}, Resources)}}, Resources);
	const auto Sections = ArrayValue({ObjectValue({{TEXT("section_ref"), StringValue(TEXT("native:section:0"), Resources)},
		{TEXT("section_kind"), StringValue(TEXT("float"), Resources)}, {TEXT("range"), Range}, {TEXT("channels"), Channels(Captured)}}, Resources)}, Resources);
	if (!TestTrue(TEXT("section collection recursively projects nested keys"), TryProjectKeyedItemVisibleValue(
		TEXT("track/sections"), Sections.Get(), SectionsContract, Resources, Visible, Error))) return false;
	const auto* VisibleSection = FindField(*Visible, TEXT("Section1"));
	TestFalse(TEXT("fixed channels reject removal"), TryResolveKeyedItemDesiredState(Empty.Get(), DefaultChannels.Get(),
		TEXT("section/channels"), GetLevelSequenceScalarChannelProjectionContract(), nullptr, Resources, Resolved, bChanged, Error));
	TestEqual(TEXT("fixed channel removal is explicit"), Error.GetCode(), FString(TEXT("keyed_item_remove_unsupported")));
	TestFalse(TEXT("retired section keys write surface is rejected"), TryResolveKeyedItemDesiredState(
		ObjectValue({{TEXT("Section1"), ObjectValue({{TEXT("keys"), Empty}}, Resources)}}, Resources).Get(), Sections.Get(),
		TEXT("track/sections"), SectionsContract, &SectionDefaults.Get(), Resources, Resolved, bChanged, Error));
	TestEqual(TEXT("retired section keys is no longer a public field"), Error.GetCode(), FString(TEXT("keyed_record_field_unknown")));
	TestTrue(TEXT("section identity and nested point clock hidden"), VisibleSection
		&& !FindField(*VisibleSection, TEXT("section_ref"))
		&& !FindField(*FindField(*FindField(*FindField(*VisibleSection, TEXT("range")), TEXT("lower")), TEXT("point")), TEXT("clock_id")));
	const auto DesiredSections = ObjectValue({
		{TEXT("Section1"), ObjectValue({{TEXT("channels"), VisibleChannels(Desired)}}, Resources)},
		{TEXT("NewSection"), ObjectValue({{TEXT("channels"), VisibleChannels(Desired)},
			{TEXT("range"), ObjectValue({{TEXT("lower"), ObjectValue({{TEXT("point"), ObjectValue({{TEXT("tick"), StringValue(TEXT("6000"), Resources)}}, Resources)}}, Resources)}}, Resources)}}, Resources)}}, Resources);
	if (!TestTrue(TEXT("nested collection resolves existing and new section from same defaults"), TryResolveKeyedItemDesiredState(
		DesiredSections.Get(), Sections.Get(), TEXT("track/sections"), SectionsContract, &SectionDefaults.Get(), Resources,
		Resolved, bChanged, Error, &NestedDefaults.Get()))) return false;
	TestEqual(TEXT("section add count"), Resolved->GetArrayItems().Num(), 2);
	for (const auto& Section : Resolved->GetArrayItems())
	{
		TestEqual(TEXT("section type comes from capture or parent defaults"), ReadString(Section.Get(), TEXT("section_kind")), FString(TEXT("float")));
		const auto* Point = FindField(*FindField(*FindField(Section.Get(), TEXT("range")), TEXT("lower")), TEXT("point"));
		TestEqual(TEXT("deep sparse bound restores hidden clock"), ReadString(*Point, TEXT("clock_id")), FString(TEXT("level_sequence:/Game/Test.Test")));
	}
	const auto Forbidden = ObjectValue({{TEXT("Section1"), ObjectValue({{TEXT("range"), ObjectValue({{TEXT("lower"),
		ObjectValue({{TEXT("point"), Time(TEXT("0"), Resources)}}, Resources)}}, Resources)}}, Resources)}}, Resources);
	TestFalse(TEXT("nested machine clock input is rejected"), TryResolveKeyedItemDesiredState(Forbidden.Get(), Sections.Get(),
		TEXT("track/sections"), SectionsContract, &SectionDefaults.Get(), Resources, Resolved, bChanged, Error, &NestedDefaults.Get()));
	auto LowerInput = [&](const FString& Kind, bool bPoint) {
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields{{TEXT("kind"), StringValue(Kind, Resources)}};
		if (bPoint) Fields.Emplace(TEXT("point"), ObjectValue({{TEXT("tick"), StringValue(TEXT("27"), Resources)}}, Resources));
		return ObjectValue({{TEXT("Section1"), ObjectValue({{TEXT("range"), ObjectValue({
			{TEXT("lower"), ObjectValue(Fields, Resources)}}, Resources)}}, Resources)}}, Resources);
	};
	const auto OpenInput = LowerInput(TEXT("open"), false);
	if (!TestTrue(TEXT("open branch retires captured point"), TryResolveKeyedItemDesiredState(OpenInput.Get(), Sections.Get(),
		TEXT("track/sections"), SectionsContract, &SectionDefaults.Get(), Resources, Resolved, bChanged, Error))) return false;
	const auto OpenSections = Resolved;
	auto LowerOf = [&](const FUeAgentCanonicalValue& Items) {
		return FindField(*FindField(Items.GetArrayItems()[0].Get(), TEXT("range")), TEXT("lower"));
	};
	TestNull(TEXT("open has no residual point or clock"), FindField(*LowerOf(*OpenSections), TEXT("point")));
	TestFalse(TEXT("explicit point conflicts with open branch"), TryResolveKeyedItemDesiredState(LowerInput(TEXT("open"), true).Get(),
		Sections.Get(), TEXT("track/sections"), SectionsContract, &SectionDefaults.Get(), Resources, Resolved, bChanged, Error));
	TestEqual(TEXT("branch contradiction has precise error"), Error.GetCode(), FString(TEXT("keyed_record_branch_field_forbidden")));
	for (const FString Kind : {FString(TEXT("inclusive")), FString(TEXT("exclusive"))})
	{
		if (!TestTrue(TEXT("missing newly required point is supplied by the current template"), TryResolveKeyedItemDesiredState(LowerInput(Kind, false).Get(),
			*OpenSections, TEXT("track/sections"), SectionsContract, &SectionDefaults.Get(), Resources, Resolved, bChanged, Error))) return false;
		TestEqual(TEXT("template supplies whole new point"), ReadString(*FindField(*LowerOf(*Resolved), TEXT("point")), TEXT("tick")), FString(TEXT("0")));
		if (!TestTrue(TEXT("new bounded branch restores current template point"), TryResolveKeyedItemDesiredState(LowerInput(Kind, true).Get(),
			*OpenSections, TEXT("track/sections"), SectionsContract, &SectionDefaults.Get(), Resources, Resolved, bChanged, Error))) return false;
		const auto* Point = FindField(*LowerOf(*Resolved), TEXT("point"));
		TestEqual(TEXT("new branch accepts only readable tick input"), ReadString(*Point, TEXT("tick")), FString(TEXT("27")));
		TestEqual(TEXT("new branch inherits hidden clock from template"), ReadString(*Point, TEXT("clock_id")), FString(TEXT("level_sequence:/Game/Test.Test")));
		const auto Bounded = Resolved;
		if (!TestTrue(TEXT("bounded branch switch preserves existing point"), TryResolveKeyedItemDesiredState(LowerInput(TEXT("exclusive"), false).Get(),
			*Bounded, TEXT("track/sections"), SectionsContract, &SectionDefaults.Get(), Resources, Resolved, bChanged, Error))) return false;
		TestEqual(TEXT("omitted old point survives allowed branch switch"), ReadString(*FindField(*LowerOf(*Resolved), TEXT("point")), TEXT("tick")), FString(TEXT("27")));
	}
	FKeyedItemProjectionContract Undeclared = SectionsContract;
	Undeclared.ObjectFieldPolicies.FindChecked(TEXT("range/lower")).DiscriminatorField.Reset();
	if (!TestTrue(TEXT("undeclared objects keep legacy sparse overlay"), TryResolveKeyedItemDesiredState(OpenInput.Get(), Sections.Get(),
		TEXT("track/sections"), Undeclared, &SectionDefaults.Get(), Resources, Resolved, bChanged, Error))) return false;
	TestNotNull(TEXT("undeclared kind does not retire old member"), FindField(*LowerOf(*Resolved), TEXT("point")));
	const auto Tracks = ArrayValue({
		ObjectValue({{TEXT("track_ref"), StringValue(TEXT("root_track:0"), Resources)},
			{TEXT("display_name"), StringValue(TEXT("Shared"), Resources)},
			{TEXT("track_kind"), StringValue(TEXT("float"), Resources)},
			{TEXT("default_channels"), DefaultChannels},
			{TEXT("default_properties"), Empty},
			{TEXT("sections"), Sections}}, Resources),
		ObjectValue({{TEXT("track_ref"), StringValue(TEXT("root_track:1"), Resources)},
			{TEXT("display_name"), StringValue(TEXT("Shared"), Resources)},
			{TEXT("track_kind"), StringValue(TEXT("float"), Resources)},
			{TEXT("default_channels"), DefaultChannels},
			{TEXT("default_properties"), Empty},
			{TEXT("sections"), ArrayValue({}, Resources)}}, Resources),
		ObjectValue({{TEXT("track_ref"), StringValue(TEXT("root_track:2"), Resources)},
			{TEXT("display_name"), StringValue(TEXT("Shared2"), Resources)},
			{TEXT("track_kind"), StringValue(TEXT("float"), Resources)},
			{TEXT("default_channels"), DefaultChannels},
			{TEXT("default_properties"), Empty},
			{TEXT("sections"), ArrayValue({}, Resources)}}, Resources)}, Resources);
	const auto& TracksContract = GetLevelSequenceRootScalarTrackProjectionContract();
	const auto DoubleTracks = ArrayValue({ObjectValue({
		{TEXT("track_ref"), StringValue(TEXT("root_track:double"), Resources)},
		{TEXT("display_name"), StringValue(TEXT("Precise"), Resources)},
		{TEXT("track_kind"), StringValue(TEXT("double"), Resources)},
		{TEXT("default_channels"), Channels(ArrayValue({}, Resources), TEXT("double"))},
		{TEXT("default_properties"), Empty},
		{TEXT("sections"), ArrayValue({}, Resources)}}, Resources)}, Resources);
	const auto NewDoubleSection = ObjectValue({{TEXT("Precise"), ObjectValue({
		{TEXT("sections"), ObjectValue({{TEXT("Added"), ObjectValue({
			{TEXT("range"), Empty}, {TEXT("channels"), VisibleChannels(Empty)}}, Resources)}}, Resources)}}, Resources)}}, Resources);
	const auto TrackDefaults = ObjectValue({{TEXT("sections"), ObjectValue({
		{TEXT("record_defaults"), SectionDefaults}, {TEXT("nested_record_defaults"), NestedDefaults}}, Resources)}}, Resources);
	if (!TestTrue(TEXT("new section inherits the captured owning numeric track type"),
		TryResolveKeyedItemDesiredState(NewDoubleSection.Get(), DoubleTracks.Get(), TEXT("root_tracks"),
			TracksContract, nullptr, Resources, Resolved, bChanged, Error, &TrackDefaults.Get())))
	{ AddError(Error.GetCode()); return false; }
	TestEqual(TEXT("double section defaults never fall back to float"),
		ReadString(FindField(Resolved->GetArrayItems()[0].Get(), TEXT("sections"))->GetArrayItems()[0].Get(), TEXT("section_kind")),
		FString(TEXT("double")));
	if (!TestTrue(TEXT("duplicate track display names project to independent readable aliases"),
		TryProjectKeyedItemVisibleValue(TEXT("root_tracks"), Tracks.Get(), TracksContract, Resources, Visible, Error))) return false;
	TestNotNull(TEXT("first semantic alias exists"), FindField(*Visible, TEXT("Shared")));
	TestNotNull(TEXT("duplicate semantic alias has continuous suffix"), FindField(*Visible, TEXT("Shared2")));
	TestEqual(TEXT("suffix collision never drops a captured item"), Visible->GetObjectItems().Num(), 3);
	TestNotNull(TEXT("literal suffixed name receives a globally unique readable alias"), FindField(*Visible, TEXT("Shared22")));
	if (!TestTrue(TEXT("complete visible collection resolves without identity loss"), TryResolveKeyedItemDesiredState(
		*Visible, Tracks.Get(), TEXT("root_tracks"), TracksContract, nullptr, Resources, Resolved, bChanged, Error))) return false;
	TestEqual(TEXT("all three captured identities survive round trip"), Resolved->GetArrayItems().Num(), 3);
	for (int32 Index = 0; Index < 3; ++Index)
		TestEqual(TEXT("deterministic aliases preserve each original identity"), ReadString(Resolved->GetArrayItems()[Index].Get(), TEXT("track_ref")), FString::Printf(TEXT("root_track:%d"), Index));
	TestNull(TEXT("track identity remains hidden"), FindField(*FindField(*Visible, TEXT("Shared2")), TEXT("track_ref")));
	const auto DesiredTracks = ObjectValue({{TEXT("Shared2"), ObjectValue({
		{TEXT("display_name"), StringValue(TEXT("Renamed"), Resources)},
		{TEXT("sections"), ObjectValue({}, Resources)}}, Resources)}}, Resources);
	if (!TestTrue(TEXT("existing-only track collection resolves independent rename delete and nested clear"),
		TryResolveKeyedItemDesiredState(DesiredTracks.Get(), Tracks.Get(), TEXT("root_tracks"), TracksContract,
			nullptr, Resources, Resolved, bChanged, Error))) return false;
	TestEqual(TEXT("one duplicate track remains"), Resolved->GetArrayItems().Num(), 1);
	TestEqual(TEXT("survivor keeps captured identity"), ReadString(Resolved->GetArrayItems()[0].Get(), TEXT("track_ref")), FString(TEXT("root_track:1")));
	TestEqual(TEXT("survivor display name changes"), ReadString(Resolved->GetArrayItems()[0].Get(), TEXT("display_name")), FString(TEXT("Renamed")));
	TestEqual(TEXT("nested sections clear remains supported"), FindField(Resolved->GetArrayItems()[0].Get(), TEXT("sections"))->GetArrayItems().Num(), 0);
	const auto UnknownTrack = ObjectValue({{TEXT("NewTrack"), ObjectValue({
		{TEXT("display_name"), StringValue(TEXT("NewTrack"), Resources)},
		{TEXT("track_kind"), StringValue(TEXT("float"), Resources)},
		{TEXT("sections"), ObjectValue({}, Resources)}}, Resources)}}, Resources);
	TestFalse(TEXT("unknown track creation is rejected before domain writes"),
		TryResolveKeyedItemDesiredState(UnknownTrack.Get(), Tracks.Get(), TEXT("root_tracks"), TracksContract,
			nullptr, Resources, Resolved, bChanged, Error));
	TestEqual(TEXT("unknown track rejection is explicit"), Error.GetCode(), FString(TEXT("keyed_item_create_unsupported")));
	TestTrue(TEXT("explicit empty track map resolves clear"), TryResolveKeyedItemDesiredState(
		Empty.Get(), Tracks.Get(), TEXT("root_tracks"), TracksContract, nullptr, Resources, Resolved, bChanged, Error));
	TestEqual(TEXT("empty desired track map clears collection"), Resolved->GetArrayItems().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentKeyedItemDesiredStateTest,
	"UeAgentInterface.Core.HjsonWorkspace.KeyedItem.DesiredState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentKeyedItemDesiredStateTest::RunTest(const FString& Parameters)
{
	auto Resources = MakeSemanticWorkspaceResourceTracker();
	const auto CapturedActor = ObjectValue({
		{TEXT("managed_id"), StringValue(TEXT("actor:machine"), Resources)},
		{TEXT("actor_guid"), StringValue(TEXT("machine-guid"), Resources)},
		{TEXT("label"), StringValue(TEXT("Old Label"), Resources)}}, Resources);
	const auto RemovedActor = ObjectValue({
		{TEXT("managed_id"), StringValue(TEXT("actor:removed"), Resources)},
		{TEXT("label"), StringValue(TEXT("Removed"), Resources)}}, Resources);
	const auto Captured = ObjectValue({
		{TEXT("ExistingActor1"), CapturedActor},
		{TEXT("RemovedActor1"), RemovedActor}}, Resources);
	const auto Visible = ObjectValue({
		{TEXT("ExistingActor1"), ObjectValue({
			{TEXT("label"), StringValue(TEXT("New Label"), Resources)}}, Resources)},
		{TEXT("NamedActor1"), ObjectValue({
			{TEXT("name"), StringValue(TEXT("/Script/Engine.PointLight"), Resources)},
			{TEXT("label"), StringValue(TEXT("Named"), Resources)}}, Resources)},
		{TEXT("IdActor1"), ObjectValue({
			{TEXT("id"), StringValue(TEXT("candidate:point-light"), Resources)}}, Resources)}},
		Resources);

	FUeAgentCanonicalValuePtr Resolved;
	bool bSemanticChanged = false;
	FWorkspaceError Error;
	TestTrue(TEXT("desired keyed item state resolves"),
		TryResolveKeyedItemDesiredState(
			Visible.Get(), Captured.Get(), TEXT("level"), Resources, Resolved,
			bSemanticChanged, Error));
	TestTrue(TEXT("desired state reports semantic changes"), bSemanticChanged);
	const FUeAgentCanonicalValue* Existing = FindField(*Resolved, TEXT("ExistingActor1"));
	TestEqual(TEXT("existing item resolves update"),
		ReadString(*Existing, TEXT("operation")), FString(TEXT("update")));
	TestEqual(TEXT("existing hidden identity is restored"),
		ReadString(*Existing, TEXT("managed_id")), FString(TEXT("actor:machine")));
	TestEqual(TEXT("existing visible field is updated"),
		ReadString(*Existing, TEXT("label")), FString(TEXT("New Label")));
	TestEqual(TEXT("omitted item resolves remove"),
		ReadString(*FindField(*Resolved, TEXT("RemovedActor1")), TEXT("operation")),
		FString(TEXT("remove")));

	const FUeAgentCanonicalValue* Named = FindField(*Resolved, TEXT("NamedActor1"));
	const FUeAgentCanonicalValue* NamedCandidate = FindField(*Named, TEXT("candidate"));
	TestEqual(TEXT("candidate name alone resolves create"),
		ReadString(*Named, TEXT("operation")), FString(TEXT("create")));
	TestEqual(TEXT("name is normalized into candidate selector"),
		ReadString(*NamedCandidate, TEXT("name")),
		FString(TEXT("/Script/Engine.PointLight")));
	const FUeAgentCanonicalValue* IdItem = FindField(*Resolved, TEXT("IdActor1"));
	const FUeAgentCanonicalValue* IdCandidate = FindField(*IdItem, TEXT("candidate"));
	TestEqual(TEXT("candidate id alone resolves create"),
		ReadString(*IdItem, TEXT("operation")), FString(TEXT("create")));
	TestEqual(TEXT("id is normalized into candidate selector"),
		ReadString(*IdCandidate, TEXT("id")), FString(TEXT("candidate:point-light")));
	TestEqual(TEXT("public id becomes a machine-local keyed handle"),
		ReadString(*IdItem, TEXT("id")), FString(TEXT("IdActor1")));

	const auto Forged = ObjectValue({
		{TEXT("ExistingActor1"), ObjectValue({
			{TEXT("actor_guid"), StringValue(TEXT("forged"), Resources)}}, Resources)}},
		Resources);
	Resolved.Reset();
	TestFalse(TEXT("visible machine identity cannot be forged"),
		TryResolveKeyedItemDesiredState(
			Forged.Get(), Captured.Get(), TEXT("level"), Resources, Resolved,
			bSemanticChanged, Error));
	TestEqual(TEXT("machine identity rejection is structured"),
		Error.GetCode(), FString(TEXT("keyed_item_machine_field_forbidden")));

	const auto NestedForged = ObjectValue({
		{TEXT("ExistingActor1"), ObjectValue({
			{TEXT("attachment"), ObjectValue({
				{TEXT("parent_managed_id"), StringValue(TEXT("forged"), Resources)}},
				Resources)}}, Resources)}}, Resources);
	Resolved.Reset();
	TestFalse(TEXT("nested machine identity cannot be forged"),
		TryResolveKeyedItemDesiredState(NestedForged.Get(), Captured.Get(),
			TEXT("level"), Resources, Resolved, bSemanticChanged, Error));
	TestEqual(TEXT("nested identity rejection is structured"), Error.GetCode(),
		FString(TEXT("keyed_item_machine_field_forbidden")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCapturedHierarchyItemProjectionTest,
	"UeAgentInterface.Core.HjsonWorkspace.KeyedItem.CapturedHierarchyItem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCapturedHierarchyItemProjectionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TSharedRef<FJsonObject> CapturedJson = MakeShared<FJsonObject>();
	CapturedJson->SetStringField(TEXT("unit_id"), TEXT("machine-unit"));
	CapturedJson->SetStringField(TEXT("actor_guid"), TEXT("machine-guid"));
	CapturedJson->SetStringField(TEXT("managed_id"), TEXT("machine-id"));
	CapturedJson->SetStringField(TEXT("object_path"), TEXT("PersistentLevel.Actor_0"));
	CapturedJson->SetStringField(TEXT("world_path"), TEXT("/Game/Maps/Fixture"));
	CapturedJson->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.PointLight"));
	TSharedRef<FJsonObject> VisibleJson = MakeShared<FJsonObject>();
	FWorkspaceError Error;
	TestTrue(TEXT("single hierarchy item projects"),
		TryProjectItemVisibleObject(TEXT("actor-unit"), CapturedJson,
			GetActorComponentHierarchyItemVisibleFieldPolicy(), VisibleJson, Error));
	TestTrue(TEXT("readable actor class remains visible"),
		VisibleJson->HasField(TEXT("actor_class")));
	TestFalse(TEXT("unit id is hidden"), VisibleJson->HasField(TEXT("unit_id")));
	TestFalse(TEXT("actor identity is hidden"), VisibleJson->HasField(TEXT("actor_guid")));
	TestFalse(TEXT("world path is hidden"), VisibleJson->HasField(TEXT("world_path")));

	auto Resources = MakeSemanticWorkspaceResourceTracker();
	const auto Captured = ObjectValue({
		{TEXT("unit_id"), StringValue(TEXT("machine-unit"), Resources)},
		{TEXT("actor_guid"), StringValue(TEXT("machine-guid"), Resources)},
		{TEXT("owner_managed_id"), StringValue(TEXT("owner-machine"), Resources)},
		{TEXT("actor_class"), StringValue(TEXT("/Script/Engine.PointLight"), Resources)}},
		Resources);
	const auto Visible = ObjectValue({
		{TEXT("actor_class"), StringValue(TEXT("/Script/Engine.SpotLight"), Resources)}},
		Resources);
	FUeAgentCanonicalValuePtr Resolved;
	bool bChanged = false;
	TestTrue(TEXT("captured hierarchy item resolves"),
		TryResolveCapturedItemDesiredState(Visible.Get(), Captured.Get(), TEXT("actor-unit"),
			GetActorComponentHierarchyItemVisibleFieldPolicy(), Resources, Resolved,
			bChanged, Error));
	TestTrue(TEXT("visible overlay reports change"), bChanged);
	TestEqual(TEXT("hidden actor identity round trips"),
		ReadString(*Resolved, TEXT("actor_guid")), FString(TEXT("machine-guid")));
	TestEqual(TEXT("hidden owner identity round trips"),
		ReadString(*Resolved, TEXT("owner_managed_id")), FString(TEXT("owner-machine")));
	TestEqual(TEXT("visible actor class overlays baseline"),
		ReadString(*Resolved, TEXT("actor_class")), FString(TEXT("/Script/Engine.SpotLight")));

	const auto Forged = ObjectValue({
		{TEXT("envelope"), ObjectValue({
			{TEXT("owner_actor_guid"), StringValue(TEXT("forged"), Resources)}}, Resources)}},
		Resources);
	Resolved.Reset();
	TestFalse(TEXT("nested machine carrier is rejected"),
		TryResolveCapturedItemDesiredState(Forged.Get(), Captured.Get(), TEXT("actor-unit"),
			GetActorComponentHierarchyItemVisibleFieldPolicy(), Resources, Resolved,
			bChanged, Error));
	TestEqual(TEXT("nested carrier rejection is structured"), Error.GetCode(),
		FString(TEXT("keyed_item_machine_field_forbidden")));
	return true;
}
