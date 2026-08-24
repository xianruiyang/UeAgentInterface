#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialRequests.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialDescriptors.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialExecutionChains.h"

namespace
{
	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("UeAgentInterface/Source/UeAgentInterfaceTests/Private/"
				"DomainAdapters/WorldSpatial/Fixtures/"
				"world_spatial_typed_request_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load WorldSpatial typed request fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		Test->TestTrue(
			TEXT("parse WorldSpatial typed request fixture"),
			FJsonSerializer::Deserialize(Reader, Object)
				&& Object.IsValid());
		return Object;
	}

	FString JsonObjectText(const TSharedPtr<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&Text);
		if (!Object.IsValid()
			|| !FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
		{
			return FString();
		}
		return Text;
	}

	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::WorldSpatial;

	FUeAgentResourceTrackerOwner MakeResources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 4 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 4 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 100000},
				{EUeAgentResourceDimension::Items, 100000},
				{EUeAgentResourceDimension::StringBytes, 4 * 1024 * 1024},
				{EUeAgentResourceDimension::WorkUnits, 100000},
				{
					EUeAgentResourceDimension::EstimatedMemoryBytes,
					16 * 1024 * 1024,
				},
			}));
	}

	const FUeAgentWorldSpatialExecutionChainContract* FindContract(
		const FString& CapabilityId)
	{
		return GetWorldSpatialExecutionChainContracts().FindByPredicate(
			[&CapabilityId](
				const FUeAgentWorldSpatialExecutionChainContract& Contract)
			{
				return Contract.GetCapabilityId() == CapabilityId;
			});
	}

	bool PayloadMatchesKind(
		const FUeAgentWorldSpatialRequestPayload& Payload,
		const FString& Kind)
	{
		if (Kind == TEXT("graph_view_read"))
		{
			return Payload.IsType<FGraphViewReadRequestInput>();
		}
		if (Kind == TEXT("graph_view_update"))
		{
			return Payload.IsType<FGraphViewUpdateRequestInput>();
		}
		if (Kind == TEXT("graph_layout_apply"))
		{
			return Payload.IsType<FGraphLayoutApplyRequestInput>();
		}
		if (Kind == TEXT("object_transform_read"))
		{
			return Payload.IsType<FObjectTransformReadRequestInput>();
		}
		if (Kind == TEXT("object_transform_update"))
		{
			return Payload.IsType<FWorldObjectTransformUpdateRequestInput>();
		}
		if (Kind == TEXT("object_align"))
		{
			return Payload.IsType<FWorldObjectAlignRequestInput>();
		}
		if (Kind == TEXT("object_snap_to_surface"))
		{
			return Payload.IsType<FWorldObjectSnapToSurfaceRequestInput>();
		}
		if (Kind == TEXT("mesh_transform_bake"))
		{
			return Payload.IsType<FMeshTransformBakeRequestInput>();
		}
		if (Kind == TEXT("mesh_pivot_update"))
		{
			return Payload.IsType<FMeshPivotUpdateRequestInput>();
		}
		if (Kind == TEXT("viewport_ray_deproject"))
		{
			return Payload.IsType<FViewportRayDeprojectRequestInput>();
		}
		if (Kind == TEXT("viewport_ray_trace"))
		{
			return Payload.IsType<FViewportRayTraceRequestInput>();
		}
		if (Kind == TEXT("viewport_actor_pick"))
		{
			return Payload.IsType<FViewportActorPickRequestInput>();
		}
		if (Kind == TEXT("viewport_actor_select"))
		{
			return Payload.IsType<FViewportActorSelectRequestInput>();
		}
		if (Kind == TEXT("mesh_element_select_by_ray"))
		{
			return Payload.IsType<FMeshElementSelectByRayRequestInput>();
		}
		if (Kind == TEXT("viewport_pixel_scale_read"))
		{
			return Payload.IsType<FViewportPixelScaleReadRequestInput>();
		}
		if (Kind == TEXT("viewport_camera_frame"))
		{
			return Payload.IsType<FViewportCameraFrameRequestInput>();
		}
		if (Kind == TEXT("viewport_state_read"))
		{
			return Payload.IsType<FViewportStateReadRequestInput>();
		}
		if (Kind == TEXT("viewport_state_restore"))
		{
			return Payload.IsType<FViewportStateRestoreRequestInput>();
		}
		if (Kind == TEXT("viewport_warning_list"))
		{
			return Payload.IsType<FViewportWarningListRequestInput>();
		}
		if (Kind == TEXT("viewport_view_mode_list"))
		{
			return Payload.IsType<FViewportViewModeListRequestInput>();
		}
		if (Kind == TEXT("viewport_instance_list"))
		{
			return Payload.IsType<FViewportInstanceListRequestInput>();
		}
		if (Kind == TEXT("viewport_state_update"))
		{
			return Payload.IsType<FViewportStateUpdateRequestInput>();
		}
		return false;
	}

	void CheckRepresentativePayload(
		FAutomationTestBase* Test,
		const FString& Kind,
		const FUeAgentWorldSpatialRequestPayload& Payload)
	{
		if (Kind == TEXT("graph_view_update"))
		{
			const FGraphViewUpdateRequestInput& Input =
				Payload.Get<FGraphViewUpdateRequestInput>();
			Test->TestEqual(TEXT("graph view zoom decoded"), Input.ViewState.Zoom, 1.25);
			Test->TestEqual(
				TEXT("graph view subject decoded"),
				Input.ViewState.Location.Subject,
				FString(TEXT("graph:/Game/UAI/Tests/WorldSpatial/"
					"BP_GraphViewFixture.EventGraph")));
		}
		else if (Kind == TEXT("graph_layout_apply"))
		{
			const FGraphLayoutApplyRequestInput& Input =
				Payload.Get<FGraphLayoutApplyRequestInput>();
			Test->TestEqual(TEXT("two node placements decoded"), Input.NodePlacements.Num(), 2);
			Test->TestEqual(TEXT("one layout relation decoded"), Input.LayoutRelations.Num(), 1);
		}
		else if (Kind == TEXT("object_align"))
		{
			const FWorldObjectAlignRequestInput& Input =
				Payload.Get<FWorldObjectAlignRequestInput>();
			Test->TestTrue(TEXT("explicit anchor decoded"), Input.AnchorRef.IsSet());
			if (Input.AnchorRef.IsSet())
			{
				Test->TestEqual(TEXT("anchor identity decoded"), Input.AnchorRef.GetValue(), FString(TEXT("actor:anchor")));
			}
		}
		else if (Kind == TEXT("viewport_ray_trace"))
		{
			const FViewportRayTraceRequestInput& Input =
				Payload.Get<FViewportRayTraceRequestInput>();
			Test->TestEqual(TEXT("trace distance decoded"), Input.TraceQuery.MaxDistanceCm, 10000.0);
			Test->TestEqual(TEXT("screen point has two lexemes"), Input.ScreenPoint.Components.Num(), 2);
		}
		else if (Kind == TEXT("viewport_actor_pick"))
		{
			const FViewportActorPickRequestInput& Input =
				Payload.Get<FViewportActorPickRequestInput>();
			Test->TestTrue(TEXT("actor pick preserves allow-no-hit policy"),
				Input.bAllowNoHit);
		}
		else if (Kind == TEXT("viewport_actor_select"))
		{
			const FViewportActorSelectRequestInput& Input =
				Payload.Get<FViewportActorSelectRequestInput>();
			Test->TestTrue(TEXT("screen selector decoded"), Input.ScreenPoint.IsSet());
			Test->TestEqual(TEXT("screen selector has no direct actor identities"), Input.ActorRefs.Num(), 0);
		}
		else if (Kind == TEXT("viewport_camera_frame"))
		{
			const FViewportCameraFrameRequestInput& Input =
				Payload.Get<FViewportCameraFrameRequestInput>();
			Test->TestEqual(TEXT("explicit frame subject decoded"), Input.SubjectRefs.Num(), 1);
			Test->TestTrue(TEXT("collision-aware policy decoded"), Input.FramePolicy.bCollisionAware);
			Test->TestTrue(TEXT("look-at policy decoded"), Input.FramePolicy.bLookAt);
			Test->TestTrue(TEXT("automatic fallback policy decoded"), Input.FramePolicy.bAutoFallback);
		}
		else if (Kind == TEXT("viewport_state_restore"))
		{
			const FViewportStateRestoreRequestInput& Input =
				Payload.Get<FViewportStateRestoreRequestInput>();
			Test->TestEqual(TEXT("viewport identity decoded"), Input.Snapshot.ViewportRef, FString(TEXT("viewport:fixture")));
			Test->TestEqual(TEXT("view mode decoded"), Input.Snapshot.Camera.ViewMode, FString(TEXT("lit")));
			Test->TestTrue(TEXT("mode widgets decoded"), Input.Snapshot.Interaction.bModeWidgets);
			Test->TestEqual(TEXT("widget mode decoded"), Input.Snapshot.Interaction.WidgetMode, FString(TEXT("none")));
		}
		else if (Kind == TEXT("viewport_warning_list"))
		{
			const FViewportWarningListRequestInput& Input =
				Payload.Get<FViewportWarningListRequestInput>();
			Test->TestTrue(TEXT("suppressed warning policy decoded"), Input.bIncludeSuppressed);
		}
		else if (Kind == TEXT("viewport_state_update"))
		{
			const FViewportStateUpdateRequestInput& Input =
				Payload.Get<FViewportStateUpdateRequestInput>();
			Test->TestTrue(TEXT("interaction patch decoded"), Input.StatePatch.Interaction.IsSet());
			if (Input.StatePatch.Interaction.IsSet())
			{
				Test->TestEqual(TEXT("interaction widget mode patch decoded"),
					Input.StatePatch.Interaction->WidgetMode, FString(TEXT("none")));
			}
		}
	}

	TSharedPtr<const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot>
	MakeChangeSnapshot(
		FAutomationTestBase* Test,
		const FString& CapabilityId,
		FUeAgentResourceTracker& Resources)
	{
		using namespace UE::UAI::StateSemantic;
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		if (!Test->TestTrue(
				TEXT("create independent current state"),
				FUeAgentCanonicalValue::TryString(
					CapabilityId + TEXT(":before"),
					Resources,
					Value,
					Error)))
		{
			return nullptr;
		}
		const FUeAgentStateRevision Revision(
			TEXT("world-spatial-owned"),
			TEXT("generation-1"),
			1);
		const FUeAgentStateEntry Entry(
			FUeAgentCanonicalIdentity(
				TEXT("world-spatial:") + CapabilityId,
				TEXT("world_spatial_subject"),
				TEXT("owned-fixture"),
				CapabilityId,
				TEXT("state"),
				TEXT("generation-1")),
			FUeAgentCanonicalType(
				TEXT("core.string"),
				EUeAgentValueKind::String,
				false,
				TEXT("scalar")),
			Value.ToSharedRef(),
			FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::DomainNative,
				TEXT("world-spatial-native"),
				TEXT("owned-fixture-before"),
				Value.ToSharedRef()),
			FUeAgentStateDetails(
				TEXT("WorldSpatial current state"),
				TEXT("independent preflight observation"),
				{TEXT("WorldSpatial"), TEXT("Preflight")},
				false,
				TEXT("available"),
				{}),
			Revision);
		TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
		Test->TestTrue(
			TEXT("create independent current snapshot"),
			FUeAgentCanonicalSnapshot::TryCreate(
				TEXT("world-spatial-current-") + CapabilityId,
				EUeAgentStatePlane::AuthoringMemory,
				Revision,
				FUeAgentTemporalPoint(TEXT("fixture-clock"), TEXT("1")),
				FUeAgentStateProvenance(
					TEXT("independent_readback"),
					TEXT("world-spatial-owned-fixture"),
					TEXT("fixture-evidence")),
				{Entry},
				Resources,
				Snapshot,
				Error));
		return Snapshot;
	}

	TSharedPtr<FUeAgentOperationContext> MakeChangeContext(
		FAutomationTestBase* Test,
		const FUeAgentOperationDescriptor& Descriptor,
		const FString& RequestId,
		const FUeAgentResourceTrackerOwner& Resources)
	{
		const TArray<EUeAgentStatePlane>& Planes =
			Descriptor.GetExecutionContextPolicy().GetAllowedStatePlanes();
		if (!Test->TestTrue(TEXT("operation has state plane"), !Planes.IsEmpty()))
		{
			return nullptr;
		}
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("world-spatial-project");
		Input.EditorInstanceId = TEXT("world-spatial-editor");
		Input.StatePlane = Planes[0];
		Input.WorldType = EUeAgentExecutionWorldType::Preview;
		Input.WorldIdentity = TEXT("owned-world");
		Input.AssetEditorIdentity = TEXT("owned-asset-editor");
		Input.ViewportIdentity = TEXT("owned-viewport");
		Input.SelectionIdentities = {TEXT("owned-selection")};
		Input.ContextRevision = TEXT("owned-context-1");
		Input.bResolvedFromCurrent = false;
		FUeAgentExecutionContextRefPtr ExecutionContext;
		FUeAgentExecutionContextError ContextError;
		if (!Test->TestTrue(
				TEXT("freeze explicit C6 context: ")
					+ Descriptor.GetIdentity().GetDescriptorId(),
				FUeAgentExecutionContextRef::TryCreate(
					Input,
					Descriptor.GetIdentity(),
					EUeAgentRequestKind::Operation,
					Descriptor.GetExecutionContextPolicy(),
					ExecutionContext,
					ContextError)))
		{
			Test->AddError(TEXT("world_spatial_context_freeze_failed:")
				+ ContextError.Code + TEXT(":facet=")
				+ FString::FromInt(static_cast<int32>(ContextError.Facet)));
			return nullptr;
		}
		const FUeAgentOperationRequest Request(
			Descriptor.GetIdentity(),
			RequestId,
			TEXT("idem-") + RequestId,
			FUeAgentContractValue(TEXT("{}")));
		const FUeAgentCallerContext Caller(
			TEXT("world-spatial-fixture"),
			{TEXT("world-spatial:write")},
			TEXT("corr-") + RequestId);
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(30.0, []() { return 100.0; });
		const FUeAgentCancellationSource Cancellation([]() { return 100.0; });
		TSharedPtr<FUeAgentOperationContext> Context;
		FUeAgentOperationContextError Error;
		Test->TestTrue(
			TEXT("create C6 operation context"),
			FUeAgentOperationContext::TryCreate(
				Request,
				Caller,
				ExecutionContext.ToSharedRef(),
				Resources,
				Deadline,
				Cancellation.GetToken(),
				Context,
				Error));
		return Context;
	}

	const UE::UAI::StateSemantic::FUeAgentCanonicalValue* FindObjectValue(
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Object,
		const FString& Name)
	{
		for (const TPair<
			FString,
			UE::UAI::StateSemantic::FUeAgentCanonicalValueRef>& Item
			: Object.GetObjectItems())
		{
			if (Item.Key == Name)
			{
				return &Item.Value.Get();
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialTypedRequestDecoderTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E1.TypedRequestDecoder",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialTypedRequestDecoderTest::RunTest(
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
			"uai.domain_adapter.world_spatial.typed_request_fixture.v1")));
	const TArray<TSharedPtr<FJsonValue>>& Cases =
		Fixture->GetArrayField(TEXT("cases"));
	TestEqual(TEXT("22 exact typed request cases"), Cases.Num(), 22);

	TSet<FString> CapabilityIds;
	TSet<FString> TypeIds;
	TSet<FString> Families;
	TSet<FString> RequestKinds;
	for (const TSharedPtr<FJsonValue>& Value : Cases)
	{
		const TSharedPtr<FJsonObject> Case = Value->AsObject();
		if (!TestTrue(TEXT("typed request case is object"), Case.IsValid()))
		{
			continue;
		}
		const FString CapabilityId = Case->GetStringField(TEXT("capability_id"));
		const FString TypeId = Case->GetStringField(TEXT("typed_request_id"));
		const FString FamilyId = Case->GetStringField(TEXT("family_id"));
		const FString RequestKind = Case->GetStringField(TEXT("request_kind"));
		TestFalse(TEXT("capability unique: ") + CapabilityId, CapabilityIds.Contains(CapabilityId));
		TestFalse(TEXT("type unique: ") + TypeId, TypeIds.Contains(TypeId));
		TestFalse(TEXT("kind unique: ") + RequestKind, RequestKinds.Contains(RequestKind));
		CapabilityIds.Add(CapabilityId);
		TypeIds.Add(TypeId);
		Families.Add(FamilyId);
		RequestKinds.Add(RequestKind);
		TestTrue(
			TEXT("valid input exists: ") + CapabilityId,
			Case->HasTypedField<EJson::Object>(TEXT("valid_input")));
	}
	TestEqual(TEXT("five family decoder groups"), Families.Num(), 5);

	TSharedPtr<const FUeAgentDescriptorRegistry> DescriptorRegistry;
	FString DescriptorError;
	if (!TestTrue(
			TEXT("create exact WorldSpatial descriptor registry"),
			TryCreateWorldSpatialDescriptorRegistry(
				DescriptorRegistry,
				DescriptorError))
		|| !DescriptorRegistry.IsValid())
	{
		return false;
	}

	TSharedPtr<IUeAgentWorldSpatialRequestDecoder> Decoder;
	FUeAgentWorldSpatialExecutionError DecoderError;
	if (!TestTrue(
			TEXT("create production WorldSpatial typed decoder"),
			TryCreateWorldSpatialRequestDecoder(Decoder, DecoderError))
		|| !Decoder.IsValid())
	{
		return false;
	}
	const FUeAgentResourceTrackerOwner Resources = MakeResources();

	for (const TSharedPtr<FJsonValue>& Value : Cases)
	{
		const TSharedPtr<FJsonObject> Case = Value->AsObject();
		const FString CapabilityId = Case->GetStringField(TEXT("capability_id"));
		const FString TypeId = Case->GetStringField(TEXT("typed_request_id"));
		const FString FamilyId = Case->GetStringField(TEXT("family_id"));
		const FString RequestKind = Case->GetStringField(TEXT("request_kind"));
		const FUeAgentWorldSpatialExecutionChainContract* Contract =
			FindContract(CapabilityId);
		if (!TestNotNull(TEXT("execution contract exists: ") + CapabilityId, Contract))
		{
			continue;
		}
		TestEqual(TEXT("fixture type matches contract: ") + CapabilityId, TypeId, Contract->GetTypedRequestId());
		TestEqual(TEXT("fixture family matches contract: ") + CapabilityId, FamilyId, Contract->GetFamilyId());

		const FUeAgentOperationDescriptor* Descriptor =
			DescriptorRegistry->FindByDescriptorId(Contract->GetDescriptorId());
		if (!TestNotNull(TEXT("descriptor exists: ") + CapabilityId, Descriptor))
		{
			continue;
		}
		const FUeAgentContractSchema* InputSchema =
			DescriptorRegistry->FindCompiledInputSchemaExact(Descriptor->GetIdentity());
		if (!TestNotNull(TEXT("compiled input schema exists: ") + CapabilityId, InputSchema))
		{
			continue;
		}

		TOptional<FUeAgentContractValue> CanonicalInput;
		FUeAgentContractSchemaError SchemaError;
		int64 WorkUnits = 0;
		const FString InputText = JsonObjectText(Case->GetObjectField(TEXT("valid_input")));
		if (!TestTrue(
				TEXT("C0 canonicalizes typed input: ") + CapabilityId,
				InputSchema->TryValidateAndCanonicalize(
					FUeAgentContractValue(InputText),
					[&WorkUnits](
						const int64 Units,
						const FString& InstancePath,
						FUeAgentContractSchemaError& OutError)
					{
						WorkUnits += Units;
						return true;
					},
					CanonicalInput,
					SchemaError))
			|| !CanonicalInput.IsSet())
		{
			continue;
		}
		TestTrue(TEXT("schema traversal is checkpointed: ") + CapabilityId, WorkUnits > 0);

		FUeAgentWorldSpatialTypedRequestPtr Request;
		DecoderError = {};
		if (!TestTrue(
				TEXT("production decoder accepts canonical input: ") + CapabilityId,
				Decoder->TryDecode(
					*Contract,
					CanonicalInput.GetValue(),
					*Resources,
					Request,
					DecoderError))
			|| !Request.IsValid())
		{
			continue;
		}
		TestEqual(TEXT("decoded exact type: ") + CapabilityId, Request->GetTypeId(), TypeId);
		TestEqual(TEXT("decoded exact family: ") + CapabilityId, Request->GetFamilyId(), FamilyId);
		TestEqual(TEXT("decoded exact kind: ") + CapabilityId, Request->GetRequestKindId(), RequestKind);

		const FUeAgentWorldSpatialRequest& Concrete =
			static_cast<const FUeAgentWorldSpatialRequest&>(*Request);
		TestTrue(TEXT("typed payload matches exact request: ") + CapabilityId, PayloadMatchesKind(Concrete.GetPayload(), RequestKind));
		if (PayloadMatchesKind(Concrete.GetPayload(), RequestKind))
		{
			CheckRepresentativePayload(this, RequestKind, Concrete.GetPayload());
		}
	}

	const TArray<TSharedPtr<FJsonValue>>& Variants =
		Fixture->GetArrayField(TEXT("t45_semantic_variants"));
	TestEqual(TEXT("four T45 semantic variants"), Variants.Num(), 4);
	for (const TSharedPtr<FJsonValue>& Value : Variants)
	{
		const TSharedPtr<FJsonObject> Variant = Value->AsObject();
		const FString VariantId = Variant->GetStringField(TEXT("variant_id"));
		const FString CapabilityId = Variant->GetStringField(TEXT("capability_id"));
		const FUeAgentWorldSpatialExecutionChainContract* Contract =
			FindContract(CapabilityId);
		if (!TestNotNull(TEXT("variant contract exists: ") + VariantId, Contract))
		{
			continue;
		}
		const FUeAgentOperationDescriptor* Descriptor =
			DescriptorRegistry->FindByDescriptorId(Contract->GetDescriptorId());
		const FUeAgentContractSchema* InputSchema = Descriptor
			? DescriptorRegistry->FindCompiledInputSchemaExact(Descriptor->GetIdentity())
			: nullptr;
		if (!TestNotNull(TEXT("variant schema exists: ") + VariantId, InputSchema))
		{
			continue;
		}
		TOptional<FUeAgentContractValue> CanonicalInput;
		FUeAgentContractSchemaError SchemaError;
		if (!TestTrue(TEXT("variant schema accepts exact input: ") + VariantId,
				InputSchema->TryValidateAndCanonicalize(
					FUeAgentContractValue(JsonObjectText(Variant->GetObjectField(TEXT("valid_input")))),
					[](const int64, const FString&, FUeAgentContractSchemaError&) { return true; },
					CanonicalInput,
					SchemaError))
			|| !CanonicalInput.IsSet())
		{
			continue;
		}
		FUeAgentWorldSpatialTypedRequestPtr Request;
		DecoderError = {};
		if (!TestTrue(TEXT("production decoder accepts variant: ") + VariantId,
				Decoder->TryDecode(*Contract, CanonicalInput.GetValue(), *Resources,
					Request, DecoderError))
			|| !Request.IsValid())
		{
			continue;
		}
		const FUeAgentWorldSpatialRequest& Concrete =
			static_cast<const FUeAgentWorldSpatialRequest&>(*Request);
		if (VariantId == TEXT("explicit_actor_identity_set"))
		{
			const FViewportActorSelectRequestInput& Input =
				Concrete.GetPayload().Get<FViewportActorSelectRequestInput>();
			TestEqual(TEXT("variant preserves two explicit actor identities"), Input.ActorRefs.Num(), 2);
			TestFalse(TEXT("identity variant omits screen selector"), Input.ScreenPoint.IsSet());
		}
		else if (VariantId == TEXT("level_viewport_widget_mode_patch"))
		{
			const FViewportStateUpdateRequestInput& Input =
				Concrete.GetPayload().Get<FViewportStateUpdateRequestInput>();
			TestTrue(TEXT("level viewport widget patch exists"),
				Input.StatePatch.Interaction.IsSet());
			if (Input.StatePatch.Interaction.IsSet())
			{
				TestEqual(TEXT("level viewport widget patch preserves rotate"),
					Input.StatePatch.Interaction->WidgetMode,
					FString(TEXT("rotate")));
			}
		}
		else
		{
			const FViewportCameraFrameRequestInput& Input =
				Concrete.GetPayload().Get<FViewportCameraFrameRequestInput>();
			TestTrue(TEXT("camera variant has exactly one selector"), Input.HasExactlyOneSelector());
			if (VariantId == TEXT("outliner_folder_frame"))
			{
				TestTrue(TEXT("folder variant preserves folder path"), Input.FolderPath.IsSet());
				TestTrue(TEXT("folder variant includes child folders"), Input.bIncludeChildFolders);
			}
			else
			{
				TestTrue(TEXT("selection variant preserves current-selection selector"), Input.bUseCurrentSelection);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialTypedChangeDraftTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E1.TypedChangeDraft",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialTypedChangeDraftTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::WorldSpatial;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TSharedPtr<const FUeAgentDescriptorRegistry> DescriptorRegistry;
	FString DescriptorError;
	if (!Fixture.IsValid()
		|| !TestTrue(
			TEXT("create exact descriptors for C6"),
			TryCreateWorldSpatialDescriptorRegistry(
				DescriptorRegistry,
				DescriptorError))
		|| !DescriptorRegistry.IsValid())
	{
		return false;
	}
	TSharedPtr<IUeAgentWorldSpatialRequestDecoder> Decoder;
	FUeAgentWorldSpatialExecutionError DecoderError;
	if (!TestTrue(
			TEXT("create production decoder for C6"),
			TryCreateWorldSpatialRequestDecoder(Decoder, DecoderError))
		|| !Decoder.IsValid())
	{
		return false;
	}

	const FUeAgentResourceTrackerOwner Resources = MakeResources();
	int32 OperationCount = 0;
	for (const TSharedPtr<FJsonValue>& Value
		: Fixture->GetArrayField(TEXT("cases")))
	{
		const TSharedPtr<FJsonObject> Case = Value->AsObject();
		const FString CapabilityId =
			Case->GetStringField(TEXT("capability_id"));
		const FUeAgentWorldSpatialExecutionChainContract* Contract =
			FindContract(CapabilityId);
		if (!Contract || Contract->GetC3Lane() != TEXT("c3o"))
		{
			continue;
		}
		++OperationCount;
		const FUeAgentOperationDescriptor* Descriptor =
			DescriptorRegistry->FindByDescriptorId(Contract->GetDescriptorId());
		const FUeAgentContractSchema* InputSchema = Descriptor
			? DescriptorRegistry->FindCompiledInputSchemaExact(
				Descriptor->GetIdentity())
			: nullptr;
		if (!TestNotNull(TEXT("C6 operation descriptor exists"), Descriptor)
			|| !TestNotNull(TEXT("C6 input schema exists"), InputSchema))
		{
			continue;
		}
		TOptional<FUeAgentContractValue> CanonicalInput;
		FUeAgentContractSchemaError SchemaError;
		if (!TestTrue(
				TEXT("C0 canonicalizes C6 input: ") + CapabilityId,
				InputSchema->TryValidateAndCanonicalize(
					FUeAgentContractValue(JsonObjectText(
						Case->GetObjectField(TEXT("valid_input")))),
					[](const int64, const FString&, FUeAgentContractSchemaError&)
					{
						return true;
					},
					CanonicalInput,
					SchemaError))
			|| !CanonicalInput.IsSet())
		{
			continue;
		}
		FUeAgentWorldSpatialTypedRequestPtr Request;
		if (!TestTrue(
				TEXT("decode typed C6 request: ") + CapabilityId,
				Decoder->TryDecode(
					*Contract,
					CanonicalInput.GetValue(),
					*Resources,
					Request,
					DecoderError))
			|| !Request.IsValid())
		{
			continue;
		}
		const TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot =
			MakeChangeSnapshot(this, CapabilityId, *Resources);
		const TSharedPtr<FUeAgentOperationContext> Context =
			MakeChangeContext(
				this,
				*Descriptor,
				TEXT("c6-") + FString::FromInt(OperationCount),
				Resources);
		if (!Snapshot.IsValid() || !Context.IsValid())
		{
			continue;
		}
		TOptional<FChangeDraft> Draft;
		FUeAgentWorldSpatialExecutionError Error;
		if (!TestTrue(
				TEXT("typed request builds C6 draft: ") + CapabilityId,
				Request->TryBuildChangeDraft(
					*Snapshot,
					*Context,
					Draft,
					Error))
			|| !TestTrue(TEXT("C6 draft is present"), Draft.IsSet()))
		{
			continue;
		}
		const FUeAgentCanonicalValuePtr& Proposed =
			Draft->GetProposedAfter();
		TestTrue(TEXT("C6 proposed state exists"), Proposed.IsValid());
		if (!Proposed.IsValid())
		{
			continue;
		}
		TestEqual(
			TEXT("C6 proposed state is typed object"),
			Proposed->GetKind(),
			EUeAgentValueKind::Object);
		const FUeAgentCanonicalValue* RequestKind =
			FindObjectValue(*Proposed, TEXT("request_kind"));
		const FUeAgentCanonicalValue* Family =
			FindObjectValue(*Proposed, TEXT("family_id"));
		const FUeAgentCanonicalValue* Payload =
			FindObjectValue(*Proposed, TEXT("typed_payload"));
		TestTrue(
			TEXT("C6 retains exact request kind"),
			RequestKind
				&& RequestKind->GetKind() == EUeAgentValueKind::String
				&& RequestKind->GetText() == Request->GetRequestKindId());
		TestTrue(
			TEXT("C6 retains exact family"),
			Family
				&& Family->GetKind() == EUeAgentValueKind::String
				&& Family->GetText() == Request->GetFamilyId());
		TestTrue(
			TEXT("C6 retains effect-bearing typed payload"),
			Payload && Payload->GetKind() == EUeAgentValueKind::Object
				&& !Payload->GetObjectItems().IsEmpty());
	}
	TestEqual(TEXT("twelve operation C6 drafts covered"), OperationCount, 12);
	return true;
}
