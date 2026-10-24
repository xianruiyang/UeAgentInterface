#include "Composition/HjsonImport/UeAgentReflectedObjectRecord.h"

#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Class.h"

namespace UE::UAI::Composition::HjsonImport
{
	namespace
	{
		bool ProjectRecordChanges(const UeAgentAuthoringState::FStateSnapshot& Snapshot, const UObject& Owner,
			const TSharedPtr<FJsonObject>& Properties, TArray<TSharedPtr<FJsonValue>>& Changes, UE::UAI::Contract::FDiagnostic& OutError)
		{
			FString Text;
			const auto Writer = TJsonWriterFactory<>::Create(&Text);
			if (!FJsonSerializer::Serialize(Properties.ToSharedRef(), Writer))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("reflected_record_json_invalid"),
					TEXT("The reflected property record could not be serialized as JSON"), TEXT("authoring"), Owner.GetPathName());
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("properties.serialization_succeeded");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.ActualValuePresent = true;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
			HjsonWorkspace::FHjsonDocumentPtr Parsed;
			HjsonWorkspace::FWorkspaceError ParseError;
			if (!HjsonWorkspace::TryParseHjsonDocument(Text, TEXT("object_properties"), Resources, Parsed, ParseError))
			{ OutError = ParseError.GetDiagnostic(); return false; }
			FSemanticWorkspaceImportError Error;
			if (!TryProjectReflectedPropertyValues(Snapshot, *Parsed->GetRoot(), Resources, Changes, Error))
			{ OutError = Error; return false; }
			return true;
		}
	}
	bool CaptureReflectedObjectRecord(const UObject& Object,
		const UeAgentDetailsAuthoringService::FCapturePolicy& Policy,
		TSharedPtr<FJsonObject>& OutProperties, TSharedPtr<FJsonObject>& OutDefinitions, UE::UAI::Contract::FDiagnostic& OutError)
	{
		UeAgentAuthoringState::FStateSnapshot Snapshot;
		TSharedPtr<FJsonObject> Document;
		if (!UeAgentDetailsAuthoringService::CaptureObjectProperties(&Object, Policy, Snapshot, Document, OutError)) return false;
		HjsonWorkspace::FWorkspaceError Error;
		if (!HjsonWorkspace::TryProjectReflectedRecord(Snapshot, OutProperties, OutDefinitions, Error, true))
		{ OutError = Error.GetDiagnostic(); return false; }
		return true;
	}

	bool PrepareReflectedObjectRecord(UObject& Object,
		const UeAgentDetailsAuthoringService::FCapturePolicy& Policy,
		const TSharedPtr<FJsonObject>& Properties,
		UeAgentAuthoringObjectApply::FPreparedApply& OutPrepared, UE::UAI::Contract::FDiagnostic& OutError)
	{
		UeAgentAuthoringState::FStateSnapshot Snapshot;
		TSharedPtr<FJsonObject> Document;
		if (!Properties.IsValid()) return true;
		if (!UeAgentDetailsAuthoringService::CaptureObjectProperties(&Object, Policy, Snapshot, Document, OutError)) return false;
		TArray<TSharedPtr<FJsonValue>> Changes;
		if (!ProjectRecordChanges(Snapshot, Object, Properties, Changes, OutError)) return false;
		UeAgentAuthoringObjectApply::FOptions Options;
		Options.bAllowDomainAuthorizedOwners = true;
		Options.bCompactValues = true;
		Options.AuthorizeProperty = Policy.AuthorizeProperty;
		UeAgentAuthoringObjectApply::FFailure Failure;
		if (!UeAgentAuthoringObjectApply::PrepareObject(&Object, Changes, Options, OutPrepared, Failure))
		{ OutError = Failure.Diagnostic; return false; }
		return true;
	}

	bool CaptureReflectedStructRecord(UStruct& Type, const void* Data, UObject& Owner,
		const TArray<FString>& PropertyPaths, const UeAgentDetailsAuthoringService::FCapturePolicy& Policy,
		TSharedPtr<FJsonObject>& OutProperties, TSharedPtr<FJsonObject>& OutDefinitions, UE::UAI::Contract::FDiagnostic& OutError)
	{
		UeAgentAuthoringState::FStateSnapshot Snapshot;
		TSharedPtr<FJsonObject> Document;
		if (!UeAgentDetailsAuthoringService::CaptureStructPropertyPaths(&Type, Data, Owner.GetPathName(),
			&Owner, PropertyPaths, Policy, Document, OutError, &Snapshot)) return false;
		HjsonWorkspace::FWorkspaceError Error;
		if (!HjsonWorkspace::TryProjectReflectedRecord(Snapshot, OutProperties, OutDefinitions, Error, true))
		{ OutError = Error.GetDiagnostic(); return false; }
		return true;
	}

	bool PrepareReflectedStructRecord(UStruct& Type, void* Data, UObject& Owner,
		const TArray<FString>& PropertyPaths, const UeAgentDetailsAuthoringService::FCapturePolicy& Policy,
		const TSharedPtr<FJsonObject>& Properties, const UeAgentAuthoringObjectApply::FOptions& Options,
		UeAgentAuthoringObjectApply::FPreparedApply& OutPrepared, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Properties.IsValid()) return true;
		UeAgentAuthoringState::FStateSnapshot Snapshot;
		TSharedPtr<FJsonObject> Document;
		if (!UeAgentDetailsAuthoringService::CaptureStructPropertyPaths(&Type, Data, Owner.GetPathName(),
			&Owner, PropertyPaths, Policy, Document, OutError, &Snapshot)) return false;
		TArray<TSharedPtr<FJsonValue>> Changes;
		if (!ProjectRecordChanges(Snapshot, Owner, Properties, Changes, OutError)) return false;
		UeAgentAuthoringObjectApply::FFailure Failure;
		auto ApplyOptions = Options;
		ApplyOptions.bCompactValues = true;
		if (!UeAgentAuthoringObjectApply::PrepareStruct(&Type, Data, &Owner, Changes, ApplyOptions, OutPrepared, Failure))
		{ OutError = Failure.Diagnostic; return false; }
		return true;
	}
}
