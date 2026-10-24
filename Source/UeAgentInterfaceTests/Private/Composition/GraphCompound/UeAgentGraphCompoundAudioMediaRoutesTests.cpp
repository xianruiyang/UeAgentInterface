#include "Misc/AutomationTest.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaRoutes.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	using namespace UE::UAI::Composition::GraphCompound;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker MakeTracker()
	{
		return FUeAgentResourceTracker();
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
			TEXT("gc017:persisted"), TEXT("generation:gc017"), 17);
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
					TEXT("gc017:string-default"),
					TEXT("fixture:gc017"),
					MakeString(TEXT(""), Resources).ToSharedRef()),
				FUeAgentStateDetails(
					TEXT("Audio/media graph element"), TEXT(""), {TEXT("GC-017")},
					true, TEXT("available"), {}),
				Revision,
				EUeAgentAuthoredPresence::Present,
				EUeAgentOverrideState::Overridden);
		}
		FUeAgentCanonicalSnapshotPtr Snapshot;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalSnapshot::TryCreate(
			TEXT("snapshot:gc017"),
			EUeAgentStatePlane::Persisted,
			Revision,
			FUeAgentTemporalPoint(TEXT("fixture"), TEXT("17")),
			FUeAgentStateProvenance(
				TEXT("independent_readback"),
				TEXT("fixture:gc017"),
				TEXT("evidence:gc017:persisted")),
			Entries,
			Resources,
			Snapshot,
			Error));
		return Snapshot.ToSharedRef();
	}

	const TArray<FString>& ExpectedCapabilities()
	{
		static const TArray<FString> Values = {
			TEXT("audio_media.asset.create"),
			TEXT("metasound.graph.interface.mutate"),
			TEXT("metasound.graph.node.mutate"),
			TEXT("metasound.graph.link.mutate"),
			TEXT("metasound.graph.default.mutate"),
			TEXT("metasound.graph.compile_settle"),
			TEXT("sound_cue.graph.node.mutate"),
			TEXT("sound_cue.graph.relation.mutate"),
			TEXT("sound_cue.graph.root.mutate"),
			TEXT("sound_cue.graph.compile_settle"),
			TEXT("texture_graph.graph.node.mutate"),
			TEXT("texture_graph.graph.link.mutate"),
			TEXT("texture_graph.graph.parameter.mutate"),
			TEXT("texture_graph.graph.validate_evaluate_settle"),
			TEXT("texture_graph.artifact.capture"),
		};
		return Values;
	}

	const TMap<FString, FString>& RouteExamples()
	{
		static const TMap<FString, FString> Values = {
			{TEXT("audio_media.asset.create"), TEXT("{\"asset_kind\":\"metasound_source\",\"asset_name\":\"MS_GC017\",\"package_path\":\"/Game/UAI\"}")},
			{TEXT("metasound.graph.interface.mutate"), TEXT("{\"changes\":[{\"access_type\":\"value\",\"data_type_ref\":\"Float\",\"default_value\":0.5,\"intent\":\"create\",\"interface_kind\":\"input\",\"name\":\"Gain\"}],\"target_ref\":\"/Game/UAI/MS_GC017\"}")},
			{TEXT("metasound.graph.node.mutate"), TEXT("{\"nodes\":[{\"class_name\":\"UE.Sine.Audio\",\"intent\":\"create\",\"major_version\":1,\"minor_version\":1,\"node_guid\":\"11111111-1111-1111-1111-111111111111\"}],\"target_ref\":\"/Game/UAI/MS_GC017\"}")},
			{TEXT("metasound.graph.link.mutate"), TEXT("{\"links\":[{\"from\":{\"data_type_ref\":\"Float\",\"direction\":\"output\",\"node_guid\":\"11111111-1111-1111-1111-111111111111\",\"vertex_guid\":\"22222222-2222-2222-2222-222222222222\"},\"intent\":\"create\",\"to\":{\"data_type_ref\":\"Float\",\"direction\":\"input\",\"node_guid\":\"33333333-3333-3333-3333-333333333333\",\"vertex_guid\":\"44444444-4444-4444-4444-444444444444\"}}],\"target_ref\":\"/Game/UAI/MS_GC017\"}")},
			{TEXT("metasound.graph.default.mutate"), TEXT("{\"data_type_ref\":\"Float\",\"intent\":\"update\",\"node_guid\":\"11111111-1111-1111-1111-111111111111\",\"target_ref\":\"/Game/UAI/MS_GC017\",\"value\":0.25,\"vertex_guid\":\"22222222-2222-2222-2222-222222222222\"}")},
			{TEXT("metasound.graph.compile_settle"), TEXT("{\"registration_policy\":\"register_and_compile\",\"target_ref\":\"/Game/UAI/MS_GC017\"}")},
			{TEXT("sound_cue.graph.node.mutate"), TEXT("{\"nodes\":[{\"intent\":\"create\",\"native_class_ref\":\"/Script/Engine.SoundNodeMixer\",\"object_path\":\"/Game/UAI/SC_GC017.SC_GC017:SoundNodeMixer_0\"}],\"target_ref\":\"/Game/UAI/SC_GC017\"}")},
			{TEXT("sound_cue.graph.relation.mutate"), TEXT("{\"relations\":[{\"child_object_path\":\"/Game/UAI/SC_GC017.SC_GC017:SoundNodeWavePlayer_0\",\"intent\":\"create\",\"order_index\":0,\"parent_object_path\":\"/Game/UAI/SC_GC017.SC_GC017:SoundNodeMixer_0\"}],\"target_ref\":\"/Game/UAI/SC_GC017\"}")},
			{TEXT("sound_cue.graph.root.mutate"), TEXT("{\"intent\":\"update\",\"root_node_object_path\":\"/Game/UAI/SC_GC017.SC_GC017:SoundNodeMixer_0\",\"target_ref\":\"/Game/UAI/SC_GC017\"}")},
			{TEXT("sound_cue.graph.compile_settle"), TEXT("{\"sync_policy\":\"sync_and_compile\",\"target_ref\":\"/Game/UAI/SC_GC017\"}")},
			{TEXT("texture_graph.graph.node.mutate"), TEXT("{\"nodes\":[{\"element_id\":\"scalar_gain\",\"expression_class_ref\":\"/Script/TextureGraph.TG_Expression_Scalar\",\"intent\":\"create\"}],\"target_ref\":\"/Game/UAI/TG_GC017\"}")},
			{TEXT("texture_graph.graph.link.mutate"), TEXT("{\"links\":[{\"from\":{\"direction\":\"output\",\"element_id\":\"scalar_gain\",\"pin_name\":\"ValueOut\"},\"intent\":\"create\",\"to\":{\"direction\":\"input\",\"element_id\":\"builtin_output\",\"pin_name\":\"Source\"}}],\"target_ref\":\"/Game/UAI/TG_GC017\"}")},
			{TEXT("texture_graph.graph.parameter.mutate"), TEXT("{\"parameters\":[{\"alias\":\"Scale\",\"element_id\":\"scalar_gain\",\"intent\":\"update\",\"pin_name\":\"Scalar\",\"value\":0.5}],\"target_ref\":\"/Game/UAI/TG_GC017\"}")},
			{TEXT("texture_graph.graph.validate_evaluate_settle"), TEXT("{\"evaluation_policy\":\"validate_and_evaluate\",\"target_ref\":\"/Game/UAI/TG_GC017\"}")},
			{TEXT("texture_graph.artifact.capture"), TEXT("{\"outputs\":[{\"element_id\":\"builtin_output\",\"output_name\":\"Output\"}],\"target_ref\":\"/Game/UAI/TG_GC017\"}")},
		};
		return Values;
	}

	TArray<FUeAgentCanonicalIdentity> ExistingFor(
		const FGraphCompoundCanonicalPayload& Payload)
	{
		TArray<FUeAgentCanonicalIdentity> Existing;
		TSet<FString> Keys;
		TSet<FString> CreatedKeys;
		for (const FGraphCompoundCanonicalEdit& Edit : Payload.GetEdits())
		{
			if (Edit.GetIntent() == EChangeIntent::Create)
			{
				CreatedKeys.Add(Edit.GetIdentity().GetCanonicalKey());
			}
		}
		for (const FGraphCompoundCanonicalEdit& Edit : Payload.GetEdits())
		{
			if (Edit.GetIntent() != EChangeIntent::Create
				&& !Keys.Contains(Edit.GetIdentity().GetCanonicalKey()))
			{
				Existing.Add(Edit.GetIdentity());
				Keys.Add(Edit.GetIdentity().GetCanonicalKey());
			}
			for (const FUeAgentCanonicalIdentity& Dependency : Edit.GetDependencies())
			{
				if (!CreatedKeys.Contains(Dependency.GetCanonicalKey())
					&& !Keys.Contains(Dependency.GetCanonicalKey()))
				{
					Existing.Add(Dependency);
					Keys.Add(Dependency.GetCanonicalKey());
				}
			}
		}
		return Existing;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundAudioMediaExactContractTest,
	"UeAgentInterface.Composition.GraphCompound.AudioMedia.ExactContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundAudioMediaExactContractTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const TArray<FAudioMediaGraphRouteProfile>* ProfileArray = nullptr;
	UE::UAI::Contract::FDiagnostic ProfileError;
	if (!TryGetAudioMediaGraphRouteProfiles(ProfileArray, ProfileError))
	{
		AddError(ProfileError.PublicMessage);
		return false;
	}
	const TArray<FAudioMediaGraphRouteProfile>& Profiles = *ProfileArray;
	TSet<FString> Capabilities;
	TSet<FString> DescriptorIds;
	const TSet<FString> TextureGraphCapabilities = {
		TEXT("texture_graph.graph.node.mutate"),
		TEXT("texture_graph.graph.link.mutate"),
		TEXT("texture_graph.graph.parameter.mutate"),
		TEXT("texture_graph.graph.validate_evaluate_settle"),
		TEXT("texture_graph.artifact.capture"),
	};
	for (const FAudioMediaGraphRouteProfile& Profile : Profiles)
	{
		Capabilities.Add(Profile.GetCapabilityId());
		DescriptorIds.Add(Profile.GetDescriptorId());
		TestTrue(*FString::Printf(TEXT("%s profile is valid"),
			*Profile.GetCapabilityId()), Profile.IsValid());
		const bool bArtifactQuery = Profile.GetCapabilityId()
			== TEXT("texture_graph.artifact.capture");
		if (bArtifactQuery)
		{
			TestEqual(TEXT("artifact state capture enters C3Q"),
				Profile.GetRouteKind(), EGraphCompoundDomainRouteKind::Query);
			TestEqual(TEXT("artifact state capture uses the query lane"),
				Profile.GetExecutionContract().GetLane(),
				EGraphCompoundExecutionLane::Query);
			TestFalse(TEXT("artifact state capture owns no mutation"),
				Profile.GetExecutionContract().RequiresNativeMutation());
			TestFalse(TEXT("artifact state capture is not mutation-ordered"),
				Profile.GetExecutionContract().ReadsBackAfterMutation());
		}
		else
		{
			TestEqual(TEXT("mutation route enters the typed operation lane"),
				Profile.GetRouteKind(),
				EGraphCompoundDomainRouteKind::TypedOperation);
			TestEqual(TEXT("mutation route enters C3O"),
				Profile.GetExecutionContract().GetLane(),
				EGraphCompoundExecutionLane::Operation);
			TestTrue(TEXT("mutation route owns one native mutation"),
				Profile.GetExecutionContract().RequiresNativeMutation());
			TestTrue(TEXT("mutation route reads back after mutation"),
				Profile.GetExecutionContract().ReadsBackAfterMutation());
		}
		const bool bTextureGraph = TextureGraphCapabilities.Contains(
			Profile.GetCapabilityId());
		TestEqual(TEXT("TextureGraph availability policy is exact"),
			Profile.RequiresNativeBackendAvailability(), bTextureGraph);
		if (bTextureGraph)
		{
			TestEqual(TEXT("TextureGraph unavailable error is stable"),
				Profile.GetUnavailableErrorCode(),
				FString(TEXT("audio_media_graph_texture_graph_unavailable")));
		}
	}
	TestEqual(TEXT("capabilities are unique"),
		Capabilities.Num(), Profiles.Num());
	TestEqual(TEXT("descriptor ids are unique"),
		DescriptorIds.Num(), Profiles.Num());
	for (const FString& Capability : ExpectedCapabilities())
	{
		TestTrue(*FString::Printf(TEXT("%s is present"), *Capability),
			Capabilities.Contains(Capability));
	}

	const FAudioMediaGraphRouteProfile* Artifact =
		FindAudioMediaGraphRouteProfile(TEXT("texture_graph.artifact.capture"));
	TestNotNull(TEXT("artifact route exists"), Artifact);
	if (Artifact)
	{
		TestEqual(TEXT("artifact state belongs to the exact query route"),
			Artifact->GetRouteKind(), EGraphCompoundDomainRouteKind::Query);
		TestEqual(TEXT("artifact query has an independent readback provider"),
			Artifact->GetExecutionContract().GetReadbackId(),
			FString(TEXT("audio_media.texture_graph.artifact_state.readback")));
	}

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	UE::UAI::Contract::FDiagnostic Error;
	TestTrue(TEXT("exact registry compiles"),
		TryCreateAudioMediaGraphRouteDescriptorRegistry(Registry, Error));
	if (Registry.IsValid())
	{
		TSet<FString> Hashes;
		for (const FUeAgentOperationDescriptor& Descriptor
			: Registry->GetDescriptors())
		{
			Hashes.Add(Descriptor.GetIdentity().GetDescriptorHash());
			TestTrue(TEXT("schema closes additional properties"),
				Descriptor.GetInputSchema().GetJsonValue().Contains(
					TEXT("\"additionalProperties\":false")));
			const FUeAgentContractSchema* Schema =
				Registry->FindCompiledInputSchemaExact(Descriptor.GetIdentity());
			TestNotNull(TEXT("compiled schema exists"), Schema);
			if (Schema)
			{
				TOptional<FUeAgentContractValue> Canonical;
				FUeAgentContractSchemaError SchemaError;
				TestFalse(TEXT("unknown fields fail closed"),
					Schema->TryValidateAndCanonicalize(
						FUeAgentContractValue(TEXT("{\"unexpected\":true}")),
						[](int64, const FString&, FUeAgentContractSchemaError&)
						{
							return true;
						},
						Canonical, SchemaError));
			}
			if (Descriptor.GetCapabilityId()
				== TEXT("texture_graph.artifact.capture"))
			{
				TestTrue(TEXT("artifact result owns output state"),
					Descriptor.GetResultSchema().GetJsonValue().Contains(
						TEXT("\"outputs\"")));
				TestTrue(TEXT("artifact result reports non-publishing state"),
					Descriptor.GetResultSchema().GetJsonValue().Contains(
						TEXT("\"published\"")));
			}
			else if (Descriptor.GetCapabilityId()
				== TEXT("metasound.graph.compile_settle"))
			{
				TestFalse(TEXT("MetaSound settle does not expose document revision custody"),
					Descriptor.GetInputSchema().GetJsonValue().Contains(
						TEXT("expected_document_revision")));
			}
			else if (Descriptor.GetCapabilityId()
				== TEXT("sound_cue.graph.compile_settle"))
			{
				TestFalse(TEXT("SoundCue settle does not expose graph revision custody"),
					Descriptor.GetInputSchema().GetJsonValue().Contains(
						TEXT("expected_graph_revision")));
			}
			else
			{
				const FUeAgentContractSchema* ResultSchema =
					Registry->FindCompiledResultSchemaExact(
						Descriptor.GetIdentity());
				TestNotNull(TEXT("mutation result schema compiles"), ResultSchema);
				if (ResultSchema)
				{
					TOptional<FUeAgentContractValue> Canonical;
					FUeAgentContractSchemaError SchemaError;
					const bool bGraphRevisionIsPrivate =
						Descriptor.GetCapabilityId().StartsWith(TEXT("metasound.graph."))
						|| Descriptor.GetCapabilityId().StartsWith(TEXT("sound_cue.graph."));
					const FString ResultExample = bGraphRevisionIsPrivate
						? FString(TEXT(
							"{\"compile_settled\":true,\"mutation_applied\":true,"
							"\"reloaded\":true,\"saved\":true,"
							"\"target_ref\":\"/Game/T.T\","
							"\"terminal_status\":\"succeeded\"}"))
						: FString(TEXT(
							"{\"compile_settled\":true,\"mutation_applied\":true,"
							"\"reloaded\":true,\"saved\":true,"
							"\"target_ref\":\"/Game/T.T\","
							"\"terminal_status\":\"succeeded\"}"));
					TestTrue(TEXT("audio mutation result requires a real lifecycle receipt"),
						ResultSchema->TryValidateAndCanonicalize(
							FUeAgentContractValue(ResultExample),
							[](int64, const FString&,
								FUeAgentContractSchemaError&)
							{
								return true;
							},
							Canonical,
							SchemaError));
				}
			}
		}
		TestEqual(TEXT("descriptor hashes are exact and unique"),
			Hashes.Num(), Registry->GetDescriptors().Num());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundAudioMediaSpecializedMappingTest,
	"UeAgentInterface.Composition.GraphCompound.AudioMedia.SpecializedMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundAudioMediaSpecializedMappingTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	for (const FString& Capability : {
		TEXT("metasound.graph.link.mutate"),
		TEXT("sound_cue.graph.relation.mutate"),
		TEXT("texture_graph.graph.link.mutate")})
	{
		FUeAgentResourceTracker Resources = MakeTracker();
		const FUeAgentCanonicalSnapshotRef Empty = MakeSnapshot(Resources, {});
		const FAudioMediaGraphRouteProfile* Profile =
			FindAudioMediaGraphRouteProfile(Capability);
		TestNotNull(*FString::Printf(TEXT("%s exists"), *Capability), Profile);
		if (!Profile)
		{
			continue;
		}
		FGraphCompoundCanonicalPayload Payload(
			GetGraphCompoundCanonicalSchemaId(), {});
		FGraphCompoundRouteError Error;
		TestTrue(*FString::Printf(TEXT("%s decodes specialized fields"),
			*Capability), TryDecodeAudioMediaGraphTypedOperation(
				*Profile,
				FUeAgentContractValue(RouteExamples().FindChecked(Capability)),
				Empty, Resources, Payload, Error));
		TestFalse(TEXT("specialized mapping produces identity edits"),
			Payload.GetEdits().IsEmpty());
		for (const FGraphCompoundCanonicalEdit& Edit : Payload.GetEdits())
		{
			TestFalse(TEXT("identity is stable and non-empty"),
				Edit.GetIdentity().GetSemanticId().IsEmpty());
		}
	}

	const TMap<FString, FString> StableIdentities = {
		{TEXT("metasound.graph.interface.mutate"),
			TEXT("input:metasound_interface:input:Gain")},
		{TEXT("sound_cue.graph.relation.mutate"),
			TEXT("relation:sound_child:/Game/UAI/SC_GC017.SC_GC017:SoundNodeMixer_0->/Game/UAI/SC_GC017.SC_GC017:SoundNodeWavePlayer_0")},
		{TEXT("sound_cue.graph.root.mutate"),
			TEXT("binding:sound_root")},
		{TEXT("metasound.graph.compile_settle"),
			TEXT("dependency:metasound_compile_settle")},
		{TEXT("sound_cue.graph.compile_settle"),
			TEXT("dependency:sound_compile_settle")},
		{TEXT("texture_graph.graph.validate_evaluate_settle"),
			TEXT("dependency:texture_graph_settle")},
		{TEXT("texture_graph.graph.node.mutate"),
			TEXT("item:texture_expression:scalar_gain")},
		{TEXT("texture_graph.graph.parameter.mutate"),
			TEXT("input:texture_parameter:scalar_gain:Scalar")},
	};
	for (const TPair<FString, FString>& Case : StableIdentities)
	{
		FUeAgentResourceTracker IdentityResources = MakeTracker();
		const FUeAgentCanonicalSnapshotRef Empty =
			MakeSnapshot(IdentityResources, {});
		const FAudioMediaGraphRouteProfile* Profile =
			FindAudioMediaGraphRouteProfile(Case.Key);
		TestNotNull(TEXT("stable-identity profile exists"), Profile);
		if (!Profile)
		{
			continue;
		}
		FGraphCompoundCanonicalPayload Payload(
			GetGraphCompoundCanonicalSchemaId(), {});
		FGraphCompoundRouteError Error;
		TestTrue(TEXT("stable-identity route decodes"),
			TryDecodeAudioMediaGraphTypedOperation(
				*Profile,
				FUeAgentContractValue(RouteExamples().FindChecked(Case.Key)),
				Empty, IdentityResources, Payload, Error));
		TestEqual(TEXT("mutable values are excluded from semantic identity"),
			Payload.GetEdits()[0].GetIdentity().GetSemanticId(), Case.Value);
	}

	FUeAgentResourceTracker Resources = MakeTracker();
	const FAudioMediaGraphRouteProfile* Artifact =
		FindAudioMediaGraphRouteProfile(TEXT("texture_graph.artifact.capture"));
	TestNotNull(TEXT("artifact query exists"), Artifact);
	if (Artifact)
	{
		FAudioMediaGraphArtifactQueryRequest Request;
		FGraphCompoundRouteError Error;
		TestTrue(TEXT("artifact query decodes exact output selectors"),
			TryDecodeAudioMediaGraphArtifactQueryRequest(
				*Artifact,
				FUeAgentContractValue(RouteExamples().FindChecked(
					TEXT("texture_graph.artifact.capture"))),
				Resources, Request, Error));
		TestTrue(TEXT("artifact query request is valid"), Request.IsValid());
		TestEqual(TEXT("artifact query keeps one selector"),
			Request.GetOutputs().Num(), 1);
		if (!Request.GetOutputs().IsEmpty())
		{
			TestEqual(TEXT("artifact selector keeps semantic output name"),
				Request.GetOutputs()[0].GetOutputName(),
				FString(TEXT("Output")));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundAudioMediaAllRoutesTest,
	"UeAgentInterface.Composition.GraphCompound.AudioMedia.AllRoutes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundAudioMediaAllRoutesTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const TArray<FAudioMediaGraphRouteProfile>* ProfileArray = nullptr;
	UE::UAI::Contract::FDiagnostic ProfileError;
	if (!TryGetAudioMediaGraphRouteProfiles(ProfileArray, ProfileError))
	{
		AddError(ProfileError.PublicMessage);
		return false;
	}
	for (const FAudioMediaGraphRouteProfile& Profile : *ProfileArray)
	{
		TestTrue(TEXT("every route has an owned typed example"),
			RouteExamples().Contains(Profile.GetCapabilityId()));
	}
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	UE::UAI::Contract::FDiagnostic RegistryError;
	TestTrue(TEXT("registry builds"),
		TryCreateAudioMediaGraphRouteDescriptorRegistry(Registry, RegistryError));
	if (!Registry.IsValid())
	{
		return false;
	}

	for (const TPair<FString, FString>& Case : RouteExamples())
	{
		FUeAgentResourceTracker Resources = MakeTracker();
		const FUeAgentCanonicalSnapshotRef Empty = MakeSnapshot(Resources, {});
		const FAudioMediaGraphRouteProfile* Profile =
			FindAudioMediaGraphRouteProfile(Case.Key);
		TestNotNull(*FString::Printf(TEXT("%s profile exists"), *Case.Key), Profile);
		if (!Profile)
		{
			continue;
		}
		if (Profile->GetRouteKind() == EGraphCompoundDomainRouteKind::Query)
		{
			FAudioMediaGraphArtifactQueryRequest Request;
			FGraphCompoundRouteError Error;
			TestTrue(TEXT("artifact route decodes as an exact C3Q request"),
				TryDecodeAudioMediaGraphArtifactQueryRequest(
					*Profile, FUeAgentContractValue(Case.Value),
					Resources, Request, Error));
			TestTrue(TEXT("artifact query request is present"), Request.IsValid());
			continue;
		}
		FGraphCompoundCanonicalPayload Payload(
			GetGraphCompoundCanonicalSchemaId(), {});
		FGraphCompoundRouteError Error;
		TestTrue(*FString::Printf(TEXT("%s decodes"), *Case.Key),
			TryDecodeAudioMediaGraphTypedOperation(
				*Profile, FUeAgentContractValue(Case.Value), Empty,
				Resources, Payload, Error));
		if (Payload.GetEdits().IsEmpty())
		{
			AddError(FString::Printf(TEXT("%s produced no identity edits"),
				*Case.Key));
			continue;
		}
		const FUeAgentCanonicalSnapshotRef Before =
			MakeSnapshot(Resources, ExistingFor(Payload));
		const FUeAgentOperationDescriptor* Descriptor =
			Registry->FindByDescriptorId(Profile->GetDescriptorId());
		TestNotNull(TEXT("descriptor exists"), Descriptor);
		if (!Descriptor)
		{
			continue;
		}
		TOptional<FChangeSetDraft> Draft;
		TestTrue(*FString::Printf(TEXT("%s builds a C6 draft"), *Case.Key),
			TryBuildAudioMediaGraphTypedOperationDraft(
				*Profile, Descriptor->GetIdentity(),
				FUeAgentContractValue(Case.Value), Before,
				TEXT("request:gc017:all"),
				TEXT("idempotency:gc017:all"),
				Resources, Draft, Error));
		TestTrue(TEXT("draft is present"), Draft.IsSet());
		if (Draft.IsSet())
		{
			TestEqual(TEXT("per-identity cardinality is preserved"),
				Draft->GetChanges().Num(), Payload.GetEdits().Num());
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundAudioMediaDeleteSelectorsTest,
	"UeAgentInterface.Composition.GraphCompound.AudioMedia.DeleteSelectors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundAudioMediaDeleteSelectorsTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const TMap<FString, FString> Cases = {
		{TEXT("metasound.graph.interface.mutate"),
			TEXT("{\"changes\":[{\"access_type\":\"value\",\"data_type_ref\":\"Float\",\"default_value\":0.5,\"intent\":\"delete\",\"interface_kind\":\"input\",\"name\":\"Gain\"}],\"target_ref\":\"/Game/UAI/MS_Delete\"}")},
		{TEXT("metasound.graph.node.mutate"),
			TEXT("{\"nodes\":[{\"class_name\":\"UE.Sine.Audio\",\"intent\":\"delete\",\"major_version\":1,\"minor_version\":1,\"node_guid\":\"11111111-1111-1111-1111-111111111111\"}],\"target_ref\":\"/Game/UAI/MS_Delete\"}")},
		{TEXT("metasound.graph.link.mutate"),
			TEXT("{\"links\":[{\"from\":{\"data_type_ref\":\"Float\",\"direction\":\"output\",\"node_guid\":\"11111111-1111-1111-1111-111111111111\",\"vertex_guid\":\"22222222-2222-2222-2222-222222222222\"},\"intent\":\"delete\",\"to\":{\"data_type_ref\":\"Float\",\"direction\":\"input\",\"node_guid\":\"33333333-3333-3333-3333-333333333333\",\"vertex_guid\":\"44444444-4444-4444-4444-444444444444\"}}],\"target_ref\":\"/Game/UAI/MS_Delete\"}")},
		{TEXT("sound_cue.graph.node.mutate"),
			TEXT("{\"nodes\":[{\"intent\":\"delete\",\"native_class_ref\":\"/Script/Engine.SoundNodeMixer\",\"object_path\":\"/Game/UAI/SC_Delete.SC_Delete:SoundNodeMixer_0\"}],\"target_ref\":\"/Game/UAI/SC_Delete\"}")},
		{TEXT("sound_cue.graph.relation.mutate"),
			TEXT("{\"relations\":[{\"child_object_path\":\"/Game/UAI/SC_Delete.SC_Delete:Child\",\"intent\":\"delete\",\"order_index\":0,\"parent_object_path\":\"/Game/UAI/SC_Delete.SC_Delete:Parent\"}],\"target_ref\":\"/Game/UAI/SC_Delete\"}")},
		{TEXT("texture_graph.graph.node.mutate"),
			TEXT("{\"nodes\":[{\"element_id\":\"delete_me\",\"expression_class_ref\":\"/Script/TextureGraph.TG_Expression_Scalar\",\"intent\":\"delete\"}],\"target_ref\":\"/Game/UAI/TG_Delete\"}")},
		{TEXT("texture_graph.graph.link.mutate"),
			TEXT("{\"links\":[{\"from\":{\"direction\":\"output\",\"element_id\":\"delete_me\",\"pin_name\":\"ValueOut\"},\"intent\":\"delete\",\"to\":{\"direction\":\"input\",\"element_id\":\"builtin_output\",\"pin_name\":\"Source\"}}],\"target_ref\":\"/Game/UAI/TG_Delete\"}")},
	};

	for (const TPair<FString, FString>& Case : Cases)
	{
		FUeAgentResourceTracker Resources = MakeTracker();
		const FUeAgentCanonicalSnapshotRef Empty = MakeSnapshot(Resources, {});
		const FAudioMediaGraphRouteProfile* Profile =
			FindAudioMediaGraphRouteProfile(Case.Key);
		TestNotNull(TEXT("delete route exists: ") + Case.Key, Profile);
		if (!Profile)
		{
			continue;
		}
		FGraphCompoundCanonicalPayload Payload(
			GetGraphCompoundCanonicalSchemaId(), {});
		FGraphCompoundRouteError Error;
		if (!TestTrue(
				TEXT("delete route normalizes: ") + Case.Key,
				TryDecodeAudioMediaGraphTypedOperation(
					*Profile,
					FUeAgentContractValue(Case.Value),
					Empty,
					Resources,
					Payload,
					Error)))
		{
			continue;
		}
		for (const FGraphCompoundCanonicalEdit& Edit : Payload.GetEdits())
		{
			TestEqual(
				TEXT("delete intent remains canonical"),
				Edit.GetIntent(),
				EChangeIntent::Delete);
			TestFalse(
				TEXT("delete carries no proposed-after state"),
				Edit.GetProposedAfter().IsValid());
			TestFalse(
				TEXT("delete identity owns typed native selectors"),
				Edit.GetIdentity().GetSelectors().IsEmpty());
			TestFalse(
				TEXT("delete identity does not fall back to one opaque selector"),
				Edit.GetIdentity().GetSelectors().Contains(TEXT("selector")));
		}
		const FUeAgentCanonicalSnapshotRef Before =
			MakeSnapshot(Resources, ExistingFor(Payload));
		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		UE::UAI::Contract::FDiagnostic RegistryError;
		TestTrue(
			TEXT("delete descriptor registry builds"),
			TryCreateAudioMediaGraphRouteDescriptorRegistry(
				Registry, RegistryError));
		const FUeAgentOperationDescriptor* Descriptor = Registry.IsValid()
			? Registry->FindByDescriptorId(Profile->GetDescriptorId())
			: nullptr;
		TestNotNull(TEXT("delete descriptor exists"), Descriptor);
		if (Descriptor)
		{
			TOptional<FChangeSetDraft> Draft;
			TestTrue(
				TEXT("delete selector identity builds one C6 draft"),
				TryBuildAudioMediaGraphTypedOperationDraft(
					*Profile,
					Descriptor->GetIdentity(),
					FUeAgentContractValue(Case.Value),
					Before,
					TEXT("request:gc017:delete"),
					TEXT("idempotency:gc017:delete"),
					Resources,
					Draft,
					Error));
			TestTrue(TEXT("delete draft exists"), Draft.IsSet());
		}
	}
	return !HasAnyErrors();
}
