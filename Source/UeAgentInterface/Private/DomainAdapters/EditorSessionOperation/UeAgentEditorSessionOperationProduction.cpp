#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationProduction.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/Contract/UeAgentExecutionContextPolicy.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::EditorSessionOperation
{
	namespace
	{
		using namespace ContextResource;
		using namespace Contract;
		using namespace ExecutionMutation;
		using namespace StateSemantic;

		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
            return false;
        }


		bool IsNormalizedNonEmpty(const FString& Value)
		{
			FString Normalized = Value;
			Normalized.TrimStartAndEndInline();
			return !Normalized.IsEmpty() && Normalized == Value;
		}

		FDiagnostic WithSessionContext(FDiagnostic Error, const FUeAgentEditorSessionOperationTypedRequest& Request)
		{
			if (Error.TargetPath.IsEmpty() && Request.GetFamily() != EUeAgentEditorSessionOperationFamily::HostLifecycle)
				Error.TargetPath = Request.GetSubjectRef();
			if (Error.SubjectId.IsEmpty()) Error.SubjectId = Request.GetSubjectRef();
			return Error;
		}

FString PolicyPrefix(EUeAgentEditorSessionOperationFamily Family)
		{
			switch (Family)
			{
			case EUeAgentEditorSessionOperationFamily::AssetEditor: return TEXT("policy:asset-editor:");
			case EUeAgentEditorSessionOperationFamily::RuntimePreview: return TEXT("policy:runtime-session:");
			case EUeAgentEditorSessionOperationFamily::WorldContext: return TEXT("policy:world-context:");
			case EUeAgentEditorSessionOperationFamily::ModelingTool: return TEXT("policy:modeling-tool:");
			case EUeAgentEditorSessionOperationFamily::HostLifecycle: return TEXT("policy:host-lifecycle:");
			case EUeAgentEditorSessionOperationFamily::PcgGeneration: return TEXT("policy:pcg-generation:");
			default: return FString();
			}
		}


		bool MatchesPolicyKind(EUeAgentEditorSessionOperationFamily Family, const FString& PolicyId)
		{
			const FString Prefix = PolicyPrefix(Family);
			return !Prefix.IsEmpty() && PolicyId.StartsWith(Prefix);
		}

		bool MechanismMatchesFamily(
			const EUeAgentEditorSessionOperationFamily Family,
			const FString& MechanismId)
		{
			return Family == EUeAgentEditorSessionOperationFamily::HostLifecycle
				? MechanismId == TEXT("host_exit_schedule")
				: MechanismId == TEXT("native_session_resource");
		}


		FDiagnostic SessionFact(const TCHAR* Code, const TCHAR* Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("execution"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValuePresent = Error.ExpectedValue.IsValid();
			return Error;
		}

		FDiagnostic SessionText(const TCHAR* Code, const TCHAR* Reason, const FString& Path,
			const FString& Actual, const FString& Expected)
		{
			return SessionFact(Code, Reason, Path, MakeShared<FJsonValueString>(Actual),
				MakeShared<FJsonValueString>(Expected));
		}

		FDiagnostic SessionCondition(const TCHAR* Code, const TCHAR* Reason, const FString& Path,
			bool Actual, bool Expected)
		{
			return SessionFact(Code, Reason, Path, MakeShared<FJsonValueBoolean>(Actual),
				MakeShared<FJsonValueBoolean>(Expected));
		}

		FString WorldTypeName(EUeAgentExecutionWorldType Type)
		{
			switch (Type)
			{
			case EUeAgentExecutionWorldType::Invalid: return TEXT("invalid");
			case EUeAgentExecutionWorldType::Editor: return TEXT("editor");
			case EUeAgentExecutionWorldType::Pie: return TEXT("pie");
			case EUeAgentExecutionWorldType::SimulateInEditor: return TEXT("simulate_in_editor");
			case EUeAgentExecutionWorldType::Game: return TEXT("game");
			case EUeAgentExecutionWorldType::Preview: return TEXT("preview");
			default: return FString::Printf(TEXT("unknown(%d)"), static_cast<int32>(Type));
			}
		}


		FDiagnostic PolicyValidation(const FUeAgentEditorSessionOperationPolicyDescriptor& Policy)
		{
			const TCHAR* Code = TEXT("editor_session_operation_policy_invalid");
			if (FindEditorSessionOperationFamilyContract(Policy.GetFamily()) == nullptr)
				return SessionFact(Code, TEXT("The registered policy has an unsupported operation family."),
					TEXT("/family"), MakeShared<FJsonValueNumber>(static_cast<int32>(Policy.GetFamily())),
					MakeShared<FJsonValueString>(TEXT("asset editor, runtime preview, world context, modeling tool, or host lifecycle")));
			if (!IsNormalizedNonEmpty(Policy.GetPolicyId()))
				return SessionText(Code, TEXT("The registered policy identifier must be non-empty and have no surrounding whitespace."),
					TEXT("/policy_id"), Policy.GetPolicyId(), TEXT("non-empty normalized policy identifier"));
			if (!MatchesPolicyKind(Policy.GetFamily(), Policy.GetPolicyId()))
				return SessionText(Code, TEXT("The registered policy identifier has the wrong prefix for its operation family."),
					TEXT("/policy_id"), Policy.GetPolicyId(), PolicyPrefix(Policy.GetFamily()));
			if (!MechanismMatchesFamily(Policy.GetFamily(), Policy.GetNativeMechanismId()))
				return SessionText(Code, TEXT("The native mechanism is incompatible with the policy operation family."),
					TEXT("/native_mechanism_id"), Policy.GetNativeMechanismId(),
					Policy.GetFamily() == EUeAgentEditorSessionOperationFamily::HostLifecycle
						? TEXT("host_exit_schedule") : TEXT("native_session_resource"));
			if (!IsNormalizedNonEmpty(Policy.GetReadbackOracleId())
				|| !Policy.GetReadbackOracleId().StartsWith(TEXT("uai.readback.")))
				return SessionText(Code, TEXT("The readback oracle identifier must be normalized and start with uai.readback."),
					TEXT("/readback_oracle_id"), Policy.GetReadbackOracleId(), TEXT("non-empty normalized uai.readback.* identifier"));
			if (Policy.GetRequiredStatePlane() == EUeAgentStatePlane::Invalid)
				return SessionText(Code, TEXT("The registered policy requires an invalid state plane."),
					TEXT("/required_state_plane"), LexToString(Policy.GetRequiredStatePlane()), TEXT("a valid state plane"));
			const auto& WorldTypes = Policy.GetAllowedWorldTypes();
			for (int32 Index = 0; Index < WorldTypes.Num(); ++Index)
			{
				if (WorldTypes[Index] == EUeAgentExecutionWorldType::Invalid)
					return SessionText(Code, TEXT("The allowed world types contain an invalid world type."),
						FString::Printf(TEXT("/allowed_world_types/%d"), Index), WorldTypeName(WorldTypes[Index]), TEXT("a non-invalid world type"));
				for (int32 Other = 0; Other < Index; ++Other)
					if (WorldTypes[Other] == WorldTypes[Index])
						return SessionText(Code, TEXT("The allowed world types contain a duplicate world type."),
							FString::Printf(TEXT("/allowed_world_types/%d"), Index), WorldTypeName(WorldTypes[Index]), TEXT("a unique world type"));
			}
			const TPair<const TCHAR*, const FString*> Discovery[] = {
				{TEXT("subject_schema"), &Policy.GetSubjectSchemaJson()},
				{TEXT("parameter_schema"), &Policy.GetParameterSchemaJson()},
				{TEXT("parameter_guidance"), &Policy.GetParameterGuidance()},
				{TEXT("result_guidance"), &Policy.GetResultGuidance()},
				{TEXT("minimum_example"), &Policy.GetMinimumExample()}};
			bool bHasAny = false;
			for (const auto& Field : Discovery) bHasAny |= !Field.Value->IsEmpty();
			if (bHasAny)
				for (const auto& Field : Discovery)
					if (Field.Value->IsEmpty())
						return SessionCondition(Code, TEXT("The policy discovery contract is partial; this required discovery field is empty."),
							FString(TEXT("/")) + Field.Key, false, true);
			return {};
		}

		FDiagnostic RequestPolicyFailure(const TCHAR* Code,
			const FUeAgentEditorSessionOperationTypedRequest& Request,
			const FUeAgentEditorSessionOperationPolicyDescriptor& Policy)
		{
			if (!Request.IsValid()) return Request.GetValidationDiagnostic();
			if (!Policy.IsValid()) return Policy.GetValidationDiagnostic();
			if (Request.GetFamily() != Policy.GetFamily())
				return SessionFact(Code, TEXT("The request operation family differs from its selected policy family."),
					TEXT("/family"), MakeShared<FJsonValueNumber>(static_cast<int32>(Request.GetFamily())),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Policy.GetFamily())));
			return SessionText(Code, TEXT("The request policy identifier differs from the selected policy."),
				TEXT("/policy_id"), Request.GetPolicyId(), Policy.GetPolicyId());
		}

		FDiagnostic NativeObservationFailure(const FUeAgentEditorSessionNativeObservation& Observation)
		{
			const TCHAR* Code = TEXT("editor_session_native_observation_invalid");
			FDiagnostic Error;
			if (Observation.GetState() == ELongRunningWorkState::Invalid)
				Error = SessionText(Code, TEXT("The native session observation has no valid lifecycle state."),
					TEXT("/state"), TEXT("invalid"), TEXT("a non-invalid lifecycle state"));
			else if (!IsNormalizedNonEmpty(Observation.GetPhase()))
				Error = SessionText(Code, TEXT("The native session phase must be non-empty and have no surrounding whitespace."),
					TEXT("/phase"), Observation.GetPhase(), TEXT("non-empty normalized phase"));
			else if (!Observation.GetSessionRef().IsEmpty()
				&& !IsNormalizedNonEmpty(Observation.GetSessionRef()))
				Error = SessionText(Code, TEXT("The native session reference has surrounding whitespace."),
					TEXT("/session_ref"), Observation.GetSessionRef(), TEXT("empty or normalized session reference"));
			else if (Observation.GetState() == ELongRunningWorkState::ExitedFailure
				|| Observation.GetState() == ELongRunningWorkState::StateUnknown)
				Error = SessionText(Code, TEXT("A failed or unknown native session state requires a non-empty normalized diagnostic code."),
					TEXT("/diagnostic/code"), Observation.GetDiagnostic().Code, TEXT("non-empty normalized diagnostic code"));
			else
				Error = SessionCondition(Code, TEXT("A native session state without failure cannot carry a failure diagnostic."),
					TEXT("/diagnostic_present"), true, false);
			if (!Observation.GetDiagnostic().IsEmpty()) Error.Diagnostics.Add(Observation.GetDiagnostic());
			return Error;
		}

		class FProductionTargetResolver final
			: public IUeAgentEditorSessionOperationTargetResolver
		{
		public:
			bool TryResolveTarget(
				const FUeAgentEditorSessionOperationTypedRequest& Request,
				const FUeAgentEditorSessionOperationPolicyDescriptor& Policy,
				FExecutionMutationError& OutError) override
			{
				if (Request.GetFamily()
						!= EUeAgentEditorSessionOperationFamily::WorldContext
					|| Policy.GetPolicyId()
						!= TEXT("policy:world-context:set-current/v1"))
				{
					return true;
				}

				static const FString Prefix(TEXT("world:"));
				if (!Request.GetSubjectRef().StartsWith(Prefix))
				{
					auto Error = SessionText(TEXT("world_context_subject_invalid"),
						TEXT("The world context subject must start with world:."), TEXT("/subject_ref"),
						Request.GetSubjectRef(), TEXT("world:/path/to/map"));
					Error.SourceKind = TEXT("command_input");
					return Fail(OutError, MoveTemp(Error));
				}
				const FString WorldPackage =
					Request.GetSubjectRef().RightChop(Prefix.Len());
				if (!FPackageName::IsValidLongPackageName(
						WorldPackage, false))
				{
					auto Error = SessionText(TEXT("world_context_subject_invalid"),
						TEXT("The world context subject contains an invalid long package name."), TEXT("/subject_ref"),
						Request.GetSubjectRef(), TEXT("world: followed by a valid long package name"));
					Error.SourceKind = TEXT("command_input");
					return Fail(OutError, MoveTemp(Error));
				}

				if (const UPackage* LoadedPackage =
						FindPackage(nullptr, *WorldPackage))
				{
					if (LoadedPackage->ContainsMap())
					{
						return true;
					}
				}

				TArray<FAssetData> Assets;
				FAssetRegistryModule::GetRegistry().GetAssetsByPackageName(
					FName(*WorldPackage), Assets, false);
				if (Assets.IsEmpty())
				{
					auto Error = SessionFact(TEXT("world_context_map_not_found"),
						TEXT("No assets were found in the requested world package."), TEXT("/package_asset_count"),
						MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("a package containing a World asset")));
					Error.TargetPath = WorldPackage;
					return Fail(OutError, MoveTemp(Error));
				}
				for (const FAssetData& Asset : Assets)
				{
					if (Asset.AssetClassPath
						== UWorld::StaticClass()->GetClassPathName())
					{
						return true;
					}
				}
				TArray<TSharedPtr<FJsonValue>> Classes;
				for (const auto& Asset : Assets) Classes.Add(MakeShared<FJsonValueString>(Asset.AssetClassPath.ToString()));
				auto Error = SessionFact(TEXT("world_context_map_unresolvable"),
					TEXT("The requested world package contains assets, but none is a World asset."), TEXT("/asset_classes"),
					MakeShared<FJsonValueArray>(MoveTemp(Classes)), MakeShared<FJsonValueString>(UWorld::StaticClass()->GetClassPathName().ToString()));
				Error.TargetPath = WorldPackage;
				return Fail(OutError, MoveTemp(Error));
			}
		};

		bool MatchesPolicy(
			const FUeAgentEditorSessionOperationTypedRequest& Request,
			const FUeAgentEditorSessionOperationPolicyDescriptor& Policy)
		{
			return Request.GetFamily() == Policy.GetFamily()
				&& Request.GetPolicyId() == Policy.GetPolicyId();
		}

		int64 NextGeneration()
		{
			static FCriticalSection Mutex;
			static int64 Generation = 0;
			FScopeLock Lock(&Mutex);
			return ++Generation;
		}

		bool ValidateOperationContext(
			const FLongRunningOperationRequest& Request,
			const FUeAgentEditorSessionOperationPolicyDescriptor& Policy,
			FExecutionMutationError& OutError)
		{
			if (!Request.IsValid()) return Fail(OutError, Request.GetValidationDiagnostic());
			const auto& Context = Request.GetExecutionContext();
			if (Context.GetStatePlane() != Policy.GetRequiredStatePlane())
				return Fail(OutError, SessionText(TEXT("editor_session_operation_context_state_plane_mismatch"),
					TEXT("The operation execution state plane differs from the selected policy requirement."),
					TEXT("/execution_context/state_plane"), LexToString(Context.GetStatePlane()),
					LexToString(Policy.GetRequiredStatePlane())));
			const auto& Allowed = Policy.GetAllowedWorldTypes();
			if (!Allowed.IsEmpty() && !Allowed.Contains(Context.GetWorldType()))
			{
				TArray<TSharedPtr<FJsonValue>> Expected;
				for (const auto Type : Allowed) Expected.Add(MakeShared<FJsonValueString>(WorldTypeName(Type)));
				return Fail(OutError, SessionFact(TEXT("editor_session_operation_context_world_type_mismatch"),
					TEXT("The operation world type is not allowed by the selected policy."),
					TEXT("/execution_context/world_type"), MakeShared<FJsonValueString>(WorldTypeName(Context.GetWorldType())),
					MakeShared<FJsonValueArray>(MoveTemp(Expected))));
			}
			return true;
		}

		bool ValidateCallIdentity(
			const FString& ExpectedOperationId,
			const FLongRunningWorkIdentity& ExpectedIdentity,
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			FExecutionMutationError& OutError)
		{
			if (ExpectedOperationId != OperationId)
			{
				auto Error = SessionCondition(TEXT("editor_session_operation_id_mismatch"),
					TEXT("The lifecycle call belongs to a different operation from this session port."),
					TEXT("/operation_identity_matches"), false, true);
				Error.ActualEvidence = MakeShared<FJsonValueString>(OperationId);
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedOperationId);
				return Fail(OutError, MoveTemp(Error));
			}
			if (!(ExpectedIdentity == Identity))
			{
				auto Error = SessionCondition(TEXT("editor_session_work_identity_mismatch"),
					TEXT("The lifecycle call resource identity or generation differs from the session port identity."),
					TEXT("/work_identity_matches"), false, true);
				auto Evidence = [](const FLongRunningWorkIdentity& Value)
				{
					auto Object = MakeShared<FJsonObject>();
					Object->SetStringField(TEXT("resource_id"), Value.GetResourceId());
					Object->SetStringField(TEXT("generation"), FString::Printf(TEXT("%lld"), Value.GetGeneration()));
					return MakeShared<FJsonValueObject>(Object);
				};
				Error.ActualEvidence = Evidence(Identity);
				Error.ExpectedEvidence = Evidence(ExpectedIdentity);
				return Fail(OutError, MoveTemp(Error));
			}
			return true;
		}

		bool TryBuildSignal(
			const FUeAgentEditorSessionOperationTypedRequest& Request,
			const FUeAgentEditorSessionOperationPolicyDescriptor& Policy,
			const FString& Phase,
			const FString& SessionRef,
			const TSharedPtr<FJsonObject>& Details,
			const int64 Revision,
			FUeAgentResourceTracker& Resources,
			TOptional<FLongRunningOperationSignal>& OutSignal,
			FExecutionMutationError& OutError)
		{
			const FUeAgentEditorSessionOperationFamilyContract* Contract =
				FindEditorSessionOperationFamilyContract(Request.GetFamily());
			if (Contract == nullptr)
			{
				return Fail(OutError, SessionFact(TEXT("editor_session_operation_family_invalid"),
					TEXT("No signal contract is registered for this editor session operation family."),
					TEXT("/family"), MakeShared<FJsonValueNumber>(static_cast<int32>(Request.GetFamily())),
					MakeShared<FJsonValueString>(TEXT("a registered operation family"))));
			}

			FUeAgentStateSemanticError StateError;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
			auto AddString = [&](const FString& Key, const FString& Value)
			{
				FUeAgentCanonicalValuePtr Canonical;
				if (!FUeAgentCanonicalValue::TryString(
						Value, Resources, Canonical, StateError))
				{
					return false;
				}
				Items.Emplace(Key, Canonical.ToSharedRef());
				return true;
			};
			const FString PublicPhase =
				Phase == TEXT("active")
				&& Request.GetFamily()
					== EUeAgentEditorSessionOperationFamily::WorldContext
				&& Policy.GetPolicyId()
					== TEXT("policy:world-context:set-current/v1")
				? TEXT("ready")
				: Phase;
			if (!AddString(TEXT("phase"), PublicPhase)
				|| !AddString(TEXT("subject_ref"), Request.GetSubjectRef())
				|| !AddString(TEXT("policy_id"), Policy.GetPolicyId())
				|| !AddString(
					TEXT("readback_oracle_id"),
					Policy.GetReadbackOracleId())
				|| (!SessionRef.IsEmpty()
					&& !AddString(TEXT("session_ref"), SessionRef)))
			{
				return Fail(OutError, StateError);
			}
			if (Details.IsValid())
			{
				FUeAgentCanonicalValuePtr CanonicalDetails;
				UE::UAI::Contract::FDiagnostic CanonicalError;
				if (!CanonicalJson::TryFromJsonValue(
						MakeShared<FJsonValueObject>(Details),
						Resources,
						CanonicalDetails,
						CanonicalError)
					|| !CanonicalDetails.IsValid())
				{
					return Fail(OutError, MoveTemp(CanonicalError));
				}
				Items.Emplace(TEXT("details"), CanonicalDetails.ToSharedRef());
			}

			FUeAgentCanonicalValuePtr Payload;
			if (!FUeAgentCanonicalValue::TryObject(
					Items, Resources, Payload, StateError))
			{
				return Fail(OutError, StateError);
			}
			OutSignal.Emplace(
				Contract->GetSignalTypeId(),
				Revision,
				Payload.ToSharedRef());
			return true;
		}

		class FNativeSessionResourcePort final
			: public ILongRunningOperationPort
		{
		public:
			FNativeSessionResourcePort(
				FUeAgentEditorSessionOperationTypedRequest InRequest,
				FUeAgentEditorSessionOperationPolicyDescriptor InPolicy,
				TSharedRef<
					IUeAgentEditorSessionNativeSession,
					ESPMode::ThreadSafe> InNativeSession)
				: Request(MoveTemp(InRequest))
				, Policy(MoveTemp(InPolicy))
				, NativeSession(MoveTemp(InNativeSession))
			{
			}

			bool Start(
				const FLongRunningOperationRequest& OperationRequest,
				const FUeAgentCancellationToken& Cancellation,
				FLongRunningWorkIdentity& OutIdentity,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (bStarted)
				{
					return Reject(OutError, SessionCondition(TEXT("editor_session_operation_already_started"), TEXT("The native editor session operation has already been started."), TEXT("/started"), true, false));
				}
				if (!ValidateOperationContext(
						OperationRequest, Policy, OutError))
				{
					OutError.MutationStarted = false;
					OutError.MutationMayHaveStarted = false;
					return false;
				}
				FString NativeResourceId;
				bNativeStartAttempted = true;
				if (!NativeSession->Start(
						OperationRequest,
						Cancellation,
						NativeResourceId,
						OutError))
				{
					return false;
				}
				if (!IsNormalizedNonEmpty(NativeResourceId))
				{
					return Reject(OutError, [&]() { auto Error = SessionCondition(TEXT("editor_session_native_resource_id_invalid"), TEXT("The native session returned an empty resource identifier or one with surrounding whitespace."), TEXT("/native_resource_id_normalized"), false, true); Error.ActualEvidence = MakeShared<FJsonValueString>(NativeResourceId); return Error; }());
				}
				OperationId = OperationRequest.GetOperationId();
				Identity = FLongRunningWorkIdentity(
					TEXT("native:editor-session:") + OperationId
						+ TEXT(":") + NativeResourceId,
					NextGeneration());
				bStarted = true;
				OutIdentity = Identity;
				return true;
			}

			bool IsFailedStartCleanupComplete() const override
			{
				FScopeLock Lock(&Mutex);
				return !bNativeStartAttempted;
			}

			bool Observe(
				const FString& InOperationId,
				const FLongRunningWorkIdentity& InIdentity,
				FUeAgentResourceTracker& Resources,
				FLongRunningWorkObservation& OutObservation,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (!ValidateStartedIdentity(
						InOperationId, InIdentity, OutError))
				{
					return false;
				}
				FUeAgentEditorSessionNativeObservation NativeObservation;
				if (!NativeSession->Observe(NativeObservation, OutError))
				{
					return false;
				}
				if (!NativeObservation.IsValid())
				{
					return Reject(OutError, NativeObservationFailure(NativeObservation));
				}

				const FString SignalKey = NativeObservation.GetPhase()
					+ TEXT("\n") + NativeObservation.GetSessionRef();
				TOptional<FLongRunningOperationSignal> Signal;
				if (SignalKey != LastSignalKey)
				{
					const int64 NextRevision = SignalRevision + 1;
					if (!TryBuildSignal(
							Request,
							Policy,
							NativeObservation.GetPhase(),
							NativeObservation.GetSessionRef(),
							NativeObservation.GetDetails(),
							NextRevision,
							Resources,
							Signal,
							OutError))
					{
						if (!NativeObservation.GetDiagnostic().IsEmpty()) OutError.Append(NativeObservation.GetDiagnostic());
						return false;
					}
					LastSignalKey = SignalKey;
					SignalRevision = NextRevision;
				}
				LastObservedState = NativeObservation.GetState();
				OutObservation = FLongRunningWorkObservation(Identity.GetGeneration(),
			NativeObservation.GetState(),
			NativeObservation.GetDiagnostic(),
			MoveTemp(Signal));
				return true;
			}

			bool RequestStop(
				const FString& InOperationId,
				const FLongRunningWorkIdentity& InIdentity,
				const FString& Reason,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (!ValidateStartedIdentity(
						InOperationId, InIdentity, OutError))
				{
					return false;
				}
				if (!IsNormalizedNonEmpty(Reason))
				{
					return Reject(OutError, SessionText(TEXT("editor_session_stop_reason_invalid"), TEXT("The stop reason must be non-empty and have no surrounding whitespace."), TEXT("/stop_reason"), Reason, TEXT("non-empty normalized stop reason")));
				}
				if (!NativeSession->RequestStop(Reason, OutError))
				{
					return false;
				}
				return true;
			}

			bool Join(
				const FString& InOperationId,
				const FLongRunningWorkIdentity& InIdentity,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (!ValidateStartedIdentity(
						InOperationId, InIdentity, OutError))
				{
					return false;
				}
				if (LastObservedState != ELongRunningWorkState::ExitedSuccess
					&& LastObservedState
						!= ELongRunningWorkState::ExitedFailure)
				{
					return Reject(OutError, SessionText(TEXT("editor_session_join_before_native_exit"), TEXT("The native session cannot be joined before an exited state has been observed."), TEXT("/observed_state"), LexToString(LastObservedState), TEXT("exited_success or exited_failure")));
				}
				if (bJoined)
				{
					return true;
				}
				if (!NativeSession->Join(OutError))
				{
					return false;
				}
				bJoined = true;
				return true;
			}

			bool Release(
				const FString& InOperationId,
				const FLongRunningWorkIdentity& InIdentity,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (!ValidateStartedIdentity(
						InOperationId, InIdentity, OutError))
				{
					return false;
				}
				if (!bJoined)
				{
					return Reject(OutError, SessionCondition(TEXT("editor_session_release_before_join"), TEXT("The native session must be joined before its resources can be released."), TEXT("/joined"), false, true));
				}
				if (bReleased)
				{
					return Reject(OutError, SessionCondition(TEXT("editor_session_already_released"), TEXT("The native session resources have already been released."), TEXT("/released"), true, false));
				}
				if (!NativeSession->Release(OutError))
				{
					return false;
				}
				bReleased = true;
				return true;
			}

		private:
			bool ValidateStartedIdentity(
				const FString& InOperationId,
				const FLongRunningWorkIdentity& InIdentity,
				FExecutionMutationError& OutError) const
			{
				return bStarted
					? ValidateCallIdentity(
						OperationId,
						Identity,
						InOperationId,
						InIdentity,
						OutError)
					: Reject(OutError, SessionCondition(TEXT("editor_session_operation_not_started"), TEXT("The native session operation has not been started."), TEXT("/started"), false, true));
			}

			bool Reject(FExecutionMutationError& OutError, FDiagnostic Error) const
			{
				return Fail(OutError, WithSessionContext(MoveTemp(Error), Request));
			}

			FUeAgentEditorSessionOperationTypedRequest Request;
			FUeAgentEditorSessionOperationPolicyDescriptor Policy;
			TSharedRef<IUeAgentEditorSessionNativeSession,
				ESPMode::ThreadSafe> NativeSession;
			mutable FCriticalSection Mutex;
			FString OperationId;
			FLongRunningWorkIdentity Identity;
			ELongRunningWorkState LastObservedState =
				ELongRunningWorkState::Invalid;
			FString LastSignalKey;
			int64 SignalRevision = 0;
			bool bNativeStartAttempted = false;
			bool bStarted = false;
			bool bJoined = false;
			bool bReleased = false;
		};

		enum class EHostExitScheduleState : uint8
		{
			NotStarted,
			Scheduled,
			CallbackRunning,
			ExitRequestedAwaitingExternalWitness,
			Cancelled,
		};

		const TCHAR* HostExitStateName(EHostExitScheduleState State)
		{
			switch (State)
			{
			case EHostExitScheduleState::NotStarted: return TEXT("not_started");
			case EHostExitScheduleState::Scheduled: return TEXT("scheduled");
			case EHostExitScheduleState::CallbackRunning: return TEXT("callback_running");
			case EHostExitScheduleState::ExitRequestedAwaitingExternalWitness: return TEXT("exit_requested_awaiting_external_witness");
			case EHostExitScheduleState::Cancelled: return TEXT("cancelled");
			default: return TEXT("unknown");
			}
		}

		struct FHostExitSharedState final
		{
			FHostExitSharedState(
				FUeAgentEditorSessionOperationTypedRequest InRequest,
				FUeAgentEditorSessionOperationPolicyDescriptor InPolicy,
				TSharedRef<
					IUeAgentEditorHostExitTarget,
					ESPMode::ThreadSafe> InTarget,
				FString InOperationId,
				FLongRunningWorkIdentity InIdentity)
				: Request(MoveTemp(InRequest))
				, Policy(MoveTemp(InPolicy))
				, Target(MoveTemp(InTarget))
				, OperationId(MoveTemp(InOperationId))
				, Identity(MoveTemp(InIdentity))
			{
			}

			FCriticalSection Mutex;
			FUeAgentEditorSessionOperationTypedRequest Request;
			FUeAgentEditorSessionOperationPolicyDescriptor Policy;
			TSharedRef<IUeAgentEditorHostExitTarget, ESPMode::ThreadSafe>
				Target;
			FString OperationId;
			FLongRunningWorkIdentity Identity;
			FTSTicker::FDelegateHandle TickerHandle;
			EHostExitScheduleState State = EHostExitScheduleState::NotStarted;
			FString LastSignalPhase;
			int64 SignalRevision = 0;
			bool bJoined = false;
			bool bReleased = false;
		};

		class FHostExitSchedulePort final
			: public ILongRunningOperationPort
		{
		public:
			FHostExitSchedulePort(
				FUeAgentEditorSessionOperationTypedRequest InRequest,
				FUeAgentEditorSessionOperationPolicyDescriptor InPolicy,
				const double InExitDelaySeconds,
				TSharedRef<
					IUeAgentEditorHostExitTarget,
					ESPMode::ThreadSafe> InExitTarget)
				: Request(MoveTemp(InRequest))
				, Policy(MoveTemp(InPolicy))
				, ExitDelaySeconds(InExitDelaySeconds)
				, ExitTarget(MoveTemp(InExitTarget))
			{
			}

			~FHostExitSchedulePort() override
			{
				FTSTicker::FDelegateHandle Handle;
				if (State.IsValid())
				{
					FScopeLock Lock(&State->Mutex);
					if (State->State == EHostExitScheduleState::Scheduled
						&& State->TickerHandle.IsValid())
					{
						State->State = EHostExitScheduleState::Cancelled;
						Handle = State->TickerHandle;
						State->TickerHandle.Reset();
					}
				}
				if (Handle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(Handle);
				}
			}

			bool Start(
				const FLongRunningOperationRequest& OperationRequest,
				const FUeAgentCancellationToken& Cancellation,
				FLongRunningWorkIdentity& OutIdentity,
				FExecutionMutationError& OutError) override
			{
				if (State.IsValid())
				{
					return Reject(OutError, SessionCondition(TEXT("editor_host_exit_already_scheduled"), TEXT("This host exit port already owns a schedule and cannot be started again."), TEXT("/schedule_exists"), true, false));
				}
				if (!IsInGameThread())
				{
					return Reject(OutError, SessionCondition(TEXT("editor_host_exit_requires_game_thread"), TEXT("The editor host exit schedule must be started or cancelled on the game thread."), TEXT("/is_game_thread"), false, true));
				}
				if (!ValidateOperationContext(
						OperationRequest, Policy, OutError))
				{
					return false;
				}
				const FString& ExpectedEditorInstance =
					ExitTarget->GetEditorInstanceId();
				if (OperationRequest.GetExecutionContext().GetEditorInstanceId()
						!= ExpectedEditorInstance
					|| Request.GetSubjectRef() != ExpectedEditorInstance)
				{
					auto Error = SessionCondition(TEXT("editor_host_exit_instance_mismatch"),
						TEXT("The exit subject or execution context identifies a different editor instance from the exit target."),
						TEXT("/editor_instance_matches"), false, true);
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetStringField(TEXT("context_editor_instance"), OperationRequest.GetExecutionContext().GetEditorInstanceId());
					Actual->SetStringField(TEXT("subject_ref"), Request.GetSubjectRef());
					Error.ActualEvidence = MakeShared<FJsonValueObject>(Actual);
					Error.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedEditorInstance);
					return Reject(OutError, MoveTemp(Error));
				}
				if (Cancellation.IsCancelled())
				{
					return Reject(OutError, SessionCondition(TEXT("editor_host_exit_cancelled_before_schedule"), TEXT("Cancellation was requested before the editor host exit could be scheduled."), TEXT("/cancellation_requested"), true, false));
				}

				OutIdentity = FLongRunningWorkIdentity(
					TEXT("native:editor-host-exit:")
						+ OperationRequest.GetOperationId(),
					NextGeneration());
				State = MakeShared<FHostExitSharedState, ESPMode::ThreadSafe>(
					Request,
					Policy,
					ExitTarget,
					OperationRequest.GetOperationId(),
					OutIdentity);
				const TWeakPtr<FHostExitSharedState, ESPMode::ThreadSafe>
					WeakState = State;
				const FTSTicker::FDelegateHandle Handle =
					FTSTicker::GetCoreTicker().AddTicker(
						FTickerDelegate::CreateLambda(
							[WeakState](float)
							{
								const TSharedPtr<
									FHostExitSharedState,
									ESPMode::ThreadSafe> Pinned =
									WeakState.Pin();
								if (!Pinned.IsValid())
								{
									return false;
								}
								TSharedPtr<
									IUeAgentEditorHostExitTarget,
									ESPMode::ThreadSafe> Target;
								{
									FScopeLock Lock(&Pinned->Mutex);
									if (Pinned->State
										!= EHostExitScheduleState::Scheduled)
									{
										return false;
									}
									Pinned->State =
										EHostExitScheduleState::CallbackRunning;
									Pinned->TickerHandle.Reset();
									Target = Pinned->Target;
								}
								Target->RequestExit();
								{
									FScopeLock Lock(&Pinned->Mutex);
									Pinned->State = EHostExitScheduleState::
										ExitRequestedAwaitingExternalWitness;
								}
								return false;
							}),
						static_cast<float>(ExitDelaySeconds));
				if (!Handle.IsValid())
				{
					State.Reset();
					return Reject(OutError, SessionCondition(TEXT("editor_host_exit_schedule_failed"), TEXT("The core ticker did not create a valid callback handle for the editor host exit."), TEXT("/ticker_handle_valid"), false, true));
				}
				{
					FScopeLock Lock(&State->Mutex);
					State->TickerHandle = Handle;
					State->State = EHostExitScheduleState::Scheduled;
				}
				return true;
			}

			bool Observe(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				FUeAgentResourceTracker& Resources,
				FLongRunningWorkObservation& OutObservation,
				FExecutionMutationError& OutError) override
			{
				if (!State.IsValid())
				{
					return Reject(OutError, SessionCondition(TEXT("editor_host_exit_not_scheduled"), TEXT("The editor host exit has no started schedule."), TEXT("/schedule_started"), false, true));
				}
				FScopeLock Lock(&State->Mutex);
				if (!ValidateCallIdentity(
						State->OperationId,
						State->Identity,
						OperationId,
						Identity,
						OutError))
				{
					return false;
				}

				ELongRunningWorkState WorkState =
					ELongRunningWorkState::Running;
				FString Phase = TEXT("exit_scheduled");
				UE::UAI::Contract::FDiagnostic Diagnostic;
				if (State->State
					== EHostExitScheduleState::CallbackRunning
					|| State->State == EHostExitScheduleState::
						ExitRequestedAwaitingExternalWitness)
				{
					Phase = TEXT(
						"exit_requested_awaiting_external_witness");
				}
				else if (State->State == EHostExitScheduleState::Cancelled)
				{
					WorkState = ELongRunningWorkState::ExitedFailure;
					Phase = TEXT("cancelled");
					Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("editor_host_exit_cancelled"), TEXT("The scheduled Editor host exit was cancelled."), TEXT("cancellation"));
				}
				else if (State->State == EHostExitScheduleState::NotStarted)
				{
					return Reject(OutError, SessionCondition(TEXT("editor_host_exit_not_scheduled"), TEXT("The editor host exit has no started schedule."), TEXT("/schedule_started"), false, true));
				}

				TOptional<FLongRunningOperationSignal> Signal;
				if (Phase != State->LastSignalPhase)
				{
					const int64 NextRevision = State->SignalRevision + 1;
					if (!TryBuildSignal(
							State->Request,
							State->Policy,
							Phase,
							FString(),
							nullptr,
							NextRevision,
							Resources,
							Signal,
							OutError))
					{
						if (!Diagnostic.IsEmpty()) OutError.Append(Diagnostic);
						return false;
					}
					State->LastSignalPhase = Phase;
					State->SignalRevision = NextRevision;
				}
				OutObservation = FLongRunningWorkObservation(
					Identity.GetGeneration(),
					WorkState,
					MoveTemp(Diagnostic),
					MoveTemp(Signal));
				return true;
			}

			bool RequestStop(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				const FString& Reason,
				FExecutionMutationError& OutError) override
			{
				if (!State.IsValid())
				{
					return Reject(OutError, SessionCondition(TEXT("editor_host_exit_not_scheduled"), TEXT("The editor host exit has no started schedule."), TEXT("/schedule_started"), false, true));
				}
				if (!IsInGameThread())
				{
					return Reject(OutError, SessionCondition(TEXT("editor_host_exit_requires_game_thread"), TEXT("The editor host exit schedule must be started or cancelled on the game thread."), TEXT("/is_game_thread"), false, true));
				}
				FTSTicker::FDelegateHandle Handle;
				{
					FScopeLock Lock(&State->Mutex);
					if (!ValidateCallIdentity(
							State->OperationId,
							State->Identity,
							OperationId,
							Identity,
							OutError))
					{
						return false;
					}
					if (State->State == EHostExitScheduleState::Cancelled)
					{
						return true;
					}
					if (State->State
						!= EHostExitScheduleState::Scheduled)
					{
						return Reject(OutError, SessionText(TEXT("editor_host_exit_already_requested"), TEXT("The editor host exit callback has progressed beyond the cancellable scheduled state."), TEXT("/schedule_state"), HostExitStateName(State->State), TEXT("scheduled")));
					}
					State->State = EHostExitScheduleState::Cancelled;
					Handle = State->TickerHandle;
					State->TickerHandle.Reset();
				}
				if (Handle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(Handle);
				}
				return true;
			}

			bool Join(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				FExecutionMutationError& OutError) override
			{
				if (!State.IsValid())
				{
					return Reject(OutError, SessionCondition(TEXT("editor_host_exit_not_scheduled"), TEXT("The editor host exit has no started schedule."), TEXT("/schedule_started"), false, true));
				}
				FScopeLock Lock(&State->Mutex);
				if (!ValidateCallIdentity(
						State->OperationId,
						State->Identity,
						OperationId,
						Identity,
						OutError))
				{
					return false;
				}
				if (State->State != EHostExitScheduleState::Cancelled)
				{
					return Reject(OutError, SessionText(TEXT("editor_host_exit_requires_external_witness"), TEXT("The host exit can be joined locally only after cancellation; an executed exit requires an external process witness."), TEXT("/schedule_state"), HostExitStateName(State->State), TEXT("cancelled for local join")));
				}
				State->bJoined = true;
				return true;
			}

			bool Release(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				FExecutionMutationError& OutError) override
			{
				if (!State.IsValid())
				{
					return Reject(OutError, SessionCondition(TEXT("editor_host_exit_not_scheduled"), TEXT("The editor host exit has no started schedule."), TEXT("/schedule_started"), false, true));
				}
				FScopeLock Lock(&State->Mutex);
				if (!ValidateCallIdentity(
						State->OperationId,
						State->Identity,
						OperationId,
						Identity,
						OutError))
				{
					return false;
				}
				if (!State->bJoined)
				{
					return Reject(OutError, SessionCondition(TEXT("editor_host_exit_release_before_join"), TEXT("The editor host exit schedule must be joined before it can be released."), TEXT("/joined"), false, true));
				}
				if (State->bReleased)
				{
					return Reject(OutError, SessionCondition(TEXT("editor_host_exit_already_released"), TEXT("The editor host exit schedule has already been released."), TEXT("/released"), true, false));
				}
				State->bReleased = true;
				return true;
			}

		private:
			FUeAgentEditorSessionOperationTypedRequest Request;
			bool Reject(FExecutionMutationError& OutError, FDiagnostic Error) const
			{
				return Fail(OutError, WithSessionContext(MoveTemp(Error), Request));
			}
			FUeAgentEditorSessionOperationPolicyDescriptor Policy;
			double ExitDelaySeconds = 0.0;
			TSharedRef<IUeAgentEditorHostExitTarget, ESPMode::ThreadSafe>
				ExitTarget;
			TSharedPtr<FHostExitSharedState, ESPMode::ThreadSafe> State;
		};

		class FPlatformEditorHostExitTarget final
			: public IUeAgentEditorHostExitTarget
		{
		public:
			explicit FPlatformEditorHostExitTarget(FString InEditorInstanceId)
				: EditorInstanceId(MoveTemp(InEditorInstanceId))
			{
			}

			const FString& GetEditorInstanceId() const override
			{
				return EditorInstanceId;
			}

			void RequestExit() override
			{
				FPlatformMisc::RequestExit(false);
			}

		private:
			FString EditorInstanceId;
		};
	}

	FUeAgentEditorSessionOperationPolicyDescriptor::
	FUeAgentEditorSessionOperationPolicyDescriptor(
		const EUeAgentEditorSessionOperationFamily InFamily,
		FString InPolicyId,
		FString InNativeMechanismId,
		FString InReadbackOracleId,
		const EUeAgentStatePlane InRequiredStatePlane,
		TArray<EUeAgentExecutionWorldType> InAllowedWorldTypes,
		FString InSubjectSchemaJson,
		FString InParameterSchemaJson,
		FString InParameterGuidance,
		FString InResultGuidance,
		FString InMinimumExample)
		: Family(InFamily)
		, PolicyId(MoveTemp(InPolicyId))
		, NativeMechanismId(MoveTemp(InNativeMechanismId))
		, ReadbackOracleId(MoveTemp(InReadbackOracleId))
		, RequiredStatePlane(InRequiredStatePlane)
		, AllowedWorldTypes(MoveTemp(InAllowedWorldTypes))
		, SubjectSchemaJson(MoveTemp(InSubjectSchemaJson))
		, ParameterSchemaJson(MoveTemp(InParameterSchemaJson))
		, ParameterGuidance(MoveTemp(InParameterGuidance))
		, ResultGuidance(MoveTemp(InResultGuidance))
		, MinimumExample(MoveTemp(InMinimumExample))
	{
		ValidationDiagnostic = PolicyValidation(*this);
	}

	EUeAgentEditorSessionOperationFamily
	FUeAgentEditorSessionOperationPolicyDescriptor::GetFamily() const
	{
		return Family;
	}

	const FString&
	FUeAgentEditorSessionOperationPolicyDescriptor::GetPolicyId() const
	{
		return PolicyId;
	}

	const FString&
	FUeAgentEditorSessionOperationPolicyDescriptor::
	GetNativeMechanismId() const
	{
		return NativeMechanismId;
	}

	const FString&
	FUeAgentEditorSessionOperationPolicyDescriptor::GetReadbackOracleId() const
	{
		return ReadbackOracleId;
	}

	EUeAgentStatePlane
	FUeAgentEditorSessionOperationPolicyDescriptor::
	GetRequiredStatePlane() const
	{
		return RequiredStatePlane;
	}

	const TArray<EUeAgentExecutionWorldType>&
	FUeAgentEditorSessionOperationPolicyDescriptor::
	GetAllowedWorldTypes() const
	{
		return AllowedWorldTypes;
	}

	const FString& FUeAgentEditorSessionOperationPolicyDescriptor::
	GetSubjectSchemaJson() const
	{
		return SubjectSchemaJson;
	}

	const FString& FUeAgentEditorSessionOperationPolicyDescriptor::
	GetParameterSchemaJson() const
	{
		return ParameterSchemaJson;
	}

	const FString& FUeAgentEditorSessionOperationPolicyDescriptor::
	GetParameterGuidance() const
	{
		return ParameterGuidance;
	}

	const FString& FUeAgentEditorSessionOperationPolicyDescriptor::
	GetResultGuidance() const
	{
		return ResultGuidance;
	}

	const FString& FUeAgentEditorSessionOperationPolicyDescriptor::
	GetMinimumExample() const
	{
		return MinimumExample;
	}

	bool FUeAgentEditorSessionOperationPolicyDescriptor::
	HasDiscoveryContract() const
	{
		return !SubjectSchemaJson.IsEmpty()
			&& !ParameterSchemaJson.IsEmpty()
			&& !ParameterGuidance.IsEmpty()
			&& !ResultGuidance.IsEmpty()
			&& !MinimumExample.IsEmpty();
	}

	bool FUeAgentEditorSessionOperationPolicyDescriptor::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	const FDiagnostic& FUeAgentEditorSessionOperationPolicyDescriptor::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
	}

	FUeAgentEditorSessionOperationPolicyRegistration::
	FUeAgentEditorSessionOperationPolicyRegistration(
		FUeAgentEditorSessionOperationPolicyDescriptor InDescriptor,
		TSharedRef<IUeAgentEditorSessionOperationPortBuilder,
			ESPMode::ThreadSafe> InBuilder)
		: Descriptor(MoveTemp(InDescriptor))
		, Builder(MoveTemp(InBuilder))
	{
	}

	const FUeAgentEditorSessionOperationPolicyDescriptor&
	FUeAgentEditorSessionOperationPolicyRegistration::GetDescriptor() const
	{
		return Descriptor;
	}

	IUeAgentEditorSessionOperationPortBuilder&
	FUeAgentEditorSessionOperationPolicyRegistration::GetBuilder() const
	{
		return Builder.Get();
	}

	FUeAgentEditorSessionOperationProductionPortFactory::
	FUeAgentEditorSessionOperationProductionPortFactory(
		TArray<FUeAgentEditorSessionOperationPolicyRegistration>
			InRegistrations,
		TSharedRef<
			IUeAgentEditorSessionOperationTargetResolver,
			ESPMode::ThreadSafe> InTargetResolver)
		: Registrations(MoveTemp(InRegistrations))
		, TargetResolver(MoveTemp(InTargetResolver))
	{
		Descriptors.Reserve(Registrations.Num());
		for (const FUeAgentEditorSessionOperationPolicyRegistration& Registration
			: Registrations)
		{
			Descriptors.Add(Registration.GetDescriptor());
		}
	}

	bool FUeAgentEditorSessionOperationProductionPortFactory::TryCreate(
		const TArray<FUeAgentEditorSessionOperationPolicyRegistration>&
			InRegistrations,
		TSharedPtr<FUeAgentEditorSessionOperationProductionPortFactory>&
			OutFactory,
		FExecutionMutationError& OutError)
	{
		return TryCreate(
			InRegistrations,
			MakeShared<FProductionTargetResolver, ESPMode::ThreadSafe>(),
			OutFactory,
			OutError);
	}

	bool FUeAgentEditorSessionOperationProductionPortFactory::TryCreate(
		const TArray<FUeAgentEditorSessionOperationPolicyRegistration>&
			InRegistrations,
		TSharedRef<
			IUeAgentEditorSessionOperationTargetResolver,
			ESPMode::ThreadSafe> InTargetResolver,
		TSharedPtr<FUeAgentEditorSessionOperationProductionPortFactory>&
			OutFactory,
		FExecutionMutationError& OutError)
	{
		OutFactory.Reset();
		if (InRegistrations.IsEmpty())
		{
			return Fail(OutError, SessionFact(TEXT("editor_session_operation_policy_catalog_empty"), TEXT("The editor session operation factory requires at least one registered policy."), TEXT("/registrations"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("one or more policy registrations"))));
		}
		for (int32 Index = 0; Index < InRegistrations.Num(); ++Index)
		{
			const FUeAgentEditorSessionOperationPolicyDescriptor& Descriptor =
				InRegistrations[Index].GetDescriptor();
			if (!Descriptor.IsValid())
			{
				auto Error = Descriptor.GetValidationDiagnostic();
				Error.OperationIndex = Index;
				if (Error.CandidateName.IsEmpty()) Error.CandidateName = Descriptor.GetPolicyId();
				return Fail(OutError, MoveTemp(Error));
			}
			for (int32 Other = 0; Other < Index; ++Other)
			{
				const FUeAgentEditorSessionOperationPolicyDescriptor& Existing =
					InRegistrations[Other].GetDescriptor();
				if (Existing.GetFamily() == Descriptor.GetFamily()
					&& Existing.GetPolicyId() == Descriptor.GetPolicyId())
				{
					return Fail(OutError, SessionText(TEXT("editor_session_operation_policy_duplicate"),
						TEXT("Two registrations declare the same policy identifier for the same operation family."),
						FString::Printf(TEXT("/registrations/%d/policy_id"), Index), Descriptor.GetPolicyId(),
						TEXT("a unique policy identifier within the operation family")));
				}
			}
		}
		OutFactory = MakeShareable(
			new FUeAgentEditorSessionOperationProductionPortFactory(
				InRegistrations,
				MoveTemp(InTargetResolver)));
		return true;
	}

	const TArray<FUeAgentEditorSessionOperationPolicyDescriptor>&
	FUeAgentEditorSessionOperationProductionPortFactory::
	GetPolicyDescriptors() const
	{
		return Descriptors;
	}

	bool FUeAgentEditorSessionOperationProductionPortFactory::
	TryCreateOperationPort(
		const FUeAgentEditorSessionOperationTypedRequest& Request,
		const FLongRunningOperationRequest& OperationRequest,
		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
		FExecutionMutationError& OutError)
	{
		OutPort.Reset();
		const FUeAgentEditorSessionOperationPolicyRegistration* Registration =
			Registrations.FindByPredicate(
				[&Request](
					const FUeAgentEditorSessionOperationPolicyRegistration&
						Candidate)
				{
					return MatchesPolicy(
						Request, Candidate.GetDescriptor());
				});
		if (Registration == nullptr)
		{
			TArray<TSharedPtr<FJsonValue>> Choices;
			for (const auto& Descriptor : Descriptors)
				if (Descriptor.GetFamily() == Request.GetFamily())
					Choices.Add(MakeShared<FJsonValueString>(Descriptor.GetPolicyId()));
			return Fail(OutError, SessionFact(TEXT("editor_session_operation_policy_not_registered"),
				TEXT("The requested policy is not registered for this operation family."), TEXT("/policy_id"),
				MakeShared<FJsonValueString>(Request.GetPolicyId()), MakeShared<FJsonValueArray>(MoveTemp(Choices))));
		}
		if (!Request.IsValid())
		{
			return Fail(OutError, Request.GetValidationDiagnostic());
		}
		if (!TargetResolver->TryResolveTarget(
				Request,
				Registration->GetDescriptor(),
				OutError))
		{
			return false;
		}
		if (!Registration->GetBuilder().TryCreatePort(
				Request,
				OperationRequest,
				Registration->GetDescriptor(),
				OutPort,
				OutError))
		{
			return false;
		}
		return OutPort.IsValid()
			? true
			: Fail(OutError, SessionCondition(TEXT("editor_session_operation_port_missing"), TEXT("The registered editor session builder returned no execution port."), TEXT("/execution_port_present"), false, true));
	}

	FUeAgentEditorSessionNativeObservation::
	FUeAgentEditorSessionNativeObservation(
		const ELongRunningWorkState InState,
		FString InPhase,
		UE::UAI::Contract::FDiagnostic InDiagnostic,
		FString InSessionRef,
		TSharedPtr<FJsonObject> InDetails)
		: State(InState)
		, Phase(MoveTemp(InPhase))
		, Diagnostic(MoveTemp(InDiagnostic))
		, SessionRef(MoveTemp(InSessionRef))
		, Details(MoveTemp(InDetails))
	{
	}

	ELongRunningWorkState
	FUeAgentEditorSessionNativeObservation::GetState() const
	{
		return State;
	}

	const FString& FUeAgentEditorSessionNativeObservation::GetPhase() const
	{
		return Phase;
	}

	const UE::UAI::Contract::FDiagnostic& FUeAgentEditorSessionNativeObservation::GetDiagnostic() const
	{
		return Diagnostic;
	}

	const FString&
	FUeAgentEditorSessionNativeObservation::GetSessionRef() const
	{
		return SessionRef;
	}

	const TSharedPtr<FJsonObject>&
	FUeAgentEditorSessionNativeObservation::GetDetails() const
	{
		return Details;
	}

	bool FUeAgentEditorSessionNativeObservation::IsValid() const
	{
		if (State == ELongRunningWorkState::Invalid
			|| !IsNormalizedNonEmpty(Phase)
			|| (!SessionRef.IsEmpty()
				&& !IsNormalizedNonEmpty(SessionRef)))
		{
			return false;
		}
		if (State == ELongRunningWorkState::ExitedFailure
			|| State == ELongRunningWorkState::StateUnknown)
		{
			return IsNormalizedNonEmpty(Diagnostic.Code);
		}
		return Diagnostic.IsEmpty();
	}

	bool TryCreateEditorSessionNativeResourcePort(
		const FUeAgentEditorSessionOperationTypedRequest& Request,
		const FUeAgentEditorSessionOperationPolicyDescriptor& Policy,
		TSharedRef<IUeAgentEditorSessionNativeSession, ESPMode::ThreadSafe>
			NativeSession,
		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
		FExecutionMutationError& OutError)
	{
		OutPort.Reset();
		if (!Request.IsValid() || !Policy.IsValid()
			|| !MatchesPolicy(Request, Policy))
		{
			return Fail(OutError, RequestPolicyFailure(TEXT("editor_session_native_resource_contract_invalid"), Request, Policy));
		}
		if (Policy.GetNativeMechanismId()
			!= TEXT("native_session_resource"))
		{
			return Fail(OutError, SessionText(TEXT("editor_session_native_mechanism_mismatch"),
				TEXT("The selected policy does not use the native session resource mechanism."),
				TEXT("/native_mechanism_id"), Policy.GetNativeMechanismId(), TEXT("native_session_resource")));
		}
		OutPort = MakeShared<FNativeSessionResourcePort, ESPMode::ThreadSafe>(
			Request, Policy, MoveTemp(NativeSession));
		return true;
	}

	bool TryCreateEditorPlatformHostExitTarget(
		FString EditorInstanceId,
		TSharedPtr<IUeAgentEditorHostExitTarget, ESPMode::ThreadSafe>&
			OutTarget,
		FExecutionMutationError& OutError)
	{
		OutTarget.Reset();
		if (!IsNormalizedNonEmpty(EditorInstanceId))
		{
			auto Error = SessionCondition(TEXT("editor_host_exit_instance_id_invalid"),
				TEXT("The exit target editor instance identifier is empty or has surrounding whitespace."),
				TEXT("/editor_instance_id_normalized"), false, true);
			Error.ActualEvidence = MakeShared<FJsonValueString>(EditorInstanceId);
			return Fail(OutError, MoveTemp(Error));
		}
		OutTarget = MakeShared<FPlatformEditorHostExitTarget,
			ESPMode::ThreadSafe>(MoveTemp(EditorInstanceId));
		return true;
	}

	bool TryCreateEditorHostExitSchedulePort(
		const FUeAgentEditorSessionOperationTypedRequest& Request,
		const FUeAgentEditorSessionOperationPolicyDescriptor& Policy,
		const double ExitDelaySeconds,
		TSharedRef<IUeAgentEditorHostExitTarget, ESPMode::ThreadSafe>
			ExitTarget,
		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
		FExecutionMutationError& OutError)
	{
		OutPort.Reset();
		if (!Request.IsValid() || !Policy.IsValid()
			|| !MatchesPolicy(Request, Policy))
		{
			return Fail(OutError, RequestPolicyFailure(TEXT("editor_host_exit_contract_invalid"), Request, Policy));
		}
		if (Policy.GetNativeMechanismId() != TEXT("host_exit_schedule"))
		{
			return Fail(OutError, SessionText(TEXT("editor_session_native_mechanism_mismatch"),
				TEXT("The selected policy does not use the host exit scheduling mechanism."),
				TEXT("/native_mechanism_id"), Policy.GetNativeMechanismId(), TEXT("host_exit_schedule")));
		}
		if (!FMath::IsFinite(ExitDelaySeconds)
			|| ExitDelaySeconds < 0.0)
		{
			TSharedPtr<FJsonValue> Actual;
			if (FMath::IsFinite(ExitDelaySeconds)) Actual = MakeShared<FJsonValueNumber>(ExitDelaySeconds);
			else Actual = MakeShared<FJsonValueString>(FMath::IsNaN(ExitDelaySeconds)
				? TEXT("NaN") : (ExitDelaySeconds > 0.0 ? TEXT("+Infinity") : TEXT("-Infinity")));
			return Fail(OutError, SessionFact(TEXT("editor_host_exit_delay_invalid"),
				TEXT("The editor host exit delay must be finite and non-negative, in seconds."),
				TEXT("/exit_delay_seconds"), MoveTemp(Actual), MakeShared<FJsonValueString>(TEXT("finite seconds >= 0"))));
		}
		if (!IsNormalizedNonEmpty(ExitTarget->GetEditorInstanceId()))
		{
			auto Error = SessionCondition(TEXT("editor_host_exit_instance_id_invalid"),
				TEXT("The exit target editor instance identifier is empty or has surrounding whitespace."),
				TEXT("/editor_instance_id_normalized"), false, true);
			Error.ActualEvidence = MakeShared<FJsonValueString>(ExitTarget->GetEditorInstanceId());
			return Fail(OutError, MoveTemp(Error));
		}
		OutPort = MakeShared<FHostExitSchedulePort, ESPMode::ThreadSafe>(
			Request,
			Policy,
			ExitDelaySeconds,
			MoveTemp(ExitTarget));
		return true;
	}
}
