#include "DomainAdapters/WorldCommands/UeAgentWorldHlodHjsonMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Composition/HjsonImport/UeAgentReflectionRootPropertyImport.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringAssets.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringExport.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringState.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"

namespace UE::UAI::DomainAdapters::WorldCommands
{
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;
	using namespace DomainAdapters::ContentGraphRead;
	using namespace CommandContract;

	namespace
	{
		FString PackageIdentity(const FString& Target)
		{
			return Target.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Target) : Target;
		}

		bool Fail(FWorkspaceError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic,
			const FString& Target = FString(), const FString& Pointer = TEXT("/"))
        {
            OutError.SetFailure(MoveTemp(Diagnostic), Target, Pointer);
            return false;
        }

		const UeAgentReflectionAuthoring::FProfile* ResolveProfile(
			const FString& Target, const FString& RootType, FWorkspaceError& OutError)
		{
			UE::UAI::Contract::FDiagnostic Error;
			if (!UeAgentReflectionAuthoring::RegisterAuthoring(Error))
			{
				Fail(OutError, Error, Target);
				return nullptr;
			}
			const auto* Profile =
				UeAgentReflectionAuthoring::FindProfileByTypeId(
					RootType == TEXT("/Script/Engine.DataLayerAsset") ? TEXT("data_layer") : TEXT("hlod_layer"));
			if (!Profile)
			{
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_hlod_reflection_profile_missing"),
					TEXT("No reflection authoring profile is registered for HLODLayer."), TEXT("authoring_resolve"));
				Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
				Diagnostic.PropertyPath = TEXT("reflection_profile_registered");
				Diagnostic.SourceKind = TEXT("native_result");
				Fail(OutError, MoveTemp(Diagnostic), Target);
			}
			return Profile;
		}

		bool Read(const FString& Target, const FString& RootType, FString& OutPayload,
			FWorkspaceError& OutError)
		{
			UObject* Object = nullptr;
			FUeAgentContentGraphCapture Capture;
			if (!TryCaptureObject(Target, RootType, Object, Capture, OutError))
				return false;
			TSharedPtr<FJsonObject> Snapshot;
			if (!TryBuildCaptureSnapshot(Target, RootType, Capture, Snapshot, OutError))
				return false;
			return TryCanonicalizeObject(Snapshot, OutPayload, OutError);
		}

		bool ReadRevision(const FString& Target, const FString& RootType, FString& Payload,
			FString& Revision, FWorkspaceError& Error)
		{
			if (!Read(Target, RootType, Payload, Error)) return false;
			UObject* Owner = nullptr;
			FUeAgentContentGraphCapture Capture;
			if (!TryCaptureObject(Target, RootType, Owner, Capture, Error)) return false;
			Revision = Capture.Revision;
			return true;
		}

		bool Create(const UeAgentReflectionAuthoring::FProfile& Profile,
			const FString& Target, const FString& RootType, const FResolvedDomainHjsonDocument& Document,
			FString& Payload, FWorkspaceError& Error)
		{
			// The domain selects its root type; the existing Reflection input,
			// native factory, preview, transaction and absent checkpoint own creation.
			const TSharedPtr<FJsonObject>* Input = nullptr;
			if (!Document.Root.IsValid()
				|| !Document.Root->TryGetObjectField(TEXT("document"), Input)
				|| !Input || !Input->IsValid())
			{
				const auto Actual = Document.Root.IsValid() ? Document.Root->TryGetField(TEXT("document")) : nullptr;
				auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("world_hlod_create_document_required"),
					TEXT("/root/document"), MakeShared<FJsonValueString>(TEXT("object")),
					Actual.IsValid() ? Actual : MakeShared<FJsonValueNull>(), TEXT("input_decode"),
					TEXT("Single-file HLOD creation requires an object-valued root.document."));
				Diagnostic.ActualValuePresent = Actual.IsValid();
				return Fail(Error, MoveTemp(Diagnostic), Target);
			}
			if (Document.ExpectedRevision != TEXT("missing"))
			{
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_hlod_expected_missing_required"),
					TEXT("Single-file HLOD creation requires expected_revision to be 'missing'."), TEXT("input_preflight"));
				Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
				Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Document.ExpectedRevision);
				Diagnostic.PropertyPath = TEXT("expects_absent_target");
				Diagnostic.Source.JsonPath = TEXT("/expected_revision");
				Diagnostic.SourceKind = TEXT("command_input");
				return Fail(Error, MoveTemp(Diagnostic), Target);
			}
			auto Source = UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
				*Input, TEXT("inline://world-hlod-create"));
			Source.ResolveSource = [Document](const FString&, const FString& Pointer)
			{
				return Document.ResolveSource(TEXT("/root/document") + (Pointer == TEXT("/") ? FString() : Pointer));
			};
			UeAgentReflectionAuthoring::FWorkspaceInspectResult Inspection;
			if (!UeAgentReflectionAuthoring::InspectAssetSource(Profile, Source,
				PackageIdentity(Target), Inspection))
				return Fail(Error, Inspection.Diagnostic, Target);
			if (Inspection.RequestedOperation != TEXT("create") || Inspection.bAssetExists)
			{
				if (Inspection.RequestedOperation != TEXT("create")) {
					const auto Actual = (*Input)->TryGetField(TEXT("operation"));
					auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("world_hlod_absent_create_required"),
						TEXT("/root/document/operation"), MakeShared<FJsonValueString>(TEXT("create")),
						Actual.IsValid() ? Actual : MakeShared<FJsonValueNull>(), TEXT("input_preflight"),
						TEXT("Single-file HLOD import requires document.operation to be create."));
					Diagnostic.ActualValuePresent = Actual.IsValid();
					return Fail(Error, MoveTemp(Diagnostic), Target);
				}
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_hlod_absent_create_required"),
					TEXT("The HLOD creation target already exists; creation requires an absent asset."), TEXT("input_preflight"));
				Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
				Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
				Diagnostic.PropertyPath = TEXT("target_present");
				Diagnostic.SourceKind = TEXT("native_result");
				return Fail(Error, MoveTemp(Diagnostic), Target);
			}
			bool bReadbackCompleted = false;
			auto FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
			{
				FWorkspaceError ReadError;
				bReadbackCompleted = TryReadbackImportPayload(
					TEXT("world-hlod-domain-provider"), Target, ObjectPathFromPackageIdentity(Target),
					nullptr, TEXT("world_hlod_create_readback_invalid"),
					TEXT("world_hlod_compile_not_settled"),
					[&Target, &RootType](FString& ReadPayload, FString& Revision, FWorkspaceError& ReadFailure)
					{ return ReadRevision(Target, RootType, ReadPayload, Revision, ReadFailure); },
					Payload, ReadError);
				if (!bReadbackCompleted) {
					Failure.Diagnostic = ReadError.GetDiagnostic();
				}
				return bReadbackCompleted;
			};
			UeAgentReflectionAuthoring::FAssetApplyResult Result;
			if (!UeAgentReflectionAuthoring::ApplyAssetSource(Profile, Source,
				PackageIdentity(Target), Result, true,
				TEXT("world-hlod-create-") + FGuid::NewGuid().ToString(EGuidFormats::Digits),
				true, FinalReadback, nullptr, true))
				return Fail(Error, Result.Diagnostic, Target);
			if (!bReadbackCompleted) {
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("domain_hjson_atomic_readback_not_executed"),
					TEXT("HLOD apply returned success without completing its required final readback."), TEXT("readback"));
				Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
				Diagnostic.PropertyPath = TEXT("final_readback_completed");
				Diagnostic.SourceKind = TEXT("native_result");
				Diagnostic.MutationMayHaveStarted = true;
				return Fail(Error, MoveTemp(Diagnostic), Target);
			}
			return true;
		}
	}

	bool TryExecuteHlodContentGraph(const FString& CommandId,
		const FString& TargetIdentity, const FString& CanonicalParamsJson,
		FString& OutPayloadJson, bool& bOutReadOnly, FWorkspaceError& OutError, const FString& RootType,
		FPreparedWorkspaceExport* PreparedExport)
	{
		OutPayloadJson.Reset();
		bOutReadOnly = false;
		OutError.Reset();
		TSharedPtr<FJsonObject> Params;
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			if (JsonError.IsValid()) return Fail(OutError, MoveTemp(JsonError), TargetIdentity);
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_hlod_params_invalid"),
				TEXT("The HLOD command parameters did not decode to a JSON object."), TEXT("input_decode"));
			Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
			Error.PropertyPath = TEXT("parameters_object_decoded");
			Error.SourceKind = TEXT("command_input");
			return Fail(OutError, MoveTemp(Error), TargetIdentity);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| ObjectPathFromPackageIdentity(Target)
				!= ObjectPathFromPackageIdentity(TargetIdentity))
		{
			const auto Actual = Params->TryGetField(TEXT("target"));
			auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("world_hlod_target_identity_mismatch"),
				TEXT("/target"), MakeShared<FJsonValueString>(TargetIdentity), Actual.IsValid() ? Actual : MakeShared<FJsonValueNull>(),
				TEXT("input_decode"), TEXT("The command target must match the asset selected for the HLOD domain."));
			Error.ActualValuePresent = Actual.IsValid();
			Error.SourceKind = TEXT("command_input");
			return Fail(OutError, MoveTemp(Error), TargetIdentity);
		}

		const auto* Profile = ResolveProfile(Target, RootType, OutError);
		if (!Profile) return false;
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			FString WorkspacePath;
			if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
				|| WorkspacePath.IsEmpty())
			{
				const auto Actual = Params->TryGetField(TEXT("workspace_path"));
				auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("world_hlod_export_path_required"),
					TEXT("/workspace_path"), MakeShared<FJsonValueString>(TEXT("non-empty workspace directory path")),
					Actual.IsValid() ? Actual : MakeShared<FJsonValueNull>(), TEXT("input_decode"),
					TEXT("HLOD export requires a non-empty workspace_path string."));
				Error.ActualValuePresent = Actual.IsValid();
				Error.SourceKind = TEXT("command_input");
				return Fail(OutError, MoveTemp(Error), Target);
			}
			bOutReadOnly = true;
			return Composition::HjsonImport::TryExportReflectionRootWorkspace(
				*Profile, Target, WorkspacePath, OutPayloadJson, OutError, PreparedExport);
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			FResolvedDomainHjsonDocument Document;
			if (!TryDecodeResolvedDocument(Target, CanonicalParamsJson,
				FDomainHjsonDocumentContract(TEXT("world_hlod"),
					RootType == TEXT("/Script/Engine.DataLayerAsset") ? TEXT("uai.world_data_layer.edit/1") : TEXT("uai.world_hlod_layer.edit/1"), RootType),
				Document, OutError)) return false;
			FScopedDomainHjsonDiagnosticSource DiagnosticSource(Document, OutError);
			if (!Document.bSemanticWorkspaceDirectory)
				return Create(*Profile, Target, RootType, Document, OutPayloadJson, OutError);
			if (!Document.SemanticWorkspace.IsValid())
			{
				auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_hlod_semantic_workspace_required"),
					TEXT("The resolved HLOD workspace directory has no semantic workspace data."), TEXT("workspace_resolve"));
				Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
				Error.PropertyPath = TEXT("semantic_workspace_resolved");
				Error.SourceKind = TEXT("native_result");
				return Fail(OutError, MoveTemp(Error), Target);
			}
			return Composition::HjsonImport::TryImportReflectionWorkspace(
				*Profile, Document.SemanticWorkspace.ToSharedRef(),
				{TEXT("world-hlod-domain-provider"), Target, ObjectPathFromPackageIdentity(Target),
					TEXT("world-hlod-semantic"),
					TEXT("world_hlod_semantic_prepare_failed"),
					TEXT("world_hlod_semantic_apply_failed"),
					TEXT("world_hlod_semantic_readback_payload_invalid"),
					TEXT("world_hlod_compile_not_settled")},
				[](TArray<TSharedPtr<FJsonValue>>& Properties)
				{
					TArray<TSharedPtr<FJsonValue>> Ordered;
					auto MoveNamed = [&Properties, &Ordered](const TCHAR* Name)
					{
						for (int32 Index = 0; Index < Properties.Num(); ++Index)
						{
							const TSharedPtr<FJsonObject> Descriptor = Properties[Index]->AsObject();
							FString Path;
							if (Descriptor.IsValid()
								&& Descriptor->TryGetStringField(TEXT("property_path"), Path)
								&& Path == Name)
							{
								Ordered.Add(Properties[Index]);
								Properties.RemoveAt(Index);
								return;
							}
						}
					};
					MoveNamed(TEXT("LayerType"));
					MoveNamed(TEXT("HLODBuilderClass"));
					Ordered.Append(MoveTemp(Properties));
					Properties = MoveTemp(Ordered);
				}, OutPayloadJson, OutError);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			bOutReadOnly = true;
			return Read(Target, RootType, OutPayloadJson, OutError);
		}
		auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_hlod_command_not_supported"),
			TEXT("The HLOD domain adapter only supports content_graph commands."), TEXT("domain_resolve"), CommandId);
		Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
		Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
		Error.PropertyPath = TEXT("content_graph_command");
		Error.SourceKind = TEXT("native_result");
		return Fail(OutError, MoveTemp(Error), Target);
	}
}
