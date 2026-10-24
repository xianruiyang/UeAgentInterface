#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceRebase.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringPropertyValues.h"
#include "Components/LightComponentBase.h"
#include "UObject/UnrealType.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringDescriptors.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentMovieSceneAuthoring.h"
#include "Utilities/AuthoringCore/UeAgentMovieSceneScalarChannel.h"
#include "MovieScene.h"
#include "MovieSceneTimeHelpers.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Components/PointLightComponent.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"
#include "Utilities/UeAgentCurveJson.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDocuments.h"
#include "Components/PrimitiveComponent.h"
#include "Components/BoxComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Rigs/RigHierarchyElements.h"
#include "Domains/Blackboard/Authoring/UeAgentBlackboardAuthoringState.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDocuments.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringDocuments.h"
#include "MetasoundFrontendLiteral.h"
#include "ControlRigBlueprint.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"

namespace
{
using namespace UE::UAI::Composition::HjsonImport;
using namespace UE::UAI::ContextResource;
using namespace UE::UAI::Contract;
using namespace UE::UAI::HjsonWorkspace;
using namespace UE::UAI::PlanningPreflight;
using namespace UE::UAI::StateSemantic;

FUeAgentResourceTracker MakeImportTracker()
{
	return FUeAgentResourceTracker();
}

FUeAgentCanonicalValueRef StringValue(const FString& Text, FUeAgentResourceTracker& Resources)
{
	FUeAgentCanonicalValuePtr Value;
	FUeAgentStateSemanticError Error;
	check(FUeAgentCanonicalValue::TryString(Text, Resources, Value, Error));
	return Value.ToSharedRef();
}

FUeAgentCanonicalValueRef DecimalValue(const FString& Lexeme, FUeAgentResourceTracker& Resources)
{
	FUeAgentCanonicalValuePtr Value;
	FUeAgentStateSemanticError Error;
	check(FUeAgentCanonicalValue::TryDecimal(Lexeme, Resources, Value, Error));
	return Value.ToSharedRef();
}

FUeAgentCanonicalValueRef IntegerValue(const FString& Lexeme, FUeAgentResourceTracker& Resources)
{
	FUeAgentCanonicalValuePtr Value;
	FUeAgentStateSemanticError Error;
	check(FUeAgentCanonicalValue::TryInteger(Lexeme, Resources, Value, Error));
	return Value.ToSharedRef();
}

FUeAgentCanonicalValueRef ObjectValue(
	TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields, FUeAgentResourceTracker& Resources)
{
	FUeAgentCanonicalValuePtr Value;
	FUeAgentStateSemanticError Error;
	check(FUeAgentCanonicalValue::TryObject(Fields, Resources, Value, Error));
	return Value.ToSharedRef();
}

const FUeAgentCanonicalValueRef* ObjectField(
	const FUeAgentCanonicalValueRef& Object, const FString& Key)
{
	for (const TPair<FString, FUeAgentCanonicalValueRef>& Field
		: Object->GetObjectItems())
	{
		if (Field.Key == Key)
		{
			return &Field.Value;
		}
	}
	return nullptr;
}

FUeAgentCanonicalValueRef ArrayValue(
	TArray<FUeAgentCanonicalValueRef> Items, FUeAgentResourceTracker& Resources)
{
	FUeAgentCanonicalValuePtr Value;
	FUeAgentStateSemanticError Error;
	check(FUeAgentCanonicalValue::TryArray(Items, Resources, Value, Error));
	return Value.ToSharedRef();
}

FUeAgentCanonicalIdentity StructureIdentity()
{
	return FUeAgentCanonicalIdentity(
		TEXT("uai.semantic.blueprint.graph"), TEXT("blueprint_graph"),
		TEXT("graph:EventGraph"),
		{{TEXT("asset_path"), TEXT("/Game/HX013/BP_Structural")},
			{TEXT("graph_kind"), TEXT("event_graph")},
			{TEXT("graph_name"), TEXT("EventGraph")},
			{CanonicalAuthoringTargetIdentitySelector,
				TEXT("blueprint_graph_identity|blueprint_graph|guid|asset_path=/game/hx013/"
					 "bp_structural&graph_guid=fixture")}},
		TEXT("blueprint_graph_identity"), TEXT("generation:structural"));
}

FString StructureAuthoringIdentityKey()
{
	return TEXT("blueprint_graph_identity|blueprint_graph|guid|asset_path=/game/hx013/"
				"bp_structural&graph_guid=fixture");
}

FUeAgentCanonicalValueRef StructureValue(
	FUeAgentResourceTracker& Resources, const bool bWithNode)
{
	TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
	Fields.Emplace(
		TEXT("graph"),
		ObjectValue(
			{{TEXT("graph_kind"), StringValue(TEXT("event_graph"), Resources)},
				{TEXT("name"), StringValue(TEXT("EventGraph"), Resources)}},
			Resources));
	if (bWithNode)
	{
		Fields.Emplace(
			TEXT("nodes"),
			ArrayValue(
				{ObjectValue(
					{{TEXT("id"), StringValue(TEXT("candidate:fixture"), Resources)}},
					Resources)},
				Resources));
	}
	return ObjectValue(MoveTemp(Fields), Resources);
}

FAuthoringModelRef MakeStructureModel(
	FUeAgentResourceTracker& Resources, const bool bWithNode)
{
	const FUeAgentCanonicalValueRef Value = StructureValue(Resources, bWithNode);
	TMap<FString, FUeAgentCanonicalValueRef> Values;
	for (const TPair<FString, FUeAgentCanonicalValueRef>& Field : Value->GetObjectItems())
	{
		Values.Add(Field.Key, Field.Value);
	}
	TArray<FSemanticUnit> Units;
	Units.Emplace(
		TEXT("target:blueprint-structure"), ESemanticUnitKind::TargetState, FString(),
		TEXT("ue.blueprint.asset"), TEXT("authoring_target"), FString(),
		TMap<FString, FUeAgentCanonicalValueRef>(),
		TArray<FString>({TEXT("structure:event-graph")}), TArray<FString>(),
		FString(), FString(), TArray<FString>(), TArray<FString>({TEXT("state")}),
		TEXT("fixture:structural"));
	Units.Emplace(
		TEXT("structure:event-graph"), ESemanticUnitKind::StructureState,
		TEXT("target:blueprint-structure"), TEXT("uai.semantic.blueprint.graph"),
		TEXT("graph_collection"), FString(), MoveTemp(Values),
		TArray<FString>(), TArray<FString>(), FString(), FString(), TArray<FString>(),
		TArray<FString>({TEXT("state")}), TEXT("fixture:structural"),
		StructureAuthoringIdentityKey());
	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	check(TryCompileAuthoringModel(
		TEXT("uai.authoring.composition/2"), 2, TEXT("llm_authoring"),
		TEXT("generation:structural"), MoveTemp(Units),
		{TEXT("target:blueprint-structure")}, Resources, Model, Error));
	return Model.ToSharedRef();
}

FAuthoringModelRef MakeComponentTreeModel(FUeAgentResourceTracker& Resources)
{
	const FString TargetId = TEXT("target:blueprint-components");
	const FString StructureId = TEXT("structure:blueprint-components");
	const FUeAgentCanonicalValueRef Component = ObjectValue(
		{{TEXT("operation"), StringValue(TEXT("create"), Resources)},
			{TEXT("candidate"), ObjectValue(
				{{TEXT("name"), StringValue(TEXT("SceneComponent"), Resources)}},
				Resources)}},
		Resources);
	const FUeAgentCanonicalValueRef Components = ObjectValue(
		{{TEXT("SceneComponent1"), Component}}, Resources);
	TArray<FSemanticUnit> Units;
	Units.Emplace(
		TargetId, ESemanticUnitKind::TargetState, FString(),
		TEXT("ue.blueprint.asset"), TEXT("authoring_target"), FString(),
		TMap<FString, FUeAgentCanonicalValueRef>(), TArray<FString>({StructureId}),
		TArray<FString>(), FString(), FString(), TArray<FString>(),
		TArray<FString>({TEXT("state")}), TEXT("fixture:structural"));
	Units.Emplace(
		StructureId, ESemanticUnitKind::StructureState, TargetId,
		TEXT("uai.semantic.blueprint.component_tree"), TEXT("component_tree"),
		FString(),
		TMap<FString, FUeAgentCanonicalValueRef>({{TEXT("components"), Components}}),
		TArray<FString>(), TArray<FString>(), FString(), FString(), TArray<FString>(),
		TArray<FString>({TEXT("state")}), TEXT("fixture:structural"));
	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	check(TryCompileAuthoringModel(
		TEXT("uai.authoring.composition/2"), 2, TEXT("llm_authoring"),
		TEXT("generation:structural"), MoveTemp(Units), {TargetId},
		Resources, Model, Error));
	return Model.ToSharedRef();
}

FAuthoringModelRef MakeMemberCollectionModel(FUeAgentResourceTracker& Resources)
{
	const FString TargetId = TEXT("target:blueprint-members");
	const FString StructureId = TEXT("structure:blueprint-members");
	const FUeAgentCanonicalValueRef VariableType = ObjectValue(
		{{TEXT("pin_category"), StringValue(TEXT("bool"), Resources)}}, Resources);
	const FUeAgentCanonicalValueRef Variable = ObjectValue(
		{{TEXT("operation"), StringValue(TEXT("create"), Resources)},
			{TEXT("type"), VariableType}},
		Resources);
	const FUeAgentCanonicalValueRef Variables = ObjectValue(
		{{TEXT("bPlayerInside"), Variable}}, Resources);
	TArray<FSemanticUnit> Units;
	Units.Emplace(
		TargetId, ESemanticUnitKind::TargetState, FString(),
		TEXT("ue.blueprint.asset"), TEXT("authoring_target"), FString(),
		TMap<FString, FUeAgentCanonicalValueRef>(), TArray<FString>({StructureId}),
		TArray<FString>(), FString(), FString(), TArray<FString>(),
		TArray<FString>({TEXT("state")}), TEXT("fixture:members"));
	Units.Emplace(
		StructureId, ESemanticUnitKind::StructureState, TargetId,
		TEXT("uai.authoring.surface.member_collection"), TEXT("member_collection"),
		FString(),
		TMap<FString, FUeAgentCanonicalValueRef>({{TEXT("variables"), Variables}}),
		TArray<FString>(), TArray<FString>(), FString(), FString(), TArray<FString>(),
		TArray<FString>({TEXT("state")}), TEXT("fixture:members"));
	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	check(TryCompileAuthoringModel(
		TEXT("uai.authoring.composition/2"), 2, TEXT("llm_authoring"),
		TEXT("generation:members"), MoveTemp(Units), {TargetId},
		Resources, Model, Error));
	return Model.ToSharedRef();
}

FUeAgentCanonicalSnapshotRef MakeStructureSnapshot(FUeAgentResourceTracker& Resources)
{
	const FUeAgentStateRevision Revision(
		TEXT("fixture:structural"), TEXT("generation:structural"), 1);
	const FUeAgentCanonicalIdentity Identity = StructureIdentity();
	const FUeAgentCanonicalValueRef Value = StructureValue(Resources, false);
	TArray<FUeAgentStateEntry> Entries;
	Entries.Emplace(
		Identity,
		FUeAgentCanonicalType(TEXT("object"), EUeAgentValueKind::Object, false, TEXT("object")),
		Value,
		FUeAgentDefaultValue(EUeAgentDefaultSourceKind::PostCreateReadback,
			Identity.GetCanonicalKey(), TEXT("fixture:structural"), Value),
		FUeAgentStateDetails(TEXT("EventGraph"), TEXT(""), {TEXT("Blueprint")}, true,
			TEXT("available"), {}),
		Revision, EUeAgentAuthoredPresence::Present, EUeAgentOverrideState::Overridden);
	FUeAgentCanonicalSnapshotPtr Snapshot;
	FUeAgentStateSemanticError Error;
	check(FUeAgentCanonicalSnapshot::TryCreate(
		TEXT("snapshot:structural"), EUeAgentStatePlane::Persisted, Revision,
		FUeAgentTemporalPoint(TEXT("fixture"), TEXT("1")),
		FUeAgentStateProvenance(
			TEXT("independent_readback"), TEXT("fixture:structural"),
			TEXT("evidence:structural")),
		Entries, Resources, Snapshot, Error));
	return Snapshot.ToSharedRef();
}

FString ClassDefaultAuthoringIdentityKey()
{
	return TEXT("blueprint_class_default_property|blueprint_class_default_property|property_path|")
		TEXT("asset_path=/game/hx013/bp_structural&property_path=spritescale");
}

FUeAgentCanonicalIdentity ClassDefaultIdentity()
{
	const FString OwnerPath =
		TEXT("/Game/HX013/BP_Structural.Default__BP_Structural_C");
	return FUeAgentCanonicalIdentity(
		TEXT("uai.semantic.blueprint.class_default.property"),
		TEXT("blueprint_class_default_property"),
		OwnerPath,
		{{TEXT("asset_path"), TEXT("/Game/HX013/BP_Structural")},
			{TEXT("owner_path"), OwnerPath},
			{TEXT("notification_owner_path"), OwnerPath},
			{TEXT("property_path"), TEXT("SpriteScale")},
			{CanonicalAuthoringTargetIdentitySelector,
				ClassDefaultAuthoringIdentityKey()}},
		TEXT("blueprint_class_default_property"),
		TEXT("generation:class-default"));
}

FAuthoringModelRef MakeClassDefaultModel(
	FUeAgentResourceTracker& Resources, const FString& Value,
	FUeAgentCanonicalValuePtr OverrideValue = nullptr, FUeAgentCanonicalValuePtr ValueDefinition = nullptr,
	const FString& ExplicitKind = FString())
{
	const FString TargetId = TEXT("target:blueprint-class-default");
	const FString PropertyId = TEXT("property:class-default:SpriteScale");
	const FString DefinitionId = TEXT("definition:class-default:double");
	TMap<FString, FUeAgentCanonicalValueRef> PropertyValues;
	if (OverrideValue)
		for (const auto& Field : OverrideValue->GetObjectItems()) PropertyValues.Add(Field.Key, Field.Value);
	else PropertyValues.Add(TEXT("literal"), DecimalValue(Value, Resources));
	TMap<FString, FUeAgentCanonicalValueRef> DefinitionValues({
		{TEXT("type"), ObjectValue({{TEXT("kind"), StringValue(!ExplicitKind.IsEmpty() ? ExplicitKind : ValueDefinition ? TEXT("property_bag") : TEXT("double"), Resources)}}, Resources)}});
	if (ValueDefinition) DefinitionValues.Add(TEXT("value_definition"), ValueDefinition.ToSharedRef());
	TArray<FSemanticUnit> Units;
	Units.Emplace(
		TargetId, ESemanticUnitKind::TargetState, FString(),
		TEXT("ue.blueprint.asset"), TEXT("authoring_target"), FString(),
		TMap<FString, FUeAgentCanonicalValueRef>(),
		TArray<FString>({PropertyId}), TArray<FString>(),
		FString(), FString(), TArray<FString>(), TArray<FString>({TEXT("state")}),
		TEXT("fixture:class-default"));
	Units.Emplace(
		PropertyId, ESemanticUnitKind::PropertySet, TargetId,
		TEXT("uai.semantic.blueprint.class_default.property"),
		TEXT("class_defaults"), TEXT("SpriteScale"),
		MoveTemp(PropertyValues),
		TArray<FString>(), TArray<FString>(), FString(), FString(),
		TArray<FString>({DefinitionId}), TArray<FString>({TEXT("state")}),
		TEXT("fixture:class-default"), ClassDefaultAuthoringIdentityKey());
	Units.Emplace(
		DefinitionId, ESemanticUnitKind::CollectionState, TargetId,
		TEXT("uai.authoring.property_definition/1"), TEXT("property_definition"), DefinitionId,
		MoveTemp(DefinitionValues),
		TArray<FString>(), TArray<FString>(), FString(), FString(), TArray<FString>(),
		TArray<FString>({TEXT("state")}), TEXT("fixture:class-default"));
	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	check(TryCompileAuthoringModel(
		TEXT("uai.authoring.composition/2"), 2, TEXT("llm_authoring"),
		TEXT("generation:class-default"), MoveTemp(Units), {TargetId},
		Resources, Model, Error));
	return Model.ToSharedRef();
}

FUeAgentCanonicalSnapshotRef MakeClassDefaultSnapshot(
	FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr OverrideValue = nullptr)
{
	const FUeAgentStateRevision Revision(
		TEXT("fixture:class-default"), TEXT("generation:class-default"), 1);
	const FUeAgentCanonicalIdentity Identity = ClassDefaultIdentity();
	const FUeAgentCanonicalValueRef Value = OverrideValue ? OverrideValue.ToSharedRef() : ObjectValue(
		{{TEXT("literal"), DecimalValue(TEXT("1.0"), Resources)}}, Resources);
	TArray<FUeAgentStateEntry> Entries;
	Entries.Emplace(
		Identity,
		FUeAgentCanonicalType(TEXT("double"), EUeAgentValueKind::Object, false, TEXT("object")),
		Value,
		FUeAgentDefaultValue(EUeAgentDefaultSourceKind::PostCreateReadback,
			Identity.GetCanonicalKey(), TEXT("fixture:class-default"), Value),
		FUeAgentStateDetails(TEXT("SpriteScale"), TEXT(""), {TEXT("Blueprint")}, true,
			TEXT("available"), {}),
		Revision, EUeAgentAuthoredPresence::Present, EUeAgentOverrideState::Overridden);
	FUeAgentCanonicalSnapshotPtr Snapshot;
	FUeAgentStateSemanticError Error;
	check(FUeAgentCanonicalSnapshot::TryCreate(
		TEXT("snapshot:class-default"), EUeAgentStatePlane::Persisted, Revision,
		FUeAgentTemporalPoint(TEXT("fixture"), TEXT("1")),
		FUeAgentStateProvenance(
			TEXT("independent_readback"), TEXT("fixture:class-default"),
			TEXT("evidence:class-default")),
		Entries, Resources, Snapshot, Error));
	return Snapshot.ToSharedRef();
}

FAuthoringModelRef MakeImportModel(
	FUeAgentResourceTracker& Resources, const FString& ValueA, const FString& ValueB)
{
	const FString FieldId = TEXT("shared.message");
	const FString DefinitionId = TEXT("definition:string");
	TArray<FSemanticUnit> Units;
	Units.Emplace(TEXT("target:blueprint"), ESemanticUnitKind::TargetState, TEXT(""),
		TEXT("ue.blueprint.asset"), TEXT("authoring_target"), TEXT(""),
		TMap<FString, FUeAgentCanonicalValueRef>(),
		TArray<FString>({TEXT("node:a"), TEXT("node:b")}), TArray<FString>(), TEXT(""), TEXT(""),
		TArray<FString>(), TArray<FString>({TEXT("state")}), TEXT("fixture:hx013"),
		TEXT("blueprint_asset|ue.blueprint.asset|asset_path|"
			 "asset_path=%2FGame%2FHX013%2FBP_Import"));
	Units.Emplace(TEXT("node:a"), ESemanticUnitKind::ItemState, TEXT("target:blueprint"),
		TEXT("uai.semantic.blueprint.node"), TEXT("graph_collection"), TEXT(""),
		TMap<FString, FUeAgentCanonicalValueRef>(), TArray<FString>({TEXT("properties:a")}),
		TArray<FString>(), TEXT(""), TEXT(""), TArray<FString>(), TArray<FString>({TEXT("state")}),
		TEXT("fixture:hx013"),
		TEXT("blueprint_node|blueprint_node|node_guid|"
			 "node_guid=00000000000000000000000000000001"));
	Units.Emplace(TEXT("node:b"), ESemanticUnitKind::ItemState, TEXT("target:blueprint"),
		TEXT("uai.semantic.blueprint.node"), TEXT("graph_collection"), TEXT(""),
		TMap<FString, FUeAgentCanonicalValueRef>(), TArray<FString>({TEXT("properties:b")}),
		TArray<FString>(), TEXT(""), TEXT(""), TArray<FString>(), TArray<FString>({TEXT("state")}),
		TEXT("fixture:hx013"),
		TEXT("blueprint_node|blueprint_node|node_guid|"
			 "node_guid=00000000000000000000000000000002"));
	Units.Emplace(TEXT("properties:a"), ESemanticUnitKind::PropertySet, TEXT("node:a"),
		TEXT("uai.semantic.blueprint.node.property"), TEXT("node_properties"), FieldId,
		TMap<FString, FUeAgentCanonicalValueRef>({{FieldId, StringValue(ValueA, Resources)}}),
		TArray<FString>(), TArray<FString>(), TEXT(""), TEXT(""), TArray<FString>({DefinitionId}),
		TArray<FString>({TEXT("state")}), TEXT("fixture:hx013"), TEXT(""));
	Units.Emplace(TEXT("properties:b"), ESemanticUnitKind::PropertySet, TEXT("node:b"),
		TEXT("uai.semantic.blueprint.node.property"), TEXT("node_properties"), FieldId,
		TMap<FString, FUeAgentCanonicalValueRef>({{FieldId, StringValue(ValueB, Resources)}}),
		TArray<FString>(), TArray<FString>(), TEXT(""), TEXT(""), TArray<FString>({DefinitionId}),
		TArray<FString>({TEXT("state")}), TEXT("fixture:hx013"), TEXT(""));
	Units.Emplace(DefinitionId, ESemanticUnitKind::CollectionState, TEXT("target:blueprint"),
		TEXT("uai.authoring.property_definition/1"), TEXT("property_definition"), DefinitionId,
		TMap<FString, FUeAgentCanonicalValueRef>({
			{TEXT("type"), ObjectValue({{TEXT("kind"), StringValue(TEXT("string"), Resources)}}, Resources)}}),
		TArray<FString>(), TArray<FString>(), TEXT(""), TEXT(""), TArray<FString>(),
		TArray<FString>({TEXT("state")}), TEXT("fixture:hx013"), TEXT(""));
	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	check(TryCompileAuthoringModel(TEXT("uai.authoring.composition/2"), 2, TEXT("llm_authoring"),
		TEXT("generation:hx013"), MoveTemp(Units), {TEXT("target:blueprint")}, Resources, Model,
		Error));
	return Model.ToSharedRef();
}

FAuthoringModelRef MakeRootProjectionModel(
	FUeAgentResourceTracker& Resources, const FString& IdentityKey)
{
	const FString DefinitionId = TEXT("definition:string");
	TArray<FSemanticUnit> Units;
	Units.Emplace(TEXT("target:root"), ESemanticUnitKind::TargetState, TEXT(""),
		TEXT("ue.material.asset"), TEXT("authoring_target"), TEXT(""),
		TMap<FString, FUeAgentCanonicalValueRef>(), TArray<FString>({TEXT("property:root")}),
		TArray<FString>(), TEXT(""), TEXT(""), TArray<FString>(), TArray<FString>({TEXT("state")}),
		TEXT("fixture:root-projection"), IdentityKey);
	Units.Emplace(TEXT("property:root"), ESemanticUnitKind::PropertySet, TEXT("target:root"),
		TEXT("uai.semantic.material.property"), TEXT("material_properties"),
		TEXT("bUsedWithWater"),
		TMap<FString, FUeAgentCanonicalValueRef>({
			{TEXT("bUsedWithWater"), StringValue(TEXT("before"), Resources)}}),
		TArray<FString>(), TArray<FString>(), TEXT(""), TEXT(""),
		TArray<FString>({DefinitionId}), TArray<FString>({TEXT("state")}),
		TEXT("fixture:root-projection"), IdentityKey);
	Units.Emplace(DefinitionId, ESemanticUnitKind::CollectionState, TEXT("target:root"),
		TEXT("uai.authoring.property_definition/1"), TEXT("property_definition"), DefinitionId,
		TMap<FString, FUeAgentCanonicalValueRef>({
			{TEXT("type"), ObjectValue({{TEXT("kind"), StringValue(TEXT("string"), Resources)}}, Resources)}}),
		TArray<FString>(), TArray<FString>(), TEXT(""), TEXT(""), TArray<FString>(),
		TArray<FString>({TEXT("state")}), TEXT("fixture:root-projection"), TEXT(""));
	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	check(TryCompileAuthoringModel(TEXT("uai.authoring.composition/2"), 2, TEXT("llm_authoring"),
		TEXT("generation:hx013"), MoveTemp(Units), {TEXT("target:root")}, Resources, Model, Error));
	return Model.ToSharedRef();
}

FUeAgentCanonicalSnapshotRef MakeImportSnapshot(FUeAgentResourceTracker& Resources)
{
	const FUeAgentStateRevision Revision(TEXT("hx013:persisted"), TEXT("generation:hx013"), 13);
	TArray<FUeAgentStateEntry> Entries;
	for (const TPair<FString, FString>& Row :
		{TPair<FString, FString>(TEXT("node:a"), TEXT("before-a")),
			TPair<FString, FString>(TEXT("node:b"), TEXT("before-b"))})
	{
		const FUeAgentCanonicalValueRef Value = StringValue(Row.Value, Resources);
		Entries.Emplace(
			FUeAgentCanonicalIdentity(TEXT("shared.message"), TEXT("authoring_field"), Row.Key,
				TEXT("shared.message"), TEXT("blueprint_node"), TEXT("generation:hx013")),
			FUeAgentCanonicalType(TEXT("string"), EUeAgentValueKind::String, false, TEXT("scalar")),
			Value,
			FUeAgentDefaultValue(EUeAgentDefaultSourceKind::Schema, TEXT("hx013:string-default"),
				TEXT("fixture:hx013"), StringValue(TEXT(""), Resources)),
			FUeAgentStateDetails(
				TEXT("Shared message"), TEXT(""), {TEXT("HX-013")}, true, TEXT("available"), {}),
			Revision, EUeAgentAuthoredPresence::Present, EUeAgentOverrideState::Overridden);
	}
	FUeAgentCanonicalSnapshotPtr Snapshot;
	FUeAgentStateSemanticError Error;
	check(FUeAgentCanonicalSnapshot::TryCreate(TEXT("snapshot:hx013:import"),
		EUeAgentStatePlane::Persisted, Revision, FUeAgentTemporalPoint(TEXT("fixture"), TEXT("13")),
		FUeAgentStateProvenance(
			TEXT("independent_readback"), TEXT("fixture:hx013"), TEXT("evidence:hx013:persisted")),
		Entries, Resources, Snapshot, Error));
	return Snapshot.ToSharedRef();
}

FString SharedActorAuthoringIdentityKey()
{
	return TEXT("level_actor:semantic-target-match");
}

FAuthoringModelRef MakeSemanticTargetMatchModel(
	FUeAgentResourceTracker& Resources, const FString& Label,
	const bool bUpdatedTransform = false)
{
	const FString TargetId = TEXT("target:level-actor");
	const FString StateId = TEXT("property:level-actor-state");
	TArray<FSemanticUnit> Units;
	Units.Emplace(TargetId, ESemanticUnitKind::TargetState, FString(),
		TEXT("uai.semantic.level.actor.target"), TEXT("authoring_target"), FString(),
		TMap<FString, FUeAgentCanonicalValueRef>({
			{TEXT("actor_name"),
				StringValue(TEXT("SemanticTargetActor"), Resources)}}),
		TArray<FString>({StateId}),
		TArray<FString>(), FString(), FString(), TArray<FString>(),
		TArray<FString>({TEXT("state")}), TEXT("fixture:semantic-target"),
		SharedActorAuthoringIdentityKey());
	Units.Emplace(StateId, ESemanticUnitKind::PropertySet, TargetId,
		TEXT("uai.semantic.level.actor"), TEXT("actor_component_properties"),
		TEXT("level_actor"),
		TMap<FString, FUeAgentCanonicalValueRef>({
			{TEXT("label"), StringValue(Label, Resources)},
			{TEXT("transform"), ObjectValue({
				{TEXT("translation"), ArrayValue({
					IntegerValue(bUpdatedTransform ? TEXT("125") : TEXT("7"), Resources),
					IntegerValue(bUpdatedTransform ? TEXT("25") : TEXT("11"), Resources),
					IntegerValue(bUpdatedTransform ? TEXT("50") : TEXT("13"), Resources)},
					Resources)}}, Resources)}}),
		TArray<FString>(), TArray<FString>(), FString(), FString(),
		TArray<FString>(), TArray<FString>({TEXT("state")}),
		TEXT("fixture:semantic-target"), SharedActorAuthoringIdentityKey());
	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	check(TryCompileAuthoringModel(TEXT("uai.authoring.composition/2"), 2,
		TEXT("llm_authoring"), TEXT("generation:semantic-target"),
		MoveTemp(Units), {TargetId}, Resources, Model, Error));
	return Model.ToSharedRef();
}

FUeAgentCanonicalSnapshotRef MakeSemanticTargetMatchSnapshot(
	FUeAgentResourceTracker& Resources)
{
	const FUeAgentStateRevision Revision(
		TEXT("fixture:semantic-target"), TEXT("generation:semantic-target"), 1);
	const TMap<FString, FString> Selectors = {
		{TEXT("actor_name"), TEXT("SemanticTargetActor")},
		{CanonicalAuthoringTargetIdentitySelector,
			SharedActorAuthoringIdentityKey()}};
	const FUeAgentCanonicalIdentity StateIdentity(
		TEXT("uai.semantic.level.actor"), TEXT("level_actor"),
		TEXT("actor-owner"), Selectors, TEXT("level_actor"),
		TEXT("generation:semantic-target"));
	const FUeAgentCanonicalIdentity TargetIdentity(
		TEXT("uai.semantic.level.actor.target"), TEXT("level_actor"),
		TEXT("actor-owner"), Selectors, TEXT("level_actor"),
		TEXT("generation:semantic-target"));
	const FUeAgentCanonicalValueRef BeforeState = ObjectValue(
		{{TEXT("label"), StringValue(TEXT("Before"), Resources)},
			{TEXT("transform"), ObjectValue({
				{TEXT("translation"), ObjectValue({
					{TEXT("components"), ArrayValue({
						DecimalValue(TEXT("7.0"), Resources),
						DecimalValue(TEXT("11.0"), Resources),
						DecimalValue(TEXT("13.0"), Resources)}, Resources)}},
					Resources)}}, Resources)}},
		Resources);
	const FUeAgentCanonicalValueRef TargetValue = ObjectValue(
		{{TEXT("actor_name"), StringValue(TEXT("SemanticTargetActor"), Resources)}},
		Resources);
	TArray<FUeAgentStateEntry> Entries;
	auto AddEntry = [&Entries, &Revision](
		const FUeAgentCanonicalIdentity& Identity,
		const FUeAgentCanonicalValueRef& Value,
		const bool bEditable)
	{
		Entries.Emplace(Identity,
			FUeAgentCanonicalType(
				TEXT("object"), EUeAgentValueKind::Object, false, TEXT("object")),
			Value,
			FUeAgentDefaultValue(EUeAgentDefaultSourceKind::PostCreateReadback,
				Identity.GetCanonicalKey(), TEXT("fixture:semantic-target"), Value),
			FUeAgentStateDetails(TEXT("Level Actor"), TEXT(""),
				{TEXT("Level")}, bEditable, TEXT("available"), {}),
			Revision, EUeAgentAuthoredPresence::Present,
			EUeAgentOverrideState::Overridden);
	};
	AddEntry(StateIdentity, BeforeState, true);
	// Keep the read-only target last: the former identity-only map overwrote the
	// editable state for their shared authoring target identity.
	AddEntry(TargetIdentity, TargetValue, false);
	FUeAgentCanonicalSnapshotPtr Snapshot;
	FUeAgentStateSemanticError Error;
	check(FUeAgentCanonicalSnapshot::TryCreate(
		TEXT("snapshot:semantic-target"), EUeAgentStatePlane::Persisted, Revision,
		FUeAgentTemporalPoint(TEXT("fixture"), TEXT("1")),
		FUeAgentStateProvenance(TEXT("independent_readback"),
			TEXT("fixture:semantic-target"), TEXT("evidence:semantic-target")),
		Entries, Resources, Snapshot, Error));
	return Snapshot.ToSharedRef();
}

FPreparedAuthoringImportPtr MakePreparedProjectionImport(
	FUeAgentResourceTracker& Resources,
	TMap<FString, FString> Selectors,
	EChangeIntent Intent = EChangeIntent::Update,
	TArray<FStructuralEdit> StructuralEdits = {})
{
	const FUeAgentCanonicalIdentity Identity(TEXT("material.property"),
		TEXT("authoring_field"), TEXT("/Game/M_Test.M_Test"), MoveTemp(Selectors),
		TEXT("material_asset"), TEXT("generation:hx013"));
	const FAuthoringModelRef Model =
		MakeRootProjectionModel(Resources, Identity.GetCanonicalKey());
	const FUeAgentStateRevision Revision(TEXT("root:persisted"), TEXT("generation:hx013"), 1);
	const FUeAgentCanonicalValueRef BeforeValue = StringValue(TEXT("before"), Resources);
	TArray<FUeAgentStateEntry> Entries;
	if (Intent != EChangeIntent::Create)
	{
		Entries.Emplace(Identity,
			FUeAgentCanonicalType(TEXT("string"), EUeAgentValueKind::String, false, TEXT("scalar")),
			BeforeValue,
			FUeAgentDefaultValue(EUeAgentDefaultSourceKind::Schema, Identity.GetCanonicalKey(),
				TEXT("fixture:root-projection"), BeforeValue),
			FUeAgentStateDetails(TEXT("Root property"), TEXT(""), {TEXT("RootProperty")}, true,
				TEXT("available"), {}),
			Revision, EUeAgentAuthoredPresence::Present, EUeAgentOverrideState::Overridden);
	}
	FUeAgentCanonicalSnapshotPtr SnapshotPtr;
	FUeAgentStateSemanticError SnapshotError;
	check(FUeAgentCanonicalSnapshot::TryCreate(TEXT("snapshot:root-projection"),
		EUeAgentStatePlane::Persisted, Revision, FUeAgentTemporalPoint(TEXT("fixture"), TEXT("1")),
		FUeAgentStateProvenance(TEXT("independent_readback"), TEXT("fixture:root-projection"),
			TEXT("evidence:root-projection")),
		Entries, Resources, SnapshotPtr, SnapshotError));
	const FUeAgentCanonicalSnapshotRef Snapshot = SnapshotPtr.ToSharedRef();
	FUeAgentCanonicalValuePtr ExpectedBefore;
	FUeAgentCanonicalValuePtr ProposedAfter;
	if (Intent != EChangeIntent::Create)
	{
		ExpectedBefore = BeforeValue;
	}
	if (Intent != EChangeIntent::Delete)
	{
		ProposedAfter = StringValue(TEXT("after"), Resources);
	}
	TArray<FChangeDraft> Changes;
	Changes.Emplace(Identity, Intent, MoveTemp(ExpectedBefore), MoveTemp(ProposedAfter),
		TArray<FString>({TEXT("authoring.write")}),
		TEXT("evidence:hx013:persisted"), TArray<FUeAgentCanonicalIdentity>());
	FChangeSetDraft Draft(
		FUeAgentDescriptorIdentity(TEXT("uai.hjson.semantic.import"), 1,
			TEXT("sha256:3333333333333333333333333333333333333333333333333333333333333333")),
		TEXT("root-projection"), TEXT("root-projection:1"), Snapshot, MoveTemp(Changes));
	FPlanningRequest Request(Draft,
		FUeAgentCallerContext(TEXT("root-projection-test"), {TEXT("authoring.write")},
			TEXT("root-projection-correlation")),
		false);
	FPlanningResult Result;
	FPlanningPreflightError PlanningError;
	check(TryPlanChanges(Request, Resources, Result, PlanningError));
	const FResolvedSemanticWorkspaceRef Workspace = MakeShared<FResolvedSemanticWorkspace>(
		EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State, Model,
		Snapshot->GetRevision(), MoveTemp(StructuralEdits),
		TArray<FString>({TEXT("properties.hjson")}), TEXT(""), TEXT(""), TArray<FString>(),
		Model->GetSemanticFingerprint());
	return MakeShared<FPreparedAuthoringImport>(
		Workspace, MoveTemp(Draft), MoveTemp(Request), MoveTemp(Result));
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceDocumentLayoutProjectionTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace.DocumentLayoutProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceDocumentLayoutProjectionTest::RunTest(const FString&)
{
	FUeAgentResourceTracker Resources = MakeImportTracker();
	FString RegistrationError;
	if (!TestTrue(TEXT("document composition contracts register"),
		RegisterOtherTargetCompositionContracts(RegistrationError))) return false;
	const FString TargetKind = TEXT("ue.control_rig.asset");
	const FString DocumentId = TEXT("uai.semantic.rigging.control_rig.document");
	const FString TargetPath = TEXT("/Game/Fixture/Rig.Rig");
	const FUeAgentStateRevision Revision(TEXT("layout:persisted"), TEXT("layout:generation"), 1);
	auto MakeDocument = [&](const FString& X)
	{
		auto MakeNode = [&](const FString& NodeX)
		{
			return ObjectValue({{TEXT("node_type"), StringValue(TEXT("unit"), Resources)},
				{TEXT("position"), ObjectValue({{TEXT("components"), ArrayValue({
					DecimalValue(NodeX, Resources), DecimalValue(TEXT("144.0"), Resources)},
					Resources)}}, Resources)}}, Resources);
		};
		return ObjectValue({{TEXT("graphs"), ObjectValue({{TEXT("graphs"), ObjectValue({
			{TEXT("RigVMModel"), ObjectValue({{TEXT("nodes"), ObjectValue({
				{TEXT("Moved"), MakeNode(X)}, {TEXT("Unchanged"), MakeNode(TEXT("640.0"))}},
				Resources)}}, Resources)}}, Resources)}}, Resources)}}, Resources);
	};
	const FUeAgentCanonicalValueRef BeforeDocument = MakeDocument(TEXT("240.0"));
	const FUeAgentCanonicalValueRef AfterDocument = MakeDocument(TEXT("320.0"));
	auto MakeEntries = [&](const FUeAgentCanonicalValueRef& Document)
	{
		TArray<FUeAgentStateEntry> Entries;
		for (const bool bDocument : {false, true})
		{
			const FString SemanticId = bDocument ? DocumentId
				: TEXT("uai.semantic.rigging.control_rig.export.target");
			const FUeAgentCanonicalIdentity Identity(SemanticId, TEXT("rigging.control_rig"),
				TargetPath, {{TEXT("asset_path"), TEXT("/Game/Fixture/Rig")}},
				TargetKind, TEXT("layout:generation"));
			const auto Value = bDocument ? Document : ObjectValue({
				{TEXT("asset_path"), StringValue(TEXT("/Game/Fixture/Rig"), Resources)}}, Resources);
			Entries.Emplace(Identity,
				FUeAgentCanonicalType(TEXT("object"), EUeAgentValueKind::Object, false, TEXT("object")),
				Value, FUeAgentDefaultValue(EUeAgentDefaultSourceKind::PostCreateReadback,
					Identity.GetCanonicalKey(), TEXT("fixture:layout"), Value),
				FUeAgentStateDetails(TEXT("Rig document"), TEXT(""), {}, bDocument,
					TEXT("available"), {}), Revision, EUeAgentAuthoredPresence::Present,
				EUeAgentOverrideState::Overridden);
		}
		return Entries;
	};
	const TArray<FUeAgentStateEntry> BeforeEntries = MakeEntries(BeforeDocument);
	FUeAgentCanonicalSnapshotPtr Snapshot;
	FUeAgentStateSemanticError StateError;
	if (!TestTrue(TEXT("document snapshot is valid"), FUeAgentCanonicalSnapshot::TryCreate(
		TEXT("snapshot:layout"), EUeAgentStatePlane::Persisted, Revision,
		FUeAgentTemporalPoint(TEXT("fixture"), TEXT("1")),
		FUeAgentStateProvenance(TEXT("independent_readback"), TargetKind, TEXT("evidence:layout")),
		BeforeEntries, Resources, Snapshot, StateError))) return false;
	auto Compose = [&](TArray<FUeAgentStateEntry> Entries, FAuthoringModelPtr& Model)
	{
		FCanonicalCompositionRequest Request;
		Request.TargetKind = TargetKind;
		Request.ProfileId = TEXT("llm_authoring");
		Request.BaseRevision = TEXT("layout:generation");
		Request.SourceProvenance = TEXT("authoring_state.capture");
		Request.Entries = MoveTemp(Entries);
		FWorkspaceError Error;
		return TryComposeCanonicalAuthoringModel(Request, Resources, Model, Error);
	};
	FAuthoringModelPtr BeforeModel, AfterModel;
	if (!TestTrue(TEXT("before and edited documents use the public composition owner"),
		Compose(BeforeEntries, BeforeModel) && Compose(MakeEntries(AfterDocument), AfterModel))) return false;
	// RigVM node layout is carried by the declared keyed graph collection,
	// not by a separate LayoutState unit. Exercise that public carrier.
	FString EditedCollectionId;
	for (const FSemanticUnit& Unit : BeforeModel->GetUnits())
	{
		const FSemanticUnit* Edited = AfterModel->FindUnit(Unit.GetUnitId());
		if (Unit.GetUnitKind() == ESemanticUnitKind::StructureState && Edited
			&& Unit.GetSemanticTypeRef() == DocumentId + TEXT("#graphs/graphs")
			&& SemanticUnitContentHash(Unit) != SemanticUnitContentHash(*Edited))
		{
			TestTrue(TEXT("only one graph collection changed"), EditedCollectionId.IsEmpty());
			EditedCollectionId = Unit.GetUnitId();
		}
	}
	if (!TestFalse(TEXT("changed layout has its keyed composition identity"), EditedCollectionId.IsEmpty())) return false;
	for (const bool bExplicitCollectionOnly : {true, false})
	{
		const FResolvedSemanticWorkspaceRef Workspace = MakeShared<FResolvedSemanticWorkspace>(
			EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State, AfterModel.ToSharedRef(),
			Revision, TArray<FStructuralEdit>(), TArray<FString>({TEXT("layout.hjson")}),
			TEXT(""), TEXT(""), TArray<FString>(), AfterModel->GetSemanticFingerprint(),
			bExplicitCollectionOnly ? TOptional<TArray<FString>>(TArray<FString>({EditedCollectionId}))
				: TOptional<TArray<FString>>());
		FSemanticWorkspaceChangeProjection Projection;
		FPreparedAuthoringImportPtr Prepared;
		FSemanticWorkspaceImportError Error;
		if (!TestTrue(TEXT("explicit collection and implicit edits use common semantic projection"),
			TryPrepareSemanticWorkspaceChanges(Workspace, BeforeModel.ToSharedRef(), Snapshot.ToSharedRef(),
				FUeAgentDescriptorIdentity(TEXT("layout.projection"), 1, BeforeModel->GetSemanticFingerprint()),
				TEXT("layout"), TEXT("layout:1"), FUeAgentCallerContext(TEXT("layout-test"),
					{TEXT("authoring.write"), TEXT("authoring.structure")}, TEXT("layout")),
				TargetPath, Resources, Projection, Prepared, Error)))
		{
			AddError(Error.Code + TEXT(":") + Error.PublicMessage);
			return false;
		}
		TestEqual(TEXT("layout does not become a root property"), Projection.RootObjectProperties.Num(), 0);
		const auto& Changes = Prepared->GetChangeSetDraft().GetChanges();
		TestEqual(TEXT("one source document change is projected"), Changes.Num(), 1);
		if (Changes.Num() == 1)
		{
			TestEqual(TEXT("document identity is preserved"), Changes[0].GetIdentity().GetSemanticId(), DocumentId);
			FUeAgentSemanticComparison Comparison;
			TestTrue(TEXT("exact source document contains the moved position and unchanged peer"),
				Changes[0].GetProposedAfter().IsValid()
				&& TryCompareValues(Changes[0].GetProposedAfter().ToSharedRef(), AfterDocument,
					TEXT("/"), Resources, Comparison, StateError) && Comparison.IsEquivalent());
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceDependencyRebaseTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace.DependencyRebase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceDependencyRebaseTest::RunTest(const FString& Parameters)
{
	auto Resources = MakeImportTracker();
	const auto Base = MakeImportModel(Resources, TEXT("before-a"), TEXT("before-b"));
	const auto Edit = MakeImportModel(Resources, TEXT("after-a"), TEXT("before-b"));
	const auto Current = MakeImportModel(Resources, TEXT("before-a"), TEXT("external-b"));
	const FUeAgentStateRevision OldRevision(TEXT("rebase"), TEXT("old"), 1);
	const auto Snapshot = MakeImportSnapshot(Resources);
	auto WorkspaceFor = [&](const FAuthoringModelRef& Baseline, const FAuthoringModelRef& Proposed, bool bFull = true,
		const FString& EditedId = TEXT("properties:a"))
	{
		TMap<FString, FString> Hashes;
		TMap<FString, FSemanticUnit> Originals;
		for (const auto& Unit : Baseline->GetUnits())
		{
			Hashes.Add(Unit.GetUnitId(), SemanticUnitRowContentHash(Unit));
			if (bFull) Originals.Add(Unit.GetUnitId(), Unit);
		}
		return FResolvedSemanticWorkspaceRef(MakeShared<FResolvedSemanticWorkspace>(
			EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State, Proposed, OldRevision,
			TArray<FStructuralEdit>(), TArray<FString>({TEXT("property.hjson")}), FString(), FString(), TArray<FString>(),
			Proposed->GetSemanticFingerprint(), TOptional<TArray<FString>>(TArray<FString>{EditedId}),
			TMap<FString, TSet<FString>>(), TMap<FString, FSourceProvenance>(), MoveTemp(Hashes),
			TMap<FString, FHjsonDocumentRef>(), MoveTemp(Originals)));
	};
	auto Source = WorkspaceFor(Base, Edit);
	auto Result = Source;
	FSemanticWorkspaceImportError Error;
	if (!TestTrue(TEXT("independent native change does not discard authored edit"),
		TryRebaseSemanticWorkspace(Source, Current, Snapshot->GetRevision(), Resources, Result, Error)))
	{ AddError(Error.PublicMessage); return false; }
	TestEqual(TEXT("local A retained"), Result->GetModel()->FindUnit(TEXT("properties:a"))->GetValues().FindChecked(TEXT("shared.message"))->GetText(), FString(TEXT("after-a")));
	TestEqual(TEXT("current B retained"), Result->GetModel()->FindUnit(TEXT("properties:b"))->GetValues().FindChecked(TEXT("shared.message"))->GetText(), FString(TEXT("external-b")));
	TestEqual(TEXT("source workspace unchanged"), Source->GetModel()->FindUnit(TEXT("properties:b"))->GetValues().FindChecked(TEXT("shared.message"))->GetText(), FString(TEXT("before-b")));
	TOptional<FChangeSetDraft> Draft;
	TestTrue(TEXT("direct changeset entry uses the same rebase owner"), TryBuildSemanticChangeSetDraft(Source, Snapshot,
		FUeAgentDescriptorIdentity(TEXT("uai.hjson.semantic.import"), 1, TEXT("sha256:1111111111111111111111111111111111111111111111111111111111111111")),
		TEXT("rebase:draft"), TEXT("rebase:draft:1"), Resources, Draft, Error, Current));
	if (Draft) TestEqual(TEXT("only edited field produces a change"), Draft->GetChanges().Num(), 1);
	const auto Conflict = MakeImportModel(Resources, TEXT("external-a"), TEXT("before-b"));
	TestFalse(TEXT("same scalar conflict rejected"), TryRebaseSemanticWorkspace(Source, Conflict, Snapshot->GetRevision(), Resources, Result, Error));
	auto IsDiagnosticString = [](const TSharedPtr<FJsonValue>& Value, const TCHAR* Expected)
	{
		return Value.IsValid() && Value->Type == EJson::String && Value->AsString() == Expected;
	};
	TestTrue(TEXT("diagnostic contains both values and exact field"), Error.Diagnostics.ContainsByPredicate(
		[&](const UE::UAI::Contract::FDiagnostic& Issue)
		{
			return Issue.SemanticPointer == TEXT("/shared.message")
				&& IsDiagnosticString(Issue.ExpectedValue, TEXT("before-a"))
				&& IsDiagnosticString(Issue.ActualValue, TEXT("external-a"))
				&& IsDiagnosticString(Issue.ProposedValue, TEXT("after-a"));
		}));
	TestTrue(TEXT("convergent same edit accepted"), TryRebaseSemanticWorkspace(Source, Edit, Snapshot->GetRevision(), Resources, Result, Error));
	TestFalse(TEXT("successful rebase clears earlier conflict"), Error.IsValid());
	auto Alter = [&](const FAuthoringModelRef& Model, const FString& Id, TMap<FString, FUeAgentCanonicalValueRef> Values)
	{
		TArray<FSemanticUnit> Units;
		for (const auto& Unit : Model->GetUnits()) Units.Add(Unit.GetUnitId() == Id ? Unit.WithValues(Values) : Unit);
		FAuthoringModelPtr Changed;
		FWorkspaceError CompileError;
		check(TryCompileAuthoringModel(Model->GetContractId(), Model->GetContractVersion(), Model->GetProfileId(),
			Model->GetBaseRevision(), MoveTemp(Units), Model->GetTargetUnitIds(), Resources, Changed, CompileError));
		return Changed.ToSharedRef();
	};
	const auto ChangedType = Alter(Current, TEXT("definition:string"), {{TEXT("type"),
		ObjectValue({{TEXT("kind"), StringValue(TEXT("double"), Resources)}}, Resources)}});
	TestFalse(TEXT("changed dependency type is not silently converted"), TryRebaseSemanticWorkspace(Source, ChangedType,
		Snapshot->GetRevision(), Resources, Result, Error));
	auto HasTypeDifference = [&]()
	{
		return Error.Diagnostics.ContainsByPredicate([&](const UE::UAI::Contract::FDiagnostic& Issue)
		{
			return Issue.Code == TEXT("workspace_dependency_changed") && Issue.SemanticPointer == TEXT("/type/kind")
				&& IsDiagnosticString(Issue.ExpectedValue, TEXT("string")) && IsDiagnosticString(Issue.ActualValue, TEXT("double"));
		});
	};
	TestTrue(TEXT("type difference diagnosed"), HasTypeDifference());
	TArray<FSemanticUnit> RetypedUnits;
	for (const auto& Unit : ChangedType->GetUnits())
	{
		auto Dependencies = Unit.GetDependencyRefs();
		for (FString& Ref : Dependencies) if (Ref == TEXT("definition:string")) Ref = TEXT("definition:double");
		const FString Id = Unit.GetUnitId() == TEXT("definition:string") ? TEXT("definition:double") : Unit.GetUnitId();
		RetypedUnits.Emplace(Id, Unit.GetUnitKind(), Unit.GetOwnerRef(), Unit.GetSemanticTypeRef(), Unit.GetRole(),
			Unit.GetFieldSetRef() == TEXT("definition:string") ? TEXT("definition:double") : Unit.GetFieldSetRef(),
			Unit.GetValues(), Unit.GetItemRefs(), Unit.GetRelationRefs(), Unit.GetOrderingRef(), Unit.GetLayoutRef(),
			MoveTemp(Dependencies), Unit.GetProfileVisibility(), Unit.GetSourceProvenance(), Unit.GetTargetIdentityKey(), Unit.GetCategoryPath());
	}
	FAuthoringModelPtr RetypedModel;
	FWorkspaceError RetypedError;
	check(TryCompileAuthoringModel(Base->GetContractId(), Base->GetContractVersion(), Base->GetProfileId(),
		Base->GetBaseRevision(), MoveTemp(RetypedUnits), Base->GetTargetUnitIds(), Resources, RetypedModel, RetypedError));
	TestFalse(TEXT("new definition identity is a type conflict"), TryRebaseSemanticWorkspace(Source, RetypedModel.ToSharedRef(),
		Snapshot->GetRevision(), Resources, Result, Error));
	TestTrue(TEXT("new definition identity still reports old and current type, not hashes"),
		HasTypeDifference());
	const auto InventoryEdit = Alter(Base, TEXT("target:blueprint"), {{TEXT("label"), StringValue(TEXT("edited"), Resources)}});
	const auto InventoryNow = Alter(Base, TEXT("node:b"), {{TEXT("native_state"), StringValue(TEXT("rebuilt"), Resources)}});
	if (TestTrue(TEXT("unchanged child inventory is not a transitive value dependency"), TryRebaseSemanticWorkspace(
		WorkspaceFor(Base, InventoryEdit, true, TEXT("target:blueprint")), InventoryNow,
		Snapshot->GetRevision(), Resources, Result, Error)))
	{
		TestEqual(TEXT("authored parent field retained"), Result->GetModel()->FindUnit(TEXT("target:blueprint"))->GetValues().FindChecked(TEXT("label"))->GetText(), FString(TEXT("edited")));
		TestEqual(TEXT("native unedited child retained"), Result->GetModel()->FindUnit(TEXT("node:b"))->GetValues().FindChecked(TEXT("native_state"))->GetText(), FString(TEXT("rebuilt")));
	}
	auto ObjectModel = [&](const FAuthoringModelRef& Model, TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields)
	{
		TMap<FString, FUeAgentCanonicalValueRef> Values;
		for (const auto& Field : Fields) Values.Add(Field.Key, Field.Value);
		return Alter(Model, TEXT("properties:a"), MoveTemp(Values));
	};
	const auto V1 = IntegerValue(TEXT("1"), Resources), V2 = IntegerValue(TEXT("2"), Resources), V3 = IntegerValue(TEXT("3"), Resources);
	const auto RecordModel = Alter(Base, TEXT("definition:string"), {{TEXT("type"), ObjectValue({
		{TEXT("kind"), StringValue(TEXT("object"), Resources)}}, Resources)}});
	const auto RecordBase = ObjectModel(RecordModel, {{TEXT("tracks"), ArrayValue({V1}, Resources)}, {TEXT("metadata"), ArrayValue({V1}, Resources)}});
	const auto RecordEdit = ObjectModel(RecordModel, {{TEXT("tracks"), ArrayValue({V2}, Resources)}, {TEXT("metadata"), ArrayValue({V1}, Resources)}});
	const auto RecordNow = ObjectModel(RecordModel, {{TEXT("tracks"), ArrayValue({V1}, Resources)}, {TEXT("metadata"), ArrayValue({V2}, Resources)}});
	if (TestTrue(TEXT("document members merge without replaying an unchanged collection"), TryRebaseSemanticWorkspace(
		WorkspaceFor(RecordBase, RecordEdit), RecordNow, Snapshot->GetRevision(), Resources, Result, Error)))
	{
		const auto* Unit = Result->GetModel()->FindUnit(TEXT("properties:a"));
		TestEqual(TEXT("authored collection retained"), Unit->GetValues().FindChecked(TEXT("tracks"))->GetArrayItems()[0]->GetText(), FString(TEXT("2")));
		TestEqual(TEXT("native peer collection retained"), Unit->GetValues().FindChecked(TEXT("metadata"))->GetArrayItems()[0]->GetText(), FString(TEXT("2")));
	}
	const auto RecordConflict = ObjectModel(RecordModel, {{TEXT("tracks"), ArrayValue({V3}, Resources)}, {TEXT("metadata"), ArrayValue({V1}, Resources)}});
	TestFalse(TEXT("concurrent changes inside one undeclared collection remain a conflict"), TryRebaseSemanticWorkspace(
		WorkspaceFor(RecordBase, RecordEdit), RecordConflict, Snapshot->GetRevision(), Resources, Result, Error));
	const auto IntegerDescriptor = ObjectValue({{TEXT("type"), ObjectValue({{TEXT("kind"), StringValue(TEXT("integer"), Resources)}}, Resources)}}, Resources);
	const auto StructModel = Alter(Base, TEXT("definition:string"), {
		{TEXT("type"), ObjectValue({{TEXT("kind"), StringValue(TEXT("struct"), Resources)}}, Resources)},
		{TEXT("value_definition"), ObjectValue({{TEXT("fields"), ObjectValue({
			{TEXT("keep"), IntegerDescriptor}, {TEXT("remove"), IntegerDescriptor},
			{TEXT("other"), IntegerDescriptor}, {TEXT("added"), IntegerDescriptor}}, Resources)}}, Resources)}});
	auto StructValueModel = [&](TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields)
	{
		return ObjectModel(StructModel, {{TEXT("fields"), ObjectValue(MoveTemp(Fields), Resources)}});
	};
	const auto ObjectBase = StructValueModel({{TEXT("keep"), V1}, {TEXT("remove"), V1}, {TEXT("other"), V1}});
	const auto ObjectEdit = StructValueModel({{TEXT("keep"), V2}, {TEXT("other"), V1}, {TEXT("added"), V3}});
	const auto ObjectNow = StructValueModel({{TEXT("keep"), V1}, {TEXT("remove"), V1}, {TEXT("other"), V2}});
	const auto ObjectSource = WorkspaceFor(ObjectBase, ObjectEdit);
	if (!TestTrue(TEXT("independent declared struct edits merge with sparse preservation"), TryRebaseSemanticWorkspace(ObjectSource, ObjectNow,
		Snapshot->GetRevision(), Resources, Result, Error))) { AddError(Error.PublicMessage); return false; }
	const auto Merged = Result->GetModel()->FindUnit(TEXT("properties:a"))->GetValues().FindChecked(TEXT("fields"));
	TestNotNull(TEXT("omitted struct member preserves captured value"), ObjectField(Merged, TEXT("remove")));
	TestEqual(TEXT("native sibling update preserved"), (*ObjectField(Merged, TEXT("other")))->GetText(), FString(TEXT("2")));
	TestEqual(TEXT("authored addition preserved"), (*ObjectField(Merged, TEXT("added")))->GetText(), FString(TEXT("3")));
	// Niagara-style ItemState schemas declare an unwrapped record under inputs.
	// Exercise the common contract rather than adding a domain-specific merger.
	const auto ItemSchema = Alter(RecordModel, TEXT("definition:string"), {
		{TEXT("type"), ObjectValue({{TEXT("kind"), StringValue(TEXT("object"), Resources)}}, Resources)},
		{TEXT("value_definition"), ObjectValue({{TEXT("fields"), ObjectValue({
			{TEXT("inputs"), ObjectValue({{TEXT("fields"), ObjectValue({
				{TEXT("left"), IntegerDescriptor}, {TEXT("right"), IntegerDescriptor}}, Resources)}}, Resources)}}, Resources)}}, Resources)}});
	auto ItemModel = [&](const FAuthoringModelRef& Schema, const FUeAgentCanonicalValueRef& Left, const FUeAgentCanonicalValueRef& Right)
	{
		const auto Model = ObjectModel(Schema, {{TEXT("inputs"), ObjectValue({{TEXT("left"), Left}, {TEXT("right"), Right}}, Resources)}});
		TArray<FSemanticUnit> Units;
		for (const auto& Unit : Model->GetUnits())
		{
			if (Unit.GetUnitId() != TEXT("properties:a")) { Units.Add(Unit); continue; }
			Units.Emplace(Unit.GetUnitId(), ESemanticUnitKind::ItemState, Unit.GetOwnerRef(), Unit.GetSemanticTypeRef(),
				Unit.GetRole(), Unit.GetFieldSetRef(), Unit.GetValues(), Unit.GetItemRefs(), Unit.GetRelationRefs(),
				Unit.GetOrderingRef(), Unit.GetLayoutRef(), Unit.GetDependencyRefs(), Unit.GetProfileVisibility(),
				Unit.GetSourceProvenance(), Unit.GetTargetIdentityKey(), Unit.GetCategoryPath());
		}
		FAuthoringModelPtr ResultModel;
		FWorkspaceError CompileError;
		check(TryCompileAuthoringModel(Model->GetContractId(), Model->GetContractVersion(), Model->GetProfileId(),
			Model->GetBaseRevision(), MoveTemp(Units), Model->GetTargetUnitIds(), Resources, ResultModel, CompileError));
		return ResultModel.ToSharedRef();
	};
	const auto ItemBase = ItemModel(ItemSchema, V1, V1), ItemEdit = ItemModel(ItemSchema, V2, V1);
	if (TestTrue(TEXT("declared ItemState direct record merges independent inputs"), TryRebaseSemanticWorkspace(
		WorkspaceFor(ItemBase, ItemEdit), ItemModel(ItemSchema, V1, V3), Snapshot->GetRevision(), Resources, Result, Error)))
	{
		const auto Inputs = Result->GetModel()->FindUnit(TEXT("properties:a"))->GetValues().FindChecked(TEXT("inputs"));
		TestEqual(TEXT("ItemState authored input survives"), (*ObjectField(Inputs, TEXT("left")))->GetText(), FString(TEXT("2")));
		TestEqual(TEXT("ItemState native input survives"), (*ObjectField(Inputs, TEXT("right")))->GetText(), FString(TEXT("3")));
	}
	TestFalse(TEXT("ItemState concurrent change to same input remains a conflict"), TryRebaseSemanticWorkspace(
		WorkspaceFor(ItemBase, ItemEdit), ItemModel(ItemSchema, V3, V1), Snapshot->GetRevision(), Resources, Result, Error));
	TestFalse(TEXT("undeclared ItemState does not acquire guessed record semantics"), TryRebaseSemanticWorkspace(
		WorkspaceFor(ItemModel(RecordModel, V1, V1), ItemModel(RecordModel, V2, V1)), ItemModel(RecordModel, V1, V3),
		Snapshot->GetRevision(), Resources, Result, Error));
	auto AggregateModel = [&](const FUeAgentCanonicalValueRef& Items, const FUeAgentCanonicalValueRef& NativeContext)
	{
		const auto Model = ObjectModel(RecordModel, {{TEXT("smart_object_items"), Items}, {TEXT("native_context"), NativeContext}});
		TArray<FSemanticUnit> Units;
		for (const auto& Unit : Model->GetUnits())
		{
			if (Unit.GetUnitId() != TEXT("properties:a")) { Units.Add(Unit); continue; }
			Units.Emplace(Unit.GetUnitId(), ESemanticUnitKind::StructureState, Unit.GetOwnerRef(), TEXT("fixture.smart_object.definition"),
				Unit.GetRole(), Unit.GetFieldSetRef(), Unit.GetValues(), Unit.GetItemRefs(), Unit.GetRelationRefs(),
				Unit.GetOrderingRef(), Unit.GetLayoutRef(), Unit.GetDependencyRefs(), Unit.GetProfileVisibility(),
				Unit.GetSourceProvenance(), Unit.GetTargetIdentityKey(), Unit.GetCategoryPath());
		}
		FAuthoringModelPtr ResultModel; FWorkspaceError CompileError;
		check(TryCompileAuthoringModel(Model->GetContractId(), Model->GetContractVersion(), Model->GetProfileId(),
			Model->GetBaseRevision(), MoveTemp(Units), Model->GetTargetUnitIds(), Resources, ResultModel, CompileError));
		return ResultModel.ToSharedRef();
	};
	const auto AggregateBase = AggregateModel(V1, V1), AggregateEdit = AggregateModel(V2, V1);
	if (TestTrue(TEXT("aggregate root retains unedited native context"), TryRebaseSemanticWorkspace(
		WorkspaceFor(AggregateBase, AggregateEdit), AggregateModel(V1, V3), Snapshot->GetRevision(), Resources, Result, Error)))
		TestEqual(TEXT("aggregate context uses current value"), Result->GetModel()->FindUnit(TEXT("properties:a"))->GetValues().FindChecked(TEXT("native_context"))->GetText(), FString(TEXT("3")));
	TestFalse(TEXT("concurrent aggregate membership remains atomic"), TryRebaseSemanticWorkspace(
		WorkspaceFor(AggregateBase, AggregateEdit), AggregateModel(V3, V1), Snapshot->GetRevision(), Resources, Result, Error));
	auto DocumentModel = [&](const FAuthoringModelRef& Model)
	{
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
		for (const auto& Pair : Model->FindUnit(TEXT("properties:a"))->GetValues()) Fields.Emplace(Pair.Key, Pair.Value);
		return Alter(Model, TEXT("properties:a"), {{TEXT("value"), ObjectValue(MoveTemp(Fields), Resources)}});
	};
	TestTrue(TEXT("document property value wrapper follows the same declared type"), TryRebaseSemanticWorkspace(
		WorkspaceFor(DocumentModel(ObjectBase), DocumentModel(ObjectEdit)), DocumentModel(ObjectNow), Snapshot->GetRevision(), Resources, Result, Error));
	const auto Literal1 = ObjectValue({{TEXT("literal"), V1}}, Resources);
	const auto Literal2 = ObjectValue({{TEXT("literal"), V2}}, Resources);
	const auto NativeBase = StructValueModel({{TEXT("keep"), Literal1}, {TEXT("other"), Literal1}});
	const auto CompactEdit = StructValueModel({{TEXT("keep"), V2}, {TEXT("other"), V1}});
	const auto NativeNow = StructValueModel({{TEXT("keep"), Literal1}, {TEXT("other"), Literal2}});
	if (TestTrue(TEXT("compact nested values restore before three-way comparison"), TryRebaseSemanticWorkspace(
		WorkspaceFor(NativeBase, CompactEdit), NativeNow, Snapshot->GetRevision(), Resources, Result, Error)))
	{
		const auto Fields = Result->GetModel()->FindUnit(TEXT("properties:a"))->GetValues().FindChecked(TEXT("fields"));
		TestEqual(TEXT("local compact edit survives"), (*ObjectField(*ObjectField(Fields, TEXT("keep")), TEXT("literal")))->GetText(), FString(TEXT("2")));
		TestEqual(TEXT("native sibling survives compact unchanged input"), (*ObjectField(*ObjectField(Fields, TEXT("other")), TEXT("literal")))->GetText(), FString(TEXT("2")));
	}
	const auto ArrayModel = Alter(Base, TEXT("definition:string"), {{TEXT("type"), ObjectValue({
		{TEXT("kind"), StringValue(TEXT("array"), Resources)}, {TEXT("element_type"), *ObjectField(IntegerDescriptor, TEXT("type"))}}, Resources)}});
	const auto ArrayBase = ObjectModel(ArrayModel, {{TEXT("items"), ArrayValue({V1, V1}, Resources)}});
	const auto ArrayEdit = ObjectModel(ArrayModel, {{TEXT("items"), ArrayValue({V2, V1}, Resources)}});
	const auto ArrayNow = ObjectModel(ArrayModel, {{TEXT("items"), ArrayValue({V1, V2}, Resources)}});
	TestFalse(TEXT("ordered array identity is never guessed"), TryRebaseSemanticWorkspace(WorkspaceFor(ArrayBase, ArrayEdit),
		ArrayNow, Snapshot->GetRevision(), Resources, Result, Error));
	{
		const auto CapturedRows = ArrayValue({
			ObjectValue({{TEXT("name"), StringValue(TEXT("A"), Resources)}, {TEXT("kept"), V1}}, Resources),
			ObjectValue({{TEXT("name"), StringValue(TEXT("Z"), Resources)}, {TEXT("kept"), V2}}, Resources)}, Resources);
		const auto RequestedRows = ArrayValue({
			ObjectValue({{TEXT("name"), StringValue(TEXT("Z"), Resources)}}, Resources),
			ObjectValue({{TEXT("name"), StringValue(TEXT("A"), Resources)}}, Resources)}, Resources);
		const auto Document = ObjectModel(RecordModel, {{TEXT("records"), CapturedRows}});
		FUeAgentCanonicalValuePtr Restored;
		FUeAgentStateSemanticError RestoreError;
		if (TestTrue(TEXT("ordinary document array retains its original partial rows"), TryRestoreSemanticPropertyValue(
			*Document->FindUnit(TEXT("properties:a")), *Document,
			ObjectValue({{TEXT("records"), CapturedRows}}, Resources), ObjectValue({{TEXT("records"), RequestedRows}}, Resources),
			Resources, Restored, RestoreError)))
		{
			const auto Rows = *ObjectField(Restored.ToSharedRef(), TEXT("records"));
			TestNull(TEXT("reordered record does not inherit fields from the old index"), ObjectField(Rows->GetArrayItems()[0], TEXT("kept")));
			TestEqual(TEXT("original requested record order is retained"), (*ObjectField(Rows->GetArrayItems()[0], TEXT("name")))->GetText(), FString(TEXT("Z")));
		}
		const auto GeneratedRows = ArrayValue({ObjectValue({{TEXT("kept"), V1}}, Resources), RequestedRows->GetArrayItems()[1]}, Resources);
		if (TestTrue(TEXT("generated identity stays absent until its native owner"), TryRestoreSemanticPropertyValue(
			*Document->FindUnit(TEXT("properties:a")), *Document,
			ObjectValue({{TEXT("records"), CapturedRows}}, Resources), ObjectValue({{TEXT("records"), GeneratedRows}}, Resources),
			Resources, Restored, RestoreError)))
			TestNull(TEXT("omitted name is not filled from a captured position"), ObjectField((*ObjectField(Restored.ToSharedRef(), TEXT("records")))->GetArrayItems()[0], TEXT("name")));
	}
	const auto ReferenceModel = Alter(Base, TEXT("definition:string"), {{TEXT("type"), ObjectValue({
		{TEXT("kind"), StringValue(TEXT("object_ref"), Resources)}}, Resources)}});
	auto ReferenceValueModel = [&](const TCHAR* ObjectPath, const FUeAgentCanonicalValueRef& Property)
	{
		return ObjectModel(ReferenceModel, {{TEXT("path"), StringValue(ObjectPath, Resources)},
			{TEXT("owned_object"), ObjectValue({{TEXT("properties"), ArrayValue({Property}, Resources)}}, Resources)}});
	};
	const auto ReferenceBase = ReferenceValueModel(TEXT("/Old"), V1);
	const auto ReferenceEdit = ReferenceValueModel(TEXT("/Old"), V2);
	const auto ReferenceNow = ReferenceValueModel(TEXT("/New"), V1);
	TestFalse(TEXT("owned fields cannot be moved to a different object reference"), TryRebaseSemanticWorkspace(
		WorkspaceFor(ReferenceBase, ReferenceEdit), ReferenceNow, Snapshot->GetRevision(), Resources, Result, Error));
	const auto UnknownBase = ObjectModel(Base, {{TEXT("left"), V1}, {TEXT("right"), V1}});
	const auto UnknownEdit = ObjectModel(Base, {{TEXT("left"), V2}, {TEXT("right"), V1}});
	const auto UnknownNow = ObjectModel(Base, {{TEXT("left"), V1}, {TEXT("right"), V2}});
	TestFalse(TEXT("object shape never substitutes for a declared struct type"), TryRebaseSemanticWorkspace(
		WorkspaceFor(UnknownBase, UnknownEdit), UnknownNow, Snapshot->GetRevision(), Resources, Result, Error));
	TestTrue(TEXT("legacy hash-only baseline can still retain independent edits"), TryRebaseSemanticWorkspace(WorkspaceFor(Base, Edit, false),
		Current, Snapshot->GetRevision(), Resources, Result, Error));
	TestFalse(TEXT("missing baseline cannot authorize concurrent overwrite"), TryRebaseSemanticWorkspace(WorkspaceFor(Base, Edit, false),
		Conflict, Snapshot->GetRevision(), Resources, Result, Error));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceImportModuleContractTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace.ModuleContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceImportModuleContractTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::Composition::HjsonImport;

	TestEqual(TEXT("state, seed and edit bundle share one workspace schema"),
		GetSemanticWorkspaceSchemaId(), FString(TEXT("uai.authoring.workspace/2")));
	TestEqual(TEXT("state mode has one stable spelling"),
		LexToString(ESemanticWorkspaceMode::State), FString(TEXT("state")));
	TestEqual(TEXT("seed mode has one stable spelling"), LexToString(ESemanticWorkspaceMode::Seed),
		FString(TEXT("seed")));
	TestEqual(TEXT("edit bundle mode has one stable spelling"),
		LexToString(ESemanticWorkspaceMode::EditBundle), FString(TEXT("edit_bundle")));
	TestTrue(TEXT("resolved semantic workspace is a native C5 contract"),
		TIsClass<FResolvedSemanticWorkspace>::Value);
	TestTrue(TEXT("structural edit is explicit and typed"), TIsClass<FStructuralEdit>::Value);
	TestTrue(TEXT("prepared import is a composition contract"),
		TIsClass<FPreparedAuthoringImport>::Value);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceImportChangeSetReassemblyTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace."
	"ChangeSetReassembly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceImportChangeSetReassemblyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::Composition::HjsonImport;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker Resources = MakeImportTracker();
	const FAuthoringModelRef CurrentModel =
		MakeImportModel(Resources, TEXT("before-a"), TEXT("before-b"));
	const FAuthoringModelRef EditedModel =
		MakeImportModel(Resources, TEXT("after-a"), TEXT("after-b"));
	const FUeAgentCanonicalSnapshotRef Snapshot = MakeImportSnapshot(Resources);
	const FResolvedSemanticWorkspaceRef Workspace =
		MakeShared<FResolvedSemanticWorkspace>(EWorkspaceSourceKind::Directory,
			ESemanticWorkspaceMode::State, EditedModel, Snapshot->GetRevision(),
			TArray<FStructuralEdit>(), TArray<FString>({TEXT("units/property/properties-a.hjson")}),
			TEXT(""), TEXT(""), TArray<FString>(), EditedModel->GetSemanticFingerprint(),
			TOptional<TArray<FString>>(TArray<FString>({TEXT("properties:a")})));
	TOptional<FChangeSetDraft> Draft;
	FSemanticWorkspaceImportError Error;
	TestTrue(TEXT("semantic property units reassemble into one typed ChangeSet"),
		TryBuildSemanticChangeSetDraft(Workspace, Snapshot,
			FUeAgentDescriptorIdentity(TEXT("uai.hjson.semantic.import"), 1,
				TEXT("sha256:"
					 "111111111111111111111111111111111111"
					 "1111111111111111111111111111")),
			TEXT("hx013:change-set"), TEXT("hx013:change-set:1"), Resources, Draft, Error));
	TestTrue(TEXT("ChangeSet is present"), Draft.IsSet());
	if (Draft.IsSet())
	{
		TestEqual(TEXT("unselected changed sibling with the same field id is omitted"),
			Draft->GetChanges().Num(), 1);
		if (Draft->GetChanges().Num() == 1)
		{
			const FChangeDraft& Change = Draft->GetChanges()[0];
			TestEqual(TEXT("owner disambiguates duplicate field id"),
				Change.GetIdentity().GetOwnerId(), FString(TEXT("node:a")));
			TestEqual(TEXT("edited value is preserved"), Change.GetProposedAfter()->GetText(),
				FString(TEXT("after-a")));
		}
	}

	TOptional<FChangeSetDraft> NoOpDraft;
	FSemanticWorkspaceImportError NoOpError;
	const FResolvedSemanticWorkspaceRef NoOpWorkspace =
		MakeShared<FResolvedSemanticWorkspace>(EWorkspaceSourceKind::Directory,
			ESemanticWorkspaceMode::State, CurrentModel, Snapshot->GetRevision(),
			TArray<FStructuralEdit>(), TArray<FString>({TEXT("units/property/properties-a.hjson")}),
			TEXT(""), TEXT(""), TArray<FString>(), CurrentModel->GetSemanticFingerprint());
	TestFalse(TEXT("omission and unchanged values are not mutations"),
		TryBuildSemanticChangeSetDraft(NoOpWorkspace, Snapshot,
			FUeAgentDescriptorIdentity(TEXT("uai.hjson.semantic.import"), 1,
				TEXT("sha256:"
					 "111111111111111111111111111111111111"
					 "1111111111111111111111111111")),
			TEXT("hx013:no-op"), TEXT("hx013:no-op:1"), Resources, NoOpDraft, NoOpError));
	TestEqual(TEXT("no-op failure is explicit"), NoOpError.Code,
		FString(TEXT("hjson_import_no_explicit_changes")));
	TestFalse(TEXT("failed no-op has no draft"), NoOpDraft.IsSet());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceImportSemanticTargetMatchTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace."
	"SemanticTargetMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceImportSemanticTargetMatchTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::Composition::HjsonImport;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker Resources = MakeImportTracker();
	const FAuthoringModelRef EditedModel =
		MakeSemanticTargetMatchModel(Resources, TEXT("After"));
	const FUeAgentCanonicalSnapshotRef Snapshot =
		MakeSemanticTargetMatchSnapshot(Resources);
	const FResolvedSemanticWorkspaceRef Workspace =
		MakeShared<FResolvedSemanticWorkspace>(
			EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State,
			EditedModel, Snapshot->GetRevision(), TArray<FStructuralEdit>(),
			TArray<FString>({TEXT("actors/SemanticTargetActor/properties.hjson")}),
			TEXT(""), TEXT(""), TArray<FString>(),
			EditedModel->GetSemanticFingerprint(),
			TOptional<TArray<FString>>(
				TArray<FString>({TEXT("property:level-actor-state")})));
	TOptional<FChangeSetDraft> Draft;
	FSemanticWorkspaceImportError Error;
	const FUeAgentDescriptorIdentity Descriptor(
		TEXT("uai.hjson.semantic.import"), 1,
		TEXT("sha256:") + FString::ChrN(64, TEXT('4')));
	TestTrue(TEXT("semantic type and object identity select the editable state"),
		TryBuildSemanticChangeSetDraft(Workspace, Snapshot, Descriptor,
			TEXT("semantic-target-match"), TEXT("semantic-target-match:1"),
			Resources, Draft, Error));
	TestTrue(TEXT("the semantic target change set is present"), Draft.IsSet());
	if (Draft.IsSet())
	{
		TestEqual(TEXT("only the editable state changes"),
			Draft->GetChanges().Num(), 1);
		if (Draft->GetChanges().Num() == 1)
		{
			const FChangeDraft& Change = Draft->GetChanges()[0];
			TestEqual(TEXT("the read-only target cannot replace the state"),
				Change.GetIdentity().GetSemanticId(),
				FString(TEXT("uai.semantic.level.actor")));
			const FUeAgentCanonicalValueRef* Label =
				ObjectField(Change.GetProposedAfter().ToSharedRef(), TEXT("label"));
			TestTrue(TEXT("the edited state value is preserved"),
				Label != nullptr && (*Label)->GetText() == TEXT("After"));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceImportCompactWrapperRoundTripTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace."
	"CompactWrapperRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceImportPropertyValueShapeTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace.PropertyValueShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceImportPropertyValueShapeTest::RunTest(const FString&)
{
	FUeAgentResourceTracker Resources = MakeImportTracker();
	const FUeAgentStateRevision Revision(TEXT("fixture:value-shape"), TEXT("generation:value-shape"), 1);
	const FString IdentityKey = TEXT("fixture:value-shape:owner");
	const FString TargetId = TEXT("target:value-shape");
	const auto BeforeArray = ArrayValue({StringValue(TEXT("old"), Resources)}, Resources);
	const auto AfterArray = ArrayValue({StringValue(TEXT("new"), Resources), StringValue(TEXT("peer"), Resources)}, Resources);
	TArray<FUeAgentStateEntry> Entries;
	TArray<FSemanticUnit> Units;
	TArray<FString> PropertyIds;
	TMap<FString, FUeAgentCanonicalValueRef> Expected;
	auto AddCase = [&](const FString& Name, const FUeAgentCanonicalValueRef& Before,
		TMap<FString, FUeAgentCanonicalValueRef> EditedValues, const FUeAgentCanonicalValueRef& After)
	{
		const FString SemanticId = TEXT("fixture.property.") + Name;
		const FString UnitId = TEXT("property:") + Name;
		const FUeAgentCanonicalIdentity Identity(SemanticId, TEXT("fixture_owner"), TEXT("owner"),
			{{CanonicalAuthoringTargetIdentitySelector, IdentityKey}}, TEXT("fixture_owner"), Revision.GetGeneration());
		const FString Container = Before->GetKind() == EUeAgentValueKind::Array ? TEXT("array")
			: Before->GetKind() == EUeAgentValueKind::Object ? TEXT("object") : TEXT("scalar");
		Entries.Emplace(Identity, FUeAgentCanonicalType(Name, Before->GetKind(), false, Container), Before,
			FUeAgentDefaultValue(EUeAgentDefaultSourceKind::PostCreateReadback, Identity.GetCanonicalKey(), TEXT("fixture"), Before),
			FUeAgentStateDetails(Name, TEXT(""), {}, true, TEXT("available"), {}), Revision,
			EUeAgentAuthoredPresence::Present, EUeAgentOverrideState::Overridden);
		Units.Emplace(UnitId, ESemanticUnitKind::PropertySet, TargetId, SemanticId, TEXT("target_settings"), Name,
			MoveTemp(EditedValues), TArray<FString>(), TArray<FString>(), FString(), FString(), TArray<FString>(),
			TArray<FString>({TEXT("state")}), TEXT("fixture:value-shape"), IdentityKey);
		PropertyIds.Add(UnitId);
		Expected.Add(SemanticId, After);
	};
	AddCase(TEXT("array"), BeforeArray, {{TEXT("value"), AfterArray}}, AfterArray);
	// A business object containing only "value" must not be mistaken for the
	// composition wrapper used by a non-object semantic value.
	AddCase(TEXT("object"), ObjectValue({{TEXT("value"), BeforeArray}}, Resources),
		{{TEXT("value"), AfterArray}}, ObjectValue({{TEXT("value"), AfterArray}}, Resources));
	// Native property explicit-presence envelopes retain their established
	// meaning even though their captured semantic value is a scalar.
	const auto AfterScalar = DecimalValue(TEXT("0.75"), Resources);
	AddCase(TEXT("native"), DecimalValue(TEXT("0.25"), Resources),
		{{TEXT("presence"), StringValue(TEXT("explicit"), Resources)}, {TEXT("value"), AfterScalar}}, AfterScalar);
	Units.Emplace(TargetId, ESemanticUnitKind::TargetState, FString(), TEXT("ue.generic.object"), TEXT("authoring_target"),
		FString(), TMap<FString, FUeAgentCanonicalValueRef>(), PropertyIds, TArray<FString>(), FString(), FString(),
		TArray<FString>(), TArray<FString>({TEXT("state")}), TEXT("fixture:value-shape"), IdentityKey);
	FAuthoringModelPtr Model;
	FWorkspaceError ModelError;
	if (!TestTrue(TEXT("property shape fixture compiles"), TryCompileAuthoringModel(TEXT("uai.authoring.composition/2"), 2,
		TEXT("llm_authoring"), TEXT("generation:value-shape"), MoveTemp(Units), {TargetId}, Resources, Model, ModelError))) return false;
	FUeAgentCanonicalSnapshotPtr Snapshot;
	FUeAgentStateSemanticError StateError;
	if (!TestTrue(TEXT("property shape snapshot is valid"), FUeAgentCanonicalSnapshot::TryCreate(TEXT("snapshot:value-shape"),
		EUeAgentStatePlane::Persisted, Revision, FUeAgentTemporalPoint(TEXT("fixture"), TEXT("1")),
		FUeAgentStateProvenance(TEXT("independent_readback"), TEXT("fixture"), TEXT("evidence:value-shape")),
		Entries, Resources, Snapshot, StateError))) { AddError(StateError.Code + TEXT(":") + StateError.PublicMessage); return false; }
	const FResolvedSemanticWorkspaceRef Workspace = MakeShared<FResolvedSemanticWorkspace>(EWorkspaceSourceKind::Directory,
		ESemanticWorkspaceMode::State, Model.ToSharedRef(), Revision, TArray<FStructuralEdit>(),
		TArray<FString>({TEXT("properties.hjson")}), TEXT(""), TEXT(""), TArray<FString>(), Model->GetSemanticFingerprint());
	TOptional<FChangeSetDraft> Draft;
	FSemanticWorkspaceImportError Error;
	if (!TestTrue(TEXT("public semantic import restores all property value shapes"), TryBuildSemanticChangeSetDraft(Workspace,
		Snapshot.ToSharedRef(), FUeAgentDescriptorIdentity(TEXT("uai.hjson.semantic.import"), 1,
			TEXT("sha256:") + FString::ChrN(64, TEXT('6'))), TEXT("value-shape"), TEXT("value-shape:1"), Resources, Draft, Error)))
	{ AddError(Error.Code + TEXT(":") + Error.PublicMessage); return false; }
	if (!TestTrue(TEXT("all three edits produce a draft"), Draft.IsSet() && Draft->GetChanges().Num() == 3)) return false;
	for (const auto& Change : Draft->GetChanges())
	{
		const auto* Value = Expected.Find(Change.GetIdentity().GetSemanticId());
		FUeAgentSemanticComparison Comparison;
		TestTrue(*Change.GetIdentity().GetSemanticId(), Value && Change.GetProposedAfter().IsValid()
			&& TryCompareValues(Change.GetProposedAfter().ToSharedRef(), *Value, TEXT("/"), Resources, Comparison, StateError)
			&& Comparison.IsEquivalent());
	}
	return !HasAnyErrors();
}

bool FUeAgentSemanticWorkspaceImportCompactWrapperRoundTripTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::Composition::HjsonImport;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker Resources = MakeImportTracker();
	const FAuthoringModelRef EditedModel =
		MakeSemanticTargetMatchModel(Resources, TEXT("Before"), true);
	const FUeAgentCanonicalSnapshotRef Snapshot =
		MakeSemanticTargetMatchSnapshot(Resources);
	const FResolvedSemanticWorkspaceRef Workspace =
		MakeShared<FResolvedSemanticWorkspace>(
			EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State,
			EditedModel, Snapshot->GetRevision(), TArray<FStructuralEdit>(),
			TArray<FString>({TEXT("actors/SemanticTargetActor/properties.hjson")}),
			TEXT(""), TEXT(""), TArray<FString>(),
			EditedModel->GetSemanticFingerprint(),
			TOptional<TArray<FString>>(
				TArray<FString>({TEXT("property:level-actor-state")})));
	TOptional<FChangeSetDraft> Draft;
	FSemanticWorkspaceImportError Error;
	const FUeAgentDescriptorIdentity Descriptor(
		TEXT("uai.hjson.semantic.import"), 1,
		TEXT("sha256:") + FString::ChrN(64, TEXT('5')));
	TestTrue(TEXT("compact direct values restore the captured wrapper shape"),
		TryBuildSemanticChangeSetDraft(Workspace, Snapshot, Descriptor,
			TEXT("compact-wrapper"), TEXT("compact-wrapper:1"),
			Resources, Draft, Error));
	TestTrue(TEXT("the compact wrapper change set is present"), Draft.IsSet());
	if (Draft.IsSet())
	{
		TestEqual(TEXT("only the transform state changes"),
			Draft->GetChanges().Num(), 1);
		if (Draft->GetChanges().Num() == 1)
		{
			const FUeAgentCanonicalValueRef Proposed =
				Draft->GetChanges()[0].GetProposedAfter().ToSharedRef();
			const FUeAgentCanonicalValueRef* Transform =
				ObjectField(Proposed, TEXT("transform"));
			const FUeAgentCanonicalValueRef* Translation = Transform
				? ObjectField(*Transform, TEXT("translation")) : nullptr;
			const FUeAgentCanonicalValueRef* Components = Translation
				? ObjectField(*Translation, TEXT("components")) : nullptr;
			TestTrue(TEXT("the components wrapper is restored"),
				Components != nullptr
				&& (*Components)->GetKind() == EUeAgentValueKind::Array
				&& (*Components)->GetArrayItems().Num() == 3);
			if (Components != nullptr
				&& (*Components)->GetKind() == EUeAgentValueKind::Array
				&& (*Components)->GetArrayItems().Num() == 3)
			{
				TestEqual(TEXT("visible integers restore decimal component kinds"),
					(*Components)->GetArrayItems()[0]->GetKind(),
					EUeAgentValueKind::Decimal);
				TestEqual(TEXT("the edited translation is preserved"),
					(*Components)->GetArrayItems()[0]->GetLexeme(),
					FString(TEXT("125")));
			}
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceImportOwnedIntentTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace.OwnedIntent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentSemanticWorkspaceImportOwnedIntentTest::RunTest(const FString&)
{
	auto Resources = MakeImportTracker();
	FHjsonDocumentPtr Baseline;
	FWorkspaceError Error;
	FUeAgentCanonicalValuePtr Captured;
	if (!TryParseHjsonDocument(TEXT("{path:null}"), TEXT("baseline.hjson"), Resources, Baseline, Error)
		|| !TryProjectHjsonCanonicalValue(*Baseline->GetRoot(), Resources, Captured, Error)) return false;
	const auto Snapshot = MakeClassDefaultSnapshot(Resources, Captured);
	for (bool bOwned : {true, false})
	{
		const FString RowText = bOwned
			? TEXT("{type:object_ref,value:{owned_object:{name:\"Native Owned\",properties:{}}}}")
			: TEXT("{type:object_ref,value:\"/Game/Reference.Reference\"}");
		FHjsonDocumentPtr Row;
		TMap<FString, FUeAgentCanonicalValueRef> Values;
		if (!TestTrue(TEXT("ordinary reference row parses through the public resolver"),
			TryParseHjsonDocument(RowText, TEXT("property.hjson"), Resources, Row, Error)
			&& TryResolveKeyedPropertyValues(*Row->GetRoot(), TEXT("object_ref"), {TEXT("path")}, Resources, Values, Error))) return false;
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
		for (const auto& Field : Values) Fields.Add(Field);
		const auto Resolved = ObjectValue(Fields, Resources);
		TestTrue(TEXT("owned intent and path shorthand have distinct native shapes"),
			bOwned ? Values.Contains(TEXT("owned_object")) && !Values.Contains(TEXT("path"))
			: Values.Contains(TEXT("path")) && Values[TEXT("path")]->GetText() == TEXT("/Game/Reference.Reference"));
		const auto Model = MakeClassDefaultModel(Resources, TEXT(""), Resolved, nullptr, TEXT("object_ref"));
		const FResolvedSemanticWorkspaceRef Workspace = MakeShared<FResolvedSemanticWorkspace>(
			EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State, Model,
			Snapshot->GetRevision(), TArray<FStructuralEdit>(), TArray<FString>({TEXT("properties.hjson")}),
			TEXT(""), TEXT(""), TArray<FString>(), Model->GetSemanticFingerprint());
		TOptional<FChangeSetDraft> Draft;
		FSemanticWorkspaceImportError ImportError;
		if (!TestTrue(TEXT("resolved ordinary reference builds a change draft"), TryBuildSemanticChangeSetDraft(
			Workspace, Snapshot, FUeAgentDescriptorIdentity(TEXT("uai.hjson.semantic.import"), 1,
				TEXT("sha256:") + FString::ChrN(64, TEXT('5'))), TEXT("owned-intent"), TEXT("owned-intent:1"),
			Resources, Draft, ImportError))) return false;
		if (!TestTrue(TEXT("one native property intention remains"), Draft.IsSet() && Draft->GetChanges().Num() == 1)) return false;
		FUeAgentSemanticComparison Comparison;
		FUeAgentStateSemanticError CompareError;
		TestTrue(TEXT("overlay preserves owned creation and ordinary reference paths exactly"),
			TryCompareValues(Draft->GetChanges()[0].GetProposedAfter().ToSharedRef(), Resolved,
				TEXT("/value"), Resources, Comparison, CompareError) && Comparison.IsEquivalent());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceImportPropertyBagSchemaTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace.PropertyBagSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceImportPropertyBagSchemaTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FUeAgentResourceTracker Resources = MakeImportTracker();
	const auto Empty = ObjectValue({}, Resources);
	const auto SpeedDesc = ObjectValue({{TEXT("name"), StringValue(TEXT("Speed"), Resources)},
		{TEXT("kind"), StringValue(TEXT("float"), Resources)}}, Resources);
	const auto CountDesc = ObjectValue({{TEXT("name"), StringValue(TEXT("Count"), Resources)},
		{TEXT("kind"), StringValue(TEXT("int"), Resources)}}, Resources);
	const auto Schema = ArrayValue({SpeedDesc, CountDesc}, Resources);
	const auto Fields = ObjectValue({{TEXT("Speed"), DecimalValue(TEXT("240.75"), Resources)},
		{TEXT("Count"), IntegerValue(TEXT("3"), Resources)}}, Resources);
	const auto Captured = ObjectValue({{TEXT("schema"), Schema}, {TEXT("fields"), Fields}}, Resources);
	const auto Definition = ObjectValue({{TEXT("builtin"), StringValue(TEXT("property_bag"), Resources)}}, Resources);
	const auto Snapshot = MakeClassDefaultSnapshot(Resources, Captured);
	auto CheckOverlay = [&](const TCHAR* Label, const FUeAgentCanonicalValueRef& Proposed,
		const FUeAgentCanonicalValueRef& Expected, bool bTyped = true)
	{
		const auto Model = MakeClassDefaultModel(Resources, TEXT(""), Proposed,
			bTyped ? FUeAgentCanonicalValuePtr(Definition) : FUeAgentCanonicalValuePtr());
		const FResolvedSemanticWorkspaceRef Workspace = MakeShared<FResolvedSemanticWorkspace>(
			EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State, Model,
			Snapshot->GetRevision(), TArray<FStructuralEdit>(), TArray<FString>({TEXT("properties.hjson")}),
			TEXT(""), TEXT(""), TArray<FString>(), Model->GetSemanticFingerprint());
		TOptional<FChangeSetDraft> Draft;
		FSemanticWorkspaceImportError Error;
		if (!TestTrue(Label, TryBuildSemanticChangeSetDraft(Workspace, Snapshot,
			FUeAgentDescriptorIdentity(TEXT("uai.hjson.semantic.import"), 1,
				TEXT("sha256:") + FString::ChrN(64, TEXT('5'))),
			TEXT("property-bag-schema"), TEXT("property-bag-schema:1"), Resources, Draft, Error))) return;
		if (!TestTrue(TEXT("one property change is available"), Draft.IsSet() && Draft->GetChanges().Num() == 1)) return;
		FUeAgentSemanticComparison Comparison;
		FUeAgentStateSemanticError CompareError;
		TestTrue(Label, TryCompareValues(Draft->GetChanges()[0].GetProposedAfter().ToSharedRef(),
			Expected, TEXT("/value"), Resources, Comparison, CompareError) && Comparison.IsEquivalent());
	};
	const auto Cleared = ObjectValue({{TEXT("schema"), ArrayValue({}, Resources)}, {TEXT("fields"), Empty}}, Resources);
	CheckOverlay(TEXT("explicit empty schema removes every captured field"), Cleared, Cleared);
	const auto CountSchema = ArrayValue({CountDesc}, Resources);
	CheckOverlay(TEXT("schema deletion preserves an omitted value only for a retained descriptor"),
		ObjectValue({{TEXT("schema"), CountSchema}}, Resources),
		ObjectValue({{TEXT("schema"), CountSchema}, {TEXT("fields"), ObjectValue({
			{TEXT("Count"), IntegerValue(TEXT("3"), Resources)}}, Resources)}}, Resources));
	const auto Unknown = ObjectValue({{TEXT("schema"), ArrayValue({}, Resources)},
		{TEXT("fields"), ObjectValue({{TEXT("Unknown"), IntegerValue(TEXT("7"), Resources)}}, Resources)}}, Resources);
	CheckOverlay(TEXT("unknown explicit fields remain available for codec rejection"), Unknown, Unknown);
	CheckOverlay(TEXT("omitting schema still permits sparse field edits"),
		ObjectValue({{TEXT("fields"), ObjectValue({{TEXT("Count"), IntegerValue(TEXT("5"), Resources)}}, Resources)}}, Resources),
		ObjectValue({{TEXT("schema"), Schema}, {TEXT("fields"), ObjectValue({
			{TEXT("Speed"), DecimalValue(TEXT("240.75"), Resources)},
			{TEXT("Count"), IntegerValue(TEXT("5"), Resources)}}, Resources)}}, Resources));
	const auto ChangedType = ObjectValue({{TEXT("schema"), ArrayValue({ObjectValue({
		{TEXT("name"), StringValue(TEXT("Speed"), Resources)},
		{TEXT("kind"), StringValue(TEXT("int"), Resources)}}, Resources)}, Resources)},
		{TEXT("fields"), ObjectValue({{TEXT("Speed"), IntegerValue(TEXT("6"), Resources)}}, Resources)}}, Resources);
	CheckOverlay(TEXT("changed descriptor does not inherit the old decimal representation"), ChangedType, ChangedType);
	CheckOverlay(TEXT("untyped objects retain their ordinary sparse merge contract"), Cleared,
		ObjectValue({{TEXT("schema"), ArrayValue({}, Resources)}, {TEXT("fields"), Fields}}, Resources), false);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceImportKeyedPropertyBoundaryTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace.KeyedPropertyBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentSemanticWorkspaceImportKeyedPropertyBoundaryTest::RunTest(const FString&)
{
	auto Resources = MakeImportTracker();
	const auto Empty = ObjectValue({}, Resources);
	const auto Record = ObjectValue({{TEXT("setting"), IntegerValue(TEXT("1"), Resources)}}, Resources);
	const auto Members = ObjectValue({{TEXT("Keep"), Record}, {TEXT("Remove"), Record}}, Resources);
	const auto Captured = ObjectValue({{TEXT("members"), Members}, {TEXT("settings"), Record}}, Resources);
	const auto Boundary = ObjectValue({{TEXT("collection_semantics"), StringValue(TEXT("keyed_desired_state"), Resources)}}, Resources);
	const auto Definition = ObjectValue({{TEXT("fields"), ObjectValue({
		{TEXT("members"), Boundary}, {TEXT("settings"), Empty}}, Resources)}}, Resources);
	const auto Snapshot = MakeClassDefaultSnapshot(Resources, Captured);
	auto Check = [&](const TCHAR* Label, const FUeAgentCanonicalValueRef& Proposed,
		const FUeAgentCanonicalValueRef& Expected, bool bDeclared = true)
	{
		const auto Model = MakeClassDefaultModel(Resources, TEXT(""), Proposed,
			bDeclared ? FUeAgentCanonicalValuePtr(Definition) : FUeAgentCanonicalValuePtr(), TEXT("object"));
		const FResolvedSemanticWorkspaceRef Workspace = MakeShared<FResolvedSemanticWorkspace>(
			EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State, Model,
			Snapshot->GetRevision(), TArray<FStructuralEdit>(), TArray<FString>({TEXT("properties.hjson")}),
			TEXT(""), TEXT(""), TArray<FString>(), Model->GetSemanticFingerprint());
		TOptional<FChangeSetDraft> Draft;
		FSemanticWorkspaceImportError Error;
		if (!TestTrue(Label, TryBuildSemanticChangeSetDraft(Workspace, Snapshot,
			FUeAgentDescriptorIdentity(TEXT("uai.hjson.semantic.import"), 1,
				TEXT("sha256:") + FString::ChrN(64, TEXT('5'))),
			TEXT("keyed-property"), TEXT("keyed-property:1"), Resources, Draft, Error)))
		{
			AddError(Error.Code + TEXT(":") + Error.PublicMessage);
			return;
		}
		if (!TestTrue(TEXT("one property change"), Draft.IsSet() && Draft->GetChanges().Num() == 1)) return;
		FUeAgentSemanticComparison Comparison;
		FUeAgentStateSemanticError CompareError;
		TestTrue(Label, TryCompareValues(Draft->GetChanges()[0].GetProposedAfter().ToSharedRef(),
			Expected, TEXT("/value"), Resources, Comparison, CompareError) && Comparison.IsEquivalent());
	};
	const auto Desired = ObjectValue({{TEXT("Keep"), Empty}, {TEXT("Added"), Empty}}, Resources);
	Check(TEXT("membership deletion and uncompleted retained record reach the keyed owner"),
		ObjectValue({{TEXT("members"), Desired}, {TEXT("settings"), Empty}}, Resources),
		ObjectValue({{TEXT("members"), Desired}, {TEXT("settings"), Record}}, Resources));
	Check(TEXT("empty membership clears the collection"), ObjectValue({{TEXT("members"), Empty}}, Resources),
		ObjectValue({{TEXT("members"), Empty}, {TEXT("settings"), Record}}, Resources));
	const auto EditedSettings = ObjectValue({{TEXT("setting"), IntegerValue(TEXT("2"), Resources)}}, Resources);
	Check(TEXT("omitted collection preserves its capture"), ObjectValue({{TEXT("settings"), EditedSettings}}, Resources),
		ObjectValue({{TEXT("members"), Members}, {TEXT("settings"), EditedSettings}}, Resources));
	Check(TEXT("undeclared ordinary map retains sparse membership"),
		ObjectValue({{TEXT("members"), Desired}}, Resources),
		ObjectValue({{TEXT("members"), ObjectValue({{TEXT("Keep"), Record}, {TEXT("Remove"), Record}, {TEXT("Added"), Empty}}, Resources)},
			{TEXT("settings"), Record}}, Resources), false);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceTypedEditProjectionTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace.TypedEditProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentSemanticWorkspaceTypedEditProjectionTest::RunTest(const FString&)
{
	auto Resources = MakeImportTracker();
	const FString PropertyId = TEXT("property:class-default:SpriteScale");
	auto Literal = [&](const FUeAgentCanonicalValueRef& Value) { return ObjectValue({{TEXT("literal"), Value}}, Resources); };
	auto Check = [&](const TCHAR* Label, const FString& Kind, const FUeAgentCanonicalValueRef& Before,
		const FUeAgentCanonicalValueRef& After, bool bChanged, FUeAgentCanonicalValuePtr Definition = nullptr)
	{
		const auto Base = MakeClassDefaultModel(Resources, TEXT(""), Before, Definition, Kind);
		const auto Edit = MakeClassDefaultModel(Resources, TEXT(""), After, Definition, Kind);
		const auto Snapshot = MakeClassDefaultSnapshot(Resources, Before);
		const FResolvedSemanticWorkspaceRef Workspace = MakeShared<FResolvedSemanticWorkspace>(
			EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State, Edit, Snapshot->GetRevision(),
			TArray<FStructuralEdit>(), TArray<FString>({TEXT("properties.hjson")}), TEXT(""), TEXT(""),
			TArray<FString>(), Edit->GetSemanticFingerprint());
		TOptional<FChangeSetDraft> Draft; FSemanticWorkspaceImportError Error;
		const bool Built = TryBuildSemanticChangeSetDraft(Workspace, Snapshot,
			FUeAgentDescriptorIdentity(TEXT("uai.hjson.semantic.import"), 1, TEXT("sha256:") + FString::ChrN(64, TEXT('5'))),
			TEXT("typed-edit"), TEXT("typed-edit:1"), Resources, Draft, Error);
		TestEqual(Label, Built, bChanged);
		if (!bChanged) TestEqual(TEXT("typed equivalent input is a draft no-op"), Error.Code, FString(TEXT("hjson_import_no_explicit_changes")));
		TSet<FString> Fields; FWorkspaceError FieldError;
		if (TestTrue(TEXT("document field delta uses the same declared comparison"), TryCollectChangedUnitValueFields(
			Base->FindUnit(PropertyId), *Edit->FindUnit(PropertyId), Resources, Fields, FieldError, &Edit.Get())))
			TestEqual(Label, !Fields.IsEmpty(), bChanged);
		if (bChanged && Draft)
		{
			FPlanningRequest Request(*Draft, FUeAgentCallerContext(TEXT("typed-edit"), {TEXT("authoring.write")}, TEXT("typed-edit")), false);
			FPlanningResult Result; FPlanningPreflightError PlanningError;
			if (!TestTrue(TEXT("typed edit prepares"), TryPlanChanges(Request, Resources, Result, PlanningError))) return;
			FPreparedAuthoringImport Prepared(Workspace, *Draft, MoveTemp(Request), MoveTemp(Result));
			TArray<TSharedPtr<FJsonValue>> Properties;
			if (!TestTrue(TEXT("typed edit projects"), TryProjectRootObjectPropertyChanges(Prepared,
				TEXT("/Game/HX013/BP_Structural.Default__BP_Structural_C"), Properties, Error))) return;
			if (Kind == TEXT("struct") && TestEqual(TEXT("one struct descriptor"), Properties.Num(), 1))
			{
				const auto Members = Properties[0]->AsObject()->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"));
				TestEqual(TEXT("equivalent typed sibling is not replayed"), Members->Values.Num(), 1);
				TestTrue(TEXT("actual member edit remains"), Members->HasField(TEXT("Count")));
			}
		}
	};
	Check(TEXT("FName case is not a workspace edit"), TEXT("name"), Literal(StringValue(TEXT("NativeName"), Resources)), Literal(StringValue(TEXT("nativename"), Resources)), false);
	Check(TEXT("same float storage is not a workspace edit"), TEXT("float"), Literal(DecimalValue(TEXT("0.10000000149011612"), Resources)), Literal(DecimalValue(TEXT("0.1"), Resources)), false);
	Check(TEXT("double precision remains significant"), TEXT("double"), Literal(DecimalValue(TEXT("0.10000000149011612"), Resources)), Literal(DecimalValue(TEXT("0.1"), Resources)), true);
	Check(TEXT("ordinary string case remains significant"), TEXT("string"), Literal(StringValue(TEXT("NativeName"), Resources)), Literal(StringValue(TEXT("nativename"), Resources)), true);
	auto Descriptor = [&](const TCHAR* Kind) { return ObjectValue({{TEXT("type"), ObjectValue({{TEXT("kind"), StringValue(Kind, Resources)}}, Resources)}}, Resources); };
	const auto Definition = ObjectValue({{TEXT("fields"), ObjectValue({{TEXT("Name"), Descriptor(TEXT("name"))}, {TEXT("Count"), Descriptor(TEXT("int32"))}}, Resources)}}, Resources);
	Check(TEXT("struct delta removes equivalent typed siblings"), TEXT("struct"),
		ObjectValue({{TEXT("fields"), ObjectValue({{TEXT("Name"), Literal(StringValue(TEXT("NativeName"), Resources))}, {TEXT("Count"), Literal(IntegerValue(TEXT("1"), Resources))}}, Resources)}}, Resources),
		ObjectValue({{TEXT("fields"), ObjectValue({{TEXT("Name"), Literal(StringValue(TEXT("nativename"), Resources))}, {TEXT("Count"), Literal(IntegerValue(TEXT("2"), Resources))}}, Resources)}}, Resources), true, Definition);
	const auto Base = MakeClassDefaultModel(Resources, TEXT(""), Literal(StringValue(TEXT("NativeName"), Resources)), nullptr, TEXT("name"));
	const auto Edit = MakeClassDefaultModel(Resources, TEXT(""), Literal(StringValue(TEXT("nativename"), Resources)), nullptr, TEXT("name"));
	const auto Now = MakeClassDefaultModel(Resources, TEXT(""), Literal(StringValue(TEXT("ExternalName"), Resources)), nullptr, TEXT("name"));
	TMap<FString, FString> Hashes; TMap<FString, FSemanticUnit> Originals;
	for (const auto& Unit : Base->GetUnits()) { Hashes.Add(Unit.GetUnitId(), SemanticUnitRowContentHash(Unit)); Originals.Add(Unit.GetUnitId(), Unit); }
	const auto Snapshot = MakeClassDefaultSnapshot(Resources);
	const FResolvedSemanticWorkspaceRef Source = MakeShared<FResolvedSemanticWorkspace>(
		EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State, Edit, Snapshot->GetRevision(),
		TArray<FStructuralEdit>(), TArray<FString>({TEXT("properties.hjson")}), FString(), FString(), TArray<FString>(),
		Edit->GetSemanticFingerprint(), TOptional<TArray<FString>>(TArray<FString>{PropertyId}),
		TMap<FString, TSet<FString>>(), TMap<FString, FSourceProvenance>(), MoveTemp(Hashes),
		TMap<FString, FHjsonDocumentRef>(), MoveTemp(Originals));
	auto Rebased = Source; FSemanticWorkspaceImportError RebaseError;
	if (TestTrue(TEXT("equivalent authored FName does not conflict with a real external edit"), TryRebaseSemanticWorkspace(
		Source, Now, FUeAgentStateRevision(TEXT("typed:new"), TEXT("generation:class-default"), 2), Resources, Rebased, RebaseError)))
		TestEqual(TEXT("rebase preserves real external value"), Rebased->GetModel()->FindUnit(PropertyId)->GetValues().FindChecked(TEXT("literal"))->GetText(), FString(TEXT("ExternalName")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceImportCompactNumericRoundTripTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace."
	"CompactNumericRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceImportCompactNumericRoundTripTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::Composition::HjsonImport;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker Resources = MakeImportTracker();
	const FUeAgentStateRevision Revision(TEXT("compact:persisted"), TEXT("generation:compact"), 1);
	const FUeAgentCanonicalIdentity ReadOnlyIdentity(TEXT("level.property"),
		TEXT("authoring_field"), TEXT("component:light"), TEXT("CustomDepthStencilValue"),
		TEXT("level_component"), TEXT("generation:compact"));
	const FUeAgentCanonicalIdentity EditableIdentity(TEXT("level.property"),
		TEXT("authoring_field"), TEXT("component:light"), TEXT("Mobility"), TEXT("level_component"),
		TEXT("generation:compact"));
	const FUeAgentCanonicalValueRef CurrentReadOnly =
		ObjectValue({{TEXT("literal"), DecimalValue(TEXT("0"), Resources)}}, Resources);
	const FUeAgentCanonicalValueRef CurrentEditable = ObjectValue(
		{{TEXT("enum_path"), StringValue(TEXT("/Script/Engine.EComponentMobility"), Resources)},
			{TEXT("numeric_value"), IntegerValue(TEXT("1"), Resources)},
			{TEXT("selected"), StringValue(TEXT("Stationary"), Resources)}},
		Resources);
	TArray<FUeAgentStateEntry> Entries;
	auto AddEntry = [&Entries, &Revision, &Resources](const FUeAgentCanonicalIdentity& Identity,
						const FUeAgentCanonicalValueRef& Value, const bool bEditable)
	{
		Entries.Emplace(Identity,
			FUeAgentCanonicalType(TEXT("object"), EUeAgentValueKind::Object, false, TEXT("object")),
			Value,
			FUeAgentDefaultValue(EUeAgentDefaultSourceKind::PostCreateReadback,
				Identity.GetCanonicalKey(), TEXT("fixture:compact"), Value),
			FUeAgentStateDetails(Identity.GetSelector(), TEXT(""), {TEXT("Level")}, bEditable,
				TEXT("available"), {}),
			Revision, EUeAgentAuthoredPresence::Present, EUeAgentOverrideState::Overridden);
	};
	AddEntry(ReadOnlyIdentity, CurrentReadOnly, false);
	AddEntry(EditableIdentity, CurrentEditable, true);
	FUeAgentCanonicalSnapshotPtr Snapshot;
	FUeAgentStateSemanticError StateError;
	check(FUeAgentCanonicalSnapshot::TryCreate(TEXT("snapshot:compact"),
		EUeAgentStatePlane::Persisted, Revision, FUeAgentTemporalPoint(TEXT("fixture"), TEXT("1")),
		FUeAgentStateProvenance(
			TEXT("independent_readback"), TEXT("fixture:compact"), TEXT("evidence:compact")),
		Entries, Resources, Snapshot, StateError));

	TArray<FSemanticUnit> Units;
	Units.Emplace(TEXT("target:level"), ESemanticUnitKind::TargetState, TEXT(""),
		TEXT("ue.level.asset"), TEXT("authoring_target"), TEXT(""),
		TMap<FString, FUeAgentCanonicalValueRef>(),
		TArray<FString>({TEXT("property:readonly"), TEXT("property:mobility")}), TArray<FString>(),
		TEXT(""), TEXT(""), TArray<FString>(), TArray<FString>({TEXT("state")}),
		TEXT("fixture:compact"),
		TEXT("level_asset|ue.level.asset|asset_path|asset_path=%2FGame%2FL"));
	auto AddProperty = [&Units](const FString& UnitId, const FString& Field,
						   const FUeAgentCanonicalValueRef& Value, const FString& IdentityKey)
	{
		TMap<FString, FUeAgentCanonicalValueRef> Values;
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Item : Value->GetObjectItems())
		{
			Values.Add(Item.Key, Item.Value);
		}
		Units.Emplace(UnitId, ESemanticUnitKind::PropertySet, TEXT("target:level"),
			TEXT("level.property"), TEXT("actor_component_hierarchy"), Field, MoveTemp(Values),
			TArray<FString>(), TArray<FString>(), TEXT(""), TEXT(""), TArray<FString>(),
			TArray<FString>({TEXT("state")}), TEXT("fixture:compact"), IdentityKey);
	};
	AddProperty(TEXT("property:readonly"), TEXT("CustomDepthStencilValue"),
		ObjectValue({{TEXT("literal"), IntegerValue(TEXT("0"), Resources)}}, Resources),
		ReadOnlyIdentity.GetCanonicalKey());
	AddProperty(TEXT("property:mobility"), TEXT("Mobility"),
		ObjectValue({{TEXT("selected"), StringValue(TEXT("Movable"), Resources)}}, Resources),
		EditableIdentity.GetCanonicalKey());
	FAuthoringModelPtr Model;
	FWorkspaceError WorkspaceError;
	check(TryCompileAuthoringModel(TEXT("uai.authoring.composition/2"), 2, TEXT("llm_authoring"),
		TEXT("generation:compact"), MoveTemp(Units), {TEXT("target:level")}, Resources, Model,
		WorkspaceError));
	const FResolvedSemanticWorkspaceRef Workspace = MakeShared<FResolvedSemanticWorkspace>(
		EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State, Model.ToSharedRef(),
		Revision, TArray<FStructuralEdit>(), TArray<FString>({TEXT("properties.hjson")}), TEXT(""),
		TEXT(""), TArray<FString>(), Model->GetSemanticFingerprint());
	TOptional<FChangeSetDraft> Draft;
	FSemanticWorkspaceImportError Error;
	TestTrue(TEXT("whole-number decimal round-trip does not mutate read-only sibling"),
		TryBuildSemanticChangeSetDraft(Workspace, Snapshot.ToSharedRef(),
			FUeAgentDescriptorIdentity(TEXT("uai.hjson.semantic.import"), 1,
				TEXT("sha256:"
					 "222222222222222222222222222222222222"
					 "2222222222222222222222222222")),
			TEXT("compact:change-set"), TEXT("compact:change-set:1"), Resources, Draft, Error));
	TestTrue(TEXT("compact numeric draft is present"), Draft.IsSet());
	if (Draft.IsSet())
	{
		TestEqual(TEXT("only the edited enum property remains"), Draft->GetChanges().Num(), 1);
		if (Draft->GetChanges().Num() == 1)
		{
			const FChangeDraft& Change = Draft->GetChanges()[0];
			TestEqual(TEXT("edited identity is Mobility"), Change.GetIdentity().GetSelector(),
				FString(TEXT("Mobility")));
			TestEqual(TEXT("compact enum metadata is restored from the snapshot"),
				Change.GetProposedAfter()->GetObjectItems().Num(), 3);
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item :
				Change.GetProposedAfter()->GetObjectItems())
			{
				if (Item.Key == TEXT("selected"))
				{
					TestEqual(TEXT("edited enum selection is preserved"), Item.Value->GetText(),
						FString(TEXT("Movable")));
				}
			}
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceImportStructureProjectionTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace.StructureProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceImportStructureProjectionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FUeAgentResourceTracker Resources = MakeImportTracker();
	const FAuthoringModelRef CurrentModel = MakeStructureModel(Resources, false);
	const FAuthoringModelRef EditedModel = MakeStructureModel(Resources, true);
	const FUeAgentCanonicalSnapshotRef Snapshot = MakeStructureSnapshot(Resources);
	TestNotEqual(TEXT("fixture preserves the real stable-key/state-key split"),
		StructureAuthoringIdentityKey(), StructureIdentity().GetCanonicalKey());
	const FResolvedSemanticWorkspaceRef Workspace = MakeShared<FResolvedSemanticWorkspace>(
		EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State, EditedModel,
		Snapshot->GetRevision(), TArray<FStructuralEdit>(),
		TArray<FString>({TEXT("structures/event-graph/structure.hjson")}),
		TEXT(""), TEXT(""), TArray<FString>(), EditedModel->GetSemanticFingerprint());
	FSemanticWorkspaceChangeProjection Projection;
	FPreparedAuthoringImportPtr PreparedImport;
	FSemanticWorkspaceImportError Error;
	TestTrue(
		TEXT("changed structure values enter the common semantic projection"),
		TryPrepareSemanticWorkspaceChanges(
			Workspace, CurrentModel, Snapshot,
			FUeAgentDescriptorIdentity(
				TEXT("uai.hjson.semantic.import"), 1,
				TEXT("sha256:4444444444444444444444444444444444444444444444444444444444444444")),
			TEXT("structural-projection"), TEXT("structural-projection:1"),
			FUeAgentCallerContext(
				TEXT("structural-projection-test"),
				{TEXT("authoring.write"), TEXT("authoring.structure")},
				TEXT("structural-projection")),
			TEXT("/Game/HX013/BP_Structural"), Resources, Projection,
			PreparedImport, Error));
	TestEqual(TEXT("structure-only edit has no root properties"),
		Projection.RootObjectProperties.Num(), 0);
	TestEqual(TEXT("one changed semantic unit is exposed"),
		Projection.ChangedUnitIds.Num(), 1);
	if (Projection.ChangedUnitIds.Num() == 1)
	{
		TestEqual(TEXT("changed unit identity is stable"), Projection.ChangedUnitIds[0],
			FString(TEXT("structure:event-graph")));
	}
	TestTrue(TEXT("projection reports a real change"), Projection.HasChanges());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceOriginalValueIntentTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace.OriginalValueIntent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceOriginalValueIntentTest::RunTest(const FString&)
{
	// Keep rejection/value assertions and classify only the deliberate failure logs.
	AddExpectedError(TEXT("UAI workspace operation: hjson_import_intent_not_retained"), EAutomationExpectedErrorFlags::Contains, 9);
	AddExpectedError(TEXT("UAI workspace operation: hjson_import_intent_conversion_not_retained"), EAutomationExpectedErrorFlags::Contains, 3);
	UeAgentAuthoringState::FStateEntry Aliased;
	Aliased.SemanticId = TEXT("core.property");
	Aliased.TargetIdentity = {TEXT("blueprint_graph_identity"), TEXT("blueprint_graph"),
		TEXT("guid"), StructureAuthoringIdentityKey(), {}};
	Aliased.Value = MakeShared<FJsonValueNumber>(5434.123456789);
	FString NormalizeError;
	auto* Intensity = FindFProperty<FFloatProperty>(ULightComponentBase::StaticClass(), TEXT("Intensity"));
	if (!TestNotNull(TEXT("native float property"), Intensity)) return false;
	if (!TestTrue(TEXT("normalization retains aliased input value and identity"),
		UeAgentAuthoringSemanticCompare::BuildExpectedPropertyEntry(Aliased.SemanticId,
			Aliased.TargetIdentity, Intensity, Aliased.Value, nullptr, Aliased, NormalizeError))) return false;
	TestEqual(TEXT("native float narrowing uses the codec"),
		Intensity->GetPropertyValue(Aliased.NativeValue->GetValuePtr()), float(5434.123456789));
	TestEqual(TEXT("semantic input survives output reset"), Aliased.SemanticId, FString(TEXT("core.property")));
	TestEqual(TEXT("identity input survives output reset"), Aliased.TargetIdentity.CanonicalKey, StructureAuthoringIdentityKey());

	FUeAgentResourceTracker Resources = MakeImportTracker();
	const auto Current = MakeStructureModel(Resources, false);
	const auto Edited = MakeStructureModel(Resources, true);
	const auto Snapshot = MakeStructureSnapshot(Resources);
	TMap<FString, FSourceProvenance> Sources;
	Sources.Add(TEXT("structure:event-graph"), FSourceProvenance(
		TEXT("structures/event-graph/structure.hjson"), TEXT("/values"), 8, 3, 0, 0, TEXT("values")));
	const FResolvedSemanticWorkspaceRef Workspace = MakeShared<FResolvedSemanticWorkspace>(
		EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State, Edited,
		Snapshot->GetRevision(), TArray<FStructuralEdit>(), TArray<FString>(),
		FString(), FString(), TArray<FString>(), Edited->GetSemanticFingerprint(),
		TOptional<TArray<FString>>(), TMap<FString, TSet<FString>>(), MoveTemp(Sources));
	FSemanticWorkspaceChangeProjection Projection;
	FPreparedAuthoringImportPtr Prepared;
	FSemanticWorkspaceImportError Error;
	if (!TestTrue(TEXT("prepare original structure intent"), TryPrepareSemanticWorkspaceChanges(
		Workspace, Current, Snapshot, FUeAgentDescriptorIdentity(TEXT("uai.hjson.semantic.import"), 1,
		TEXT("sha256:4444444444444444444444444444444444444444444444444444444444444444")),
		TEXT("original-intent"), TEXT("original-intent:1"),
		FUeAgentCallerContext(TEXT("original-intent"), {TEXT("authoring.write"), TEXT("authoring.structure")}, TEXT("original-intent")),
		TEXT("/Game/HX013/BP_Structural"), Resources, Projection, Prepared, Error))) return false;

	UE::UAI::Contract::FDiagnostic RegisterError;
	if (!TestTrue(TEXT("register native semantic comparison"), UeAgentBlueprintAuthoring::RegisterDescriptors(RegisterError))) return false;
	{
		FWorkspaceError CompositionError;
		if (!TestTrue(TEXT("register existing graph composition contract"), RegisterGraphTreeTargetCompositionContracts(RegisterError))) return false;
		FCanonicalCompositionRequest CanonicalRequest;
		CanonicalRequest.TargetKind = TEXT("ue.blueprint.asset"); CanonicalRequest.ProfileId = Current->GetProfileId();
		CanonicalRequest.BaseRevision = TEXT("native-canonical-parity"); CanonicalRequest.SourceProvenance = TEXT("fixture.capture");
		CanonicalRequest.Entries = Snapshot->GetEntries();
		FAuthoringModelPtr CanonicalModel, NativeModel;
		if (!TestTrue(TEXT("compose frozen canonical capture"), TryComposeCanonicalAuthoringModel(CanonicalRequest, Resources, CanonicalModel, CompositionError))) return false;
		UeAgentAuthoringState::FStateEntry Native;
		Native.SemanticId = StructureIdentity().GetSemanticId(); Native.Type.Kind = TEXT("object");
		Native.TargetIdentity = {TEXT("blueprint_graph_identity"), TEXT("blueprint_graph"), TEXT("guid"),
			StructureAuthoringIdentityKey(), StructureIdentity().GetSelectors()};
		Native.AccessorId = TEXT("fixture.native_graph"); Native.bAvailable = true; Native.bEditable = true;
		if (!CanonicalJson::TryToJsonValue(Snapshot->GetEntries()[0].GetValue().Get(), Native.Value, RegisterError)) return false;
		FCompositionRequest NativeRequest;
		NativeRequest.TargetKind = CanonicalRequest.TargetKind; NativeRequest.ProfileId = CanonicalRequest.ProfileId;
		NativeRequest.BaseRevision = CanonicalRequest.BaseRevision; NativeRequest.SourceProvenance = CanonicalRequest.SourceProvenance;
		NativeRequest.Entries = {Native};
		if (!TestTrue(TEXT("compose independent native capture"), TryComposeAuthoringModel(NativeRequest, Resources, NativeModel, CompositionError))) return false;
		const FSemanticUnit* CanonicalGraph = nullptr;
		for (const auto& Unit : CanonicalModel->GetUnits())
			if (Unit.GetSemanticTypeRef() == Native.SemanticId && Unit.GetUnitKind() == ESemanticUnitKind::StructureState) { CanonicalGraph = &Unit; break; }
		if (!TestNotNull(TEXT("canonical graph unit is represented"), CanonicalGraph)) return false;
		TestEqual(TEXT("canonical envelope restores the authoritative native target"), CanonicalGraph->GetTargetIdentityKey(), Native.TargetIdentity.CanonicalKey);
		const auto* NativeGraph = NativeModel->FindUnit(CanonicalGraph->GetUnitId());
		TestNotNull(TEXT("native and frozen canonical captures have the same semantic unit identity"), NativeGraph);
	}
	{
		FHjsonDocumentPtr Fragment;
		FWorkspaceError ParseError;
		if (!TestTrue(TEXT("split module input source parses"), TryParseHjsonDocument(
			TEXT("{inputs:{\"Module.Color\":{value:[0.15,0.65]}}}"), TEXT("modules/Color.hjson"), Resources, Fragment, ParseError))) return false;
		const FSourceProvenance ParentSource(TEXT("stack.hjson"), TEXT("/"), 1, 1, 0, 0, FString());
		const auto Modules = MakeShared<FHjsonNode>(EHjsonNodeKind::Object, ParentSource, FString(), FString(), false,
			TArray<FHjsonNodeRef>(), TArray<TPair<FString, FHjsonNodeRef>>{{TEXT("Color"), Fragment->GetRoot()}});
		const auto Root = MakeShared<FHjsonNode>(EHjsonNodeKind::Object, ParentSource, FString(), FString(), false,
			TArray<FHjsonNodeRef>(), TArray<TPair<FString, FHjsonNodeRef>>{{TEXT("modules"), Modules}});
		TMap<FString, FHjsonDocumentRef> Documents;
		Documents.Add(TEXT("structure:event-graph"), MakeShared<FHjsonDocument>(TEXT("stack.hjson"), Root));
		const auto LocatedWorkspace = MakeShared<FResolvedSemanticWorkspace>(EWorkspaceSourceKind::Directory,
			ESemanticWorkspaceMode::State, Edited, Snapshot->GetRevision(), TArray<FStructuralEdit>(), TArray<FString>(),
			FString(), FString(), TArray<FString>(), Edited->GetSemanticFingerprint(), TOptional<TArray<FString>>(),
			TMap<FString, TSet<FString>>(), TMap<FString, FSourceProvenance>(), TMap<FString, FString>(), MoveTemp(Documents));
		FDocumentValueSourceBinding Binding;
		Binding.DocumentIdentity = TEXT("captured-stack"); Binding.UnitId = TEXT("structure:event-graph");
		Binding.DocumentPointer = TEXT("/modules"); Binding.UnitValuePointer = TEXT("/modules");
		const FString Pointer = TEXT("/modules/Color/inputs/Module.Color/value/0");
		auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("hjson_import_intent_not_retained"),
			Pointer, MakeShared<FJsonValueNumber>(0.15), MakeShared<FJsonValueNumber>(0.16), TEXT("final_readback"));
		LocatedWorkspace->LocateDocumentDiagnostic({TEXT("captured-stack")}, {Binding}, Diagnostic);
		TestEqual(TEXT("aggregate value diagnostic points to the edited fragment"), Diagnostic.Source.SourceFile, FString(TEXT("modules/Color.hjson")));
		TestEqual(TEXT("diagnostic uses the fragment's original pointer"), Diagnostic.Source.JsonPath, FString(TEXT("/inputs/Module.Color/value/0")));
		TestEqual(TEXT("semantic pointer remains independently available"), Diagnostic.SemanticPointer, Pointer);
	}
	{
		UeAgentAuthoringEncodedTypes::FScope Scope;
		UeAgentAuthoringTypeDescriptors::FDescriptor ObjectType;
		ObjectType.Kind = TEXT("object");
		const auto Policy = MakeShared<FJsonObject>();
		Policy->SetStringField(TEXT("omitted_patch_field_policy"), TEXT("preserve_current"));
		const auto Inputs = MakeShared<FJsonObject>();
		Inputs->SetNumberField(TEXT("old"), 18);
		Inputs->SetNumberField(TEXT("new"), 90);
		const TSharedPtr<FJsonValue> InputValue = MakeShared<FJsonValueObject>(Inputs);
		UeAgentAuthoringEncodedTypes::FScope::Record(InputValue, ObjectType, Policy);
		const auto Native = MakeShared<FJsonObject>();
		Native->SetField(TEXT("inputs"), InputValue);
		const auto Inventory = MakeShared<FJsonObject>();
		Inventory->SetNumberField(TEXT("node"), 1);
		Native->SetObjectField(TEXT("inventory"), Inventory);
		UeAgentAuthoringState::FStateEntry Declaration;
		Declaration.Type = ObjectType;
		Declaration.Value = MakeShared<FJsonValueObject>(Native);
		Declaration.ComparisonValueDefinition = MakeShared<FJsonValueObject>(Scope.BuildDefinition(Declaration.Value));
		const auto Before = UeAgentAuthoringState::CloneCanonicalValue(Declaration.Value);
		Before->AsObject()->GetObjectField(TEXT("inputs"))->RemoveField(TEXT("new"));
		const auto Desired = UeAgentAuthoringState::CloneCanonicalValue(Declaration.Value);
		Desired->AsObject()->GetObjectField(TEXT("inputs"))->RemoveField(TEXT("old"));
		auto Expected = UeAgentAuthoringSemanticCompare::OverlayValueDelta(Before, Desired, Declaration.Value, &Declaration);
		TestTrue(TEXT("patch omission retains existing native members"),
			CanonicalJson::AreJsonValuesEquivalent(Expected, Declaration.Value));
		Desired->AsObject()->RemoveField(TEXT("inputs"));
		Expected = UeAgentAuthoringSemanticCompare::OverlayValueDelta(Before, Desired, Declaration.Value, &Declaration);
		TestTrue(TEXT("omitting the entire patch map does not request reset"), Expected->AsObject()->HasField(TEXT("inputs")));
		Desired->AsObject()->GetObjectField(TEXT("inventory"))->RemoveField(TEXT("node"));
		Expected = UeAgentAuthoringSemanticCompare::OverlayValueDelta(Before, Desired, Declaration.Value, &Declaration);
		TestFalse(TEXT("ordinary inventory deletion remains an explicit edit"),
			Expected->AsObject()->GetObjectField(TEXT("inventory"))->HasField(TEXT("node")));
		const auto Changed = MakeShared<FJsonObject>(); Changed->SetNumberField(TEXT("new"), 100);
		Desired->AsObject()->SetObjectField(TEXT("inputs"), Changed);
		Expected = UeAgentAuthoringSemanticCompare::OverlayValueDelta(Before, Desired, Declaration.Value, &Declaration);
		TestEqual(TEXT("patch policy cannot hide a lost explicit value"),
			Expected->AsObject()->GetObjectField(TEXT("inputs"))->GetNumberField(TEXT("new")), 100.0);
		Desired->AsObject()->SetField(TEXT("inputs"), MakeShared<FJsonValueNull>());
		Expected = UeAgentAuthoringSemanticCompare::OverlayValueDelta(Before, Desired, Declaration.Value, &Declaration);
		TestTrue(TEXT("explicit null is not omission"), Expected->AsObject()->TryGetField(TEXT("inputs"))->Type == EJson::Null);
	}
	{
		UeAgentAuthoringEncodedTypes::FScope Scope;
		UeAgentAuthoringTypeDescriptors::FDescriptor FloatType;
		FloatType.Kind = TEXT("float");
		const auto NativeNumber = MakeShared<FJsonValueNumber>(double(float(0.15)));
		UeAgentAuthoringEncodedTypes::FScope::Record(NativeNumber, FloatType, nullptr);
		const auto Native = MakeShared<FJsonObject>();
		Native->SetArrayField(TEXT("value"), {NativeNumber});
		UeAgentAuthoringState::FStateEntry Actual;
		Actual.SemanticId = TEXT("core.property"); Actual.Type.Kind = TEXT("object");
		Actual.TargetIdentity = Aliased.TargetIdentity;
		Actual.bAvailable = true;
		Actual.AccessorId = TEXT("original_intent.native_float_fixture");
		Actual.Value = MakeShared<FJsonValueObject>(Native);
		Actual.ComparisonValueDefinition = MakeShared<FJsonValueObject>(Scope.BuildDefinition(Actual.Value));
		if (!TestTrue(TEXT("native numeric fixture satisfies the capture contract"), Actual.IsValid())) return false;
		auto Expected = Actual;
		Expected.Value = UeAgentAuthoringState::CloneCanonicalValue(Actual.Value);
		Expected.Value->AsObject()->SetArrayField(TEXT("value"), {MakeShared<FJsonValueNumber>(0.15)});
		UeAgentAuthoringSemanticCompare::FEntryComparison Comparison;
		UeAgentAuthoringSemanticCompare::FCompareOptions Options; Options.bCompareAvailability = false;
		TestTrue(TEXT("nested native float provenance reaches the common comparator"),
			UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && Comparison.bEquivalent);
		Expected.Value->AsObject()->SetArrayField(TEXT("value"), {MakeShared<FJsonValueNumber>(0.16)});
		TestTrue(TEXT("native float comparison still rejects a different stored value"),
			UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && !Comparison.bEquivalent);
	}
	{
		UeAgentAuthoringEncodedTypes::FScope Scope;
		const auto Native = MakeShared<FJsonObject>();
		const auto Inputs = MakeShared<FJsonObject>();
		const auto Default = MakeShared<FJsonObject>();
		Default->SetStringField(TEXT("kind"), TEXT("bool")); Default->SetBoolField(TEXT("value"), false);
		using UeAgentAuthoringEncodedTypes::EFieldEmission;
		UeAgentAuthoringEncodedTypes::FScope::SetNativeField(Inputs, TEXT("Switch"), MakeShared<FJsonValueObject>(Default), EFieldEmission::NativeDefault);
		UeAgentAuthoringEncodedTypes::FScope::SetNativeField(Native, TEXT("inputs"), MakeShared<FJsonValueObject>(Inputs), EFieldEmission::NonEmptyObject);
		UeAgentAuthoringState::FStateEntry Actual;
		Actual.SemanticId = TEXT("core.property"); Actual.Type.Kind = TEXT("object"); Actual.TargetIdentity = Aliased.TargetIdentity;
		Actual.AccessorId = TEXT("original_intent.omitted_native_fixture");
		// Clone before projection, as nested document assembly does. The native
		// facts stay complete; omission and its proof belong to the projector.
		Actual.Value = Scope.ProjectAuthoringValue(UeAgentAuthoringState::CloneCanonicalValue(MakeShared<FJsonValueObject>(Native)));
		Actual.ComparisonValueDefinition = MakeShared<FJsonValueObject>(Scope.BuildDefinition(Actual.Value));
		auto Expected = Actual;
		Expected.Value = UeAgentAuthoringState::CloneCanonicalValue(Actual.Value);
		const auto ExplicitInputs = MakeShared<FJsonObject>();
		ExplicitInputs->SetObjectField(TEXT("Switch"), UeAgentAuthoringState::CloneCanonicalValue(MakeShared<FJsonValueObject>(Default))->AsObject());
		Expected.Value->AsObject()->SetObjectField(TEXT("inputs"), ExplicitInputs);
		UeAgentAuthoringSemanticCompare::FEntryComparison Comparison;
		UeAgentAuthoringSemanticCompare::FCompareOptions Options; Options.bCompareAvailability = false;
		TestTrue(TEXT("explicit default equals a proven omitted native input"),
			UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && Comparison.bEquivalent);
		const auto OmittedBefore = Actual;
		Actual.Value = UeAgentAuthoringState::CloneCanonicalValue(Expected.Value);
		TestTrue(TEXT("restore may materialize the same effective defaults"),
			UeAgentAuthoringSemanticCompare::CompareEntries(OmittedBefore, Actual, Options, Comparison) && Comparison.bEquivalent);
		Actual.Value->AsObject()->GetObjectField(TEXT("inputs"))->GetObjectField(TEXT("Switch"))->SetBoolField(TEXT("value"), true);
		TestTrue(TEXT("restore still rejects a changed effective override"),
			UeAgentAuthoringSemanticCompare::CompareEntries(OmittedBefore, Actual, Options, Comparison) && !Comparison.bEquivalent);
		Actual = OmittedBefore;
		ExplicitInputs->GetObjectField(TEXT("Switch"))->SetBoolField(TEXT("value"), true);
		TestTrue(TEXT("an omitted false default cannot validate authored true"),
			UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && !Comparison.bEquivalent);
		ExplicitInputs->GetObjectField(TEXT("Switch"))->SetBoolField(TEXT("value"), false);
		Actual.ComparisonValueDefinition.Reset();
		Expected.ComparisonValueDefinition.Reset();
		TestTrue(TEXT("absence without native provenance is still a mismatch"),
			UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && !Comparison.bEquivalent);
		TestTrue(TEXT("projection preserves the native capture"), Native->HasField(TEXT("inputs")) && Inputs->HasField(TEXT("Switch")));
		TestTrue(TEXT("projection omits the empty group in the public payload"), OmittedBefore.Value->AsObject()->Values.IsEmpty());
		const auto PresentGroup = MakeShared<FJsonObject>();
		PresentGroup->SetObjectField(TEXT("inputs"), Inputs);
		const auto PresentProjection = Scope.ProjectAuthoringValue(MakeShared<FJsonValueObject>(PresentGroup));
		TestTrue(TEXT("presence-sensitive empty groups are retained"), PresentProjection->AsObject()->HasField(TEXT("inputs"))
			&& PresentProjection->AsObject()->GetObjectField(TEXT("inputs"))->Values.IsEmpty());
		TSharedPtr<FJsonValue> NestedProjection;
		{
			UeAgentAuthoringEncodedTypes::FScope Nested;
			const auto Item = MakeShared<FJsonObject>();
			UeAgentAuthoringEncodedTypes::FScope::SetNativeField(Item, TEXT("value"), MakeShared<FJsonValueBoolean>(false), EFieldEmission::NativeDefault);
			NestedProjection = Nested.ProjectAuthoringValue(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueObject>(Item)}));
		}
		TestTrue(TEXT("projection keeps array membership and nested capture proof"), NestedProjection->AsArray().Num() == 1
			&& NestedProjection->AsArray()[0]->AsObject()->Values.IsEmpty() && Scope.BuildDefinition(NestedProjection).IsValid());
	}
	{
		UeAgentAuthoringEncodedTypes::FScope Scope;
		FBodyInstance Native, Defaults;
		Native.PhysicsBlendWeight = 0.25f;
		const auto Fields = MakeShared<FJsonObject>();
		for (const FName Name : {FName(TEXT("bNotifyRigidBodyCollision")), FName(TEXT("PhysicsBlendWeight"))})
		{
			const auto Property = FindFProperty<FProperty>(FBodyInstance::StaticStruct(), Name);
			if (!TestNotNull(TEXT("reflected default field"), Property)) return false;
			Fields->SetField(Name.ToString(), UeAgentPropertyValueCodec::Encode(
				Property, Property->ContainerPtrToValuePtr<void>(&Native), nullptr, {}));
		}
		const auto Sparse = UeAgentPropertyValueCodec::ProjectNativeDefaultStructFields(
			FBodyInstance::StaticStruct(), &Native, &Defaults, Fields);
		TestTrue(TEXT("native default projection retains changed reflected values"),
			!Sparse->HasField(TEXT("bNotifyRigidBodyCollision")) && Sparse->HasField(TEXT("PhysicsBlendWeight")));
		TestEqual(TEXT("native default projection does not mutate capture"), Fields->Values.Num(), 2);
		TestTrue(TEXT("no default instance means no assumed omissions"),
			UeAgentPropertyValueCodec::ProjectNativeDefaultStructFields(
				FBodyInstance::StaticStruct(), &Native, nullptr, Fields) == Fields);
		UeAgentAuthoringState::FStateEntry Actual;
		Actual.SemanticId = TEXT("core.property"); Actual.Type.Kind = TEXT("object"); Actual.TargetIdentity = Aliased.TargetIdentity;
		Actual.Value = MakeShared<FJsonValueObject>(Sparse);
		Actual.AccessorId = TEXT("original_intent.reflected_default_fixture");
		Actual.ComparisonValueDefinition = MakeShared<FJsonValueObject>(Scope.BuildDefinition(Actual.Value));
		auto Expected = Actual;
		Expected.Value = UeAgentAuthoringState::CloneCanonicalValue(MakeShared<FJsonValueObject>(Fields));
		UeAgentAuthoringSemanticCompare::FEntryComparison Comparison;
		UeAgentAuthoringSemanticCompare::FCompareOptions Options; Options.bCompareAvailability = false;
		TestTrue(TEXT("reflected defaults compare through the common omission mechanism"),
			UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && Comparison.bEquivalent);
		Expected.Value->AsObject()->SetBoolField(TEXT("bNotifyRigidBodyCollision"), true);
		TestTrue(TEXT("reflected omission cannot hide an unapplied nondefault edit"),
			UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && !Comparison.bEquivalent);
	}
	{
		UeAgentAuthoringEncodedTypes::FScope Scope;
		FRigControlSettings Native, Defaults;
		Native.bDrawLimits = !Defaults.bDrawLimits;
		TSharedPtr<FJsonObject> Fields;
		FString CaptureError;
		if (!TestTrue(TEXT("rig settings use complete Details capture before default projection"),
			UeAgentDetailsAuthoringService::CaptureStructDeltaFields(FRigControlSettings::StaticStruct(),
				&Native, &Defaults, TEXT("struct:/Script/ControlRig.RigControlSettings"), nullptr, Fields, CaptureError)))
		{
			AddError(CaptureError); return false;
		}
		TestTrue(TEXT("rig changed field remains and default field is sparse"),
			Fields->HasField(TEXT("bDrawLimits")) && !Fields->HasField(TEXT("DisplayName")));
		UeAgentAuthoringState::FStateEntry Actual;
		Actual.SemanticId = TEXT("core.property"); Actual.Type.Kind = TEXT("object"); Actual.TargetIdentity = Aliased.TargetIdentity;
		Actual.Value = MakeShared<FJsonValueObject>(Fields);
		Actual.AccessorId = TEXT("original_intent.rig_default_fixture");
		Actual.ComparisonValueDefinition = MakeShared<FJsonValueObject>(Scope.BuildDefinition(Actual.Value));
		auto Expected = Actual;
		Expected.Value = UeAgentAuthoringState::CloneCanonicalValue(Actual.Value);
		const auto NameProperty = FindFProperty<FProperty>(FRigControlSettings::StaticStruct(), TEXT("DisplayName"));
		if (!TestNotNull(TEXT("rig settings name field"), NameProperty)) return false;
		Expected.Value->AsObject()->SetField(TEXT("DisplayName"), UeAgentPropertyValueCodec::Encode(
			NameProperty, NameProperty->ContainerPtrToValuePtr<void>(&Native), nullptr, {}));
		UeAgentAuthoringSemanticCompare::FEntryComparison Comparison;
		UeAgentAuthoringSemanticCompare::FCompareOptions Options; Options.bCompareAvailability = false;
		TestTrue(TEXT("rig Details projection preserves default comparison proof"),
			UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && Comparison.bEquivalent);
	}
	{
		auto* BodyProperty = FindFProperty<FStructProperty>(UPrimitiveComponent::StaticClass(), TEXT("BodyInstance"));
		if (!TestNotNull(TEXT("native reflected body property"), BodyProperty)) return false;
		FBodyInstance Body;
		Body.bNotifyRigidBodyCollision = false;
		Body.PhysicsBlendWeight = 0.25f;
		for (const bool bAuthorizeDerived : {false, true})
		{
			UeAgentAuthoringState::FReadLimits Limits;
			Limits.StructFieldFilter = [bAuthorizeDerived](const UScriptStruct*, const FProperty* Field) {
				return UeAgentPropertyValueCodec::IsStructFieldAuthoringEditable(Field)
					|| (bAuthorizeDerived && Field->GetFName() == TEXT("PhysicsBlendWeight"));
			};
			UeAgentPropertyValueCodec::FOptions CodecOptions;
			CodecOptions.StructFieldFilter = Limits.StructFieldFilter;
			const auto Captured = UeAgentPropertyValueCodec::Encode(BodyProperty, &Body, nullptr, CodecOptions);
			if (!TestTrue(TEXT("captured body value"), Captured && Captured->Type == EJson::Object)) return false;
			const auto Requested = UeAgentAuthoringState::CloneCanonicalValue(Captured);
			Requested->AsObject()->GetObjectField(TEXT("fields"))->SetBoolField(TEXT("bNotifyRigidBodyCollision"), true);
			UeAgentAuthoringState::FStateEntry Expected;
			if (!TestTrue(TEXT("normalize through captured member scope"),
				UeAgentAuthoringSemanticCompare::BuildExpectedPropertyEntry(Aliased.SemanticId,
					Aliased.TargetIdentity, BodyProperty, Requested, nullptr, Expected, NormalizeError, &Limits))) return false;
			const auto* Decoded = static_cast<const FBodyInstance*>(Expected.NativeValue->GetValuePtr());
			TestTrue(TEXT("explicit notification edit survives normalization"), Decoded->bNotifyRigidBodyCollision);
			TestEqual(TEXT("authorized noneditable member follows the actual capture"),
				Expected.Value->AsObject()->GetObjectField(TEXT("fields"))->HasField(TEXT("PhysicsBlendWeight")), bAuthorizeDerived);
			TestFalse(TEXT("uncaptured native cache is not demanded or exported"),
				Expected.Value->AsObject()->GetObjectField(TEXT("fields"))->HasField(TEXT("bInterpolateWhenSubStepping")));
			UeAgentAuthoringState::FStateEntry Wrong = Expected;
			Wrong.NativeValue = UeAgentAuthoringPropertyValues::FPropertyValueStorage::CopyFrom(BodyProperty, &Body, NormalizeError);
			Wrong.Value = Captured;
			UeAgentAuthoringSemanticCompare::FEntryComparison Comparison;
			UeAgentAuthoringSemanticCompare::FCompareOptions CompareOptions;
			CompareOptions.bCompareAvailability = false;
			TestTrue(TEXT("compare changed field in the same read scope"),
				UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Wrong, CompareOptions, Comparison));
			TestFalse(TEXT("capture scope cannot hide a lost authored edit"), Comparison.bEquivalent);
		}
		UeAgentAuthoringState::FStateEntry Full;
		const auto Complete = UeAgentPropertyValueCodec::Encode(BodyProperty, &Body, nullptr, {});
		TestTrue(TEXT("unfiltered native scope remains supported"),
			UeAgentAuthoringSemanticCompare::BuildExpectedPropertyEntry(Aliased.SemanticId,
				Aliased.TargetIdentity, BodyProperty, Complete, nullptr, Full, NormalizeError));
		// Exercise capture -> snapshot copy -> expected normalization, not a
		// hand-supplied filter which could conceal a missing production handoff.
		auto* Component = NewObject<UBoxComponent>();
		Component->BodyInstance.PhysicsBlendWeight = 0.25f;
		UeAgentAuthoringState::FReadRequest Request;
		UeAgentAuthoringTargetLocator::FResolveResult Resolve;
		if (!TestTrue(TEXT("build scoped native body request"), UeAgentAuthoringState::BuildPropertyReadRequest(
			Component, TEXT("BodyInstance"), TEXT("core.property"), Request, Resolve))) return false;
		for (const bool bAuthorizeDerived : {false, true})
		{
			Request.Limits.StructFieldFilter = [bAuthorizeDerived](const UScriptStruct*, const FProperty* Field) {
				return UeAgentPropertyValueCodec::IsStructFieldAuthoringEditable(Field)
					|| (bAuthorizeDerived && Field->GetFName() == TEXT("PhysicsBlendWeight"));
			};
			Request.SnapshotIdentityOverride = Request.TargetIdentity;
			Request.SnapshotIdentityOverride->CanonicalKey += TEXT(":snapshot");
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!TestTrue(TEXT("native field scope capture"), UeAgentAuthoringState::CaptureSnapshot({Request}, Capture))) return false;
			Request.Limits = {}; // No external request table remains available.
			UeAgentAuthoringState::FStateEntry Entry;
			if (!TestTrue(TEXT("copy capture through overridden snapshot identity"), Capture.Snapshot.FindEntry(
				Request.SemanticId, Request.SnapshotIdentityOverride->CanonicalKey, Entry))) return false;
			const auto EditedValue = UeAgentAuthoringState::CloneCanonicalValue(Entry.Value);
			EditedValue->AsObject()->GetObjectField(TEXT("fields"))->SetBoolField(TEXT("bNotifyRigidBodyCollision"), true);
			if (!bAuthorizeDerived)
			{
				FUeAgentCanonicalValuePtr BeforeValue, DesiredValue;
				if (!CanonicalJson::TryFromJsonValue(Entry.Value, Resources, BeforeValue, NormalizeError)
					|| !CanonicalJson::TryFromJsonValue(EditedValue, Resources, DesiredValue, NormalizeError)) return false;
				const FUeAgentCanonicalIdentity Identity(Entry.SemanticId, Entry.TargetIdentity.TargetKind,
					Entry.TargetIdentity.CanonicalKey, {{CanonicalAuthoringTargetIdentitySelector, Entry.TargetIdentity.CanonicalKey}},
					Entry.TargetIdentity.ModelId, TEXT("fixture:scoped-capture"));
				FChangeSetDraft Draft(Prepared->GetChangeSetDraft().GetDescriptorIdentity(), TEXT("scoped-capture"), TEXT("scoped-capture"), Snapshot,
					{FChangeDraft(Identity, EChangeIntent::Update, BeforeValue, DesiredValue, {}, FString(), {})});
				FPreparedAuthoringImport ScopedPrepared(Workspace, Draft, Prepared->GetPlanningRequest(), Prepared->GetPreflightResult());
				FWorkspaceError IntentError;
				TestFalse(TEXT("final importer rejects a lost explicit field within the captured scope"),
					TryVerifySemanticWorkspaceValueIntent(ScopedPrepared, Capture.Snapshot, IntentError));
				Component->BodyInstance.bNotifyRigidBodyCollision = true;
				Request.Limits = *Entry.NativeReadLimits;
				UeAgentAuthoringState::FCaptureResult Applied;
				if (!UeAgentAuthoringState::CaptureSnapshot({Request}, Applied)) return false;
				Request.Limits = {};
				TestTrue(TEXT("final importer uses capture scope without a parallel request table"),
					TryVerifySemanticWorkspaceValueIntent(ScopedPrepared, Applied.Snapshot, IntentError));
				Component->BodyInstance.bNotifyRigidBodyCollision = false;
			}
			if (!TestTrue(TEXT("capture owns its expected-value member scope"),
				UeAgentAuthoringSemanticCompare::BuildExpectedPropertyEntry(Entry, EditedValue, nullptr, Entry, NormalizeError))) return false;
			TestEqual(TEXT("authorized member survives capture copy and in-place normalization"),
				Entry.Value->AsObject()->GetObjectField(TEXT("fields"))->HasField(TEXT("PhysicsBlendWeight")), bAuthorizeDerived);
			TestTrue(TEXT("scope stays attached to the normalized entry"), Entry.NativeReadLimits.IsValid());
			TestFalse(TEXT("field outside this capture is not reconstructed"),
				Entry.Value->AsObject()->GetObjectField(TEXT("fields"))->HasField(TEXT("bInterpolateWhenSubStepping")));
			const auto InvalidValue = UeAgentAuthoringState::CloneCanonicalValue(Entry.Value);
			InvalidValue->AsObject()->GetObjectField(TEXT("fields"))->SetBoolField(TEXT("bInterpolateWhenSubStepping"), true);
			UeAgentAuthoringState::FStateEntry InvalidExpected;
			TestFalse(TEXT("scope does not silently discard an explicitly supplied hidden field"),
				UeAgentAuthoringSemanticCompare::BuildExpectedPropertyEntry(Entry, InvalidValue, nullptr, InvalidExpected, NormalizeError));
			Capture.Snapshot.ReleaseNativeValues();
			Capture.Snapshot.CopyEntry(0, Entry);
			TestFalse(TEXT("scope callbacks retire with native storage"), Entry.NativeReadLimits.IsValid());
		}
	}
	{
		// An unchanged exported array may use compact names/enums in the input
		// and canonical wrappers in the baseline. A native profile change can
		// legitimately empty that array; representation changes are not edits.
		auto* Component = NewObject<UBoxComponent>();
		Component->SetCollisionProfileName(TEXT("OverlapAllDynamic"));
		UeAgentAuthoringState::FReadRequest Request;
		UeAgentAuthoringTargetLocator::FResolveResult Resolve;
		if (!UeAgentAuthoringState::BuildPropertyReadRequest(Component, TEXT("BodyInstance"), TEXT("core.property"), Request, Resolve)) return false;
		Request.Limits.StructFieldFilter = [](const UScriptStruct*, const FProperty* Field) {
			return UeAgentPropertyValueCodec::IsStructFieldAuthoringEditable(Field);
		};
		UeAgentAuthoringState::FCaptureResult BeforeCapture, AfterCapture;
		if (!UeAgentAuthoringState::CaptureSnapshot({Request}, BeforeCapture)) return false;
		UeAgentAuthoringState::FStateEntry BeforeEntry, AfterEntry;
		BeforeCapture.Snapshot.CopyEntry(0, BeforeEntry);
		const auto Requested = UeAgentAuthoringState::CloneCanonicalValue(BeforeEntry.Value);
		const auto Fields = Requested->AsObject()->GetObjectField(TEXT("fields"));
		Fields->SetStringField(TEXT("CollisionProfileName"), TEXT("BlockAllDynamic"));
		const auto Responses = Fields->GetObjectField(TEXT("CollisionResponses"))->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("ResponseArray"));
		for (const auto& Item : Responses->GetArrayField(TEXT("items")))
		{
			const auto ItemFields = Item->AsObject()->GetObjectField(TEXT("fields"));
			for (const auto& Pair : {TPair<const TCHAR*, const TCHAR*>(TEXT("Channel"), TEXT("literal")),
				TPair<const TCHAR*, const TCHAR*>(TEXT("Response"), TEXT("selected"))})
				ItemFields->SetStringField(Pair.Key, ItemFields->GetObjectField(Pair.Key)->GetStringField(Pair.Value));
		}
		Component->SetCollisionProfileName(TEXT("BlockAllDynamic"));
		if (!UeAgentAuthoringState::CaptureSnapshot({Request}, AfterCapture)) return false;
		AfterCapture.Snapshot.CopyEntry(0, AfterEntry);
		auto VerifyRequested = [&](const TSharedPtr<FJsonValue>& Input) {
			FUeAgentCanonicalValuePtr BeforeValue, DesiredValue;
			if (!CanonicalJson::TryFromJsonValue(BeforeEntry.Value, Resources, BeforeValue, NormalizeError)
				|| !CanonicalJson::TryFromJsonValue(Input, Resources, DesiredValue, NormalizeError)) return false;
			const FUeAgentCanonicalIdentity Identity(BeforeEntry.SemanticId, BeforeEntry.TargetIdentity.TargetKind,
				BeforeEntry.TargetIdentity.CanonicalKey, {{CanonicalAuthoringTargetIdentitySelector, BeforeEntry.TargetIdentity.CanonicalKey}},
				BeforeEntry.TargetIdentity.ModelId, TEXT("fixture:representation-delta"));
			FChangeSetDraft Draft(Prepared->GetChangeSetDraft().GetDescriptorIdentity(), TEXT("representation-delta"), TEXT("representation-delta"), Snapshot,
				{FChangeDraft(Identity, EChangeIntent::Update, BeforeValue, DesiredValue, {}, FString(), {})});
			FPreparedAuthoringImport IntentPrepared(Workspace, Draft, Prepared->GetPlanningRequest(), Prepared->GetPreflightResult());
			FWorkspaceError IntentError;
			return TryVerifySemanticWorkspaceValueIntent(IntentPrepared, AfterCapture.Snapshot, IntentError);
		};
		if (!TestTrue(TEXT("native array normalization survives unchanged compact names and enums"), VerifyRequested(Requested))) return false;
		const auto& Items = Responses->GetArrayField(TEXT("items"));
		if (!TestTrue(TEXT("native overlap fixture has response entries"), !Items.IsEmpty())) return false;
		Items[0]->AsObject()->GetObjectField(TEXT("fields"))->SetStringField(TEXT("Response"), TEXT("ECR_Ignore"));
		TestFalse(TEXT("typed delta still rejects a lost explicit collection edit"), VerifyRequested(Requested));
	}
	{
		auto* Preview = NewObject<UPointLightComponent>();
		UeAgentAuthoringState::FReadRequest PropertyRequest;
		UeAgentAuthoringTargetLocator::FResolveResult Resolve;
		if (!TestTrue(TEXT("native property proof request"), UeAgentAuthoringState::BuildPropertyReadRequest(
			Preview, TEXT("Intensity"), TEXT("core.property"), PropertyRequest, Resolve))) return false;
		UeAgentAuthoringIdentity::FCanonicalIdentity SnapshotIdentity;
		FString IdentityError;
		if (!TestTrue(TEXT("domain snapshot property identity"), UeAgentAuthoringIdentity::BuildCanonicalIdentity(
			PropertyRequest.TargetIdentity.ModelId, PropertyRequest.TargetIdentity.TargetKind,
			{{TEXT("owner_path"), TEXT("/Fixture/SnapshotOwner")}, {TEXT("property_path"), TEXT("Intensity")}},
			SnapshotIdentity, IdentityError))) return false;
		PropertyRequest.SnapshotIdentityOverride = SnapshotIdentity;
		UeAgentNativeNormalizationIntent::FScope Scope;
		UeAgentNativeNormalizationIntent::FTargetScope Target(PropertyRequest.TargetIdentity.GetSelector(TEXT("owner_path")));
		const auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("property_path"), TEXT("Intensity"));
		Row->SetNumberField(TEXT("value"), 5.0);
		UeAgentAuthoringObjectApply::FPreparedApply PropertyApply;
		UeAgentAuthoringObjectApply::FFailure PropertyFailure;
		UeAgentAuthoringObjectApply::FApplyResult PropertyResult;
		UeAgentAuthoringObjectApply::FOptions PropertyOptions;
		PropertyOptions.bEmitEditorNotifications = false;
		PropertyOptions.bMarkPackageDirty = false;
		if (!TestTrue(TEXT("prepare immutable decoded source"), UeAgentAuthoringObjectApply::PrepareObject(
			Preview, {MakeShared<FJsonValueObject>(Row)}, PropertyOptions, PropertyApply, PropertyFailure))
			|| !TestTrue(TEXT("apply isolated source"), UeAgentAuthoringObjectApply::ApplyPrepared(PropertyApply, PropertyResult))) return false;
		Preview->Intensity = 1.0f; // Simulate the native normalizer on the isolated preview.
		{
			UeAgentAuthoringDiagnostics::FScopedWarnings Warnings(false);
			UeAgentAuthoringObjectApply::ObservePrepared(PropertyApply);
			TestEqual(TEXT("consistency observation preserves the native result"), Preview->Intensity, 1.0f);
			TestEqual(TEXT("consistency difference produces one warning"), Warnings.GetWarnings().Num(), 1);
			if (!Warnings.GetWarnings().IsEmpty()) {
				const auto Warning = Warnings.GetWarnings()[0]->AsObject();
				TestEqual(TEXT("warning severity is explicit"), Warning->GetStringField(TEXT("severity")), FString(TEXT("warning")));
				{
					UeAgentAuthoringDiagnostics::FScopedWarnings Nested;
					UeAgentAuthoringObjectApply::ObservePrepared(PropertyApply);
				}
				TestEqual(TEXT("nested command does not duplicate the warning"), Warnings.GetWarnings().Num(), 1);
				TSharedPtr<FJsonObject> Payload;
				TestTrue(TEXT("warning payload remains valid"), FJsonSerializer::Deserialize(
					TJsonReaderFactory<>::Create(Warnings.AppendToPayload(TEXT("{\"changed\":true,\"warnings\":[{\"code\":\"existing\"}]}"))), Payload));
				if (Payload) {
					TestTrue(TEXT("native result fields survive"), Payload->GetBoolField(TEXT("changed")));
					TestEqual(TEXT("prior warnings survive"), Payload->GetArrayField(TEXT("warnings")).Num(), 2);
				}
			}
			TestFalse(TEXT("pure development comparison still detects the difference"),
				UeAgentAuthoringObjectApply::VerifyPrepared(PropertyApply, PropertyResult));
		}
		TArray<TSharedPtr<FJsonValue>> Normalized;
		if (!TestTrue(TEXT("capture normalized proof"), PropertyApply.CaptureNativeNormalizedDescriptors(Normalized, PropertyFailure))) return false;
		UeAgentAuthoringState::FCaptureResult NativeCapture;
		if (!TestTrue(TEXT("independent normalized property capture"), UeAgentAuthoringState::CaptureSnapshot({PropertyRequest}, NativeCapture))) return false;
		const auto Identity = FUeAgentCanonicalIdentity(TEXT("core.property"), SnapshotIdentity.TargetKind,
			SnapshotIdentity.CanonicalKey,
			{{CanonicalAuthoringTargetIdentitySelector, SnapshotIdentity.CanonicalKey}},
			SnapshotIdentity.ModelId, TEXT("fixture:native-normalization"));
		UeAgentAuthoringState::FStateEntry CapturedProperty;
		if (!TestTrue(TEXT("captured domain property"), NativeCapture.Snapshot.CopyEntry(0, CapturedProperty))) return false;
		TestEqual(TEXT("capture retains reflection identity alongside domain identity"),
			CapturedProperty.NativeTargetIdentity.CanonicalKey, PropertyRequest.TargetIdentity.CanonicalKey);
		TestTrue(TEXT("domain capture resolves the original native proof"), Scope.Find(CapturedProperty) != nullptr);
		auto MakeProofDraft = [&](double Requested) {
			return FChangeSetDraft(Prepared->GetChangeSetDraft().GetDescriptorIdentity(), TEXT("native-proof"), TEXT("native-proof"), Snapshot,
				{FChangeDraft(Identity, EChangeIntent::Update,
					ObjectValue({{TEXT("literal"), DecimalValue(TEXT("0"), Resources)}}, Resources),
					ObjectValue({{TEXT("literal"), DecimalValue(FString::SanitizeFloat(Requested), Resources)}}, Resources), {}, FString(), {})});
		};
		FPreparedAuthoringImport NativePrepared(Workspace, MakeProofDraft(5.0), Prepared->GetPlanningRequest(), Prepared->GetPreflightResult());
		FWorkspaceError ProofError;
		TestTrue(TEXT("native normalization retains the original decoded edit"),
			TryVerifySemanticWorkspaceValueIntent(NativePrepared, NativeCapture.Snapshot, ProofError, {}, &Scope));
		FPreparedAuthoringImport LostPrepared(Workspace, MakeProofDraft(6.0), Prepared->GetPlanningRequest(), Prepared->GetPreflightResult());
		TestFalse(TEXT("normalization cannot hide a conversion which lost the original edit"),
			TryVerifySemanticWorkspaceValueIntent(LostPrepared, NativeCapture.Snapshot, ProofError, {}, &Scope));
		TestEqual(TEXT("conversion loss classification"), ProofError.GetCode(), FString(TEXT("hjson_import_intent_conversion_not_retained")));
		Preview->Intensity = 0.0f;
		if (!TestTrue(TEXT("capture wrong final normalized value"), UeAgentAuthoringState::CaptureSnapshot({PropertyRequest}, NativeCapture))) return false;
		TestFalse(TEXT("source proof cannot accept an incorrect final native value"),
			TryVerifySemanticWorkspaceValueIntent(NativePrepared, NativeCapture.Snapshot, ProofError, {}, &Scope));
	}
	{
		auto* Component = NewObject<UBoxComponent>();
		Component->SetCollisionProfileName(TEXT("OverlapAllDynamic"));
		UeAgentAuthoringState::FReadRequest Request;
		UeAgentAuthoringTargetLocator::FResolveResult Resolve;
		if (!UeAgentAuthoringState::BuildPropertyReadRequest(Component, TEXT("BodyInstance"), TEXT("core.property"), Request, Resolve)) return false;
		Request.Limits.StructFieldFilter = [](const UScriptStruct*, const FProperty* Field) {
			return UeAgentPropertyValueCodec::IsStructFieldAuthoringEditable(Field);
		};
		Request.SnapshotIdentityOverride = Request.TargetIdentity;
		Request.SnapshotIdentityOverride->CanonicalKey += TEXT(":domain");
		UeAgentAuthoringState::FCaptureResult BeforeCapture, AfterCapture;
		if (!UeAgentAuthoringState::CaptureSnapshot({Request}, BeforeCapture)) return false;
		UeAgentAuthoringState::FStateEntry BeforeEntry, AfterEntry;
		BeforeCapture.Snapshot.CopyEntry(0, BeforeEntry);
		const auto Requested = UeAgentAuthoringState::CloneCanonicalValue(BeforeEntry.Value);
		const auto Fields = Requested->AsObject()->GetObjectField(TEXT("fields"));
		Fields->SetStringField(TEXT("CollisionProfileName"), TEXT("BlockAll"));
		const auto Responses = Fields->GetObjectField(TEXT("CollisionResponses"))->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("ResponseArray"));
		const auto& Items = Responses->GetArrayField(TEXT("items"));
		if (!TestTrue(TEXT("normalization source has explicit response entries"), !Items.IsEmpty())) return false;
		for (const auto& Item : Items)
			Item->AsObject()->GetObjectField(TEXT("fields"))->SetStringField(TEXT("Response"), TEXT("ECR_Block"));
		UeAgentNativeNormalizationIntent::FScope Scope;
		UeAgentNativeNormalizationIntent::FTargetScope Target(Request.TargetIdentity.GetSelector(TEXT("owner_path")));
		const auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("property_path"), TEXT("BodyInstance"));
		Row->SetField(TEXT("value"), Requested);
		UeAgentAuthoringObjectApply::FPreparedApply PropertyApply;
		UeAgentAuthoringObjectApply::FFailure PropertyFailure;
		UeAgentAuthoringObjectApply::FApplyResult PropertyResult;
		UeAgentAuthoringObjectApply::FOptions PropertyOptions;
		PropertyOptions.bEmitEditorNotifications = false;
		PropertyOptions.bMarkPackageDirty = false;
		{
			UeAgentAuthoringDiagnostics::FScopedWarnings ImmediateWarnings(false);
			if (!TestTrue(TEXT("prepare filtered struct normalization source"), UeAgentAuthoringObjectApply::PrepareObject(
				Component, {MakeShared<FJsonValueObject>(Row)}, PropertyOptions, PropertyApply, PropertyFailure))
				|| !TestTrue(TEXT("apply filtered struct source"), UeAgentAuthoringObjectApply::ApplyPrepared(PropertyApply, PropertyResult))) return false;
			TestFalse(TEXT("reflection write does not prematurely verify the native response cache"), ImmediateWarnings.HasWarnings());
		}
		Component->SetCollisionProfileName(TEXT("BlockAll"));
		// Reflection has already stored this profile name. The native setter
		// skips an unchanged name; the production normalizer explicitly loads it.
		Component->BodyInstance.LoadProfileData(false);
		TArray<TSharedPtr<FJsonValue>> Normalized;
		if (!TestTrue(TEXT("capture native struct normalization proof"), PropertyApply.CaptureNativeNormalizedDescriptors(Normalized, PropertyFailure))
			|| !UeAgentAuthoringState::CaptureSnapshot({Request}, AfterCapture)) return false;
		AfterCapture.Snapshot.CopyEntry(0, AfterEntry);
		// Storage compaction is a type semantic, not an asset-specific proof.
		// Check it with the normalization scope deliberately absent.
		const auto ExplicitBody = UeAgentAuthoringState::CloneCanonicalValue(AfterEntry.Value);
		ExplicitBody->AsObject()->GetObjectField(TEXT("fields"))->SetField(TEXT("CollisionResponses"),
			UeAgentAuthoringState::CloneCanonicalValue(Fields->TryGetField(TEXT("CollisionResponses"))));
		UeAgentAuthoringState::FStateEntry ExplicitEntry;
		UeAgentAuthoringSemanticCompare::FEntryComparison EffectiveComparison;
		UeAgentAuthoringSemanticCompare::FCompareOptions EffectiveOptions;
		EffectiveOptions.bCompareAvailability = false;
		if (!TestTrue(TEXT("decode explicit response expectation"), UeAgentAuthoringSemanticCompare::BuildExpectedPropertyEntry(
			AfterEntry, ExplicitBody, nullptr, ExplicitEntry, NormalizeError))) return false;
		TestTrue(TEXT("nested sparse response storage compares by effective channels without proof"),
			UeAgentAuthoringSemanticCompare::CompareEntries(ExplicitEntry, AfterEntry, EffectiveOptions, EffectiveComparison)
			&& EffectiveComparison.bEquivalent);
		auto CanonicalExpected = ExplicitEntry, CanonicalActual = AfterEntry;
		CanonicalExpected.NativeValue.Reset(); CanonicalActual.NativeValue.Reset();
		TestTrue(TEXT("canonical typed response projection uses the same native semantics"),
			UeAgentAuthoringSemanticCompare::CompareEntries(CanonicalExpected, CanonicalActual, EffectiveOptions, EffectiveComparison)
			&& EffectiveComparison.bEquivalent);
		UeAgentPropertyValueCodec::FOptions StoredOptions;
		StoredOptions.bCompareStoredValues = true;
		StoredOptions.StructFieldFilter = Request.Limits.StructFieldFilter;
		TestFalse(TEXT("explicit stored edits remain distinct for change detection"),
			UeAgentPropertyValueCodec::AreSemanticallyEquivalent(AfterEntry.NativeValue->GetProperty(),
				ExplicitEntry.NativeValue->GetValuePtr(), AfterEntry.NativeValue->GetValuePtr(), nullptr, StoredOptions));
		const auto ResponseProperty = FindFProperty<FStructProperty>(FBodyInstance::StaticStruct(), TEXT("CollisionResponses"));
		auto* NativeResponses = ResponseProperty->ContainerPtrToValuePtr<FCollisionResponse>(&Component->BodyInstance);
		const auto ArrayProperty = FindFProperty<FArrayProperty>(FCollisionResponse::StaticStruct(), TEXT("ResponseArray"));
		const auto AuthoredArray = Fields->GetObjectField(TEXT("CollisionResponses"))->GetObjectField(TEXT("fields"))->TryGetField(TEXT("ResponseArray"));
		UeAgentPropertyValueCodec::FOptions MemberOptions;
		MemberOptions.NativeMemberContainer = NativeResponses;
		TestTrue(TEXT("resolved response-array member retains parent effective-value semantics"),
			UeAgentPropertyValueCodec::VerifyValueIntent(ArrayProperty, AuthoredArray,
				ArrayProperty->ContainerPtrToValuePtr<void>(NativeResponses), Component, MemberOptions, NormalizeError));
		MemberOptions.NativeMemberContainer = nullptr;
		TestFalse(TEXT("an ordinary standalone array is not treated as sparse defaults"),
			UeAgentPropertyValueCodec::VerifyValueIntent(ArrayProperty, AuthoredArray,
				ArrayProperty->ContainerPtrToValuePtr<void>(NativeResponses), Component, MemberOptions, NormalizeError));
		const auto CacheProperty = FindFProperty<FStructProperty>(FCollisionResponse::StaticStruct(), TEXT("ResponseToChannels"));
		auto* NativeCache = CacheProperty->ContainerPtrToValuePtr<FCollisionResponseContainer>(NativeResponses);
		const auto SavedCache = *NativeCache;
		NativeCache->SetResponse(ECC_Pawn, ECR_Ignore);
		MemberOptions.NativeMemberContainer = NativeResponses;
		TestTrue(TEXT("template cache lifecycle does not change authored member meaning"),
			UeAgentPropertyValueCodec::VerifyValueIntent(ArrayProperty, AuthoredArray,
				ArrayProperty->ContainerPtrToValuePtr<void>(NativeResponses), Component, MemberOptions, NormalizeError));
		UeAgentAuthoringState::FCaptureResult StaleCapture;
		if (!UeAgentAuthoringState::CaptureSnapshot({Request}, StaleCapture)) return false;
		UeAgentAuthoringState::FStateEntry StaleEntry, StoredExpectation;
		StaleCapture.Snapshot.CopyEntry(0, StaleEntry);
		if (!UeAgentAuthoringSemanticCompare::BuildExpectedPropertyEntry(StaleEntry, StaleEntry.Value, nullptr, StoredExpectation, NormalizeError)) return false;
		TestTrue(TEXT("authoring equality does not require a template transient cache to be rebuilt"),
			UeAgentAuthoringSemanticCompare::CompareEntries(StoredExpectation, StaleEntry, EffectiveOptions, EffectiveComparison)
			&& EffectiveComparison.bEquivalent);
		TestEqual(TEXT("verification does not repair the native cache"), NativeCache->GetResponse(ECC_Pawn), ECR_Ignore);
		*NativeCache = SavedCache;
		auto& ActualResponses = *ArrayProperty->ContainerPtrToValuePtr<TArray<FResponseChannel>>(NativeResponses);
		const auto SavedResponses = ActualResponses;
		auto& ChangedResponse = ActualResponses.AddDefaulted_GetRef();
		ChangedResponse.Channel = TEXT("Pawn"); ChangedResponse.Response = ECR_Ignore;
		TestFalse(TEXT("a different actual sparse response still fails member intent verification"),
			UeAgentPropertyValueCodec::VerifyValueIntent(ArrayProperty, AuthoredArray,
				ArrayProperty->ContainerPtrToValuePtr<void>(NativeResponses), Component, MemberOptions, NormalizeError));
		if (!UeAgentAuthoringState::CaptureSnapshot({Request}, StaleCapture)) return false;
		StaleCapture.Snapshot.CopyEntry(0, StaleEntry);
		TestTrue(TEXT("a different actual sparse response still fails nested semantic comparison"),
			UeAgentAuthoringSemanticCompare::CompareEntries(ExplicitEntry, StaleEntry, EffectiveOptions, EffectiveComparison)
			&& !EffectiveComparison.bEquivalent);
		TestEqual(TEXT("verification leaves native sparse storage unchanged"), ActualResponses.Num(), SavedResponses.Num() + 1);
		ActualResponses = SavedResponses;
		const auto* Proof = Scope.Find(AfterEntry);
		if (!TestTrue(TEXT("filtered struct domain capture resolves native proof"), Proof != nullptr)) return false;
		TestFalse(TEXT("source proof excludes nonauthoring native caches"),
			Proof->SourceValue->AsObject()->GetObjectField(TEXT("fields"))->HasField(TEXT("bInterpolateWhenSubStepping")));
		auto VerifyRequested = [&](const TSharedPtr<FJsonValue>& Input, FWorkspaceError& Error) {
			FUeAgentCanonicalValuePtr BeforeValue, DesiredValue;
			if (!CanonicalJson::TryFromJsonValue(BeforeEntry.Value, Resources, BeforeValue, NormalizeError)
				|| !CanonicalJson::TryFromJsonValue(Input, Resources, DesiredValue, NormalizeError)) return false;
			const FUeAgentCanonicalIdentity Identity(BeforeEntry.SemanticId, BeforeEntry.TargetIdentity.TargetKind,
				BeforeEntry.TargetIdentity.CanonicalKey, {{CanonicalAuthoringTargetIdentitySelector, BeforeEntry.TargetIdentity.CanonicalKey}},
				BeforeEntry.TargetIdentity.ModelId, TEXT("fixture:filtered-native-proof"));
			FChangeSetDraft Draft(Prepared->GetChangeSetDraft().GetDescriptorIdentity(), TEXT("filtered-native-proof"), TEXT("filtered-native-proof"), Snapshot,
				{FChangeDraft(Identity, EChangeIntent::Update, BeforeValue, DesiredValue, {}, FString(), {})});
			FPreparedAuthoringImport IntentPrepared(Workspace, Draft, Prepared->GetPlanningRequest(), Prepared->GetPreflightResult());
			return TryVerifySemanticWorkspaceValueIntent(IntentPrepared, AfterCapture.Snapshot, Error, {}, &Scope);
		};
		FWorkspaceError ProofError;
		if (!TestTrue(TEXT("editable struct proof accepts native response compaction"), VerifyRequested(Requested, ProofError))) return false;
		Items[0]->AsObject()->GetObjectField(TEXT("fields"))->SetStringField(TEXT("Response"), TEXT("ECR_Ignore"));
		TestFalse(TEXT("scoped proof cannot conceal a lost explicit struct member edit"), VerifyRequested(Requested, ProofError));
		TestEqual(TEXT("scoped source loss remains diagnosed"), ProofError.GetCode(), FString(TEXT("hjson_import_intent_conversion_not_retained")));
	}
	{
		FRichCurve NativeCurve;
		NativeCurve.AddKey(float(0.123456789), float(0.987654321));
		UeAgentAuthoringState::FStateEntry ActualCurve;
		ActualCurve.SemanticId = UeAgentBlueprintAuthoring::GraphSemanticId;
		ActualCurve.AccessorId = UeAgentBlueprintAuthoring::StateAccessorId;
		ActualCurve.TargetIdentity = Aliased.TargetIdentity;
		ActualCurve.Type.Kind = TEXT("object");
		ActualCurve.Value = MakeShared<FJsonValueObject>(UeAgentCurveJson::BuildFloatCurveJson(NativeCurve, FString(), FString(), true));
		auto RequestedCurve = ActualCurve;
		RequestedCurve.Value = UeAgentAuthoringState::CloneCanonicalValue(ActualCurve.Value);
		const auto Key = RequestedCurve.Value->AsObject()->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("value"))->GetArrayField(TEXT("keys"))[0]->AsObject();
		Key->SetNumberField(TEXT("time"), 0.123456789); Key->SetNumberField(TEXT("value"), 0.987654321);
		UeAgentAuthoringSemanticCompare::FEntryComparison CurveComparison;
		TestTrue(TEXT("formal curve fields compare in their native float32 storage"),
			UeAgentAuthoringSemanticCompare::CompareEntries(RequestedCurve, ActualCurve, {}, CurveComparison) && CurveComparison.bEquivalent);
		Key->SetNumberField(TEXT("value"), 0.98);
		TestTrue(TEXT("curve narrowing does not hide real value loss"),
			UeAgentAuthoringSemanticCompare::CompareEntries(RequestedCurve, ActualCurve, {}, CurveComparison) && !CurveComparison.bEquivalent);
	}
	// The native owner chooses a shared layout policy; unrelated numbers stay exact.
	{
		FKeyedItemProjectionContract Contract;
		TestTrue(TEXT("aggregate contract available"), TryGetKeyedItemProjectionContract(TEXT("smart_object_items"), Contract));
		auto Row = [](const TCHAR* Id, int32 Index) {
			const auto Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("id"), Id); Value->SetNumberField(TEXT("index"), Index);
			// An alternate file alias is not a display-name change when the label is explicit.
			Value->SetStringField(TEXT("label"), FString(Id).ToUpper());
			Value->SetStringField(TEXT("kind"), TEXT("slot")); Value->SetStringField(TEXT("parent"), TEXT("root"));
			Value->SetStringField(TEXT("operation"), TEXT("update")); return Value;
		};
		const auto Baseline = MakeShared<FJsonObject>(), Records = MakeShared<FJsonObject>(), Native = MakeShared<FJsonObject>();
		Baseline->SetObjectField(TEXT("A"), Row(TEXT("a"), 0)); Baseline->SetObjectField(TEXT("B"), Row(TEXT("b"), 1));
		Records->SetObjectField(TEXT("A"), Row(TEXT("a"), 18));
		Native->SetObjectField(TEXT("renamed A"), Row(TEXT("a"), 1)); Native->SetObjectField(TEXT("B"), Row(TEXT("b"), 0));
		TSharedPtr<FJsonValue> Before = MakeShared<FJsonValueObject>(Baseline), Desired = MakeShared<FJsonValueObject>(Records), Actual = MakeShared<FJsonValueObject>(Native);
		TMap<FString, FString> Paths;
		TestTrue(TEXT("aggregate projection binds native identity and preserves omitted context"),
			TryProjectKeyedItemOriginalIntent(Contract, Before, Desired, Actual, Paths, RegisterError));
		TestEqual(TEXT("authored rank agrees with independently ordered native position A"), Actual->AsObject()->GetObjectField(TEXT("A"))->GetIntegerField(TEXT("index")), Desired->AsObject()->GetObjectField(TEXT("A"))->GetIntegerField(TEXT("index")));
		TestEqual(TEXT("authored rank agrees with independently ordered native position B"), Actual->AsObject()->GetObjectField(TEXT("B"))->GetIntegerField(TEXT("index")), Desired->AsObject()->GetObjectField(TEXT("B"))->GetIntegerField(TEXT("index")));
		TestTrue(TEXT("actual facts retain native identity without visible-field filtering"), Actual->AsObject()->GetObjectField(TEXT("A"))->HasField(TEXT("id")));
		Actual->AsObject()->GetObjectField(TEXT("A"))->SetNumberField(TEXT("index"), 0);
		TestNotEqual(TEXT("a failed native reorder is still detectable"), Actual->AsObject()->GetObjectField(TEXT("A"))->GetIntegerField(TEXT("index")), Desired->AsObject()->GetObjectField(TEXT("A"))->GetIntegerField(TEXT("index")));
		const auto Removed = Row(TEXT("a"), 0); Removed->SetStringField(TEXT("operation"), TEXT("remove"));
		Records->SetObjectField(TEXT("A"), Removed);
		Before = MakeShared<FJsonValueObject>(Baseline); Desired = MakeShared<FJsonValueObject>(Records); Actual = MakeShared<FJsonValueObject>(Native);
		TestTrue(TEXT("removal instructions project to desired membership"), TryProjectKeyedItemOriginalIntent(Contract, Before, Desired, Actual, Paths, RegisterError));
		TestTrue(TEXT("retained removed native item remains a mismatch"), !Desired->AsObject()->HasField(TEXT("A")) && Actual->AsObject()->HasField(TEXT("A")));
	}
	struct FPositionCase { const TCHAR* Comparator; double Requested; double Expected; };
	const FPositionCase PositionCases[] = {
		{UeAgentAuthoringSemanticCompare::GraphInt32PositionComparator, -42.6, -42.0},
		{UeAgentAuthoringSemanticCompare::GraphRoundedInt32PositionComparator, -42.6, -43.0},
		{UeAgentAuthoringSemanticCompare::GraphVectorPositionComparator, -42.006, -42.0},
		{TEXT("uai.compare.graph_vector_small_movement"), -42.00006, -42.0}};
	for (const auto& Policy : PositionCases)
	{
		const auto OldNode = MakeShared<FJsonObject>(), DesiredNode = MakeShared<FJsonObject>();
		const auto OldPosition = MakeShared<FJsonObject>(), DesiredPosition = MakeShared<FJsonObject>();
		OldPosition->SetNumberField(TEXT("x"), -42.0); OldPosition->SetNumberField(TEXT("y"), 1.0);
		DesiredPosition->SetNumberField(TEXT("x"), Policy.Requested);
		DesiredPosition->SetNumberField(TEXT("y"), 1.0);
		OldNode->SetObjectField(TEXT("pos"), OldPosition); DesiredNode->SetObjectField(TEXT("pos"), DesiredPosition);
		OldNode->SetStringField(TEXT("name"), TEXT("old label")); DesiredNode->SetStringField(TEXT("name"), TEXT("new label"));
		DesiredNode->SetNumberField(TEXT("untyped_rate"), 0.123456789);
		TSharedPtr<FJsonValue> BeforePosition = MakeShared<FJsonValueObject>(OldNode);
		TSharedPtr<FJsonValue> RequestedPosition = MakeShared<FJsonValueObject>(DesiredNode), NativePosition;
		TMap<FString, FString> SourcePaths;
		TestTrue(TEXT("native graph position normalization"), UeAgentAuthoringSemanticCompare::FComparatorRegistry::Get().ProjectOriginalIntent(
			Policy.Comparator, Aliased, BeforePosition, RequestedPosition, NativePosition, SourcePaths, RegisterError));
		TestEqual(TEXT("native layout result"), RequestedPosition->AsObject()->GetObjectField(TEXT("pos"))->GetNumberField(TEXT("x")), Policy.Expected);
		TestFalse(TEXT("normalized original coordinate remains an explicit write"), BeforePosition->AsObject()->GetObjectField(TEXT("pos"))->HasField(TEXT("x")));
		TestEqual(TEXT("non-layout number remains exact"), RequestedPosition->AsObject()->GetNumberField(TEXT("untyped_rate")), 0.123456789);
	}
	UeAgentAuthoringState::FStateEntry ExpectedDocument;
	{
		using namespace UE::UAI::Authoring::Timeline;
		auto* Scene = NewObject<UMovieScene>();
		Scene->SetTickResolutionDirectly(FFrameRate(24000, 1));
		Scene->SetPlaybackRange(0, 48000);
		auto* Track = Scene->AddTrack<UMovieSceneFloatTrack>();
		auto* Section = CastChecked<UMovieSceneFloatSection>(Track->CreateNewSection());
		Track->AddSection(*Section);
		Section->SetRange(TRange<FFrameNumber>(TRangeBound<FFrameNumber>::Inclusive(24000),
			TRangeBound<FFrameNumber>::Exclusive(36000)));
		Section->GetChannel().AddLinearKey(24000, 5.5f);
		Section->GetChannel().GetData().GetValues()[0].Tangent.LeaveTangent = 1.25f;
		auto* Empty = Track->CreateNewSection();
		Track->AddSection(*Empty);
		Empty->SetRange(TRange<FFrameNumber>(0, 48000));
		TSharedPtr<FJsonObject> Captured;
		FUeAgentTimelineAnimationExecutionError Failure;
		if (!TestTrue(TEXT("capture native MovieScene fixture"), TryReadMovieSceneStructure(
			*Scene, TEXT("test:movie_scene"), {}, Captured, Failure))) return false;
		TSharedPtr<FJsonValue> Original = MakeShared<FJsonValueObject>(Captured);
		auto Requested = UeAgentAuthoringState::CloneCanonicalValue(Original);
		auto RequestedTrack = Requested->AsObject()->GetArrayField(TEXT("root_tracks"))[0]->AsObject();
		auto RequestedSection = RequestedTrack->GetArrayField(TEXT("sections"))[0]->AsObject();
		auto RequestedKey = RequestedSection->GetArrayField(TEXT("channels"))[0]->AsObject()->GetArrayField(TEXT("keys"))[0]->AsObject();
		RequestedKey->GetObjectField(TEXT("time"))->SetStringField(TEXT("tick"), TEXT("30000"));
		RequestedTrack->GetArrayField(TEXT("sections"))[1]->AsObject()->GetObjectField(TEXT("range"))
			->GetObjectField(TEXT("upper"))->GetObjectField(TEXT("point"))->SetStringField(TEXT("tick"), TEXT("0"));
		Requested->AsObject()->GetObjectField(TEXT("tick_resolution"))->SetStringField(TEXT("numerator"), TEXT("48000"));
		// Native operations form an independent oracle for the frozen input.
		Section->GetChannel().GetData().GetTimes()[0] = FFrameNumber(30000);
		Empty->SetRange(TRange<FFrameNumber>(0, 0));
		UE::MovieScene::TimeHelpers::MigrateFrameTimes(FFrameRate(24000, 1), FFrameRate(48000, 1), Scene, false);
		Track->RemoveSectionAt(1);
		TSharedPtr<FJsonObject> Native;
		if (!TestTrue(TEXT("capture independent native result"), TryReadMovieSceneStructure(
			*Scene, TEXT("test:movie_scene"), {}, Native, Failure))) return false;
		TSharedPtr<FJsonValue> Actual = MakeShared<FJsonValueObject>(Native);
		TMap<FString, FString> Paths;
		if (!TestTrue(TEXT("shared MovieScene original intent projection"),
			UeAgentAuthoringSemanticCompare::FComparatorRegistry::Get().ProjectOriginalIntent(
				TEXT("uai.compare.movie_scene"), Aliased, Original, Requested, Actual, Paths, RegisterError))) return false;
		const auto& WantedSections = Requested->AsObject()->GetArrayField(TEXT("root_tracks"))[0]->AsObject()->GetArrayField(TEXT("sections"));
		TestEqual(TEXT("native empty-section relationship is honored"), WantedSections.Num(), 1);
		auto WantedKey = WantedSections[0]->AsObject()->GetArrayField(TEXT("channels"))[0]->AsObject()->GetArrayField(TEXT("keys"))[0];
		auto NativeKey = Actual->AsObject()->GetArrayField(TEXT("root_tracks"))[0]->AsObject()->GetArrayField(TEXT("sections"))[0]
			->AsObject()->GetArrayField(TEXT("channels"))[0]->AsObject()->GetArrayField(TEXT("keys"))[0];
		TestEqual(TEXT("same-batch key edit migrates through the native clock"),
			WantedKey->AsObject()->GetObjectField(TEXT("time"))->GetStringField(TEXT("tick")), FString(TEXT("60000")));
		TestTrue(TEXT("native retiming including tangent agrees with original intent"),
			CanonicalJson::AreJsonValuesEquivalent(WantedKey, NativeKey));
		NativeKey->AsObject()->SetStringField(TEXT("value_lexeme"), TEXT("6.5"));
		TestFalse(TEXT("native normalization cannot accept a lost requested value"),
			CanonicalJson::AreJsonValuesEquivalent(WantedKey, NativeKey));
		TestEqual(TEXT("projection does not rewrite frozen baseline"),
			Original->AsObject()->GetObjectField(TEXT("tick_resolution"))->GetStringField(TEXT("numerator")), FString(TEXT("24000")));
	}
	ExpectedDocument.SemanticId = UeAgentBlueprintAuthoring::GraphSemanticId;
	ExpectedDocument.AccessorId = UeAgentBlueprintAuthoring::StateAccessorId;
	ExpectedDocument.TargetIdentity = Aliased.TargetIdentity;
	ExpectedDocument.Type.Kind = TEXT("object");
	const auto Document = MakeShared<FJsonObject>();
	Document->SetNumberField(TEXT("rate"), 5434.123456789);
	ExpectedDocument.Value = MakeShared<FJsonValueObject>(Document);
	const auto Definition = MakeShared<FJsonObject>(), Fields = MakeShared<FJsonObject>();
	const auto Field = MakeShared<FJsonObject>(), Type = MakeShared<FJsonObject>();
	Type->SetStringField(TEXT("kind"), TEXT("float"));
	Field->SetObjectField(TEXT("type"), Type);
	Fields->SetObjectField(TEXT("rate"), Field);
	Definition->SetObjectField(TEXT("fields"), Fields);
	ExpectedDocument.ComparisonValueDefinition = MakeShared<FJsonValueObject>(Definition);
	if (!TestTrue(TEXT("declared document fixture is a valid state entry"), ExpectedDocument.IsValid())) return false;
	auto ActualDocument = ExpectedDocument;
	ActualDocument.Value = UeAgentAuthoringState::CloneCanonicalValue(ExpectedDocument.Value);
	// Read real float storage rather than relying on an optimized intermediate
	// double->float->double expression to create the fixture's precision change.
	const double NativeFloat = Intensity->GetPropertyValue(Aliased.NativeValue->GetValuePtr());
	ActualDocument.Value->AsObject()->SetNumberField(TEXT("rate"), NativeFloat);
	UeAgentAuthoringSemanticCompare::FEntryComparison DocumentComparison;
	TestTrue(TEXT("declared native float in a document normalizes without native storage"),
		UeAgentAuthoringSemanticCompare::CompareEntries(ExpectedDocument, ActualDocument, {}, DocumentComparison)
		&& DocumentComparison.bEquivalent);
	ActualDocument.Value->AsObject()->SetNumberField(TEXT("rate"), 6000.0);
	TestTrue(TEXT("a real declared-float difference is retained"),
		UeAgentAuthoringSemanticCompare::CompareEntries(ExpectedDocument, ActualDocument, {}, DocumentComparison)
		&& !DocumentComparison.bEquivalent);
	ActualDocument.Value->AsObject()->SetNumberField(TEXT("rate"), NativeFloat);
	ExpectedDocument.ComparisonValueDefinition.Reset();
	ActualDocument.ComparisonValueDefinition.Reset();
	TestFalse(TEXT("untyped fixture numbers differ before semantic comparison"),
		UE::UAI::StateSemantic::CanonicalJson::AreJsonValuesEquivalent(ExpectedDocument.Value, ActualDocument.Value));
	if (!UeAgentAuthoringSemanticCompare::CompareEntries(ExpectedDocument, ActualDocument, {}, DocumentComparison))
		AddError(TEXT("untyped document comparison failed: ") + DocumentComparison.Diagnostic.Code + TEXT(" ") + DocumentComparison.Diagnostic.PublicMessage);
	else TestFalse(TEXT("untyped JSON numbers remain exact"), DocumentComparison.bEquivalent);
	{
		auto NameA = ExpectedDocument, NameB = ExpectedDocument;
		NameA.Type.Kind = NameB.Type.Kind = TEXT("name");
		NameA.Value = MakeShared<FJsonValueString>(TEXT("NativeName")); NameB.Value = MakeShared<FJsonValueString>(TEXT("nativename"));
		TestTrue(TEXT("declared FName uses native identity semantics"), UeAgentAuthoringSemanticCompare::CompareEntries(NameA, NameB, {}, DocumentComparison) && DocumentComparison.bEquivalent);
		const auto WrappedName = MakeShared<FJsonObject>(); WrappedName->SetStringField(TEXT("literal"), TEXT("nativename"));
		NameB.Value = MakeShared<FJsonValueObject>(WrappedName);
		TestTrue(TEXT("declared FName shares the codec's literal and envelope normalization"), UeAgentAuthoringSemanticCompare::CompareEntries(NameA, NameB, {}, DocumentComparison) && DocumentComparison.bEquivalent);
		const auto WrappedExpectedName = MakeShared<FJsonObject>(); WrappedExpectedName->SetStringField(TEXT("literal"), TEXT("NativeName"));
		NameA.Value = MakeShared<FJsonValueObject>(WrappedExpectedName);
		TestTrue(TEXT("declared FName codec envelope uses the same identity semantics"), UeAgentAuthoringSemanticCompare::CompareEntries(NameA, NameB, {}, DocumentComparison) && DocumentComparison.bEquivalent);
		NameA.Value = MakeShared<FJsonValueString>(TEXT("NativeName"));
		NameB.Value = MakeShared<FJsonValueString>(TEXT("AnotherName"));
		TestTrue(TEXT("different FName remains different"), UeAgentAuthoringSemanticCompare::CompareEntries(NameA, NameB, {}, DocumentComparison) && !DocumentComparison.bEquivalent);
		NameA.Type.Kind = NameB.Type.Kind = TEXT("string"); NameB.Value = MakeShared<FJsonValueString>(TEXT("nativename"));
		TestTrue(TEXT("ordinary strings remain case sensitive"), UeAgentAuthoringSemanticCompare::CompareEntries(NameA, NameB, {}, DocumentComparison) && !DocumentComparison.bEquivalent);
	}
	{
		const auto DocumentArgs = MakeShared<FJsonObject>(), Socket = MakeShared<FJsonObject>(), Mode = MakeShared<FJsonObject>();
		Socket->SetStringField(TEXT("name"), TEXT(" SocketIntent "));
		Socket->SetStringField(TEXT("bone"), TEXT(" Hips "));
		Mode->SetStringField(TEXT("bone"), TEXT("Spine")); Mode->SetStringField(TEXT("mode"), TEXT(" Animation-Scaled "));
		DocumentArgs->SetArrayField(TEXT("sockets"), {MakeShared<FJsonValueObject>(Socket)});
		DocumentArgs->SetArrayField(TEXT("retargeting"), {MakeShared<FJsonValueObject>(Mode)});
		TSharedPtr<FJsonValue> Arguments = MakeShared<FJsonValueObject>(DocumentArgs);
		TestTrue(TEXT("document aliases use the existing native parser"), UeAgentAnimationAuthoring::ProjectDocumentOriginalIntent(Arguments, RegisterError));
		TestEqual(TEXT("socket name uses native trimming"), Socket->GetStringField(TEXT("name")), FString(TEXT("SocketIntent")));
		TestEqual(TEXT("retarget alias uses native enum semantics"), Mode->GetStringField(TEXT("mode")), FString(TEXT("animation_scaled")));
		TestEqual(TEXT("bone identity retains authored spelling"), Mode->GetStringField(TEXT("bone")), FString(TEXT("Spine")));
		Mode->SetStringField(TEXT("mode"), TEXT("no_such_mode"));
		TestFalse(TEXT("normalization cannot admit an unknown mode"), UeAgentAnimationAuthoring::ProjectDocumentOriginalIntent(Arguments, RegisterError));
	}
	UeAgentAuthoringState::FStateAccessorRegistry Accessors;
	{
		FKeyedItemProjectionContract Contract;
		TestTrue(TEXT("shared SmartObject collection contract"), TryGetKeyedItemProjectionContract(TEXT("smart_object_items"), Contract));
		auto Row = [](const TCHAR* Id, const TCHAR* Label, int32 Index) {
			const auto Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("id"), Id); Value->SetStringField(TEXT("kind"), TEXT("behavior"));
			Value->SetStringField(TEXT("unit_id"), FString(TEXT("item:")) + Id);
			Value->SetStringField(TEXT("parent"), TEXT("root")); Value->SetNumberField(TEXT("index"), Index);
			Value->SetStringField(TEXT("label"), Label); return Value;
		};
		const auto BaseRows = MakeShared<FJsonObject>(), Edits = MakeShared<FJsonObject>(), NativeRows = MakeShared<FJsonObject>();
		BaseRows->SetObjectField(TEXT("First"), Row(TEXT("behavior:root:0"), TEXT("FirstValue"), 0));
		BaseRows->SetObjectField(TEXT("Second"), Row(TEXT("behavior:root:1"), TEXT("SecondValue"), 1));
		const auto EditFirst = MakeShared<FJsonObject>(), EditSecond = MakeShared<FJsonObject>();
		EditFirst->SetStringField(TEXT("operation"), TEXT("update")); EditFirst->SetNumberField(TEXT("index"), 18);
		EditSecond->SetStringField(TEXT("operation"), TEXT("update")); EditSecond->SetNumberField(TEXT("index"), 0);
		Edits->SetObjectField(TEXT("First"), EditFirst); Edits->SetObjectField(TEXT("Second"), EditSecond);
		NativeRows->SetObjectField(TEXT("FreshFirstAlias"), Row(TEXT("behavior:root:1"), TEXT("FirstValue"), 1));
		NativeRows->SetObjectField(TEXT("FreshSecondAlias"), Row(TEXT("behavior:root:0"), TEXT("SecondValue"), 0));
		TSharedPtr<FJsonValue> BeforeRows = MakeShared<FJsonValueObject>(BaseRows), DesiredRows = MakeShared<FJsonValueObject>(Edits), ActualRows = MakeShared<FJsonValueObject>(NativeRows);
		TMap<FString, FString> Paths;
		TestTrue(TEXT("derived ordinal identities follow original normalized intent"), TryProjectKeyedItemOriginalIntent(Contract, BeforeRows, DesiredRows, ActualRows, Paths, RegisterError));
		TestEqual(TEXT("first behavior retains its source alias after reordering"), ActualRows->AsObject()->GetObjectField(TEXT("First"))->GetStringField(TEXT("label")), FString(TEXT("FirstValue")));
		TestEqual(TEXT("second behavior retains its source alias after reordering"), ActualRows->AsObject()->GetObjectField(TEXT("Second"))->GetStringField(TEXT("label")), FString(TEXT("SecondValue")));
		TestEqual(TEXT("authored rank is compacted by the shared collection owner"), DesiredRows->AsObject()->GetObjectField(TEXT("First"))->GetIntegerField(TEXT("index")), 1);
		TestFalse(TEXT("native unit bindings are not business intent"), DesiredRows->AsObject()->GetObjectField(TEXT("First"))->HasField(TEXT("unit_id")));
		TestEqual(TEXT("derived identity keeps first authored label"), DesiredRows->AsObject()->GetObjectField(TEXT("First"))->GetStringField(TEXT("label")), ActualRows->AsObject()->GetObjectField(TEXT("First"))->GetStringField(TEXT("label")));
		TestEqual(TEXT("derived identity keeps second authored label"), DesiredRows->AsObject()->GetObjectField(TEXT("Second"))->GetStringField(TEXT("label")), ActualRows->AsObject()->GetObjectField(TEXT("Second"))->GetStringField(TEXT("label")));
		ActualRows->AsObject()->GetObjectField(TEXT("First"))->SetStringField(TEXT("label"), TEXT("WrongValue"));
		TestNotEqual(TEXT("projecting metadata cannot hide a lost explicit value"), DesiredRows->AsObject()->GetObjectField(TEXT("First"))->GetStringField(TEXT("label")), ActualRows->AsObject()->GetObjectField(TEXT("First"))->GetStringField(TEXT("label")));
	}
	{
		FKeyedItemProjectionContract Contract;
		TryGetKeyedItemProjectionContract(TEXT("smart_object_items"), Contract);
		auto Row = [](const TCHAR* Id, int32 Index) {
			const auto Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("id"), Id); Value->SetStringField(TEXT("kind"), TEXT("slot"));
			Value->SetStringField(TEXT("parent"), TEXT("root")); Value->SetNumberField(TEXT("index"), Index);
			return Value;
		};
		const auto Base = MakeShared<FJsonObject>(), Edits = MakeShared<FJsonObject>(), Native = MakeShared<FJsonObject>();
		Base->SetObjectField(TEXT("Item#3"), Row(TEXT("stable-first"), 0));
		Base->SetObjectField(TEXT("Item#2"), Row(TEXT("stable-second"), 1));
		const auto First = Row(TEXT("stable-first"), 18), Second = Row(TEXT("stable-second"), 0);
		First->SetStringField(TEXT("operation"), TEXT("update")); Second->SetStringField(TEXT("operation"), TEXT("update"));
		Edits->SetObjectField(TEXT("Item"), First); Edits->SetObjectField(TEXT("Item#2"), Second);
		Native->SetObjectField(TEXT("OtherAlias"), Row(TEXT("stable-first"), 1));
		Native->SetObjectField(TEXT("OtherAlias#2"), Row(TEXT("stable-second"), 0));
		TSharedPtr<FJsonValue> Before = MakeShared<FJsonValueObject>(Base), Desired = MakeShared<FJsonValueObject>(Edits), Actual = MakeShared<FJsonValueObject>(Native);
		TMap<FString, FString> Paths;
		TestTrue(TEXT("independent capture aliases align by immutable native identity"), TryProjectKeyedItemOriginalIntent(Contract, Before, Desired, Actual, Paths, RegisterError));
		TestEqual(TEXT("capture retains exactly two original identities"), Before->AsObject()->Values.Num(), 2);
		TestEqual(TEXT("intent does not create a third duplicate-label row"), Desired->AsObject()->Values.Num(), 2);
		TestEqual(TEXT("original alias belongs to the right captured item"), Before->AsObject()->GetObjectField(TEXT("Item"))->GetIntegerField(TEXT("index")), 0);
		TestEqual(TEXT("native result uses the same original alias"), Actual->AsObject()->GetObjectField(TEXT("Item"))->GetIntegerField(TEXT("index")), 1);
	}
	{
		FKeyedItemProjectionContract Contract;
		TryGetKeyedItemProjectionContract(TEXT("modules"), Contract);
		auto Row = [](const TCHAR* Unit, int32 Index, double Value) {
			const auto Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("unit_id"), Unit);
			Result->SetNumberField(TEXT("module_index"), Index);
			const auto Inputs = MakeShared<FJsonObject>(); Inputs->SetNumberField(TEXT("ExplicitInput"), Value);
			Result->SetObjectField(TEXT("inputs"), Inputs); return Result;
		};
		const auto Base = MakeShared<FJsonObject>(), Edits = MakeShared<FJsonObject>(), Native = MakeShared<FJsonObject>();
		Base->SetObjectField(TEXT("Assignment"), Row(TEXT("module:first"), 0, 1));
		Base->SetObjectField(TEXT("Assignment#2"), Row(TEXT("module:second"), 1, 2));
		Edits->SetObjectField(TEXT("Assignment"), Row(TEXT("module:first"), 0, 3));
		Edits->SetObjectField(TEXT("Assignment#2"), Row(TEXT("module:second"), 1, 2));
		Native->SetObjectField(TEXT("ChangedNativeTitle"), Row(TEXT("module:first"), 0, 3));
		Native->SetObjectField(TEXT("Assignment"), Row(TEXT("module:second"), 1, 2));
		TSharedPtr<FJsonValue> Before = MakeShared<FJsonValueObject>(Base), Desired = MakeShared<FJsonValueObject>(Edits), Actual = MakeShared<FJsonValueObject>(Native);
		TMap<FString, FString> Paths, NativeAliases;
		TestTrue(TEXT("aggregate native title changes rebind through captured semantic identity"),
			TryProjectKeyedItemOriginalIntent(Contract, Before, Desired, Actual, Paths, RegisterError, nullptr, &NativeAliases));
		TestEqual(TEXT("first original alias retains its input"), Actual->AsObject()->GetObjectField(TEXT("Assignment"))->GetObjectField(TEXT("inputs"))->GetNumberField(TEXT("ExplicitInput")), 3.0);
		TestEqual(TEXT("second original alias retains its input"), Actual->AsObject()->GetObjectField(TEXT("Assignment#2"))->GetObjectField(TEXT("inputs"))->GetNumberField(TEXT("ExplicitInput")), 2.0);
		TestEqual(TEXT("native type/source metadata follows the same alias mapping"), NativeAliases.FindRef(TEXT("ChangedNativeTitle")), FString(TEXT("Assignment")));
		Actual->AsObject()->GetObjectField(TEXT("Assignment"))->GetObjectField(TEXT("inputs"))->SetNumberField(TEXT("ExplicitInput"), 4);
		TestNotEqual(TEXT("identity normalization cannot hide a lost module input"), Actual->AsObject()->GetObjectField(TEXT("Assignment"))->GetObjectField(TEXT("inputs"))->GetNumberField(TEXT("ExplicitInput")), Desired->AsObject()->GetObjectField(TEXT("Assignment"))->GetObjectField(TEXT("inputs"))->GetNumberField(TEXT("ExplicitInput")));
	}
	{
		auto* Board = NewObject<UBlackboardData>(CreatePackage(TEXT("/Game/UAI/Tests/FieldIdentityFixture")), TEXT("FieldIdentityFixture"));
		FBlackboardEntry Key;
		Key.EntryName = TEXT("NativeFocus");
		Key.KeyType = NewObject<UBlackboardKeyType_Float>(Board);
		Board->Keys.Add(Key);
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		// Match the public reader's registration order: identities precede state accessors.
		if (!TestTrue(TEXT("register native Blackboard authoring"), UeAgentReflectionAuthoring::RegisterAuthoring(RegisterError))) { AddError(RegisterError.ToPublicDetail()); return false; }
		if (!TestTrue(TEXT("build native Blackboard requests"), UeAgentBlackboardAuthoring::AppendStructuralReadRequests(
				*Board, TEXT("/Game/UAI/Tests/FieldIdentityFixture"), Requests, RegisterError))) { AddError(RegisterError.ToPublicDetail()); return false; }
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!TestTrue(TEXT("capture native Blackboard key"), UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))) return false;
		bool bFound = false;
		for (const auto& Request : Requests) if (Request.SemanticId == UeAgentBlackboardAuthoring::KeySemanticId) {
			UeAgentAuthoringState::FStateEntry Entry;
			if (!Capture.Snapshot.FindEntry(Request.SemanticId, Request.TargetIdentity.CanonicalKey, Entry)) continue;
			FString NativeId;
			Entry.Value->AsObject()->TryGetStringField(TEXT("id"), NativeId);
			TestEqual(TEXT("native capture retains the mutation receipt identity"), NativeId, Request.TargetIdentity.GetSelector(TEXT("key_name")));
			bFound |= NativeId == TEXT("NativeFocus");
		}
		TestTrue(TEXT("native key entry was inspected"), bFound);
	}
	{
		FKeyedItemProjectionContract Contract;
		TryGetKeyedItemProjectionContract(TEXT("blackboard_keys"), Contract);
		const auto Edits = MakeShared<FJsonObject>(), Arguments = MakeShared<FJsonObject>(), Native = MakeShared<FJsonObject>();
		const auto Created = MakeShared<FJsonObject>();
		Created->SetStringField(TEXT("operation"), TEXT("create"));
		Created->SetStringField(TEXT("name"), TEXT("Focus"));
		Edits->SetObjectField(TEXT("NewFocus"), Created);
		const auto Argument = MakeShared<FJsonObject>();
		Argument->SetStringField(TEXT("id"), TEXT("native-focus"));
		Argument->SetStringField(TEXT("name"), TEXT("Focus"));
		Argument->SetNumberField(TEXT("index"), -1);
		Arguments->SetObjectField(TEXT("NewFocus"), Argument);
		const auto NativeRow = UeAgentAuthoringState::CloneCanonicalValue(MakeShared<FJsonValueObject>(Argument))->AsObject();
		NativeRow->SetStringField(TEXT("unit_id"), TEXT("composed:focus"));
		NativeRow->SetNumberField(TEXT("index"), 0);
		Native->SetObjectField(TEXT("Focus"), NativeRow);
		TSharedPtr<FJsonValue> Before = MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()),
			Desired = MakeShared<FJsonValueObject>(Edits), Actual = MakeShared<FJsonValueObject>(Native);
		TMap<FString, FString> Paths, Aliases;
		TestTrue(TEXT("creation receipt native ID binds the composed unit ID"),
			TryProjectKeyedItemOriginalIntent(Contract, Before, Desired, Actual, Paths, RegisterError, Arguments, &Aliases));
		TestEqual(TEXT("new native item retains authored creation alias"), Aliases.FindRef(TEXT("Focus")), FString(TEXT("NewFocus")));
		TestFalse(TEXT("creation candidate name is not persistent business state"), Desired->AsObject()->GetObjectField(TEXT("NewFocus"))->HasField(TEXT("name")));
		TestEqual(TEXT("native name is retained without authoring projection"), Actual->AsObject()->GetObjectField(TEXT("NewFocus"))->GetStringField(TEXT("name")), FString(TEXT("Focus")));
		TestEqual(TEXT("native creation identity remains a fact"), Actual->AsObject()->GetObjectField(TEXT("NewFocus"))->GetStringField(TEXT("id")), FString(TEXT("native-focus")));
	}
	{
		FKeyedItemProjectionContract Contract;
		TryGetKeyedItemProjectionContract(TEXT("modules"), Contract);
		auto Row = [](const TCHAR* Id, double Input) {
			const auto Value = MakeShared<FJsonObject>(); Value->SetStringField(TEXT("unit_id"), Id);
			const auto Inputs = MakeShared<FJsonObject>(); Inputs->SetNumberField(TEXT("ExplicitInput"), Input);
			Value->SetObjectField(TEXT("inputs"), Inputs); return Value;
		};
		for (const bool bExistingSurvives : {false, true})
		{
			const auto Base = MakeShared<FJsonObject>(), Edits = MakeShared<FJsonObject>(), Native = MakeShared<FJsonObject>();
			Base->SetObjectField(TEXT("Assignment"), Row(TEXT("existing"), 1));
			Edits->SetObjectField(TEXT("Assignment"), Row(TEXT("existing"), 3));
			Native->SetObjectField(TEXT("Assignment"), Row(TEXT("different-object"), 3));
			if (bExistingSurvives) Native->SetObjectField(TEXT("FreshTitle"), Row(TEXT("existing"), 3));
			TSharedPtr<FJsonValue> Before = MakeShared<FJsonValueObject>(Base), Desired = MakeShared<FJsonValueObject>(Edits), Actual = MakeShared<FJsonValueObject>(Native);
			TMap<FString, FString> Paths, Aliases;
			TestTrue(TEXT("different native identities cannot share a comparison alias"),
				TryProjectKeyedItemOriginalIntent(Contract, Before, Desired, Actual, Paths, RegisterError, nullptr, &Aliases));
			TestEqual(TEXT("no native row was overwritten by a readable-name collision"), Actual->AsObject()->Values.Num(), bExistingSurvives ? 2 : 1);
			TestEqual(TEXT("only the original identity can occupy the edited alias"), Actual->AsObject()->HasField(TEXT("Assignment")), bExistingSurvives);
			TestNotEqual(TEXT("unmatched same-title object is not mistaken for edited object"), Aliases.FindRef(TEXT("Assignment")), FString(TEXT("Assignment")));
		}
	}
	{
		FKeyedItemProjectionContract Contract;
		TryGetKeyedItemProjectionContract(TEXT("smart_object_items"), Contract);
		auto Row = [](const TCHAR* Id, double Index) {
			const auto Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("id"), Id); Value->SetStringField(TEXT("kind"), TEXT("slot"));
			Value->SetStringField(TEXT("parent"), TEXT("root")); Value->SetNumberField(TEXT("index"), Index);
			return Value;
		};
		const auto Base = MakeShared<FJsonObject>(), Edits = MakeShared<FJsonObject>(), Native = MakeShared<FJsonObject>();
		Base->SetObjectField(TEXT("First"), Row(TEXT("first"), 0));
		Base->SetObjectField(TEXT("Second"), Row(TEXT("second"), 1));
		Edits->SetObjectField(TEXT("First"), Row(TEXT("first"), 4294967297.0));
		Edits->SetObjectField(TEXT("Second"), Row(TEXT("second"), 2147483647.0));
		Native->SetObjectField(TEXT("First"), Row(TEXT("first"), 1));
		Native->SetObjectField(TEXT("Second"), Row(TEXT("second"), 0));
		TSharedPtr<FJsonValue> Before = MakeShared<FJsonValueObject>(Base), Desired = MakeShared<FJsonValueObject>(Edits), Actual = MakeShared<FJsonValueObject>(Native);
		TMap<FString, FString> Paths;
		TestTrue(TEXT("finite integral rank is not bounded by native array position width"), TryProjectKeyedItemOriginalIntent(Contract, Before, Desired, Actual, Paths, RegisterError));
		for (const TCHAR* Alias : {TEXT("First"), TEXT("Second")})
			TestEqual(TEXT("large exact ranks compact in native order"), Actual->AsObject()->GetObjectField(Alias)->GetNumberField(TEXT("index")), Desired->AsObject()->GetObjectField(Alias)->GetNumberField(TEXT("index")));
	}
	{
		FKeyedItemProjectionContract Contract;
		TryGetKeyedItemProjectionContract(TEXT("tree_items"), Contract);
		const auto Base = MakeShared<FJsonObject>(), Edits = MakeShared<FJsonObject>(), Native = MakeShared<FJsonObject>(), Arguments = MakeShared<FJsonObject>();
		const auto Source = MakeShared<FJsonObject>(), Argument = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("operation"), TEXT("create")); Source->SetStringField(TEXT("parent"), TEXT("Root"));
		Source->SetStringField(TEXT("label"), TEXT("IntentDelay"));
		Argument->SetStringField(TEXT("id"), TEXT("new-native-id")); Argument->SetStringField(TEXT("kind"), TEXT("node"));
		Argument->SetStringField(TEXT("parent"), TEXT("Root")); Argument->SetStringField(TEXT("placement"), TEXT("task"));
		Argument->SetStringField(TEXT("label"), TEXT("IntentDelay")); Argument->SetNumberField(TEXT("index"), -1);
		Edits->SetObjectField(TEXT("IntentDelay"), Source); Arguments->SetObjectField(TEXT("IntentDelay"), Argument);
		const auto ActualRow = UeAgentAuthoringState::CloneCanonicalValue(MakeShared<FJsonValueObject>(Argument))->AsObject();
		ActualRow->SetNumberField(TEXT("index"), 0); ActualRow->RemoveField(TEXT("label"));
		Native->SetObjectField(TEXT("IntentDelay"), ActualRow);
		TSharedPtr<FJsonValue> Before = MakeShared<FJsonValueObject>(Base), Desired = MakeShared<FJsonValueObject>(Edits), Actual = MakeShared<FJsonValueObject>(Native);
		TMap<FString, FString> Paths;
		TestTrue(TEXT("created rank defaults use frozen constructor arguments"), TryProjectKeyedItemOriginalIntent(Contract, Before, Desired, Actual, Paths, RegisterError, Arguments));
		TestFalse(TEXT("identity projection never invents an omitted native label"), Actual->AsObject()->GetObjectField(TEXT("IntentDelay"))->HasField(TEXT("label")));
		TestEqual(TEXT("explicit input label remains required"), Desired->AsObject()->GetObjectField(TEXT("IntentDelay"))->GetStringField(TEXT("label")), FString(TEXT("IntentDelay")));
		TestEqual(TEXT("created rank uses native constructor order"), Actual->AsObject()->GetObjectField(TEXT("IntentDelay"))->GetNumberField(TEXT("index")), Desired->AsObject()->GetObjectField(TEXT("IntentDelay"))->GetNumberField(TEXT("index")));
		Actual->AsObject()->GetObjectField(TEXT("IntentDelay"))->SetStringField(TEXT("label"), TEXT("LostIntent"));
		TestNotEqual(TEXT("created defaults cannot hide a discarded explicit field"), Actual->AsObject()->GetObjectField(TEXT("IntentDelay"))->GetStringField(TEXT("label")), Desired->AsObject()->GetObjectField(TEXT("IntentDelay"))->GetStringField(TEXT("label")));
	}
	{
		FKeyedItemProjectionContract Contract;
		TryGetKeyedItemProjectionContract(TEXT("blackboard_keys"), Contract);
		const auto Base = MakeShared<FJsonObject>(), Edits = MakeShared<FJsonObject>(), Native = MakeShared<FJsonObject>(), Arguments = MakeShared<FJsonObject>();
		const auto Old = MakeShared<FJsonObject>(), New = MakeShared<FJsonObject>(), Selector = MakeShared<FJsonObject>();
		Old->SetStringField(TEXT("key_type"), TEXT("Vector")); Old->SetNumberField(TEXT("index"), 0);
		Base->SetObjectField(TEXT("Key"), Old);
		const auto Source = UeAgentAuthoringState::CloneCanonicalValue(MakeShared<FJsonValueObject>(Old))->AsObject();
		Source->SetStringField(TEXT("operation"), TEXT("update")); Selector->SetStringField(TEXT("name"), TEXT("Float"));
		Source->SetObjectField(TEXT("candidate"), Selector); Edits->SetObjectField(TEXT("Key"), Source);
		New->SetStringField(TEXT("key_type"), TEXT("Float")); New->SetNumberField(TEXT("index"), 0);
		New->SetStringField(TEXT("id"), TEXT("Key")); Arguments->SetObjectField(TEXT("Key"), New);
		const auto ReplacedNative = UeAgentAuthoringState::CloneCanonicalValue(MakeShared<FJsonValueObject>(New))->AsObject();
		ReplacedNative->SetStringField(TEXT("unit_id"), TEXT("composed:replaced-key"));
		Native->SetObjectField(TEXT("Key"), ReplacedNative);
		TSharedPtr<FJsonValue> Before = MakeShared<FJsonValueObject>(Base), Desired = MakeShared<FJsonValueObject>(Edits), Actual = MakeShared<FJsonValueObject>(Native);
		TMap<FString, FString> Paths;
		TestTrue(TEXT("candidate replacement uses decoded type arguments instead of unchanged old display context"), TryProjectKeyedItemOriginalIntent(Contract, Before, Desired, Actual, Paths, RegisterError, Arguments));
		TestFalse(TEXT("creation selector is not persistent business state"), Desired->AsObject()->GetObjectField(TEXT("Key"))->HasField(TEXT("candidate")));
		TestEqual(TEXT("selected native type retains the original choice"), Actual->AsObject()->GetObjectField(TEXT("Key"))->GetStringField(TEXT("key_type")), Desired->AsObject()->GetObjectField(TEXT("Key"))->GetStringField(TEXT("key_type")));
		Actual->AsObject()->GetObjectField(TEXT("Key"))->SetStringField(TEXT("key_type"), TEXT("Vector"));
		TestNotEqual(TEXT("selector projection cannot hide a lost selected type"), Actual->AsObject()->GetObjectField(TEXT("Key"))->GetStringField(TEXT("key_type")), Desired->AsObject()->GetObjectField(TEXT("Key"))->GetStringField(TEXT("key_type")));
	}
	{
		UeAgentNativeNormalizationIntent::FScope Scope;
		UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
		Identity.CanonicalKey = TEXT("same-native-asset");
		{
			UeAgentNativeNormalizationIntent::FValueScope ValueScope(Identity, TEXT("asset.metadata"));
			UeAgentNativeNormalizationIntent::RecordValue(TEXT("/metadata"), MakeShared<FJsonValueNumber>(1), MakeShared<FJsonValueNumber>(1), RegisterError);
		}
		{
			UeAgentNativeNormalizationIntent::FValueScope ValueScope(Identity, TEXT("asset.document"));
			UeAgentNativeNormalizationIntent::RecordValue(TEXT("/materials"), MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(2), RegisterError);
		}
		TestEqual(TEXT("normalization metadata and document paths do not share the same asset-key bucket"),
			Scope.FindValues(Identity, TEXT("asset.metadata"))->Num(), 1);
		TestEqual(TEXT("normalization document retains its own semantic path"),
			Scope.FindValues(Identity, TEXT("asset.document"))->Last().Path, FString(TEXT("/materials")));
		TestNull(TEXT("a third semantic representation does not inherit another document's proofs"),
			Scope.FindValues(Identity, TEXT("asset.unrelated")));
	}
	UeAgentAuthoringState::FStateAccessorDescriptor Descriptor;
	Descriptor.AccessorId = UeAgentBlueprintAuthoring::StateAccessorId;
	Descriptor.TargetKinds = {TEXT("blueprint_graph")};
	TSharedPtr<FJsonValue> NativeValue;
	bool bNativeReadOnly = false;
	bool bEmitDefault = false;
	UeAgentAuthoringState::ERegistrationResult Registered;
	if (!TestTrue(TEXT("isolated capture accessor"), Accessors.RegisterAccessor(Descriptor,
		[&NativeValue, &bNativeReadOnly, &bEmitDefault](const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& Value, FString&) {
			Value.TargetIdentity = Request.TargetIdentity;
			Value.Type.Kind = TEXT("object");
			Value.Value = NativeValue;
			if (bEmitDefault) {
				Value.Value = UeAgentAuthoringState::CloneCanonicalValue(NativeValue);
				UeAgentAuthoringEncodedTypes::FScope::SetNativeField(Value.Value->AsObject(), TEXT("native_default_field"),
					MakeShared<FJsonValueBoolean>(false), UeAgentAuthoringEncodedTypes::EFieldEmission::NativeDefault);
			}
			if (bNativeReadOnly) Value.bEditable = false;
			return UeAgentAuthoringState::EReadOutcome::Resolved;
		}, Registered, RegisterError))) return false;
	UeAgentAuthoringState::FReadRequest Request;
	Request.SemanticId = UeAgentBlueprintAuthoring::GraphSemanticId;
	Request.TargetIdentity = {TEXT("blueprint_graph_identity"), TEXT("blueprint_graph"),
		TEXT("guid"), StructureAuthoringIdentityKey(), {}};
	FWorkspaceError IntentError;
	UeAgentAuthoringState::FCaptureResult Capture;
	FString JsonError;
	UE::UAI::StateSemantic::CanonicalJson::TryToJsonValue(*StructureValue(Resources, false), NativeValue, JsonError);
	if (!TestTrue(TEXT("capture discarded domain delta"), UeAgentAuthoringState::CaptureSnapshot({Request}, Capture, nullptr, &Accessors))) return false;
	UeAgentAuthoringState::FStateEntry CapturedEntry;
	if (!TestTrue(TEXT("capture entry"), Capture.Snapshot.CopyEntry(0, CapturedEntry))) return false;
	TestTrue(TEXT("an unspecified instance flag inherits DomainMutation"), CapturedEntry.bEditable);
	TestFalse(TEXT("a successful readback cannot hide a discarded original delta"),
		TryVerifySemanticWorkspaceValueIntent(*Prepared, Capture.Snapshot, IntentError));
	TestEqual(TEXT("retention error"), IntentError.GetCode(), FString(TEXT("hjson_import_intent_not_retained")));
	TestEqual(TEXT("original source retained"), IntentError.GetSourceUnit(), FString(TEXT("structures/event-graph/structure.hjson")));
	TestEqual(TEXT("original source line retained"), IntentError.GetLine(), 8);
	auto IntentDiagnostic = IntentError.GetDiagnostic();
	TestFalse(TEXT("retention failure exposes the semantic difference"), IntentDiagnostic.SemanticPointer.IsEmpty());
	bNativeReadOnly = true;
	if (!TestTrue(TEXT("capture explicit native read-only"), UeAgentAuthoringState::CaptureSnapshot({Request}, Capture, nullptr, &Accessors))) return false;
	Capture.Snapshot.CopyEntry(0, CapturedEntry);
	TestFalse(TEXT("explicit native read-only overrides DomainMutation"), CapturedEntry.bEditable);
	bNativeReadOnly = false;

	UE::UAI::StateSemantic::CanonicalJson::TryToJsonValue(*StructureValue(Resources, true), NativeValue, JsonError);
	NativeValue->AsObject()->SetStringField(TEXT("native_revision"), TEXT("settled"));
	NativeValue->AsObject()->GetObjectField(TEXT("graph"))->SetStringField(TEXT("name"), TEXT("NativeSideEffect"));
	const auto NewNode = NativeValue->AsObject()->GetArrayField(TEXT("nodes"))[0]->AsObject();
	NewNode->SetStringField(TEXT("native_default"), TEXT("retained"));
	if (!TestTrue(TEXT("capture retained delta and native side effect"), UeAgentAuthoringState::CaptureSnapshot({Request}, Capture, nullptr, &Accessors))) return false;
	TestTrue(TEXT("unedited native state is not an old-state replay"),
		TryVerifySemanticWorkspaceValueIntent(*Prepared, Capture.Snapshot, IntentError));
	{
		bEmitDefault = true;
		UeAgentAuthoringState::FCaptureResult Presentation, Native;
		TestTrue(TEXT("capture authoring presentation"), UeAgentAuthoringState::CaptureSnapshot({Request}, Presentation, nullptr, &Accessors));
		TestTrue(TEXT("capture independent native facts"), UeAgentAuthoringState::CaptureSnapshot({Request}, Native, nullptr, &Accessors,
			UeAgentAuthoringState::ECaptureValueMode::Native));
		UeAgentAuthoringState::FStateEntry Presented, Full;
		Presentation.Snapshot.CopyEntry(0, Presented); Native.Snapshot.CopyEntry(0, Full);
		TestFalse(TEXT("export presentation still omits proven default"), Presented.Value->AsObject()->HasField(TEXT("native_default_field")));
		TestTrue(TEXT("native intent capture keeps default facts"), Full.Value->AsObject()->HasField(TEXT("native_default_field")));
		TestTrue(TEXT("unrequested native default is not an import requirement"),
			TryVerifySemanticWorkspaceValueIntent(*Prepared, Native.Snapshot, IntentError));
		bEmitDefault = false;
	}
	{
		const auto BeforeArray = ObjectValue({{TEXT("items"), ArrayValue({StringValue(TEXT("a"), Resources), StringValue(TEXT("b"), Resources)}, Resources)}}, Resources);
		const auto DesiredArray = ObjectValue({{TEXT("items"), ArrayValue({StringValue(TEXT("a"), Resources), StringValue(TEXT("c"), Resources)}, Resources)}}, Resources);
		const auto& Identity = Prepared->GetChangeSetDraft().GetChanges()[0].GetIdentity();
		FChangeSetDraft Draft(Prepared->GetChangeSetDraft().GetDescriptorIdentity(), TEXT("array-intent"), TEXT("array-intent"), Snapshot,
			{FChangeDraft(Identity, EChangeIntent::Update, BeforeArray, DesiredArray, {}, FString(), {})});
		FPreparedAuthoringImport ArrayPrepared(Workspace, Draft, Prepared->GetPlanningRequest(), Prepared->GetPreflightResult());
		NativeValue->AsObject()->SetArrayField(TEXT("items"), {MakeShared<FJsonValueString>(TEXT("wrong")), MakeShared<FJsonValueString>(TEXT("c"))});
		UeAgentAuthoringState::FCaptureResult ArrayCapture;
		if (!UeAgentAuthoringState::CaptureSnapshot({Request}, ArrayCapture, nullptr, &Accessors)) return false;
		TestFalse(TEXT("array replacement verifies unchanged elements as well as the changed index"),
			TryVerifySemanticWorkspaceValueIntent(ArrayPrepared, ArrayCapture.Snapshot, IntentError));
		NativeValue->AsObject()->SetArrayField(TEXT("items"), {MakeShared<FJsonValueString>(TEXT("a")), MakeShared<FJsonValueString>(TEXT("c"))});
		if (!UeAgentAuthoringState::CaptureSnapshot({Request}, ArrayCapture, nullptr, &Accessors)) return false;
		TestTrue(TEXT("complete requested array replacement is retained"),
			TryVerifySemanticWorkspaceValueIntent(ArrayPrepared, ArrayCapture.Snapshot, IntentError));
		NativeValue->AsObject()->RemoveField(TEXT("items"));
	}
	{
		UeAgentNativeNormalizationIntent::FScope Scope;
		UeAgentNativeNormalizationIntent::FValueScope Target(Request.TargetIdentity);
		const auto Source = MakeShared<FJsonValueString>(TEXT("candidate:fixture"));
		const auto Normalized = MakeShared<FJsonValueString>(TEXT("native:early"));
		TestTrue(TEXT("record frozen native argument at an array member"),
			UeAgentNativeNormalizationIntent::RecordValue(TEXT("/nodes/0/id"), Source, Normalized, JsonError));
		NewNode->SetStringField(TEXT("id"), TEXT("native:allocated"));
		if (!TestTrue(TEXT("capture normalized document field"), UeAgentAuthoringState::CaptureSnapshot({Request}, Capture, nullptr, &Accessors))) return false;
		TestTrue(TEXT("finalize only from the completed isolated proposal"), UeAgentNativeNormalizationIntent::FinalizeValues(Capture.Snapshot, JsonError));
		TestEqual(TEXT("finalization keeps the immutable native argument"), (*Scope.FindValues(Request.TargetIdentity))[0].SourceValue->AsString(), FString(TEXT("candidate:fixture")));
		TestTrue(TEXT("native document normalization retains the original argument"),
			TryVerifySemanticWorkspaceValueIntent(*Prepared, Capture.Snapshot, IntentError, {}, &Scope));
		NewNode->SetStringField(TEXT("id"), TEXT("wrong:final"));
		UeAgentAuthoringState::CaptureSnapshot({Request}, Capture, nullptr, &Accessors);
		TestFalse(TEXT("document proof cannot accept an incorrect final value"),
			TryVerifySemanticWorkspaceValueIntent(*Prepared, Capture.Snapshot, IntentError, {}, &Scope));
	}
	{
		UeAgentNativeNormalizationIntent::FScope Scope;
		UeAgentNativeNormalizationIntent::FValueScope Target(Request.TargetIdentity, Request.SemanticId);
		TestTrue(TEXT("record the resolved argument independently of native application"),
			UeAgentNativeNormalizationIntent::RecordValue(TEXT("/nodes/0/id"),
				MakeShared<FJsonValueString>(TEXT("candidate:fixture")), MakeShared<FJsonValueString>(TEXT("native:resolved")),
				JsonError, nullptr, nullptr, UeAgentNativeNormalizationIntent::EValueCompletion::ResolvedArgument));
		NewNode->SetStringField(TEXT("id"), TEXT("wrong:proposal"));
		if (!UeAgentAuthoringState::CaptureSnapshot({Request}, Capture, nullptr, &Accessors)) return false;
		TestTrue(TEXT("complete proposal containing an incorrect application"), UeAgentNativeNormalizationIntent::FinalizeValues(Capture.Snapshot, JsonError));
		TestEqual(TEXT("proposal cannot redefine an independently resolved argument"),
			(*Scope.FindValues(Request.TargetIdentity, Request.SemanticId))[0].NormalizedValue->AsString(), FString(TEXT("native:resolved")));
		TestFalse(TEXT("resolved argument exposes incorrect native application"),
			TryVerifySemanticWorkspaceValueIntent(*Prepared, Capture.Snapshot, IntentError, {}, &Scope));
		NewNode->SetStringField(TEXT("id"), TEXT("native:resolved"));
		if (!UeAgentAuthoringState::CaptureSnapshot({Request}, Capture, nullptr, &Accessors)) return false;
		TestTrue(TEXT("resolved argument accepts the independently correct actual value"),
			TryVerifySemanticWorkspaceValueIntent(*Prepared, Capture.Snapshot, IntentError, {}, &Scope));
	}
	{
		UeAgentNativeNormalizationIntent::FScope Scope;
		UeAgentNativeNormalizationIntent::FValueScope Target(Request.TargetIdentity);
		TSharedPtr<FJsonValue> SourceRoot;
		UE::UAI::StateSemantic::CanonicalJson::TryToJsonValue(*StructureValue(Resources, true), SourceRoot, JsonError);
		const auto SourceRows = SourceRoot->AsObject()->TryGetField(TEXT("nodes"));
		TestTrue(TEXT("record a complete owned collection argument"),
			UeAgentNativeNormalizationIntent::RecordValue(TEXT("/nodes"), SourceRows, SourceRows, JsonError));
		NewNode->SetStringField(TEXT("id"), TEXT("native:allocated"));
		if (!TestTrue(TEXT("capture completed collection proposal"), UeAgentAuthoringState::CaptureSnapshot({Request}, Capture, nullptr, &Accessors))) return false;
		TestTrue(TEXT("finalize collection from isolated native result"), UeAgentNativeNormalizationIntent::FinalizeValues(Capture.Snapshot, JsonError));
		TestTrue(TEXT("owned collection accepts native identity and defaults after proving the argument"),
			TryVerifySemanticWorkspaceValueIntent(*Prepared, Capture.Snapshot, IntentError, {}, &Scope));
		NewNode->SetStringField(TEXT("id"), TEXT("wrong:after-proposal"));
		UeAgentAuthoringState::CaptureSnapshot({Request}, Capture, nullptr, &Accessors);
		TestFalse(TEXT("owned collection still compares independent actual against the frozen proposal"),
			TryVerifySemanticWorkspaceValueIntent(*Prepared, Capture.Snapshot, IntentError, {}, &Scope));
	}
	{
		UeAgentNativeNormalizationIntent::FScope Scope;
		UeAgentNativeNormalizationIntent::FValueScope Target(Request.TargetIdentity);
		TestTrue(TEXT("capture a deliberately incorrect decoded source"),
			UeAgentNativeNormalizationIntent::RecordValue(TEXT("/nodes/0/id"),
				MakeShared<FJsonValueString>(TEXT("wrong:source")), MakeShared<FJsonValueString>(TEXT("native:allocated")), JsonError));
		NewNode->SetStringField(TEXT("id"), TEXT("native:allocated"));
		UeAgentAuthoringState::CaptureSnapshot({Request}, Capture, nullptr, &Accessors);
		TestFalse(TEXT("document normalization cannot self-validate a lost original argument"),
			TryVerifySemanticWorkspaceValueIntent(*Prepared, Capture.Snapshot, IntentError, {}, &Scope));
		TestEqual(TEXT("document conversion failure is distinct"), IntentError.GetCode(), FString(TEXT("hjson_import_intent_conversion_not_retained")));
	}
	NewNode->SetStringField(TEXT("id"), TEXT("discarded-original-id"));
	if (!TestTrue(TEXT("capture discarded new-record field"), UeAgentAuthoringState::CaptureSnapshot({Request}, Capture, nullptr, &Accessors))) return false;
	TestFalse(TEXT("new-record defaults cannot hide a lost original field"),
		TryVerifySemanticWorkspaceValueIntent(*Prepared, Capture.Snapshot, IntentError));
	IntentDiagnostic = IntentError.GetDiagnostic();
	TestEqual(TEXT("new-record field difference pointer"), IntentDiagnostic.SemanticPointer, FString(TEXT("/nodes/0/id")));
	if (!TestTrue(TEXT("field difference values are present"), IntentDiagnostic.ExpectedValue.IsValid() && IntentDiagnostic.ActualValue.IsValid())) return false;
	TestEqual(TEXT("diagnostic retains original requested value"), IntentDiagnostic.ExpectedValue->AsString(), FString(TEXT("candidate:fixture")));
	TestEqual(TEXT("diagnostic retains actual value"), IntentDiagnostic.ActualValue->AsString(), FString(TEXT("discarded-original-id")));
	{
		UeAgentAuthoringEncodedTypes::FScope Types;
		auto* Light = NewObject<UPointLightComponent>();
		Light->Intensity = 0.15f;
		Light->LightmassSettings.IndirectLightingSaturation = 0.15f;
		UeAgentDetailsAuthoringService::FCapturePolicy Policy;
		Policy.IncludeProperty = [](const FProperty* Property, const void*, const UObject*) { return Property->GetFName() == TEXT("Intensity") || Property->GetFName() == TEXT("LightmassSettings"); };
		UeAgentAuthoringState::FStateSnapshot Native;
		TSharedPtr<FJsonObject> LightDocument, Categories;
		if (!TestTrue(TEXT("capture reflected compact record"), UeAgentDetailsAuthoringService::CaptureObjectProperties(Light, Policy, Native, LightDocument, JsonError)
			&& UeAgentDetailsAuthoringService::ProjectReadableProperties(Native, Categories, JsonError))) return false;
		UeAgentAuthoringState::FStateEntry Actual = Aliased;
		Actual.NativeValue.Reset(); Actual.Type.Kind = TEXT("object"); Actual.ValueDefinition.Reset();
		Actual.Value = MakeShared<FJsonValueObject>(Categories);
		Actual.ComparisonValueDefinition = MakeShared<FJsonValueObject>(Types.BuildDefinition(Actual.Value));
		auto Expected = Actual; Expected.Value = UeAgentAuthoringState::CloneCanonicalValue(Actual.Value);
		TSharedPtr<FJsonObject> Property;
		for (const auto& Category : Expected.Value->AsObject()->Values)
			if (Category.Value->AsObject()->HasField(TEXT("Intensity"))) Property = Category.Value->AsObject()->GetObjectField(TEXT("Intensity"));
		if (!TestTrue(TEXT("compact reflected value has its original field"), Property.IsValid())) return false;
		Property->SetNumberField(TEXT("value"), 0.15);
		UeAgentAuthoringSemanticCompare::FEntryComparison Comparison;
		UeAgentAuthoringSemanticCompare::FCompareOptions Options; Options.bCompareAvailability = false;
		TestTrue(TEXT("compact reflected projection retains native float storage"), UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && Comparison.bEquivalent);
		Property->SetNumberField(TEXT("value"), 0.16);
		TestTrue(TEXT("compact reflected projection still detects a lost edit"), UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && !Comparison.bEquivalent);
		Property->SetNumberField(TEXT("value"), 0.15);
		TSharedPtr<FJsonObject> Nested;
		for (const auto& Category : Expected.Value->AsObject()->Values)
			if (Category.Value->AsObject()->HasField(TEXT("LightmassSettings")))
				Nested = Category.Value->AsObject()->GetObjectField(TEXT("LightmassSettings"))->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"));
		if (!TestTrue(TEXT("compact nested reflection fields retained"), Nested && Nested->HasField(TEXT("IndirectLightingSaturation")))) return false;
		Nested->SetNumberField(TEXT("IndirectLightingSaturation"), 0.15);
		TestTrue(TEXT("compact nested member retains native float type"), UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && Comparison.bEquivalent);
		Nested->SetNumberField(TEXT("IndirectLightingSaturation"), 0.16);
		TestTrue(TEXT("compact nested member retains lost-edit detection"), UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && !Comparison.bEquivalent);
	}
	{
		using namespace UE::UAI::Authoring::Timeline;
		UeAgentAuthoringEncodedTypes::FScope Types;
		FMovieSceneFloatChannel Channel;
		const auto ChannelValue = ScalarChannel::CaptureValue(Channel, FMovieSceneFloatValue(0.15f));
		UeAgentAuthoringState::FStateEntry Actual = Aliased;
		Actual.NativeValue.Reset(); Actual.Type = UeAgentAuthoringTypeDescriptors::DescribeScalar<float>(); Actual.ValueDefinition.Reset();
		Actual.Value = ChannelValue;
		Actual.ComparisonValueDefinition = MakeShared<FJsonValueObject>(Types.BuildDefinition(ChannelValue));
		auto Expected = Actual; Expected.Value = MakeShared<FJsonValueString>(TEXT("0.15"));
		UeAgentAuthoringSemanticCompare::FEntryComparison Comparison;
		UeAgentAuthoringSemanticCompare::FCompareOptions Options; Options.bCompareAvailability = false;
		TestTrue(TEXT("timeline scalar lexeme compares through native float codec"), UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && Comparison.bEquivalent);
		Expected.Value = MakeShared<FJsonValueString>(TEXT("0.16"));
		TestTrue(TEXT("timeline lexical carrier does not hide a different float"), UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && !Comparison.bEquivalent);
		const auto Lexical = ScalarChannel::CaptureValueDefinition(FMovieSceneIntegerChannel());
		const auto Integer = Lexical->GetObjectField(TEXT("type"));
		const auto IntegerComparison = UeAgentPropertyValueCodec::CompareCanonicalValues(Integer, Lexical, MakeShared<FJsonValueString>(TEXT("42")), MakeShared<FJsonValueString>(TEXT("42")));
		TestTrue(TEXT("integer lexeme uses the native integer parser"), IntegerComparison.IsSet() && IntegerComparison->bEquivalent);
		const auto OverflowComparison = UeAgentPropertyValueCodec::CompareCanonicalValues(Integer, Lexical, MakeShared<FJsonValueString>(TEXT("2147483648")), MakeShared<FJsonValueString>(TEXT("2147483648")));
		TestFalse(TEXT("integer lexical storage enforces native range"), OverflowComparison.IsSet() && OverflowComparison->bEquivalent);
		const auto Boolean = ScalarChannel::CaptureValueDefinition(FMovieSceneBoolChannel());
		const auto BooleanComparison = UeAgentPropertyValueCodec::CompareCanonicalValues(Boolean->GetObjectField(TEXT("type")), Boolean, MakeShared<FJsonValueString>(TEXT("false")), MakeShared<FJsonValueString>(TEXT("false")));
		TestTrue(TEXT("boolean lexical storage retains false"), BooleanComparison.IsSet() && BooleanComparison->bEquivalent);
	}
	{
		UeAgentAuthoringEncodedTypes::FScope Types;
		for (const bool bArray : {false, true})
		{
			FMetasoundFrontendLiteral Literal;
			if (bArray) Literal.Set(TArray<float>{0.15f}); else Literal.Set(0.15f);
			const auto Encoded = UeAgentAudioAuthoring::EncodeMetaSoundLiteral(Literal);
			if (!TestTrue(TEXT("native MetaSound literal encodes"), Encoded.IsValid())) return false;
			UeAgentAuthoringState::FStateEntry Actual = Aliased;
			Actual.NativeValue.Reset(); Actual.Type.Kind = TEXT("object"); Actual.ValueDefinition.Reset();
			Actual.Value = MakeShared<FJsonValueObject>(Encoded);
			Actual.ComparisonValueDefinition = MakeShared<FJsonValueObject>(Types.BuildDefinition(Actual.Value));
			auto Expected = Actual; Expected.Value = UeAgentAuthoringState::CloneCanonicalValue(Actual.Value);
			const auto SetRequested = [&](double Number) {
				if (bArray) Expected.Value->AsObject()->SetArrayField(TEXT("value"), {MakeShared<FJsonValueNumber>(Number)});
				else Expected.Value->AsObject()->SetNumberField(TEXT("value"), Number);
			};
			UeAgentAuthoringSemanticCompare::FEntryComparison Comparison;
			UeAgentAuthoringSemanticCompare::FCompareOptions Options; Options.bCompareAvailability = false;
			SetRequested(0.15);
			TestTrue(TEXT("MetaSound scalar or array retains float type"), UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && Comparison.bEquivalent);
			SetRequested(0.16);
			TestTrue(TEXT("MetaSound literal detects real loss"), UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && !Comparison.bEquivalent);
		}
	}
	{
		using namespace UeAgentRiggingAuthoring;
		auto* Rig = Cast<UControlRigBlueprint>(CreateFactorySeed(EAssetKind::ControlRig, GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UControlRigBlueprint::StaticClass()), RF_Transient, JsonError));
		if (!TestNotNull(TEXT("native ControlRig fixture"), Rig)) return false;
		auto* Hierarchy = Rig->GetHierarchy();
		auto* Controller = Hierarchy->GetController(true);
		FRigControlSettings Settings; Settings.ControlType = ERigControlType::Float;
		Controller->AddControl(TEXT("IntentFloat"), FRigElementKey(), Settings, FRigControlValue::Make(0.15f), FTransform::Identity, FTransform::Identity, false, false);
		const auto CurveKey = Controller->AddCurve(TEXT("IntentCurve"), 0.15f, false, false);
		// UE creates a curve with bIsValueSet=false; the authoring owner sets the
		// authored value explicitly after native creation as well.
		Hierarchy->SetCurveValue(CurveKey, 0.15f, false);
		UeAgentAuthoringEncodedTypes::FScope Types;
		TSharedPtr<FJsonObject> RigDocument;
		if (!TestTrue(TEXT("capture native ControlRig initial values"), BuildDocument(EAssetKind::ControlRig, *Rig, RigDocument, JsonError))) return false;
		for (const auto& Row : RigDocument->GetObjectField(TEXT("hierarchy"))->GetObjectField(TEXT("elements"))->Values)
		{
			const TCHAR* ValueField = Row.Key.StartsWith(TEXT("control:")) ? TEXT("initial_value") : TEXT("value");
			UeAgentAuthoringState::FStateEntry Actual = Aliased;
			Actual.NativeValue.Reset(); Actual.Type.Kind = TEXT("object"); Actual.ValueDefinition.Reset();
			Actual.Value = Row.Value->AsObject()->TryGetField(ValueField);
			if (!TestTrue(TEXT("ControlRig float field exists"), Actual.Value.IsValid())) return false;
			Actual.ComparisonValueDefinition = MakeShared<FJsonValueObject>(Types.BuildDefinition(Actual.Value));
			auto Expected = Actual; Expected.Value = UeAgentAuthoringState::CloneCanonicalValue(Actual.Value);
			Expected.Value->AsObject()->SetNumberField(TEXT("literal"), 0.15);
			UeAgentAuthoringSemanticCompare::FEntryComparison Comparison;
			UeAgentAuthoringSemanticCompare::FCompareOptions Options; Options.bCompareAvailability = false;
			if (!TestTrue(FString(TEXT("ControlRig native float retains storage: ")) + Row.Key,
				UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && Comparison.bEquivalent))
			{
				FString DefinitionText;
				FJsonSerializer::Serialize(Actual.ComparisonValueDefinition->AsObject().ToSharedRef(), TJsonWriterFactory<>::Create(&DefinitionText));
				AddInfo(FString::Printf(TEXT("native=%.17g; difference=%s; type=%s"),
					Actual.Value->AsObject()->GetNumberField(TEXT("literal")), *Comparison.ValueDifferencePath, *DefinitionText));
			}
			Expected.Value->AsObject()->SetNumberField(TEXT("literal"), 0.16);
			TestTrue(TEXT("ControlRig still detects an incorrect float"), UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison) && !Comparison.bEquivalent);
		}
	}
	{
		auto* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
		if (!TestTrue(TEXT("native skeleton source"), Mesh && Mesh->GetSkeleton())) return false;
		auto* Skeleton = DuplicateObject<USkeleton>(Mesh->GetSkeleton(), GetTransientPackage());
		TSharedPtr<FJsonObject> SkeletonDocument;
		if (!TestTrue(TEXT("capture skeleton readonly bones"), UeAgentAnimationAuthoring::BuildDocument(UeAgentAnimationAuthoring::EAssetKind::Skeleton, *Skeleton, SkeletonDocument, JsonError))) return false;
		const auto Bones = UeAgentAuthoringState::CloneCanonicalValue(SkeletonDocument->TryGetField(TEXT("bones")));
		if (!TestTrue(TEXT("skeleton has native bones"), !Bones->AsArray().IsEmpty())) return false;
		const auto Transform = Bones->AsArray()[0]->AsObject()->GetObjectField(TEXT("reference_transform"));
		auto Rotation = Transform->GetArrayField(TEXT("rotation"));
		Rotation[1] = MakeShared<FJsonValueNumber>(Rotation[1]->AsNumber() + 360.0);
		Transform->SetArrayField(TEXT("rotation"), Rotation);
		const auto Requested = MakeShared<FJsonObject>(); Requested->SetField(TEXT("bones"), Bones);
		UeAgentAnimationAuthoring::FMutationStats Stats;
		TestTrue(TEXT("temporary animation validation preserves transform type and winding semantics"),
			UeAgentAnimationAuthoring::ApplyDocument(UeAgentAnimationAuthoring::EAssetKind::Skeleton, *Skeleton, Requested, Stats, JsonError));
		Rotation[1] = MakeShared<FJsonValueNumber>(Rotation[1]->AsNumber() + 15.0);
		Transform->SetArrayField(TEXT("rotation"), Rotation);
		TestFalse(TEXT("readonly animation guard still rejects real transform change"),
			UeAgentAnimationAuthoring::ApplyDocument(UeAgentAnimationAuthoring::EAssetKind::Skeleton, *Skeleton, Requested, Stats, JsonError));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceImportComponentTreeDraftTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace.ComponentTreeDraft",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceImportComponentTreeDraftTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FUeAgentResourceTracker Resources = MakeImportTracker();
	const FAuthoringModelRef Model = MakeComponentTreeModel(Resources);
	const FUeAgentCanonicalSnapshotRef Snapshot = MakeStructureSnapshot(Resources);
	const FResolvedSemanticWorkspaceRef Workspace = MakeShared<FResolvedSemanticWorkspace>(
		EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State, Model,
		Snapshot->GetRevision(), TArray<FStructuralEdit>(),
		TArray<FString>({TEXT("structures/components/structure.hjson")}),
		TEXT(""), TEXT(""), TArray<FString>(), Model->GetSemanticFingerprint(),
		TOptional<TArray<FString>>(
			TArray<FString>({TEXT("structure:blueprint-components")})));
	TOptional<FChangeSetDraft> Draft;
	FSemanticWorkspaceImportError Error;
	TestTrue(
		TEXT("component-tree root map forms a semantic change draft"),
		TryBuildSemanticChangeSetDraft(
			Workspace, Snapshot,
			FUeAgentDescriptorIdentity(
				TEXT("uai.hjson.semantic.import"), 1,
				TEXT("sha256:5555555555555555555555555555555555555555555555555555555555555555")),
			TEXT("component-tree-draft"), TEXT("component-tree-draft:1"),
			Resources, Draft, Error));
	TestTrue(TEXT("component-tree draft is present"), Draft.IsSet());
	if (Draft.IsSet())
	{
		TestEqual(TEXT("component-tree draft contains one keyed-item change"),
			Draft->GetChanges().Num(), 1);
		if (Draft->GetChanges().Num() == 1)
		{
			const FChangeDraft& Change = Draft->GetChanges()[0];
			TestEqual(TEXT("component-tree draft uses the common keyed-item identity"),
				Change.GetIdentity().GetTargetKind(),
				FString(TEXT("authoring_keyed_item_desired_state")));
			bool bHasComponents = false;
			if (Change.GetProposedAfter().IsValid())
			{
				for (const TPair<FString, FUeAgentCanonicalValueRef>& Field :
					Change.GetProposedAfter()->GetObjectItems())
				{
					bHasComponents |= Field.Key == TEXT("components");
				}
			}
			TestTrue(TEXT("component-tree proposed object retains components"),
				bHasComponents);
		}
	}
	FSemanticWorkspaceChangeProjection Projection;
	FPreparedAuthoringImportPtr PreparedImport;
	TestTrue(
		TEXT("component-tree root enters the common semantic projection"),
		TryPrepareSemanticWorkspaceChanges(
			Workspace, Model, Snapshot,
			FUeAgentDescriptorIdentity(
				TEXT("uai.hjson.semantic.import"), 1,
				TEXT("sha256:5555555555555555555555555555555555555555555555555555555555555555")),
			TEXT("component-tree-projection"), TEXT("component-tree-projection:1"),
			FUeAgentCallerContext(
				TEXT("component-tree-projection-test"),
				{TEXT("authoring.write"), TEXT("authoring.structure")},
				TEXT("component-tree-projection")),
			TEXT("/Game/HX013/BP_Structural"), Resources, Projection,
			PreparedImport, Error));
	TestTrue(TEXT("component-tree projection reports a real change"),
		Projection.HasChanges());
	TestTrue(TEXT("component-tree root id is exposed as changed"),
		Projection.ChangedUnitIds.Contains(TEXT("structure:blueprint-components")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceImportMemberCollectionDraftTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace.MemberCollectionDraft",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceImportMemberCollectionDraftTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FUeAgentResourceTracker Resources = MakeImportTracker();
	const FAuthoringModelRef Model = MakeMemberCollectionModel(Resources);
	const FUeAgentCanonicalSnapshotRef Snapshot = MakeStructureSnapshot(Resources);
	const FString StructureId = TEXT("structure:blueprint-members");
	const FResolvedSemanticWorkspaceRef Workspace = MakeShared<FResolvedSemanticWorkspace>(
		EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State, Model,
		Snapshot->GetRevision(), TArray<FStructuralEdit>(),
		TArray<FString>({TEXT("structures/members/structure.hjson")}),
		TEXT(""), TEXT(""), TArray<FString>(), Model->GetSemanticFingerprint(),
		TOptional<TArray<FString>>(TArray<FString>({StructureId})));
	TOptional<FChangeSetDraft> Draft;
	FSemanticWorkspaceImportError Error;
	const FUeAgentDescriptorIdentity Descriptor(
		TEXT("uai.hjson.semantic.import"), 1,
		TEXT("sha256:") + FString::ChrN(64, TEXT('6')));
	TestTrue(
		TEXT("member collection root map forms a semantic change draft"),
		TryBuildSemanticChangeSetDraft(
			Workspace, Snapshot, Descriptor,
			TEXT("member-collection-draft"), TEXT("member-collection-draft:1"),
			Resources, Draft, Error));
	TestTrue(TEXT("member collection draft is present"), Draft.IsSet());
	if (Draft.IsSet() && Draft->GetChanges().Num() == 1)
	{
		bool bHasVariables = false;
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Field :
			Draft->GetChanges()[0].GetProposedAfter()->GetObjectItems())
		{
			bHasVariables |= Field.Key == TEXT("variables");
		}
		TestTrue(TEXT("member collection proposed object retains variables"),
			bHasVariables);
	}
	else
	{
		AddError(TEXT("member collection draft must contain one change"));
	}
	FSemanticWorkspaceChangeProjection Projection;
	FPreparedAuthoringImportPtr PreparedImport;
	TestTrue(
		TEXT("member collection root enters the common semantic projection"),
		TryPrepareSemanticWorkspaceChanges(
			Workspace, Model, Snapshot, Descriptor,
			TEXT("member-collection-projection"),
			TEXT("member-collection-projection:1"),
			FUeAgentCallerContext(
				TEXT("member-collection-projection-test"),
				{TEXT("authoring.write"), TEXT("authoring.structure")},
				TEXT("member-collection-projection")),
			TEXT("/Game/HX013/BP_Members"), Resources, Projection,
			PreparedImport, Error));
	TestTrue(TEXT("member collection projection reports a real change"),
		Projection.HasChanges());
	TestTrue(TEXT("member collection root id is exposed as changed"),
		Projection.ChangedUnitIds.Contains(StructureId));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceImportDomainPropertyProjectionTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace.DomainPropertyProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceImportDomainPropertyProjectionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FUeAgentResourceTracker Resources = MakeImportTracker();
	const FAuthoringModelRef CurrentModel = MakeClassDefaultModel(Resources, TEXT("1.0"));
	const FAuthoringModelRef EditedModel = MakeClassDefaultModel(Resources, TEXT("1.25"));
	const FUeAgentCanonicalSnapshotRef Snapshot = MakeClassDefaultSnapshot(Resources);
	const FResolvedSemanticWorkspaceRef Workspace = MakeShared<FResolvedSemanticWorkspace>(
		EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State, EditedModel,
		Snapshot->GetRevision(), TArray<FStructuralEdit>(),
		TArray<FString>({TEXT("properties.hjson")}), TEXT(""), TEXT(""),
		TArray<FString>(), EditedModel->GetSemanticFingerprint());
	FSemanticWorkspaceChangeProjection Projection;
	FPreparedAuthoringImportPtr PreparedImport;
	FSemanticWorkspaceImportError Error;
	TestTrue(
		TEXT("a non-root property remains available to the domain adapter"),
		TryPrepareSemanticWorkspaceChanges(
			Workspace, CurrentModel, Snapshot,
			FUeAgentDescriptorIdentity(
				TEXT("uai.hjson.semantic.import"), 1,
				TEXT("sha256:5555555555555555555555555555555555555555555555555555555555555555")),
			TEXT("domain-property-projection"), TEXT("domain-property-projection:1"),
			FUeAgentCallerContext(
				TEXT("domain-property-projection-test"),
				{TEXT("authoring.write")}, TEXT("domain-property-projection")),
			TEXT("/Game/HX013/BP_Structural.BP_Structural"),
			Resources, Projection, PreparedImport, Error));
	TestEqual(TEXT("the CDO property is not projected onto the Blueprint asset"),
		Projection.RootObjectProperties.Num(), 0);
	TestEqual(TEXT("one domain property unit is exposed"),
		Projection.PropertyChangesByUnitId.Num(), 1);
	TestEqual(TEXT("the common projection counts domain-owned properties"),
		Projection.GetPropertyChangeCount(), 1);
	const FString PropertyId = TEXT("property:class-default:SpriteScale");
	const TSharedPtr<FJsonValue>* Descriptor =
		Projection.PropertyChangesByUnitId.Find(PropertyId);
	TestTrue(TEXT("the property descriptor keeps its semantic unit identity"),
		Descriptor && Descriptor->IsValid() && (*Descriptor)->Type == EJson::Object);
	if (Descriptor && Descriptor->IsValid() && (*Descriptor)->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> Object = (*Descriptor)->AsObject();
		TestEqual(TEXT("the property path is preserved"),
			Object->GetStringField(TEXT("property_path")), FString(TEXT("SpriteScale")));
		const TSharedPtr<FJsonObject>* Value = nullptr;
		TestTrue(TEXT("the common property codec shape is preserved"),
			Object->TryGetObjectField(TEXT("value"), Value)
				&& Value && (*Value)->HasField(TEXT("literal")));
		if (Value && (*Value)->HasField(TEXT("literal")))
		{
			TestEqual(TEXT("the compact literal is restored before domain routing"),
				(*Value)->GetNumberField(TEXT("literal")), 1.25);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticWorkspaceImportRootPropertyProjectionTest,
	"UeAgentInterface.Composition.HjsonImport.SemanticWorkspace.RootPropertyProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticWorkspaceImportRootPropertyProjectionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString RootPath = TEXT("/Game/M_Test.M_Test");
	FUeAgentResourceTracker Resources = MakeImportTracker();
	const FPreparedAuthoringImportPtr Prepared = MakePreparedProjectionImport(Resources,
		{{TEXT("owner_path"), RootPath}, {TEXT("property_path"), TEXT("bUsedWithWater")},
			{TEXT("notification_owner_path"), RootPath}});
	TArray<TSharedPtr<FJsonValue>> Properties;
	FSemanticWorkspaceImportError Error;
	TestTrue(TEXT("root-object updates project without mutation"),
		TryProjectRootObjectPropertyChanges(*Prepared, RootPath, Properties, Error));
	TestEqual(TEXT("one property descriptor is projected"), Properties.Num(), 1);
	if (Properties.Num() == 1 && Properties[0].IsValid())
	{
		const TSharedPtr<FJsonObject> Property = Properties[0]->AsObject();
		TestTrue(TEXT("descriptor is an object"), Property.IsValid());
		if (Property.IsValid())
		{
			TestEqual(TEXT("property path is preserved"),
				Property->GetStringField(TEXT("property_path")), FString(TEXT("bUsedWithWater")));
			TestEqual(TEXT("canonical value is converted"),
				Property->TryGetField(TEXT("value"))->AsString(), FString(TEXT("after")));
			TestEqual(TEXT("exact type assertion is restored from the hidden definition"),
				Property->GetStringField(TEXT("kind")), FString(TEXT("string")));
		}
	}

	auto ExpectRejected = [this, &Resources, &RootPath](const FString& Label,
		TMap<FString, FString> Selectors, EChangeIntent Intent, TArray<FStructuralEdit> Edits,
		const FString& ExpectedCode)
	{
		const FPreparedAuthoringImportPtr Candidate = MakePreparedProjectionImport(
			Resources, MoveTemp(Selectors), Intent, MoveTemp(Edits));
		TArray<TSharedPtr<FJsonValue>> Projected({MakeShared<FJsonValueString>(TEXT("sentinel"))});
		FSemanticWorkspaceImportError ProjectionError;
		TestFalse(Label, TryProjectRootObjectPropertyChanges(
			*Candidate, RootPath, Projected, ProjectionError));
		TestEqual(Label + TEXT(" reports the stable error"), ProjectionError.Code, ExpectedCode);
		TestEqual(Label + TEXT(" leaves no partial projection"), Projected.Num(), 0);
	};
	ExpectRejected(TEXT("nested owner is rejected"),
		{{TEXT("owner_path"), RootPath + TEXT(":EditorOnlyData")},
			{TEXT("property_path"), TEXT("bUsedWithWater")}}, EChangeIntent::Update, {},
		TEXT("hjson_import_root_property_owner_mismatch"));
	ExpectRejected(TEXT("notification owner mismatch is rejected"),
		{{TEXT("owner_path"), RootPath}, {TEXT("property_path"), TEXT("bUsedWithWater")},
			{TEXT("notification_owner_path"), RootPath + TEXT(":EditorOnlyData")}},
		EChangeIntent::Update, {}, TEXT("hjson_import_root_property_notification_owner_mismatch"));
	ExpectRejected(TEXT("missing property path is rejected"),
		{{TEXT("owner_path"), RootPath}}, EChangeIntent::Update, {},
		TEXT("hjson_import_root_property_path_missing"));
	ExpectRejected(TEXT("non-update intent is rejected"),
		{{TEXT("owner_path"), RootPath}, {TEXT("property_path"), TEXT("bUsedWithWater")}},
		EChangeIntent::Create, {}, TEXT("hjson_import_root_property_intent_unsupported"));
	ExpectRejected(TEXT("structural edits are rejected before projection"),
		{{TEXT("owner_path"), RootPath}, {TEXT("property_path"), TEXT("bUsedWithWater")}},
		EChangeIntent::Update,
		{FStructuralEdit(TEXT("add:child"), EStructuralEditKind::Add, TEXT("target:material"),
			TEXT("child"))}, TEXT("hjson_import_root_property_structural_edits_unsupported"));
	return !HasAnyErrors();
}
