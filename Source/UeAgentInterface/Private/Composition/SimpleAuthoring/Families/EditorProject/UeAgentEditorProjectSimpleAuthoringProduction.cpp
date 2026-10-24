#include "Composition/SimpleAuthoring/Families/EditorProject/UeAgentEditorProjectSimpleAuthoringProduction.h"
#include "Core/Contract/UeAgentContractJson.h"

#include "Composition/SimpleAuthoring/Families/EditorProject/UeAgentEditorProjectSimpleAuthoringStateMechanics.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Composition::SimpleAuthoring::EditorProject
{
	namespace
	{
		using namespace UE::UAI::AssetDurable;
		using namespace UE::UAI::ContextResource;
		using namespace UE::UAI::Contract;
		using namespace UE::UAI::Discovery;
		using namespace UE::UAI::DomainAdapters::
			EditorProjectSimpleEffect;
		using namespace UE::UAI::DomainAdapters::GenericEffect;
		using namespace UE::UAI::ExecutionMutation;
		using namespace UE::UAI::PlanningPreflight;
		using namespace UE::UAI::StateSemantic;
		using namespace UE::UAI::Verification;

		constexpr const TCHAR* PreStatePortId =
			TEXT("editor-project.authoring-memory.v1");
		constexpr const TCHAR* PersistedStatePortId =
			TEXT("editor-project.independent-readback.v1");
		constexpr const TCHAR* RequestEvidencePrefix =
			TEXT("editor-project-request-v1:");
		constexpr const TCHAR* EvidenceDurableKind =
			TEXT("_uai_durable_kind");
		constexpr const TCHAR* EvidenceDurableLocation =
			TEXT("_uai_durable_location");

		using FEditorProjectProfile =
			FUeAgentEditorProjectSimpleEffectProductionProfile;

		struct FEditorProjectProfileCatalog
		{
			FEditorProjectProfileCatalog()
			{
				TryGetEditorProjectSimpleEffectProductionProfiles(
					Profiles,
					Error);
			}

			TArray<FEditorProjectProfile> Profiles;
			UE::UAI::Contract::FDiagnostic Error;
		};

		const FEditorProjectProfileCatalog& GetProfileCatalog()
		{
			static const FEditorProjectProfileCatalog Catalog;
			return Catalog;
		}

		const FEditorProjectProfile* FindProfile(
			const FUeAgentDescriptorIdentity& Identity)
		{
			for (const FEditorProjectProfile& Profile
				: GetProfileCatalog().Profiles)
			{
				if (Identity.GetDescriptorId() == Profile.DescriptorId
					&& Identity.GetVersion() == Profile.DescriptorVersion
					&& Identity.GetDescriptorHash()
						== Profile.DescriptorHash)
				{
					return &Profile;
				}
			}
			return nullptr;
		}



		FDiagnostic ResolvedEditorProjectTargetFailure(
			const FUeAgentEditorProjectSimpleEffectResolvedTarget& Target,
			const FString& TargetRef, const FString& ExpectedProfile,
			const FString& Code, const FString& ExpectedRevision = FString(), bool bCheckRevision = false)
		{
			if (!Target.IsValid() && !Target.GetValidationDiagnostic().IsEmpty())
				return Target.GetValidationDiagnostic();
			const bool bCanonicalMismatch = Target.IsValid() && Target.GetCanonicalRef() != TargetRef;
			const bool bProfileMismatch = Target.IsValid() && Target.GetProfileId() != ExpectedProfile;
			const bool bRevisionMismatch = Target.IsValid() && bCheckRevision && Target.GetRevision() != ExpectedRevision;
			auto Error = FDiagnostic::Failure(Code,
				!Target.IsValid() ? TEXT("The resolver did not return a usable editor project target.")
				: bCanonicalMismatch ? TEXT("The resolver returned a different editor project target.")
				: bProfileMismatch ? TEXT("The resolved editor project target has a different target kind.")
				: bRevisionMismatch ? TEXT("The editor project target changed after its revision was captured.")
				: TEXT("The resolver did not confirm the editor project target resolution."), TEXT("simple_authoring"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = !Target.IsValid() ? TEXT("/target_resolved")
				: bCanonicalMismatch ? TEXT("/target_ref")
				: bProfileMismatch ? TEXT("/target_kind_matches")
				: bRevisionMismatch ? TEXT("/target_revision_matches") : TEXT("/target_resolution_confirmed");
			Error.ActualValue = bCanonicalMismatch
				? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Target.GetCanonicalRef()))
				: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false));
			Error.ExpectedValue = bCanonicalMismatch
				? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TargetRef))
				: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true));
			Error.ActualValuePresent = true;
			Error.ExpectedValuePresent = true;
			if (bProfileMismatch)
			{
				Error.ActualEvidence = MakeShared<FJsonValueString>(Target.GetProfileId());
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedProfile);
			}
			if (bRevisionMismatch)
			{
				Error.ActualEvidence = MakeShared<FJsonValueString>(Target.GetRevision());
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedRevision);
			}
			return Error;
		}

		void SetProductionError(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
        { OutError = MoveTemp(Diagnostic); if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;  }


		FDiagnostic EditorProjectFailure(const FString& Code, const FString& Reason, const FString& Field)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("simple_authoring"));
			Error.Schema = TEXT("uai.runtime.diagnostic/1");
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			return Error;
		}

		FDiagnostic JsonValueFailure(const FString& Path, const TSharedPtr<FJsonValue>& Actual, const FString& Expected, const bool bInternal = false)
		{
			auto Error = EditorProjectFailure(TEXT("editor_project_state_field_invalid"),
				TEXT("The EditorProject state field does not satisfy the required value."), Path);
			Error.ActualValuePresent = Actual.IsValid();
			if (bInternal) Error.ActualEvidence = Actual;
			else if (Actual.IsValid() && (Actual->Type == EJson::Array || Actual->Type == EJson::Object))
			{
				auto Summary = MakeShared<FJsonObject>();
				Summary->SetStringField(TEXT("kind"), Actual->Type == EJson::Array ? TEXT("array") : TEXT("object"));
				if (Actual->Type == EJson::Array) Summary->SetNumberField(TEXT("count"), Actual->AsArray().Num());
				else if (Actual->AsObject().IsValid()) Summary->SetNumberField(TEXT("count"), Actual->AsObject()->Values.Num());
				Error.ActualValue = MakeShared<FJsonValueObject>(Summary);
			}
			else Error.ActualValue = Actual;
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return Error;
		}

		FDiagnostic JsonFieldFailure(const TSharedPtr<FJsonObject>& Object, const FString& Field,
			const FString& Expected, const FString& Path = FString(), const bool bInternal = false)
		{
			return JsonValueFailure(Path.IsEmpty() ? Field : Path,
				Object.IsValid() ? Object->Values.FindRef(Field) : nullptr, Expected, bInternal);
		}

		FDiagnostic MissingProfile(const FUeAgentDescriptorIdentity& Identity)
		{
			const auto& Catalog = GetProfileCatalog();
			if (!Catalog.Error.IsEmpty()) return Catalog.Error;
			const auto* SameId = Catalog.Profiles.FindByPredicate([&](const FEditorProjectProfile& Profile)
				{ return Profile.DescriptorId == Identity.GetDescriptorId(); });
			const auto* SameVersion = Catalog.Profiles.FindByPredicate([&](const FEditorProjectProfile& Profile)
				{ return Profile.DescriptorId == Identity.GetDescriptorId() && Profile.DescriptorVersion == Identity.GetVersion(); });
			auto Error = EditorProjectFailure(TEXT("editor_project_profile_missing"),
				!SameId ? TEXT("No EditorProject profile is registered for the requested descriptor.")
					: !SameVersion ? TEXT("No registered EditorProject profile matches the requested descriptor version.")
					: TEXT("The EditorProject descriptor hash differs from the registered profile."),
				!SameId ? TEXT("descriptor_id") : !SameVersion ? TEXT("descriptor_version") : TEXT("descriptor_hash"));
			Error.CandidateName = Identity.GetDescriptorId();
			if (!SameId)
			{
				Error.ActualValue = MakeShared<FJsonValueString>(Identity.GetDescriptorId());
				TArray<TSharedPtr<FJsonValue>> Supported;
				for (const auto& Profile : Catalog.Profiles) Supported.Add(MakeShared<FJsonValueString>(Profile.DescriptorId));
				Error.ExpectedValue = MakeShared<FJsonValueArray>(Supported);
			}
			else if (!SameVersion)
			{
				Error.ActualValue = MakeShared<FJsonValueNumber>(Identity.GetVersion());
				TArray<TSharedPtr<FJsonValue>> Versions;
				for (const auto& Profile : Catalog.Profiles)
					if (Profile.DescriptorId == Identity.GetDescriptorId()) Versions.Add(MakeShared<FJsonValueNumber>(Profile.DescriptorVersion));
				Error.ExpectedValue = MakeShared<FJsonValueArray>(Versions);
			}
			else
			{
				Error.ActualEvidence = MakeShared<FJsonValueString>(Identity.GetDescriptorHash());
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(SameVersion->DescriptorHash);
			}
			return Error;
		}

		FDiagnostic CountFailure(const FString& Code, const FString& Reason, const FString& Field, const int32 Actual, const int32 Expected)
		{
			auto Error = EditorProjectFailure(Code, Reason, Field);
			Error.ActualValue = MakeShared<FJsonValueNumber>(Actual);
			Error.ExpectedValue = MakeShared<FJsonValueNumber>(Expected);
			return Error;
		}

		bool TrySerializeJson(const TSharedPtr<FJsonObject>& Object, FString& OutJson, FDiagnostic& OutError)
		{
			OutJson.Reset();
			if (!Object.IsValid())
			{
				OutError = EditorProjectFailure(TEXT("editor_project_request_evidence_invalid"),
					TEXT("The EditorProject request evidence object is missing."), TEXT("binding_evidence"));
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
			if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
			{
				OutError = EditorProjectFailure(TEXT("editor_project_request_evidence_invalid"),
					TEXT("The EditorProject request evidence object could not be serialized to JSON."), TEXT("binding_evidence"));
				return false;
			}
			return true;
		}

		FString DurableKindName(const EDurableArtifactKind Kind)
		{
			return Kind == EDurableArtifactKind::Package ? TEXT("package")
				: Kind == EDurableArtifactKind::ProjectConfig ? TEXT("project_config")
				: FString::FromInt(static_cast<int32>(Kind));
		}

		bool TryParseEvidence(
			const FUeAgentCandidateBinding& Binding,
			TSharedPtr<FJsonObject>& OutPayload,
			FUeAgentDiscoveryError& OutError)
		{
			const FString& Evidence = Binding.GetBindingEvidence();
			if (!Evidence.StartsWith(RequestEvidencePrefix))
			{
				OutError = EditorProjectFailure(TEXT("editor_project_request_evidence_invalid"),
					TEXT("The EditorProject binding evidence has an unsupported format prefix."), TEXT("binding_evidence/prefix"));
				OutError.SubjectId = Binding.GetBindingId();
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Evidence.Left(FCString::Strlen(RequestEvidencePrefix)));
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(RequestEvidencePrefix);
				return false;
			}
			if (!Contract::Json::TryParseObject(Evidence.RightChop(FCString::Strlen(RequestEvidencePrefix)), OutPayload, OutError))
			{
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("binding_evidence");
				OutError.SubjectId = Binding.GetBindingId();
				return false;
			}
			return true;
		}

		bool TryReadEvidence(const TSharedPtr<FJsonObject>& Payload, FString& OutTargetRef,
			FString& OutExpectedRevision, EDurableArtifactKind& OutKind, FString& OutLocation, FDiagnostic& OutError)
		{
			OutTargetRef.Reset();
			OutExpectedRevision.Reset();
			OutLocation.Reset();
			FString Kind;
			const auto Read = [&](const TCHAR* Field, FString& Value, const TCHAR* Path, const bool bInternal)
			{
				if (Payload.IsValid() && Payload->TryGetStringField(Field, Value)) return true;
				OutError = JsonFieldFailure(Payload, Field, TEXT("string"), Path, bInternal);
				return false;
			};
			if (!Read(TEXT("target_ref"), OutTargetRef, TEXT("target_ref"), false)
				|| !Read(TEXT("expected_revision"), OutExpectedRevision, TEXT("expected_revision"), true)
				|| !Read(EvidenceDurableKind, Kind, TEXT("binding_evidence/durable_kind"), false)
				|| !Read(EvidenceDurableLocation, OutLocation, TEXT("binding_evidence/durable_location"), true)) return false;
			if (OutTargetRef.IsEmpty() || OutExpectedRevision.IsEmpty() || OutLocation.IsEmpty())
			{
				const bool bTarget = OutTargetRef.IsEmpty();
				const bool bRevision = OutExpectedRevision.IsEmpty();
				OutError = JsonValueFailure(bTarget ? TEXT("target_ref") : bRevision ? TEXT("expected_revision") : TEXT("binding_evidence/durable_location"),
					MakeShared<FJsonValueString>(TEXT("")), TEXT("nonempty string"), !bTarget);
				return false;
			}
			if (Kind == TEXT("package"))
			{
				OutKind = EDurableArtifactKind::Package;
				return true;
			}
			if (Kind == TEXT("project_config"))
			{
				OutKind = EDurableArtifactKind::ProjectConfig;
				return true;
			}
			OutError = JsonValueFailure(TEXT("binding_evidence/durable_kind"),
				MakeShared<FJsonValueString>(Kind), TEXT("package or project_config"));
			return false;
		}

		bool TryCanonicalJson(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError,
			const FString& Path = TEXT("state"))
		{
			OutValue.Reset();
			if (!JsonValue.IsValid())
			{
				OutError = EditorProjectFailure(TEXT("editor_project_state_value_missing"),
					TEXT("The generated EditorProject state contains a missing JSON value."), Path);
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			const auto Complete = [&](const bool bSucceeded)
			{
				if (!bSucceeded)
				{
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = OutError.PropertyPath.IsEmpty()
						? Path : Path + (OutError.PropertyPath.StartsWith(TEXT("/")) ? OutError.PropertyPath : TEXT("/") + OutError.PropertyPath);
				}
				return bSucceeded;
			};
			switch (JsonValue->Type)
			{
			case EJson::Null:
				return Complete(FUeAgentCanonicalValue::TryNull(
					Resources,
					OutValue,
					OutError));
			case EJson::Boolean:
				return Complete(FUeAgentCanonicalValue::TryBoolean(
					JsonValue->AsBool(),
					Resources,
					OutValue,
					OutError));
			case EJson::String:
				return Complete(FUeAgentCanonicalValue::TryString(
					JsonValue->AsString(),
					Resources,
					OutValue,
					OutError));
			case EJson::Number:
				{
					const double Number = JsonValue->AsNumber();
					const bool bInteger = FMath::IsNearlyEqual(Number, FMath::FloorToDouble(Number))
						&& Number >= static_cast<double>(MIN_int64) && Number <= static_cast<double>(MAX_int64);
					return Complete(bInteger
						? FUeAgentCanonicalValue::TryInteger(FString::Printf(TEXT("%lld"), static_cast<long long>(Number)), Resources, OutValue, OutError)
						: FUeAgentCanonicalValue::TryDecimal(FString::SanitizeFloat(Number), Resources, OutValue, OutError));
				}
			case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					const auto& Values = JsonValue->AsArray();
					for (int32 Index = 0; Index < Values.Num(); ++Index)
					{
						FUeAgentCanonicalValuePtr CanonicalItem;
						if (!TryCanonicalJson(
								Values[Index],
								Resources,
								CanonicalItem,
								OutError,
								Path + TEXT("/") + FString::FromInt(Index)))
						{
							return false;
						}
						Items.Add(CanonicalItem.ToSharedRef());
					}
					return Complete(FUeAgentCanonicalValue::TryArray(
						Items,
						Resources,
						OutValue,
						OutError));
				}
			case EJson::Object:
				{
					TArray<
						TPair<
							FString,
							FUeAgentCanonicalValueRef>> Items;
					for (const TPair<
						FString,
						TSharedPtr<FJsonValue>>& Pair
						: JsonValue->AsObject()->Values)
					{
						FUeAgentCanonicalValuePtr CanonicalItem;
						if (!TryCanonicalJson(
								Pair.Value,
								Resources,
								CanonicalItem,
								OutError,
								Path + TEXT("/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))))
						{
							return false;
						}
						Items.Emplace(
							Pair.Key,
							CanonicalItem.ToSharedRef());
					}
					return Complete(FUeAgentCanonicalValue::TryObject(
						Items,
						Resources,
						OutValue,
						OutError));
				}
			default:
				OutError = EditorProjectFailure(TEXT("editor_project_state_value_kind_invalid"),
					TEXT("The generated EditorProject state contains an unsupported JSON kind."), Path);
				OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(JsonValue->Type));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("null, boolean, string, number, array or object"));
				return false;
			}
		}

		FUeAgentCanonicalType EditorProjectStateType(
			const FString& TypeId = TEXT("editor_project_current_state"))
		{
			return FUeAgentCanonicalType(
				TypeId,
				EUeAgentValueKind::Object,
				false,
				TEXT("object"));
		}

		FUeAgentDefaultValue EditorProjectStateDefault(
			const FUeAgentCanonicalValueRef& Value)
		{
			return FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::Schema,
				TEXT("editor-project:empty-default"),
				TEXT("EditorProject state has no implicit target"),
				Value);
		}

		FUeAgentStateDetails EditorProjectStateDetails(
			const FEditorProjectProfile& Profile)
		{
			return FUeAgentStateDetails(
				Profile.CapabilityId,
				TEXT(""),
				{
					TEXT("EditorProject"),
					TEXT("SimpleAuthoring"),
					Profile.NativeOperationType,
				},
				true,
				TEXT("available"),
				{});
		}

		class FEditorProjectTargetResolver final
			: public IUeAgentSimpleAuthoringTargetResolver
		{
		public:
			FEditorProjectTargetResolver(
				TSharedRef<
					IUeAgentEditorProjectSimpleEffectTargetResolver,
					ESPMode::ThreadSafe> InTargetResolver,
				TSharedRef<
					IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver,
					ESPMode::ThreadSafe> InDurableBindingResolver)
				: TargetResolver(MoveTemp(InTargetResolver))
				, DurableBindingResolver(MoveTemp(InDurableBindingResolver))
			{
			}

			bool TryResolveTarget(
				const FUeAgentOperationRequest& CanonicalRequest,
				const FUeAgentExecutionContextRef& Context,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				TOptional<FUeAgentCandidateBinding>& OutTarget,
				FUeAgentSimpleAuthoringProductionError& OutError) override
			{
				OutTarget.Reset();
				const FEditorProjectProfile* Profile =
					FindProfile(CanonicalRequest.GetDescriptorIdentity());
				TSharedPtr<FJsonObject> Payload;
				FString TargetRef;
				FString ExpectedRevision;
				if (!Profile)
				{
					OutError = MissingProfile(CanonicalRequest.GetDescriptorIdentity());
					return false;
				}
				if (!Contract::Json::TryParseObject(CanonicalRequest.GetPayload().GetJsonValue(), Payload, OutError))
				{
					if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("command_input");
					return false;
				}
				if (!Payload->TryGetStringField(TEXT("target_ref"), TargetRef) || TargetRef.IsEmpty())
				{
					OutError = JsonFieldFailure(Payload, TEXT("target_ref"), TEXT("nonempty reference string"));
					OutError.SourceKind = TEXT("command_input");
					OutError.Source.JsonPath = TEXT("/target_ref");
					return false;
				}
				if (Cancellation.IsCancelled())
				{
					OutError = EditorProjectFailure(TEXT("editor_project_target_resolution_cancelled"),
						TEXT("The EditorProject target resolution was cancelled."), TEXT("cancellation"));
					OutError.TargetPath = TargetRef;
					return false;
				}
				FUeAgentEditorProjectSimpleEffectResolvedTarget Target;
				FUeAgentEditorProjectSimpleAuthoringDurableBinding Binding;
				FExecutionMutationError Error;
				if (!TargetResolver->TryResolveTarget(TargetRef, Target, Error))
				{
					if (Error.TargetPath.IsEmpty()) Error.TargetPath = TargetRef;
					if (Error.Source.JsonPath.IsEmpty()) Error.Source.JsonPath = TEXT("/target_ref");
					SetProductionError(OutError, MoveTemp(Error), TargetRef);
					return false;
				}
				if (Target.GetProfileId() != Profile->ProfileId)
				{
					Error = ResolvedEditorProjectTargetFailure(Target, TargetRef,
						Profile->ProfileId, TEXT("editor_project_target_revision_mismatch"));
					if (Error.TargetPath.IsEmpty()) Error.TargetPath = TargetRef;
					if (Error.Source.JsonPath.IsEmpty()) Error.Source.JsonPath = TEXT("/target_ref");
					SetProductionError(OutError, MoveTemp(Error), TargetRef);
					return false;
				}
				ExpectedRevision = Target.GetRevision();
				Payload->SetStringField(
					TEXT("expected_revision"),
					ExpectedRevision);
				if (!DurableBindingResolver->TryResolveDurableBinding(
						Target,
						Binding,
						Error)
					|| !Binding.IsValid())
				{
					OutError = Error;
					if (OutError.IsEmpty())
					{
						OutError = EditorProjectFailure(TEXT("editor_project_durable_binding_missing"),
							!Binding.IsValid() ? TEXT("The EditorProject durable binding has an empty location.")
								: TEXT("The EditorProject durable resolver did not confirm the binding."),
							!Binding.IsValid() ? TEXT("durable_location") : TEXT("binding_resolution"));
						OutError.ActualValuePresent = false;
						OutError.ExpectedValuePresent = true;
					}
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					return false;
				}
				const EDurableArtifactKind RequiredKind =
					Profile->ProfileId == TEXT("string_table")
						? EDurableArtifactKind::Package
						: EDurableArtifactKind::ProjectConfig;
				if (Binding.GetArtifactKind() != RequiredKind)
				{
					OutError = EditorProjectFailure(TEXT("editor_project_durable_binding_kind_invalid"),
						TEXT("The EditorProject durable binding has a different artifact kind from the profile requirement."), TEXT("artifact_kind"));
					OutError.TargetPath = TargetRef;
					OutError.ActualValue = MakeShared<FJsonValueString>(DurableKindName(Binding.GetArtifactKind()));
					OutError.ExpectedValue = MakeShared<FJsonValueString>(DurableKindName(RequiredKind));
					return false;
				}
				Payload->SetStringField(
					EvidenceDurableKind,
					RequiredKind == EDurableArtifactKind::Package
						? TEXT("package")
						: TEXT("project_config"));
				Payload->SetStringField(
					EvidenceDurableLocation,
					Binding.GetDurableLocation());
				FString Evidence;
				if (!TrySerializeJson(Payload, Evidence, OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					return false;
				}
				const FUeAgentCanonicalIdentity Identity(
					TEXT("uai.editor_project:") + TargetRef,
					Profile->NativeOperationType,
					Context.GetProjectInstanceId(),
					TargetRef,
					Profile->ProfileId,
					Context.GetContextRevision());
				OutTarget.Emplace(
					TEXT("editor-project:")
						+ CanonicalRequest.GetRequestId(),
					FUeAgentCandidateSummary(
						Identity,
						CanonicalRequest.GetDescriptorIdentity(),
						TEXT("editor_project"),
						TargetRef,
						TEXT("exact EditorProject typed target"),
						TArray<FString>{
							TEXT("EditorProject"),
							TEXT("SimpleAuthoring"),
						}),
					FUeAgentCatalogRevision(
						TEXT("editor-project-exact-target"),
						Context.GetContextRevision(),
						1),
					Binding.GetDurableLocation(),
					PersistedStatePortId,
					TArray<EUeAgentCandidateOperation>{
						EUeAgentCandidateOperation::Update,
					},
					FString(RequestEvidencePrefix) + Evidence);
				if (!OutTarget->IsValid())
				{
					OutError = *OutTarget->GetValidationDiagnostic();
					OutTarget.Reset();
					return false;
				}
				return true;
			}

		private:
			TSharedRef<
				IUeAgentEditorProjectSimpleEffectTargetResolver,
				ESPMode::ThreadSafe> TargetResolver;
			TSharedRef<
				IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver,
				ESPMode::ThreadSafe> DurableBindingResolver;
		};

		class FEditorProjectStateReadPort final
			: public IUeAgentStateReadPort
		{
		public:
			FEditorProjectStateReadPort(
				FString InPortId,
				const EUeAgentStatePlane InRequiredPlane,
				TSharedRef<
					IUeAgentEditorProjectSimpleEffectTargetResolver,
					ESPMode::ThreadSafe> InResolver)
				: PortId(MoveTemp(InPortId))
				, RequiredPlane(InRequiredPlane)
				, Resolver(MoveTemp(InResolver))
			{
			}

			const FString& GetPortId() const override
			{
				return PortId;
			}

			bool TryRead(
				const FUeAgentStateReadRequest& Request,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentDiscoveryError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = FUeAgentDiscoveryError();
				const FEditorProjectProfile* Profile =
					Request.IsValid()
						? FindProfile(
							Request.GetBinding().GetSummary()
								.GetDescriptorIdentity())
						: nullptr;
				if (!Profile)
				{
					OutError = !Request.IsValid() ? *Request.GetBinding().GetValidationDiagnostic()
						: MissingProfile(Request.GetBinding().GetSummary().GetDescriptorIdentity());
					return false;
				}
				if (Request.GetStatePlane() != RequiredPlane)
				{
					OutError = EditorProjectFailure(TEXT("editor_project_state_request_invalid"),
						TEXT("The EditorProject read port does not serve the requested state plane."), TEXT("state_plane"));
					FString Actual = Contract::LexToString(Request.GetStatePlane());
					if (Actual.IsEmpty()) Actual = FString::FromInt(static_cast<int32>(Request.GetStatePlane()));
					OutError.ActualValue = MakeShared<FJsonValueString>(Actual);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Contract::LexToString(RequiredPlane));
					OutError.TargetPath = Request.GetBinding().GetPublicRef();
					return false;
				}
				if (Cancellation.IsCancelled())
				{
					OutError = EditorProjectFailure(TEXT("editor_project_state_request_cancelled"),
						TEXT("The EditorProject state read was cancelled before resolving its target."), TEXT("cancellation"));
					OutError.TargetPath = Request.GetBinding().GetPublicRef();
					return false;
				}
				const FUeAgentCandidateBinding& Binding =
					Request.GetBinding();
				TSharedPtr<FJsonObject> Payload;
				FString TargetRef;
				FString ExpectedRevision;
				FString DurableLocation;
				EDurableArtifactKind DurableKind =
					EDurableArtifactKind::Package;
				if (!TryParseEvidence(Binding, Payload, OutError)
					|| !TryReadEvidence(
						Payload,
						TargetRef,
						ExpectedRevision,
						DurableKind,
						DurableLocation, OutError))
				{
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Binding.GetBindingId();
					return false;
				}
				FUeAgentEditorProjectSimpleEffectResolvedTarget Target;
				FExecutionMutationError ResolveError;
				if (!Resolver->TryResolveTarget(
						TargetRef,
						Target,
						ResolveError)
					|| !Target.IsValid()
					|| Target.GetCanonicalRef() != TargetRef
					|| Target.GetProfileId() != Profile->ProfileId
					|| (RequiredPlane == EUeAgentStatePlane::AuthoringMemory
						&& Target.GetRevision() != ExpectedRevision))
				{
					OutError = ResolveError.IsEmpty()
						? ResolvedEditorProjectTargetFailure(Target, TargetRef, Profile->ProfileId,
							TEXT("editor_project_state_target_mismatch"), ExpectedRevision,
							RequiredPlane == EUeAgentStatePlane::AuthoringMemory)
						: MoveTemp(ResolveError);
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TargetRef;
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					return false;
				}
				FUeAgentCanonicalValuePtr State;
				TArray<Private::FEditorProjectDependencyState> Dependencies;
				if (!Private::TryBuildEditorProjectTargetState(
						*Resolver,
						Target,
						Profile->ProfileId,
						Payload,
						RequiredPlane
							== EUeAgentStatePlane::AuthoringMemory,
						Resources,
						State,
						Dependencies,
						ResolveError))
				{
					OutError = ResolveError;
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TargetRef;
					return false;
				}
				FUeAgentCanonicalValuePtr Empty;
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryObject(
						{},
						Resources,
						Empty,
						StateError))
				{
					OutError = StateError;
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TargetRef;
					return false;
				}
				const FUeAgentStateRevision Revision(
					PortId,
					Binding.GetCatalogRevision().GetGeneration(),
					Binding.GetCatalogRevision().GetSequence());
				TArray<FUeAgentStateEntry> Entries;
				Entries.Emplace(
					Binding.GetSummary().GetIdentity(),
					EditorProjectStateType(),
					State.ToSharedRef(),
					EditorProjectStateDefault(Empty.ToSharedRef()),
					EditorProjectStateDetails(*Profile),
					Revision,
					EUeAgentAuthoredPresence::Present,
					EUeAgentOverrideState::Overridden);
				for (int32 DependencyIndex = 0; DependencyIndex < Dependencies.Num(); ++DependencyIndex)
				{
					const Private::FEditorProjectDependencyState& Dependency = Dependencies[DependencyIndex];
					const TSharedPtr<FJsonObject> DependencyObject =
						MakeShared<FJsonObject>();
					DependencyObject->SetStringField(
						TEXT("target_ref"),
						Dependency.TargetRef);
					DependencyObject->SetStringField(
						TEXT("target_revision"),
						Dependency.TargetRevision);
					DependencyObject->SetStringField(
						TEXT("profile_id"),
						Dependency.ProfileId);
					DependencyObject->SetStringField(
						TEXT("type_id"),
						Dependency.TypeId);
					FUeAgentCanonicalValuePtr DependencyValue;
					if (!Dependency.IsValid())
					{
						const TCHAR* Field = Dependency.TargetRef.IsEmpty() ? TEXT("target_ref")
							: Dependency.TargetRevision.IsEmpty() ? TEXT("target_revision")
							: Dependency.ProfileId.IsEmpty() ? TEXT("profile_id") : TEXT("type_id");
						OutError = JsonValueFailure(FString::Printf(TEXT("dependencies/%d/%s"), DependencyIndex, Field),
							MakeShared<FJsonValueString>(TEXT("")), TEXT("nonempty string"), Dependency.TargetRef.IsEmpty() ? false : Dependency.TargetRevision.IsEmpty());
						OutError.TargetPath = Dependency.TargetRef;
						return false;
					}
					if (!TryCanonicalJson(MakeShared<FJsonValueObject>(DependencyObject), Resources, DependencyValue, OutError,
						FString::Printf(TEXT("dependencies/%d"), DependencyIndex)))
					{
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Dependency.TargetRef;
						return false;
				}
					Entries.Emplace(
						FUeAgentCanonicalIdentity(
							TEXT("uai.editor_project.dependency:")
								+ Dependency.TargetRef,
							TEXT("editor_project.dependency.read"),
							Binding.GetSummary().GetIdentity().GetOwnerId(),
							Dependency.TargetRef,
							Dependency.ProfileId,
							Binding.GetSummary().GetIdentity().GetGeneration()),
						EditorProjectStateType(Dependency.TypeId),
						DependencyValue.ToSharedRef(),
						EditorProjectStateDefault(Empty.ToSharedRef()),
						EditorProjectStateDetails(*Profile),
						Revision,
						EUeAgentAuthoredPresence::Present,
						EUeAgentOverrideState::Overridden);
				}
				FUeAgentCanonicalSnapshotPtr Snapshot;
				if (!FUeAgentCanonicalSnapshot::TryCreate(
						TEXT("snapshot:editor-project:") + PortId
							+ TEXT(":") + Binding.GetBindingId(),
						RequiredPlane,
						Revision,
						FUeAgentTemporalPoint(
							TEXT("editor-project-operation"),
							TEXT("1"),
							TEXT("1"),
							TEXT("1"),
							TEXT("observed")),
						FUeAgentStateProvenance(
							TEXT("independent_readback"),
							PortId,
							TargetRef),
						MoveTemp(Entries),
						Resources,
						Snapshot,
						StateError))
				{
					OutError = StateError;
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TargetRef;
					return false;
				}
				OutSnapshot = MoveTemp(Snapshot);
				return true;
			}

		private:
			FString PortId;
			EUeAgentStatePlane RequiredPlane =
				EUeAgentStatePlane::AuthoringMemory;
			TSharedRef<
				IUeAgentEditorProjectSimpleEffectTargetResolver,
				ESPMode::ThreadSafe> Resolver;
		};

		class FEditorProjectDraftBuilder final
			: public IUeAgentSimpleAuthoringDraftBuilder
		{
		public:
			bool TryBuildDraft(
				const FUeAgentOperationRequest& CanonicalRequest,
				const FUeAgentExecutionContextRef& Context,
				const FUeAgentCanonicalSnapshotRef& PreState,
				FUeAgentResourceTracker& Resources,
				TOptional<FChangeSetDraft>& OutDraft,
				FUeAgentSimpleAuthoringProductionError& OutError) override
			{
				FExecutionMutationError Error;
				if (!TryBuildEditorProjectSimpleEffectChangeSetDraft(
						CanonicalRequest,
						Context,
						PreState,
						Resources,
						OutDraft,
						Error))
				{
					SetProductionError(OutError, Error);
					return false;
				}
				return true;
			}
		};

		class FEditorProjectChangeTargetProjector final
			: public IUeAgentSimpleAuthoringChangeTargetProjector
		{
		public:
			bool TryProjectChangeTargets(
				const FUeAgentOperationRequest& CanonicalRequest,
				const FUeAgentExecutionContextRef& Context,
				const FChangeSetDraft& Draft,
				const FUeAgentCandidateBinding& ResolvedTarget,
				const FUeAgentCanonicalSnapshotRef& PreState,
				FUeAgentResourceTracker& Resources,
				TArray<FUeAgentCandidateBinding>& OutTargets,
				FUeAgentSimpleAuthoringProductionError& OutError) override
			{
				OutTargets.Reset();
				const TArray<FChangeDraft>& Changes = Draft.GetChanges();
				for (int32 Index = 0; Index < Changes.Num(); ++Index)
				{
					const FChangeDraft& Change = Changes[Index];
					OutTargets.Emplace(
						ResolvedTarget.GetBindingId() + TEXT(":change:")
							+ FString::FromInt(Index),
						FUeAgentCandidateSummary(
							Change.GetIdentity(),
							CanonicalRequest.GetDescriptorIdentity(),
							TEXT("editor_project"),
							Change.GetIdentity().GetSelector(),
							TEXT("exact EditorProject change target"),
							TArray<FString>{
								TEXT("EditorProject"),
								TEXT("SimpleAuthoring"),
							}),
						ResolvedTarget.GetCatalogRevision(),
						ResolvedTarget.GetPublicRef(),
						ResolvedTarget.GetReadPortId(),
						TArray<EUeAgentCandidateOperation>{
							EUeAgentCandidateOperation::Update,
						},
						ResolvedTarget.GetBindingEvidence());
					if (!OutTargets.Last().IsValid())
					{
						OutError = *OutTargets.Last().GetValidationDiagnostic();
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = OutTargets.Last().GetPublicRef();
						OutTargets.Reset();
						return false;
					}
				}
				if (OutTargets.IsEmpty())
				{
					OutError = CountFailure(TEXT("editor_project_change_target_invalid"),
						TEXT("At least one EditorProject change target is required."), TEXT("change_targets"), 0, 1);
					return false;
				}
				return true;
			}
		};

		class FEditorProjectMutationBindingProjector final
			: public IUeAgentSimpleAuthoringMutationBindingProjector
		{
		public:
			bool TryProjectMutationBindings(
				const FPlanningPreflightPlanRef& Plan,
				const TArray<FUeAgentCandidateBinding>& Targets,
				const FUeAgentCanonicalSnapshotRef& PreState,
				const FUeAgentExecutionContextRef& Context,
				FUeAgentResourceTracker& Resources,
				TArray<FUeAgentGenericEffectBinding>& OutBindings,
				FUeAgentSimpleAuthoringProductionError& OutError) override
			{
				OutBindings.Reset();
				const TArray<FPlannedChange>& Changes =
					Plan->GetChangeSet().GetChanges();
				if (Changes.Num() != Targets.Num())
				{
					OutError = CountFailure(TEXT("editor_project_change_targets_missing"),
						TEXT("The number of EditorProject targets differs from the planned changes."), TEXT("change_targets"), Targets.Num(), Changes.Num());
					OutError.SubjectId = Plan->GetRequestId();
					return false;
				}
				for (int32 Index = 0; Index < Changes.Num(); ++Index)
				{
					OutBindings.Emplace(
						Targets[Index],
						TEXT("UAI.EditorProject:CanonicalTypedUpdate"),
						FUeAgentCanonicalType(
							TEXT("editor_project_typed_update"),
							EUeAgentValueKind::Object,
							false,
							TEXT("object")),
						TEXT("editor-project:typed-native-binding"));
					if (Targets[Index].GetSummary().GetIdentity() != Changes[Index].GetIdentity())
					{
						OutError = EditorProjectFailure(TEXT("editor_project_mutation_binding_invalid"),
							TEXT("The EditorProject mutation target identity differs from its planned change."),
							FString::Printf(TEXT("targets/%d/identity"), Index));
						OutError.TargetPath = Targets[Index].GetPublicRef();
						OutError.ActualEvidence = MakeShared<FJsonValueString>(Targets[Index].GetSummary().GetIdentity().GetCanonicalKey());
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Changes[Index].GetIdentity().GetCanonicalKey());
						OutBindings.Reset();
						return false;
					}
					if (!OutBindings.Last().IsValid())
					{
						OutError = OutBindings.Last().GetValidationDiagnostic();
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Targets[Index].GetPublicRef();
						OutBindings.Reset();
						return false;
					}
				}
				if (OutBindings.IsEmpty())
				{
					OutError = CountFailure(TEXT("editor_project_mutation_binding_invalid"),
						TEXT("At least one EditorProject mutation binding is required."), TEXT("mutation_bindings"), 0, 1);
					return false;
				}
				return true;
			}
		};

		class FEditorProjectDurableTargetProjector final
			: public IUeAgentSimpleAuthoringDurableTargetProjector
		{
		public:
			bool TryProjectDurableTargets(
				const FPlanningPreflightPlanRef& Plan,
				const FUeAgentCandidateBinding& Target,
				const FUeAgentCanonicalSnapshotRef& PreState,
				FUeAgentResourceTracker& Resources,
				TArray<FAssetDurableTarget>& OutTargets,
				FUeAgentSimpleAuthoringProductionError& OutError) override
			{
				OutTargets.Reset();
				TSharedPtr<FJsonObject> Payload;
				FString TargetRef;
				FString ExpectedRevision;
				FString DurableLocation;
				EDurableArtifactKind Kind = EDurableArtifactKind::Package;
				const TArray<FPlannedChange>& Changes =
					Plan->GetChangeSet().GetChanges();
				if (Changes.Num() != 1)
				{
					OutError = CountFailure(TEXT("editor_project_durable_target_missing"),
						TEXT("EditorProject durable projection requires exactly one planned change."), TEXT("changes"), Changes.Num(), 1);
					return false;
				}
				if (!TryParseEvidence(Target, Payload, OutError)
					|| !TryReadEvidence(Payload, TargetRef, ExpectedRevision, Kind, DurableLocation, OutError))
				{
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Target.GetBindingId();
					return false;
				}
				if (Kind == EDurableArtifactKind::Package)
				{
					OutTargets.Emplace(
						Changes[0].GetIdentity(),
						DurableLocation,
						EPackageHeaderKind::Asset,
						EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup);
				}
				else
				{
					OutTargets.Emplace(
						Changes[0].GetIdentity(),
						Kind,
						DurableLocation,
						EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup);
				}
				if (!OutTargets[0].IsValid())
				{
					OutError = OutTargets[0].GetValidationDiagnostic();
					return false;
				}
				return true;
			}
		};

		class FEditorProjectExpectedStateProjector final
			: public IUeAgentSimpleAuthoringExpectedStateProjector
		{
		public:
			bool TryProjectExpectedState(
				const FPlanningPreflightPlanRef& Plan,
				const FUeAgentCandidateBinding& ResolvedTarget,
				const TArray<FUeAgentCandidateBinding>& ChangeTargets,
				const FUeAgentCanonicalSnapshotRef& PreState,
				const FDurableInventory& Inventory,
				FUeAgentResourceTracker& Resources,
				TArray<FVerificationTarget>& OutTargets,
				FUeAgentSimpleAuthoringProductionError& OutError) override
			{
				OutTargets.Reset();
				const FEditorProjectProfile* Profile =
					FindProfile(Plan->GetDescriptorIdentity());
				const TArray<FPlannedChange>& Changes =
					Plan->GetChangeSet().GetChanges();
				if (!Profile)
				{
					OutError = MissingProfile(Plan->GetDescriptorIdentity());
					return false;
				}
				if (Changes.Num() != 1)
				{
					OutError = CountFailure(TEXT("editor_project_expected_change_invalid"),
						TEXT("EditorProject expected-state projection requires exactly one planned change."), TEXT("changes"), Changes.Num(), 1);
					return false;
				}
				if (!Changes[0].GetBefore().IsValid() || !Changes[0].GetAfter().IsValid())
				{
					const bool bBefore = !Changes[0].GetBefore().IsValid();
					OutError = EditorProjectFailure(TEXT("editor_project_expected_change_invalid"),
						bBefore ? TEXT("The planned EditorProject change has no before-state.") : TEXT("The planned EditorProject change has no after-state."),
						bBefore ? TEXT("changes/0/before") : TEXT("changes/0/after"));
					OutError.ActualValuePresent = false;
					OutError.ExpectedValuePresent = true;
					OutError.TargetPath = ResolvedTarget.GetSummary().GetIdentity().GetSelector();
					return false;
				}
				const FUeAgentCandidateBinding* ChangeTarget =
					ChangeTargets.FindByPredicate(
						[&Changes](const FUeAgentCandidateBinding& Candidate)
						{
							return Candidate.GetSummary().GetIdentity()
								== Changes[0].GetIdentity();
						});
				FUeAgentCanonicalValuePtr ExpectedValue;
				if (!ChangeTarget)
				{
					OutError = EditorProjectFailure(TEXT("editor_project_verification_target_missing"),
						TEXT("No resolved EditorProject target matches the planned change to verify."), TEXT("change_targets"));
					OutError.TargetPath = ResolvedTarget.GetSummary().GetIdentity().GetSelector();
					OutError.ActualValuePresent = false;
					OutError.ExpectedValuePresent = true;
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Changes[0].GetIdentity().GetCanonicalKey());
					return false;
				}
				if (!Private::TryProjectEditorProjectExpectedState(*Changes[0].GetBefore(), *Changes[0].GetAfter(),
					Resources, ExpectedValue, OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Changes[0].GetIdentity().GetSelector();
					return false;
				}
				FUeAgentCanonicalValuePtr Empty;
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryObject(
						{},
						Resources,
						Empty,
						StateError))
				{
					SetProductionError(OutError, StateError);
					return false;
				}
				const FUeAgentStateRevision Revision(
					TEXT("editor-project-operation-expected"),
					Plan->GetFingerprint(),
					1);
				TArray<FUeAgentStateEntry> Entries;
				Entries.Emplace(
					Changes[0].GetIdentity(),
					EditorProjectStateType(),
					ExpectedValue.ToSharedRef(),
					EditorProjectStateDefault(Empty.ToSharedRef()),
					EditorProjectStateDetails(*Profile),
					Revision,
					EUeAgentAuthoredPresence::Present,
					EUeAgentOverrideState::Overridden);
				FUeAgentCanonicalSnapshotPtr Expected;
				if (!FUeAgentCanonicalSnapshot::TryCreateExpected(
						TEXT("snapshot:editor-project:expected:")
							+ Plan->GetRequestId(),
						EUeAgentStatePlane::Persisted,
						Revision,
						FUeAgentTemporalPoint(
							TEXT("editor-project-operation"),
							TEXT("1"),
							TEXT("1"),
							TEXT("1"),
							TEXT("expected")),
						FUeAgentStateProvenance(
							TEXT("operation_expected_state"),
							Plan->GetRequestId(),
							Plan->GetFingerprint()),
						MoveTemp(Entries),
						Resources,
						Expected,
						StateError))
				{
					SetProductionError(OutError, StateError);
					return false;
				}
				TArray<FString> ArtifactIds;
				for (const FDurableArtifact& Artifact : Inventory.GetArtifacts())
				{
					ArtifactIds.Add(Artifact.GetArtifactId());
				}
				OutTargets.Emplace(
					Changes[0].GetIdentity(),
					*ChangeTarget,
					EVerificationOracleKind::Persisted,
					Expected,
					ArtifactIds,
					TArray<FString>());
				if (!OutTargets.Last().IsValid())
				{
					OutError = *OutTargets.Last().GetValidationDiagnostic();
					return false;
				}
				return true;
			}
		};

		class FEditorProjectUeHook final
			: public IUeAgentGenericEffectUeHook
		{
		public:
			explicit FEditorProjectUeHook(
				TSharedRef<
					IUeAgentEditorProjectSimpleEffectNativeHook,
					ESPMode::ThreadSafe> InNativeHook)
				: NativeHook(MoveTemp(InNativeHook))
			{
			}

			bool TryApply(
				const FExecutionMutationRequest& Request,
				const FLockFenceLease& Lease,
				const TArray<FUeAgentGenericEffectBinding>& Bindings,
				FUeAgentResourceTracker& Resources,
				FUeAgentGenericEffectApplyResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				return NativeHook->TryApply(
					Request,
					Lease,
					Bindings,
					Resources,
					OutResult,
					OutError);
			}

			bool TryRollback(
				const FExecutionMutationRequest& Request,
				const FUeAgentGenericEffectApplyResult& Applied,
				FUeAgentResourceTracker& Resources,
				bool& bOutConfirmed,
				FExecutionMutationError& OutError) override
			{
				return NativeHook->TryRollback(
					Request,
					Applied,
					Resources,
					bOutConfirmed,
					OutError);
			}

			void ReleaseVerifiedOperation(
				const FString& OperationId) override
			{
				NativeHook->ReleaseVerifiedOperation(OperationId);
			}

			bool TryWaitForSettle(
				const FBarrierRequest& Request,
				FUeAgentResourceTracker& Resources,
				FBarrierResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				return NativeHook->TryWaitForSettle(
					Request,
					Resources,
					OutResult,
					OutError);
			}

		private:
			TSharedRef<
				IUeAgentEditorProjectSimpleEffectNativeHook,
				ESPMode::ThreadSafe> NativeHook;
		};

		class FEditorProjectConfigStageSource final
			: public IProjectConfigDurableStageSource
		{
		public:
			FEditorProjectConfigStageSource(
				TSharedRef<
					IUeAgentEditorProjectSimpleEffectTargetResolver,
					ESPMode::ThreadSafe> InTargetResolver,
				TSharedRef<
					IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver,
					ESPMode::ThreadSafe> InDurableBindingResolver)
				: TargetResolver(MoveTemp(InTargetResolver))
				, DurableBindingResolver(MoveTemp(InDurableBindingResolver))
			{
			}

			bool TryWriteStagedArtifact(
				const FAssetDurableTarget& Target,
				const FString& StagedPath,
				FUeAgentResourceTracker& Resources,
				FAssetDurableError& OutError) override
			{
				OutError = FAssetDurableError();
				if (!Target.IsValid())
				{
					OutError = Target.GetValidationDiagnostic();
					return false;
				}
				if (Target.GetArtifactKind() != EDurableArtifactKind::ProjectConfig)
				{
					OutError = EditorProjectFailure(TEXT("editor_project_config_stage_target_kind_invalid"),
						TEXT("The EditorProject config stage requires a project_config artifact."), TEXT("artifact_kind"));
					OutError.TargetPath = Target.GetIdentity().GetSelector();
					OutError.ActualValue = MakeShared<FJsonValueString>(DurableKindName(Target.GetArtifactKind()));
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("project_config"));
					return false;
				}
				FUeAgentEditorProjectSimpleEffectResolvedTarget Resolved;
				FUeAgentEditorProjectSimpleAuthoringDurableBinding Binding;
				FExecutionMutationError Error;
				if (!TargetResolver->TryResolveTarget(
						Target.GetIdentity().GetSelector(),
						Resolved,
						Error)
					|| !Resolved.IsValid()
					|| !DurableBindingResolver->TryResolveDurableBinding(
						Resolved,
						Binding,
						Error)
					|| !Binding.IsValid())
				{
					if (Error.IsEmpty() && !Resolved.IsValid()) Error = Resolved.GetValidationDiagnostic();
					if (Error.IsEmpty())
					{
						Error = FDiagnostic::Failure(TEXT("editor_project_config_stage_target_unresolved"),
							!Resolved.IsValid() ? TEXT("The configuration stage resolver did not return a usable target.")
							: !Binding.IsValid() ? TEXT("The configuration stage binding has no durable location.")
							: TEXT("The configuration stage binding resolver did not confirm the binding."), TEXT("asset_durable"));
						Error.SourceKind = TEXT("native_result");
						Error.PropertyPath = !Resolved.IsValid() ? TEXT("/target_resolved")
							: !Binding.IsValid() ? TEXT("/durable_location_present") : TEXT("/binding_resolution_confirmed");
						Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
						Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
						Error.ActualValuePresent = true;
						Error.ExpectedValuePresent = true;
					}
					OutError = MoveTemp(Error);
					return false;
				}
				if (Binding.GetArtifactKind()
						!= EDurableArtifactKind::ProjectConfig
					|| Binding.GetDurableLocation()
						!= Target.GetDurableLocation())
				{
					const bool bKind = Binding.GetArtifactKind() != EDurableArtifactKind::ProjectConfig;
					OutError = EditorProjectFailure(TEXT("editor_project_config_stage_binding_mismatch"),
						bKind ? TEXT("The resolved config stage binding has a different artifact kind.")
							: TEXT("The resolved config stage binding points to a different durable location."),
						bKind ? TEXT("artifact_kind") : TEXT("durable_location"));
					OutError.TargetPath = Target.GetIdentity().GetSelector();
					if (bKind)
					{
						OutError.ActualValue = MakeShared<FJsonValueString>(DurableKindName(Binding.GetArtifactKind()));
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("project_config"));
					}
					else
					{
						OutError.ActualEvidence = MakeShared<FJsonValueString>(Binding.GetDurableLocation());
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Target.GetDurableLocation());
					}
					return false;
				}
				if (!Private::TryWriteEditorProjectStagedConfig(
						Resolved,
						StagedPath,
						Error))
				{
					OutError = Error;
					return false;
				}
				return true;
			}

		private:
			TSharedRef<
				IUeAgentEditorProjectSimpleEffectTargetResolver,
				ESPMode::ThreadSafe> TargetResolver;
			TSharedRef<
				IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver,
				ESPMode::ThreadSafe> DurableBindingResolver;
		};

		bool ValidateConfig(
			const FUeAgentEditorProjectSimpleAuthoringProviderConfig& Config,
			FUeAgentEditorProjectSimpleAuthoringError& OutError)
		{
			const TCHAR* MissingResolver = !Config.DescriptorRegistry.IsValid() ? TEXT("descriptor_registry")
				: !Config.MutationTargetResolver.IsValid() ? TEXT("mutation_target_resolver")
				: !Config.PreStateTargetResolver.IsValid() ? TEXT("pre_state_target_resolver")
				: !Config.PersistedTargetResolver.IsValid() ? TEXT("persisted_target_resolver") : nullptr;
			if (MissingResolver)
			{
				OutError = EditorProjectFailure(TEXT("editor_project_simple_authoring_dependencies_missing"),
					FString::Printf(TEXT("The EditorProject provider has no '%s'."), MissingResolver), MissingResolver);
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			if (Config.PreStateTargetResolver == Config.PersistedTargetResolver)
			{
				OutError = EditorProjectFailure(TEXT("editor_project_simple_authoring_dependencies_missing"),
					TEXT("The EditorProject pre-state and persisted-state resolvers must be separate instances."), TEXT("persisted_target_resolver"));
				OutError.ActualValue = MakeShared<FJsonValueString>(TEXT("same instance as pre_state_target_resolver"));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("independent resolver instance"));
				return false;
			}
			const TCHAR* Missing = !Config.DurableBindingResolver.IsValid() ? TEXT("durable_binding_resolver")
				: !Config.DurableService.IsValid() ? TEXT("durable_service")
				: !Config.AbsenceOracle.IsValid() ? TEXT("absence_oracle")
				: !Config.ReferenceOracle.IsValid() ? TEXT("reference_oracle")
				: !Config.Projection.IsSet() ? TEXT("projection") : nullptr;
			if (Missing)
			{
				OutError = EditorProjectFailure(TEXT("editor_project_simple_authoring_dependencies_missing"),
					FString::Printf(TEXT("The EditorProject provider has no '%s'."), Missing), Missing);
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			if (!Config.Projection->IsValid())
			{
				OutError = Config.Projection->GetValidationDiagnostic();
				return false;
			}
			if (Config.OwnerLeaseId.IsEmpty())
			{
				OutError = EditorProjectFailure(TEXT("editor_project_simple_authoring_dependencies_missing"),
					TEXT("The EditorProject provider has an empty owner lease identifier."), TEXT("owner_lease_id"));
				OutError.ActualValue = MakeShared<FJsonValueString>(TEXT(""));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty identifier"));
				return false;
			}
			const FEditorProjectProfileCatalog& Catalog =
				GetProfileCatalog();
			if (!Catalog.Error.IsEmpty())
			{
				OutError = Catalog.Error;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TEXT("editor_project");
				return false;
			}
			for (const FEditorProjectProfile& Profile : Catalog.Profiles)
			{
				const FUeAgentOperationDescriptor* Descriptor =
					Config.DescriptorRegistry->FindByDescriptorId(
						Profile.DescriptorId);
				if (!Descriptor)
				{
					OutError = EditorProjectFailure(TEXT("editor_project_simple_authoring_descriptor_missing"),
						TEXT("The required EditorProject operation descriptor is not registered."), TEXT("descriptor_id"));
					OutError.CandidateName = Profile.DescriptorId;
					OutError.ActualValuePresent = false;
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Profile.DescriptorId);
					return false;
				}
				if (Descriptor->GetCapabilityId() != Profile.CapabilityId || Descriptor->GetDomainId() != TEXT("editor_project"))
				{
					const bool bCapabilityMismatch = Descriptor->GetCapabilityId() != Profile.CapabilityId;
					OutError = EditorProjectFailure(TEXT("editor_project_simple_authoring_descriptor_missing"),
						TEXT("The registered EditorProject descriptor does not match its profile."),
						bCapabilityMismatch ? TEXT("capability_id") : TEXT("domain_id"));
					OutError.CandidateName = Profile.DescriptorId;
					OutError.ActualValue = MakeShared<FJsonValueString>(bCapabilityMismatch ? Descriptor->GetCapabilityId() : Descriptor->GetDomainId());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(bCapabilityMismatch ? Profile.CapabilityId : TEXT("editor_project"));
					return false;
				}
				if (Descriptor->GetIdentity().GetVersion() != Profile.DescriptorVersion)
				{
					OutError = EditorProjectFailure(TEXT("editor_project_simple_authoring_descriptor_missing"),
						TEXT("The registered EditorProject descriptor version differs from its profile."), TEXT("descriptor_version"));
					OutError.CandidateName = Profile.DescriptorId;
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Descriptor->GetIdentity().GetVersion());
					OutError.ExpectedValue = MakeShared<FJsonValueNumber>(Profile.DescriptorVersion);
					return false;
				}
				if (Descriptor->GetIdentity().GetDescriptorHash() != Profile.DescriptorHash)
				{
					OutError = EditorProjectFailure(TEXT("editor_project_simple_authoring_descriptor_missing"),
						TEXT("The registered EditorProject descriptor hash differs from its profile."), TEXT("descriptor_hash"));
					OutError.CandidateName = Profile.DescriptorId;
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Descriptor->GetIdentity().GetDescriptorHash());
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Profile.DescriptorHash);
					return false;
				}
			}
			return true;
		}
	}

	FUeAgentEditorProjectSimpleAuthoringDurableBinding::
		FUeAgentEditorProjectSimpleAuthoringDurableBinding(
			const EDurableArtifactKind InArtifactKind,
			FString InDurableLocation)
		: ArtifactKind(InArtifactKind)
		, DurableLocation(MoveTemp(InDurableLocation))
		, bValid(!DurableLocation.IsEmpty())
	{
	}

	EDurableArtifactKind
	FUeAgentEditorProjectSimpleAuthoringDurableBinding::
		GetArtifactKind() const
	{
		return ArtifactKind;
	}

	const FString&
	FUeAgentEditorProjectSimpleAuthoringDurableBinding::
		GetDurableLocation() const
	{
		return DurableLocation;
	}

	bool FUeAgentEditorProjectSimpleAuthoringDurableBinding::IsValid() const
	{
		return bValid;
	}

	bool FUeAgentEditorProjectConfigStageSourceFactory::TryCreate(
		TSharedPtr<
			IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe> TargetResolver,
		TSharedPtr<
			IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver,
			ESPMode::ThreadSafe> DurableBindingResolver,
		TSharedPtr<IProjectConfigDurableStageSource>& OutStageSource,
		FUeAgentEditorProjectSimpleAuthoringError& OutError)
	{
		OutStageSource.Reset();
		OutError = FUeAgentEditorProjectSimpleAuthoringError();
		if (!TargetResolver.IsValid() || !DurableBindingResolver.IsValid())
		{
			const TCHAR* Missing = !TargetResolver.IsValid() ? TEXT("target_resolver") : TEXT("durable_binding_resolver");
			OutError = EditorProjectFailure(TEXT("editor_project_config_stage_dependencies_missing"),
				FString::Printf(TEXT("The configuration stage source has no '%s'."), Missing), Missing);
			OutError.ActualValuePresent = false;
			OutError.ExpectedValuePresent = true;
			return false;
		}
		OutStageSource = MakeShared<FEditorProjectConfigStageSource>(
			TargetResolver.ToSharedRef(),
			DurableBindingResolver.ToSharedRef());
		return true;
	}

	bool FUeAgentEditorProjectSimpleAuthoringProviderFactory::TryCreate(
		const FUeAgentEditorProjectSimpleAuthoringProviderConfig& Config,
		TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider>& OutProvider,
		FUeAgentEditorProjectSimpleAuthoringError& OutError)
	{
		OutProvider.Reset();
		OutError = FUeAgentEditorProjectSimpleAuthoringError();
		if (!ValidateConfig(Config, OutError))
		{
			return false;
		}
		const TSharedRef<
			IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe> MutationResolver =
				Config.MutationTargetResolver.ToSharedRef();
		const TSharedRef<
			IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe> PreStateResolver =
				Config.PreStateTargetResolver.ToSharedRef();
		const TSharedRef<
			IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe> PersistedResolver =
				Config.PersistedTargetResolver.ToSharedRef();
		const TSharedRef<
			IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver,
			ESPMode::ThreadSafe> DurableBindingResolver =
				Config.DurableBindingResolver.ToSharedRef();
		TSharedPtr<IUeAgentEditorProjectSimpleEffectNativeHook> NativeHook;
		FExecutionMutationError NativeError;
		if (!FUeAgentEditorProjectSimpleEffectNativeHookFactory::TryCreate(
				MutationResolver,
				NativeHook,
				NativeError))
		{
			OutError = NativeError;
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TEXT("editor_project");
			return false;
		}

		FUeAgentSimpleAuthoringProductionProviderConfig ProviderConfig;
		ProviderConfig.FamilyId = TEXT("editor_project");
		ProviderConfig.OwnerLeaseId = Config.OwnerLeaseId;
		ProviderConfig.PreStatePlane = EUeAgentStatePlane::AuthoringMemory;
		ProviderConfig.AtomicityMode = EAtomicityMode::CompensatedSaga;
		ProviderConfig.Projection = Config.Projection;
		ProviderConfig.TargetResolver =
			MakeShared<FEditorProjectTargetResolver>(
				PreStateResolver,
				DurableBindingResolver);
		ProviderConfig.PreStateReadPort =
			MakeShared<FEditorProjectStateReadPort>(
				PreStatePortId,
				EUeAgentStatePlane::AuthoringMemory,
				PreStateResolver);
		ProviderConfig.ChangeTargetProjector =
			MakeShared<FEditorProjectChangeTargetProjector>();
		ProviderConfig.DraftBuilder =
			MakeShared<FEditorProjectDraftBuilder>();
		ProviderConfig.MutationBindingProjector =
			MakeShared<FEditorProjectMutationBindingProjector>();
		ProviderConfig.DurableTargetProjector =
			MakeShared<FEditorProjectDurableTargetProjector>();
		ProviderConfig.DurableService = Config.DurableService;
		ProviderConfig.UeHook = MakeShared<FEditorProjectUeHook>(
			NativeHook.ToSharedRef());
		ProviderConfig.ExpectedStateProjector =
			MakeShared<FEditorProjectExpectedStateProjector>();
		const TSharedRef<IUeAgentStateReadPort, ESPMode::ThreadSafe>
			PersistedStateReadPort =
				MakeShared<FEditorProjectStateReadPort>(
				PersistedStatePortId,
				EUeAgentStatePlane::Persisted,
				PersistedResolver);
		ProviderConfig.PersistedStateOracle = MakeShared<
			FStateReadPortPersistedStateOracle,
			ESPMode::ThreadSafe>(PersistedStateReadPort);
		ProviderConfig.AbsenceOracle = Config.AbsenceOracle;
		ProviderConfig.ReferenceOracle = Config.ReferenceOracle;

		return FUeAgentSimpleAuthoringProductionProvider::TryCreate(
			ProviderConfig, OutProvider, OutError);
	}
}
