#include "Core/HjsonWorkspace/UeAgentAuthoringContentLayout.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Utilities/UeAgentReadableName.h"

#include "Misc/Paths.h"
#include <initializer_list>

namespace UE::UAI::HjsonWorkspace
{
	using namespace StateSemantic;
	FString MakeAuthoringContentPathSegment(const FString& Name)
	{
		FString Result = FPaths::MakeValidFileName(Name);
		Result.ReplaceInline(TEXT("/"), TEXT("_"));
		Result.ReplaceInline(TEXT("\\"), TEXT("_"));
		Result.TrimStartAndEndInline();
		while (Result.EndsWith(TEXT("."))) Result.LeftChopInline(1);
		if (Result.IsEmpty()) return TEXT("unnamed");
		FString Device = Result;
		int32 Dot;
		if (Device.FindChar(TEXT('.'), Dot)) Device.LeftInline(Dot);
		Device = Device.ToUpper();
		const bool bPortName = Device.Len() == 4
			&& (Device.StartsWith(TEXT("COM")) || Device.StartsWith(TEXT("LPT")))
			&& Device[3] >= TEXT('1') && Device[3] <= TEXT('9');
		if (Device == TEXT("CON") || Device == TEXT("PRN") || Device == TEXT("AUX") || Device == TEXT("NUL") || bPortName)
			Result = TEXT("_") + Result;
		return Result;
	}

	namespace
	{
		enum class EContentType : uint8
		{
			Settings, Samples, Curves, Events, Hierarchy, Records, Timeline, Binding, Track, Graph,
			SampleMap, CurveMap, RecordMap, HierarchyMap, GraphMap, TimelineMap, BindingMap, TrackMap
		};

		struct FContentField
		{
			const TCHAR* Name;
			EContentType Type;
		};

		struct FContentContext
		{
			FPublishAuthoringContent Publish;
			ContextResource::FUeAgentResourceTracker& Resources;
			FWorkspaceError& Error;
			bool Build(const TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Fields, FUeAgentCanonicalValuePtr& Value)
			{
				FUeAgentStateSemanticError StateError;
				if (FUeAgentCanonicalValue::TryObject(Fields, Resources, Value, StateError)) return true;
				Error.SetFailure(StateError); return false;
			}
		};

		bool OrganizeValue(FUeAgentCanonicalValuePtr& Value, EContentType Type,
			const FString& Name, FContentContext& Context);

		bool OrganizeFields(FUeAgentCanonicalValuePtr& Object,
			std::initializer_list<FContentField> Fields, const FString& Name,
			FContentContext& Context)
		{
			if (!Object || Object->GetKind() != EUeAgentValueKind::Object) return true;
			auto Members = Object->GetObjectItems();
			for (auto& Member : Members)
			{
				for (const auto& Field : Fields)
					if (Member.Key.Equals(Field.Name, ESearchCase::CaseSensitive))
					{
						FUeAgentCanonicalValuePtr Value = Member.Value;
						if (!OrganizeValue(Value, Field.Type,
							Name.IsEmpty() ? FString(Field.Name) : Name + TEXT("/") + Field.Name, Context)) return false;
						Member.Value = Value.ToSharedRef();
						break;
					}
			}
			return Context.Build(Members, Object);
		}

		bool OrganizeValue(FUeAgentCanonicalValuePtr& Value, EContentType Type,
			const FString& Name, FContentContext& Context)
		{
			if (!Value.IsValid()) return true;
			const bool bMap = Type == EContentType::SampleMap || Type == EContentType::CurveMap
				|| Type == EContentType::RecordMap || Type == EContentType::TimelineMap
				|| Type == EContentType::HierarchyMap || Type == EContentType::GraphMap
				|| Type == EContentType::BindingMap || Type == EContentType::TrackMap;
			if (bMap && Value->GetKind() == EUeAgentValueKind::Object)
			{
				// The map remains editable, including when empty. Members retain native identities.
				auto Members = Value->GetObjectItems();
				TSet<FString> UsedNames{TEXT("index")};
				TSet<FString> ReservedNames;
				for (const auto& Member : Members) ReservedNames.Add(MakeAuthoringContentPathSegment(Member.Key).ToLower());
				for (auto& Item : Members)
				{
					FUeAgentCanonicalValuePtr Member = Item.Value;
					const FString Base = MakeAuthoringContentPathSegment(Item.Key);
					const FString Segment = AllocateReadableName(Base, [&](const FString& Candidate) {
						return UsedNames.Contains(Candidate.ToLower())
							|| (Candidate != Base && ReservedNames.Contains(Candidate.ToLower()));
					});
					UsedNames.Add(Segment.ToLower());
					const EContentType MemberType = Type == EContentType::SampleMap ? EContentType::Samples
						: Type == EContentType::CurveMap ? EContentType::Curves
						: Type == EContentType::HierarchyMap ? EContentType::Hierarchy
						: Type == EContentType::GraphMap ? EContentType::Graph
						: Type == EContentType::TimelineMap ? EContentType::Timeline
						: Type == EContentType::BindingMap ? EContentType::Binding
						: Type == EContentType::TrackMap ? EContentType::Track
						: EContentType::Records;
					if (!OrganizeValue(Member, MemberType, Name + TEXT("/") + Segment, Context)) return false;
					Item.Value = Member.ToSharedRef();
				}
				if (!Context.Build(Members, Value)) return false;
			}
			else if (Type == EContentType::Binding && Value->GetKind() == EUeAgentValueKind::Object)
			{
				if (!OrganizeFields(Value,
					{{TEXT("tracks"), EContentType::TrackMap}}, Name, Context)) return false;
			}
			else if (Type == EContentType::Timeline && Value->GetKind() == EUeAgentValueKind::Object)
			{
				if (!OrganizeFields(Value, {
					{TEXT("bindings"), EContentType::BindingMap},
					{TEXT("root_tracks"), EContentType::TrackMap}}, Name, Context)) return false;
			}
			// A curve's keys/tangents and a track's sections/channels stay together.
			// Sample components and aligned pose matrices are never recursively split.
			FUeAgentCanonicalValuePtr Reference;
			const bool bContainer = bMap || ((Type == EContentType::Timeline || Type == EContentType::Binding)
				&& Value->GetKind() == EUeAgentValueKind::Object);
			if (!Context.Publish(Value, bContainer ? Name + TEXT("/index") : Name, Reference)) return false;
			Value = MoveTemp(Reference);
			return true;
		}
	}

	bool TryOrganizeAuthoringContent(const FString& SemanticType, const FString& Role,
		FUeAgentCanonicalValuePtr& VisibleRow, FPublishAuthoringContent Publish,
		ContextResource::FUeAgentResourceTracker& Resources, FWorkspaceError& OutError)
	{
		FContentContext Context{Publish, Resources, OutError};
		if (!VisibleRow || VisibleRow->GetKind() != EUeAgentValueKind::Object) return true;
		const bool bCurveTable = VisibleRow->GetObjectItems().ContainsByPredicate([](const auto& Field) {
			return Field.Key.Equals(TEXT("table_curve_mode"), ESearchCase::CaseSensitive);
		});
		// These are composition declarations, not alternate domain parsers or writers.
		// Every declaration below is executed by the same type-driven organizer.
		// These root names belong to registered keyed projection contracts. Their
		// identities, ordering and references survive physical assembly unchanged.
		if (!OrganizeFields(VisibleRow, {
			{TEXT("rows"), bCurveTable ? EContentType::CurveMap : EContentType::RecordMap},
			{TEXT("modules"), EContentType::RecordMap},
			{TEXT("emitters"), EContentType::RecordMap},
			{TEXT("user_parameters"), EContentType::RecordMap},
			{TEXT("rig_elements"), EContentType::HierarchyMap},
			{TEXT("rig_graphs"), EContentType::GraphMap},
			{TEXT("ik_goals"), EContentType::RecordMap},
			{TEXT("ik_solvers"), EContentType::RecordMap},
			{TEXT("ik_chains"), EContentType::HierarchyMap},
			{TEXT("ik_retarget_ops"), EContentType::RecordMap},
			{TEXT("ik_retarget_mappings"), EContentType::RecordMap},
			{TEXT("ik_retarget_poses"), EContentType::SampleMap},
			{TEXT("tree_items"), EContentType::HierarchyMap},
			{TEXT("blackboard_keys"), EContentType::RecordMap},
			{TEXT("smart_object_items"), EContentType::HierarchyMap}}, FString(), Context)) return false;
		if (Role == TEXT("animation_collection"))
			return OrganizeFields(VisibleRow, {{TEXT("animations"), EContentType::TimelineMap}}, FString(), Context);
		if (Role == TEXT("timeline_collection"))
			return OrganizeFields(VisibleRow, {
				{TEXT("bindings"), EContentType::BindingMap},
				{TEXT("root_tracks"), EContentType::TrackMap},
				{TEXT("tracks"), EContentType::TrackMap}}, FString(), Context);

		auto Members = VisibleRow->GetObjectItems();
		auto* Value = Members.FindByPredicate([](const auto& Field) {
			return Field.Key.Equals(TEXT("value"), ESearchCase::CaseSensitive);
		});
		if (!Value || Value->Value->GetKind() != EUeAgentValueKind::Object) return true;
		FUeAgentCanonicalValuePtr Document = Value->Value;
		auto OrganizeDocument = [&]() -> bool {
		if (SemanticType == TEXT("uai.semantic.animation.anim_sequence.document"))
			return OrganizeFields(Document, {
				{TEXT("bone_tracks"), EContentType::SampleMap},
				{TEXT("float_curves"), EContentType::CurveMap},
				{TEXT("transform_curves"), EContentType::CurveMap},
				{TEXT("notify_tracks"), EContentType::Events},
				{TEXT("notifies"), EContentType::Events},
				{TEXT("sync_markers"), EContentType::Events}}, FString(), Context);
		if (SemanticType == TEXT("uai.semantic.animation.anim_montage.document"))
			return OrganizeFields(Document, {
				{TEXT("slots"), EContentType::Timeline}, {TEXT("sections"), EContentType::Timeline},
				{TEXT("notify_tracks"), EContentType::Events}, {TEXT("notifies"), EContentType::Events}}, FString(), Context);
		if (SemanticType == TEXT("uai.semantic.animation.pose_asset.document"))
			return OrganizeFields(Document, {{TEXT("pose_data"), EContentType::Samples}}, FString(), Context);
		if (SemanticType == TEXT("uai.semantic.animation.skeleton.document"))
			return OrganizeFields(Document, {
				{TEXT("sockets"), EContentType::Hierarchy}, {TEXT("virtual_bones"), EContentType::Hierarchy},
				{TEXT("slot_groups"), EContentType::Hierarchy}, {TEXT("retargeting"), EContentType::Hierarchy},
				{TEXT("bones"), EContentType::Hierarchy}}, FString(), Context);
		if (SemanticType == TEXT("uai.semantic.reflection.physics_asset.profile_state"))
			return OrganizeFields(Document, {
				{TEXT("bodies"), EContentType::RecordMap}, {TEXT("constraints"), EContentType::RecordMap}}, FString(), Context);
		if (SemanticType == TEXT("uai.semantic.reflection.static_mesh.profile_state"))
			return OrganizeFields(Document, {
				{TEXT("collision"), EContentType::Settings}, {TEXT("build_settings"), EContentType::Settings},
				{TEXT("sockets"), EContentType::Hierarchy}}, FString(), Context);
		if (SemanticType == TEXT("uai.semantic.reflection.pcg_data_asset.profile_state"))
			return OrganizeFields(Document, {{TEXT("sets"), EContentType::Samples}}, FString(), Context);
		if (SemanticType == TEXT("uai.semantic.reflection.material_parameter_collection.profile_state"))
			return OrganizeFields(Document, {
				{TEXT("scalars"), EContentType::Records}, {TEXT("vectors"), EContentType::Records}}, FString(), Context);
		if (Role == TEXT("curve"))
			return OrganizeValue(Document, EContentType::Curves, TEXT("curve"), Context);
		return true;
		};
		if (!OrganizeDocument()) return false;
		Value->Value = Document.ToSharedRef();
		return Context.Build(Members, VisibleRow);
	}
}
