#include "Misc/AutomationTest.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "DomainAdapters/WorldSpatial/UeAgentLevelSemanticWorkspaceAdapter.h"
#include "Dom/JsonObject.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::DomainAdapters::WorldSpatial;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	FUeAgentCanonicalValueRef StringValue(const FString& Text,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value; FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(Text, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef BoolValue(bool bValue,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value; FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryBoolean(bValue, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef NumberValue(double Number,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value; FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryDecimal(
			FString::SanitizeFloat(Number), Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef ObjectValue(
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value; FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryObject(Fields, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FChangeDraft PropertyChange(const FString& ManagedId,
		FUeAgentCanonicalValuePtr Before, FUeAgentCanonicalValuePtr After)
	{
		return FChangeDraft(
			FUeAgentCanonicalIdentity(
				TEXT("uai.semantic.level.actor.property"), TEXT("level_actor_property"),
				TEXT("fixture"),
				{{TEXT("world_path"), TEXT("/Game/Test/Map")},
				 {TEXT("managed_id"), ManagedId},
				 {TEXT("property_path"), TEXT("bHidden")}},
				TEXT("property"), TEXT("1")),
			EChangeIntent::Update, MoveTemp(Before), MoveTemp(After),
			{TEXT("authoring.write")}, TEXT("evidence:test"), {});
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLevelSemanticWorkspaceAdapterTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.LevelSemanticWorkspaceAdapter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLevelSemanticWorkspaceAdapterTest::RunTest(const FString& Parameters)
{
	auto Resources = MakeSemanticWorkspaceResourceTracker();
	const auto CandidateName = ObjectValue({
		{TEXT("name"), StringValue(TEXT("/Script/Engine.PointLight"), Resources)}}, Resources);
	const auto CandidateId = ObjectValue({
		{TEXT("id"), StringValue(TEXT("candidate:spot-light"), Resources)}}, Resources);
	const auto Actors = ObjectValue({
		{TEXT("Existing"), ObjectValue({
			{TEXT("managed_id"), StringValue(TEXT("existing-id"), Resources)},
			{TEXT("operation"), StringValue(TEXT("update"), Resources)},
			{TEXT("label"), StringValue(TEXT("Updated"), Resources)}}, Resources)},
		{TEXT("Removed"), ObjectValue({
			{TEXT("managed_id"), StringValue(TEXT("removed-id"), Resources)},
			{TEXT("operation"), StringValue(TEXT("remove"), Resources)}}, Resources)},
		{TEXT("Named"), ObjectValue({
			{TEXT("id"), StringValue(TEXT("Named"), Resources)},
			{TEXT("operation"), StringValue(TEXT("create"), Resources)},
			{TEXT("candidate"), CandidateName},
			{TEXT("transform"), ObjectValue({
				{TEXT("location"), ObjectValue({
					{TEXT("x"), NumberValue(10.0, Resources)},
					{TEXT("y"), NumberValue(20.0, Resources)},
					{TEXT("z"), NumberValue(30.0, Resources)}}, Resources)},
				{TEXT("rotation"), ObjectValue({
					{TEXT("pitch"), NumberValue(0.0, Resources)},
					{TEXT("yaw"), NumberValue(90.0, Resources)},
					{TEXT("roll"), NumberValue(0.0, Resources)}}, Resources)},
				{TEXT("scale"), ObjectValue({
					{TEXT("x"), NumberValue(1.0, Resources)},
					{TEXT("y"), NumberValue(1.0, Resources)},
					{TEXT("z"), NumberValue(1.0, Resources)}}, Resources)}}, Resources)}},
			Resources)},
		{TEXT("ById"), ObjectValue({
			{TEXT("id"), StringValue(TEXT("ById"), Resources)},
			{TEXT("operation"), StringValue(TEXT("create"), Resources)},
			{TEXT("candidate"), CandidateId}}, Resources)}}, Resources);

	TArray<FSemanticUnit> Units;
	Units.Emplace(TEXT("target"), ESemanticUnitKind::TargetState, FString(),
		TEXT("ue.level.asset"), TEXT("authoring_target"), FString(),
		TMap<FString, FUeAgentCanonicalValueRef>{{TEXT("world_path"),
			StringValue(TEXT("/Game/Test/Map"), Resources)}}, TArray<FString>{},
		TArray<FString>{}, FString(), FString(), TArray<FString>{},
		TArray<FString>{TEXT("state")}, TEXT("fixture"), FString());
	Units.Emplace(TEXT("hierarchy"), ESemanticUnitKind::StructureState,
		TEXT("target"), TEXT("uai.authoring.surface.actor_component_hierarchy"),
		TEXT("actor_component_hierarchy"), FString(),
		TMap<FString, FUeAgentCanonicalValueRef>{{TEXT("actors"), Actors}},
		TArray<FString>{}, TArray<FString>{}, FString(), FString(),
		TArray<FString>{}, TArray<FString>{TEXT("state")}, TEXT("fixture"), FString());
	FAuthoringModelPtr Model; FWorkspaceError Error;
	TestTrue(TEXT("fixture model compiles"), TryCompileAuthoringModel(
		TEXT("fixture"), 1, TEXT("llm_authoring"), TEXT("revision"),
		MoveTemp(Units), {TEXT("target")}, Resources, Model, Error));
	const FResolvedSemanticWorkspaceRef Workspace = MakeShared<FResolvedSemanticWorkspace>(
		EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State,
		Model.ToSharedRef(), FUeAgentStateRevision(TEXT("p"), TEXT("r"), 1),
		TArray<FStructuralEdit>{}, TArray<FString>{TEXT("actors.hjson")},
		FString(), FString(), TArray<FString>{}, TEXT("workspace"),
		TOptional<TArray<FString>>(TArray<FString>{TEXT("hierarchy")}));
	FUeAgentCanonicalValuePtr Before = BoolValue(false, Resources);
	FUeAgentCanonicalValuePtr After = BoolValue(true, Resources);
	const FChangeDraft Mixed = PropertyChange(TEXT("existing-id"), Before, After);
	const FChangeDraft StructureChange(
		FUeAgentCanonicalIdentity(
			TEXT("keyed-item-desired-state:hierarchy"),
			TEXT("authoring_keyed_item_desired_state"),
			TEXT("hierarchy"), TEXT("actor_component_hierarchy"),
			TEXT("authoring_structure"), TEXT("1")),
		EChangeIntent::Create, FUeAgentCanonicalValuePtr(), Actors,
		{TEXT("authoring.structure")}, TEXT("evidence:test"), {});
	TSharedRef<FJsonObject> Bundle = MakeShared<FJsonObject>();
	TestTrue(TEXT("production-shaped structure change needs no property path"),
		TryBuildLevelSemanticWorkspaceBundle(
			Workspace, {StructureChange, Mixed}, Bundle, Error));
	const TSharedPtr<FJsonObject>* ProjectedUnits = nullptr;
	TestTrue(TEXT("virtual units exist"), Bundle->TryGetObjectField(TEXT("units"), ProjectedUnits));
	int32 ExistingCount = 0; TSet<FString> NewManagedIds;
	bool bNamedTransformNormalized = false;
	for (const auto& Pair : (*ProjectedUnits)->Values)
	{
		if (!Pair.Key.EndsWith(TEXT("actor.hjson"))) continue;
		const TSharedPtr<FJsonObject> Unit = Pair.Value->AsObject();
		const TSharedPtr<FJsonObject>* Target = nullptr;
		Unit->TryGetObjectField(TEXT("target"), Target);
		FString ManagedId; (*Target)->TryGetStringField(TEXT("managed_id"), ManagedId);
		if (ManagedId == TEXT("existing-id"))
		{
			++ExistingCount;
			TestTrue(TEXT("mixed unit retains properties"), Unit->HasField(TEXT("properties")));
		}
		FString Operation; Unit->TryGetStringField(TEXT("operation"), Operation);
		if (Operation == TEXT("create"))
		{
			TestFalse(TEXT("generated managed id is non-empty"), ManagedId.IsEmpty());
			NewManagedIds.Add(ManagedId);
			const TSharedPtr<FJsonObject>* Extension = nullptr;
			TestTrue(TEXT("create retains candidate"),
				Unit->TryGetObjectField(TEXT("provider_extension"), Extension)
				&& (*Extension)->HasField(TEXT("candidate")));
			if (Extension && Extension->IsValid())
			{
				const TSharedPtr<FJsonObject>* Candidate = nullptr;
				FString CandidateNameText;
				if ((*Extension)->TryGetObjectField(TEXT("candidate"), Candidate)
					&& Candidate && Candidate->IsValid()
					&& (*Candidate)->TryGetStringField(TEXT("name"), CandidateNameText)
					&& CandidateNameText == TEXT("/Script/Engine.PointLight"))
				{
					const TSharedPtr<FJsonObject>* Transform = nullptr;
					const TSharedPtr<FJsonObject>* Rotation = nullptr;
					const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
					bNamedTransformNormalized =
						(*Extension)->TryGetObjectField(TEXT("transform"), Transform)
						&& Transform && Transform->IsValid()
						&& (*Transform)->TryGetObjectField(TEXT("rotation"), Rotation)
						&& Rotation && Rotation->IsValid()
						&& (*Rotation)->TryGetArrayField(TEXT("components"), Components)
						&& Components && Components->Num() == 3;
				}
			}
		}
	}
	TestEqual(TEXT("mixed actor has one virtual unit"), ExistingCount, 1);
	TestEqual(TEXT("two creates get distinct hidden managed ids"), NewManagedIds.Num(), 2);
	TestTrue(TEXT("semantic actor transform uses the shared level transform adapter"),
		bNamedTransformNormalized);

	FUeAgentCanonicalValuePtr RemoveBefore = BoolValue(false, Resources);
	FUeAgentCanonicalValuePtr RemoveAfter = BoolValue(true, Resources);
	const FChangeDraft RemoveConflict =
		PropertyChange(TEXT("removed-id"), RemoveBefore, RemoveAfter);
	TestFalse(TEXT("remove plus property is rejected"),
		TryBuildLevelSemanticWorkspaceBundle(
			Workspace, {RemoveConflict}, Bundle, Error));
	TestEqual(TEXT("remove conflict is structured"), Error.GetCode(),
		FString(TEXT("level_semantic_actor_remove_property_conflict")));
	return true;
}
