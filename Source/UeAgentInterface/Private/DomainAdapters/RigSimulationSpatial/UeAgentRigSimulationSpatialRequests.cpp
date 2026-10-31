#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialRequests.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	bool FRigSimulationSpatialSimulationSession::TryValidate(FDiagnostic& OutError) const
	{
		OutError.Reset();
		const FString Field = SessionId.IsEmpty() ? TEXT("session_id")
			: OwnerRef.IsEmpty() ? TEXT("owner_ref") : Generation.IsEmpty() ? TEXT("generation") : FString();
		if (Field.IsEmpty()) return true;
		OutError = FDiagnostic::Failure(TEXT("rig_simulation_spatial_session_invalid"),
			TEXT("The simulation session requires a nonempty ") + Field + TEXT("."), TEXT("validation"));
		OutError.PropertyPath = TEXT("/") + Field;
		OutError.Source.JsonPath = OutError.PropertyPath;
		OutError.TargetPath = OwnerRef;
		OutError.ActualValuePresent = true;
		OutError.ActualValue = MakeShared<FJsonValueString>(FString());
		OutError.ExpectedValuePresent = true;
		OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty string"));
		return false;
	}


	namespace
	{
		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
            return false;
        }

		struct FRequestSpec
		{
			const TCHAR* CapabilityId;
			const TCHAR* TypeId;
			const TCHAR* RequestKindId;
			const TCHAR* FamilyId;
			bool bOperation;
			bool bExpectedRevision;
			bool bCreateTarget;
			TArray<FString> IdentityFields;
			TArray<FString> RequiredFields;
			TArray<FString> OptionalFields;
		};

		const TArray<FRequestSpec>& RequestSpecs()
		{
			static const TArray<FRequestSpec> Specs = {
				{TEXT("rig.control.bake.context"), TEXT("rig_simulation_spatial.control_rig.bake_context.request.v1"), TEXT("control_rig.bake_context.request.v1"), TEXT("rig_bake"), false, false, false, {TEXT("sequence_ref"), TEXT("rig_ref")}, {TEXT("sequence_ref"), TEXT("binding_name"), TEXT("rig_ref")}, {TEXT("target_animation_ref")}},
				{TEXT("rig.skeleton.preview_mesh.bind"), TEXT("rig_simulation_spatial.skeleton.bind_preview_mesh.request.v1"), TEXT("skeleton.bind_preview_mesh.request.v1"), TEXT("skeletal_attachment"), true, true, false, {TEXT("skeleton_ref"), TEXT("preview_mesh_ref")}, {TEXT("skeleton_ref"), TEXT("preview_mesh_ref"), TEXT("expected_revision")}, {}},
				{TEXT("rig.skeleton.socket.update"), TEXT("rig_simulation_spatial.skeleton.update_socket.request.v1"), TEXT("skeleton.update_socket.request.v1"), TEXT("skeletal_attachment"), true, true, false, {TEXT("skeleton_ref")}, {TEXT("skeleton_ref"), TEXT("operation"), TEXT("socket_ref"), TEXT("parent_bone_ref"), TEXT("socket_transform"), TEXT("expected_revision")}, {}},
				{TEXT("rig.control.bake.animation"), TEXT("rig_simulation_spatial.control_rig.bake_animation.request.v1"), TEXT("control_rig.bake_animation.request.v1"), TEXT("rig_bake"), true, true, false, {TEXT("source_sequence_ref"), TEXT("target_animation_ref"), TEXT("binding_ref"), TEXT("rig_ref")}, {TEXT("source_sequence_ref"), TEXT("target_animation_ref"), TEXT("binding_ref"), TEXT("rig_ref"), TEXT("authored_range"), TEXT("sample_rate"), TEXT("expected_revision")}, {}},
				{TEXT("rig.control.bake.controls"), TEXT("rig_simulation_spatial.control_rig.bake_controls.request.v1"), TEXT("control_rig.bake_controls.request.v1"), TEXT("rig_bake"), true, true, false, {TEXT("target_sequence_ref"), TEXT("binding_ref"), TEXT("rig_ref")}, {TEXT("target_sequence_ref"), TEXT("binding_ref"), TEXT("rig_ref"), TEXT("authored_range"), TEXT("sample_rate"), TEXT("expected_revision")}, {}},
				{TEXT("rig.ik.definition.update"), TEXT("rig_simulation_spatial.ik.update_definition.request.v1"), TEXT("ik.update_definition.request.v1"), TEXT("ik_retarget"), true, true, false, {TEXT("ik_rig_ref")}, {TEXT("ik_rig_ref"), TEXT("definition_changes"), TEXT("expected_revision")}, {TEXT("preview_context")}},
				{TEXT("rig.retarget.definition.update"), TEXT("rig_simulation_spatial.retarget.update_definition.request.v1"), TEXT("retarget.update_definition.request.v1"), TEXT("ik_retarget"), true, true, false, {TEXT("retargeter_ref"), TEXT("source_rig_ref"), TEXT("target_rig_ref")}, {TEXT("retargeter_ref"), TEXT("source_rig_ref"), TEXT("target_rig_ref"), TEXT("chain_mappings"), TEXT("expected_revision")}, {}},
				{TEXT("rig.retarget.pose.update"), TEXT("rig_simulation_spatial.retarget.update_pose.request.v1"), TEXT("retarget.update_pose.request.v1"), TEXT("ik_retarget"), true, true, false, {TEXT("retargeter_ref")}, {TEXT("retargeter_ref"), TEXT("subject_side"), TEXT("pose"), TEXT("pose_frame"), TEXT("expected_revision")}, {}},
				{TEXT("rig.retarget.pose.auto_align"), TEXT("rig_simulation_spatial.retarget.auto_align_pose.request.v1"), TEXT("retarget.auto_align_pose.request.v1"), TEXT("ik_retarget"), true, true, false, {TEXT("retargeter_ref")}, {TEXT("retargeter_ref"), TEXT("subject_side"), TEXT("alignment_policy"), TEXT("expected_revision")}, {TEXT("bone_refs")}},
				{TEXT("rig.retarget.assets.generate"), TEXT("rig_simulation_spatial.retarget.generate_assets.request.v1"), TEXT("retarget.generate_assets.request.v1"), TEXT("ik_retarget"), true, false, true, {TEXT("source_subject_ref"), TEXT("target_subject_ref"), TEXT("retargeter_ref"), TEXT("destination_root")}, {TEXT("source_subject_ref"), TEXT("target_subject_ref"), TEXT("retargeter_ref"), TEXT("destination_root"), TEXT("asset_requests")}, {}},
				{TEXT("physics.material.update"), TEXT("rig_simulation_spatial.physics.update_material.request.v1"), TEXT("physics.update_material.request.v1"), TEXT("simulation_definition"), true, true, false, {TEXT("physical_material_ref")}, {TEXT("physical_material_ref"), TEXT("material_state"), TEXT("expected_revision")}, {}},
				{TEXT("physics.asset.structure.replace"), TEXT("rig_simulation_spatial.physics.replace_asset_structure.request.v1"), TEXT("physics.replace_asset_structure.request.v1"), TEXT("simulation_definition"), true, true, false, {TEXT("physics_asset_ref")}, {TEXT("physics_asset_ref"), TEXT("bodies"), TEXT("constraints"), TEXT("profiles"), TEXT("expected_revision")}, {}},
				{TEXT("physics.constraint.update"), TEXT("rig_simulation_spatial.physics.update_constraint.request.v1"), TEXT("physics.update_constraint.request.v1"), TEXT("simulation_definition"), true, true, false, {TEXT("physics_asset_ref")}, {TEXT("physics_asset_ref"), TEXT("constraint_ref"), TEXT("endpoint_a"), TEXT("endpoint_b"), TEXT("constraint_state"), TEXT("expected_revision")}, {}},
				{TEXT("physics.simulation.probe"), TEXT("rig_simulation_spatial.physics.probe_simulation.request.v1"), TEXT("physics.probe_simulation.request.v1"), TEXT("simulation_observation"), true, false, false, {TEXT("subject_refs")}, {TEXT("subject_refs"), TEXT("simulation_session"), TEXT("simulation_range")}, {TEXT("impulses"), TEXT("velocities")}},
				{TEXT("deformer.geometry.sample"), TEXT("rig_simulation_spatial.deformer.sample_geometry.request.v1"), TEXT("deformer.sample_geometry.request.v1"), TEXT("simulation_observation"), false, false, false, {TEXT("geometry_cache_ref")}, {TEXT("geometry_cache_ref"), TEXT("sample_time"), TEXT("output_frame")}, {TEXT("vertex_selector")}},
				{TEXT("deformer.training_inputs.validate"), TEXT("rig_simulation_spatial.deformer.validate_training_inputs.request.v1"), TEXT("deformer.validate_training_inputs.request.v1"), TEXT("simulation_observation"), false, false, false, {TEXT("training_input_refs")}, {TEXT("training_input_refs"), TEXT("temporal_ranges"), TEXT("frame_mappings")}, {}},
				{TEXT("deformer.evaluation.sample"), TEXT("rig_simulation_spatial.deformer.sample_evaluation.request.v1"), TEXT("deformer.sample_evaluation.request.v1"), TEXT("simulation_observation"), false, false, false, {TEXT("deformer_ref"), TEXT("subject_ref")}, {TEXT("deformer_ref"), TEXT("subject_ref"), TEXT("sample_time"), TEXT("output_frame")}, {TEXT("vertex_selector")}},
			};
			return Specs;
		}

		const FRequestSpec* FindSpec(const FString& CapabilityId)
		{
			return RequestSpecs().FindByPredicate(
				[&CapabilityId](const FRequestSpec& Spec)
				{
					return CapabilityId == Spec.CapabilityId;
				});
		}

		bool ValidateFields(
			const TSharedPtr<FJsonObject>& Object,
			const TArray<FString>& Required,
			const TArray<FString>& Optional,
			const FString& Subject,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			const auto Locate = [&](FDiagnostic Diagnostic)
			{
				Diagnostic.SourceKind = TEXT("canonical_input");
				Diagnostic.Source.JsonPath = TEXT("/") + Diagnostic.PropertyPath.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
				return Fail(Error, MoveTemp(Diagnostic), Subject);
			};
			if (!Object.IsValid())
			{
				return Locate(UE::UAI::Contract::FDiagnostic::Failure(TEXT("rig_simulation_spatial_object_invalid"),
					TEXT("A structured input object is required."), TEXT("native_execution")));
			}
			for (const FString& Field : Required)
			{
				if (!Object->HasField(Field))
				{
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Field.EndsWith(TEXT("_ref")) || Field.EndsWith(TEXT("_refs"))
							? TEXT("rig_simulation_spatial_required_identity_missing")
							: Field == TEXT("expected_revision")
								? TEXT("rig_simulation_spatial_expected_revision_missing")
								: TEXT("rig_simulation_spatial_required_field_missing"),
						TEXT("A required input field is missing."), TEXT("native_execution"));
					Diagnostic.PropertyPath = Field;
					Diagnostic.ActualValuePresent = false;
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("required field"));
					return Locate(MoveTemp(Diagnostic));
				}
			}
			TSet<FString> Allowed;
			for (const FString& Field : Required) Allowed.Add(Field);
			for (const FString& Field : Optional) Allowed.Add(Field);
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				if (!Allowed.Contains(Pair.Key))
				{
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("rig_simulation_spatial_unknown_field"),
						TEXT("This field is not supported by the input schema."), TEXT("native_execution"));
					Diagnostic.PropertyPath = Pair.Key;
					Diagnostic.ActualValue = MakeShared<FJsonValueString>(Pair.Key);
					Diagnostic.ActualValuePresent = true;
					TArray<TSharedPtr<FJsonValue>> Expected;
					for (const FString& Field : Required) Expected.Add(MakeShared<FJsonValueString>(Field));
					for (const FString& Field : Optional) Expected.Add(MakeShared<FJsonValueString>(Field));
					Diagnostic.ExpectedValue = MakeShared<FJsonValueArray>(MoveTemp(Expected));
					return Locate(MoveTemp(Diagnostic));
				}
			}
			return true;
		}

		bool RequiredString(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			FString& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error,
			bool bPublicValue = true)
		{
			Out.Reset();
			const auto Reject = [&]()
			{
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(FString(Field).EndsWith(TEXT("_ref"))
						? TEXT("rig_simulation_spatial_required_identity_missing")
						: TEXT("rig_simulation_spatial_required_string_missing"),
					TEXT("The required input value must be a non-empty string."), TEXT("native_execution"));
				Diagnostic.PropertyPath = Field;
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty string"));
				const TSharedPtr<FJsonValue> Actual = Object.IsValid() ? Object->TryGetField(Field) : nullptr;
				Diagnostic.ActualValuePresent = Actual.IsValid();
				if (bPublicValue) Diagnostic.ActualValue = Actual;
				else Diagnostic.ActualEvidence = Actual;
				Diagnostic.SourceKind = TEXT("canonical_input");
				Diagnostic.Source.JsonPath = TEXT("/") + FString(Field);
				return Fail(Error, MoveTemp(Diagnostic), Field);
			};
			if (!Object.IsValid() || !Object->TryGetStringField(Field, Out)) return Reject();
			Out.TrimStartAndEndInline();
			return !Out.IsEmpty() || Reject();
		}

		bool OptionalString(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			FString& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out.Reset();
			if (!Object->HasField(Field)) return true;
			return RequiredString(Object, Field, Out, Error);
		}

		bool RequiredObject(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TSharedPtr<FJsonObject>& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out.Reset();
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (!Object.IsValid() || !Object->TryGetObjectField(Field, Found)
				|| !Found || !Found->IsValid())
			{
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Field == FString(TEXT("expected_revision"))
						? TEXT("rig_simulation_spatial_expected_revision_missing")
						: TEXT("rig_simulation_spatial_required_object_missing"),
					TEXT("The required input value must be an object."), TEXT("native_execution"));
				Diagnostic.PropertyPath = Field;
				Diagnostic.ActualEvidence = Object.IsValid() ? Object->TryGetField(Field) : nullptr;
				Diagnostic.ActualValuePresent = Diagnostic.ActualEvidence.IsValid();
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
				Diagnostic.SourceKind = TEXT("canonical_input");
				Diagnostic.Source.JsonPath = TEXT("/") + FString(Field);
				return Fail(Error, MoveTemp(Diagnostic), Field);
			}
			Out = *Found;
			return true;
		}

		bool OptionalObject(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TSharedPtr<FJsonObject>& Out,
			bool& bPresent,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out.Reset();
			bPresent = Object.IsValid() && Object->HasField(Field);
			return !bPresent || RequiredObject(Object, Field, Out, Error);
		}

		bool StringArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TArray<FString>& Out,
			bool bRequired,
			FUeAgentRigSimulationSpatialExecutionError& Error,
			bool bPublicValue = true)
		{
			Out.Reset();
			const FString Pointer = TEXT("/") + FString(Field).Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
			const auto Reject = [&](const FString& Code, const FString& Reason,
				const TSharedPtr<FJsonValue>& Actual, const FString& Expected, int32 Index = INDEX_NONE)
			{
				auto Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
				Diagnostic.PropertyPath = FString(Field) + (Index == INDEX_NONE ? FString() : FString::Printf(TEXT("[%d]"), Index));
				Diagnostic.SourceKind = TEXT("canonical_input");
				Diagnostic.Source.JsonPath = Pointer + (Index == INDEX_NONE ? FString() : FString::Printf(TEXT("/%d"), Index));
				Diagnostic.ActualValuePresent = Actual.IsValid();
				if (bPublicValue) Diagnostic.ActualValue = Actual;
				else Diagnostic.ActualEvidence = Actual;
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Expected);
				return Fail(Error, MoveTemp(Diagnostic), Field);
			};
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object->TryGetArrayField(Field, Values) || !Values)
			{
				return !bRequired || Reject(FString(Field).EndsWith(TEXT("_refs"))
						? TEXT("rig_simulation_spatial_required_identity_missing")
						: TEXT("rig_simulation_spatial_required_array_missing"),
					TEXT("The required input value must be an array of strings."), Object->TryGetField(Field), TEXT("array of strings"));
			}
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
				FString Text;
				if (!Value.IsValid() || !Value->TryGetString(Text))
				{
					return Reject(TEXT("rig_simulation_spatial_string_array_invalid"),
						TEXT("This array element must be a string."), Value, TEXT("string"), Index);
				}
				Text.TrimStartAndEndInline();
				if (Text.IsEmpty()) return Reject(TEXT("rig_simulation_spatial_string_array_invalid"),
					TEXT("This array element must not be empty or whitespace."), Value, TEXT("non-empty string"), Index);
				Out.Add(MoveTemp(Text));
			}
			return (!bRequired || !Out.IsEmpty()) || Reject(TEXT("rig_simulation_spatial_required_array_empty"),
				TEXT("The required array must contain at least one element."), Object->TryGetField(Field), TEXT("non-empty array"));
		}

		FString InputPointerToken(const FString& Field)
		{
			return Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		FDiagnostic InputValueFailure(const FString& Code, const FString& Reason, const FString& Field,
			const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected, bool bPublicValue = true)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			Error.PropertyPath = Field;
			Error.SourceKind = TEXT("canonical_input");
			Error.Source.JsonPath = TEXT("/") + InputPointerToken(Field);
			Error.ActualValuePresent = Actual.IsValid();
			if (bPublicValue) Error.ActualValue = Actual;
			else Error.ActualEvidence = Actual;
			Error.ExpectedValue = Expected;
			return Error;
		}

		bool RequireStateChanges(const TSharedPtr<FJsonObject>& Object,
			const FString& Subject, FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			return !Object->Values.IsEmpty() || Fail(Error,
				InputValueFailure(TEXT("rig_simulation_spatial_state_empty"),
					TEXT("The state object must specify at least one property to change."), FString(),
					MakeShared<FJsonValueObject>(Object), MakeShared<FJsonValueString>(TEXT("non-empty state object"))), Subject);
		}

		bool ValidateTemporalState(const TSharedPtr<FJsonObject>& Object, const FString& State,
			const FString& Code, FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			if (State == TEXT("authored") || State == TEXT("evaluation") || State == TEXT("simulation")) return true;
			TArray<TSharedPtr<FJsonValue>> Expected;
			for (const TCHAR* Value : {TEXT("authored"), TEXT("evaluation"), TEXT("simulation")})
				Expected.Add(MakeShared<FJsonValueString>(Value));
			return Fail(Error, InputValueFailure(Code, TEXT("The temporal state is not supported."), TEXT("state"),
				Object->TryGetField(TEXT("state")), MakeShared<FJsonValueArray>(MoveTemp(Expected))));
		}

		void PrefixInputDiagnostic(FDiagnostic& Error, const FString& Prefix)
		{
			if (Error.SourceKind != TEXT("canonical_input")) return;
			Error.Source.JsonPath = Prefix + (Error.Source.JsonPath == TEXT("/") ? FString() : Error.Source.JsonPath);
		}

		void LocateDecodedInput(FDiagnostic& Error, const FString& InputRootPointer)
		{
			if (Error.SourceKind == TEXT("canonical_input") || Error.SourceKind == TEXT("canonical_related_input"))
			{
				const bool bRelated = Error.SourceKind == TEXT("canonical_related_input");
				Error.Source.JsonPath = (InputRootPointer == TEXT("/") ? FString() : InputRootPointer)
					+ (Error.Source.JsonPath == TEXT("/") ? FString() : Error.Source.JsonPath);
				if (Error.Source.JsonPath.IsEmpty()) Error.Source.JsonPath = TEXT("/");
				Error.SourceKind = bRelated ? TEXT("related_input") : TEXT("command_input");
			}
			for (auto& Child : Error.Diagnostics) LocateDecodedInput(Child, InputRootPointer);
			if (Error.PublicCause)
			{
				auto Cause = MakeShared<FDiagnostic>(*Error.PublicCause);
				LocateDecodedInput(*Cause, InputRootPointer);
				Error.PublicCause = MoveTemp(Cause);
			}
		}

		template <typename ItemType, typename ParserType>
		bool ParseAt(const TSharedPtr<FJsonObject>& Object, ItemType& Out, ParserType Parser,
			FUeAgentRigSimulationSpatialExecutionError& Error, const FString& Pointer)
		{
			if (Parser(Object, Out, Error)) return true;
			PrefixInputDiagnostic(Error, Pointer);
			return false;
		}

		template <typename ItemType, typename ParserType>
		bool ObjectArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TArray<ItemType>& Out,
			bool bRequired,
			ParserType Parser,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			const FString ArrayPointer = TEXT("/") + InputPointerToken(Field);
			const auto RejectArray = [&](const TCHAR* Code, const TCHAR* Reason, const FString& Pointer,
				const TCHAR* Expected, const TSharedPtr<FJsonValue>& Actual)
			{
				Error = FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
				Error.PropertyPath = Pointer;
				Error.SourceKind = TEXT("canonical_input");
				Error.Source.JsonPath = Pointer;
				Error.ActualValuePresent = Actual.IsValid();
				Error.ActualEvidence = Actual;
				Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
				return false;
			};
			if (!Object->TryGetArrayField(Field, Values) || !Values)
			{
				return !bRequired || RejectArray(TEXT("rig_simulation_spatial_required_array_missing"),
					TEXT("The required input must be an array of objects."), ArrayPointer, TEXT("non-empty object array"), Object->TryGetField(Field));
			}
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
				const TSharedPtr<FJsonObject> ItemObject =
					Value.IsValid() ? Value->AsObject() : nullptr;
				ItemType Item;
				const FString ItemPointer = ArrayPointer + FString::Printf(TEXT("/%d"), Index);
				if (!ItemObject.IsValid()) return RejectArray(TEXT("rig_simulation_spatial_array_item_invalid"),
					TEXT("This array element must be an object."), ItemPointer, TEXT("object"), Value);
				if (!ParseAt(ItemObject, Item, Parser, Error, ItemPointer)) return false;
				Out.Add(MoveTemp(Item));
			}
			return (!bRequired || !Out.IsEmpty()) || RejectArray(TEXT("rig_simulation_spatial_required_array_empty"),
				TEXT("The required object array is empty."), ArrayPointer, TEXT("at least one object"), Object->TryGetField(Field));
		}

		bool OptionalBool(const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field, TOptional<bool>& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out.Reset();
			if (!Object->HasField(Field)) return true;
			bool Value = false;
			if (!Object->TryGetBoolField(Field, Value))
				return Fail(Error, InputValueFailure(TEXT("rig_simulation_spatial_boolean_invalid"),
					TEXT("The input value must be a boolean."), Field, Object->TryGetField(Field),
					MakeShared<FJsonValueString>(TEXT("boolean"))), Field);
			Out = Value;
			return true;
		}

		bool ParseRevision(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialRevision& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out = {};
			FString Sequence;
			if (!ValidateFields(Object,
				{TEXT("source_id"), TEXT("generation"), TEXT("sequence")}, {},
				TEXT("expected_revision"), Error)
				|| !RequiredString(Object, TEXT("source_id"), Out.SourceId, Error, false)
				|| !RequiredString(Object, TEXT("generation"), Out.Generation, Error, false)
				|| !RequiredString(Object, TEXT("sequence"), Sequence, Error, false)) return false;
			if (!LexTryParseString(Out.Sequence, *Sequence) || Out.Sequence < 0)
				return Fail(Error, InputValueFailure(TEXT("rig_simulation_spatial_revision_invalid"),
					TEXT("The revision sequence must parse as a non-negative integer."), TEXT("sequence"),
					Object->TryGetField(TEXT("sequence")), MakeShared<FJsonValueString>(TEXT("non-negative integer string")), false));
			return true;
		}

		bool ParseRate(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialRate& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out = {};
			if (!ValidateFields(Object,
				{TEXT("numerator"), TEXT("denominator")}, {}, TEXT("rate"), Error)
				|| !RequiredString(Object, TEXT("numerator"), Out.Numerator, Error)
				|| !RequiredString(Object, TEXT("denominator"), Out.Denominator, Error)) return false;
			int64 Numerator = 0;
			int64 Denominator = 0;
			const TCHAR* InvalidField = !LexTryParseString(Numerator, *Out.Numerator) ? TEXT("numerator")
				: !LexTryParseString(Denominator, *Out.Denominator) ? TEXT("denominator")
				: Numerator <= 0 ? TEXT("numerator") : Denominator <= 0 ? TEXT("denominator") : nullptr;
			if (InvalidField)
				return Fail(Error, InputValueFailure(TEXT("rig_simulation_spatial_rate_invalid"),
					TEXT("The rate component must parse as a positive integer."), InvalidField,
					Object->TryGetField(InvalidField), MakeShared<FJsonValueString>(TEXT("positive integer string"))));
			return true;
		}

		bool ParsePoint(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialPoint& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out = {};
			TSharedPtr<FJsonObject> Rate;
			return ValidateFields(Object,
				{TEXT("clock_id"), TEXT("tick"), TEXT("rate"), TEXT("state")}, {},
				TEXT("temporal_point"), Error)
				&& RequiredString(Object, TEXT("clock_id"), Out.ClockId, Error)
				&& RequiredString(Object, TEXT("tick"), Out.Tick, Error)
				&& RequiredObject(Object, TEXT("rate"), Rate, Error)
				&& ParseAt(Rate, Out.Rate, ParseRate, Error, TEXT("/rate"))
				&& RequiredString(Object, TEXT("state"), Out.State, Error)
				&& ValidateTemporalState(Object, Out.State, TEXT("rig_simulation_spatial_temporal_point_invalid"), Error);
		}

		bool ParseRange(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialRange& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out = {};
			TSharedPtr<FJsonObject> Rate;
			return ValidateFields(Object,
				{TEXT("clock_id"), TEXT("lower_tick"), TEXT("upper_tick"), TEXT("rate"), TEXT("state")}, {},
				TEXT("temporal_range"), Error)
				&& RequiredString(Object, TEXT("clock_id"), Out.ClockId, Error)
				&& RequiredString(Object, TEXT("lower_tick"), Out.LowerTick, Error)
				&& RequiredString(Object, TEXT("upper_tick"), Out.UpperTick, Error)
				&& RequiredObject(Object, TEXT("rate"), Rate, Error)
				&& ParseAt(Rate, Out.Rate, ParseRate, Error, TEXT("/rate"))
				&& RequiredString(Object, TEXT("state"), Out.State, Error)
				&& ValidateTemporalState(Object, Out.State, TEXT("rig_simulation_spatial_temporal_range_invalid"), Error);
		}

		bool ParseFrame(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialFrame3d& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out = {};
			double Dimension = 0.0;
			if (!ValidateFields(Object,
				{TEXT("frame_id"), TEXT("basis_frame_id"), TEXT("dimension"), TEXT("unit"), TEXT("basis_evidence_id")}, {},
				TEXT("frame_3d"), Error))
			{
				return false;
			}
			if (!RequiredString(Object, TEXT("frame_id"), Out.FrameId, Error)
				|| !RequiredString(Object, TEXT("basis_frame_id"), Out.BasisFrameId, Error)) return false;
			const bool bHasDimension = Object->TryGetNumberField(TEXT("dimension"), Dimension);
			if (!bHasDimension || (Out.Dimension = static_cast<int32>(Dimension)) != 3)
			{
				Error = FDiagnostic::Failure(TEXT("rig_simulation_spatial_frame_invalid"),
					bHasDimension ? TEXT("The frame dimension must resolve to 3.") : TEXT("The frame dimension must be numeric."), TEXT("native_execution"));
				Error.PropertyPath = TEXT("dimension");
				Error.ActualValue = Object->TryGetField(TEXT("dimension"));
				Error.ActualValuePresent = Error.ActualValue.IsValid();
				Error.ExpectedValue = MakeShared<FJsonValueNumber>(3);
				Error.SourceKind = TEXT("canonical_input");
				Error.Source.JsonPath = TEXT("/dimension");
				return false;
			}
			return RequiredString(Object, TEXT("unit"), Out.Unit, Error)
				&& RequiredString(Object, TEXT("basis_evidence_id"), Out.BasisEvidenceId, Error, false);
		}

		bool ParseTransform(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialTransform3d& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out = {};
			TSharedPtr<FJsonObject> Frame;
			if (!ValidateFields(Object,
				{TEXT("subject"), TEXT("quantity"), TEXT("frame"), TEXT("components"), TEXT("conversion_evidence_ids")}, {},
				TEXT("transform_3d"), Error)
				|| !RequiredString(Object, TEXT("subject"), Out.Subject, Error)
				|| !RequiredString(Object, TEXT("quantity"), Out.Quantity, Error)
				|| !RequiredObject(Object, TEXT("frame"), Frame, Error)
				|| !ParseAt(Frame, Out.Frame, ParseFrame, Error, TEXT("/frame"))
				|| !StringArray(Object, TEXT("components"), Out.Components, true, Error)
				|| !StringArray(Object, TEXT("conversion_evidence_ids"),
					Out.ConversionEvidenceIds, true, Error, false)) return false;
			if (Out.Quantity != TEXT("transform"))
				return Fail(Error, InputValueFailure(TEXT("rig_simulation_spatial_transform_invalid"),
					TEXT("The spatial quantity must be transform."), TEXT("quantity"), Object->TryGetField(TEXT("quantity")),
					MakeShared<FJsonValueString>(TEXT("transform"))));
			if (Out.Components.Num() != 16)
				return Fail(Error, InputValueFailure(TEXT("rig_simulation_spatial_transform_invalid"),
					TEXT("The transform must contain exactly 16 components."), TEXT("components"),
					MakeShared<FJsonValueNumber>(Out.Components.Num()), MakeShared<FJsonValueNumber>(16)));
			return true;
		}

		bool ParseIkChange(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialIkDefinitionChange& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			return ValidateFields(Object,
				{TEXT("kind"), TEXT("operation"), TEXT("primary_ref")},
				{TEXT("secondary_ref"), TEXT("tertiary_ref"), TEXT("solver_kind")},
				TEXT("ik_definition_change"), Error)
				&& RequiredString(Object, TEXT("kind"), Out.Kind, Error)
				&& RequiredString(Object, TEXT("operation"), Out.Operation, Error)
				&& RequiredString(Object, TEXT("primary_ref"), Out.PrimaryRef, Error)
				&& OptionalString(Object, TEXT("secondary_ref"), Out.SecondaryRef, Error)
				&& OptionalString(Object, TEXT("tertiary_ref"), Out.TertiaryRef, Error)
				&& OptionalString(Object, TEXT("solver_kind"), Out.SolverKind, Error);
		}

		bool ParsePoseItem(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialPoseItem& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Transform;
			return ValidateFields(Object, {TEXT("bone_ref"), TEXT("transform")}, {},
				TEXT("pose_item"), Error)
				&& RequiredString(Object, TEXT("bone_ref"), Out.BoneRef, Error)
				&& RequiredObject(Object, TEXT("transform"), Transform, Error)
				&& ParseAt(Transform, Out.Transform, ParseTransform, Error, TEXT("/transform"));
		}

		bool ParseChainMapping(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialChainMapping& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			return ValidateFields(Object,
				{TEXT("source_chain_ref"), TEXT("target_chain_ref")}, {TEXT("goal_ref")},
				TEXT("chain_mapping"), Error)
				&& RequiredString(Object, TEXT("source_chain_ref"), Out.SourceChainRef, Error)
				&& RequiredString(Object, TEXT("target_chain_ref"), Out.TargetChainRef, Error)
				&& OptionalString(Object, TEXT("goal_ref"), Out.GoalRef, Error);
		}

		bool ParseAssetRequest(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialAssetRequest& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			return ValidateFields(Object,
				{TEXT("asset_kind"), TEXT("name"), TEXT("overwrite_policy")}, {},
				TEXT("asset_request"), Error)
				&& RequiredString(Object, TEXT("asset_kind"), Out.AssetKind, Error)
				&& RequiredString(Object, TEXT("name"), Out.Name, Error)
				&& RequiredString(Object, TEXT("overwrite_policy"), Out.OverwritePolicy, Error);
		}

		bool ParseMaterialState(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialMaterialState& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			return ValidateFields(Object, {},
				{TEXT("friction"), TEXT("static_friction"), TEXT("restitution"), TEXT("density"), TEXT("raise_mass_to_power"), TEXT("friction_combine_mode"), TEXT("restitution_combine_mode")},
				TEXT("physical_material_state"), Error)
				&& RequireStateChanges(Object, TEXT("physical_material_state"), Error)
				&& OptionalString(Object, TEXT("friction"), Out.Friction, Error)
				&& OptionalString(Object, TEXT("static_friction"), Out.StaticFriction, Error)
				&& OptionalString(Object, TEXT("restitution"), Out.Restitution, Error)
				&& OptionalString(Object, TEXT("density"), Out.Density, Error)
				&& OptionalString(Object, TEXT("raise_mass_to_power"), Out.RaiseMassToPower, Error)
				&& OptionalString(Object, TEXT("friction_combine_mode"), Out.FrictionCombineMode, Error)
				&& OptionalString(Object, TEXT("restitution_combine_mode"), Out.RestitutionCombineMode, Error);
		}

		bool ParseBodyState(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialPhysicsBodyState& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			return ValidateFields(Object, {},
				{TEXT("collision_enabled"), TEXT("physics_type"), TEXT("mass_kg"), TEXT("consider_for_bounds")},
				TEXT("physics_body_state"), Error)
				&& RequireStateChanges(Object, TEXT("physics_body_state"), Error)
				&& OptionalString(Object, TEXT("collision_enabled"), Out.CollisionEnabled, Error)
				&& OptionalString(Object, TEXT("physics_type"), Out.PhysicsType, Error)
				&& OptionalString(Object, TEXT("mass_kg"), Out.MassKg, Error)
				&& OptionalBool(Object, TEXT("consider_for_bounds"), Out.ConsiderForBounds, Error);
		}

		bool ParseConstraintState(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialConstraintState& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			if (!ValidateFields(Object, {},
				{TEXT("linear_x_motion"), TEXT("linear_y_motion"), TEXT("linear_z_motion"), TEXT("linear_limit_cm"), TEXT("twist_motion"), TEXT("swing1_motion"), TEXT("swing2_motion"), TEXT("twist_limit_degrees"), TEXT("swing1_limit_degrees"), TEXT("swing2_limit_degrees"), TEXT("disable_collision")},
				TEXT("physics_constraint_state"), Error)
				|| !RequireStateChanges(Object, TEXT("physics_constraint_state"), Error))
			{
				return false;
			}
			return OptionalString(Object, TEXT("linear_x_motion"), Out.LinearXMotion, Error)
				&& OptionalString(Object, TEXT("linear_y_motion"), Out.LinearYMotion, Error)
				&& OptionalString(Object, TEXT("linear_z_motion"), Out.LinearZMotion, Error)
				&& OptionalString(Object, TEXT("linear_limit_cm"), Out.LinearLimitCm, Error)
				&& OptionalString(Object, TEXT("twist_motion"), Out.TwistMotion, Error)
				&& OptionalString(Object, TEXT("swing1_motion"), Out.Swing1Motion, Error)
				&& OptionalString(Object, TEXT("swing2_motion"), Out.Swing2Motion, Error)
				&& OptionalString(Object, TEXT("twist_limit_degrees"), Out.TwistLimitDegrees, Error)
				&& OptionalString(Object, TEXT("swing1_limit_degrees"), Out.Swing1LimitDegrees, Error)
				&& OptionalString(Object, TEXT("swing2_limit_degrees"), Out.Swing2LimitDegrees, Error)
				&& OptionalBool(Object, TEXT("disable_collision"), Out.DisableCollision, Error);
		}

		bool ParseEndpoint(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialConstraintEndpoint& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Frame;
			return ValidateFields(Object, {TEXT("body_ref"), TEXT("frame")}, {},
				TEXT("constraint_endpoint"), Error)
				&& RequiredString(Object, TEXT("body_ref"), Out.BodyRef, Error)
				&& RequiredObject(Object, TEXT("frame"), Frame, Error)
				&& ParseAt(Frame, Out.Frame, ParseTransform, Error, TEXT("/frame"));
		}

		bool ParseBody(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialPhysicsBody& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> State;
			return ValidateFields(Object,
				{TEXT("body_ref"), TEXT("bone_ref"), TEXT("shape_refs"), TEXT("state")}, {},
				TEXT("physics_body"), Error)
				&& RequiredString(Object, TEXT("body_ref"), Out.BodyRef, Error)
				&& RequiredString(Object, TEXT("bone_ref"), Out.BoneRef, Error)
				&& StringArray(Object, TEXT("shape_refs"), Out.ShapeRefs, true, Error)
				&& RequiredObject(Object, TEXT("state"), State, Error)
				&& ParseAt(State, Out.State, ParseBodyState, Error, TEXT("/state"));
		}

		bool ParseConstraint(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialPhysicsConstraint& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> A;
			TSharedPtr<FJsonObject> B;
			TSharedPtr<FJsonObject> State;
			return ValidateFields(Object,
				{TEXT("constraint_ref"), TEXT("endpoint_a"), TEXT("endpoint_b"), TEXT("state")}, {},
				TEXT("physics_constraint"), Error)
				&& RequiredString(Object, TEXT("constraint_ref"), Out.ConstraintRef, Error)
				&& RequiredObject(Object, TEXT("endpoint_a"), A, Error)
				&& ParseAt(A, Out.EndpointA, ParseEndpoint, Error, TEXT("/endpoint_a"))
				&& RequiredObject(Object, TEXT("endpoint_b"), B, Error)
				&& ParseAt(B, Out.EndpointB, ParseEndpoint, Error, TEXT("/endpoint_b"))
				&& RequiredObject(Object, TEXT("state"), State, Error)
				&& ParseAt(State, Out.State, ParseConstraintState, Error, TEXT("/state"));
		}

		bool ParseProfile(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialPhysicsProfile& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			return ValidateFields(Object,
				{TEXT("profile_name")}, {TEXT("body_refs"), TEXT("constraint_refs")},
				TEXT("physics_profile"), Error)
				&& RequiredString(Object, TEXT("profile_name"), Out.ProfileName, Error)
				&& StringArray(Object, TEXT("body_refs"), Out.BodyRefs, false, Error)
				&& StringArray(Object, TEXT("constraint_refs"), Out.ConstraintRefs, false, Error);
		}

		bool ParseSession(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialSimulationSession& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			return ValidateFields(Object,
				{TEXT("session_id"), TEXT("owner_ref"), TEXT("generation")}, {},
				TEXT("simulation_session"), Error)
				&& RequiredString(Object, TEXT("session_id"), Out.SessionId, Error, false)
				&& RequiredString(Object, TEXT("owner_ref"), Out.OwnerRef, Error)
				&& RequiredString(Object, TEXT("generation"), Out.Generation, Error, false);
		}

		bool ParseVectorSample(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialVectorSample& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Time;
			TSharedPtr<FJsonObject> Frame;
			return ValidateFields(Object,
				{TEXT("subject_ref"), TEXT("time"), TEXT("frame"), TEXT("components"), TEXT("evidence_ids")}, {},
				TEXT("vector_sample"), Error)
				&& RequiredString(Object, TEXT("subject_ref"), Out.SubjectRef, Error)
				&& RequiredObject(Object, TEXT("time"), Time, Error)
				&& ParseAt(Time, Out.Time, ParsePoint, Error, TEXT("/time"))
				&& RequiredObject(Object, TEXT("frame"), Frame, Error)
				&& ParseAt(Frame, Out.Frame, ParseFrame, Error, TEXT("/frame"))
				&& StringArray(Object, TEXT("components"), Out.Components, true, Error)
				&& StringArray(Object, TEXT("evidence_ids"), Out.EvidenceIds, true, Error, false);
		}

		bool ParseSelector(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialSelector& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			if (!ValidateFields(Object, {TEXT("mode")},
				{TEXT("refs"), TEXT("start"), TEXT("count")}, TEXT("selector"), Error)
				|| !RequiredString(Object, TEXT("mode"), Out.Mode, Error)
				|| !StringArray(Object, TEXT("refs"), Out.Refs, false, Error)) return false;
			for (const TCHAR* Field : {TEXT("start"), TEXT("count")})
			{
				if (!Object->HasField(Field)) continue;
				double Value = 0.0;
				if (!Object->TryGetNumberField(Field, Value) || Value < 0.0
					|| !FMath::IsNearlyEqual(Value, FMath::RoundToDouble(Value)))
					return Fail(Error, InputValueFailure(TEXT("rig_simulation_spatial_selector_integer_invalid"),
						TEXT("The selector value must be a non-negative integer."), Field, Object->TryGetField(Field),
						MakeShared<FJsonValueString>(TEXT("non-negative integer"))), Field);
				if (FString(Field) == TEXT("start")) Out.Start = static_cast<int32>(Value);
				else Out.Count = static_cast<int32>(Value);
			}
			return true;
		}

		bool ParseFrameMapping(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialFrameMapping& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Source;
			TSharedPtr<FJsonObject> Target;
			return ValidateFields(Object,
				{TEXT("source_frame"), TEXT("target_frame"), TEXT("conversion_evidence_ids")}, {},
				TEXT("frame_mapping"), Error)
				&& RequiredObject(Object, TEXT("source_frame"), Source, Error)
				&& ParseAt(Source, Out.SourceFrame, ParseFrame, Error, TEXT("/source_frame"))
				&& RequiredObject(Object, TEXT("target_frame"), Target, Error)
				&& ParseAt(Target, Out.TargetFrame, ParseFrame, Error, TEXT("/target_frame"))
				&& StringArray(Object, TEXT("conversion_evidence_ids"),
					Out.ConversionEvidenceIds, true, Error, false);
		}

		bool ConvertCanonicalValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentRigSimulationSpatialExecutionError& Error,
			const FString& InputPointer)
		{
			OutValue.Reset();
			const auto Reject = [&](FDiagnostic Diagnostic)
			{
				auto RelatedInput = MakeShared<FJsonObject>();
				RelatedInput->SetStringField(TEXT("source_kind"), TEXT("related_input"));
				RelatedInput->SetStringField(TEXT("json_path"), InputPointer.IsEmpty() ? TEXT("/") : InputPointer);
				Diagnostic.RelatedSources.Add(MakeShared<FJsonValueObject>(MoveTemp(RelatedInput)));
				return Fail(Error, MoveTemp(Diagnostic));
			};
			if (!JsonValue.IsValid()) return Reject(FDiagnostic::Failure(TEXT("rig_simulation_spatial_intent_json_invalid"),
				TEXT("The canonical input conversion encountered an unavailable JSON value."), TEXT("native_execution")));
			FUeAgentStateSemanticError StateError;
			switch (JsonValue->Type)
			{
			case EJson::Null:
				return FUeAgentCanonicalValue::TryNull(Resources, OutValue, StateError)
					|| Reject(MoveTemp(StateError));
			case EJson::Boolean:
				return FUeAgentCanonicalValue::TryBoolean(JsonValue->AsBool(), Resources,
					OutValue, StateError) || Reject(MoveTemp(StateError));
			case EJson::String:
				return FUeAgentCanonicalValue::TryString(JsonValue->AsString(), Resources,
					OutValue, StateError) || Reject(MoveTemp(StateError));
			case EJson::Number:
			{
				const double Number = JsonValue->AsNumber();
				const bool bInteger = FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number));
				const FString Lexeme = bInteger
					? FString::Printf(TEXT("%.0f"), Number)
					: FString::SanitizeFloat(Number, 0);
				return (bInteger
					? FUeAgentCanonicalValue::TryInteger(Lexeme, Resources, OutValue, StateError)
					: FUeAgentCanonicalValue::TryDecimal(Lexeme, Resources, OutValue, StateError))
					|| Reject(MoveTemp(StateError));
			}
			case EJson::Array:
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				const auto& Array = JsonValue->AsArray();
				for (int32 Index = 0; Index < Array.Num(); ++Index)
				{
					FUeAgentCanonicalValuePtr Canonical;
					if (!ConvertCanonicalValue(Array[Index], Resources, Canonical, Error,
						InputPointer + FString::Printf(TEXT("/%d"), Index))) return false;
					Items.Add(Canonical.ToSharedRef());
				}
				return FUeAgentCanonicalValue::TryArray(Items, Resources, OutValue, StateError)
					|| Reject(MoveTemp(StateError));
			}
			case EJson::Object:
			{
				TArray<FString> Keys;
				JsonValue->AsObject()->Values.GetKeys(Keys);
				Keys.Sort();
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
				for (const FString& Key : Keys)
				{
					FUeAgentCanonicalValuePtr Canonical;
					if (!ConvertCanonicalValue(
							JsonValue->AsObject()->Values.FindChecked(Key), Resources,
							Canonical, Error, InputPointer + TEXT("/") + InputPointerToken(Key))) return false;
					Items.Emplace(Key, Canonical.ToSharedRef());
				}
				return FUeAgentCanonicalValue::TryObject(Items, Resources, OutValue, StateError)
					|| Reject(MoveTemp(StateError));
			}
			default:
			{
				auto Diagnostic = FDiagnostic::Failure(TEXT("rig_simulation_spatial_intent_json_kind_invalid"),
					TEXT("The canonical input conversion does not support this JSON value kind."), TEXT("native_execution"));
				Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(JsonValue->Type));
				Diagnostic.ActualValuePresent = true;
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("null, boolean, string, number, array or object"));
				return Reject(MoveTemp(Diagnostic));
			}
			}
		}

		EUeAgentRigSimulationSpatialPortFamily PortFamily(const FString& FamilyId)
		{
			if (FamilyId == TEXT("skeletal_attachment")) return EUeAgentRigSimulationSpatialPortFamily::SkeletalAttachment;
			if (FamilyId == TEXT("rig_bake")) return EUeAgentRigSimulationSpatialPortFamily::RigBake;
			if (FamilyId == TEXT("ik_retarget")) return EUeAgentRigSimulationSpatialPortFamily::IkRetarget;
			if (FamilyId == TEXT("simulation_definition")) return EUeAgentRigSimulationSpatialPortFamily::SimulationDefinition;
			return EUeAgentRigSimulationSpatialPortFamily::SimulationObservation;
		}

		template <typename ValueType>
		struct TLocatedInput
		{
			ValueType Value;
			FString Pointer;
			TLocatedInput(const ValueType& InValue, FString InPointer)
				: Value(InValue), Pointer(MoveTemp(InPointer)) {}
		};

		struct FDecoded
		{
			FUeAgentRigSimulationSpatialRequestPayload Payload;
			FString PrimaryTargetRef;
			TOptional<FRigSimulationSpatialRevision> ExpectedRevision;
			TArray<TLocatedInput<FRigSimulationSpatialPoint>> Points;
			TArray<TLocatedInput<FRigSimulationSpatialRange>> Ranges;
			TArray<TLocatedInput<FRigSimulationSpatialTransform3d>> Transforms;
			TArray<TLocatedInput<TPair<FString, FRigSimulationSpatialFrame3d>>> FrameUses;
			TArray<TLocatedInput<FRigSimulationSpatialVectorSample>> DirectionSamples;
		};

		bool ParseExpectedRevision(const TSharedPtr<FJsonObject>& Root,
			FDecoded& Out, FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Object;
			FRigSimulationSpatialRevision Revision;
			if (!RequiredObject(Root, TEXT("expected_revision"), Object, Error)
				|| !ParseAt(Object, Revision, ParseRevision, Error, TEXT("/expected_revision"))) return false;
			Out.ExpectedRevision = MoveTemp(Revision);
			return true;
		}

		bool DecodePayload(const FRequestSpec& Spec,
			const TSharedPtr<FJsonObject>& Root, FDecoded& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			if (!ValidateFields(Root, Spec.RequiredFields, Spec.OptionalFields,
				Spec.CapabilityId, Error)) return false;
			const FString Capability = Spec.CapabilityId;
			TSharedPtr<FJsonObject> Object;
			bool bPresent = false;

			if (Capability == TEXT("rig.skeleton.preview_mesh.bind"))
			{
				FRigSkeletonPreviewMeshBindInput Value;
				if (!RequiredString(Root, TEXT("skeleton_ref"), Value.SkeletonRef, Error)
					|| !RequiredString(Root, TEXT("preview_mesh_ref"), Value.PreviewMeshRef, Error)
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.SkeletonRef;
				Out.Payload.Set<FRigSkeletonPreviewMeshBindInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("rig.skeleton.socket.update"))
			{
				FRigSkeletonSocketUpdateInput Value;
				if (!RequiredString(Root, TEXT("skeleton_ref"), Value.SkeletonRef, Error)
					|| !RequiredString(Root, TEXT("operation"), Value.Operation, Error)
					|| !RequiredString(Root, TEXT("socket_ref"), Value.SocketRef, Error)
					|| !RequiredString(Root, TEXT("parent_bone_ref"), Value.ParentBoneRef, Error)
					|| !RequiredObject(Root, TEXT("socket_transform"), Object, Error)
					|| !ParseAt(Object, Value.SocketTransform, ParseTransform, Error, TEXT("/socket_transform"))
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.SkeletonRef;
				Out.Transforms.Emplace(Value.SocketTransform, TEXT("/socket_transform/frame"));
				Out.Payload.Set<FRigSkeletonSocketUpdateInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("rig.control.bake.context"))
			{
				FRigControlBakeContextInput Value;
				if (!RequiredString(Root, TEXT("sequence_ref"), Value.SequenceRef, Error)
					|| !RequiredString(Root, TEXT("binding_name"), Value.BindingName, Error)
					|| !RequiredString(Root, TEXT("rig_ref"), Value.RigRef, Error)
					|| (Root->HasField(TEXT("target_animation_ref"))
						&& !RequiredString(Root, TEXT("target_animation_ref"), Value.TargetAnimationRef, Error))) return false;
				Out.PrimaryTargetRef = Value.SequenceRef;
				Out.Payload.Set<FRigControlBakeContextInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("rig.control.bake.animation"))
			{
				FRigControlBakeAnimationInput Value;
				if (!RequiredString(Root, TEXT("source_sequence_ref"), Value.SourceSequenceRef, Error)
					|| !RequiredString(Root, TEXT("target_animation_ref"), Value.TargetAnimationRef, Error)
					|| !RequiredString(Root, TEXT("binding_ref"), Value.BindingRef, Error)
					|| !RequiredString(Root, TEXT("rig_ref"), Value.RigRef, Error)
					|| !RequiredObject(Root, TEXT("authored_range"), Object, Error)
					|| !ParseAt(Object, Value.AuthoredRange, ParseRange, Error, TEXT("/authored_range"))
					|| !RequiredObject(Root, TEXT("sample_rate"), Object, Error)
					|| !ParseAt(Object, Value.SampleRate, ParseRate, Error, TEXT("/sample_rate"))
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.TargetAnimationRef;
				Out.Ranges.Emplace(Value.AuthoredRange, TEXT("/authored_range"));
				Out.Payload.Set<FRigControlBakeAnimationInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("rig.control.bake.controls"))
			{
				FRigControlBakeControlsInput Value;
				if (!RequiredString(Root, TEXT("target_sequence_ref"), Value.TargetSequenceRef, Error)
					|| !RequiredString(Root, TEXT("binding_ref"), Value.BindingRef, Error)
					|| !RequiredString(Root, TEXT("rig_ref"), Value.RigRef, Error)
					|| !RequiredObject(Root, TEXT("authored_range"), Object, Error)
					|| !ParseAt(Object, Value.AuthoredRange, ParseRange, Error, TEXT("/authored_range"))
					|| !RequiredObject(Root, TEXT("sample_rate"), Object, Error)
					|| !ParseAt(Object, Value.SampleRate, ParseRate, Error, TEXT("/sample_rate"))
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.TargetSequenceRef;
				Out.Ranges.Emplace(Value.AuthoredRange, TEXT("/authored_range"));
				Out.Payload.Set<FRigControlBakeControlsInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("rig.ik.definition.update"))
			{
				FRigIkDefinitionUpdateInput Value;
				if (!RequiredString(Root, TEXT("ik_rig_ref"), Value.IkRigRef, Error)
					|| !ObjectArray(Root, TEXT("definition_changes"), Value.DefinitionChanges,
						true, ParseIkChange, Error)
					|| !ParseExpectedRevision(Root, Out, Error)
					|| !OptionalObject(Root, TEXT("preview_context"), Object, bPresent, Error)) return false;
				if (bPresent && (!ValidateFields(Object, {TEXT("preview_mesh_ref")}, {},
					TEXT("ik_preview_context"), Error)
					|| !RequiredString(Object, TEXT("preview_mesh_ref"), Value.PreviewMeshRef, Error)))
				{
					PrefixInputDiagnostic(Error, TEXT("/preview_context"));
					return false;
				}
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.IkRigRef;
				Out.Payload.Set<FRigIkDefinitionUpdateInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("rig.retarget.definition.update"))
			{
				FRigRetargetDefinitionUpdateInput Value;
				if (!RequiredString(Root, TEXT("retargeter_ref"), Value.RetargeterRef, Error)
					|| !RequiredString(Root, TEXT("source_rig_ref"), Value.SourceRigRef, Error)
					|| !RequiredString(Root, TEXT("target_rig_ref"), Value.TargetRigRef, Error)
					|| !ObjectArray(Root, TEXT("chain_mappings"), Value.ChainMappings,
						true, ParseChainMapping, Error)
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.RetargeterRef;
				Out.Payload.Set<FRigRetargetDefinitionUpdateInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("rig.retarget.pose.update"))
			{
				FRigRetargetPoseUpdateInput Value;
				if (!RequiredString(Root, TEXT("retargeter_ref"), Value.RetargeterRef, Error)
					|| !RequiredString(Root, TEXT("subject_side"), Value.SubjectSide, Error)
					|| !ObjectArray(Root, TEXT("pose"), Value.Pose, true, ParsePoseItem, Error)
					|| !RequiredObject(Root, TEXT("pose_frame"), Object, Error)
					|| !ParseAt(Object, Value.PoseFrame, ParseFrame, Error, TEXT("/pose_frame"))
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.RetargeterRef;
				Out.FrameUses.Emplace(TPair<FString, FRigSimulationSpatialFrame3d>(Value.RetargeterRef, Value.PoseFrame), TEXT("/pose_frame"));
				for (int32 Index = 0; Index < Value.Pose.Num(); ++Index)
					Out.Transforms.Emplace(Value.Pose[Index].Transform, FString::Printf(TEXT("/pose/%d/transform/frame"), Index));
				Out.Payload.Set<FRigRetargetPoseUpdateInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("rig.retarget.pose.auto_align"))
			{
				FRigRetargetPoseAutoAlignInput Value;
				if (!RequiredString(Root, TEXT("retargeter_ref"), Value.RetargeterRef, Error)
					|| !RequiredString(Root, TEXT("subject_side"), Value.SubjectSide, Error)
					|| !RequiredString(Root, TEXT("alignment_policy"), Value.AlignmentPolicy, Error)
					|| !ParseExpectedRevision(Root, Out, Error)
					|| !StringArray(Root, TEXT("bone_refs"), Value.BoneRefs, false, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.RetargeterRef;
				Out.Payload.Set<FRigRetargetPoseAutoAlignInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("rig.retarget.assets.generate"))
			{
				FRigRetargetAssetsGenerateInput Value;
				if (!RequiredString(Root, TEXT("source_subject_ref"), Value.SourceSubjectRef, Error)
					|| !RequiredString(Root, TEXT("target_subject_ref"), Value.TargetSubjectRef, Error)
					|| !RequiredString(Root, TEXT("retargeter_ref"), Value.RetargeterRef, Error)
					|| !RequiredString(Root, TEXT("destination_root"), Value.DestinationRoot, Error)
					|| !ObjectArray(Root, TEXT("asset_requests"), Value.AssetRequests,
						true, ParseAssetRequest, Error)) return false;
				Out.PrimaryTargetRef = Value.DestinationRoot;
				Out.Payload.Set<FRigRetargetAssetsGenerateInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("physics.material.update"))
			{
				FPhysicsMaterialUpdateInput Value;
				if (!RequiredString(Root, TEXT("physical_material_ref"), Value.PhysicalMaterialRef, Error)
					|| !RequiredObject(Root, TEXT("material_state"), Object, Error)
					|| !ParseAt(Object, Value.MaterialState, ParseMaterialState, Error, TEXT("/material_state"))
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.PhysicalMaterialRef;
				Out.Payload.Set<FPhysicsMaterialUpdateInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("physics.asset.structure.replace"))
			{
				FPhysicsAssetStructureReplaceInput Value;
				if (!RequiredString(Root, TEXT("physics_asset_ref"), Value.PhysicsAssetRef, Error)
					|| !ObjectArray(Root, TEXT("bodies"), Value.Bodies, true, ParseBody, Error)
					|| !ObjectArray(Root, TEXT("constraints"), Value.Constraints, true, ParseConstraint, Error)
					|| !ObjectArray(Root, TEXT("profiles"), Value.Profiles, true, ParseProfile, Error)
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.PhysicsAssetRef;
				for (int32 Index = 0; Index < Value.Constraints.Num(); ++Index)
				{
					const auto& Item = Value.Constraints[Index];
					Out.Transforms.Emplace(Item.EndpointA.Frame, FString::Printf(TEXT("/constraints/%d/endpoint_a/frame/frame"), Index));
					Out.Transforms.Emplace(Item.EndpointB.Frame, FString::Printf(TEXT("/constraints/%d/endpoint_b/frame/frame"), Index));
				}
				Out.Payload.Set<FPhysicsAssetStructureReplaceInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("physics.constraint.update"))
			{
				FPhysicsConstraintUpdateInput Value;
				if (!RequiredString(Root, TEXT("physics_asset_ref"), Value.PhysicsAssetRef, Error)
					|| !RequiredString(Root, TEXT("constraint_ref"), Value.ConstraintRef, Error)
					|| !RequiredObject(Root, TEXT("endpoint_a"), Object, Error)
					|| !ParseAt(Object, Value.EndpointA, ParseEndpoint, Error, TEXT("/endpoint_a"))
					|| !RequiredObject(Root, TEXT("endpoint_b"), Object, Error)
					|| !ParseAt(Object, Value.EndpointB, ParseEndpoint, Error, TEXT("/endpoint_b"))
					|| !RequiredObject(Root, TEXT("constraint_state"), Object, Error)
					|| !ParseAt(Object, Value.ConstraintState, ParseConstraintState, Error, TEXT("/constraint_state"))
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.PhysicsAssetRef;
				Out.Transforms.Emplace(Value.EndpointA.Frame, TEXT("/endpoint_a/frame/frame"));
				Out.Transforms.Emplace(Value.EndpointB.Frame, TEXT("/endpoint_b/frame/frame"));
				Out.Payload.Set<FPhysicsConstraintUpdateInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("physics.simulation.probe"))
			{
				FPhysicsSimulationProbeInput Value;
				if (!StringArray(Root, TEXT("subject_refs"), Value.SubjectRefs, true, Error)
					|| !RequiredObject(Root, TEXT("simulation_session"), Object, Error)
					|| !ParseAt(Object, Value.SimulationSession, ParseSession, Error, TEXT("/simulation_session"))
					|| !RequiredObject(Root, TEXT("simulation_range"), Object, Error)
					|| !ParseAt(Object, Value.SimulationRange, ParseRange, Error, TEXT("/simulation_range"))
					|| !ObjectArray(Root, TEXT("impulses"), Value.Impulses, false, ParseVectorSample, Error)
					|| !ObjectArray(Root, TEXT("velocities"), Value.Velocities, false, ParseVectorSample, Error)) return false;
				Out.PrimaryTargetRef = Value.SimulationSession.OwnerRef;
				Out.Ranges.Emplace(Value.SimulationRange, TEXT("/simulation_range"));
				for (int32 Index = 0; Index < Value.Impulses.Num(); ++Index)
				{
					const auto& Item = Value.Impulses[Index];
					Out.Points.Emplace(Item.Time, FString::Printf(TEXT("/impulses/%d/time"), Index));
					Out.DirectionSamples.Emplace(Item, FString::Printf(TEXT("/impulses/%d/frame"), Index));
				}
				for (int32 Index = 0; Index < Value.Velocities.Num(); ++Index)
				{
					const auto& Item = Value.Velocities[Index];
					Out.Points.Emplace(Item.Time, FString::Printf(TEXT("/velocities/%d/time"), Index));
					Out.DirectionSamples.Emplace(Item, FString::Printf(TEXT("/velocities/%d/frame"), Index));
				}
				Out.Payload.Set<FPhysicsSimulationProbeInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("deformer.geometry.sample"))
			{
				FDeformerGeometrySampleInput Value;
				if (!RequiredString(Root, TEXT("geometry_cache_ref"), Value.GeometryCacheRef, Error)
					|| !RequiredObject(Root, TEXT("sample_time"), Object, Error)
					|| !ParseAt(Object, Value.SampleTime, ParsePoint, Error, TEXT("/sample_time"))
					|| !RequiredObject(Root, TEXT("output_frame"), Object, Error)
					|| !ParseAt(Object, Value.OutputFrame, ParseFrame, Error, TEXT("/output_frame"))
					|| !OptionalObject(Root, TEXT("vertex_selector"), Object, bPresent, Error)) return false;
				if (bPresent)
				{
					FRigSimulationSpatialSelector Selector;
					if (!ParseAt(Object, Selector, ParseSelector, Error, TEXT("/vertex_selector"))) return false;
					Value.VertexSelector = MoveTemp(Selector);
				}
				Out.PrimaryTargetRef = Value.GeometryCacheRef;
				Out.Points.Emplace(Value.SampleTime, TEXT("/sample_time"));
				Out.FrameUses.Emplace(TPair<FString, FRigSimulationSpatialFrame3d>(Value.GeometryCacheRef, Value.OutputFrame), TEXT("/output_frame"));
				Out.Payload.Set<FDeformerGeometrySampleInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("deformer.training_inputs.validate"))
			{
				FDeformerTrainingInputsValidateInput Value;
				if (!StringArray(Root, TEXT("training_input_refs"), Value.TrainingInputRefs, true, Error)
					|| !ObjectArray(Root, TEXT("temporal_ranges"), Value.TemporalRanges, true, ParseRange, Error)
					|| !ObjectArray(Root, TEXT("frame_mappings"), Value.FrameMappings, true, ParseFrameMapping, Error)) return false;
				Out.PrimaryTargetRef = Value.TrainingInputRefs[0];
				for (int32 Index = 0; Index < Value.TemporalRanges.Num(); ++Index)
					Out.Ranges.Emplace(Value.TemporalRanges[Index], FString::Printf(TEXT("/temporal_ranges/%d"), Index));
				for (int32 Index = 0; Index < Value.FrameMappings.Num(); ++Index)
				{
					const auto& Mapping = Value.FrameMappings[Index];
					Out.FrameUses.Emplace(TPair<FString, FRigSimulationSpatialFrame3d>(Out.PrimaryTargetRef, Mapping.SourceFrame),
						FString::Printf(TEXT("/frame_mappings/%d/source_frame"), Index));
					Out.FrameUses.Emplace(TPair<FString, FRigSimulationSpatialFrame3d>(Out.PrimaryTargetRef, Mapping.TargetFrame),
						FString::Printf(TEXT("/frame_mappings/%d/target_frame"), Index));
				}
				Out.Payload.Set<FDeformerTrainingInputsValidateInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("deformer.evaluation.sample"))
			{
				FDeformerEvaluationSampleInput Value;
				if (!RequiredString(Root, TEXT("deformer_ref"), Value.DeformerRef, Error)
					|| !RequiredString(Root, TEXT("subject_ref"), Value.SubjectRef, Error)
					|| !RequiredObject(Root, TEXT("sample_time"), Object, Error)
					|| !ParseAt(Object, Value.SampleTime, ParsePoint, Error, TEXT("/sample_time"))
					|| !RequiredObject(Root, TEXT("output_frame"), Object, Error)
					|| !ParseAt(Object, Value.OutputFrame, ParseFrame, Error, TEXT("/output_frame"))
					|| !OptionalObject(Root, TEXT("vertex_selector"), Object, bPresent, Error)) return false;
				if (bPresent)
				{
					FRigSimulationSpatialSelector Selector;
					if (!ParseAt(Object, Selector, ParseSelector, Error, TEXT("/vertex_selector"))) return false;
					Value.VertexSelector = MoveTemp(Selector);
				}
				Out.PrimaryTargetRef = Value.DeformerRef;
				Out.Points.Emplace(Value.SampleTime, TEXT("/sample_time"));
				Out.FrameUses.Emplace(TPair<FString, FRigSimulationSpatialFrame3d>(Value.SubjectRef, Value.OutputFrame), TEXT("/output_frame"));
				Out.Payload.Set<FDeformerEvaluationSampleInput>(MoveTemp(Value));
				return true;
			}
			return Fail(Error, UE::UAI::Contract::FDiagnostic::Failure(TEXT("rig_simulation_spatial_request_decode_missing"),
				TEXT("The registered capability has no request payload decoder."), TEXT("native_execution")), Capability);
		}

		bool BuildFormalPortRequest(
			const FRequestSpec& Spec,
			const FDecoded& Decoded,
			const IUeAgentRigSimulationSpatialRequestEnvironment& Environment,
			FUeAgentRigSimulationSpatialPortRequest& OutRequest,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			const auto RequireTemporalPath = [&](const FString& ClockId, const FString& Pointer)
			{
				if (Environment.HasTemporalPath(ClockId)) return true;
				Error = InputValueFailure(TEXT("rig_simulation_spatial_clock_conversion_missing"),
					TEXT("The requested clock has no conversion path to the native clock."), TEXT("clock_id"),
					MakeShared<FJsonValueString>(ClockId), MakeShared<FJsonValueString>(TEXT("clock with a registered path to the native clock")));
				Error.TargetPath = ClockId;
				Error.SubjectId = Spec.CapabilityId;
				PrefixInputDiagnostic(Error, Pointer);
				return false;
			};
			TArray<FUeAgentTemporalPoint> Points;
			for (const auto& Located : Decoded.Points)
			{
				const auto& Point = Located.Value;
				if (!RequireTemporalPath(Point.ClockId, Located.Pointer)) return false;
				Points.Add(Point.ToTemporalPoint());
			}
			TArray<FUeAgentTemporalRange> Ranges;
			for (const auto& Located : Decoded.Ranges)
			{
				const auto& Range = Located.Value;
				if (!RequireTemporalPath(Range.ClockId, Located.Pointer)) return false;
				Ranges.Add(Range.ToTemporalRange());
			}

			TArray<FUeAgentSpatialValuePtr> Values;
			const auto ResolveFrameInput = [&](const FRigSimulationSpatialFrame3d& WireFrame,
				const FString& FramePointer, FUeAgentSpatialFrame& Frame)
			{
				if (!Environment.HasSpatialPath(WireFrame.FrameId))
				{
					Error = FDiagnostic::Failure(TEXT("rig_simulation_spatial_frame_conversion_missing"),
						TEXT("The requested frame has no conversion path to a native frame."), TEXT("native_execution"));
					Error.PropertyPath = TEXT("frame_id");
					Error.TargetPath = WireFrame.FrameId;
					Error.ActualValuePresent = true;
					Error.ActualValue = MakeShared<FJsonValueString>(WireFrame.FrameId);
					Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("frame with a registered path to a native frame"));
					Error.SubjectId = Spec.CapabilityId;
					Error.SourceKind = TEXT("canonical_input");
					Error.Source.JsonPath = FramePointer + TEXT("/frame_id");
					return false;
				}
				if (Environment.TryResolveFrame(WireFrame, Frame, Error)) return true;
				if (Error.SubjectId.IsEmpty()) Error.SubjectId = Spec.CapabilityId;
				if (Error.Source.SourceFile.IsEmpty() && Error.Source.JsonPath.IsEmpty())
				{
					if (Error.SourceKind == TEXT("native_result") || Error.PropertyPath.IsEmpty())
					{
						Error.SourceKind = TEXT("canonical_related_input");
						Error.Source.JsonPath = FramePointer;
					}
					else
					{
						Error.SourceKind = TEXT("canonical_input");
						Error.Source.JsonPath = FramePointer + TEXT("/") + InputPointerToken(Error.PropertyPath);
					}
				}
				return false;
			};
			for (const auto& Located : Decoded.Transforms)
			{
				const auto& Transform = Located.Value;
				FUeAgentSpatialFrame Frame(TEXT(""), TEXT(""), 0, {}, {}, TEXT(""),
					TEXT(""), FUeAgentTemporalPoint(TEXT(""), TEXT("")));
				if (!ResolveFrameInput(Transform.Frame, Located.Pointer, Frame)) return false;
				const FUeAgentTemporalPoint ObservedAt = Frame.GetObservedAt();
				Values.Add(MakeShared<FUeAgentSpatialValue>(
					Transform.Subject, EUeAgentSpatialQuantityKind::Transform,
					MoveTemp(Frame), Transform.Components,
					ObservedAt, Transform.ConversionEvidenceIds));
			}
			for (const auto& Located : Decoded.FrameUses)
			{
				const auto& Use = Located.Value;
				FUeAgentSpatialFrame Frame(TEXT(""), TEXT(""), 0, {}, {}, TEXT(""),
					TEXT(""), FUeAgentTemporalPoint(TEXT(""), TEXT("")));
				if (!ResolveFrameInput(Use.Value, Located.Pointer, Frame)) return false;
				const FUeAgentTemporalPoint ObservedAt = Frame.GetObservedAt();
				Values.Add(MakeShared<FUeAgentSpatialValue>(
					Use.Key, EUeAgentSpatialQuantityKind::Point, MoveTemp(Frame),
					TArray<FString>{TEXT("0"), TEXT("0"), TEXT("0")}, ObservedAt,
					TArray<FString>{Use.Value.BasisEvidenceId}));
			}
			for (const auto& Located : Decoded.DirectionSamples)
			{
				const auto& Sample = Located.Value;
				FUeAgentSpatialFrame Frame(TEXT(""), TEXT(""), 0, {}, {}, TEXT(""),
					TEXT(""), FUeAgentTemporalPoint(TEXT(""), TEXT("")));
				if (!ResolveFrameInput(Sample.Frame, Located.Pointer, Frame)) return false;
				Values.Add(MakeShared<FUeAgentSpatialValue>(
					Sample.SubjectRef, EUeAgentSpatialQuantityKind::Direction,
					MoveTemp(Frame), Sample.Components, Sample.Time.ToTemporalPoint(),
					Sample.EvidenceIds));
			}
			TSet<FString> Facets;
			Facets.Add(TEXT("explicit_object_environment"));
			Facets.Add(TEXT("explicit_revision_environment"));
			Facets.Add(TEXT("explicit_frame_clock_environment"));
			OutRequest = FUeAgentRigSimulationSpatialPortRequest(
				PortFamily(Spec.FamilyId), MoveTemp(Points), MoveTemp(Ranges),
				MoveTemp(Values), Environment.GetSpatialConversions(), MoveTemp(Facets),
				Environment.GetTemporalConversions());
			return true;
		}

		class FRequestDecoder final : public IUeAgentRigSimulationSpatialRequestDecoder
		{
		public:
			explicit FRequestDecoder(
				TSharedRef<IUeAgentRigSimulationSpatialRequestEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}

			bool TryDecode(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentContractValue& CanonicalInput,
				FUeAgentResourceTracker& Resources,
				FUeAgentRigSimulationSpatialTypedRequestPtr& OutRequest,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutRequest.Reset();
				OutError = {};
				ON_SCOPE_EXIT
				{
					LocateDecodedInput(OutError, Environment->GetInputRootPointer());
				};
				const FRequestSpec* Spec = FindSpec(Contract.GetCapabilityId());
				if (!Spec || Contract.GetTypedRequestId() != Spec->TypeId
					|| Contract.GetFamilyId() != Spec->FamilyId
					|| Contract.RequiresNativeMutation() != Spec->bOperation)
				{
					return Fail(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("rig_simulation_spatial_request_contract_mismatch"),
						TEXT("The request execution contract does not match the registered capability."), TEXT("native_execution")), Contract.GetCapabilityId());
				}
				TSharedPtr<FJsonObject> Root;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!Json::TryParseObject(CanonicalInput.GetJsonValue(), Root, JsonError)
					|| !Root.IsValid())
				{
					OutError = MoveTemp(JsonError);
					if (OutError.IsEmpty()) OutError = FDiagnostic::Failure(TEXT("rig_simulation_spatial_input_object_unavailable"),
						TEXT("The canonical input parser did not provide a JSON object."), TEXT("native_execution"));
					// Parser coordinates belong to this generated document, not the original request text.
					if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("canonical_document");
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Contract.GetCapabilityId();
					return false;
				}
				FDecoded Decoded;
				if (!DecodePayload(*Spec, Root, Decoded, OutError)) return false;
				if (Decoded.PrimaryTargetRef.IsEmpty())
					return Fail(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("rig_simulation_spatial_required_identity_missing"),
						TEXT("The decoded request contains no primary target reference."), TEXT("native_execution")), Contract.GetCapabilityId());
				if (!Spec->bCreateTarget
					&& !Environment->HasExplicitObject(Decoded.PrimaryTargetRef))
				{
					OutError = FDiagnostic::Failure(TEXT("rig_simulation_spatial_explicit_target_unavailable"),
						TEXT("The primary target could not be resolved to an available object."), TEXT("native_execution"));
					OutError.TargetPath = Decoded.PrimaryTargetRef;
					OutError.ActualValue = MakeShared<FJsonValueString>(Decoded.PrimaryTargetRef);
					OutError.ActualValuePresent = true;
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("available explicitly referenced object"));
					OutError.SubjectId = Contract.GetCapabilityId();
					return false;
				}
				if (Spec->bExpectedRevision)
				{
					if (!Decoded.ExpectedRevision.IsSet())
					{
						OutError = FDiagnostic::Failure(TEXT("rig_simulation_spatial_expected_revision_missing"),
							TEXT("This request requires an expected target revision."), TEXT("native_execution"));
						OutError.PropertyPath = TEXT("expected_revision");
						OutError.SourceKind = TEXT("canonical_input");
						OutError.Source.JsonPath = TEXT("/expected_revision");
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("expected target revision"));
						OutError.SubjectId = Contract.GetCapabilityId();
						return false;
					}
					FRigSimulationSpatialRevision Actual;
					if (!Environment->TryReadRevision(
							Decoded.PrimaryTargetRef, Actual, OutError)) return false;
					if (!(Actual == Decoded.ExpectedRevision.GetValue()))
					{
						const FRigSimulationSpatialRevision& Expected =
							Decoded.ExpectedRevision.GetValue();
						OutError = FDiagnostic::Failure(TEXT("rig_simulation_spatial_revision_mismatch"),
							TEXT("The target no longer matches the expected revision."), TEXT("native_execution"));
						OutError.TargetPath = Decoded.PrimaryTargetRef;
						OutError.SubjectId = Contract.GetCapabilityId();
						const bool bSourceChanged = Actual.SourceId != Expected.SourceId;
						const bool bGenerationChanged = Actual.Generation != Expected.Generation;
						const FString Field = bSourceChanged ? TEXT("source_id") : bGenerationChanged ? TEXT("generation") : TEXT("sequence");
						OutError.PropertyPath = TEXT("expected_revision.") + Field;
						OutError.ActualEvidence = MakeShared<FJsonValueString>(bSourceChanged ? Actual.SourceId
							: bGenerationChanged ? Actual.Generation : FString::Printf(TEXT("%lld"), Actual.Sequence));
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(bSourceChanged ? Expected.SourceId
							: bGenerationChanged ? Expected.Generation : FString::Printf(TEXT("%lld"), Expected.Sequence));
						OutError.SourceKind = TEXT("canonical_input");
						OutError.Source.JsonPath = TEXT("/expected_revision/") + Field;
						return false;
					}
				}
				FUeAgentRigSimulationSpatialPortRequest Formal(
					EUeAgentRigSimulationSpatialPortFamily::SimulationObservation,
					{}, {}, {}, {}, {});
				if (!BuildFormalPortRequest(*Spec, Decoded, *Environment, Formal, OutError))
					return false;
				FUeAgentCanonicalValuePtr Intent;
				if (!ConvertCanonicalValue(MakeShared<FJsonValueObject>(Root),
						Resources, Intent, OutError, Environment->GetInputRootPointer() == TEXT("/")
							? FString() : Environment->GetInputRootPointer()) || !Intent.IsValid())
				{
					if (OutError.IsEmpty()) OutError = FDiagnostic::Failure(TEXT("rig_simulation_spatial_canonical_intent_unavailable"),
						TEXT("The canonical input conversion did not provide an intent value."), TEXT("native_execution"));
					return false;
				}
				OutRequest = MakeShared<FUeAgentRigSimulationSpatialRequest>(
					Spec->TypeId, Spec->FamilyId, Spec->RequestKindId,
					Spec->bOperation, MoveTemp(Decoded.PrimaryTargetRef),
					MoveTemp(Decoded.ExpectedRevision), MoveTemp(Decoded.Payload),
					MoveTemp(Formal), Intent.ToSharedRef());
				return OutRequest.IsValid() || Fail(OutError, FDiagnostic::Failure(TEXT("rig_simulation_spatial_typed_request_unavailable"),
					TEXT("The decoder could not create the typed request."), TEXT("native_execution")), Contract.GetCapabilityId());
			}

		private:
			TSharedRef<IUeAgentRigSimulationSpatialRequestEnvironment> Environment;
		};
	}

	bool FRigSimulationSpatialRevision::IsValid() const
	{
		return !SourceId.IsEmpty() && !Generation.IsEmpty() && Sequence >= 0;
	}

	bool FRigSimulationSpatialRevision::operator==(
		const FRigSimulationSpatialRevision& Other) const
	{
		return SourceId == Other.SourceId && Generation == Other.Generation
			&& Sequence == Other.Sequence;
	}

	bool FRigSimulationSpatialRate::IsValid() const
	{
		int64 NumeratorValue = 0;
		int64 DenominatorValue = 0;
		return LexTryParseString(NumeratorValue, *Numerator)
			&& LexTryParseString(DenominatorValue, *Denominator)
			&& NumeratorValue > 0 && DenominatorValue > 0;
	}

	bool FRigSimulationSpatialPoint::IsValid() const
	{
		return !ClockId.IsEmpty() && !Tick.IsEmpty() && Rate.IsValid()
			&& (State == TEXT("authored") || State == TEXT("evaluation")
				|| State == TEXT("simulation"));
	}

	FUeAgentTemporalPoint FRigSimulationSpatialPoint::ToTemporalPoint() const
	{
		return FUeAgentTemporalPoint(ClockId, Tick, Rate.Numerator, Rate.Denominator, State);
	}

	bool FRigSimulationSpatialRange::IsValid() const
	{
		return !ClockId.IsEmpty() && !LowerTick.IsEmpty() && !UpperTick.IsEmpty()
			&& Rate.IsValid() && (State == TEXT("authored")
				|| State == TEXT("evaluation") || State == TEXT("simulation"));
	}

	FUeAgentTemporalRange FRigSimulationSpatialRange::ToTemporalRange() const
	{
		return FUeAgentTemporalRange(
			ClockId, State,
			FUeAgentTemporalBound::Inclusive(FUeAgentTemporalPoint(
				ClockId, LowerTick, Rate.Numerator, Rate.Denominator, State)),
			FUeAgentTemporalBound::Inclusive(FUeAgentTemporalPoint(
				ClockId, UpperTick, Rate.Numerator, Rate.Denominator, State)));
	}

	bool FRigSimulationSpatialFrame3d::IsValid() const
	{
		return !FrameId.IsEmpty() && !BasisFrameId.IsEmpty() && Dimension == 3
			&& !Unit.IsEmpty() && !BasisEvidenceId.IsEmpty();
	}

	bool FRigSimulationSpatialTransform3d::IsValid() const
	{
		return !Subject.IsEmpty() && Quantity == TEXT("transform") && Frame.IsValid()
			&& Components.Num() == 16 && !ConversionEvidenceIds.IsEmpty();
	}

	FUeAgentRigSimulationSpatialRequest::FUeAgentRigSimulationSpatialRequest(
		FString InTypeId,
		FString InFamilyId,
		FString InRequestKindId,
		const bool bInOperation,
		FString InPrimaryTargetRef,
		TOptional<FRigSimulationSpatialRevision> InExpectedRevision,
		FUeAgentRigSimulationSpatialRequestPayload InPayload,
		FUeAgentRigSimulationSpatialPortRequest InFormalPortRequest,
		FUeAgentCanonicalValueRef InIntentPayload)
		: TypeId(MoveTemp(InTypeId))
		, FamilyId(MoveTemp(InFamilyId))
		, RequestKindId(MoveTemp(InRequestKindId))
		, bOperation(bInOperation)
		, PrimaryTargetRef(MoveTemp(InPrimaryTargetRef))
		, ExpectedRevision(MoveTemp(InExpectedRevision))
		, Payload(MoveTemp(InPayload))
		, FormalPortRequest(MoveTemp(InFormalPortRequest))
		, IntentPayload(MoveTemp(InIntentPayload))
	{
	}

	const FString& FUeAgentRigSimulationSpatialRequest::GetTypeId() const { return TypeId; }
	const FString& FUeAgentRigSimulationSpatialRequest::GetFamilyId() const { return FamilyId; }
	const FString& FUeAgentRigSimulationSpatialRequest::GetRequestKindId() const { return RequestKindId; }
	const FString& FUeAgentRigSimulationSpatialRequest::GetPrimaryTargetRef() const { return PrimaryTargetRef; }
	const FUeAgentRigSimulationSpatialRequestPayload& FUeAgentRigSimulationSpatialRequest::GetPayload() const { return Payload; }
	const FUeAgentRigSimulationSpatialPortRequest& FUeAgentRigSimulationSpatialRequest::GetFormalPortRequest() const { return FormalPortRequest; }

	bool FUeAgentRigSimulationSpatialRequest::TryBuildChangeDraft(
		const FUeAgentCanonicalSnapshot& Snapshot,
		const FUeAgentOperationContext& Context,
		TOptional<FChangeDraft>& OutDraft,
		FUeAgentRigSimulationSpatialExecutionError& OutError) const
	{
		OutDraft.Reset();
		OutError = {};
		if (!bOperation)
			return Fail(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("rig_simulation_spatial_query_has_no_change_draft"),
				TEXT("A query request cannot produce a mutation change draft."), TEXT("native_execution")), RequestKindId);
		if (Snapshot.GetEntries().IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("rig_simulation_spatial_preflight_snapshot_empty"),
				TEXT("The preflight snapshot contains no state entry to update."), TEXT("native_execution"));
			OutError.TargetPath = PrimaryTargetRef;
			OutError.SubjectId = RequestKindId;
			OutError.PropertyPath = TEXT("snapshot.entries");
			OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
			OutError.ActualValuePresent = true;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one state entry"));
			return false;
		}
		const FUeAgentStateEntry& Primary = Snapshot.GetEntries()[0];
		TArray<FUeAgentCanonicalIdentity> ImpactTargets;
		for (const FUeAgentStateEntry& Entry : Snapshot.GetEntries())
			ImpactTargets.Add(Entry.GetIdentity());
		OutDraft.Emplace(
			Primary.GetIdentity(), EChangeIntent::Update, Primary.GetValue(),
			IntentPayload, TArray<FString>{TEXT("rig-simulation-spatial:write")},
			TEXT("rig-simulation-spatial-typed-intent:") + RequestKindId,
			MoveTemp(ImpactTargets));
		return OutDraft->IsValid() || Fail(OutError, OutDraft->GetValidationDiagnostic(), RequestKindId);
	}

	bool TryCreateRigSimulationSpatialRequestDecoder(
		TSharedRef<IUeAgentRigSimulationSpatialRequestEnvironment> Environment,
		TSharedPtr<IUeAgentRigSimulationSpatialRequestDecoder>& OutDecoder,
		FUeAgentRigSimulationSpatialExecutionError& OutError)
	{
		OutDecoder.Reset();
		OutError = {};
		TSet<FString> Capabilities;
		TSet<FString> Types;
		TSet<FString> Kinds;
		for (const FRequestSpec& Spec : RequestSpecs())
		{
			if (Capabilities.Contains(Spec.CapabilityId)
				|| Types.Contains(Spec.TypeId) || Kinds.Contains(Spec.RequestKindId))
			{
				OutError = FDiagnostic::Failure(TEXT("rig_simulation_spatial_request_spec_duplicate"),
					TEXT("The request specifications contain a duplicate identifier."), TEXT("native_execution"));
				OutError.SubjectId = Spec.CapabilityId;
				const bool bCapabilityDuplicate = Capabilities.Contains(Spec.CapabilityId);
				const bool bTypeDuplicate = Types.Contains(Spec.TypeId);
				OutError.PropertyPath = bCapabilityDuplicate ? TEXT("capability_id") : bTypeDuplicate ? TEXT("type_id") : TEXT("request_kind_id");
				OutError.ActualValue = MakeShared<FJsonValueString>(bCapabilityDuplicate ? Spec.CapabilityId : bTypeDuplicate ? Spec.TypeId : Spec.RequestKindId);
				OutError.ActualValuePresent = true;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("unique request specification identifier"));
				return false;
			}
			Capabilities.Add(Spec.CapabilityId);
			Types.Add(Spec.TypeId);
			Kinds.Add(Spec.RequestKindId);
		}
		OutDecoder = MakeShared<FRequestDecoder>(MoveTemp(Environment));
		return true;
	}

	bool TryGetRigSimulationSpatialIdentityFields(
		const FString& CapabilityId,
		TArray<FString>& OutFields,
		FUeAgentRigSimulationSpatialExecutionError& OutError)
	{
		OutFields.Reset();
		OutError = {};
		const FRequestSpec* Spec = FindSpec(CapabilityId);
		if (!Spec)
		{
			OutError = FDiagnostic::Failure(TEXT("rig_simulation_spatial_request_spec_unknown"),
				TEXT("No request specification is registered for this capability."), TEXT("native_execution"));
			OutError.SubjectId = CapabilityId;
			OutError.PropertyPath = TEXT("capability_id");
			OutError.ActualValue = MakeShared<FJsonValueString>(CapabilityId);
			OutError.ActualValuePresent = true;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered capability"));
			return false;
		}
		OutFields = Spec->IdentityFields;
		return true;
	}
}
