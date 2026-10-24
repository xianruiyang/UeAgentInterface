#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialIkRetargetNative.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"
#include "Domains/Rigging/UeAgentIKDomainBackend.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "FileHelpers.h"
#include "HAL/PlatformTime.h"
#include "Misc/Crc.h"
#include "Misc/PackageName.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/Solvers/IKRigBodyMoverSolver.h"
#include "Rig/Solvers/IKRigFullBodyIK.h"
#include "Rig/Solvers/IKRigPoleSolver.h"
#include "Rig/Solvers/IKRigSetTransform.h"
#include "RigEditor/IKRigController.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	using namespace BindingGateway::Operation;
	using namespace ContextResource;
	using namespace Discovery;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
		using UE::UAI::Contract::FDiagnostic;

		bool NativeFailure(FDiagnostic& OutError, const FString& Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target = FString())
		{
			OutError = FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = Path;
			OutError.Source.JsonPath = Path;
			OutError.TargetPath = Target;
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValuePresent = Expected.IsValid();
			OutError.ExpectedValue = MoveTemp(Expected);
			return false;
		}

		bool InputFailure(FDiagnostic& OutError, const FString& Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target = FString())
		{
			NativeFailure(OutError, Code, Reason, Path, MoveTemp(Actual), MoveTemp(Expected), Target);
			OutError.SourceKind = TEXT("command_input");
			return false;
		}

		bool MissingNative(FDiagnostic& OutError, const FString& Code, const FString& Reason,
			const FString& Path, const FString& Target)
		{
			return NativeFailure(OutError, Code, Reason, Path,
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target);
		}

		bool UnsupportedCapability(FDiagnostic& OutError, const FString& Capability)
		{
			return NativeFailure(OutError, TEXT("rig_simulation_spatial_family_port_unavailable"),
				TEXT("This native port does not implement the requested capability."), TEXT("/capability_id"),
				MakeShared<FJsonValueString>(Capability), MakeShared<FJsonValueString>(
					TEXT("rig.ik.definition.update, rig.retarget.definition.update, rig.retarget.pose.update, rig.retarget.pose.auto_align, or rig.retarget.assets.generate")));
		}

		void AppendDifference(FDiagnostic& OutDifference, FDiagnostic Difference)
		{
			Difference.Stage = TEXT("verification");
			if (OutDifference.IsEmpty()) OutDifference = MoveTemp(Difference);
			else OutDifference.Diagnostics.Add(MoveTemp(Difference));
		}

		bool MatchFact(bool bMatches, FDiagnostic& OutDifference, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target)
		{
			if (bMatches) return true;
			FDiagnostic Difference;
			NativeFailure(Difference, TEXT("rig_simulation_spatial_ik_retarget_state_mismatch"),
				Reason, Path, MoveTemp(Actual), MoveTemp(Expected), Target);
			AppendDifference(OutDifference, MoveTemp(Difference));
			return false;
		}

		void NativeContext(FDiagnostic& Error, const FString& Path = FString(), bool bCanonicalValue = false)
		{
			Error.SourceKind = TEXT("native_result");
			if (!Path.IsEmpty() || bCanonicalValue)
			{
				auto Locate = [&Path, bCanonicalValue](FString Pointer)
				{
					// Canonical constructors locate their value parameter at /value.
					if (bCanonicalValue && (Pointer == TEXT("/value") || Pointer.StartsWith(TEXT("/value/"))))
						Pointer.RightChopInline(6);
					if (Pointer.IsEmpty() || Pointer == TEXT("/")) return Path;
					if (!Pointer.StartsWith(TEXT("/")))
						Pointer = TEXT("/") + Pointer.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
					return Path + Pointer;
				};
				if (Error.Source.InternalJsonPath.IsEmpty()) Error.Source.InternalJsonPath = Error.Source.JsonPath;
				Error.Source.JsonPath = Locate(Error.Source.JsonPath);
				Error.PropertyPath = Error.PropertyPath.IsEmpty() ? Error.Source.JsonPath : Locate(Error.PropertyPath);
				if (!Error.SemanticPointer.IsEmpty()) Error.SemanticPointer = Locate(Error.SemanticPointer);
			}
			for (auto& Child : Error.Diagnostics) NativeContext(Child, Path, bCanonicalValue);
			if (Error.PublicCause)
			{
				auto Cause = MakeShared<FDiagnostic>(*Error.PublicCause);
				NativeContext(*Cause, Path, bCanonicalValue);
				Error.PublicCause = MoveTemp(Cause);
			}
		}

		bool NativeFailureAt(FDiagnostic& OutError, FDiagnostic Error, const FString& Path, const FString& Target = FString())
		{
			NativeContext(Error, Path);
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target;
			OutError = MoveTemp(Error);
			return false;
		}

		bool ForwardFailure(FDiagnostic& OutError, FDiagnostic Diagnostic, const FString& Subject = FString())
		{
			OutError = MoveTemp(Diagnostic);
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
			return false;
		}

		bool ReferenceFailure(FDiagnostic& OutError, const FString& Code, const FString& Reason,
			const FString& Path, const FString& Reference, const FString& Target)
		{
			return InputFailure(OutError, Code, Reason, Path, MakeShared<FJsonValueString>(Reference),
				MakeShared<FJsonValueString>(TEXT("a reference registered in the selected rig binding")), Target);
		}

		TSharedRef<FJsonValue> NumberFact(double Value)
		{
			if (FMath::IsFinite(Value)) return MakeShared<FJsonValueNumber>(Value);
			return MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Value));
		}

		TSharedRef<FJsonValue> VectorFact(const FVector& Value)
		{
			return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
				NumberFact(Value.X), NumberFact(Value.Y), NumberFact(Value.Z)});
		}

		TSharedRef<FJsonValue> RotationFact(const FQuat& Value)
		{
			return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
				NumberFact(Value.X), NumberFact(Value.Y), NumberFact(Value.Z), NumberFact(Value.W)});
		}


		bool MatchNativeName(const FName& Actual, const FName& Expected, const FString& Path,
			const FString& Reason, const FString& ExpectedRef, const FString& Target, FDiagnostic& OutDifference)
		{
			if (Actual == Expected) return true;
			FDiagnostic Difference;
			NativeFailure(Difference, TEXT("rig_simulation_spatial_ik_retarget_state_mismatch"), Reason, Path,
				nullptr, MakeShared<FJsonValueString>(ExpectedRef), Target);
			Difference.ActualEvidence = MakeShared<FJsonValueString>(Actual.ToString());
			Difference.ExpectedEvidence = MakeShared<FJsonValueString>(Expected.ToString());
			AppendDifference(OutDifference, MoveTemp(Difference));
			return false;
		}

		bool MatchNativeObject(const UObject* Actual, const UObject* Expected, const FString& Path,
			const FString& Reason, const FString& ExpectedRef, const FString& Target, FDiagnostic& OutDifference)
		{
			if (Actual == Expected) return true;
			FDiagnostic Difference;
			NativeFailure(Difference, TEXT("rig_simulation_spatial_ik_retarget_state_mismatch"), Reason, Path,
				MakeShared<FJsonValueBoolean>(Actual != nullptr), MakeShared<FJsonValueString>(ExpectedRef), Target);
			if (Actual) Difference.ActualEvidence = MakeShared<FJsonValueString>(Actual->GetPathName());
			if (Expected) Difference.ExpectedEvidence = MakeShared<FJsonValueString>(Expected->GetPathName());
			AppendDifference(OutDifference, MoveTemp(Difference));
			return false;
		}

		FString ValueKindName(EUeAgentValueKind Kind)
		{
			switch (Kind)
			{
			case EUeAgentValueKind::Null: return TEXT("null");
			case EUeAgentValueKind::Boolean: return TEXT("boolean");
			case EUeAgentValueKind::String: return TEXT("string");
			case EUeAgentValueKind::Integer: return TEXT("integer");
			case EUeAgentValueKind::Decimal: return TEXT("decimal");
			case EUeAgentValueKind::Array: return TEXT("array");
			case EUeAgentValueKind::Object: return TEXT("object");
			case EUeAgentValueKind::Temporal: return TEXT("temporal");
			case EUeAgentValueKind::Spatial: return TEXT("spatial");
			default: return FString::FromInt(static_cast<int32>(Kind));
			}
		}


		bool TrySaveProjectAsset(
			UObject& Object,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			UPackage* Package = Object.GetOutermost();
			if (!UE::UAI::EditorResources::IsPersistentPackage(Package)
				|| !Package->IsDirty())
			{
				return true;
			}
			if (!UE::UAI::EditorResources::SavePackages({Package}, false, OutError))
			{
				return ForwardFailure(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("rig_simulation_spatial_persistence_failed"), OutError, TEXT("execution")), Object.GetPathName());
			}
			return true;
		}

		bool IsCapability(const FString& CapabilityId)
		{
			return CapabilityId == TEXT("rig.ik.definition.update")
				|| CapabilityId == TEXT("rig.retarget.definition.update")
				|| CapabilityId == TEXT("rig.retarget.pose.update")
				|| CapabilityId == TEXT("rig.retarget.pose.auto_align")
				|| CapabilityId == TEXT("rig.retarget.assets.generate");
		}

		const FUeAgentRigSimulationSpatialRequest* ProductionRequest(
			const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Request->GetFamilyId() != TEXT("ik_retarget")
				|| Request->GetTypeId().IsEmpty() || Request->GetRequestKindId().IsEmpty())
			{
				NativeFailure(OutError, TEXT("rig_simulation_spatial_ik_retarget_typed_request_invalid"),
					Request->GetFamilyId() != TEXT("ik_retarget") ? TEXT("The typed request belongs to a different family.")
						: TEXT("The typed request is missing routing metadata."),
					Request->GetFamilyId() != TEXT("ik_retarget") ? TEXT("/family_id")
						: Request->GetTypeId().IsEmpty() ? TEXT("/type_id") : TEXT("/request_kind"),
					MakeShared<FJsonValueString>(Request->GetFamilyId() != TEXT("ik_retarget") ? Request->GetFamilyId() : FString()),
					MakeShared<FJsonValueString>(Request->GetFamilyId() != TEXT("ik_retarget") ? TEXT("ik_retarget") : TEXT("a nonempty identifier")));
				return nullptr;
			}
			return static_cast<const FUeAgentRigSimulationSpatialRequest*>(&Request.Get());
		}

		TOptional<FUeAgentCandidateBinding> FirstTarget(
			const TArray<FUeAgentOperationTargetBinding>& Targets)
		{
			return Targets.IsEmpty() ? TOptional<FUeAgentCandidateBinding>()
				: TOptional<FUeAgentCandidateBinding>(Targets[0].GetBinding());
		}

		bool CheckRevision(
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			const FString& ExplicitRef,
			const FRigSimulationSpatialRevision& Expected,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			FRigSimulationSpatialRevision Actual;
			if (!Environment.TryReadRevision(ExplicitRef, Actual, OutError)) return false;
			if (Actual == Expected) return true;
			const bool bSource = Actual.SourceId != Expected.SourceId;
			const bool bGeneration = !bSource && Actual.Generation != Expected.Generation;
			NativeFailure(OutError, TEXT("rig_simulation_spatial_revision_mismatch"),
				bSource ? TEXT("The revision source differs from the expected source.")
					: bGeneration ? TEXT("The revision generation differs from the expected generation.")
					: TEXT("The revision sequence differs from the expected sequence."),
				bSource ? TEXT("/expected_revision/source_id") : bGeneration
					? TEXT("/expected_revision/generation") : TEXT("/expected_revision/sequence"),
				nullptr, nullptr, ExplicitRef);
			if (bSource || bGeneration)
			{
				OutError.ActualEvidence = MakeShared<FJsonValueString>(bSource ? Actual.SourceId : Actual.Generation);
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(bSource ? Expected.SourceId : Expected.Generation);
			}
			else
			{
				OutError.ActualValuePresent = true;
				OutError.ActualValue = MakeShared<FJsonValueString>(::LexToString(Actual.Sequence));
				OutError.ExpectedValuePresent = true;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(::LexToString(Expected.Sequence));
			}
			return false;
		}

		bool TryNativeTransform(
			const FUeAgentSpatialValue& Value,
			FTransform& OutTransform,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			const TArray<FString>& Components = Value.GetComponents();
			if (!Value.IsValid())
				return NativeFailureAt(OutError, Value.GetValidationDiagnostic(), TEXT("/converted_transform"));
			if (Value.GetQuantityKind() != EUeAgentSpatialQuantityKind::Transform)
				return NativeFailure(OutError, TEXT("rig_simulation_spatial_native_transform_invalid"),
					TEXT("The converted spatial value is not a transform."), TEXT("/converted_transform/quantity_kind"),
					MakeShared<FJsonValueString>(Value.GetQuantityKind() == EUeAgentSpatialQuantityKind::Point ? TEXT("point")
						: Value.GetQuantityKind() == EUeAgentSpatialQuantityKind::Direction ? TEXT("direction") : TEXT("normal")),
					MakeShared<FJsonValueString>(TEXT("transform")));
			if (Components.Num() != 16)
				return NativeFailure(OutError, TEXT("rig_simulation_spatial_native_transform_invalid"),
					TEXT("The converted transform requires sixteen matrix components."), TEXT("/converted_transform/components"),
					MakeShared<FJsonValueNumber>(Components.Num()), MakeShared<FJsonValueNumber>(16));
			FMatrix Matrix = FMatrix::Identity;
			for (int32 Row = 0; Row < 4; ++Row)
			{
				for (int32 Column = 0; Column < 4; ++Column)
				{
					double Component = 0.0;
					if (!LexTryParseString(Component,
							*Components[Row * 4 + Column])
						|| !FMath::IsFinite(Component))
					{
						return NativeFailure(OutError,
							TEXT("rig_simulation_spatial_native_transform_numeric_invalid"),
							TEXT("The converted transform component is not a finite number."),
							TEXT("/converted_transform/components/") + FString::FromInt(Row * 4 + Column),
							MakeShared<FJsonValueString>(Components[Row * 4 + Column]), MakeShared<FJsonValueString>(TEXT("a finite number")));
					}
					// C4 is a column-vector homogeneous matrix.  UE's FMatrix is
					// row-vector based, so the explicit representation is transposed.
					Matrix.M[Column][Row] = Component;
				}
			}
			OutTransform = FTransform(Matrix);
			OutTransform.NormalizeRotation();
			if (OutTransform.IsValid()) return true;
			const FQuat Rotation = OutTransform.GetRotation();
			const FVector Translation = OutTransform.GetTranslation();
			const FVector Scale = OutTransform.GetScale3D();
			const double NativeComponents[] = {Rotation.X, Rotation.Y, Rotation.Z, Rotation.W,
				Translation.X, Translation.Y, Translation.Z, Scale.X, Scale.Y, Scale.Z};
			const TCHAR* Paths[] = {TEXT("/rotation/x"), TEXT("/rotation/y"), TEXT("/rotation/z"), TEXT("/rotation/w"),
				TEXT("/translation/x"), TEXT("/translation/y"), TEXT("/translation/z"), TEXT("/scale/x"), TEXT("/scale/y"), TEXT("/scale/z")};
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(NativeComponents); ++Index)
				if (!FMath::IsFinite(NativeComponents[Index]))
					return NativeFailure(OutError, TEXT("rig_simulation_spatial_native_transform_invalid"),
						TEXT("The native transform contains a non-finite component."), TEXT("/converted_transform") + FString(Paths[Index]),
						MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), NativeComponents[Index])),
						MakeShared<FJsonValueString>(TEXT("a finite number")));
			return NativeFailure(OutError, TEXT("rig_simulation_spatial_native_transform_invalid"),
				TEXT("The native transform quaternion is not normalized."), TEXT("/converted_transform/rotation"),
				MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
					MakeShared<FJsonValueNumber>(Rotation.X), MakeShared<FJsonValueNumber>(Rotation.Y),
					MakeShared<FJsonValueNumber>(Rotation.Z), MakeShared<FJsonValueNumber>(Rotation.W)}),
				MakeShared<FJsonValueString>(TEXT("a normalized quaternion")));
		}

		bool TryStringValue(
			const FString& Text,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const FString& Path)
		{
			FUeAgentStateSemanticError Error;
			if (FUeAgentCanonicalValue::TryString(Text, Context.GetResourceTracker(), OutValue, Error)) return true;
			NativeContext(Error, Path, true);
			OutError = FDiagnostic::WrapFailure(TEXT("rig_simulation_spatial_native_string_failed"), Error, TEXT("native_execution"));
			return false;
		}

		bool TrySnapshot(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			EUeAgentRigSimulationSpatialReadPhase Phase,
			const FString& PublicRef,
			const FString& State,
			const FUeAgentOperationContext& Context,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const FUeAgentCanonicalValuePtr& GeneratedAssetRefs = nullptr)
		{
			FUeAgentCanonicalValuePtr RefValue;
			FUeAgentCanonicalValuePtr CapabilityValue;
			FUeAgentCanonicalValuePtr StateValue;
			FUeAgentCanonicalValuePtr ObjectValue;
			FUeAgentStateSemanticError Error;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> ResultFields;
			if (!TryStringValue(PublicRef, Context, RefValue, OutError, TEXT("/public_ref"))
				|| !TryStringValue(Contract.GetCapabilityId(), Context,
					CapabilityValue, OutError, TEXT("/capability_id"))
				|| !TryStringValue(State, Context, StateValue, OutError, TEXT("/observed_state")))
			{
				return false;
			}
			ResultFields = {
				{TEXT("public_ref"), RefValue.ToSharedRef()},
				{TEXT("capability_id"), CapabilityValue.ToSharedRef()},
				{TEXT("observed_state"), StateValue.ToSharedRef()},
			};
			if (GeneratedAssetRefs.IsValid())
			{
				ResultFields.Add({
					TEXT("generated_asset_refs"),
					GeneratedAssetRefs.ToSharedRef(),
				});
			}
			if (!FUeAgentCanonicalValue::TryObject(
					ResultFields,
					Context.GetResourceTracker(), ObjectValue, Error))
			{
				NativeContext(Error, FString(), true);
				return ForwardFailure(OutError, MoveTemp(Error));
			}
			const uint64 Upper = FCrc::StrCrc32(*State);
			const uint64 Lower = FCrc::StrCrc32(*(Contract.GetReadbackId() + State));
			const FUeAgentStateRevision Revision(Contract.GetReadbackId(),
				TEXT("native-observation-v1"),
				static_cast<int64>(((Upper << 32) | Lower) & MAX_int64));
			const FUeAgentStateEntry Entry(
				FUeAgentCanonicalIdentity(
					TEXT("rig-simulation-spatial:") + Contract.GetCapabilityId(),
					TEXT("rig_simulation_spatial_target"), PublicRef, PublicRef,
					TEXT("result"), TEXT("native-observation-v1")),
				FUeAgentCanonicalType(Contract.GetTypedResultId(),
					EUeAgentValueKind::Object, false, TEXT("object")),
				ObjectValue.ToSharedRef(),
				FUeAgentDefaultValue(EUeAgentDefaultSourceKind::DomainNative,
					Contract.GetReadbackId(), TEXT("independent-native-readback"),
					ObjectValue.ToSharedRef()),
				FUeAgentStateDetails(TEXT("result"),
					TEXT("independent IK/Retarget native observation"),
					{TEXT("RigSimulationSpatial"), TEXT("IkRetarget")}, false,
					TEXT("available"), {}), Revision);
			if (FUeAgentCanonicalSnapshot::TryCreate(
				FString::Printf(TEXT("rig-simulation-spatial:%s:%d:%s"),
					*Contract.GetCapabilityId(), static_cast<int32>(Phase),
					*Context.GetRequestId()),
				Context.GetExecutionContext().GetStatePlane(), Revision,
				FUeAgentTemporalPoint(TEXT("editor-native"),
					FString::Printf(TEXT("%llu"), FPlatformTime::Cycles64())),
				FUeAgentStateProvenance(TEXT("independent_readback"),
					Contract.GetReadbackId(),
					TEXT("native-state:") + Contract.GetCapabilityId()),
				{Entry}, Context.GetResourceTracker(), OutSnapshot, Error)) return true;
			NativeContext(Error);
			return ForwardFailure(OutError, MoveTemp(Error));
		}

		FString SnapshotState(const FUeAgentCanonicalSnapshot& Snapshot)
		{
			if (Snapshot.GetEntries().IsEmpty()) return FString();
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Pair
				: Snapshot.GetEntries()[0].GetValue()->GetObjectItems())
			{
				if (Pair.Key == TEXT("observed_state")) return Pair.Value->GetLexeme();
			}
			return FString();
		}

		FUeAgentCanonicalValuePtr SnapshotResultField(
			const FUeAgentCanonicalSnapshot& Snapshot,
			const FString& Field)
		{
			if (Snapshot.GetEntries().IsEmpty()) return nullptr;
			const FUeAgentCanonicalValueRef& Root =
				Snapshot.GetEntries()[0].GetValue();
			if (Root->GetKind() != EUeAgentValueKind::Object) return nullptr;
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Pair
				: Root->GetObjectItems())
			{
				if (Pair.Key == Field) return Pair.Value;
			}
			return nullptr;
		}

		const FName* ResolveName(
			const TMap<FString, FName>& Bindings,
			const FString& PublicRef)
		{
			return Bindings.Find(PublicRef);
		}

		const FBoneChain* FindChain(
			const UIKRigController& Controller,
			const FName ChainName)
		{
			return Controller.GetRetargetChains().FindByPredicate(
				[ChainName](const FBoneChain& Chain)
				{
					return Chain.ChainName == ChainName;
				});
		}

		UScriptStruct* SolverStruct(const FString& Kind)
		{
			if (Kind == TEXT("full_body_ik")) return FIKRigFullBodyIKSolver::StaticStruct();
			if (Kind == TEXT("pole")) return FIKRigPoleSolver::StaticStruct();
			if (Kind == TEXT("set_transform")) return FIKRigSetTransform::StaticStruct();
			if (Kind == TEXT("body_mover")) return FIKRigBodyMoverSolver::StaticStruct();
			return nullptr;
		}

		int32 FindSolverIndex(UIKRigController& Controller, UScriptStruct* Type)
		{
			for (int32 Index = 0; Index < Controller.GetNumSolvers(); ++Index)
			{
				const FInstancedStruct* Solver = Controller.GetSolverStructAtIndex(Index);
				if (Solver && Solver->GetScriptStruct() == Type) return Index;
			}
			return INDEX_NONE;
		}

		bool ApplyIkDefinition(
			UIKRigDefinition& Rig,
			const FUeAgentRigSimulationSpatialIkRigBinding& Binding,
			const FRigIkDefinitionUpdateInput& Input,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			UIKRigController* Controller = UIKRigController::GetController(&Rig);
			if (!Controller) return MissingNative(OutError, TEXT("rig_simulation_spatial_ik_controller_unavailable"),
				TEXT("The IK Rig controller is unavailable."), TEXT("/controller"), Input.IkRigRef);
			for (int32 ChangeIndex = 0; ChangeIndex < Input.DefinitionChanges.Num(); ++ChangeIndex)
			{
				const auto& Change = Input.DefinitionChanges[ChangeIndex];
				const FString Path = TEXT("/definition_changes/") + FString::FromInt(ChangeIndex);
				const bool bSet = Change.Operation == TEXT("set");
				if (!bSet && Change.Operation != TEXT("remove"))
					return InputFailure(OutError, TEXT("rig_simulation_spatial_ik_definition_operation_invalid"),
						TEXT("The IK definition change uses an unsupported operation."), Path + TEXT("/operation"),
						MakeShared<FJsonValueString>(Change.Operation), MakeShared<FJsonValueString>(TEXT("set or remove")), Input.IkRigRef);
				if (Change.Kind == TEXT("preview_mesh"))
				{
					const FString MeshRef = !Input.PreviewMeshRef.IsEmpty()
						? Input.PreviewMeshRef : Change.PrimaryRef;
					USkeletalMesh* Mesh = bSet ? Cast<USkeletalMesh>(
						Environment.ResolveExplicitObject(MeshRef)) : nullptr;
					if (bSet && !Mesh) return InputFailure(OutError, TEXT("rig_simulation_spatial_ik_preview_mesh_unavailable"),
						TEXT("The preview mesh reference does not resolve to a skeletal mesh."),
						!Input.PreviewMeshRef.IsEmpty() ? TEXT("/preview_context/preview_mesh_ref") : Path + TEXT("/primary_ref"),
						MakeShared<FJsonValueString>(MeshRef), MakeShared<FJsonValueString>(TEXT("an available skeletal mesh reference")), Input.IkRigRef);
					if (Controller->GetSkeletalMesh() != Mesh && !Controller->SetSkeletalMesh(Mesh))
						return NativeFailure(OutError, TEXT("rig_simulation_spatial_ik_preview_mesh_apply_failed"),
							bSet ? TEXT("The IK Rig controller rejected the preview mesh assignment.") : TEXT("The IK Rig controller rejected clearing the preview mesh."),
							Path + TEXT("/preview_mesh_assignment"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), MeshRef);
				}
				else if (Change.Kind == TEXT("retarget_root"))
				{
					const FName* Root = ResolveName(Binding.BoneBindings, Change.PrimaryRef);
					if (!Root) return ReferenceFailure(OutError, TEXT("rig_simulation_spatial_ik_bone_binding_unavailable"),
						TEXT("The retarget root bone reference has no registered bone binding."), Path + TEXT("/primary_ref"), Change.PrimaryRef, Input.IkRigRef);
					const FName Desired = bSet ? *Root : NAME_None;
					UE::UAI::Contract::FDiagnostic IntentError;
					if (Controller->GetRetargetRoot() != Desired
						&& !UeAgentIKAssetOps::ApplyRetargetRoot(*Controller, Desired, IntentError,
							UE::UAI::Contract::FSourceLocation(), Input.IkRigRef))
						return NativeFailureAt(OutError, MoveTemp(IntentError), Path, Input.IkRigRef);
				}
				else if (Change.Kind == TEXT("goal"))
				{
					const FName* Goal = ResolveName(Binding.GoalBindings, Change.PrimaryRef);
					if (!Goal) return ReferenceFailure(OutError, TEXT("rig_simulation_spatial_ik_goal_binding_unavailable"),
						TEXT("The goal reference has no registered goal binding."), Path + TEXT("/primary_ref"), Change.PrimaryRef, Input.IkRigRef);
					if (bSet)
					{
						const FName* Bone = ResolveName(
							Binding.BoneBindings, Change.SecondaryRef);
						if (!Bone) return ReferenceFailure(OutError, TEXT("rig_simulation_spatial_ik_bone_binding_unavailable"),
							TEXT("The goal's bone reference has no registered bone binding."), Path + TEXT("/secondary_ref"), Change.SecondaryRef, Input.IkRigRef);
						if (Controller->GetGoal(*Goal))
						{
							if (Controller->GetBoneForGoal(*Goal) != *Bone && !Controller->SetGoalBone(*Goal, *Bone))
								return NativeFailure(OutError, TEXT("rig_simulation_spatial_ik_goal_apply_failed"),
									TEXT("The IK Rig controller rejected assigning the goal to the requested bone."), Path + TEXT("/goal_bone_assignment"),
									MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Change.PrimaryRef);
						}
						else if (Controller->AddNewGoal(*Goal, *Bone).IsNone())
							return MissingNative(OutError, TEXT("rig_simulation_spatial_ik_goal_apply_failed"),
								TEXT("The IK Rig controller did not create the requested goal."), Path + TEXT("/goal"), Change.PrimaryRef);
					}
					else
					{
						if (!Controller->GetGoal(*Goal)) return MissingNative(OutError, TEXT("rig_simulation_spatial_ik_goal_remove_failed"),
							TEXT("The goal requested for removal is absent."), Path + TEXT("/goal"), Change.PrimaryRef);
						if (!Controller->RemoveGoal(*Goal)) return NativeFailure(OutError, TEXT("rig_simulation_spatial_ik_goal_remove_failed"),
							TEXT("The IK Rig controller rejected removing the goal."), Path + TEXT("/goal_removal"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Change.PrimaryRef);
					}
				}
				else if (Change.Kind == TEXT("retarget_chain"))
				{
					const FName* Chain = ResolveName(Binding.ChainBindings, Change.PrimaryRef);
					if (!Chain) return ReferenceFailure(OutError, TEXT("rig_simulation_spatial_ik_chain_binding_unavailable"),
						TEXT("The chain reference has no registered chain binding."), Path + TEXT("/primary_ref"), Change.PrimaryRef, Input.IkRigRef);
					if (bSet)
					{
						const FName* Start = ResolveName(Binding.BoneBindings, Change.SecondaryRef);
						const FName* End = ResolveName(Binding.BoneBindings, Change.TertiaryRef);
						if (!Start || !End) return ReferenceFailure(OutError, TEXT("rig_simulation_spatial_ik_chain_bone_binding_unavailable"),
							!Start ? TEXT("The chain start bone has no registered binding.") : TEXT("The chain end bone has no registered binding."),
							Path + (!Start ? TEXT("/secondary_ref") : TEXT("/tertiary_ref")), !Start ? Change.SecondaryRef : Change.TertiaryRef, Change.PrimaryRef);
						if (FindChain(*Controller, *Chain)) Controller->RemoveRetargetChain(*Chain);
						if (Controller->AddRetargetChain(*Chain, *Start, *End, NAME_None).IsNone())
							return MissingNative(OutError, TEXT("rig_simulation_spatial_ik_chain_apply_failed"),
								TEXT("The IK Rig controller did not create the retarget chain."), Path + TEXT("/chain"), Change.PrimaryRef);
					}
					else
					{
						if (!FindChain(*Controller, *Chain)) return MissingNative(OutError, TEXT("rig_simulation_spatial_ik_chain_remove_failed"),
							TEXT("The retarget chain requested for removal is absent."), Path + TEXT("/chain"), Change.PrimaryRef);
						if (!Controller->RemoveRetargetChain(*Chain)) return NativeFailure(OutError, TEXT("rig_simulation_spatial_ik_chain_remove_failed"),
							TEXT("The IK Rig controller rejected removing the retarget chain."), Path + TEXT("/chain_removal"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Change.PrimaryRef);
					}
				}
				else if (Change.Kind == TEXT("solver"))
				{
					UScriptStruct* Type = SolverStruct(Change.SolverKind);
					if (!Type) return InputFailure(OutError, TEXT("rig_simulation_spatial_ik_solver_kind_invalid"),
						TEXT("The solver kind has no supported native solver type."), Path + TEXT("/solver_kind"),
						MakeShared<FJsonValueString>(Change.SolverKind), MakeShared<FJsonValueString>(TEXT("full_body_ik, pole, set_transform, or body_mover")), Input.IkRigRef);
					const int32 Existing = FindSolverIndex(*Controller, Type);
					if (bSet && Existing == INDEX_NONE && Controller->AddSolver(Type) == INDEX_NONE)
						return NativeFailure(OutError, TEXT("rig_simulation_spatial_ik_solver_apply_failed"),
							TEXT("The IK Rig controller returned no solver index when adding the solver."), Path + TEXT("/solver_index"),
							MakeShared<FJsonValueNumber>(INDEX_NONE), MakeShared<FJsonValueString>(TEXT("a valid solver index")), Change.PrimaryRef);
					if (!bSet)
					{
						if (Existing == INDEX_NONE) return MissingNative(OutError, TEXT("rig_simulation_spatial_ik_solver_remove_failed"),
							TEXT("The solver requested for removal is absent."), Path + TEXT("/solver"), Change.PrimaryRef);
						if (!Controller->RemoveSolver(Existing)) return NativeFailure(OutError, TEXT("rig_simulation_spatial_ik_solver_remove_failed"),
							TEXT("The IK Rig controller rejected removing the solver."), Path + TEXT("/solver_removal"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Change.PrimaryRef);
					}
				}
				else return InputFailure(OutError, TEXT("rig_simulation_spatial_ik_definition_kind_invalid"),
					TEXT("The IK definition change kind is unsupported."), Path + TEXT("/kind"),
					MakeShared<FJsonValueString>(Change.Kind),
					MakeShared<FJsonValueString>(TEXT("preview_mesh, retarget_root, goal, retarget_chain, or solver")), Input.IkRigRef);
			}
			return true;
		}

		bool ApplyRetargetDefinition(
			UIKRetargeter& Retargeter,
			const FUeAgentRigSimulationSpatialRetargeterBinding& Binding,
			const FRigRetargetDefinitionUpdateInput& Input,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Input.SourceRigRef == Input.TargetRigRef)
				return InputFailure(OutError, TEXT("rig_simulation_spatial_retarget_subject_binding_mismatch"),
					TEXT("Source and target rig references must differ."), TEXT("/target_rig_ref"),
					MakeShared<FJsonValueString>(Input.TargetRigRef), MakeShared<FJsonValueString>(TEXT("a rig other than ") + Input.SourceRigRef), Input.RetargeterRef);
			if (Input.SourceRigRef != Binding.SourceRigIdentity)
				return InputFailure(OutError, TEXT("rig_simulation_spatial_retarget_subject_binding_mismatch"),
					TEXT("The source rig reference differs from the retargeter binding."), TEXT("/source_rig_ref"),
					MakeShared<FJsonValueString>(Input.SourceRigRef), MakeShared<FJsonValueString>(Binding.SourceRigIdentity), Input.RetargeterRef);
			if (Input.TargetRigRef != Binding.TargetRigIdentity)
				return InputFailure(OutError, TEXT("rig_simulation_spatial_retarget_subject_binding_mismatch"),
					TEXT("The target rig reference differs from the retargeter binding."), TEXT("/target_rig_ref"),
					MakeShared<FJsonValueString>(Input.TargetRigRef), MakeShared<FJsonValueString>(Binding.TargetRigIdentity), Input.RetargeterRef);
			const auto* SourceBinding = Environment.FindIkRigBinding(Input.SourceRigRef);
			const auto* TargetBinding = Environment.FindIkRigBinding(Input.TargetRigRef);
			UIKRigDefinition* SourceRig = SourceBinding ? SourceBinding->Rig.Get() : nullptr;
			UIKRigDefinition* TargetRig = TargetBinding ? TargetBinding->Rig.Get() : nullptr;
			UIKRetargeterController* Controller =
				UIKRetargeterController::GetController(&Retargeter);
			if (!SourceRig || !TargetRig) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_subject_unavailable"),
				!SourceRig ? TEXT("The source IK Rig binding has no available rig.") : TEXT("The target IK Rig binding has no available rig."),
				!SourceRig ? TEXT("/source_rig_binding") : TEXT("/target_rig_binding"), !SourceRig ? Input.SourceRigRef : Input.TargetRigRef);
			if (SourceRig == TargetRig)
				return NativeFailure(OutError, TEXT("rig_simulation_spatial_retarget_subject_unavailable"),
					TEXT("Source and target rig bindings refer to the same native rig."), TEXT("/rig_bindings/distinct"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Input.RetargeterRef);
			if (!Controller) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_subject_unavailable"),
				TEXT("The retargeter controller is unavailable."), TEXT("/controller"), Input.RetargeterRef);
			Controller->SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
			Controller->SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);
			Controller->SetPreviewMesh(ERetargetSourceOrTarget::Source,
				Cast<USkeletalMesh>(Environment.ResolveExplicitObject(
					SourceBinding->PreviewMeshIdentity)));
			Controller->SetPreviewMesh(ERetargetSourceOrTarget::Target,
				Cast<USkeletalMesh>(Environment.ResolveExplicitObject(
					TargetBinding->PreviewMeshIdentity)));
			if (!Controller->GetChainMapping(NAME_None)) Controller->AddDefaultOps();
			UIKRigController* TargetController = UIKRigController::GetController(TargetRig);
			if (!TargetController) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_target_controller_unavailable"),
				TEXT("The target IK Rig controller is unavailable."), TEXT("/target_rig/controller"), Input.TargetRigRef);
			UE::UAI::Contract::FDiagnostic MappingError;
			Controller->AutoMapChains(EAutoMapChainType::Clear, true);
			for (int32 MappingIndex = 0; MappingIndex < Input.ChainMappings.Num(); ++MappingIndex)
			{
				const auto& Mapping = Input.ChainMappings[MappingIndex];
				const FString Path = TEXT("/chain_mappings/") + FString::FromInt(MappingIndex);
				const FName* SourceChain = ResolveName(
					SourceBinding->ChainBindings, Mapping.SourceChainRef);
				const FName* TargetChain = ResolveName(
					TargetBinding->ChainBindings, Mapping.TargetChainRef);
				if (!SourceChain || !TargetChain) return ReferenceFailure(OutError, TEXT("rig_simulation_spatial_retarget_chain_binding_unavailable"),
					!SourceChain ? TEXT("The source chain reference has no registered binding.") : TEXT("The target chain reference has no registered binding."),
					Path + (!SourceChain ? TEXT("/source_chain_ref") : TEXT("/target_chain_ref")),
					!SourceChain ? Mapping.SourceChainRef : Mapping.TargetChainRef, Input.RetargeterRef);
				UIKRigController* SourceController = UIKRigController::GetController(SourceRig);
				if (!SourceController) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_source_controller_unavailable"),
					TEXT("The source IK Rig controller is unavailable for chain lookup."), Path + TEXT("/source_controller"), Input.SourceRigRef);
				if (!FindChain(*SourceController, *SourceChain)) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_chain_binding_unavailable"),
					TEXT("The registered source chain is absent from the IK Rig."), Path + TEXT("/source_chain"), Mapping.SourceChainRef);
				const FBoneChain* TargetChainValue = FindChain(*TargetController, *TargetChain);
				if (!TargetChainValue) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_chain_binding_unavailable"),
					TEXT("The registered target chain is absent from the IK Rig."), Path + TEXT("/target_chain"), Mapping.TargetChainRef);
				if (!Mapping.GoalRef.IsEmpty())
				{
					const FName* Goal = ResolveName(TargetBinding->GoalBindings, Mapping.GoalRef);
					if (!Goal) return ReferenceFailure(OutError, TEXT("rig_simulation_spatial_retarget_goal_binding_mismatch"),
						TEXT("The goal reference has no registered goal binding."), Path + TEXT("/goal_ref"), Mapping.GoalRef, Input.RetargeterRef);
					if (TargetChainValue->IKGoalName != *Goal)
					{
						NativeFailure(OutError, TEXT("rig_simulation_spatial_retarget_goal_binding_mismatch"),
							TEXT("The target chain's goal differs from the requested registered goal."), Path + TEXT("/goal_ref"),
							nullptr, MakeShared<FJsonValueString>(Mapping.GoalRef), Mapping.TargetChainRef);
						OutError.ActualEvidence = MakeShared<FJsonValueString>(TargetChainValue->IKGoalName.ToString());
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Goal->ToString());
						return false;
					}
				}
				UeAgentIKAssetOps::FRetargetMappingDiagnosticSource Source;
				Source.TargetPath = Input.RetargeterRef;
				if (!UeAgentIKAssetOps::ApplyRetargetChainMapping(*Controller, *SourceChain, *TargetChain,
					NAME_None, MappingError, Source)) return NativeFailureAt(OutError, MoveTemp(MappingError), Path, Input.RetargeterRef);
			}
			return true;
		}

		bool SideFacts(
			const FString& SubjectSide,
			const FUeAgentRigSimulationSpatialRetargeterBinding& RetargeterBinding,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			ERetargetSourceOrTarget& OutSide,
			const FUeAgentRigSimulationSpatialIkRigBinding*& OutRigBinding,
			FString& OutNativeFrame,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (SubjectSide == TEXT("source"))
			{
				OutSide = ERetargetSourceOrTarget::Source;
				OutRigBinding = Environment.FindIkRigBinding(
					RetargeterBinding.SourceRigIdentity);
				OutNativeFrame = RetargeterBinding.SourceNativePoseFrameId;
			}
			else if (SubjectSide == TEXT("target"))
			{
				OutSide = ERetargetSourceOrTarget::Target;
				OutRigBinding = Environment.FindIkRigBinding(
					RetargeterBinding.TargetRigIdentity);
				OutNativeFrame = RetargeterBinding.TargetNativePoseFrameId;
			}
			else return InputFailure(OutError, TEXT("rig_simulation_spatial_retarget_subject_side_invalid"),
				TEXT("The retarget subject side is unsupported."), TEXT("/subject_side"),
				MakeShared<FJsonValueString>(SubjectSide), MakeShared<FJsonValueString>(TEXT("source or target")), RetargeterBinding.Identity);
			if (!OutRigBinding) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_subject_binding_unavailable"),
				TEXT("The selected retarget side has no registered IK Rig binding."),
				SubjectSide == TEXT("source") ? TEXT("/source_rig_binding") : TEXT("/target_rig_binding"), RetargeterBinding.Identity);
			if (OutNativeFrame.IsEmpty())
				return NativeFailure(OutError, TEXT("rig_simulation_spatial_retarget_pose_frame_unavailable"),
					TEXT("The selected retarget side has no native pose frame identifier."),
					SubjectSide == TEXT("source") ? TEXT("/source_native_pose_frame_id") : TEXT("/target_native_pose_frame_id"),
					MakeShared<FJsonValueString>(OutNativeFrame), MakeShared<FJsonValueString>(TEXT("a nonempty native frame identifier")), RetargeterBinding.Identity);
			return true;
		}

		bool DesiredPoseTransform(
			const FRigSimulationSpatialPoseItem& Item,
			const FUeAgentRigSimulationSpatialRequest& Request,
			const FString& NativeFrame,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentResourceTracker& Resources,
			FTransform& OutTransform,
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const FString& Path)
		{
			if (Item.BoneRef != Item.Transform.Subject)
				return InputFailure(OutError, TEXT("rig_simulation_spatial_retarget_pose_subject_mismatch"),
					TEXT("The pose transform subject differs from the pose item's bone reference."), Path + TEXT("/transform/subject"),
					MakeShared<FJsonValueString>(Item.Transform.Subject), MakeShared<FJsonValueString>(Item.BoneRef), Item.BoneRef);
			const FUeAgentSpatialValuePtr* Value = Request.GetFormalPortRequest()
				.GetSpatialValues().FindByPredicate(
					[&Item](const FUeAgentSpatialValuePtr& Candidate)
					{
						return Candidate.IsValid()
							&& Candidate->GetSubject() == Item.Transform.Subject
							&& Candidate->GetQuantityKind()
								== EUeAgentSpatialQuantityKind::Transform;
					});
			if (!Value) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_pose_formal_value_missing"),
				TEXT("The formal request contains no transform for this pose bone."), TEXT("/formal_request/spatial_values"), Item.BoneRef);
			FUeAgentSpatialValuePtr Native;
			if (!Environment.TryConvertSpatialValueToFrame(*Value->ToSharedRef(), NativeFrame, Resources, Native, OutError)) return false;
			if (!Native.IsValid()) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_pose_native_value_missing"),
				TEXT("Spatial conversion returned no native pose transform."), TEXT("/converted_transform"), Item.BoneRef);
			if (!TryNativeTransform(*Native, OutTransform, OutError))
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Item.BoneRef;
				return false;
			}
			return true;
		}

		bool ApplyRetargetPose(
			UIKRetargeter& Retargeter,
			const FUeAgentRigSimulationSpatialRetargeterBinding& RetargeterBinding,
			const FRigRetargetPoseUpdateInput& Input,
			const FUeAgentRigSimulationSpatialRequest& Request,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentResourceTracker& Resources,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			ERetargetSourceOrTarget Side;
			const FUeAgentRigSimulationSpatialIkRigBinding* RigBinding = nullptr;
			FString NativeFrame;
			if (!SideFacts(Input.SubjectSide, RetargeterBinding, Environment,
				Side, RigBinding, NativeFrame, OutError)) return false;
			UIKRetargeterController* Controller =
				UIKRetargeterController::GetController(&Retargeter);
			UIKRigController* RigController = RigBinding
				? UIKRigController::GetController(RigBinding->Rig.Get()) : nullptr;
			UeAgentIKAssetOps::FRetargetPoseDiagnosticSource PoseSource;
			PoseSource.TargetPath = Input.RetargeterRef;
			if (!Controller || !RigController) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_pose_controller_unavailable"),
				!Controller ? TEXT("The retargeter controller is unavailable for the pose update.")
					: TEXT("The selected IK Rig controller is unavailable for the pose update."),
				!Controller ? TEXT("/controller") : TEXT("/rig_controller"), Input.RetargeterRef);
			const FName RootBone = RigController->GetRetargetRoot();
			for (int32 PoseIndex = 0; PoseIndex < Input.Pose.Num(); ++PoseIndex)
			{
				const auto& Item = Input.Pose[PoseIndex];
				const FString Path = TEXT("/pose/") + FString::FromInt(PoseIndex);
				const FName* Bone = ResolveName(RigBinding->BoneBindings, Item.BoneRef);
				FTransform Transform;
				if (!Bone) return ReferenceFailure(OutError, TEXT("rig_simulation_spatial_retarget_pose_bone_unresolved"),
					TEXT("The pose bone reference has no binding in the selected IK Rig."), Path + TEXT("/bone_ref"), Item.BoneRef, Input.RetargeterRef);
				if (!DesiredPoseTransform(Item, Request, NativeFrame,
					Environment, Resources, Transform, OutError, Path)) return false;
				if (!Transform.GetScale3D().Equals(FVector::OneVector, 0.001))
					return NativeFailure(OutError, TEXT("rig_simulation_spatial_retarget_pose_scale_unsupported"),
						TEXT("A retarget pose transform requires unit scale within tolerance 0.001."), Path + TEXT("/converted_transform/scale"),
						MakeShared<FJsonValueObject>(UeAgentIKAssetOps::BuildVectorJson(Transform.GetScale3D())),
						MakeShared<FJsonValueObject>(UeAgentIKAssetOps::BuildVectorJson(FVector::OneVector)), Item.BoneRef);
				if (*Bone == RootBone)
				{
					const FVector Translation = Transform.GetTranslation();
					UE::UAI::Contract::FDiagnostic PoseError;
					if (!UeAgentIKAssetOps::ApplyRetargetPoseRootTranslation(*Controller, Side,
						Translation, PoseError, PoseSource))
						return NativeFailureAt(OutError, MoveTemp(PoseError), Path + TEXT("/converted_transform"), Item.BoneRef);
				}
				else if (!Transform.GetTranslation().IsNearlyZero(0.001))
					return NativeFailure(OutError, TEXT("rig_simulation_spatial_retarget_pose_non_root_translation_unsupported"),
						TEXT("A non-root retarget pose bone requires zero translation within tolerance 0.001."), Path + TEXT("/converted_transform/translation"),
						MakeShared<FJsonValueObject>(UeAgentIKAssetOps::BuildVectorJson(Transform.GetTranslation())),
						MakeShared<FJsonValueObject>(UeAgentIKAssetOps::BuildVectorJson(FVector::ZeroVector)), Item.BoneRef);
				UE::UAI::Contract::FDiagnostic PoseError;
				if (!UeAgentIKAssetOps::ApplyRetargetPoseRotation(*Controller,
					Side, *Bone, Transform.GetRotation(), PoseError, PoseSource))
					return NativeFailureAt(OutError, MoveTemp(PoseError), Path + TEXT("/converted_transform"), Item.BoneRef);
			}
			return true;
		}

		bool ApplyAutoAlign(
			UIKRetargeter& Retargeter,
			const FUeAgentRigSimulationSpatialRetargeterBinding& RetargeterBinding,
			const FRigRetargetPoseAutoAlignInput& Input,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			ERetargetSourceOrTarget Side;
			const FUeAgentRigSimulationSpatialIkRigBinding* RigBinding = nullptr;
			FString NativeFrame;
			if (!SideFacts(Input.SubjectSide, RetargeterBinding, Environment,
				Side, RigBinding, NativeFrame, OutError)) return false;
			UIKRetargeterController* Controller =
				UIKRetargeterController::GetController(&Retargeter);
			if (!Controller) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_pose_controller_unavailable"),
				TEXT("The retargeter controller is unavailable for pose alignment."), TEXT("/controller"), Input.RetargeterRef);
			TArray<FName> Bones;
			for (int32 BoneIndex = 0; BoneIndex < Input.BoneRefs.Num(); ++BoneIndex)
			{
				const FString& BoneRef = Input.BoneRefs[BoneIndex];
				const FName* Bone = ResolveName(RigBinding->BoneBindings, BoneRef);
				if (!Bone) return ReferenceFailure(OutError, TEXT("rig_simulation_spatial_retarget_pose_bone_binding_unavailable"),
					TEXT("The alignment bone reference has no binding in the selected IK Rig."),
					TEXT("/bone_refs/") + FString::FromInt(BoneIndex), BoneRef, Input.RetargeterRef);
				Bones.Add(*Bone);
			}
			if (Input.AlignmentPolicy == TEXT("root"))
			{
				if (Bones.Num() != 1) return InputFailure(OutError, TEXT("rig_simulation_spatial_retarget_root_alignment_bone_required"),
					TEXT("Root alignment requires exactly one bone reference."), TEXT("/bone_refs"),
					MakeShared<FJsonValueNumber>(Bones.Num()), MakeShared<FJsonValueNumber>(1), Input.RetargeterRef);
				Controller->SnapBoneToGround(Bones[0], Side);
			}
			else if (Input.AlignmentPolicy == TEXT("chain"))
			{
				if (Bones.IsEmpty()) return InputFailure(OutError, TEXT("rig_simulation_spatial_retarget_chain_alignment_bones_required"),
					TEXT("Chain alignment requires at least one bone reference."), TEXT("/bone_refs"),
					MakeShared<FJsonValueNumber>(Bones.Num()), MakeShared<FJsonValueString>(TEXT(">= 1")), Input.RetargeterRef);
				Controller->AutoAlignBones(Bones,
					ERetargetAutoAlignMethod::ChainToChain, Side);
			}
			else if (Input.AlignmentPolicy == TEXT("full_pose"))
			{
				if (!Bones.IsEmpty()) return InputFailure(OutError, TEXT("rig_simulation_spatial_retarget_full_pose_alignment_bones_forbidden"),
					TEXT("Full-pose alignment does not accept individual bone references."), TEXT("/bone_refs"),
					MakeShared<FJsonValueNumber>(Bones.Num()), MakeShared<FJsonValueNumber>(0), Input.RetargeterRef);
				Controller->AutoAlignAllBones(Side,
					ERetargetAutoAlignMethod::ChainToChain);
			}
			else return InputFailure(OutError, TEXT("rig_simulation_spatial_retarget_alignment_policy_invalid"),
				TEXT("The pose alignment policy is unsupported."), TEXT("/alignment_policy"),
				MakeShared<FJsonValueString>(Input.AlignmentPolicy), MakeShared<FJsonValueString>(TEXT("root, chain, or full_pose")), Input.RetargeterRef);
			return true;
		}

		FString AssetObjectPath(const FString& Root, const FString& Name)
		{
			return Root / Name + TEXT(".") + Name;
		}

		FString ReadState(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FUeAgentRigSimulationSpatialRequest& Request,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentResourceTracker& Resources,
			FDiagnostic& OutDifference,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Contract.GetCapabilityId() == TEXT("rig.ik.definition.update"))
			{
				const auto& Input = Request.GetPayload().Get<FRigIkDefinitionUpdateInput>();
				const auto* Binding = Environment.FindIkRigBinding(Input.IkRigRef);
				UIKRigController* Controller = Binding
					? UIKRigController::GetController(Binding->Rig.Get()) : nullptr;
				if (!Binding || !Controller) return MissingNative(OutError, TEXT("rig_simulation_spatial_ik_readback_unavailable"),
					!Binding ? TEXT("The IK Rig reference has no registered binding.") : TEXT("The IK Rig controller is unavailable for readback."),
					!Binding ? TEXT("/ik_rig_binding") : TEXT("/controller"), Input.IkRigRef), FString();
				bool bMatches = true;
				for (int32 ChangeIndex = 0; ChangeIndex < Input.DefinitionChanges.Num(); ++ChangeIndex)
				{
					const auto& Change = Input.DefinitionChanges[ChangeIndex];
					const FString Path = TEXT("/definition_changes/") + FString::FromInt(ChangeIndex);
					const bool bSet = Change.Operation == TEXT("set");
					if (Change.Kind == TEXT("preview_mesh"))
					{
						UObject* Mesh = Environment.ResolveExplicitObject(
							!Input.PreviewMeshRef.IsEmpty()
								? Input.PreviewMeshRef : Change.PrimaryRef);
						bMatches &= MatchNativeObject(Controller->GetSkeletalMesh(), bSet ? Mesh : nullptr,
							Path + TEXT("/preview_mesh"), TEXT("The observed IK preview mesh differs from the requested state."),
							bSet ? (!Input.PreviewMeshRef.IsEmpty() ? Input.PreviewMeshRef : Change.PrimaryRef) : TEXT("no preview mesh"),
							Input.IkRigRef, OutDifference);
					}
					else if (Change.Kind == TEXT("retarget_root"))
					{
						const FName* Name = ResolveName(Binding->BoneBindings,
							Change.PrimaryRef);
						bMatches &= MatchFact(Name != nullptr, OutDifference, TEXT("The retarget root reference has no registered bone binding."),
							Path + TEXT("/primary_ref"), MakeShared<FJsonValueString>(Change.PrimaryRef),
							MakeShared<FJsonValueString>(TEXT("a registered bone reference")), Input.IkRigRef)
							&& MatchNativeName(Controller->GetRetargetRoot(), bSet ? *Name : NAME_None, Path + TEXT("/retarget_root"),
								TEXT("The observed retarget root differs from the requested state."), bSet ? Change.PrimaryRef : TEXT("no retarget root"), Input.IkRigRef, OutDifference);
					}
					else if (Change.Kind == TEXT("goal"))
					{
						const FName* Goal = ResolveName(Binding->GoalBindings,
							Change.PrimaryRef);
						const FName* Bone = ResolveName(Binding->BoneBindings,
							Change.SecondaryRef);
						bMatches &= MatchFact(Goal != nullptr, OutDifference, TEXT("The goal reference has no registered binding."),
							Path + TEXT("/primary_ref"), MakeShared<FJsonValueString>(Change.PrimaryRef),
							MakeShared<FJsonValueString>(TEXT("a registered goal reference")), Input.IkRigRef)
							&& (bSet
								? MatchFact(Bone != nullptr, OutDifference, TEXT("The goal's bone reference has no registered binding."),
									Path + TEXT("/secondary_ref"), MakeShared<FJsonValueString>(Change.SecondaryRef),
									MakeShared<FJsonValueString>(TEXT("a registered bone reference")), Change.PrimaryRef)
									&& MatchFact(Controller->GetGoal(*Goal) != nullptr, OutDifference, TEXT("The requested IK goal is absent."),
										Path + TEXT("/goal/present"), MakeShared<FJsonValueBoolean>(false),
										MakeShared<FJsonValueBoolean>(true), Change.PrimaryRef)
									&& MatchNativeName(Controller->GetBoneForGoal(*Goal), *Bone, Path + TEXT("/goal/bone"),
										TEXT("The goal is attached to a different bone."), Change.SecondaryRef, Change.PrimaryRef, OutDifference)
								: MatchFact(!Controller->GetGoal(*Goal), OutDifference, TEXT("The removed IK goal is still present."),
									Path + TEXT("/goal/present"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Change.PrimaryRef));
					}
					else if (Change.Kind == TEXT("retarget_chain"))
					{
						const FName* Chain = ResolveName(Binding->ChainBindings,
							Change.PrimaryRef);
						bMatches &= MatchFact(Chain != nullptr, OutDifference, TEXT("The retarget chain reference has no registered binding."),
							Path + TEXT("/primary_ref"), MakeShared<FJsonValueString>(Change.PrimaryRef),
							MakeShared<FJsonValueString>(TEXT("a registered chain reference")), Input.IkRigRef)
							&& MatchFact((FindChain(*Controller, *Chain) != nullptr) == bSet, OutDifference,
								bSet ? TEXT("The requested retarget chain is absent.") : TEXT("The removed retarget chain is still present."),
								Path + TEXT("/chain/present"), MakeShared<FJsonValueBoolean>(!bSet), MakeShared<FJsonValueBoolean>(bSet), Change.PrimaryRef);
					}
					else if (Change.Kind == TEXT("solver"))
					{
						const int32 Index = FindSolverIndex(*Controller,
							SolverStruct(Change.SolverKind));
						bMatches &= MatchFact((Index != INDEX_NONE) == bSet, OutDifference,
							bSet ? TEXT("The requested IK solver is absent.") : TEXT("The removed IK solver is still present."),
							Path + TEXT("/solver/present"), MakeShared<FJsonValueBoolean>(Index != INDEX_NONE),
							MakeShared<FJsonValueBoolean>(bSet), Change.PrimaryRef);
					}
				}
				return FString::Printf(TEXT("ik=%s|matches=%d|root=%s|solvers=%d"),
					*Input.IkRigRef, bMatches ? 1 : 0,
					*Controller->GetRetargetRoot().ToString(), Controller->GetNumSolvers());
			}
			if (Contract.GetCapabilityId() == TEXT("rig.retarget.definition.update"))
			{
				const auto& Input = Request.GetPayload()
					.Get<FRigRetargetDefinitionUpdateInput>();
				const auto* Binding = Environment.FindRetargeterBinding(Input.RetargeterRef);
				UIKRetargeterController* Controller = Binding
					? UIKRetargeterController::GetController(Binding->Retargeter.Get()) : nullptr;
				UIKRigDefinition* Source = Cast<UIKRigDefinition>(
					Environment.ResolveExplicitObject(Input.SourceRigRef));
				UIKRigDefinition* Target = Cast<UIKRigDefinition>(
					Environment.ResolveExplicitObject(Input.TargetRigRef));
				bool bMatches = MatchFact(Controller != nullptr, OutDifference, TEXT("The retargeter controller is unavailable."),
					TEXT("/controller"), MakeShared<FJsonValueBoolean>(Controller != nullptr), MakeShared<FJsonValueBoolean>(true), Input.RetargeterRef)
					&& MatchFact(Source != nullptr, OutDifference, TEXT("The source rig reference does not resolve to an IK Rig."),
						TEXT("/source_rig_ref"), MakeShared<FJsonValueString>(Input.SourceRigRef), MakeShared<FJsonValueString>(TEXT("an available IK Rig reference")), Input.RetargeterRef)
					&& MatchFact(Target != nullptr, OutDifference, TEXT("The target rig reference does not resolve to an IK Rig."),
						TEXT("/target_rig_ref"), MakeShared<FJsonValueString>(Input.TargetRigRef), MakeShared<FJsonValueString>(TEXT("an available IK Rig reference")), Input.RetargeterRef)
					&& MatchFact(Source != Target, OutDifference, TEXT("The source and target references resolve to the same IK Rig."),
						TEXT("/rig_bindings/distinct"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Input.RetargeterRef)
					&& MatchNativeObject(Controller->GetIKRig(ERetargetSourceOrTarget::Source), Source, TEXT("/source_rig"),
						TEXT("The retargeter uses a different source rig."), Input.SourceRigRef, Input.RetargeterRef, OutDifference)
					&& MatchNativeObject(Controller->GetIKRig(ERetargetSourceOrTarget::Target), Target, TEXT("/target_rig"),
						TEXT("The retargeter uses a different target rig."), Input.TargetRigRef, Input.RetargeterRef, OutDifference);
				if (Controller)
				{
					const auto* SourceBinding = Environment.FindIkRigBinding(Input.SourceRigRef);
					const auto* TargetBinding = Environment.FindIkRigBinding(Input.TargetRigRef);
					for (int32 MappingIndex = 0; MappingIndex < Input.ChainMappings.Num(); ++MappingIndex)
					{
						const auto& Mapping = Input.ChainMappings[MappingIndex];
						const FString Path = TEXT("/chain_mappings/") + FString::FromInt(MappingIndex);
						const FName* SourceName = SourceBinding
							? ResolveName(SourceBinding->ChainBindings,
								Mapping.SourceChainRef) : nullptr;
						const FName* TargetName = TargetBinding
							? ResolveName(TargetBinding->ChainBindings,
								Mapping.TargetChainRef) : nullptr;
						bMatches &= MatchFact(SourceName != nullptr, OutDifference, TEXT("The source chain reference has no registered binding."),
							Path + TEXT("/source_chain_ref"), MakeShared<FJsonValueString>(Mapping.SourceChainRef),
							MakeShared<FJsonValueString>(TEXT("a registered source chain reference")), Input.RetargeterRef)
							&& MatchFact(TargetName != nullptr, OutDifference, TEXT("The target chain reference has no registered binding."),
								Path + TEXT("/target_chain_ref"), MakeShared<FJsonValueString>(Mapping.TargetChainRef),
								MakeShared<FJsonValueString>(TEXT("a registered target chain reference")), Input.RetargeterRef)
							&& MatchNativeName(Controller->GetSourceChain(*TargetName), *SourceName, Path + TEXT("/source_chain_ref"),
								TEXT("The target chain is mapped to a different source chain."), Mapping.SourceChainRef, Mapping.TargetChainRef, OutDifference);
					}
				}
				return FString::Printf(
					TEXT("retargeter=%s|source=%s|target=%s|matches=%d"),
					*Input.RetargeterRef, *Input.SourceRigRef, *Input.TargetRigRef,
					bMatches ? 1 : 0);
			}
			if (Contract.GetCapabilityId() == TEXT("rig.retarget.pose.update"))
			{
				const auto& Input = Request.GetPayload().Get<FRigRetargetPoseUpdateInput>();
				const auto* Binding = Environment.FindRetargeterBinding(Input.RetargeterRef);
				ERetargetSourceOrTarget Side;
				const FUeAgentRigSimulationSpatialIkRigBinding* RigBinding = nullptr;
				FString NativeFrame;
				if (!Binding) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_binding_unavailable"),
					TEXT("The retargeter reference has no registered binding."), TEXT("/retargeter_binding"), Input.RetargeterRef), FString();
				if (!SideFacts(Input.SubjectSide, *Binding, Environment, Side, RigBinding, NativeFrame, OutError)) return FString();
				UIKRetargeterController* Controller =
					UIKRetargeterController::GetController(Binding->Retargeter.Get());
				UIKRigController* RigController = RigBinding
					? UIKRigController::GetController(RigBinding->Rig.Get()) : nullptr;
				if (!Controller || !RigController) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_pose_controller_unavailable"),
					!Controller ? TEXT("The retargeter controller is unavailable for pose readback.") : TEXT("The selected IK Rig controller is unavailable for pose readback."),
					!Controller ? TEXT("/controller") : TEXT("/rig_controller"), Input.RetargeterRef), FString();
				bool bMatches = true;
				for (int32 PoseIndex = 0; PoseIndex < Input.Pose.Num(); ++PoseIndex)
				{
					const auto& Item = Input.Pose[PoseIndex];
					const FString Path = TEXT("/pose/") + FString::FromInt(PoseIndex);
					const FName* Bone = ResolveName(RigBinding->BoneBindings, Item.BoneRef);
					FTransform Desired;
					if (!Bone) return ReferenceFailure(OutError, TEXT("rig_simulation_spatial_retarget_pose_bone_unresolved"),
						TEXT("The pose bone reference has no binding in the selected IK Rig."), Path + TEXT("/bone_ref"), Item.BoneRef, Input.RetargeterRef), FString();
					if (!DesiredPoseTransform(Item, Request, NativeFrame, Environment, Resources, Desired, OutError, Path)) return FString();
					const FQuat ActualRotation = Controller->GetRotationOffsetForRetargetPoseBone(*Bone, Side);
					bMatches &= MatchFact(ActualRotation.Equals(Desired.GetRotation(), 0.001), OutDifference,
						TEXT("The observed pose rotation differs from the requested native rotation beyond tolerance 0.001."),
						Path + TEXT("/converted_transform/rotation"), RotationFact(ActualRotation), RotationFact(Desired.GetRotation()), Item.BoneRef);
					if (*Bone == RigController->GetRetargetRoot())
					{
						const FVector ActualTranslation = Controller->GetRootOffsetInRetargetPose(Side);
						bMatches &= MatchFact(ActualTranslation.Equals(Desired.GetTranslation(), 0.001), OutDifference,
							TEXT("The observed root translation differs from the requested native translation beyond tolerance 0.001."),
							Path + TEXT("/converted_transform/translation"), VectorFact(ActualTranslation), VectorFact(Desired.GetTranslation()), Item.BoneRef);
					}
				}
				const FVector Root = Controller->GetRootOffsetInRetargetPose(Side);
				return FString::Printf(TEXT("retargeter=%s|side=%s|root=%.9g,%.9g,%.9g|matches=%d"),
					*Input.RetargeterRef, *Input.SubjectSide, Root.X, Root.Y, Root.Z,
					bMatches ? 1 : 0);
			}
			if (Contract.GetCapabilityId() == TEXT("rig.retarget.pose.auto_align"))
			{
				const auto& Input = Request.GetPayload()
					.Get<FRigRetargetPoseAutoAlignInput>();
				const auto* Binding = Environment.FindRetargeterBinding(Input.RetargeterRef);
				ERetargetSourceOrTarget Side;
				const FUeAgentRigSimulationSpatialIkRigBinding* RigBinding = nullptr;
				FString NativeFrame;
				if (!Binding) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_binding_unavailable"),
					TEXT("The retargeter reference has no registered binding."), TEXT("/retargeter_binding"), Input.RetargeterRef), FString();
				if (!SideFacts(Input.SubjectSide, *Binding, Environment, Side, RigBinding, NativeFrame, OutError)) return FString();
				UIKRetargeterController* Controller =
					UIKRetargeterController::GetController(Binding->Retargeter.Get());
				if (!Controller) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_pose_controller_unavailable"),
					TEXT("The retargeter controller is unavailable for alignment readback."), TEXT("/controller"), Input.RetargeterRef), FString();
				const FVector Root = Controller->GetRootOffsetInRetargetPose(Side);
				return FString::Printf(TEXT("retargeter=%s|side=%s|root=%.9g,%.9g,%.9g|matches=1"),
					*Input.RetargeterRef, *Input.SubjectSide, Root.X, Root.Y, Root.Z);
			}
			if (Contract.GetCapabilityId() == TEXT("rig.retarget.assets.generate"))
			{
				const auto& Input = Request.GetPayload()
					.Get<FRigRetargetAssetsGenerateInput>();
				USkeletalMesh* Target = Cast<USkeletalMesh>(
					Environment.ResolveExplicitObject(Input.TargetSubjectRef));
				bool bMatches = MatchFact(Target != nullptr, OutDifference, TEXT("The target subject does not resolve to a skeletal mesh."),
					TEXT("/target_subject_ref"), MakeShared<FJsonValueString>(Input.TargetSubjectRef),
					MakeShared<FJsonValueString>(TEXT("an available skeletal mesh reference")), Input.DestinationRoot);
				int32 GeneratedCount = 0;
				for (int32 AssetIndex = 0; AssetIndex < Input.AssetRequests.Num(); ++AssetIndex)
				{
					const auto& Item = Input.AssetRequests[AssetIndex];
					const FString ItemPath = TEXT("/asset_requests/") + FString::FromInt(AssetIndex);
					const FString Path = AssetObjectPath(Input.DestinationRoot, Item.Name);
					UAnimSequence* Animation = FindObject<UAnimSequence>(nullptr, *Path);
					const bool bAssetMatches = MatchFact(Item.AssetKind == TEXT("animation_sequence"), OutDifference,
						TEXT("The generated asset kind is unsupported by this readback."), ItemPath + TEXT("/asset_kind"),
						MakeShared<FJsonValueString>(Item.AssetKind), MakeShared<FJsonValueString>(TEXT("animation_sequence")), Input.DestinationRoot)
						&& MatchFact(Animation != nullptr, OutDifference, TEXT("The requested generated animation is absent."),
							ItemPath + TEXT("/generated_asset"), MakeShared<FJsonValueBoolean>(Animation != nullptr),
							MakeShared<FJsonValueBoolean>(true), Path)
						// The missing target has already produced a mismatch; do not dereference it.
						&& Target
						&& MatchNativeObject(Animation->GetSkeleton(), Target->GetSkeleton(), ItemPath + TEXT("/skeleton"),
							TEXT("The generated animation uses a different skeleton than the target subject."), Input.TargetSubjectRef, Path, OutDifference)
						&& MatchFact(Animation->GetDataModel() != nullptr, OutDifference, TEXT("The generated animation has no data model."),
							ItemPath + TEXT("/data_model"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Path);
					if (bAssetMatches)
					{
						const int32 KeyCount = Animation->GetDataModel()->GetNumberOfKeys();
						bMatches &= MatchFact(KeyCount > 0, OutDifference, TEXT("The generated animation has no keys."),
							ItemPath + TEXT("/keys"), MakeShared<FJsonValueNumber>(KeyCount), MakeShared<FJsonValueString>(TEXT("> 0")), Path);
					}
					else bMatches = false;
					GeneratedCount += Animation ? 1 : 0;
				}
				return FString::Printf(TEXT("destination=%s|generated=%d|matches=%d"),
					*Input.DestinationRoot, GeneratedCount, bMatches ? 1 : 0);
			}
			UnsupportedCapability(OutError, Contract.GetCapabilityId());
			return FString();
		}

		class FReadback final : public IUeAgentRigSimulationSpatialReadbackPort
		{
		public:
			explicit FReadback(
				TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}

			bool TryRead(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const TOptional<FUeAgentCandidateBinding>& Target,
				EUeAgentRigSimulationSpatialReadPhase Phase,
				const FUeAgentOperationContext& Context,
				TOptional<FUeAgentRigSimulationSpatialReadbackResult>& OutReadback,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutReadback.Reset();
				TSharedPtr<const FUeAgentCanonicalSnapshot> OutSnapshot;
				OutError = {};
				Environment->CountFamilyDispatch();
				if (!IsCapability(Contract.GetCapabilityId())) return UnsupportedCapability(OutError, Contract.GetCapabilityId());
				const FUeAgentRigSimulationSpatialRequest* Typed =
					ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const FString PublicRef = Typed->GetPrimaryTargetRef();
				if (PublicRef.IsEmpty()
					|| (Target.IsSet() && Target->GetPublicRef() != PublicRef))
					return NativeFailure(OutError, TEXT("rig_simulation_spatial_explicit_target_mismatch"),
						PublicRef.IsEmpty() ? TEXT("The request has no primary target reference.") : TEXT("The readback target differs from the request's primary target."),
						TEXT("/primary_target_ref"), MakeShared<FJsonValueString>(PublicRef.IsEmpty() ? PublicRef : Target->GetPublicRef()),
						MakeShared<FJsonValueString>(PublicRef.IsEmpty() ? TEXT("a nonempty logical reference") : PublicRef), PublicRef);
				FDiagnostic Difference;
				const FString State = ReadState(Contract, *Typed, *Environment,
					Context.GetResourceTracker(), Difference, OutError);
				FUeAgentCanonicalValuePtr GeneratedAssetRefs;
				if (!State.IsEmpty()
					&& Phase == EUeAgentRigSimulationSpatialReadPhase::Verification
					&& Contract.GetCapabilityId()
						== TEXT("rig.retarget.assets.generate"))
				{
					const auto& Input = Typed->GetPayload()
						.Get<FRigRetargetAssetsGenerateInput>();
					TArray<FUeAgentCanonicalValueRef> ObservedRefs;
					FUeAgentStateSemanticError StateError;
					for (int32 AssetIndex = 0; AssetIndex < Input.AssetRequests.Num(); ++AssetIndex)
					{
						const auto& Item = Input.AssetRequests[AssetIndex];
						const FString ObjectPath =
							AssetObjectPath(Input.DestinationRoot, Item.Name);
						const UAnimSequence* Animation =
							FindObject<UAnimSequence>(nullptr, *ObjectPath);
						FUeAgentCanonicalValuePtr ObservedRef;
						if (!Animation) return MissingNative(OutError, TEXT("rig_simulation_spatial_generated_asset_unavailable"),
							TEXT("The generated animation is unavailable when reading its result reference."),
							TEXT("/asset_requests/") + FString::FromInt(AssetIndex) + TEXT("/generated_asset"), ObjectPath);
						if (!TryStringValue(Animation->GetPathName(), Context, ObservedRef, OutError,
							TEXT("/generated_asset_refs/") + FString::FromInt(AssetIndex))) return false;
						ObservedRefs.Add(ObservedRef.ToSharedRef());
					}
					if (!FUeAgentCanonicalValue::TryArray(
							ObservedRefs,
							Context.GetResourceTracker(),
							GeneratedAssetRefs,
							StateError))
					{
						NativeContext(StateError, TEXT("/generated_asset_refs"), true);
						if (StateError.TargetPath.IsEmpty()) StateError.TargetPath = Input.DestinationRoot;
						return ForwardFailure(OutError, MoveTemp(StateError));
					}
				}
				if (State.IsEmpty() || !TrySnapshot(Contract, Phase, PublicRef,
					State, Context, OutSnapshot, OutError, GeneratedAssetRefs)) return false;
				OutReadback.Emplace(OutSnapshot.ToSharedRef(), MoveTemp(Difference));
				return true;
			}

		private:
			TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment;
		};

		class FMutation final : public IUeAgentRigSimulationSpatialMutationPort
		{
		public:
			FMutation(
				TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> InEnvironment,
				TSharedPtr<IUeAgentRigSimulationSpatialAssetEffectPort> InAssetEffectPort)
				: Environment(MoveTemp(InEnvironment))
				, AssetEffectPort(MoveTemp(InAssetEffectPort)) {}

			bool TryMutate(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FUeAgentRigSimulationSpatialMutationResult& OutResult,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutResult = {};
				OutError = {};
				Environment->CountFamilyDispatch();
				if (!IsCapability(Contract.GetCapabilityId())) return UnsupportedCapability(OutError, Contract.GetCapabilityId());
				const FUeAgentRigSimulationSpatialRequest* Typed =
					ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const TOptional<FUeAgentCandidateBinding> Target = FirstTarget(Targets);
				if (!Target.IsSet()
					|| Target->GetPublicRef() != Typed->GetPrimaryTargetRef())
					return NativeFailure(OutError, TEXT("rig_simulation_spatial_explicit_target_mismatch"),
						!Target.IsSet() ? TEXT("The mutation has no resolved target binding.") : TEXT("The mutation target differs from the request's primary target."),
						TEXT("/primary_target_ref"),
						Target.IsSet() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Target->GetPublicRef())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
						MakeShared<FJsonValueString>(Typed->GetPrimaryTargetRef()), Typed->GetPrimaryTargetRef());

				UObject* MutatedObject = nullptr;
				if (Contract.GetCapabilityId() == TEXT("rig.ik.definition.update"))
				{
					const auto& Input = Typed->GetPayload().Get<FRigIkDefinitionUpdateInput>();
					const auto* Binding = Environment->FindIkRigBinding(Input.IkRigRef);
					UIKRigDefinition* Rig = Binding ? Binding->Rig.Get() : nullptr;
					if (!Binding || !Rig) return MissingNative(OutError, TEXT("rig_simulation_spatial_ik_binding_unavailable"),
						!Binding ? TEXT("The IK Rig reference has no registered binding.") : TEXT("The IK Rig binding has no available native rig."),
						!Binding ? TEXT("/ik_rig_binding") : TEXT("/ik_rig_binding/rig"), Input.IkRigRef);
					UObject* Resolved = Environment->ResolveExplicitObject(Input.IkRigRef);
					if (Resolved != Rig)
					{
						NativeFailure(OutError, TEXT("rig_simulation_spatial_ik_binding_unavailable"),
							TEXT("The IK Rig reference resolves to a different object than its binding."), TEXT("/ik_rig_ref"),
							MakeShared<FJsonValueBoolean>(Resolved != nullptr), MakeShared<FJsonValueString>(TEXT("the registered IK Rig")), Input.IkRigRef);
						if (Resolved) OutError.ActualEvidence = MakeShared<FJsonValueString>(Resolved->GetPathName());
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Rig->GetPathName());
						return false;
					}
					if (!CheckRevision(*Environment, Input.IkRigRef,
						Input.ExpectedRevision, OutError)) return false;
					UIKRigDefinition* Probe = DuplicateObject<UIKRigDefinition>(Rig,
						GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(),
							UIKRigDefinition::StaticClass(), TEXT("UAI_IK_Probe")));
					if (!Probe) return MissingNative(OutError, TEXT("rig_simulation_spatial_ik_probe_create_failed"),
						TEXT("Creating the IK definition validation clone returned no object."), TEXT("/validation_clone"), Input.IkRigRef);
					if (!ApplyIkDefinition(*Probe, *Binding, Input, *Environment, OutError)) return false;
					Rig->Modify();
					if (!ApplyIkDefinition(*Rig, *Binding, Input, *Environment, OutError))
						return false;
					Rig->MarkPackageDirty();
					MutatedObject = Rig;
				}
				else if (Contract.GetCapabilityId()
					== TEXT("rig.retarget.definition.update"))
				{
					const auto& Input = Typed->GetPayload()
						.Get<FRigRetargetDefinitionUpdateInput>();
					const auto* Binding = Environment->FindRetargeterBinding(Input.RetargeterRef);
					UIKRetargeter* Retargeter = Binding ? Binding->Retargeter.Get() : nullptr;
					if (!Binding || !Retargeter) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_binding_unavailable"),
						!Binding ? TEXT("The retargeter reference has no registered binding.") : TEXT("The retargeter binding has no available native object."),
						!Binding ? TEXT("/retargeter_binding") : TEXT("/retargeter_binding/retargeter"), Input.RetargeterRef);
					UObject* Resolved = Environment->ResolveExplicitObject(Input.RetargeterRef);
					if (Resolved != Retargeter)
					{
						NativeFailure(OutError, TEXT("rig_simulation_spatial_retarget_binding_unavailable"),
							TEXT("The retargeter reference resolves to a different object than its binding."), TEXT("/retargeter_ref"),
							MakeShared<FJsonValueBoolean>(Resolved != nullptr), MakeShared<FJsonValueString>(TEXT("the registered retargeter")), Input.RetargeterRef);
						if (Resolved) OutError.ActualEvidence = MakeShared<FJsonValueString>(Resolved->GetPathName());
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Retargeter->GetPathName());
						return false;
					}
					if (!CheckRevision(*Environment, Input.RetargeterRef,
						Input.ExpectedRevision, OutError)) return false;
					UIKRetargeter* Probe = DuplicateObject<UIKRetargeter>(Retargeter,
						GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(),
							UIKRetargeter::StaticClass(), TEXT("UAI_Retarget_Probe")));
					if (!Probe) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_probe_create_failed"),
						TEXT("Creating the retarget definition validation clone returned no object."), TEXT("/validation_clone"), Input.RetargeterRef);
					if (!ApplyRetargetDefinition(*Probe, *Binding, Input, *Environment, OutError)) return false;
					Retargeter->Modify();
					if (!ApplyRetargetDefinition(*Retargeter, *Binding, Input,
						*Environment, OutError)) return false;
					Retargeter->MarkPackageDirty();
					MutatedObject = Retargeter;
				}
				else if (Contract.GetCapabilityId() == TEXT("rig.retarget.pose.update"))
				{
					const auto& Input = Typed->GetPayload().Get<FRigRetargetPoseUpdateInput>();
					const auto* Binding = Environment->FindRetargeterBinding(Input.RetargeterRef);
					UIKRetargeter* Retargeter = Binding ? Binding->Retargeter.Get() : nullptr;
					if (!Binding || !Retargeter) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_binding_unavailable"),
						!Binding ? TEXT("The retargeter reference has no registered binding.") : TEXT("The retargeter binding has no available native object."),
						!Binding ? TEXT("/retargeter_binding") : TEXT("/retargeter_binding/retargeter"), Input.RetargeterRef);
					if (!CheckRevision(*Environment, Input.RetargeterRef,
						Input.ExpectedRevision, OutError)) return false;
					UIKRetargeter* Probe = DuplicateObject<UIKRetargeter>(Retargeter,
						GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(),
							UIKRetargeter::StaticClass(), TEXT("UAI_Pose_Probe")));
					if (!Probe) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_probe_create_failed"),
						TEXT("Creating the retarget pose validation clone returned no object."), TEXT("/validation_clone"), Input.RetargeterRef);
					if (!ApplyRetargetPose(*Probe, *Binding, Input, *Typed, *Environment, Context.GetResourceTracker(), OutError)) return false;
					Retargeter->Modify();
					if (!ApplyRetargetPose(*Retargeter, *Binding, Input, *Typed,
						*Environment, Context.GetResourceTracker(), OutError)) return false;
					Retargeter->MarkPackageDirty();
					MutatedObject = Retargeter;
				}
				else if (Contract.GetCapabilityId()
					== TEXT("rig.retarget.pose.auto_align"))
				{
					const auto& Input = Typed->GetPayload()
						.Get<FRigRetargetPoseAutoAlignInput>();
					const auto* Binding = Environment->FindRetargeterBinding(Input.RetargeterRef);
					UIKRetargeter* Retargeter = Binding ? Binding->Retargeter.Get() : nullptr;
					if (!Binding || !Retargeter) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_binding_unavailable"),
						!Binding ? TEXT("The retargeter reference has no registered binding.") : TEXT("The retargeter binding has no available native object."),
						!Binding ? TEXT("/retargeter_binding") : TEXT("/retargeter_binding/retargeter"), Input.RetargeterRef);
					if (!CheckRevision(*Environment, Input.RetargeterRef,
						Input.ExpectedRevision, OutError)) return false;
					UIKRetargeter* Probe = DuplicateObject<UIKRetargeter>(Retargeter,
						GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(),
							UIKRetargeter::StaticClass(), TEXT("UAI_Align_Probe")));
					if (!Probe) return MissingNative(OutError, TEXT("rig_simulation_spatial_retarget_probe_create_failed"),
						TEXT("Creating the pose alignment validation clone returned no object."), TEXT("/validation_clone"), Input.RetargeterRef);
					if (!ApplyAutoAlign(*Probe, *Binding, Input, *Environment, OutError)) return false;
					Retargeter->Modify();
					if (!ApplyAutoAlign(*Retargeter, *Binding, Input,
						*Environment, OutError)) return false;
					Retargeter->MarkPackageDirty();
					MutatedObject = Retargeter;
				}
				else
				{
					const auto& Input = Typed->GetPayload()
						.Get<FRigRetargetAssetsGenerateInput>();
					if (!AssetEffectPort.IsValid()) return MissingNative(OutError, TEXT("rig_simulation_spatial_asset_effect_port_unavailable"),
						TEXT("No asset effect port is configured for retarget asset generation."), TEXT("/asset_effect_port"), Input.DestinationRoot);
					UObject* Source = Environment->ResolveExplicitObject(Input.SourceSubjectRef);
					UObject* TargetSubject =
						Environment->ResolveExplicitObject(Input.TargetSubjectRef);
					UIKRetargeter* Retargeter = Cast<UIKRetargeter>(
						Environment->ResolveExplicitObject(Input.RetargeterRef));
					FUeAgentRigSimulationSpatialRetargetAssetEffectRequest EffectRequest;
					EffectRequest.SourceSubject = Source;
					EffectRequest.TargetSubject = TargetSubject;
					EffectRequest.Retargeter = Retargeter;
					EffectRequest.DestinationRoot = Input.DestinationRoot;
					EffectRequest.AssetRequests = Input.AssetRequests;
					if (!EffectRequest.TryValidate(OutError)) return false;
					FUeAgentRigSimulationSpatialRetargetAssetEffectResult EffectResult;
					if (!AssetEffectPort->TryApplyRetargetAssets(
						EffectRequest, Context, EffectResult, OutError)
						|| !EffectResult.TryValidate(OutError)) return false;
					MutatedObject = EffectResult.GeneratedAssets[0].Get();
				}

				if (!IsValid(MutatedObject)) return MissingNative(OutError, TEXT("rig_simulation_spatial_ik_retarget_mutation_target_unavailable"),
					TEXT("The native mutation produced no available target object."), TEXT("/mutation/target"), Typed->GetPrimaryTargetRef());
				const FString OperationId = Context.GetRequestId() + TEXT(":")
					+ Contract.GetCapabilityId();
				Environment->RecordMutation(OperationId, MutatedObject);
				OutResult = FUeAgentRigSimulationSpatialMutationResult(OperationId,
					TEXT("native-mutation:") + MutatedObject->GetPathName());
				// Both constructor inputs contain nonempty fixed prefixes.
				return true;
			}

		private:
			TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment;
			TSharedPtr<IUeAgentRigSimulationSpatialAssetEffectPort> AssetEffectPort;
		};

		class FSettle final : public IUeAgentRigSimulationSpatialSettlePort
		{
		public:
			explicit FSettle(
				TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}

			bool TrySettle(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutError = {};
				if (!IsCapability(Contract.GetCapabilityId())) return UnsupportedCapability(OutError, Contract.GetCapabilityId());
				UObject* Object = Environment->FindMutation(Mutation.GetOperationId());
				if (!IsValid(Object))
				{
					MissingNative(OutError, TEXT("rig_simulation_spatial_settle_target_unavailable"),
						TEXT("The recorded mutation has no available native object to settle."), TEXT("/mutation/target"), FString());
					OutError.SubjectId = Mutation.GetOperationId();
					return false;
				}
				if (UAnimSequence* Animation = Cast<UAnimSequence>(Object))
					Animation->RefreshCacheData();
				Object->PostEditChange();
				return TrySaveProjectAsset(*Object, OutError);
			}

		private:
			TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment;
		};

		class FComparator final : public IUeAgentRigSimulationSpatialResultComparator
		{
		public:
			bool TryCompare(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentRigSimulationSpatialReadbackResult& AfterReadback,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				const auto& After = AfterReadback.Snapshot.Get();
				OutError = {};
				if (!IsCapability(Contract.GetCapabilityId())) return UnsupportedCapability(OutError, Contract.GetCapabilityId());
				const auto* Typed = ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const FString BeforeState = SnapshotState(Before);
				const FString AfterState = SnapshotState(After);
				FDiagnostic Difference;
				if (MatchFact(!BeforeState.IsEmpty(), Difference, TEXT("The before snapshot has no observed state."),
					TEXT("/before/observed_state"), MakeShared<FJsonValueBoolean>(!BeforeState.IsEmpty()),
					MakeShared<FJsonValueBoolean>(true), Typed->GetPrimaryTargetRef()))
					MatchFact(!AfterState.IsEmpty(), Difference, TEXT("The after snapshot has no observed state."),
						TEXT("/after/observed_state"), MakeShared<FJsonValueBoolean>(!AfterState.IsEmpty()),
						MakeShared<FJsonValueBoolean>(true), Typed->GetPrimaryTargetRef());
				if (!AfterReadback.Difference.IsEmpty()) AppendDifference(Difference, AfterReadback.Difference);
				if (Contract.GetCapabilityId() == TEXT("rig.retarget.pose.auto_align") && BeforeState == AfterState)
				{
					FDiagnostic Unchanged;
					NativeFailure(Unchanged, TEXT("rig_simulation_spatial_ik_retarget_state_mismatch"),
						TEXT("Pose alignment did not change the observed pose state."), TEXT("/observed_state/changed"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Typed->GetPrimaryTargetRef());
					Unchanged.ActualEvidence = MakeShared<FJsonValueString>(AfterState);
					Unchanged.ExpectedEvidence = MakeShared<FJsonValueString>(BeforeState);
					AppendDifference(Difference, MoveTemp(Unchanged));
				}
				const bool bMatches = Difference.IsEmpty();
				FUeAgentCanonicalValuePtr PublicResult;
				if (bMatches && Contract.GetCapabilityId()
					== TEXT("rig.retarget.assets.generate"))
				{
					const FUeAgentCanonicalValuePtr GeneratedRefs =
						SnapshotResultField(After, TEXT("generated_asset_refs"));
					if (!GeneratedRefs.IsValid()) return MissingNative(OutError, TEXT("rig_simulation_spatial_generated_asset_refs_invalid"),
						TEXT("The verified snapshot has no generated asset references."), TEXT("/generated_asset_refs"), Typed->GetPrimaryTargetRef());
					if (GeneratedRefs->GetKind() != EUeAgentValueKind::Array)
						return NativeFailure(OutError, TEXT("rig_simulation_spatial_generated_asset_refs_invalid"),
							TEXT("Generated asset references must be an array."), TEXT("/generated_asset_refs"),
							MakeShared<FJsonValueString>(ValueKindName(GeneratedRefs->GetKind())), MakeShared<FJsonValueString>(TEXT("array")), Typed->GetPrimaryTargetRef());
					if (GeneratedRefs->GetArrayItems().IsEmpty())
						return NativeFailure(OutError, TEXT("rig_simulation_spatial_generated_asset_refs_invalid"),
							TEXT("The verified snapshot contains an empty generated asset reference array."), TEXT("/generated_asset_refs"),
							MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT(">= 1")), Typed->GetPrimaryTargetRef());
					FUeAgentStateSemanticError StateError;
					if (!FUeAgentCanonicalValue::TryObject({{TEXT("generated_asset_refs"), GeneratedRefs.ToSharedRef()}},
						Context.GetResourceTracker(), PublicResult, StateError))
					{
						NativeContext(StateError, FString(), true);
						return ForwardFailure(OutError, MoveTemp(StateError));
					}
				}
				OutOutcome = FVerificationOutcome(TEXT("rig-simulation-spatial-verify:") + Contract.GetCapabilityId(),
				bMatches ? EVerificationStatus::Verified
						: EVerificationStatus::Mismatch,
				bMatches ? EEffectOutcome::Complete : EEffectOutcome::Partial,
				MoveTemp(Difference),
				false,
				{},
				{TEXT("independent-state:") + After.GetSnapshotId()},
				{},
				MoveTemp(PublicResult));
				return true;
			}
		};
	}

	namespace
	{
		bool IkBindingFailure(Contract::FDiagnostic& OutError, const TCHAR* Code, const TCHAR* Reason,
			const FString& Path, const FString& Identity, TSharedPtr<FJsonValue> Actual, const TCHAR* Expected)
		{
			OutError = Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_binding"));
			OutError.PropertyPath = Path;
			OutError.TargetPath = Identity;
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return false;
		}
	}

	bool FUeAgentRigSimulationSpatialIkRigBinding::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		const auto Reject = [&](const TCHAR* Reason, const FString& Path, TSharedPtr<FJsonValue> Actual, const TCHAR* Expected)
		{
			return IkBindingFailure(OutError, TEXT("ik_rig_binding_invalid"), Reason, Path, Identity, MoveTemp(Actual), Expected);
		};
		if (Identity.IsEmpty()) return Reject(TEXT("The IK rig binding requires an object reference."), TEXT("identity"),
			MakeShared<FJsonValueString>(Identity), TEXT("non-empty IK rig reference"));
		if (!Rig.IsValid()) return Reject(TEXT("The bound IK rig object is unavailable."), TEXT("rig"), nullptr, TEXT("valid IK rig object"));
		if (PreviewMeshIdentity.IsEmpty()) return Reject(TEXT("The IK rig binding requires a preview mesh reference."), TEXT("preview_mesh_identity"),
			MakeShared<FJsonValueString>(PreviewMeshIdentity), TEXT("non-empty preview mesh reference"));
		if (BoneBindings.IsEmpty()) return Reject(TEXT("The IK rig binding contains no bones."), TEXT("bone_bindings"),
			MakeShared<FJsonValueNumber>(0), TEXT("at least one bone binding"));
		const auto ValidateNames = [&](const TMap<FString, FName>& Values, const TCHAR* Path)
		{
			for (const auto& Pair : Values)
			{
				if (Pair.Key.IsEmpty() || Pair.Value.IsNone())
				{
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetStringField(TEXT("identity"), Pair.Key);
					Actual->SetStringField(TEXT("native_name"), Pair.Value.ToString());
					Reject(Pair.Key.IsEmpty() ? TEXT("The IK binding entry requires a non-empty identity.")
						: TEXT("The IK binding entry must have a native name other than None."),
						FString(Path) + (Pair.Key.IsEmpty() ? TEXT(".identity") : TEXT(".native_name")),
						MakeShared<FJsonValueObject>(Actual), TEXT("non-empty identity and native name other than None"));
					OutError.CandidateName = Pair.Key;
					return false;
				}
			}
			return true;
		};
		return ValidateNames(BoneBindings, TEXT("bone_bindings")) && ValidateNames(GoalBindings, TEXT("goal_bindings"))
			&& ValidateNames(ChainBindings, TEXT("chain_bindings"));
	}

	bool FUeAgentRigSimulationSpatialIkRigBinding::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentRigSimulationSpatialRetargeterBinding::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		const auto Reject = [&](const TCHAR* Reason, const TCHAR* Path, TSharedPtr<FJsonValue> Actual, const TCHAR* Expected)
		{
			return IkBindingFailure(OutError, TEXT("retargeter_binding_invalid"), Reason, Path, Identity, MoveTemp(Actual), Expected);
		};
		if (Identity.IsEmpty()) return Reject(TEXT("The retargeter binding requires an object reference."), TEXT("identity"),
			MakeShared<FJsonValueString>(Identity), TEXT("non-empty retargeter reference"));
		if (!Retargeter.IsValid()) return Reject(TEXT("The bound retargeter object is unavailable."), TEXT("retargeter"), nullptr, TEXT("valid retargeter object"));
		if (SourceRigIdentity.IsEmpty()) return Reject(TEXT("The retargeter binding requires a source IK rig reference."), TEXT("source_rig_identity"),
			MakeShared<FJsonValueString>(SourceRigIdentity), TEXT("non-empty source rig reference"));
		if (TargetRigIdentity.IsEmpty()) return Reject(TEXT("The retargeter binding requires a target IK rig reference."), TEXT("target_rig_identity"),
			MakeShared<FJsonValueString>(TargetRigIdentity), TEXT("non-empty target rig reference"));
		if (SourceRigIdentity == TargetRigIdentity) return Reject(TEXT("The retargeter source and target IK rigs must be different."), TEXT("target_rig_identity"),
			MakeShared<FJsonValueString>(TargetRigIdentity), TEXT("a rig reference different from the source"));
		if (SourceNativePoseFrameId.IsEmpty()) return Reject(TEXT("The retargeter binding requires a source native pose frame."), TEXT("source_native_pose_frame_id"),
			MakeShared<FJsonValueString>(SourceNativePoseFrameId), TEXT("non-empty source frame identifier"));
		if (TargetNativePoseFrameId.IsEmpty()) return Reject(TEXT("The retargeter binding requires a target native pose frame."), TEXT("target_native_pose_frame_id"),
			MakeShared<FJsonValueString>(TargetNativePoseFrameId), TEXT("non-empty target frame identifier"));
		if (SourceNativePoseFrameId == TargetNativePoseFrameId) return Reject(TEXT("The retargeter source and target pose frames must be different."), TEXT("target_native_pose_frame_id"),
			MakeShared<FJsonValueString>(TargetNativePoseFrameId), TEXT("a frame identifier different from the source"));
		return true;
	}

	bool FUeAgentRigSimulationSpatialRetargeterBinding::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentRigSimulationSpatialRetargetAssetEffectRequest::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError = {};
		const FString Code = TEXT("rig_simulation_spatial_retarget_asset_effect_request_invalid");
		if (!SourceSubject.IsValid()) return MissingNative(OutError, Code,
			TEXT("The asset effect source subject object is unavailable."), TEXT("/source_subject"), DestinationRoot);
		if (!TargetSubject.IsValid()) return MissingNative(OutError, Code,
			TEXT("The asset effect target subject object is unavailable."), TEXT("/target_subject"), DestinationRoot);
		if (SourceSubject == TargetSubject)
			return NativeFailure(OutError, Code, TEXT("The asset effect source and target refer to the same object."),
				TEXT("/subjects/distinct"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), DestinationRoot);
		if (!Retargeter.IsValid()) return MissingNative(OutError, Code,
			TEXT("The asset effect retargeter is unavailable."), TEXT("/retargeter"), DestinationRoot);
		if (AssetRequests.IsEmpty())
			return InputFailure(OutError, Code, TEXT("The asset effect requires at least one asset request."), TEXT("/asset_requests"),
				MakeShared<FJsonValueNumber>(AssetRequests.Num()), MakeShared<FJsonValueString>(TEXT(">= 1")), DestinationRoot);
		FText PathReason;
		if (!FPackageName::IsValidLongPackageName(DestinationRoot, false, &PathReason))
			return InputFailure(OutError, Code, PathReason.ToString(), TEXT("/destination_root"),
				MakeShared<FJsonValueString>(DestinationRoot), MakeShared<FJsonValueString>(TEXT("a valid long package name")), DestinationRoot);
		TSet<FString> Names;
		for (int32 Index = 0; Index < AssetRequests.Num(); ++Index)
		{
			const auto& Request = AssetRequests[Index];
			const FString Path = TEXT("/asset_requests/") + FString::FromInt(Index);
			if (Request.AssetKind.IsEmpty())
				return InputFailure(OutError, Code, TEXT("The requested asset kind is empty."), Path + TEXT("/asset_kind"),
					MakeShared<FJsonValueString>(Request.AssetKind), MakeShared<FJsonValueString>(TEXT("a nonempty asset kind")), DestinationRoot);
			if (Request.Name.IsEmpty())
				return InputFailure(OutError, Code, TEXT("The requested asset name is empty."), Path + TEXT("/name"),
					MakeShared<FJsonValueString>(Request.Name), MakeShared<FJsonValueString>(TEXT("a nonempty asset name")), DestinationRoot);
			if (Request.OverwritePolicy != TEXT("fail") && Request.OverwritePolicy != TEXT("replace"))
				return InputFailure(OutError, Code, TEXT("The asset overwrite policy is unsupported."), Path + TEXT("/overwrite_policy"),
					MakeShared<FJsonValueString>(Request.OverwritePolicy), MakeShared<FJsonValueString>(TEXT("fail or replace")), DestinationRoot);
			if (Names.Contains(Request.Name))
				return InputFailure(OutError, Code, TEXT("The requested asset name occurs more than once."), Path + TEXT("/name"),
					MakeShared<FJsonValueString>(Request.Name), MakeShared<FJsonValueString>(TEXT("a unique asset name")), DestinationRoot);
			const FString ObjectPath = AssetObjectPath(DestinationRoot, Request.Name);
			if (!FPackageName::IsValidObjectPath(ObjectPath, &PathReason))
				return InputFailure(OutError, Code, PathReason.ToString(), Path + TEXT("/name"),
					MakeShared<FJsonValueString>(ObjectPath), MakeShared<FJsonValueString>(TEXT("a valid destination object path")), DestinationRoot);
			Names.Add(Request.Name);
		}
		return true;
	}

	bool FUeAgentRigSimulationSpatialRetargetAssetEffectRequest::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentRigSimulationSpatialRetargetAssetEffectResult::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError = {};
		const FString Code = TEXT("rig_simulation_spatial_retarget_asset_effect_result_invalid");
		if (GeneratedAssets.IsEmpty())
			return NativeFailure(OutError, Code, TEXT("The asset effect returned no generated assets."), TEXT("/generated_assets"),
				MakeShared<FJsonValueNumber>(GeneratedAssets.Num()), MakeShared<FJsonValueString>(TEXT(">= 1")));
		if (GeneratedAssets.Num() != GeneratedAssetRefs.Num())
			return NativeFailure(OutError, Code, TEXT("Generated assets and references have different counts."), TEXT("/generated_asset_refs"),
				MakeShared<FJsonValueNumber>(GeneratedAssetRefs.Num()), MakeShared<FJsonValueNumber>(GeneratedAssets.Num()));
		TSet<FString> Refs;
		for (int32 Index = 0; Index < GeneratedAssets.Num(); ++Index)
		{
			const FString Path = TEXT("/generated_asset_refs/") + FString::FromInt(Index);
			if (!GeneratedAssets[Index].IsValid())
				return MissingNative(OutError, Code, TEXT("The generated asset object is unavailable."),
					TEXT("/generated_assets/") + FString::FromInt(Index), GeneratedAssetRefs[Index]);
			if (GeneratedAssetRefs[Index].IsEmpty())
				return NativeFailure(OutError, Code, TEXT("The generated asset reference is empty."), Path,
					MakeShared<FJsonValueString>(GeneratedAssetRefs[Index]), MakeShared<FJsonValueString>(TEXT("a nonempty object reference")));
			const FString ActualPath = GeneratedAssets[Index]->GetPathName();
			if (ActualPath != GeneratedAssetRefs[Index])
			{
				NativeFailure(OutError, Code, TEXT("The generated asset object does not match its returned reference."), Path,
					nullptr, MakeShared<FJsonValueString>(GeneratedAssetRefs[Index]));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(ActualPath);
				return false;
			}
			if (Refs.Contains(GeneratedAssetRefs[Index]))
				return NativeFailure(OutError, Code, TEXT("The generated asset reference occurs more than once."), Path,
					MakeShared<FJsonValueString>(GeneratedAssetRefs[Index]), MakeShared<FJsonValueString>(TEXT("a unique generated asset reference")));
			Refs.Add(GeneratedAssetRefs[Index]);
		}
		return true;
	}

	bool FUeAgentRigSimulationSpatialRetargetAssetEffectResult::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool TryCreateRigSimulationSpatialIkRetargetNativePorts(
		TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment,
		TSharedPtr<IUeAgentRigSimulationSpatialAssetEffectPort> AssetEffectPort,
		FUeAgentRigSimulationSpatialNativePorts& OutPorts,
		FUeAgentRigSimulationSpatialExecutionError& OutError)
	{
		OutPorts = {};
		OutError = {};
		OutPorts.SupportedCapabilities = {
			TEXT("rig.ik.definition.update"),
			TEXT("rig.retarget.definition.update"),
			TEXT("rig.retarget.pose.update"),
			TEXT("rig.retarget.pose.auto_align"),
			TEXT("rig.retarget.assets.generate"),
		};
		OutPorts.Readback = MakeShared<FReadback>(Environment);
		OutPorts.Mutation = MakeShared<FMutation>(Environment, MoveTemp(AssetEffectPort));
		OutPorts.Settle = MakeShared<FSettle>(Environment);
		OutPorts.Comparator = MakeShared<FComparator>();
		return OutPorts.TryValidate(OutError);
	}
}
