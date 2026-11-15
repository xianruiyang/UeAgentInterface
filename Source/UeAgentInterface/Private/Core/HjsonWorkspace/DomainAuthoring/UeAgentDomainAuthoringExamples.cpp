#include "Core/HjsonWorkspace/UeAgentAuthoringExamples.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Domains/Animation/Authoring/UeAgentAnimationSequenceStructure.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::HjsonWorkspace
{
	namespace
	{
		using V = TSharedPtr<FJsonValue>;
		V S(const TCHAR* Value) { return MakeShared<FJsonValueString>(Value); }
		V N(double Value) { return MakeShared<FJsonValueNumber>(Value); }
		V O(std::initializer_list<TPair<FString, V>> Fields)
		{
			const auto Result = MakeShared<FJsonObject>();
			for (const auto& Field : Fields) Result->SetField(Field.Key, Field.Value);
			return MakeShared<FJsonValueObject>(Result);
		}
		V A(std::initializer_list<V> Values) { return MakeShared<FJsonValueArray>(TArray<V>(Values)); }
		V Typed(const TCHAR* Type, V Value) { return O({{TEXT("type"), S(Type)}, {TEXT("value"), Value}}); }
	}

	TArray<FAuthoringExample> BuildDomainKeyedItemExamples(const FKeyedItemProjectionContract& Contract)
	{
		// Only logical record requirements belong here. Native property values recurse
		// through their existing type descriptors in the common generator.
		const FString& Kind = Contract.RootField;
		V Row;
		FString Key = TEXT("NewItem");
		TArray<FString> Notes;
		if (Kind == TEXT("entries")) Row = O({{TEXT("source"), S(TEXT("Example"))}});
		else if (Kind == TEXT("targets")) Row = O({{TEXT("native_culture"), S(TEXT("en"))}, {TEXT("cultures"), A({S(TEXT("en"))})}});
		else if (Kind == TEXT("variables")) Row = O({{TEXT("type"), O({{TEXT("pin_category"), S(TEXT("float"))}})}});
		else if (Kind == TEXT("delegates")) Row = O({});
		else if (Kind == TEXT("interfaces")) Row = O({{TEXT("class_path"), S(TEXT("<interface class path>"))}});
		else if (Kind == TEXT("graph_declarations")) Row = O({{TEXT("graph_kind"), S(TEXT("function_graph"))}});
		else if (Kind == TEXT("emitters")) Row = O({{TEXT("source_asset_path"), S(TEXT("<emitter asset path>"))}});
		else if (Kind == TEXT("modules") && Contract.DirectCreationFields.Contains(TEXT("assignment_targets")))
		{
			return {
				{TEXT("script_module"), O({{TEXT("NewModule"), O({{TEXT("name"), S(TEXT("<supported module candidate name>"))}})}}),
					{TEXT("Use a supported script-module candidate from this stack's context. Merge the new row; preserve existing entries.")}},
				{TEXT("assignment"), O({{TEXT("NewAssignment"), O({{TEXT("module_kind"), S(TEXT("assignment"))},
					{TEXT("assignment_targets"), O({{TEXT("Particles.Lifetime"), MakeShared<FJsonValueNull>()}})}})}}),
					{TEXT("Assignment uses native parameter declarations directly, without a candidate name. Null infers a known native attribute's type; custom variables require an explicit type. Merge this row; preserve existing entries.")}}
			};
		}
		else if (Kind == TEXT("user_parameters"))
		{
			Key = TEXT("User.Example");
			Row = O({{TEXT("type"), S(TEXT("float"))}, {TEXT("value"), O({{TEXT("literal"), N(0)}})}});
		}
		else if (Kind == TEXT("animations")) Row = O({{TEXT("display_label"), S(TEXT("Example"))}});
		else if (Kind == TEXT("folders")) Row = O({{TEXT("name"), S(TEXT("Example"))}});
		else if (Kind == TEXT("bindings") && Contract.CollectionFieldContracts.Contains(TEXT("tracks")))
		{
			Row = O({{TEXT("binding_kind"), S(Contract.VisibleFieldPolicy.Allows(TEXT("tags")) ? TEXT("possessable") : TEXT("widget"))},
				{TEXT("object_ref"), S(TEXT("<object reference in this binding context>"))}, {TEXT("tracks"), O({})}});
		}
		else if (Kind == TEXT("bindings")) Row = O({{TEXT("widget_name"), S(TEXT("<widget name>"))},
			{TEXT("property_name"), S(TEXT("<bindable property>"))}, {TEXT("binding_kind"), S(TEXT("function"))},
			{TEXT("function_name"), S(TEXT("<function name>"))}});
		else if (Kind == TEXT("root_tracks") || Kind == TEXT("tracks"))
		{
			Row = O({{TEXT("name"), S(TEXT("<track candidate name>"))}});
			Notes.Add(TEXT("Use the track candidates for this root/binding. Supply property_path when the selected native property track requires it. After import, read the refreshed workspace for its sections and native channels."));
		}
		else if (Kind == TEXT("sections"))
		{
			Row = O({});
			Notes.Add(TEXT("A new section receives the owning track's native section/channel/range defaults. Edit those fields if needed; do not invent a different channel layout."));
		}
		else if (Kind == TEXT("keys"))
		{
			const auto Time = O({{TEXT("tick"), S(TEXT("0"))}, {TEXT("rate"), O({
				{TEXT("numerator"), S(TEXT("<current tick resolution numerator>"))},
				{TEXT("denominator"), S(TEXT("<current tick resolution denominator>"))}})}});
			const auto NewKey = [&Time](const TCHAR* Field, const V& Value) {
				return O({{TEXT("NewItem"), O({{TEXT("time"), Time}, {Field, Value}})}});
			};
			return {
				{TEXT("scalar_key"), NewKey(TEXT("value_lexeme"), S(TEXT("<channel value as text>"))),
					{TEXT("For numeric, string, boolean and enum channels, supply the channel's exact text lexeme. Use the current tick resolution. Preserve existing keys; native defaults supply omitted interpolation and tangent fields.")}},
				{TEXT("reference_key"), NewKey(TEXT("value"), O({{TEXT("path"), S(TEXT("<compatible object or class path>"))}})),
					{TEXT("For ObjectPath channels, supply a reference compatible with the native property or a null path. Use the current tick resolution and preserve existing keys.")}}
			};
		}
		else if (Kind == TEXT("rig_elements"))
		{
			Key = TEXT("null:Example"); Row = O({{TEXT("kind"), S(TEXT("null"))}, {TEXT("name"), S(TEXT("Example"))}});
		}
		else if (Kind == TEXT("rig_graphs"))
		{
			return {{TEXT("existing_graph"), O({}), {TEXT("Use an existing native graph path. This map edits graph nodes/links; it does not create native RigVM graphs. Query nodes in that graph's context.")}}};
		}
		else if (Kind == TEXT("ik_chains")) Row = O({{TEXT("start_bone"), S(TEXT("<start bone>"))}, {TEXT("end_bone"), S(TEXT("<end bone>"))}});
		else if (Kind == TEXT("ik_goals")) Row = O({{TEXT("bone_name"), S(TEXT("<bone name>"))}, {TEXT("transform_space"), S(TEXT("character_global"))}});
		else if (Kind == TEXT("ik_solvers") || Kind == TEXT("ik_retarget_ops"))
			Row = O({{TEXT("struct_path"), S(TEXT("<native solver/op struct path>"))}, {TEXT("value"), O({{TEXT("fields"), O({})}})}});
		else if (Kind == TEXT("ik_retarget_mappings"))
		{
			Key = TEXT("<op>:<target chain>");
			Row = O({{TEXT("op"), S(TEXT("<op>"))}, {TEXT("target_chain"), S(TEXT("<target chain>"))}, {TEXT("source_chain"), S(TEXT("<source chain>"))}});
		}
		else if (Kind == TEXT("ik_retarget_poses"))
		{
			Key = TEXT("<existing source or target side>");
			Row = O({{TEXT("root_translation_space"), S(TEXT("retarget_root_global_translation_delta"))},
				{TEXT("bone_rotation_space"), S(TEXT("bone_local_rotation_delta"))},
				{TEXT("poses"), O({{TEXT("Example"), O({})}})}});
			Notes.Add(TEXT("Merge Example into the existing side's poses map, preserving its other poses and current_pose. Empty pose members use native zero translation/identity rotation."));
		}
		if (!Row) return {};
		Notes.Add(TEXT("This is one new entry to merge into the collection. Preserve other entries. Replace contextual placeholders using current candidates/references; do not copy captured identities."));
		return {{TEXT("new_item"), O({{Key, Row}}), Notes}};
	}

	void RegisterDomainAuthoringExampleRules(FAuthoringExampleCatalog& Catalog)
	{
		// Domain recipes describe editable logical records and the native writable curve subset.
		// Both Montage and Sequence consume the same event owner.
		Catalog.AddRule([](const FString& Semantic, const FString&, const FString& Field) -> TArray<FAuthoringExample>
		{
			if (Semantic != TEXT("uai.semantic.animation.anim_sequence.document")
				&& Semantic != TEXT("uai.semantic.animation.anim_montage.document")) return {};
			if (Semantic == TEXT("uai.semantic.animation.anim_sequence.document")
				&& (Field.StartsWith(TEXT("float_curves/")) || Field.StartsWith(TEXT("transform_curves/"))))
			{
				const bool bTransform = Field.StartsWith(TEXT("transform_curves/"));
				const bool bCollection = Field.EndsWith(TEXT("/index"));
				V Curve;
				if (bTransform)
				{
					const auto Components = MakeShared<FJsonObject>();
					for (const FString Component : {FString(TEXT("position")), FString(TEXT("rotation")), FString(TEXT("scale"))})
					{
						const auto Axes = MakeShared<FJsonObject>();
						for (const FString Axis : {FString(TEXT("x")), FString(TEXT("y")), FString(TEXT("z"))})
							Axes->SetObjectField(Axis, UeAgentAnimationAuthoring::BuildSequenceCurveAuthoringExample(true,
								Component == TEXT("scale") ? 1.f : 0.f));
						Components->SetObjectField(Component, Axes);
					}
					Curve = MakeShared<FJsonValueObject>(Components);
				}
				else Curve = MakeShared<FJsonValueObject>(UeAgentAnimationAuthoring::BuildSequenceCurveAuthoringExample(false, 0.f));
				return {{bCollection ? TEXT("/value:new_curve") : TEXT("/value:curve"),
					bCollection ? O({{TEXT("NewCurve"), Curve}}) : Curve, {
					bCollection ? TEXT("Merge this named curve into value, preserving existing curves. Replace NewCurve with the intended curve name.")
						: TEXT("This is the editable curve value. Preserve keys and attributes that you do not intend to change."),
					TEXT("Keys use seconds. Each scalar curve contains channels.value; do not flatten that channel wrapper."),
					bTransform ? TEXT("Transform curves contain position/rotation/scale and x/y/z scalar channels. The animation controller supports their keys but not default_value or extrapolation fields.")
						: TEXT("Animation float curves support keys and extrapolation, but not default_value. Other curve assets have a different writable subset.")}}};
			}
			if (Field == TEXT("notifies"))
			{
				TArray<FAuthoringExample> Examples;
				const TArray<FString> CommonNotes{
					TEXT("Append a new row to value; preserve existing rows and their instance keys. New rows omit instance/definition. Time and duration are seconds; track_index selects an existing notify track."),
					TEXT("settings is the native category map with typed property rows. Omitted properties use native defaults; linkage is handled by the animation owner.")};
				for (const FString Kind : {FString(TEXT("named")), FString(TEXT("notify")), FString(TEXT("state"))})
				{
					const auto Category = MakeShared<FJsonObject>();
					Category->SetField(TEXT("NotifyName"), Typed(TEXT("name"), S(TEXT("Event"))));
					TArray<FString> Notes = CommonNotes;
					if (Kind != TEXT("named"))
					{
						const FString Member = Kind == TEXT("state") ? TEXT("NotifyStateClass") : TEXT("Notify");
						Category->SetField(Member, Typed(TEXT("object_ref"), O({{TEXT("owned_object"), O({{TEXT("name"), S(TEXT("<candidate name>"))}})}})));
						Notes.Add(TEXT("Use asset.owned_candidates with property_path Notifies[<new index>].") + Member
							+ TEXT("; copy its returned name/select. The prospective index need not exist. After import, read refreshed native payload properties before editing them."));
					}
					Examples.Add({TEXT("/value:") + Kind, A({O({{TEXT("time"), N(0)}, {TEXT("duration"), N(Kind == TEXT("state") ? 0.2 : 0)},
						{TEXT("track_index"), N(0)}, {TEXT("settings"), O({{TEXT("AnimNotifyEvent"), MakeShared<FJsonValueObject>(Category)}})}})}), Notes});
				}
				return Examples;
			}
			if (Field == TEXT("notify_tracks")) return {{TEXT("/value:new_track"),
				A({O({{TEXT("settings"), O({{TEXT("fields"), O({{TEXT("TrackName"), S(TEXT("Events"))}})}})}})}),
				{TEXT("Append to value. Retain existing instance keys; omit instance for the new track.")}}};
			if (Field == TEXT("sync_markers")) return {{TEXT("/value:new_marker"),
				A({O({{TEXT("settings"), O({{TEXT("fields"), O({{TEXT("MarkerName"), S(TEXT("Marker"))}, {TEXT("Time"), N(0)}, {TEXT("TrackIndex"), N(0)}})}})}})}),
				{TEXT("Append to value; Time is seconds and TrackIndex selects a notify track. New markers omit native Guid and instance.")}}};
			return {};
		});
	}
}
