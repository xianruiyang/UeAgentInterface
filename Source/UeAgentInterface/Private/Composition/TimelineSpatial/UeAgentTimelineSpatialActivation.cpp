#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialDescriptors.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialExecutionChains.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationDescriptors.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationExecutionChains.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialDescriptors.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialExecutionChains.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Composition::TimelineSpatial
{
	using namespace CommandContract;
	using namespace Contract;

	namespace
	{
		FDiagnostic RegistryFailure(const FString& Code, const FString& Reason,
			const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("registry"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.ActualValuePresent = true;
			Error.ExpectedValuePresent = true;
			return Error;
		}

		bool IsIdentifier(const FString& Value)
		{
			if (Value.IsEmpty()
				|| !(FChar::IsAlpha(Value[0]) || Value[0] == TEXT('_')))
			{
				return false;
			}
			for (int32 Index = 1; Index < Value.Len(); ++Index)
			{
				if (!(FChar::IsAlnum(Value[Index])
					|| Value[Index] == TEXT('_')))
				{
					return false;
				}
			}
			return true;
		}

		bool TryPythonBinding(
			const FString& CapabilityId,
			FString& OutNamespace,
			FString& OutMethod,
			FDiagnostic& OutError)
		{
			OutError.Reset();
			TArray<FString> Segments;
			CapabilityId.ParseIntoArray(Segments, TEXT("."), true);
			if (Segments.Num() < 2)
			{
				OutError = FDiagnostic::Failure(TEXT("timeline_spatial_activation_profile_invalid"),
					TEXT("A Timeline/Spatial capability requires at least two non-empty dotted segments to form its public namespace and method."),
					TEXT("registry"), FString(), CapabilityId);
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("/capability_id");
				OutError.ActualValue = MakeShared<FJsonValueString>(CapabilityId);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("namespace.method"));
				return false;
			}
			OutNamespace = Segments[0];
			Segments.RemoveAt(0);
			OutMethod = FString::Join(Segments, TEXT("_"));
			if (!IsIdentifier(OutNamespace) || !IsIdentifier(OutMethod))
			{
				const bool bNamespaceInvalid = !IsIdentifier(OutNamespace);
				OutError = FDiagnostic::Failure(TEXT("timeline_spatial_activation_profile_invalid"),
					FString::Printf(TEXT("Command '%s' produces an invalid public %s identifier."),
						*CapabilityId, bNamespaceInvalid ? TEXT("namespace") : TEXT("method")),
					TEXT("registry"), FString(), CapabilityId);
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = bNamespaceInvalid ? TEXT("/namespace") : TEXT("/method");
				OutError.ActualValue = MakeShared<FJsonValueString>(bNamespaceInvalid ? OutNamespace : OutMethod);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(
					TEXT("letter or underscore followed by letters, digits or underscores"));
				return false;
			}
			return true;
		}

		bool HasRequiredInput(const FUeAgentOperationDescriptor& Descriptor)
		{
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(
					Descriptor.GetInputSchema().GetJsonValue());
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
			return Root->TryGetArrayField(TEXT("required"), Required)
				&& Required
				&& !Required->IsEmpty();
		}

		bool RequiresPublicContext(
			const FString& AdapterId,
			const FString& CapabilityId)
		{
			if (AdapterId == TEXT("world_spatial"))
			{
				return CapabilityId != TEXT("mesh.transform.bake")
					&& CapabilityId != TEXT("mesh.pivot.update")
					&& CapabilityId != TEXT("viewport.instance.list");
			}
			if (AdapterId == TEXT("timeline_animation"))
			{
				return CapabilityId.StartsWith(TEXT("animation.subject."))
					|| CapabilityId
						== TEXT("animation.ik_probe_candidate.apply");
			}
			static const TSet<FString> PublicRigCapabilities = {
				TEXT("rig.control.bake.animation"),
				TEXT("rig.control.bake.controls"),
				TEXT("physics.simulation.probe"),
				TEXT("deformer.geometry.sample"),
				TEXT("deformer.evaluation.sample"),
			};
			return PublicRigCapabilities.Contains(CapabilityId);
		}

		bool IsSessionControl(const FString& CapabilityId)
		{
			static const TSet<FString> SessionCapabilities = {
				TEXT("graph.view.update"),
				TEXT("viewport.actor.select"),
				TEXT("mesh.element.select_by_ray"),
				TEXT("viewport.camera.frame"),
				TEXT("viewport.state.restore"),
				TEXT("viewport.state.update"),
				TEXT("animation.subject.pose.update"),
				TEXT("animation.subject.morph_weights.update"),
				TEXT("physics.simulation.probe"),
			};
			return SessionCapabilities.Contains(CapabilityId);
		}

		bool RequiresTarget(const FString& CapabilityId)
		{
			static const TSet<FString> TargetlessCapabilities = {
				TEXT("viewport.warning.list"),
				TEXT("viewport.view_mode.list"),
				TEXT("viewport.instance.list"),
				TEXT("animation.montage.create"),
				TEXT("timeline.sequence.create"),
				TEXT("rig.retarget.assets.generate"),
			};
			return !TargetlessCapabilities.Contains(CapabilityId);
		}

		TArray<FUeAgentModelResultFieldDeclaration> ModelResultFields(
			const DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialExecutionChainContract&)
		{
			return {};
		}

		TArray<FUeAgentModelResultFieldDeclaration> ModelResultFields(
			const DomainAdapters::TimelineAnimation::
				FUeAgentTimelineAnimationExecutionChainContract& Chain)
		{
			return Chain.GetModelResultFields();
		}

		TArray<FUeAgentModelResultFieldDeclaration> ModelResultFields(
			const DomainAdapters::RigSimulationSpatial::
				FUeAgentRigSimulationSpatialExecutionChainContract& Chain)
		{
			return Chain.GetModelResultFields();
		}

		template <typename TExecutionChainContract>
		bool TryAppendProfiles(
			const FString& AdapterId,
			const TArray<TExecutionChainContract>& Chains,
			const FUeAgentDescriptorRegistry& Descriptors,
			TArray<FUeAgentTimelineSpatialActivationProfile>& OutProfiles,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			for (const TExecutionChainContract& Chain : Chains)
			{
				const FUeAgentOperationDescriptor* Descriptor =
					Descriptors.FindByDescriptorId(Chain.GetDescriptorId());
				if (!Descriptor
					|| Descriptor->GetCapabilityId()
						!= Chain.GetCapabilityId())
				{
					OutError = !Descriptor
						? RegistryFailure(TEXT("timeline_spatial_descriptor_missing"),
							TEXT("The execution chain has no registered operation descriptor"), TEXT("descriptor_present"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true))
						: RegistryFailure(TEXT("timeline_spatial_descriptor_missing"),
							TEXT("The operation descriptor capability differs from its execution chain"), TEXT("capability_id"),
							MakeShared<FJsonValueString>(Descriptor->GetCapabilityId()), MakeShared<FJsonValueString>(Chain.GetCapabilityId()));
					OutError.CandidateName = Chain.GetCapabilityId();
					OutError.SubjectId = Chain.GetDescriptorId();
					return false;
				}

				FUeAgentTimelineSpatialActivationProfile Profile(
					Descriptor->GetIdentity());
				Profile.AdapterId = AdapterId;
				Profile.CapabilityId = Chain.GetCapabilityId();
				Profile.CommandId = Profile.CapabilityId;
				Profile.OperationKind = Chain.GetOperationKind();
				Profile.EffectClass =
					Profile.OperationKind == TEXT("query")
						? TEXT("read_only")
						: IsSessionControl(Profile.CapabilityId)
							? TEXT("session_control")
							: TEXT("write");
				Profile.FamilyId = Chain.GetFamilyId();
				Profile.TypedOperationId = Chain.GetBindingId();
				Profile.ProviderBinding = Chain.GetBindingId();
				Profile.ContextSource = RequiresPublicContext(
					AdapterId,
					Profile.CapabilityId)
					? TEXT("public_required")
					: TEXT("immutable_application");
				Profile.OracleKind = Chain.GetOracleKind();
				Profile.ModelResultFields = ModelResultFields(Chain);
				Profile.bRequiresTarget = RequiresTarget(Profile.CapabilityId);
				Profile.bRequiresInput = HasRequiredInput(*Descriptor);
				Profile.bSupportsProjection = true;
				if (!TryPythonBinding(
						Profile.CapabilityId,
						Profile.Namespace,
						Profile.Method, OutError)
					|| !Profile.TryValidate(OutError))
				{
					return false;
				}
				OutProfiles.Add(MoveTemp(Profile));
			}
			return true;
		}

		bool TryPublicParameters(
			const FUeAgentTimelineSpatialActivationProfile& Profile,
			const FUeAgentOperationDescriptor& Descriptor,
			TArray<FUeAgentCommandParameter>& OutParameters,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TArray<FUeAgentCommandParameter> Parameters;
			if (Profile.bRequiresTarget)
			{
				Parameters.Emplace(
					TEXT("target"),
					EUeAgentCommandWireType::String,
					true,
					false);
			}
			Parameters.Emplace(
				TEXT("input"),
				EUeAgentCommandWireType::Object,
				Profile.bRequiresInput,
				false);
			Parameters.Emplace(
				TEXT("context"),
				EUeAgentCommandWireType::Object,
				Profile.ContextSource == TEXT("public_required"),
				false);
			if (Profile.bSupportsProjection)
			{
				Parameters.Emplace(
					TEXT("projection"),
					EUeAgentCommandWireType::Object,
					false,
					false);
			}
			OutParameters.Reset();
			for (const FUeAgentCommandParameter& Parameter : Parameters)
			{
				FString SchemaJson;
				if (!TryBuildTimelineSpatialPublicParameterSchema(
					Profile, Descriptor, Parameter.GetName(), SchemaJson, OutError)) return false;
				OutParameters.Emplace(Parameter.GetName(), Parameter.GetWireType(),
					Parameter.IsRequired(), Parameter.IsNullable(), MoveTemp(SchemaJson));
			}
			return true;
		}

		TSharedRef<FJsonObject> StringSchema(
			const bool bNonEmpty = false,
			const FString& Description = FString())
		{
			const TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("string"));
			if (bNonEmpty)
			{
				Schema->SetNumberField(TEXT("minLength"), 1);
			}
			if (!Description.IsEmpty())
			{
				Schema->SetStringField(TEXT("description"), Description);
			}
			return Schema;
		}

		TSharedRef<FJsonObject> ArraySchema(
			const TSharedRef<FJsonObject>& ItemSchema)
		{
			const TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("array"));
			Schema->SetObjectField(TEXT("items"), ItemSchema);
			return Schema;
		}

		TSharedRef<FJsonObject> ScalarSchema(const TCHAR* Type)
		{
			const TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), Type);
			return Schema;
		}

		FString FacetField(const EUeAgentExecutionContextFacet Facet)
		{
			switch (Facet)
			{
			case EUeAgentExecutionContextFacet::World:
				return TEXT("world");
			case EUeAgentExecutionContextFacet::Level:
				return TEXT("level");
			case EUeAgentExecutionContextFacet::DataLayer:
				return TEXT("data_layers");
			case EUeAgentExecutionContextFacet::WorldPartition:
				return TEXT("world_partition");
			case EUeAgentExecutionContextFacet::PieInstance:
				return TEXT("pie_instance");
			case EUeAgentExecutionContextFacet::NetMode:
				return TEXT("net_mode");
			case EUeAgentExecutionContextFacet::NetRole:
				return TEXT("net_role");
			case EUeAgentExecutionContextFacet::AssetEditor:
				return TEXT("asset_editor");
			case EUeAgentExecutionContextFacet::Viewport:
				return TEXT("viewport");
			case EUeAgentExecutionContextFacet::Selection:
				return TEXT("selection");
			default:
				return FString();
			}
		}

		bool IsServerResolvedFacet(const EUeAgentExecutionContextFacet Facet)
		{
			return Facet == EUeAgentExecutionContextFacet::World
				|| Facet == EUeAgentExecutionContextFacet::Level;
		}

		bool TryBuildContextSchema(
			const FUeAgentOperationDescriptor& Descriptor,
			FString& OutSchemaJson,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("object"));
			Schema->SetBoolField(TEXT("additionalProperties"), false);
			Schema->SetStringField(
				TEXT("description"),
				TEXT("Model-writable execution context. project_instance_id, editor_instance_id, context_revision, and resolved_from_current are server-owned and excluded."));

			const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			const TSharedRef<FJsonObject> StatePlane = StringSchema();
			TArray<TSharedPtr<FJsonValue>> StatePlaneValues;
			for (const EUeAgentStatePlane Plane :
				Descriptor.GetExecutionContextPolicy().GetAllowedStatePlanes())
			{
				StatePlaneValues.Add(MakeShared<FJsonValueString>(LexToString(Plane)));
			}
			StatePlane->SetArrayField(TEXT("enum"), MoveTemp(StatePlaneValues));
			Properties->SetObjectField(TEXT("state_plane"), StatePlane);

			const TSharedRef<FJsonObject> WorldType = StringSchema();
			WorldType->SetArrayField(TEXT("enum"), {
				MakeShared<FJsonValueString>(TEXT("editor")),
				MakeShared<FJsonValueString>(TEXT("pie")),
				MakeShared<FJsonValueString>(TEXT("simulate_in_editor")),
				MakeShared<FJsonValueString>(TEXT("game")),
				MakeShared<FJsonValueString>(TEXT("preview")),
			});
			Properties->SetObjectField(TEXT("world_type"), WorldType);
			Properties->SetObjectField(TEXT("world"), StringSchema());
			Properties->SetObjectField(TEXT("level"), StringSchema());
			Properties->SetObjectField(TEXT("world_partition"), StringSchema());
			Properties->SetObjectField(TEXT("net_mode"), StringSchema());
			Properties->SetObjectField(TEXT("net_role"), StringSchema());
			Properties->SetObjectField(TEXT("asset_editor"), StringSchema(true));
			Properties->SetObjectField(
				TEXT("viewport"),
				StringSchema(
					true,
					TEXT("Readable viewport_ref returned by viewport.instance.list; for viewport commands it must equal target.")));
			Properties->SetObjectField(TEXT("viewport_scope"), StringSchema());
			Properties->SetObjectField(TEXT("viewport_role"), StringSchema());
			Properties->SetObjectField(
				TEXT("data_layers"), ArraySchema(StringSchema(true)));
			Properties->SetObjectField(
				TEXT("selection"), ArraySchema(StringSchema(true)));
			Properties->SetObjectField(TEXT("pie_instance"), ScalarSchema(TEXT("integer")));
			Schema->SetObjectField(TEXT("properties"), Properties);

			TArray<TSharedPtr<FJsonValue>> Required;
			for (const EUeAgentExecutionContextFacet Facet :
				Descriptor.GetExecutionContextPolicy().GetRequiredFacets())
			{
				if (IsServerResolvedFacet(Facet))
				{
					continue;
				}
				const FString Field = FacetField(Facet);
				if (Field.IsEmpty())
				{
					OutError = RegistryFailure(TEXT("timeline_spatial_public_context_facet_unknown"),
						TEXT("A required execution-context facet has no public context field"), TEXT("execution_context.required_facets"),
						MakeShared<FJsonValueNumber>(static_cast<int32>(Facet)),
						MakeShared<FJsonValueString>(TEXT("facet with a public context field")));
					OutError.CandidateName = Descriptor.GetCapabilityId();
					return false;
				}
				Required.Add(MakeShared<FJsonValueString>(Field));
			}
			if (!Required.IsEmpty())
			{
				Schema->SetArrayField(TEXT("required"), MoveTemp(Required));
			}
			return Json::TryCanonicalize(Schema, OutSchemaJson, OutError);
		}

		bool TryBuildProjectionSchema(
			FString& OutSchemaJson,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("object"));
			Schema->SetBoolField(TEXT("additionalProperties"), false);
			const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			Properties->SetObjectField(TEXT("profile_id"), StringSchema(true));
			Properties->SetObjectField(TEXT("include_defaults"), ScalarSchema(TEXT("boolean")));
			Properties->SetObjectField(TEXT("include_details"), ScalarSchema(TEXT("boolean")));
			Properties->SetObjectField(TEXT("include_provenance"), ScalarSchema(TEXT("boolean")));
			Schema->SetObjectField(TEXT("properties"), Properties);
			return Json::TryCanonicalize(Schema, OutSchemaJson, OutError);
		}
	}

	bool FUeAgentTimelineSpatialActivationProfile::IsValid() const
	{
		FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentTimelineSpatialActivationProfile::TryValidate(FDiagnostic& OutError) const
	{
		OutError.Reset();
		auto FailField = [this, &OutError](const TCHAR* Field, const TCHAR* Reason,
			const FString& Actual, TSharedPtr<FJsonValue> Expected, bool bPublicActual = true)
		{
			OutError = FDiagnostic::Failure(TEXT("timeline_spatial_activation_profile_invalid"),
				FString::Printf(TEXT("The Timeline/Spatial profile for command '%s' %s"),
					*CommandId, Reason), TEXT("registry"), FString(), CommandId);
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = Field;
			if (bPublicActual) OutError.ActualValue = MakeShared<FJsonValueString>(Actual);
			else OutError.ActualEvidence = MakeShared<FJsonValueString>(Actual);
			OutError.ActualValuePresent = true;
			OutError.ExpectedValue = MoveTemp(Expected);
			return false;
		};
		auto Required = [&FailField](const TCHAR* Field, const TCHAR* Reason,
			const FString& Actual, bool bPublicActual = true)
		{
			return !Actual.IsEmpty() || FailField(Field, Reason, Actual,
				MakeShared<FJsonValueString>(TEXT("non-empty string")), bPublicActual);
		};
		if (!Required(TEXT("/adapter_id"), TEXT("requires a non-empty adapter identifier."), AdapterId, false)
			|| !Required(TEXT("/capability_id"), TEXT("requires a non-empty capability identifier."), CapabilityId))
			return false;
		if (CommandId != CapabilityId)
			return FailField(TEXT("/command_id"), TEXT("has a command identifier that differs from its capability."),
				CommandId, MakeShared<FJsonValueString>(CapabilityId));
		if (!IsIdentifier(Namespace))
			return FailField(TEXT("/namespace"), TEXT("requires a valid public namespace identifier."), Namespace,
				MakeShared<FJsonValueString>(TEXT("letter or underscore followed by letters, digits or underscores")));
		if (!IsIdentifier(Method))
			return FailField(TEXT("/method"), TEXT("requires a valid public method identifier."), Method,
				MakeShared<FJsonValueString>(TEXT("letter or underscore followed by letters, digits or underscores")));
		if (!DescriptorIdentity.IsValid())
		{
			OutError = FDiagnostic::WrapFailure(TEXT("timeline_spatial_activation_profile_invalid"),
				DescriptorIdentity.GetValidationDiagnostic(), TEXT("registry"));
			OutError.SubjectId = CommandId;
			OutError.PublicMessage = FString::Printf(
				TEXT("The Timeline/Spatial profile for command '%s' has an invalid descriptor identity: %s"),
				*CommandId, *OutError.PublicMessage);
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("/descriptor_identity") + OutError.PropertyPath;
			return false;
		}
		if (OperationKind != TEXT("query") && OperationKind != TEXT("operation"))
		{
			TArray<TSharedPtr<FJsonValue>> Allowed = { MakeShared<FJsonValueString>(TEXT("query")),
				MakeShared<FJsonValueString>(TEXT("operation")) };
			return FailField(TEXT("/operation_kind"), TEXT("has an unsupported operation kind."),
				OperationKind, MakeShared<FJsonValueArray>(MoveTemp(Allowed)));
		}
		if (EffectClass != TEXT("read_only") && EffectClass != TEXT("session_control") && EffectClass != TEXT("write"))
		{
			TArray<TSharedPtr<FJsonValue>> Allowed = { MakeShared<FJsonValueString>(TEXT("read_only")),
				MakeShared<FJsonValueString>(TEXT("session_control")), MakeShared<FJsonValueString>(TEXT("write")) };
			return FailField(TEXT("/effect_class"), TEXT("has an unsupported effect class."),
				EffectClass, MakeShared<FJsonValueArray>(MoveTemp(Allowed)));
		}
		if (!Required(TEXT("/family_id"), TEXT("requires a non-empty family identifier."), FamilyId, false)
			|| !Required(TEXT("/typed_operation_id"), TEXT("requires a non-empty typed operation identifier."), TypedOperationId, false)
			|| !Required(TEXT("/provider_binding"), TEXT("requires a non-empty provider binding."), ProviderBinding, false))
			return false;
		if (ContextSource != TEXT("public_required") && ContextSource != TEXT("immutable_application"))
		{
			TArray<TSharedPtr<FJsonValue>> Allowed = { MakeShared<FJsonValueString>(TEXT("public_required")),
				MakeShared<FJsonValueString>(TEXT("immutable_application")) };
			return FailField(TEXT("/context_source"), TEXT("has an unsupported context source."),
				ContextSource, MakeShared<FJsonValueArray>(MoveTemp(Allowed)));
		}
		return Required(TEXT("/oracle_kind"), TEXT("requires a non-empty oracle kind."), OracleKind, false);
	}

	bool TryBuildTimelineSpatialPublicParameterSchema(
		const FUeAgentTimelineSpatialActivationProfile& Profile,
		const FUeAgentOperationDescriptor& Descriptor,
		const FString& ParameterName,
		FString& OutSchemaJson,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutSchemaJson.Reset();
		OutError.Reset();
		if (Descriptor.GetCapabilityId() != Profile.CommandId)
		{
			OutError = RegistryFailure(TEXT("timeline_spatial_public_schema_descriptor_mismatch"),
				TEXT("The descriptor capability differs from the command requesting its parameter schema"), TEXT("capability_id"),
				MakeShared<FJsonValueString>(Descriptor.GetCapabilityId()), MakeShared<FJsonValueString>(Profile.CommandId));
			return false;
		}
		if (ParameterName == TEXT("input"))
		{
			OutSchemaJson = Descriptor.GetInputSchema().GetJsonValue();
			if (OutSchemaJson.IsEmpty())
			{
				OutError = RegistryFailure(TEXT("timeline_spatial_input_schema_missing"),
					TEXT("The operation descriptor has no input schema JSON"), TEXT("input_schema_present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = Profile.CommandId;
				return false;
			}
			return true;
		}
		if (ParameterName == TEXT("target"))
		{
			const TSharedRef<FJsonObject> Schema = StringSchema(
				true,
				TEXT("Exact readable viewport_ref/target or graph_ref. Viewport targets must equal the viewport_ref returned by viewport.instance.list."));
			return Json::TryCanonicalize(Schema, OutSchemaJson, OutError);
		}
		if (ParameterName == TEXT("context"))
		{
			return TryBuildContextSchema(Descriptor, OutSchemaJson, OutError);
		}
		if (ParameterName == TEXT("projection") && Profile.bSupportsProjection)
		{
			return TryBuildProjectionSchema(OutSchemaJson, OutError);
		}
		TArray<TSharedPtr<FJsonValue>> AllowedParameters{
			MakeShared<FJsonValueString>(TEXT("input")), MakeShared<FJsonValueString>(TEXT("target")),
			MakeShared<FJsonValueString>(TEXT("context"))};
		if (Profile.bSupportsProjection) AllowedParameters.Add(MakeShared<FJsonValueString>(TEXT("projection")));
		OutError = RegistryFailure(TEXT("timeline_spatial_public_parameter_schema_unknown"),
			TEXT("The command has no public schema for the requested parameter"), TEXT("parameter_name"),
			MakeShared<FJsonValueString>(ParameterName), MakeShared<FJsonValueArray>(AllowedParameters));
		OutError.CandidateName = Profile.CommandId;
		return false;
	}

	bool TryCreateTimelineSpatialDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TArray<FUeAgentOperationDescriptor> Merged;
		auto AppendRegistry = [&Merged, &OutError](
			const TCHAR* Label,
			const TFunction<bool(
				TSharedPtr<const FUeAgentDescriptorRegistry>&,
				UE::UAI::Contract::FDiagnostic& )>& Factory)
		{
			TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
			UE::UAI::Contract::FDiagnostic Error;
			const bool bCreated = Factory(Registry, Error);
			if (!bCreated || !Registry.IsValid() || Registry->GetDescriptors().IsEmpty())
			{
				OutError = MoveTemp(Error);
				if (OutError.IsEmpty())
				{
					OutError = RegistryFailure(TEXT("timeline_spatial_descriptor_source_invalid"),
						!bCreated ? TEXT("The domain descriptor factory failed without a diagnostic")
							: !Registry.IsValid() ? TEXT("The domain descriptor factory returned no registry")
							: TEXT("The domain descriptor registry contains no descriptors"),
						!bCreated ? TEXT("registry_factory_succeeded")
							: !Registry.IsValid() ? TEXT("registry_present") : TEXT("descriptors_present"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				}
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Label;
				return false;
			}
			Merged.Append(Registry->GetDescriptors());
			return true;
		};

		if (!AppendRegistry(
				TEXT("world_spatial"),
				DomainAdapters::WorldSpatial::
					TryCreateWorldSpatialDescriptorRegistry)
			|| !AppendRegistry(
				TEXT("timeline_animation"),
				DomainAdapters::TimelineAnimation::
					TryCreateTimelineAnimationDescriptorRegistry)
			|| !AppendRegistry(
				TEXT("rig_simulation_spatial"),
				DomainAdapters::RigSimulationSpatial::
					TryCreateRigSimulationSpatialDescriptorRegistry))
		{
			return false;
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Merged),
			OutRegistry,
			OutError);
	}

	bool TryGetTimelineSpatialActivationProfiles(
		const TArray<FUeAgentTimelineSpatialActivationProfile>*& OutProfiles, FDiagnostic& OutError)
	{
		struct FProfileCache
		{
			TArray<FUeAgentTimelineSpatialActivationProfile> Profiles;
			FDiagnostic Diagnostic;
		};
		static const FProfileCache Cache = []()
		{
			FProfileCache Result;
			auto& Profiles = Result.Profiles;
			TSharedPtr<const FUeAgentDescriptorRegistry> Descriptors;
			FDiagnostic& Error = Result.Diagnostic;
			const bool bCreated =
				TryCreateTimelineSpatialDescriptorRegistry(Descriptors, Error)
				&& Descriptors.IsValid()
				&& TryAppendProfiles(
					TEXT("world_spatial"),
					DomainAdapters::WorldSpatial::
						GetWorldSpatialExecutionChainContracts(),
					*Descriptors,
					Profiles,
					Error)
				&& TryAppendProfiles(
					TEXT("timeline_animation"),
					DomainAdapters::TimelineAnimation::
						GetTimelineAnimationExecutionChainContracts(),
					*Descriptors,
					Profiles,
					Error)
				&& TryAppendProfiles(
					TEXT("rig_simulation_spatial"),
					DomainAdapters::RigSimulationSpatial::
						GetRigSimulationSpatialExecutionChainContracts(),
						*Descriptors,
						Profiles,
						Error)
				&& Profiles.Num() == Descriptors->GetDescriptors().Num();
			if (!bCreated)
			{
				if (Error.IsEmpty())
				{
					Error = RegistryFailure(TEXT("timeline_spatial_activation_profiles_invalid"),
						Descriptors.IsValid() ? TEXT("The activation profile count differs from the descriptor count")
							: TEXT("The descriptor factory returned no registry for activation profiles"),
						Descriptors.IsValid() ? TEXT("activation_profile_count") : TEXT("descriptor_registry_present"),
						Descriptors.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Profiles.Num()))
							: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
						Descriptors.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Descriptors->GetDescriptors().Num()))
							: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)));
				}
				Profiles.Reset();
			}
			return Result;
		}();
		OutProfiles = nullptr;
		OutError = Cache.Diagnostic;
		if (!OutError.IsEmpty()) return false;
		OutProfiles = &Cache.Profiles;
		return true;
	}

	bool TryCreateTimelineSpatialProductionCommandContractRegistry(
		TSharedPtr<const FUeAgentCommandContractRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> Base;
		if (!SimpleAuthoring::TryCreateProductionCommandContractRegistry(
				Base,
				OutError)
			|| !Base.IsValid())
		{
			if (OutError.IsEmpty()) OutError = RegistryFailure(TEXT("timeline_spatial_base_contract_registry_missing"),
				TEXT("The base command contract factory returned no usable registry"), TEXT("base_registry_present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		const TArray<FUeAgentTimelineSpatialActivationProfile>* Profiles = nullptr;
		if (!TryGetTimelineSpatialActivationProfiles(Profiles, OutError)) return false;

		TArray<FUeAgentCommandContract> Contracts = Base->GetContracts();
		TSharedPtr<const FUeAgentDescriptorRegistry> Descriptors;
		if (!TryCreateTimelineSpatialDescriptorRegistry(Descriptors, OutError)) return false;
		for (const FUeAgentTimelineSpatialActivationProfile& Profile
			: *Profiles)
		{
			const FUeAgentOperationDescriptor* Descriptor = Descriptors->FindExact(Profile.DescriptorIdentity);
			TArray<FUeAgentCommandParameter> Parameters;
			if (!Descriptor)
			{
				OutError = RegistryFailure(TEXT("timeline_spatial_public_schema_descriptor_missing"),
					TEXT("No operation descriptor matches the activation profile"), TEXT("descriptor_present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = Profile.CommandId;
				OutError.SubjectId = Profile.DescriptorIdentity.GetDescriptorId();
				return false;
			}
			if (!TryPublicParameters(Profile, *Descriptor, Parameters, OutError)) return false;
			Contracts.Emplace(
				Profile.CommandId,
				Profile.Namespace,
				Profile.Method,
				MoveTemp(Parameters),
				1,
				1,
				EUeAgentCommandStatus::Active);
		}

		return FUeAgentCommandContractRegistry::TryCreate(
			MoveTemp(Contracts),
			OutRegistry,
			OutError);
	}
}
