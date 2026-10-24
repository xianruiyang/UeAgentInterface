#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaImagePolicy.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"

#include <initializer_list>

#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentAudioAssetEditorCapture.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentAssetEditorImageCapture.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentBlueprintEditorCapture.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentLevelViewportSequenceCapture.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentPcgViewportCapture.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentSequenceCapture.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentTextureCapture.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentUmgWidgetCapture.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentViewportCapture.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentWorldViewportCapture.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"
#include "Core/AssetPreview/UeAgentAssetPreviewState.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	using namespace CaptureDiagnostics;
	using FArtifactMechanics = bool (*)(
		const FUeAgentRequestContext&,
		TSharedPtr<FJsonObject>&,
		TArray<TSharedPtr<FJsonValue>>&,
		UE::UAI::Contract::FDiagnostic&);
	using FDirectMechanics = bool (*)(
		const TSharedPtr<FJsonObject>&,
		const FString&,
		TSharedPtr<FJsonObject>&,
		UE::UAI::Contract::FDiagnostic&);
	using FContextMechanics = bool (*)(
		const FUeAgentRequestContext&,
		TSharedPtr<FJsonObject>&,
		UE::UAI::Contract::FDiagnostic&);

	namespace
	{
		void LocateProjectedDiagnostic(UE::UAI::Contract::FDiagnostic& Diagnostic,
			const TMap<FString, UE::UAI::Contract::FSourceLocation>& Sources)
		{
			if (Diagnostic.IsEmpty()) return;
			if (Diagnostic.SourceKind == TEXT("command_input") && Diagnostic.Source.SourceFile.IsEmpty())
			{
				const FString Pointer = !Diagnostic.Source.JsonPath.IsEmpty() ? Diagnostic.Source.JsonPath : Diagnostic.SemanticPointer;
				const auto Source = UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Sources, Pointer);
				if (Source.bCommandInput || !Source.SourceFile.IsEmpty())
				{
					Diagnostic.Source = Source;
					Diagnostic.SemanticPointer = Source.JsonPath;
					if (Diagnostic.PropertyPath == Pointer) Diagnostic.PropertyPath = Source.JsonPath;
					Diagnostic.SourceKind = Source.SourceFile.IsEmpty() ? TEXT("command_input") : TEXT("authored_value");
				}
				else
				{
					Diagnostic.Source = {};
					Diagnostic.SemanticPointer.Reset();
					Diagnostic.SourceKind = TEXT("native_result");
				}
			}
			for (auto& Child : Diagnostic.Diagnostics) LocateProjectedDiagnostic(Child, Sources);
			if (Diagnostic.PublicCause.IsValid())
			{
				auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Diagnostic.PublicCause);
				LocateProjectedDiagnostic(*Cause, Sources);
				Diagnostic.PublicCause = Cause;
			}
		}

		void MarkDerivedParameter(TMap<FString, UE::UAI::Contract::FSourceLocation>& Sources, const TCHAR* Field)
		{
			const FString Pointer = FString(TEXT("/")) + Field;
			for (auto It = Sources.CreateIterator(); It; ++It)
				if (It.Key() == Pointer || It.Key().StartsWith(Pointer + TEXT("/"))) It.RemoveCurrent();
			Sources.Add(Pointer, UE::UAI::Contract::FSourceLocation{});
		}

		struct FPolicyRoute final
		{
			FUeAgentCaptureMediaImagePolicyDescriptor Descriptor;
			FString InternalCommandId;
			FArtifactMechanics ArtifactMechanics = nullptr;
			FDirectMechanics DirectMechanics = nullptr;
			FString AssetNotFoundError;
			FContextMechanics ContextMechanics = nullptr;
		};

		FString TransformCandidatesParameterSchema()
		{
			return TEXT(
				"{\"$defs\":{\"rotator\":{\"additionalProperties\":false,"
				"\"properties\":{\"pitch\":{\"type\":\"number\"},"
				"\"roll\":{\"type\":\"number\"},\"yaw\":{\"type\":\"number\"}},"
				"\"required\":[\"pitch\",\"yaw\",\"roll\"],\"type\":\"object\"},"
				"\"vector3\":{\"additionalProperties\":false,\"properties\":{"
				"\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},"
				"\"z\":{\"type\":\"number\"}},\"required\":[\"x\",\"y\",\"z\"],"
				"\"type\":\"object\"}},\"additionalProperties\":false,\"properties\":{"
				"\"candidates\":{\"items\":{\"additionalProperties\":false,"
				"\"properties\":{\"label\":{\"minLength\":1,\"type\":\"string\"},"
				"\"relative_to_original\":{\"type\":\"boolean\"},\"transform\":{"
				"\"additionalProperties\":false,\"minProperties\":1,\"properties\":{"
				"\"location\":{\"$ref\":\"#/$defs/vector3\"},\"rotation\":{"
				"\"$ref\":\"#/$defs/rotator\"},\"scale\":{\"$ref\":\"#/$defs/vector3\"}},"
				"\"type\":\"object\"}},\"required\":[\"transform\"],\"type\":\"object\"},"
				"\"minItems\":1,\"type\":\"array\"},"
				"\"format\":{\"enum\":[\"png\",\"jpg\"],\"type\":\"string\"},"
				"\"frame_sheet_padding\":{\"minimum\":0,\"type\":\"integer\"},"
				"\"max_size\":{\"minimum\":1,\"type\":\"integer\"},"
				"\"output_file\":{\"minLength\":1,\"type\":\"string\"},"
				"\"quality\":{\"maximum\":100,\"minimum\":1,\"type\":\"integer\"},"
				"\"warmup_frames\":{\"minimum\":0,\"type\":\"integer\"}},"
				"\"required\":[\"candidates\"],\"type\":\"object\"}");
		}

		FString ActorSubjectSchema()
		{
			return TEXT(
				"{\"minLength\":23,\"pattern\":\"^capture-subject:actor:.+\","
				"\"type\":\"string\"}");
		}

		FString ActiveViewportSubjectSchema()
		{
			return TEXT(
				"{\"const\":\"capture-subject:viewport:active\","
				"\"type\":\"string\"}");
		}

		FString ViewportParameterSchema(const FString& AdditionalProperties = FString())
		{
			return TEXT(R"schema({"type":"object","additionalProperties":false,"properties":{
"format":{"type":"string","enum":["png","jpg","jpeg"],"default":"png"},
"max_size":{"type":"integer","minimum":1},"output_file":{"type":"string"},
"quality":{"type":"integer","minimum":1,"maximum":100,"default":85},
"warmup_frames":{"type":"integer","minimum":0,"maximum":2147483647,"default":2},
"render_warmup_frames":{"type":"integer","minimum":0,"maximum":2147483647},
"restore_view_mode_after_capture":{"type":"boolean","default":true},"restore_view_mode":{"type":"boolean"},
"view_mode":{"type":"string"},"editor_view_mode":{"type":"string"},"mode":{"type":"string"},
"mode_key":{"type":"string"},"debug_view_mode":{"type":"string"},
"visualization_family":{"type":"string"},"visualization_mode":{"type":"string"},
"visualizer":{"type":"string"},"view_mode_family":{"type":"string"},
"visualizer_mode":{"type":"string"},"debug_mode":{"type":"string"})schema")
				+ (AdditionalProperties.IsEmpty() ? FString() : TEXT(",") + AdditionalProperties)
				+ TEXT("}}");
		}

		FString PcgViewportParameterSchema()
		{
			return ViewportParameterSchema(TEXT(R"schema(
"actor":{"type":"string","description":"Optional framing selector; normally derived from the actor subject. Prefer the exact subject_ref."},
"actor_name":{"type":"string","description":"Framing alias used when actor is empty or absent."},
"actor_label":{"type":"string","description":"Framing alias used when actor and actor_name are empty or absent."},
"id":{"type":"string","description":"Optional framing identity; it must identify the same actor as the subject and other framing selectors."},
"padding":{"type":"number","default":1.1,"description":"Multiplier applied to the actor bounds for framing."},
"look_at":{"type":"boolean","default":false},
"collision_aware":{"type":"boolean","default":false},
"trace_channel":{"type":"string","default":"visibility","description":"Case-insensitive visibility, camera, worldstatic/world_static, worlddynamic/world_dynamic, pawn or physicsbody/physics_body; the corresponding ecc_ names are accepted."},
"safety_offset_cm":{"type":"number","default":15,"description":"Collision clearance in centimeters; negative values are clamped to zero."},
"trace_complex":{"type":"boolean","default":false},
"auto_fallback":{"type":"boolean","default":false},
"fallback_step_cm":{"type":"number","default":200,"description":"Automatic alternate-view step in centimeters; negative values are clamped to zero."},
"fallback_offsets_cm":{"type":"array","description":"Explicit world-space alternate-view offsets in centimeters. Each vector has three finite components; zero offsets are ignored.","items":{"anyOf":[
{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}},
{"type":"object","required":["components"],"properties":{"components":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}}}}
]}})schema"));
		}

		FString ViewportSequenceParameterSchema()
		{
			return TEXT(
				"{\"additionalProperties\":false,\"properties\":{"
				"\"format\":{\"enum\":[\"png\",\"jpg\"],\"type\":\"string\"},"
				"\"frame_count\":{\"minimum\":1,\"type\":\"integer\"},"
				"\"frame_sheet_padding\":{\"minimum\":0,\"type\":\"integer\"},"
				"\"interval_frames\":{\"minimum\":1,\"type\":\"integer\"},"
				"\"max_size\":{\"minimum\":1,\"type\":\"integer\"},"
				"\"output_file\":{\"minLength\":1,\"type\":\"string\"},"
				"\"quality\":{\"maximum\":100,\"minimum\":1,\"type\":\"integer\"},"
				"\"view_mode\":{\"minLength\":1,\"type\":\"string\"},"
				"\"warmup_frames\":{\"minimum\":0,\"type\":\"integer\"}},"
				"\"type\":\"object\"}");
		}

		FString UmgParameterSchema()
		{
			const FString Region = TEXT(R"schema({"type":"object","additionalProperties":false,"properties":{
"x":{"type":"integer","default":0},"y":{"type":"integer","default":0},
"width":{"type":"integer","minimum":1},"height":{"type":"integer","minimum":1},
"crop_x":{"type":"integer"},"crop_y":{"type":"integer"},
"crop_width":{"type":"integer","minimum":1},"crop_height":{"type":"integer","minimum":1},
"left":{"type":"integer"},"top":{"type":"integer"},
"w":{"type":"integer","minimum":1},"h":{"type":"integer","minimum":1}},
"description":"Pixel rectangle in the captured canvas: x/y default to zero; supply width and height. crop_* and left/top/w/h are accepted aliases."})schema");
			return TEXT(R"schema({"type":"object","additionalProperties":false,"properties":{
"capture_width":{"type":"integer","minimum":1,"default":1920,"description":"Offscreen canvas width in pixels, before cropping or output resizing."},
"capture_height":{"type":"integer","minimum":1,"default":1080,"description":"Offscreen canvas height in pixels, before cropping or output resizing."},
"use_project_dpi_scale":{"type":"boolean","default":false},
"crop_mode":{"type":"string","default":"auto_bounds","enum":["auto_bounds","manual_region","full_canvas","auto","manual","region","full"],"description":"full_canvas preserves the requested canvas. auto_bounds crops transparent margins; a supplied region selects manual_region unless another crop mode is explicit."},
"region":)schema") + Region + TEXT(R"schema(,"crop":)schema") + Region + TEXT(R"schema(,
"bounds_padding":{"type":"integer","minimum":0,"default":0},
"bounds_alpha_threshold":{"type":"integer","minimum":0,"maximum":255,"default":0},
"padding":{"type":"integer","minimum":0},"alpha_threshold":{"type":"integer","minimum":0,"maximum":255},
"crop_x":{"type":"integer"},"crop_y":{"type":"integer"},
"crop_width":{"type":"integer","minimum":1},"crop_height":{"type":"integer","minimum":1},
"x":{"type":"integer"},"y":{"type":"integer"},"width":{"type":"integer","minimum":1},"height":{"type":"integer","minimum":1},
"left":{"type":"integer"},"top":{"type":"integer"},"w":{"type":"integer","minimum":1},"h":{"type":"integer","minimum":1},
"format":{"type":"string","enum":["png","jpg","jpeg"],"default":"png"},
"quality":{"type":"integer","minimum":1,"maximum":100,"default":90},
"max_size":{"type":"integer","minimum":1,"description":"Optional post-crop output resizing; omit to preserve pixels."},
"output_file":{"type":"string"},"asset_path":{"type":"string","description":"Optional subject alias; must identify the same WidgetBlueprint as subject_ref."}
}})schema");
		}

		FString ImageParameterSchema(const FString& Properties, const int32 DefaultQuality)
		{
			// These are public projection defaults, which precede the native handler defaults.
			FString Common = TEXT(R"schema({"type":"object","additionalProperties":false,"properties":{
"format":{"type":"string","enum":["png","jpg","jpeg"],"default":"png"},
"output_file":{"type":"string"},"max_size":{"type":"integer","minimum":1})schema");
			if (DefaultQuality >= 0)
				Common += FString::Printf(TEXT(",\"quality\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100,\"default\":%d}"), DefaultQuality);
			return Common + (Properties.IsEmpty() ? FString() : TEXT(",") + Properties) + TEXT("}}");
		}

		FString AssetEditorParameterSchema()
		{
			return ImageParameterSchema(TEXT(R"schema(
"asset_path":{"type":"string","description":"Optional asset path; normally derived from subject_ref."},
"open_editor_if_needed":{"type":"boolean","default":true},
"require_non_black":{"type":"boolean","default":true})schema"), 90);
		}

		FString BlueprintParameterSchema(const bool bControlRig = false)
		{
			FString Properties = TEXT(R"schema(
"asset_path":{"type":"string","description":"Optional asset path; normally derived from subject_ref."},
"target":{"type":"string","default":"viewport","description":"viewport, components, eventgraph/event_graph, graph, graph:<name>, or window."},
"allow_ui_mutation":{"type":"boolean","default":false,"description":"Also permits window mutation."},
"allow_window_mutation":{"type":"boolean","default":false},
"open_editor_if_needed":{"type":"boolean","default":false},
"graph_name":{"type":"string","default":"EventGraph"},
"restore_graph_view_after_capture":{"type":"boolean","default":true},
"capture_width":{"type":"integer","minimum":1,"description":"Optional graph draw width; when graph size is requested the native default is 1440 pixels."},
"capture_height":{"type":"integer","minimum":1,"description":"Optional graph draw height; when graph size is requested the native default is 900 pixels."})schema");
			if (bControlRig) Properties += TEXT(R"schema(,"graph_name_or_path":{"type":"string","description":"Alias used when graph_name is omitted."})schema");
			return ImageParameterSchema(Properties, 85);
		}

		FString SequenceParameterSchema()
		{
			return ImageParameterSchema(TEXT(R"schema(
"asset_path":{"type":"string","description":"Optional asset path; normally derived from subject_ref."},
"target":{"type":"string","enum":["viewport","camera","sequence_camera"],"default":"viewport"},
"width":{"type":"integer","minimum":1},"height":{"type":"integer","minimum":1},
"fallback_to_viewport_camera":{"type":"boolean","default":true},
"frames":{"type":"array","minItems":1,"description":"Explicit display-frame numbers or time/frame objects; overrides count/start/interval. Time uses the sequence display rate.","items":{"anyOf":[
{"type":"number","minimum":0},{"type":"object","properties":{
"time_seconds":{"type":"number","minimum":0},"seconds":{"type":"number","minimum":0},
"frame_index":{"type":"number","minimum":0},"frame":{"type":"number","minimum":0},"display_frame":{"type":"number","minimum":0}},
"anyOf":[{"required":["time_seconds"]},{"required":["seconds"]},{"required":["frame_index"]},{"required":["frame"]},{"required":["display_frame"]}]}]}},
"count":{"type":"integer","minimum":1,"default":1},"frame_count":{"type":"integer","minimum":1},"sample_count":{"type":"integer","minimum":1},
"start_time_seconds":{"type":"number","minimum":0},"first_time_seconds":{"type":"number","minimum":0},"time_seconds":{"type":"number","minimum":0},
"interval_seconds":{"type":"number"},"time_interval_seconds":{"type":"number"},
"start_frame_index":{"type":"number","minimum":0},"first_frame_index":{"type":"number","minimum":0},
"frame_index":{"type":"number","minimum":0},"frame":{"type":"number","minimum":0},"display_frame":{"type":"number","minimum":0},
"frame_interval":{"type":"number","default":1},"frame_stride":{"type":"number"},
"create_frame_sheet":{"type":"boolean","description":"Defaults true for multiple frames. At least sheet or individual output must remain enabled."},
"frame_sheet":{"type":"boolean"},"sequence_frame_image":{"type":"boolean"},
"write_individual_frames":{"type":"boolean","description":"Defaults true for one frame."},
"keep_individual_frames":{"type":"boolean"},"output_individual_frames":{"type":"boolean"})schema"), 95);
		}

		FString TextureParameterSchema()
		{
			return ImageParameterSchema(TEXT(R"schema(
"asset_path":{"type":"string","description":"Optional texture path; normally derived from subject_ref."},
"mip_index":{"type":"integer","minimum":0,"default":0},"slice":{"type":"integer","minimum":0,"default":0},
"face":{"type":"integer","minimum":0,"default":0},"require_non_black":{"type":"boolean","default":false})schema"), INDEX_NONE);
		}

		FString ViewportBufferParameterSchema()
		{
			return ImageParameterSchema(TEXT(R"schema(
"buffer":{"type":"string","default":"SceneDepth","description":"SceneDepth, DeviceDepth, BaseColor or WorldNormal; native aliases are also accepted."})schema"), 85);
		}

		FUeAgentCaptureMediaImagePolicyDescriptor AssetImagePolicy(
			const TCHAR* PolicyId, const TCHAR* Mechanics, const FString& Parameters,
			const TCHAR* Guidance, const TCHAR* ExampleAsset, const TCHAR* ExampleParameters)
		{
			return {PolicyId, Mechanics, Parameters, Guidance,
				TEXT("Finish the returned operation, inspect its native result and materialize the image artifacts, then release. Read reported capture scope and any native editor changes."),
				FString::Printf(TEXT("client.capture.image_execute(subject_ref='capture-subject:asset:%s', policy_id='%s', parameters=%s)"), ExampleAsset, PolicyId, ExampleParameters),
				TEXT(R"schema({"type":"string","pattern":"^capture-subject:asset:.+"})schema")};
		}

		const TArray<FPolicyRoute>& Routes()
		{
			static const TArray<FPolicyRoute> Value = {
				{{TEXT("policy:image-capture:asset-preview/v1"),
					TEXT("mechanics:capture:asset-preview/v1"),
					TEXT("{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"target\":{\"type\":\"string\",\"minLength\":1},\"mode\":{\"type\":\"string\",\"enum\":[\"single\",\"sequence\"]}}}"),
					TEXT("Capture the requested asset's real preview using its applied HJSON preview configuration. Select single or sequence and an optional target from preview/index.hjson; configure size and sampling in that target's capture.hjson."),
					TEXT("Advance to terminal, inspect actual frame times and artifacts, then release."),
					TEXT("client.capture.image_execute(subject_ref='capture-subject:asset:/Game/Example/M_Material.M_Material', policy_id='policy:image-capture:asset-preview/v1', parameters={'mode': 'single'})"),
					TEXT("{\"type\":\"string\",\"pattern\":\"^capture-subject:asset:.+\"}")},
					TEXT("asset_preview_capture"), nullptr, nullptr, FString(), nullptr},
				{AssetImagePolicy(TEXT("policy:image-capture:anim-blueprint/v1"),
					TEXT("mechanics:capture:anim-blueprint-editor/v1"), BlueprintParameterSchema(),
					TEXT("Capture an Animation Blueprint editor viewport or graph. Opening defaults false; graph or window changes require explicit mutation flags. This captures editor UI, not a PIE animation test."),
					TEXT("/Game/Anim/ABP_Example.ABP_Example"), TEXT("{'open_editor_if_needed': True}")),
					TEXT("anim_blueprint_screenshot"),
					nullptr,
					nullptr,
					FString(),
					&FBlueprintEditorCapture::Capture},
				{AssetImagePolicy(TEXT("policy:image-capture:behavior-tree/v1"),
					TEXT("mechanics:capture:behavior-tree-widget/v1"), AssetEditorParameterSchema(),
					TEXT("Capture the native Behavior Tree editor widget. The editor opens if needed by default; this does not execute the behavior tree."),
					TEXT("/Game/AI/BT_Example.BT_Example"), TEXT("{}")),
					TEXT("behavior_tree_screenshot"),
					nullptr,
					&TryCaptureAssetEditorImage,
					TEXT("behavior_tree_not_found")},
				{AssetImagePolicy(TEXT("policy:image-capture:blueprint/v1"),
					TEXT("mechanics:capture:blueprint-editor/v1"), BlueprintParameterSchema(),
					TEXT("Capture a Blueprint editor viewport, component view, graph or window. Opening defaults false. Supply allow_ui_mutation for graph changes and use restore_graph_view_after_capture to restore the graph view."),
					TEXT("/Game/Blueprints/BP_Example.BP_Example"), TEXT("{'open_editor_if_needed': True}")),
					TEXT("blueprint_screenshot"),
					nullptr,
					nullptr,
					FString(),
					&FBlueprintEditorCapture::Capture},
				{AssetImagePolicy(TEXT("policy:image-capture:control-rig/v1"),
					TEXT("mechanics:capture:control-rig-editor/v1"), BlueprintParameterSchema(true),
					TEXT("Capture the Control Rig Blueprint editor. graph_name_or_path is an alias for graph_name; graph/window mutations require explicit flags. This does not run a rig evaluation."),
					TEXT("/Game/Rigs/CR_Example.CR_Example"), TEXT("{'open_editor_if_needed': True}")),
					TEXT("control_rig_screenshot"),
					nullptr,
					nullptr,
					FString(),
					&FBlueprintEditorCapture::CaptureControlRig},
				{AssetImagePolicy(TEXT("policy:image-capture:level-sequence/v1"),
					TEXT("mechanics:capture:level-sequence-evaluation/v1"), SequenceParameterSchema(),
					TEXT("Evaluate a Level Sequence at explicit frames/time objects or count/start/interval samples; default is display frame zero. This is an independent final-color scene render with camera post-processing, not a viewport backbuffer screenshot. Viewport fallback borrows camera geometry, not editor rendering state or exposure history. Multiple frames require sheet or individual output."),
					TEXT("/Game/Cinematics/LS_Example.LS_Example"), TEXT("{'frames': [0]}")),
					TEXT("sequence_screenshot"),
					nullptr,
					nullptr,
					FString(),
					&FSequenceCapture::Capture},
				{AssetImagePolicy(TEXT("policy:image-capture:metasound/v1"),
					TEXT("mechanics:capture:metasound-graph-widget/v1"), AssetEditorParameterSchema(),
					TEXT("Capture the native graph editor for a MetaSound Source or Patch; this is a graph image, not rendered audio."),
					TEXT("/Game/Audio/MS_Example.MS_Example"), TEXT("{}")),
					TEXT("metasound_screenshot"),
					nullptr,
					&TryCaptureMetaSoundEditorImage,
					TEXT("metasound_not_found")},
				{{TEXT("policy:image-capture:pcg-viewport/v1"),
					TEXT("mechanics:capture:pcg-framed-viewport/v1"),
					PcgViewportParameterSchema(),
					TEXT("Capture the active level viewport, optionally framing an exact actor first. Use an actor subject for framing or capture-subject:viewport:active to retain the current framing. Camera position/rotation changes remain applied; restore the previous viewport explicitly when needed. This captures visible PCG output and does not generate a PCG graph."),
					TEXT("Finish the operation, inspect frame_actor and screenshot details and materialize the image artifact, then release. Realtime and temporary view mode follow the viewport capture restoration settings; actor framing does not restore the camera transform."),
					TEXT("client.capture.image_execute(subject_ref='capture-subject:actor:<exact-object-path>', policy_id='policy:image-capture:pcg-viewport/v1', parameters={'padding': 1.1, 'look_at': True})"),
					TEXT("{\"anyOf\":[") + ActorSubjectSchema() + TEXT(",") + ActiveViewportSubjectSchema() + TEXT("]}")},
					TEXT("pcg_screenshot"),
					nullptr,
					&TryCapturePcgViewportImage},
				{AssetImagePolicy(TEXT("policy:image-capture:sound-cue/v1"),
					TEXT("mechanics:capture:sound-cue-graph-widget/v1"), AssetEditorParameterSchema(),
					TEXT("Capture a Sound Cue graph editor widget. The native result also reports node count; no audio playback is implied."),
					TEXT("/Game/Audio/SC_Example.SC_Example"), TEXT("{}")),
					TEXT("sound_cue_screenshot"),
					nullptr,
					&TryCaptureSoundCueEditorImage,
					TEXT("sound_cue_not_found")},
				{AssetImagePolicy(TEXT("policy:image-capture:state-tree/v1"),
					TEXT("mechanics:capture:state-tree-widget/v1"), AssetEditorParameterSchema(),
					TEXT("Capture the native StateTree editor widget. The editor may open if needed; this does not execute the state machine."),
					TEXT("/Game/AI/ST_Example.ST_Example"), TEXT("{}")),
					TEXT("state_tree_screenshot"),
					nullptr,
					&TryCaptureAssetEditorImage,
					TEXT("state_tree_not_found")},
				{AssetImagePolicy(TEXT("policy:image-capture:texture/v1"),
					TEXT("mechanics:capture:texture-source/v1"), TextureParameterSchema(),
					TEXT("Read a texture source image at the requested mip, slice and face. Source pixels must be available; max_size optionally resizes the output. No asset editor window is required."),
					TEXT("/Game/Textures/T_Example.T_Example"), TEXT("{}")),
					TEXT("texture_screenshot"),
					nullptr,
					&TryCaptureTextureImage},
				{{TEXT("policy:image-capture:transform-candidates/v1"),
					TEXT("mechanics:capture:transform-candidates-preview/v1"),
					TransformCandidatesParameterSchema(),
					TEXT("Use subject_ref capture-subject:actor:<exact Actor object path> and provide 1-15 labeled candidates. The policy captures the original plus each candidate from the current Level Viewport into one frame sheet, restores Actor transform, package dirty state, Actor selection, and viewport realtime state, and does not persist or choose a candidate."),
					TEXT("Poll operation.status until terminal. Success publishes one frame-sheet artifact and guarantees the preview state was restored; inspect the sheet, then submit the chosen transform separately and release the operation."),
					TEXT("client.capture.image_execute(subject_ref='capture-subject:actor:<exact-object-path>', policy_id='policy:image-capture:transform-candidates/v1', parameters={'candidates': [{'label': 'candidate-1', 'transform': {'location': {'x': 0, 'y': 0, 'z': 0}}}]})"),
					ActorSubjectSchema()},
					TEXT("probe_actor_transform_candidates"),
					&FWorldViewportCapture::ProbeActorTransformCandidates},
				{{TEXT("policy:image-capture:umg/v1"),
					TEXT("mechanics:capture:umg-offscreen-widget/v1"),
					UmgParameterSchema(),
					TEXT("Capture a WidgetBlueprint in design-time mode. capture_width/capture_height specify canvas pixels; crop_mode=full_canvas preserves that canvas, while region={x,y,width,height} selects a pixel crop. Omit max_size to avoid output resizing. Set use_project_dpi_scale=true for the project viewport DPI rule. This does not run Construct or validate runtime interaction."),
					TEXT("Resume the operation to terminal, read its receipt and materialize the image artifact. The receipt distinguishes design-time rendering, applied DPI scale, canvas and crop. Release the operation after reading artifacts."),
					TEXT("client.capture.image_execute(subject_ref='capture-subject:asset:/Game/UI/WBP_Menu.WBP_Menu', policy_id='policy:image-capture:umg/v1', parameters={'capture_width': 1920, 'capture_height': 1080, 'crop_mode': 'full_canvas', 'use_project_dpi_scale': True})"),
					TEXT("{\"type\":\"string\",\"pattern\":\"^capture-subject:asset:.+\"}")},
					TEXT("umg_screenshot"),
					nullptr,
					&TryCaptureUmgWidgetImage},
				{{TEXT("policy:image-capture:viewport-buffer/v1"),
					TEXT("mechanics:capture:level-viewport-buffer/v1"), ViewportBufferParameterSchema(),
					TEXT("Capture the active level viewport buffer: SceneDepth, DeviceDepth, BaseColor or WorldNormal. The buffer is sampled from the current scene and viewpoint."),
					TEXT("Finish the operation and inspect the buffer image artifact before release."),
					TEXT("client.capture.image_execute(subject_ref='capture-subject:viewport:active', policy_id='policy:image-capture:viewport-buffer/v1', parameters={'buffer': 'SceneDepth'})"),
					ActiveViewportSubjectSchema()},
					TEXT("screenshot_viewport_buffer"),
					nullptr,
					&TryCaptureViewportBufferImage},
				{{TEXT("policy:image-capture:viewport/v1"),
					TEXT("mechanics:capture:level-viewport-visualization/v1"),
					ViewportParameterSchema(),
					TEXT("Use exact subject_ref capture-subject:viewport:active. Optional parameters select png/jpg/jpeg output, quality, maximum size, output path, warmup frames and a temporary view mode that is restored after capture."),
					TEXT("Poll operation.status until terminal, read latest_signal.payload.artifact_ref, then verify the published image artifact before releasing the operation."),
					TEXT("client.capture.image_execute(subject_ref='capture-subject:viewport:active', policy_id='policy:image-capture:viewport/v1', parameters={'format': 'png', 'max_size': 1024, 'restore_view_mode_after_capture': True})"),
					ActiveViewportSubjectSchema()},
					TEXT("screenshot_viewport"),
					nullptr,
					&TryCaptureViewportImage},
				{{TEXT("policy:image-capture:viewport-sequence/v1"),
					TEXT("mechanics:capture:level-viewport-sequence/v1"),
					ViewportSequenceParameterSchema(),
					TEXT("Use exact subject_ref capture-subject:viewport:active. Captures the requested ordered frames across real Editor engine frames from one active Level Viewport and publishes a frame sheet plus raw frame files."),
					TEXT("Poll operation.status until terminal, then inspect frame_sheet_file_path, raw_frames and capture metadata before releasing the operation."),
					TEXT("client.capture.image_execute(subject_ref='capture-subject:viewport:active', policy_id='policy:image-capture:viewport-sequence/v1', parameters={'frame_count': 4, 'interval_frames': 1, 'format': 'png'})"),
					ActiveViewportSubjectSchema()},
					TEXT("screenshot_viewport_sequence"),
					nullptr,
					nullptr,
					FString(),
					nullptr},
			};
			return Value;
		}

		const FPolicyRoute* FindRoute(const FString& PolicyId)
		{
			return Routes().FindByPredicate(
				[&PolicyId](const FPolicyRoute& Route)
				{
					return Route.Descriptor.GetPolicyId() == PolicyId;
				});
		}

		bool TryProjectParameters(
			const FPolicyRoute& Route,
			const FUeAgentCaptureMediaLongJobTypedRequest& Request,
			const FString& PreferredOutputPath,
			TSharedPtr<FJsonObject>& OutParameters,
			FString& OutRequestedFinalPath,
			FString& OutFormat,
			TMap<FString, UE::UAI::Contract::FSourceLocation>& OutSources,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutParameters.Reset();
			OutSources.Reset();
			TSharedPtr<FJsonValue> JsonValue;
			if (!StateSemantic::CanonicalJson::TryToJsonValue(
					Request.GetParameters().Get(), JsonValue, OutError)
				|| !JsonValue.IsValid()
				|| JsonValue->Type != EJson::Object)
			{
				if (OutError.IsEmpty()) OutError = Input(TEXT("capture_image_parameters_invalid"), TEXT("Capture image parameters must be an object."),
					TEXT("capture_prepare"), TEXT("/parameters"), JsonValue, MakeShared<FJsonValueString>(TEXT("object")));
				return false;
			}
			OutParameters = MakeShared<FJsonObject>();
			OutParameters->Values = JsonValue->AsObject()->Values;
			const auto ResolveOriginalParameter = [](const FString& Pointer)
			{
				UE::UAI::Contract::FSourceLocation Source;
				Source.JsonPath = TEXT("/parameters") + Pointer;
				Source.bCommandInput = true;
				return Source;
			};
			UeAgentAuthoringDiagnostics::FScopedSourceLocations SourceScope(
				JsonValue->AsObject().Get(), ResolveOriginalParameter, true);
			OutSources = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*OutParameters);
			OutSources.Add(TEXT("/"), ResolveOriginalParameter(FString()));
			FString RequestedOutput;
			if (OutParameters->TryGetStringField(
					TEXT("output_file"), RequestedOutput)
				&& !RequestedOutput.TrimStartAndEnd().IsEmpty())
			{
				OutRequestedFinalPath = UeAgentJsonDiagnostics::
					ResolveProjectRelativeFilePath(RequestedOutput);
			}
			const FString& Subject = Request.GetSubjectRef();
			const bool bActorSubject = Subject.StartsWith(TEXT("capture-subject:actor:"));
			const bool bAssetSubject = Subject.StartsWith(TEXT("capture-subject:asset:"));
			const bool bPcgViewport = Route.Descriptor.GetPolicyId() == TEXT("policy:image-capture:pcg-viewport/v1");
			// Resolve all provided aliases before any native framing or preview action.
			// They may use different readable spellings, but must denote one subject.
			if (bActorSubject || bPcgViewport)
			{
				TArray<UObject*> Actors;
				if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
					for (TActorIterator<AActor> It(World); It; ++It) Actors.Add(*It);
				UObject* Selected = nullptr;
				auto ResolveActor = [&](FString Reference, const FString& Pointer)
				{
					Reference.RemoveFromStart(TEXT("actor:"));
					UObject* Resolved = nullptr;
					if (!ContentDiscovery::TryResolveScopedObjectReference(Actors, Reference, Resolved, OutError))
					{
						OutError.Source.JsonPath = Pointer;
						OutError.Source.bCommandInput = true;
						return false;
					}
					if (Selected && Resolved != Selected)
					{
						OutError = Input(TEXT("capture_subject_alias_mismatch"), TEXT("The capture subject and framing aliases must identify the same actor."),
							TEXT("capture_prepare"), Pointer, MakeShared<FJsonValueString>(Resolved->GetPathName()), MakeShared<FJsonValueString>(Selected->GetPathName()));
						OutError.TargetPath = Selected->GetPathName();
						return false;
					}
					Selected = Resolved;
					return true;
				};
				if (bActorSubject && !ResolveActor(Subject.Mid(22), TEXT("/subject_ref"))) return false;
				for (const TCHAR* Field : {TEXT("actor"), TEXT("actor_name"), TEXT("actor_label"), TEXT("id")})
				{
					FString Alias;
					if (!OutParameters->TryGetStringField(Field, Alias) || Alias.IsEmpty()) continue;
					if (!ResolveActor(Alias, FString(TEXT("/parameters/")) + Field)) return false;
				}
			}
			if (bAssetSubject && OutParameters->HasField(TEXT("asset_path")))
			{
				FString Alias;
				if (OutParameters->TryGetStringField(TEXT("asset_path"), Alias)
					&& UeAgentAuthoringAssetIdentity::NormalizePackagePath(Alias)
						!= UeAgentAuthoringAssetIdentity::NormalizePackagePath(Subject.Mid(22)))
				{
					OutError = Input(TEXT("capture_subject_alias_mismatch"), TEXT("The asset_path alias must identify the capture subject asset."),
						TEXT("capture_prepare"), TEXT("/parameters/asset_path"), MakeShared<FJsonValueString>(Alias), MakeShared<FJsonValueString>(Subject.Mid(22)));
					OutError.TargetPath = Subject.Mid(22);
					return false;
				}
			}
			const auto LocateSubjectParameter = [&OutSources](const TCHAR* Field)
			{
				MarkDerivedParameter(OutSources, Field);
				UE::UAI::Contract::FSourceLocation Source;
				Source.JsonPath = TEXT("/subject_ref");
				Source.bCommandInput = true;
				OutSources.Add(FString(TEXT("/")) + Field, MoveTemp(Source));
			};
			if (Subject.StartsWith(TEXT("capture-subject:asset:"))
				&& !OutParameters->HasField(TEXT("asset_path")))
			{
				OutParameters->SetStringField(
					TEXT("asset_path"), Subject.Mid(22));
				LocateSubjectParameter(TEXT("asset_path"));
			}
			else if (Subject.StartsWith(TEXT("capture-subject:actor:")))
			{
				const FString ActorRef = Subject.Mid(22);
				if (Route.Descriptor.GetPolicyId()
					== TEXT("policy:image-capture:transform-candidates/v1"))
				{
					OutParameters->SetStringField(TEXT("id"), ActorRef);
					OutParameters->SetBoolField(TEXT("create_frame_sheet"), true);
					OutParameters->SetBoolField(TEXT("write_individual_frames"), false);
					LocateSubjectParameter(TEXT("id"));
					for (const TCHAR* Field : { TEXT("create_frame_sheet"), TEXT("write_individual_frames") }) MarkDerivedParameter(OutSources, Field);
				}
				else if (!OutParameters->HasField(TEXT("actor")))
				{
					OutParameters->SetStringField(TEXT("actor"), ActorRef);
					LocateSubjectParameter(TEXT("actor"));
				}
			}
			OutFormat = TEXT("png");
			if (!OutParameters->TryGetStringField(TEXT("format"), OutFormat)) MarkDerivedParameter(OutSources, TEXT("format"));
			if (Route.Descriptor.GetPolicyId() == TEXT("policy:image-capture:asset-preview/v1"))
			{
				FString Target;
				OutParameters->TryGetStringField(TEXT("target"), Target);
				const auto State = AssetPreview::FindLocalState(OutParameters->GetStringField(TEXT("asset_path")), Target);
				const FUeAgentAssetPreviewCapture Config = State ? State->Capture : FUeAgentAssetPreviewCapture();
				FString Mode = TEXT("single");
				OutParameters->TryGetStringField(TEXT("mode"), Mode);
				if (Mode != TEXT("single") && Mode != TEXT("sequence"))
				{
					OutError = Input(TEXT("asset_preview_capture_mode_invalid"), TEXT("Asset preview capture mode must be single or sequence."),
						TEXT("capture_prepare"), TEXT("/mode"), OutParameters->TryGetField(TEXT("mode")), MakeShared<FJsonValueString>(TEXT("single or sequence")));
					LocateProjectedDiagnostic(OutError, OutSources);
					return false;
				}
				OutFormat = Config.Format == EUeAgentAssetPreviewImageFormat::Jpeg ? TEXT("jpg") : TEXT("png");
				OutParameters->SetBoolField(TEXT("asset_preview"), true);
				OutParameters->SetNumberField(TEXT("frame_count"), Mode == TEXT("sequence") ? Config.FrameCount : 1);
				OutParameters->SetNumberField(TEXT("capture_width"), Config.Width);
				OutParameters->SetNumberField(TEXT("capture_height"), Config.Height);
				OutParameters->SetNumberField(TEXT("render_width"), Config.RenderWidth);
				OutParameters->SetNumberField(TEXT("render_height"), Config.RenderHeight);
				OutParameters->SetNumberField(TEXT("interval_seconds"), Config.IntervalSeconds);
				OutParameters->SetBoolField(TEXT("timeline_sampling"), Config.Sampling != EUeAgentAssetPreviewSampling::Realtime);
				OutParameters->SetBoolField(TEXT("continuous_sampling"), Config.Sampling == EUeAgentAssetPreviewSampling::Continuous);
				OutParameters->SetBoolField(TEXT("timeline_frame_units"), Config.TimeUnit == EUeAgentAssetPreviewTimeUnit::Frames);
				OutParameters->SetNumberField(TEXT("timeline_start_frame"), Config.StartFrame);
				OutParameters->SetNumberField(TEXT("timeline_step_frames"), Config.StepFrames);
				OutParameters->SetNumberField(TEXT("timeline_start_seconds"), Config.StartTimeSeconds);
				OutParameters->SetNumberField(TEXT("timeline_step_seconds"), Config.StepTimeSeconds);
				OutParameters->SetBoolField(TEXT("reset_before_capture"), Config.ResetBeforeCapture);
				OutParameters->SetNumberField(TEXT("warmup_seconds"), Config.WarmupSeconds);
				for (const TCHAR* Field : { TEXT("format"), TEXT("asset_preview"), TEXT("frame_count"), TEXT("capture_width"), TEXT("capture_height"),
					TEXT("render_width"), TEXT("render_height"), TEXT("interval_seconds"), TEXT("timeline_sampling"), TEXT("continuous_sampling"),
					TEXT("timeline_frame_units"), TEXT("timeline_start_frame"), TEXT("timeline_step_frames"), TEXT("timeline_start_seconds"),
					TEXT("timeline_step_seconds"), TEXT("reset_before_capture"), TEXT("warmup_seconds") })
					MarkDerivedParameter(OutSources, Field);
			}
			OutFormat.TrimStartAndEndInline();
			OutFormat.ToLowerInline();
			if (OutFormat == TEXT("jpeg"))
			{
				OutFormat = TEXT("jpg");
			}
			if (OutFormat != TEXT("png") && OutFormat != TEXT("jpg"))
			{
				OutError = Input(TEXT("capture_image_format_unsupported"), TEXT("The image capture format is unsupported."),
					TEXT("capture_prepare"), TEXT("/format"), OutParameters->TryGetField(TEXT("format")), MakeShared<FJsonValueString>(TEXT("png or jpg/jpeg")));
				LocateProjectedDiagnostic(OutError, OutSources);
				return false;
			}
			if (!OutRequestedFinalPath.IsEmpty())
			{
				FString Extension = FPaths::GetExtension(
					OutRequestedFinalPath).ToLower();
				if (Extension == TEXT("jpeg"))
				{
					Extension = TEXT("jpg");
				}
				if (Extension.IsEmpty())
				{
					OutRequestedFinalPath += TEXT(".") + OutFormat;
				}
				else if (Extension != OutFormat)
				{
					OutError = Input(TEXT("capture_image_output_extension_mismatch"), TEXT("The requested output file extension differs from the capture image format."),
						TEXT("capture_prepare"), TEXT("/output_file"), MakeShared<FJsonValueString>(Extension), MakeShared<FJsonValueString>(OutFormat));
					OutError.TargetPath = OutRequestedFinalPath;
					LocateProjectedDiagnostic(OutError, OutSources);
					return false;
				}
			}
			OutParameters->SetStringField(TEXT("format"), OutFormat);
			OutParameters->SetStringField(
				TEXT("output_file"),
				FPaths::ChangeExtension(PreferredOutputPath, OutFormat));
			MarkDerivedParameter(OutSources, TEXT("output_file"));
			if (Route.Descriptor.GetPolicyId()
				== TEXT("policy:image-capture:transform-candidates/v1"))
			{
				if (!TryNormalizeTransformCandidateParameters(
						OutParameters, OutError, &OutSources))
				{
					LocateProjectedDiagnostic(OutError, OutSources);
					return false;
				}
				OutParameters->SetStringField(
					TEXT("frame_sheet_output_file"),
					FPaths::ChangeExtension(PreferredOutputPath, OutFormat));
				MarkDerivedParameter(OutSources, TEXT("frame_sheet_output_file"));
			}
			else if (IsViewportSequenceCapturePolicy(Route.Descriptor.GetPolicyId()))
			{
				OutParameters->SetStringField(
					TEXT("frame_sheet_output_file"),
					FPaths::ChangeExtension(PreferredOutputPath, OutFormat));
				MarkDerivedParameter(OutSources, TEXT("frame_sheet_output_file"));
			}
			return true;
		}

		bool TryResolveProducedPath(
			const TSharedPtr<FJsonObject>& Result,
			const TSharedPtr<FJsonObject>& Parameters,
			FString& OutPath,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutPath.Reset();
			OutError.Reset();
			const auto AttemptedPaths = MakeShared<FJsonObject>();
			const auto AttemptedStates = MakeShared<FJsonObject>();
			const auto TryPath = [&](const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& Pointer)
			{
				const auto Value = Object.IsValid() ? Object->TryGetField(Field) : nullptr;
				const bool bExists = Object.IsValid() && Object->TryGetStringField(Field, OutPath)
					&& FPaths::FileExists(OutPath);
AttemptedPaths->SetField(Pointer, Facts({
					{ TEXT("value"), Value }, { TEXT("exists"), MakeShared<FJsonValueBoolean>(bExists) } }));
				AttemptedStates->SetField(Pointer, Facts({
					{ TEXT("path_provided"), MakeShared<FJsonValueBoolean>(Value.IsValid() && Value->Type == EJson::String) },
					{ TEXT("exists"), MakeShared<FJsonValueBoolean>(bExists) } }));
				return bExists;
			};
			if (Result.IsValid())
			{
				for (const TCHAR* Field
					: {TEXT("frame_sheet_file_path"), TEXT("file_path"),
						TEXT("output_file"), TEXT("path")})
				{
					if (TryPath(Result, Field, FString(TEXT("/result/")) + Field)) return true;
				}
			}
			if (TryPath(Parameters, TEXT("output_file"), TEXT("/parameters/output_file"))) return true;
			OutError = Native(TEXT("capture_image_output_missing"),
				TEXT("Capture returned no path to an existing output file, and the prepared output path does not exist."),
				TEXT("capture_finalize"), TEXT("output_file"), MakeShared<FJsonValueObject>(AttemptedStates),
				MakeShared<FJsonValueString>(TEXT("at least one existing output file")));
			OutError.ActualEvidence = MakeShared<FJsonValueObject>(AttemptedPaths);
			return false;
		}

		void CopyReceiptField(
			const TSharedPtr<FJsonObject>& Source,
			const TSharedRef<FJsonObject>& Receipt,
			const TCHAR* Field)
		{
			if (Source.IsValid())
			{
				const TSharedPtr<FJsonValue>* Value = Source->Values.Find(Field);
				if (Value && Value->IsValid())
				{
					Receipt->SetField(Field, *Value);
				}
			}
		}

		bool TryBuildCaptureReceipt(
			const FPolicyRoute& Route,
			const FUeAgentCaptureMediaLongJobTypedRequest& Request,
			const TSharedPtr<FJsonObject>& Result,
			FUeAgentCaptureMediaImageExecutionResult& OutResult,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const bool bAssetPreview = Route.Descriptor.GetPolicyId() == TEXT("policy:image-capture:asset-preview/v1");
			const bool bSinglePreview = bAssetPreview && Result.IsValid()
				&& !Result->HasField(TEXT("frame_sheet_file_path"));
			const bool bViewportSequence = IsViewportSequenceCapturePolicy(Route.Descriptor.GetPolicyId()) && !bSinglePreview;
			const bool bLevelSequence = Route.Descriptor.GetPolicyId() == TEXT("policy:image-capture:level-sequence/v1");

			const TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
			Receipt->SetStringField(
				TEXT("schema"), TEXT("uai.capture.image.receipt.v1"));
			Receipt->SetStringField(
				TEXT("policy_id"), Route.Descriptor.GetPolicyId());
			Receipt->SetStringField(
				TEXT("mechanics_id"), Route.Descriptor.GetMechanicsId());
			Receipt->SetStringField(TEXT("subject_ref"), Request.GetSubjectRef());
			Receipt->SetStringField(
				TEXT("captured_at_utc"), FDateTime::UtcNow().ToIso8601());
			for (const TCHAR* Field : {
				TEXT("width"), TEXT("height"), TEXT("bytes"), TEXT("format"),
				TEXT("capture_mode"), TEXT("render_path"), TEXT("final_lit_composition"),
				TEXT("scene_capture_buffer"), TEXT("warmup_frames_requested"),
				TEXT("capture"), TEXT("viewport"),
				TEXT("requested_view_mode"), TEXT("view_mode_before_capture"),
				TEXT("view_mode_applied"), TEXT("view_mode_after_restore"),
				TEXT("view_mode_left_applied"), TEXT("asset_path"),
				TEXT("canvas"), TEXT("crop"), TEXT("crop_mode"),
				TEXT("use_project_dpi_scale"), TEXT("dpi_scale"),
				TEXT("widget_execution_mode"), TEXT("preserve_alpha"),
				TEXT("frame_index"), TEXT("time_seconds"), TEXT("frame_count"),
				TEXT("camera_source"), TEXT("sequence_camera_frame_count"), TEXT("viewport_fallback_frame_count"),
				TEXT("fallback_to_viewport_camera"), TEXT("frame_sheet_created"), TEXT("individual_frame_files_created"),
				TEXT("display_rate_numerator"), TEXT("display_rate_denominator"),
				TEXT("tick_resolution_numerator"), TEXT("tick_resolution_denominator"),
				TEXT("world_package_was_dirty_before_capture"), TEXT("restored_world_dirty_state")})
			{
				CopyReceiptField(Result, Receipt, Field);
			}
			const TSharedPtr<FJsonObject>* FrameSheet = nullptr;
			if (Result.IsValid()
				&& Result->TryGetObjectField(TEXT("frame_sheet"), FrameSheet)
				&& FrameSheet && FrameSheet->IsValid())
			{
				const TSharedRef<FJsonObject> PublicSheet = MakeShared<FJsonObject>();
				PublicSheet->Values = (*FrameSheet)->Values;
				PublicSheet->RemoveField(TEXT("file_path"));
				Receipt->SetObjectField(TEXT("frame_sheet"), PublicSheet);
			}

			if (bViewportSequence)
			{
				const TArray<TSharedPtr<FJsonValue>>* Frames = nullptr;
				if (!Result.IsValid()
					|| !Result->TryGetArrayField(TEXT("raw_frames"), Frames)
					|| !Frames || Frames->Num() < 1)
				{
					OutError = Native(TEXT("capture_viewport_sequence_frames_missing"),
						TEXT("Viewport sequence capture returned no nonempty raw_frames array."),
						TEXT("capture_finalize"), TEXT("raw_frames"), Result.IsValid() ? Result->TryGetField(TEXT("raw_frames")) : nullptr,
						MakeShared<FJsonValueString>(TEXT("nonempty array")));
					return false;
				}
				TArray<TSharedPtr<FJsonValue>> PublicFrames;
				PublicFrames.Reserve(Frames->Num());
				OutResult.AdditionalProducedPaths.Reserve(Frames->Num());
				for (int32 Index = 0; Index < Frames->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject>* Frame = nullptr;
					FString RawPath;
					if (!(*Frames)[Index].IsValid()
						|| !(*Frames)[Index]->TryGetObject(Frame)
						|| !Frame || !Frame->IsValid())
					{
						OutError = Native(TEXT("capture_viewport_sequence_frame_missing"),
							TEXT("A captured frame receipt is not an object."), TEXT("capture_finalize"),
							FString::Printf(TEXT("raw_frames/%d"), Index), (*Frames)[Index], MakeShared<FJsonValueString>(TEXT("object")));
						return false;
					}
					if (!(*Frame)->TryGetStringField(TEXT("raw_file_path"), RawPath) || !FPaths::FileExists(RawPath))
					{
						OutError = Native(TEXT("capture_viewport_sequence_frame_missing"),
							TEXT("A captured frame receipt has no path to an existing raw image file."), TEXT("capture_finalize"),
							FString::Printf(TEXT("raw_frames/%d/raw_file_path"), Index),
							Facts({ { TEXT("path_provided"), MakeShared<FJsonValueBoolean>(!RawPath.IsEmpty()) },
								{ TEXT("exists"), MakeShared<FJsonValueBoolean>(false) } }),
							MakeShared<FJsonValueString>(TEXT("path to an existing raw image file")));
						OutError.ActualEvidence = (*Frame)->TryGetField(TEXT("raw_file_path"));
						return false;
					}
					OutResult.AdditionalProducedPaths.Add(
						FPaths::ConvertRelativePathToFull(RawPath));
					const TSharedRef<FJsonObject> PublicFrame =
						MakeShared<FJsonObject>();
					PublicFrame->Values = (*Frame)->Values;
					PublicFrame->RemoveField(TEXT("raw_file_path"));
					PublicFrame->SetNumberField(
						TEXT("raw_artifact_index"), Index);
					PublicFrames.Add(MakeShared<FJsonValueObject>(PublicFrame));
				}
				Receipt->SetArrayField(TEXT("frames"), MoveTemp(PublicFrames));
			}
			if (bLevelSequence)
			{
				const TArray<TSharedPtr<FJsonValue>>* Frames = nullptr;
				if (Result.IsValid() && Result->TryGetArrayField(TEXT("frames"), Frames) && Frames)
				{
					TArray<TSharedPtr<FJsonValue>> PublicFrames;
					PublicFrames.Reserve(Frames->Num());
					for (int32 Index = 0; Index < Frames->Num(); ++Index)
					{
						const TSharedPtr<FJsonObject>* Frame = nullptr;
						if (!(*Frames)[Index].IsValid() || !(*Frames)[Index]->TryGetObject(Frame) || !Frame || !Frame->IsValid())
						{
							OutError = Native(TEXT("capture_sequence_frame_invalid"), TEXT("The sequence capture returned a frame that is not an object."),
								TEXT("capture_finalize"), FString::Printf(TEXT("frames/%d"), Index), (*Frames)[Index], MakeShared<FJsonValueString>(TEXT("frame object")));
							return false;
						}
						const TSharedRef<FJsonObject> PublicFrame = MakeShared<FJsonObject>();
						PublicFrame->Values = (*Frame)->Values;
						PublicFrame->RemoveField(TEXT("file_path"));
						FString FramePath;
						if ((*Frame)->TryGetStringField(TEXT("file_path"), FramePath) && !FramePath.IsEmpty())
						{
							FramePath = FPaths::ConvertRelativePathToFull(FramePath);
							if (FPaths::IsSamePath(FramePath, OutResult.ProducedPath))
							{
								PublicFrame->SetBoolField(TEXT("uses_primary_artifact"), true);
							}
							else
							{
								int32 ArtifactIndex = OutResult.AdditionalProducedPaths.IndexOfByPredicate(
									[&FramePath](const FString& Path) { return FPaths::IsSamePath(Path, FramePath); });
								if (ArtifactIndex == INDEX_NONE) ArtifactIndex = OutResult.AdditionalProducedPaths.Add(FramePath);
								PublicFrame->SetNumberField(TEXT("raw_artifact_index"), ArtifactIndex);
							}
						}
						PublicFrames.Add(MakeShared<FJsonValueObject>(PublicFrame));
					}
					Receipt->SetArrayField(TEXT("frames"), MoveTemp(PublicFrames));
				}
			}
			OutResult.CaptureReceipt = Receipt;
			return true;
		}
	}

	FUeAgentCaptureMediaImagePolicyDescriptor::
	FUeAgentCaptureMediaImagePolicyDescriptor(
		FString InPolicyId,
		FString InMechanicsId,
		FString InParameterSchemaJson,
		FString InParameterGuidance,
		FString InResultGuidance,
		FString InMinimumExample,
		FString InSubjectSchemaJson)
		: PolicyId(MoveTemp(InPolicyId))
		, MechanicsId(MoveTemp(InMechanicsId))
		, ParameterSchemaJson(MoveTemp(InParameterSchemaJson))
		, SubjectSchemaJson(MoveTemp(InSubjectSchemaJson))
		, ParameterGuidance(MoveTemp(InParameterGuidance))
		, ResultGuidance(MoveTemp(InResultGuidance))
		, MinimumExample(MoveTemp(InMinimumExample))
	{
	}

	const FString&
	FUeAgentCaptureMediaImagePolicyDescriptor::GetPolicyId() const
	{
		return PolicyId;
	}

	const FString&
	FUeAgentCaptureMediaImagePolicyDescriptor::GetMechanicsId() const
	{
		return MechanicsId;
	}

	const FString& FUeAgentCaptureMediaImagePolicyDescriptor::
	GetParameterSchemaJson() const
	{
		return ParameterSchemaJson;
	}

	const FString& FUeAgentCaptureMediaImagePolicyDescriptor::
	GetSubjectSchemaJson() const
	{
		return SubjectSchemaJson;
	}

	const FString& FUeAgentCaptureMediaImagePolicyDescriptor::
	GetParameterGuidance() const
	{
		return ParameterGuidance;
	}

	const FString& FUeAgentCaptureMediaImagePolicyDescriptor::
	GetResultGuidance() const
	{
		return ResultGuidance;
	}

	const FString& FUeAgentCaptureMediaImagePolicyDescriptor::
	GetMinimumExample() const
	{
		return MinimumExample;
	}

	bool FUeAgentCaptureMediaImagePolicyDescriptor::IsValid() const
	{
		UE::UAI::Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentCaptureMediaImagePolicyDescriptor::TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (!PolicyId.StartsWith(TEXT("policy:image-capture:")))
		{
			OutError = Native(TEXT("capture_image_policy_descriptor_invalid"),
				TEXT("An image capture policy ID must start with policy:image-capture:."), TEXT("policy_validation"), TEXT("policy_id"),
				MakeShared<FJsonValueString>(PolicyId), MakeShared<FJsonValueString>(TEXT("policy:image-capture:<name>")));
			OutError.CandidateName = PolicyId;
			return false;
		}
		if (!MechanicsId.StartsWith(TEXT("mechanics:capture:")))
		{
			OutError = Native(TEXT("capture_image_policy_descriptor_invalid"),
				TEXT("The image policy references an invalid capture mechanics ID."), TEXT("policy_validation"), TEXT("mechanics_id_valid"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.CandidateName = PolicyId;
			OutError.ActualEvidence = MakeShared<FJsonValueString>(MechanicsId);
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(TEXT("mechanics:capture:<name>"));
			return false;
		}
		const bool bHasAnyDiscoveryField = !ParameterSchemaJson.IsEmpty()
			|| !SubjectSchemaJson.IsEmpty() || !ParameterGuidance.IsEmpty()
			|| !ResultGuidance.IsEmpty() || !MinimumExample.IsEmpty();
		if (bHasAnyDiscoveryField)
		{
			for (const auto& Field : {TPair<const TCHAR*, const FString*>(TEXT("parameter_schema"), &ParameterSchemaJson),
				TPair<const TCHAR*, const FString*>(TEXT("subject_schema"), &SubjectSchemaJson),
				TPair<const TCHAR*, const FString*>(TEXT("parameter_guidance"), &ParameterGuidance),
				TPair<const TCHAR*, const FString*>(TEXT("result_guidance"), &ResultGuidance),
				TPair<const TCHAR*, const FString*>(TEXT("minimum_example"), &MinimumExample)})
			{
				if (Field.Value->IsEmpty())
				{
					OutError = Native(TEXT("capture_image_policy_descriptor_invalid"),
						TEXT("A partially specified image policy discovery contract is missing a required field."), TEXT("policy_validation"), Field.Key,
						MakeShared<FJsonValueString>(*Field.Value), MakeShared<FJsonValueString>(TEXT("nonempty string")));
					OutError.CandidateName = PolicyId;
					return false;
				}
			}
		}
		return true;
	}

	const TArray<FUeAgentCaptureMediaImagePolicyDescriptor>&
	GetCaptureMediaImagePolicies()
	{
		static const TArray<FUeAgentCaptureMediaImagePolicyDescriptor> Policies = []
		{
			TArray<FUeAgentCaptureMediaImagePolicyDescriptor> Value;
			for (const FPolicyRoute& Route : Routes())
			{
				Value.Add(Route.Descriptor);
			}
			Value.Sort([](const auto& A, const auto& B)
			{
				return A.GetPolicyId() < B.GetPolicyId();
			});
			return Value;
		}();
		return Policies;
	}

	const FUeAgentCaptureMediaImagePolicyDescriptor*
	FindCaptureMediaImagePolicy(const FString& PolicyId)
	{
		return GetCaptureMediaImagePolicies().FindByPredicate(
			[&PolicyId](const FUeAgentCaptureMediaImagePolicyDescriptor& Policy)
			{
				return Policy.GetPolicyId() == PolicyId;
			});
	}

	bool TryNormalizeTransformCandidateParameters(
		const TSharedPtr<FJsonObject>& Parameters,
		UE::UAI::Contract::FDiagnostic& OutError,
		TMap<FString, UE::UAI::Contract::FSourceLocation>* ParameterSources)
	{
		OutError.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
		if (!Parameters.IsValid()
			|| !Parameters->TryGetArrayField(TEXT("candidates"), Candidates)
			|| !Candidates)
		{
			OutError = Input(TEXT("capture_transform_candidates_missing_candidates"), TEXT("Transform probing requires a candidates array."),
				TEXT("capture_prepare"), TEXT("/candidates"), Parameters.IsValid() ? Parameters->TryGetField(TEXT("candidates")) : nullptr,
				MakeShared<FJsonValueString>(TEXT("array")));
			return false;
		}
		for (int32 Index = 0; Index < Candidates->Num(); ++Index)
		{
			const auto& CandidateValue = (*Candidates)[Index];
			FString TransformPointer = TEXT("/candidates/") + ::LexToString(Index);
			const TSharedPtr<FJsonObject>* Candidate = nullptr;
			if (!CandidateValue.IsValid()
				|| !CandidateValue->TryGetObject(Candidate)
				|| !Candidate || !Candidate->IsValid())
			{
				OutError = Input(TEXT("capture_transform_candidate_invalid"), TEXT("Each transform candidate must be an object."),
					TEXT("capture_prepare"), TransformPointer, CandidateValue, MakeShared<FJsonValueString>(TEXT("object")));
				return false;
			}
			TSharedPtr<FJsonObject> Transform = *Candidate;
			const TSharedPtr<FJsonObject>* Nested = nullptr;
			if ((*Candidate)->TryGetObjectField(TEXT("transform"), Nested)
				&& Nested && Nested->IsValid())
			{
				Transform = *Nested;
				TransformPointer += TEXT("/transform");
			}
			auto NormalizeField = [&Transform, &OutError, &TransformPointer, ParameterSources](
				const TCHAR* Field,
				std::initializer_list<const TCHAR*> ComponentNames)
			{
				const TSharedPtr<FJsonObject>* Named = nullptr;
				if (!Transform->TryGetObjectField(Field, Named)
					|| !Named || !Named->IsValid()
					|| (*Named)->HasField(TEXT("components")))
				{
					return true;
				}
				TArray<TSharedPtr<FJsonValue>> Components;
				int32 ComponentIndex = 0;
				for (const TCHAR* ComponentName : ComponentNames)
				{
					double Component = 0.0;
					if (!(*Named)->TryGetNumberField(ComponentName, Component))
					{
						OutError = Input(TEXT("capture_transform_candidate_component_missing"), TEXT("The named transform component must be a number."),
							TEXT("capture_prepare"), TransformPointer + TEXT("/") + Field + TEXT("/") + ComponentName,
							(*Named)->TryGetField(ComponentName), MakeShared<FJsonValueString>(TEXT("number")));
						return false;
					}
					Components.Add(MakeShared<FJsonValueNumber>(Component));
					if (ParameterSources)
					{
						const FString OriginalPointer = TransformPointer + TEXT("/") + Field + TEXT("/") + ComponentName;
						const auto Source = UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(*ParameterSources, OriginalPointer);
						ParameterSources->Add(TransformPointer + TEXT("/") + Field + TEXT("/components/") + ::LexToString(ComponentIndex), Source);
					}
					++ComponentIndex;
				}
				const TSharedRef<FJsonObject> Canonical = MakeShared<FJsonObject>();
				Canonical->SetArrayField(TEXT("components"), MoveTemp(Components));
				Transform->SetObjectField(Field, Canonical);
				return true;
			};
			if (!NormalizeField(TEXT("location"), {TEXT("x"), TEXT("y"), TEXT("z")})
				|| !NormalizeField(
					TEXT("rotation"), {TEXT("pitch"), TEXT("yaw"), TEXT("roll")})
				|| !NormalizeField(TEXT("scale"), {TEXT("x"), TEXT("y"), TEXT("z")}))
			{
				return false;
			}
		}
		return true;
	}

	bool FUeAgentCaptureMediaImageExecutionResult::IsValid() const
	{
		UE::UAI::Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentCaptureMediaImageExecutionResult::TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		FString MissingField;
		if (ProducedPath.IsEmpty()) MissingField = TEXT("produced_path");
		else if (Format.IsEmpty()) MissingField = TEXT("format");
		else
		{
			const int32 Index = AdditionalProducedPaths.IndexOfByPredicate([](const FString& Path) { return Path.IsEmpty(); });
			if (Index != INDEX_NONE) MissingField = FString::Printf(TEXT("additional_produced_paths/%d"), Index);
		}
		if (MissingField.IsEmpty()) return true;
		OutError = Native(TEXT("capture_image_result_invalid"), TEXT("Capture produced an incomplete image execution result."),
			TEXT("capture_finalize"), MissingField, MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("nonempty string")));
		return false;
	}

	bool FUeAgentPreparedCaptureMediaImageExecution::IsValid() const
	{
		UE::UAI::Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentPreparedCaptureMediaImageExecution::TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (!Context.Params.IsValid())
		{
			OutError = Native(TEXT("capture_image_finalize_contract_invalid"), TEXT("The prepared capture has no parameter object."),
				TEXT("capture_prepare"), TEXT("parameters"), nullptr, MakeShared<FJsonValueString>(TEXT("object")));
			return false;
		}
		if (Context.Command.IsEmpty())
		{
			OutError = Native(TEXT("capture_image_finalize_contract_invalid"), TEXT("The prepared capture has no registered execution command."),
				TEXT("capture_prepare"), TEXT("execution_command_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Context.Command);
			return false;
		}
		if (Format.IsEmpty())
		{
			OutError = Native(TEXT("capture_image_finalize_contract_invalid"), TEXT("The prepared capture has no output format."),
				TEXT("capture_prepare"), TEXT("format"), MakeShared<FJsonValueString>(Format), MakeShared<FJsonValueString>(TEXT("nonempty string")));
			return false;
		}
		return true;
	}

	void FUeAgentPreparedCaptureMediaImageExecution::LocateDiagnosticSources(UE::UAI::Contract::FDiagnostic& Diagnostic) const
	{
		LocateProjectedDiagnostic(Diagnostic, ParameterSources);
	}

	bool IsViewportSequenceCapturePolicy(const FString& PolicyId)
	{
		return PolicyId == TEXT("policy:image-capture:viewport-sequence/v1")
			|| PolicyId == TEXT("policy:image-capture:asset-preview/v1");
	}

	bool TryPrepareCaptureMediaImagePolicyExecution(
		const FUeAgentCaptureMediaLongJobTypedRequest& Request,
		const FString& PreferredOutputPath,
		FUeAgentPreparedCaptureMediaImageExecution& OutPrepared,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutPrepared = {};
		OutError.Reset();
		if (!IsInGameThread())
		{
			OutError = Native(TEXT("capture_image_game_thread_required"), TEXT("Image capture preparation must run on the game thread."),
				TEXT("capture_prepare"), TEXT("game_thread"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		const FPolicyRoute* const Route = FindRoute(Request.GetPolicyId());
		if (!Route)
		{
			OutError = Input(TEXT("capture_image_policy_unsupported"), TEXT("No image capture policy is registered for the requested policy ID."),
				TEXT("capture_prepare"), TEXT("/policy_id"), MakeShared<FJsonValueString>(Request.GetPolicyId()),
				MakeShared<FJsonValueString>(TEXT("registered image capture policy ID")));
			return false;
		}
		if (!Route->Descriptor.TryValidate(OutError)) return false;
		if (!FApp::CanEverRender())
		{
			OutError = Native(TEXT("capture_image_rhi_unavailable"), TEXT("This process cannot render images."),
				TEXT("capture_prepare"), TEXT("rendering_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.CandidateName = Request.GetPolicyId();
			return false;
		}
		TSharedPtr<FJsonObject> Parameters;
		if (!TryProjectParameters(
				*Route,
				Request,
				PreferredOutputPath,
				Parameters,
				OutPrepared.RequestedFinalPath,
				OutPrepared.Format,
				OutPrepared.ParameterSources,
				OutError))
		{
			return false;
		}
		OutPrepared.Context.RequestId =
			FGuid::NewGuid().ToString(EGuidFormats::Digits);
		OutPrepared.Context.Command = Route->InternalCommandId;
		OutPrepared.Context.Params = Parameters;
		return OutPrepared.TryValidate(OutError);
	}

	bool TryFinalizeCaptureMediaImagePolicyExecution(
		const FUeAgentCaptureMediaLongJobTypedRequest& Request,
		const FUeAgentPreparedCaptureMediaImageExecution& Prepared,
		const TSharedPtr<FJsonObject>& MechanicsResult,
		FUeAgentCaptureMediaImageExecutionResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutResult = {};
		OutError.Reset();
		const FPolicyRoute* const Route = FindRoute(Request.GetPolicyId());
		if (!Route)
		{
			OutError = Input(TEXT("capture_image_policy_unsupported"), TEXT("No image capture policy is registered for the requested policy ID."),
				TEXT("capture_finalize"), TEXT("/policy_id"), MakeShared<FJsonValueString>(Request.GetPolicyId()),
				MakeShared<FJsonValueString>(TEXT("registered image capture policy ID")));
			return false;
		}
		if (!Route->Descriptor.TryValidate(OutError)) return false;
		if (!Prepared.TryValidate(OutError)) return false;
		OutResult.RequestedFinalPath = Prepared.RequestedFinalPath;
		OutResult.Format = Prepared.Format;
		if (Route->Descriptor.GetPolicyId()
			== TEXT("policy:image-capture:transform-candidates/v1"))
		{
			const TCHAR* Fields[] = { TEXT("restored"), TEXT("dirty_state_restored"), TEXT("selection_restored"), TEXT("viewport_state_restored") };
			bool Values[UE_ARRAY_COUNT(Fields)] = {};
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(Fields); ++Index)
			{
				if (!MechanicsResult.IsValid() || !MechanicsResult->TryGetBoolField(Fields[Index], Values[Index]))
				{
					OutError = Native(TEXT("capture_transform_candidates_restore_unverified"),
						TEXT("Transform candidate capture did not report a boolean restoration result."), TEXT("capture_finalize"), Fields[Index],
						MechanicsResult.IsValid() ? MechanicsResult->TryGetField(Fields[Index]) : nullptr, MakeShared<FJsonValueString>(TEXT("boolean")));
					return false;
				}
			}
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(Fields); ++Index)
			{
				if (!Values[Index])
				{
					OutError = Native(TEXT("capture_transform_candidates_restore_unverified"),
						TEXT("Transform candidate capture reported that this preview state was not restored."), TEXT("capture_finalize"), Fields[Index],
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
			}
		}
		if (!TryResolveProducedPath(
				MechanicsResult, Prepared.Context.Params, OutResult.ProducedPath, OutError))
		{
			return false;
		}
		OutResult.ProducedPath =
			FPaths::ConvertRelativePathToFull(OutResult.ProducedPath);
		const int64 ProducedBytes = IFileManager::Get().FileSize(*OutResult.ProducedPath);
		if (ProducedBytes <= 0)
		{
			OutError = Native(TEXT("capture_image_output_empty"), TEXT("The captured output file is empty or its size could not be read."),
				TEXT("capture_finalize"), TEXT("bytes"), Number(ProducedBytes), MakeShared<FJsonValueString>(TEXT("greater than zero")));
			OutError.TargetPath = Prepared.RequestedFinalPath;
			OutError.InternalContext = OutResult.ProducedPath;
			return false;
		}
		if (!TryBuildCaptureReceipt(
				*Route, Request, MechanicsResult, OutResult, OutError)
			|| !OutResult.TryValidate(OutError))
		{
			IFileManager::Get().Delete(
				*OutResult.ProducedPath, false, true, true);
			for (const FString& Path : OutResult.AdditionalProducedPaths)
			{
				IFileManager::Get().Delete(*Path, false, true, true);
			}
			return false;
		}
		return true;
	}

	bool TryExecuteCaptureMediaImagePolicy(
		const FUeAgentCaptureMediaLongJobTypedRequest& Request,
		const FString& PreferredOutputPath,
		FUeAgentCaptureMediaImageExecutionResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const FPolicyRoute* const Route = FindRoute(Request.GetPolicyId());
		if (!Route)
		{
			OutError = Input(TEXT("capture_image_policy_unsupported"), TEXT("No image capture policy is registered for the requested policy ID."),
				TEXT("capture_execute"), TEXT("/policy_id"), MakeShared<FJsonValueString>(Request.GetPolicyId()),
				MakeShared<FJsonValueString>(TEXT("registered image capture policy ID")));
			return false;
		}
		if (!Route->Descriptor.TryValidate(OutError)) return false;
		if (IsViewportSequenceCapturePolicy(Request.GetPolicyId()))
		{
			OutError = Native(TEXT("capture_image_policy_requires_engine_frame_session"),
				TEXT("This image policy must execute in an engine-frame capture session."), TEXT("capture_execute"), TEXT("execution_mode"),
				MakeShared<FJsonValueString>(TEXT("direct")), MakeShared<FJsonValueString>(TEXT("engine_frame_session")));
			OutError.CandidateName = Request.GetPolicyId();
			return false;
		}
		if (!Route->ArtifactMechanics
			&& !Route->DirectMechanics
			&& !Route->ContextMechanics)
		{
			OutError = Native(TEXT("capture_image_policy_unsupported"),
				TEXT("The registered image capture policy has no execution callback."), TEXT("capture_execute"), TEXT("execution_callback_available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.CandidateName = Request.GetPolicyId();
			OutError.SubjectId = Route->Descriptor.GetMechanicsId();
			return false;
		}
		FUeAgentPreparedCaptureMediaImageExecution Prepared;
		if (!TryPrepareCaptureMediaImagePolicyExecution(
				Request, PreferredOutputPath, Prepared, OutError))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Result;
		bool bExecuted = false;
		if (Route->ContextMechanics)
		{
			bExecuted = Route->ContextMechanics(
				Prepared.Context, Result, OutError);
		}
		else if (Route->DirectMechanics)
		{
			bExecuted = Route->DirectMechanics(
				Prepared.Context.Params,
				Route->AssetNotFoundError,
				Result,
				OutError);
		}
		else
		{
			TArray<TSharedPtr<FJsonValue>> LegacyArtifacts;
			bExecuted = Route->ArtifactMechanics(
				Prepared.Context, Result, LegacyArtifacts, OutError);
		}
		if (!bExecuted)
		{
			Prepared.LocateDiagnosticSources(OutError);
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Route->Descriptor.GetMechanicsId();
			return false;
		}
		return TryFinalizeCaptureMediaImagePolicyExecution(
			Request, Prepared, Result, OutResult, OutError);
	}
}
