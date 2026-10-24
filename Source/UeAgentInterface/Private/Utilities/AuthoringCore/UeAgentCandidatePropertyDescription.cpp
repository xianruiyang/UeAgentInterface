#include "Utilities/AuthoringCore/UeAgentCandidatePropertyDescription.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::CandidateProperties
{
	bool ReadRequest(const TSharedPtr<FJsonObject>& Description, FRequest& Out, UE::UAI::Contract::FDiagnostic& OutError)
	{
		Out = {};
		auto Reject = [&](const TCHAR* Code, const FString& Pointer, const TCHAR* Expected,
			const TSharedPtr<FJsonValue>& Actual, const TCHAR* Reason)
		{
			OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Pointer,
				MakeShared<FJsonValueString>(Expected), Actual, TEXT("candidate_description"), Reason);
			if (Description) UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Description);
			return false;
		};
		if (!Description || !Description->TryGetStringField(TEXT("name"), Out.Name) || Out.Name.TrimStartAndEnd().IsEmpty())
			return Reject(TEXT("candidate_description_name_required"), TEXT("/name"), TEXT("non-empty string"),
				Description ? Description->TryGetField(TEXT("name")) : nullptr, TEXT("A non-empty candidate name is required."));
		const TSharedPtr<FJsonObject>* Select = nullptr;
		if (Description->HasField(TEXT("select")))
		{
			if (!Description->TryGetObjectField(TEXT("select"), Select))
				return Reject(TEXT("candidate_description_select_invalid"), TEXT("/select"), TEXT("object of strings"),
					Description->TryGetField(TEXT("select")), TEXT("The candidate selection must be an object of string fields."));
			for (const auto& Pair : (*Select)->Values)
			{
				FString Value;
				if (!Pair.Value || !Pair.Value->TryGetString(Value))
					return Reject(TEXT("candidate_description_select_invalid"), TEXT("/select/") +
						Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")), TEXT("string"),
						Pair.Value, TEXT("Each candidate selection field must be a string."));
				Out.Select.Add(Pair.Key, Value);
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
		if (Description->HasField(TEXT("fields")))
		{
			if (!Description->TryGetArrayField(TEXT("fields"), Fields))
				return Reject(TEXT("candidate_description_fields_invalid"), TEXT("/fields"), TEXT("array of non-empty property names"),
					Description->TryGetField(TEXT("fields")), TEXT("Candidate fields must be an array of non-empty property names."));
			for (int32 Index = 0; Index < Fields->Num(); ++Index)
			{
				const auto& Value = (*Fields)[Index];
				FString Field;
				if (!Value || !Value->TryGetString(Field) || Field.IsEmpty())
					return Reject(TEXT("candidate_description_fields_invalid"), TEXT("/fields/") + FString::FromInt(Index),
						TEXT("non-empty property name"), Value, TEXT("Each requested property name must be a non-empty string."));
				Out.Fields.Add(FName(*Field));
			}
		}
		return true;
	}

	bool DescribeObject(const UObject& Prototype, const FRequest& Request,
		UeAgentAuthoringState::FEditablePropertyReadOptions Policy,
		TSharedPtr<FJsonObject>& OutDescription, UE::UAI::Contract::FDiagnostic& OutError)
	{
		// Candidate prototypes use native object-property identity, not the future
		// graph owner's export identity. Keep only its editable-field policy.
		Policy.SemanticId = TEXT("core.property");
		Policy.BuildSnapshotIdentity = {};
		const auto Include = Policy.IncludeProperty;
		Policy.IncludeProperty = [&Request, Include](const FProperty* Property, const void* Value, const UObject* Owner)
		{
			return (Request.Fields.IsEmpty() || Request.Fields.Contains(Property->GetFName()))
				&& (!Include || Include(Property, Value, Owner));
		};
		TArray<UeAgentAuthoringState::FReadRequest> Reads;
		if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(&Prototype, Policy, Reads, OutError)) return false;
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!UeAgentAuthoringState::CaptureSnapshot(Reads, Capture))
		{ OutError = Capture.Diagnostic; return false; }
		UeAgentDetailsAuthoringService::FProjectionPolicy Projection;
		Projection.bIncludeDisplayName = false;
		Projection.bIncludeTooltip = false;
		Projection.bIncludeAvailability = false;
		Projection.bIncludeCategoryPath = false;
		TSharedPtr<FJsonObject> Document;
		auto Entries = Capture.Snapshot.CopyEntries();
		Entries.RemoveAll([](const auto& Entry) { return !Entry.bAuthoringVisible; });
		if (!UeAgentDetailsAuthoringService::BuildPropertiesDocument(Entries, Projection, Document, OutError)) return false;
		OutDescription = MakeShared<FJsonObject>();
		OutDescription->SetStringField(TEXT("name"), Request.Name);
		OutDescription->SetStringField(TEXT("basis"), TEXT("native_defaults"));
		const auto& Properties = Document->GetArrayField(TEXT("properties"));
		OutDescription->SetArrayField(TEXT("properties"), Properties);
		TSet<FName> Unavailable = Request.Fields;
		for (const auto& Property : Properties)
			Unavailable.Remove(FName(*Property->AsObject()->GetStringField(TEXT("property_path"))));
		if (!Unavailable.IsEmpty())
		{
			TArray<FName> Names = Unavailable.Array();
			Names.Sort(FNameLexicalLess());
			TArray<TSharedPtr<FJsonValue>> Values;
			for (FName Name : Names) Values.Add(MakeShared<FJsonValueString>(Name.ToString()));
			OutDescription->SetArrayField(TEXT("unavailable_fields"), Values);
		}
		return true;
	}

	void SetCreationSupport(FJsonObject& Description, bool bSupported, const FString& Reason)
	{
		const auto Creation = MakeShared<FJsonObject>();
		Creation->SetBoolField(TEXT("supported"), bSupported);
		if (!Reason.IsEmpty()) Creation->SetStringField(TEXT("reason"), Reason);
		Description.SetObjectField(TEXT("creation"), Creation);
	}

	bool BuildCreationSupportPayload(const FRequest& Request, bool bSupported,
		const FString& Reason, const FString& Basis, const FString& InterfaceScope,
		FString& OutPayload, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const auto Description = MakeShared<FJsonObject>();
		Description->SetStringField(TEXT("name"), Request.Name);
		Description->SetStringField(TEXT("basis"), Basis);
		SetCreationSupport(*Description, bSupported, Reason);
		Description->SetStringField(TEXT("interface_scope"), InterfaceScope);
		if (!Request.Fields.IsEmpty())
		{
			TArray<FName> Fields = Request.Fields.Array();
			Fields.Sort(FNameLexicalLess());
			TArray<TSharedPtr<FJsonValue>> Unavailable;
			for (FName Field : Fields) Unavailable.Add(MakeShared<FJsonValueString>(Field.ToString()));
			Description->SetArrayField(TEXT("unavailable_fields"), Unavailable);
		}
		return BuildPayload(Request, Description, OutPayload, OutError);
	}

	bool BuildPayload(const FRequest& Request, const TSharedPtr<FJsonObject>& Description,
		FString& OutPayload, UE::UAI::Contract::FDiagnostic& OutError)
	{
		auto Payload = MakeShared<FJsonObject>();
		Payload->SetArrayField(TEXT("names"), {MakeShared<FJsonValueString>(Request.Name)});
		Payload->SetObjectField(TEXT("description"), Description);
		return CommandContract::Json::TryCanonicalize(Payload, OutPayload, OutError);
	}
}
