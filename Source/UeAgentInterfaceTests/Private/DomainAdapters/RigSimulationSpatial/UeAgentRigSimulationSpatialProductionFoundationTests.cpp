#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialExecutionChains.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialProductionRuntime.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialRequests.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace
{
	using namespace UE::UAI::DomainAdapters::RigSimulationSpatial;

	TSharedPtr<FJsonObject> LoadExactFixture(FAutomationTestBase* Test)
	{
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("UeAgentInterface/Source/UeAgentInterfaceTests/Private/"
				"DomainAdapters/RigSimulationSpatial/Fixtures/"
				"rig_simulation_spatial_exact_contract_cases.json")));
		FString Text;
		if (!Test->TestTrue(TEXT("load corrected exact fixture"),
			FFileHelper::LoadFileToString(Text, *Path))) return nullptr;
		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(TEXT("parse corrected exact fixture"),
			FJsonSerializer::Deserialize(Reader, Fixture) && Fixture.IsValid()))
			return nullptr;
		return Fixture;
	}

	FString ExampleString(
		const FString& Path,
		const FRigSimulationSpatialRevision& Revision)
	{
		if (Path.EndsWith(TEXT("source_id"))) return Revision.SourceId;
		if (Path.EndsWith(TEXT("generation"))) return Revision.Generation;
		if (Path.EndsWith(TEXT("sequence"))) return LexToString(Revision.Sequence);
		if (Path.EndsWith(TEXT("clock_id"))) return TEXT("clock://evaluation");
		if (Path.EndsWith(TEXT("upper_tick"))) return TEXT("1");
		if (Path.EndsWith(TEXT("tick"))) return TEXT("0");
		if (Path.EndsWith(TEXT("numerator"))) return TEXT("30");
		if (Path.EndsWith(TEXT("denominator"))) return TEXT("1");
		if (Path.EndsWith(TEXT("frame_id"))
			&& !Path.EndsWith(TEXT("basis_frame_id"))) return TEXT("frame://requested");
		if (Path.EndsWith(TEXT("basis_frame_id"))) return TEXT("frame://basis");
		if (Path.EndsWith(TEXT("basis_evidence_id"))) return TEXT("basis://fixture");
		if (Path.EndsWith(TEXT("unit"))) return TEXT("cm");
		if (Path.Contains(TEXT("evidence"))) return TEXT("conversion://fixture");
		if (Path.EndsWith(TEXT("destination_root"))) return TEXT("fixture://object");
		if (Path.EndsWith(TEXT("_ref")) || Path.EndsWith(TEXT("_refs[]")))
			return TEXT("fixture://object");
		if (Path.EndsWith(TEXT("name"))) return TEXT("FixtureAsset");
		return TEXT("fixture");
	}

	TSharedPtr<FJsonValue> BuildExampleValue(
		const TSharedPtr<FJsonObject>& Schema,
		const TSharedPtr<FJsonObject>& Components,
		const FString& Path,
		const FRigSimulationSpatialRevision& Revision,
		FString& OutError)
	{
		if (!Schema.IsValid())
		{
			OutError = TEXT("example_schema_invalid:") + Path;
			return nullptr;
		}
		FString ComponentName;
		if (Schema->TryGetStringField(TEXT("component"), ComponentName))
		{
			const TSharedPtr<FJsonValue>* Found = Components->Values.Find(ComponentName);
			if (!Found || !Found->IsValid() || (*Found)->Type != EJson::Object)
			{
				OutError = TEXT("example_component_missing:") + ComponentName;
				return nullptr;
			}
			return BuildExampleValue((*Found)->AsObject(), Components, Path,
				Revision, OutError);
		}

		FString Type;
		if (!Schema->TryGetStringField(TEXT("type"), Type))
		{
			OutError = TEXT("example_type_missing:") + Path;
			return nullptr;
		}
		if (Type == TEXT("string"))
		{
			FString Constant;
			if (Schema->TryGetStringField(TEXT("const"), Constant))
				return MakeShared<FJsonValueString>(Constant);
			const TArray<TSharedPtr<FJsonValue>>* Enum = nullptr;
			if (Schema->TryGetArrayField(TEXT("enum"), Enum) && Enum && !Enum->IsEmpty())
				return MakeShared<FJsonValueString>((*Enum)[0]->AsString());
			return MakeShared<FJsonValueString>(ExampleString(Path, Revision));
		}
		if (Type == TEXT("integer") || Type == TEXT("number"))
		{
			double Value = 0.0;
			if (Path.EndsWith(TEXT("sequence"))) Value = Revision.Sequence;
			else if (!Schema->TryGetNumberField(TEXT("const"), Value))
				Schema->TryGetNumberField(TEXT("minimum"), Value);
			return MakeShared<FJsonValueNumber>(Value);
		}
		if (Type == TEXT("boolean")) return MakeShared<FJsonValueBoolean>(false);
		if (Type == TEXT("array"))
		{
			const TSharedPtr<FJsonObject>* Items = nullptr;
			double Minimum = 0.0;
			Schema->TryGetNumberField(TEXT("minItems"), Minimum);
			if (!Schema->TryGetObjectField(TEXT("items"), Items) || !Items || !Items->IsValid())
			{
				OutError = TEXT("example_array_items_missing:") + Path;
				return nullptr;
			}
			TArray<TSharedPtr<FJsonValue>> Values;
			if (Path.EndsWith(TEXT("components"))
				&& static_cast<int32>(Minimum) == 16)
			{
				static constexpr const TCHAR* IdentityMatrix[16] = {
					TEXT("1"), TEXT("0"), TEXT("0"), TEXT("0"),
					TEXT("0"), TEXT("1"), TEXT("0"), TEXT("0"),
					TEXT("0"), TEXT("0"), TEXT("1"), TEXT("0"),
					TEXT("0"), TEXT("0"), TEXT("0"), TEXT("1")};
				for (const TCHAR* Component : IdentityMatrix)
					Values.Add(MakeShared<FJsonValueString>(Component));
				return MakeShared<FJsonValueArray>(MoveTemp(Values));
			}
			for (int32 Index = 0; Index < static_cast<int32>(Minimum); ++Index)
			{
				TSharedPtr<FJsonValue> Item = BuildExampleValue(
					*Items, Components, Path + TEXT("[]"), Revision, OutError);
				if (!Item.IsValid()) return nullptr;
				Values.Add(MoveTemp(Item));
			}
			return MakeShared<FJsonValueArray>(MoveTemp(Values));
		}
		if (Type == TEXT("object"))
		{
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (!Schema->TryGetObjectField(TEXT("properties"), Properties)
				|| !Properties || !Properties->IsValid())
			{
				OutError = TEXT("example_properties_missing:") + Path;
				return nullptr;
			}
			TSet<FString> Required;
			const TArray<TSharedPtr<FJsonValue>>* RequiredValues = nullptr;
			if (Schema->TryGetArrayField(TEXT("required"), RequiredValues)
				&& RequiredValues)
			{
				for (const TSharedPtr<FJsonValue>& Value : *RequiredValues)
					Required.Add(Value->AsString());
			}
			double MinimumProperties = 0.0;
			Schema->TryGetNumberField(TEXT("minProperties"), MinimumProperties);
			TArray<FString> Keys;
			(*Properties)->Values.GetKeys(Keys);
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			for (const FString& Key : Keys)
			{
				if (!Required.Contains(Key)
					&& Result->Values.Num() >= static_cast<int32>(MinimumProperties)) continue;
				const TSharedPtr<FJsonValue> PropertySchema =
					(*Properties)->Values.FindChecked(Key);
				TSharedPtr<FJsonValue> Value = BuildExampleValue(
					PropertySchema->AsObject(), Components,
					Path.IsEmpty() ? Key : Path + TEXT(".") + Key,
					Revision, OutError);
				if (!Value.IsValid()) return nullptr;
				Result->SetField(Key, MoveTemp(Value));
			}
			return MakeShared<FJsonValueObject>(Result);
		}
		OutError = TEXT("example_type_unsupported:") + Type;
		return nullptr;
	}

	FString BuildDescriptorInput(
		const TSharedPtr<FJsonObject>& Descriptor,
		const TSharedPtr<FJsonObject>& Components,
		const FRigSimulationSpatialRevision& Revision,
		FString& OutError)
	{
		const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		for (const TSharedPtr<FJsonValue>& FieldValue
			: Descriptor->GetArrayField(TEXT("input_fields")))
		{
			const TSharedPtr<FJsonObject> Field = FieldValue->AsObject();
			if (!Field->GetBoolField(TEXT("required"))) continue;
			const FString Name = Field->GetStringField(TEXT("name"));
			TSharedPtr<FJsonObject> Schema;
			FString Component;
			if (Field->TryGetStringField(TEXT("component"), Component))
			{
				Schema = MakeShared<FJsonObject>();
				Schema->SetStringField(TEXT("component"), Component);
			}
			else
			{
				const TSharedPtr<FJsonObject>* Found = nullptr;
				if (!Field->TryGetObjectField(TEXT("schema"), Found) || !Found)
				{
					OutError = TEXT("descriptor_field_schema_missing:") + Name;
					return FString();
				}
				Schema = *Found;
			}
			TSharedPtr<FJsonValue> Value = BuildExampleValue(
				Schema, Components, Name, Revision, OutError);
			if (!Value.IsValid()) return FString();
			Root->SetField(Name, MoveTemp(Value));
		}
		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
		{
			OutError = TEXT("descriptor_input_serialize_failed");
			return FString();
		}
		return Text;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentRigSimulationSpatialProductionFoundationTest,
	"UeAgentInterface.DomainAdapters.RigSimulationSpatial.E2.ProductionFoundation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentRigSimulationSpatialProductionFoundationTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::RigSimulationSpatial;
	using namespace UE::UAI::StateSemantic;

	FUeAgentRigSimulationSpatialProductionRuntimeConfig Config;
	Config.ProjectInstanceId = TEXT("rsp002-project");
	Config.EditorInstanceId = TEXT("rsp002-editor");
	Config.ServerSessionId = TEXT("rsp002-session");
	Config.RuntimeGeneration = TEXT("rsp002-generation");
	Config.RegistryEpoch = 1;
	Config.ObjectBindings.Add(FUeAgentRigSimulationSpatialObjectBinding(
		TEXT("skeleton://fixture"), GetTransientPackage()));
	Config.ObjectBindings.Add(FUeAgentRigSimulationSpatialObjectBinding(
		TEXT("geometry-cache://fixture"), GetTransientPackage()));
	Config.ClockBindings.Add(FUeAgentRigSimulationSpatialClockBinding(
		TEXT("clock://evaluation"), TEXT("clock://native-evaluation"),
		TEXT("clock-evidence://evaluation")));

	TSharedPtr<FUeAgentRigSimulationSpatialProductionRuntime> Runtime;
	FUeAgentRigSimulationSpatialExecutionError Error;
	if (!TestTrue(
			TEXT("production runtime composes from explicit environment"),
			FUeAgentRigSimulationSpatialProductionRuntime::TryCreate(
				Config, Runtime, Error))
		|| !Runtime.IsValid())
	{
		AddError(TEXT("rig_simulation_spatial_runtime_unavailable:")
			+ Error.Code + TEXT(":") + Error.Detail);
		return false;
	}

	for (const FUeAgentRigSimulationSpatialExecutionChainContract& Contract
		: GetRigSimulationSpatialExecutionChainContracts())
	{
		if (Contract.GetC3Lane() == TEXT("c3q"))
		{
			TestTrue(
				TEXT("query has exact production provider: ")
					+ Contract.GetCapabilityId(),
				Runtime->GetQueryProviderRegistry()->FindExact(
					Contract.GetBindingId()).IsValid());
		}
	}

	const TMap<FString, TArray<FString>> IdentityFields = {
		{TEXT("rig.skeleton.preview_mesh.bind"), {TEXT("skeleton_ref"), TEXT("preview_mesh_ref")}},
		{TEXT("rig.skeleton.socket.update"), {TEXT("skeleton_ref")}},
		{TEXT("rig.control.bake.animation"), {TEXT("source_sequence_ref"), TEXT("target_animation_ref"), TEXT("binding_ref"), TEXT("rig_ref")}},
		{TEXT("rig.control.bake.controls"), {TEXT("target_sequence_ref"), TEXT("binding_ref"), TEXT("rig_ref")}},
		{TEXT("rig.ik.definition.update"), {TEXT("ik_rig_ref")}},
		{TEXT("rig.retarget.definition.update"), {TEXT("retargeter_ref"), TEXT("source_rig_ref"), TEXT("target_rig_ref")}},
		{TEXT("rig.retarget.pose.update"), {TEXT("retargeter_ref")}},
		{TEXT("rig.retarget.pose.auto_align"), {TEXT("retargeter_ref")}},
		{TEXT("rig.retarget.assets.generate"), {TEXT("source_subject_ref"), TEXT("target_subject_ref"), TEXT("retargeter_ref"), TEXT("destination_root")}},
		{TEXT("physics.material.update"), {TEXT("physical_material_ref")}},
		{TEXT("physics.asset.structure.replace"), {TEXT("physics_asset_ref")}},
		{TEXT("physics.constraint.update"), {TEXT("physics_asset_ref")}},
		{TEXT("physics.simulation.probe"), {TEXT("subject_refs")}},
		{TEXT("deformer.geometry.sample"), {TEXT("geometry_cache_ref")}},
		{TEXT("deformer.training_inputs.validate"), {TEXT("training_input_refs")}},
		{TEXT("deformer.evaluation.sample"), {TEXT("deformer_ref"), TEXT("subject_ref")}},
	};
	for (const TPair<FString, TArray<FString>>& Pair : IdentityFields)
	{
		TArray<FString> Actual;
		TestTrue(TEXT("identity specification exists: ") + Pair.Key,
			Runtime->TryGetIdentityFields(Pair.Key, Actual, Error));
		TestEqual(TEXT("identity field count: ") + Pair.Key,
			Actual.Num(), Pair.Value.Num());
		for (int32 Index = 0; Index < Pair.Value.Num() && Index < Actual.Num(); ++Index)
		{
			TestEqual(TEXT("identity field order: ") + Pair.Key,
				Actual[Index], Pair.Value[Index]);
		}
	}

	FRigSimulationSpatialRevision Revision;
	TestTrue(TEXT("revision comes from explicit object binding"),
		Runtime->TryReadRevision(TEXT("skeleton://fixture"), Revision, Error));

	FUeAgentRigSimulationSpatialProductionRuntimeConfig CompleteConfig = Config;
	CompleteConfig.ObjectBindings.Add(FUeAgentRigSimulationSpatialObjectBinding(
		TEXT("fixture://object"), GetTransientPackage()));
	const FUeAgentTemporalPoint ObservedAt(
		TEXT("clock://evaluation"), TEXT("0"), TEXT("30"), TEXT("1"),
		TEXT("evaluation"));
	const TArray<TArray<FString>> IdentityBasis = {
		{TEXT("1"), TEXT("0"), TEXT("0")},
		{TEXT("0"), TEXT("1"), TEXT("0")},
		{TEXT("0"), TEXT("0"), TEXT("1")},
	};
	CompleteConfig.FrameBindings.Add(FUeAgentRigSimulationSpatialFrameBinding(
		FUeAgentSpatialFrame(
			TEXT("frame://requested"), TEXT("frame://basis"), 3,
			IdentityBasis, {TEXT("0"), TEXT("0"), TEXT("0")}, TEXT("cm"),
			TEXT("basis://fixture"), ObservedAt),
		false));
	CompleteConfig.FrameBindings.Add(FUeAgentRigSimulationSpatialFrameBinding(
		FUeAgentSpatialFrame(
			TEXT("frame://native"), TEXT("frame://basis"), 3,
			IdentityBasis, {TEXT("0"), TEXT("0"), TEXT("0")}, TEXT("cm"),
			TEXT("basis://fixture"), ObservedAt),
		true));
	CompleteConfig.SpatialConversions.Add(FUeAgentSpatialConversion(
		TEXT("conversion://fixture"), TEXT("frame://requested"),
		TEXT("frame://native"), 3, 3, IdentityBasis,
		{TEXT("0"), TEXT("0"), TEXT("0")}, ObservedAt,
		TEXT("conversion://fixture")));

	TSharedPtr<FUeAgentRigSimulationSpatialProductionRuntime> CompleteRuntime;
	if (!TestTrue(TEXT("complete production environment composes"),
		FUeAgentRigSimulationSpatialProductionRuntime::TryCreate(
			CompleteConfig, CompleteRuntime, Error))
		|| !CompleteRuntime.IsValid())
	{
		AddError(TEXT("rig_simulation_spatial_complete_runtime_unavailable:")
			+ Error.Code + TEXT(":") + Error.Detail);
		return false;
	}
	FRigSimulationSpatialRevision CompleteRevision;
	if (!TestTrue(TEXT("complete environment exposes fixture revision"),
		CompleteRuntime->TryReadRevision(
			TEXT("fixture://object"), CompleteRevision, Error)))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> ExactFixture = LoadExactFixture(this);
	if (!ExactFixture.IsValid()) return false;
	const TSharedPtr<FJsonObject>* Components = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Descriptors = nullptr;
	if (!TestTrue(TEXT("exact fixture exposes reusable components"),
		ExactFixture->TryGetObjectField(TEXT("components"), Components)
			&& Components && Components->IsValid())
		|| !TestTrue(TEXT("exact fixture exposes descriptors"),
			ExactFixture->TryGetArrayField(TEXT("descriptors"), Descriptors)
				&& Descriptors && !Descriptors->IsEmpty()))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& DescriptorValue : *Descriptors)
	{
		const TSharedPtr<FJsonObject> Descriptor = DescriptorValue->AsObject();
		const FString CapabilityId = Descriptor->GetStringField(TEXT("capability_id"));
		FString BuildError;
		const FString Input = BuildDescriptorInput(
			Descriptor, *Components, CompleteRevision, BuildError);
		if (!TestTrue(TEXT("schema generates exact input: ") + CapabilityId,
			BuildError.IsEmpty() && !Input.IsEmpty()))
		{
			AddError(CapabilityId + TEXT(":") + BuildError);
			continue;
		}
		TSharedPtr<const FUeAgentRigSimulationSpatialRequest> TypedRequest;
		if (!TestTrue(TEXT("production decoder accepts exact input: ") + CapabilityId,
			CompleteRuntime->TryDecodeForExecution(
				CapabilityId, FUeAgentContractValue(Input), TypedRequest, Error)))
		{
			AddError(CapabilityId + TEXT(":") + Error.Code + TEXT(":") + Error.Detail);
			continue;
		}
		TestTrue(TEXT("typed request exists: ") + CapabilityId,
			TypedRequest.IsValid());
		if (TypedRequest.IsValid())
		{
			TestTrue(TEXT("typed request kind is explicit: ") + CapabilityId,
				!TypedRequest->GetRequestKindId().IsEmpty());
			TestEqual(TEXT("typed request target is explicit: ") + CapabilityId,
				TypedRequest->GetPrimaryTargetRef(), TEXT("fixture://object"));
			for (const FUeAgentSpatialValuePtr& SpatialValue
				: TypedRequest->GetFormalPortRequest().GetSpatialValues())
			{
				TestTrue(TEXT("decoded C4 spatial value is valid: ") + CapabilityId,
					SpatialValue.IsValid() && SpatialValue->IsValid());
			}
		}
	}
	TestEqual(TEXT("all typed decodes avoid native family dispatch"),
		CompleteRuntime->GetFamilyDispatchCallCount(), 0);

	const FString ValidPreview = FString::Printf(
		TEXT("{\"skeleton_ref\":\"skeleton://fixture\","
			"\"preview_mesh_ref\":\"mesh://fixture\","
			"\"expected_revision\":{\"source_id\":\"%s\","
			"\"generation\":\"%s\",\"sequence\":\"%lld\"}}"),
		*Revision.SourceId, *Revision.Generation, Revision.Sequence);
	TSharedPtr<const FUeAgentRigSimulationSpatialRequest> Request;
	TestTrue(TEXT("correct target and revision decode to typed request"),
		Runtime->TryDecodeForExecution(
			TEXT("rig.skeleton.preview_mesh.bind"),
			FUeAgentContractValue(ValidPreview), Request, Error));
	if (Request.IsValid())
	{
		TestEqual(TEXT("typed request kind is exact"),
			Request->GetRequestKindId(), TEXT("skeleton.bind_preview_mesh.request.v1"));
		TestEqual(TEXT("typed request primary target is exact"),
			Request->GetPrimaryTargetRef(), TEXT("skeleton://fixture"));
	}

	const int32 CallsBefore = Runtime->GetFamilyDispatchCallCount();
	Request.Reset();
	TestFalse(TEXT("missing target fails before family dispatch"),
		Runtime->TryDecodeForExecution(
			TEXT("rig.skeleton.preview_mesh.bind"),
			FUeAgentContractValue(TEXT("{\"preview_mesh_ref\":\"mesh://fixture\"}")),
			Request, Error));
	TestEqual(TEXT("missing target error is stable"), Error.Code,
		TEXT("rig_simulation_spatial_required_identity_missing"));

	Request.Reset();
	TestFalse(TEXT("missing revision fails before family dispatch"),
		Runtime->TryDecodeForExecution(
			TEXT("rig.skeleton.preview_mesh.bind"),
			FUeAgentContractValue(TEXT("{\"skeleton_ref\":\"skeleton://fixture\","
				"\"preview_mesh_ref\":\"mesh://fixture\"}")), Request, Error));
	TestEqual(TEXT("missing revision error is stable"), Error.Code,
		TEXT("rig_simulation_spatial_expected_revision_missing"));

	Request.Reset();
	TestFalse(TEXT("missing frame conversion fails before family dispatch"),
		Runtime->TryDecodeForExecution(
			TEXT("deformer.geometry.sample"),
			FUeAgentContractValue(TEXT(
				"{\"geometry_cache_ref\":\"geometry-cache://fixture\","
				"\"sample_time\":{\"clock_id\":\"clock://evaluation\","
				"\"tick\":\"0\",\"rate\":{\"numerator\":\"30\","
				"\"denominator\":\"1\"},\"state\":\"evaluation\"},"
				"\"output_frame\":{\"frame_id\":\"frame://requested\","
				"\"basis_frame_id\":\"frame://basis\",\"dimension\":3,"
				"\"unit\":\"cm\",\"basis_evidence_id\":\"basis://fixture\"}}")),
			Request, Error));
	TestEqual(TEXT("missing conversion error is stable"), Error.Code,
		TEXT("rig_simulation_spatial_frame_conversion_missing"));
	TestEqual(TEXT("all preflight failures precede native family dispatch"),
		Runtime->GetFamilyDispatchCallCount(), CallsBefore);
	return true;
}
