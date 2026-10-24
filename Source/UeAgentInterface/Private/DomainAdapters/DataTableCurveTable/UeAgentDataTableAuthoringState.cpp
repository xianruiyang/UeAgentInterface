#include "DomainAdapters/DataTableCurveTable/UeAgentDataTableAuthoringState.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataTable.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "UObject/StructOnScope.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

namespace UE::UAI::DomainAdapters::DataTableCurveTable
{
	namespace
	{
		const FString IdentityModel = TEXT("data_table.authoring.identity");
		const FString TargetKind = TEXT("data_table");
		const FString AccessorId = TEXT("data_table.authoring.state");
		const FString TargetSemantic = TEXT("data_table.export.target");
		const FString RowSemantic = TEXT("data_table.rows");
		const FString PresenceSemantic = TEXT("data_table.target.presence");

		UE::UAI::Contract::FDiagnostic MissingDataTableSchema(
			const UDataTable* Table, const UObject* Root, const FString& Target)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("data_table_authoring_row_struct_missing"),
				Table ? TEXT("The DataTable has no row struct for authoring state capture.")
					: TEXT("The state capture root is missing or is not a DataTable."), TEXT("state_capture"));
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = Target;
			Error.PropertyPath = Table ? TEXT("/row_struct_ref") : TEXT("/root_class");
			Error.ActualValue = !Table && Root
				? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Root->GetClass()->GetPathName()))
				: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
			Error.ExpectedValue = MakeShared<FJsonValueString>(Table ? TEXT("a table row UScriptStruct") : TEXT("/Script/Engine.DataTable"));
			Error.ActualValuePresent = true;
			Error.ExpectedValuePresent = true;
			return Error;
		}

		void SupplementDataTableStateOwner(UE::UAI::Contract::FDiagnostic& Error,
			const FString& NativeOwner, const FString& LogicalOwner)
		{
			for (auto& Child : Error.Diagnostics) SupplementDataTableStateOwner(Child, NativeOwner, LogicalOwner);
			if (Error.PublicCause)
			{
				auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Error.PublicCause);
				SupplementDataTableStateOwner(*Cause, NativeOwner, LogicalOwner); Error.PublicCause = MoveTemp(Cause);
			}
			if (Error.TargetPath.IsEmpty() || Error.TargetPath == NativeOwner) Error.TargetPath = LogicalOwner;
		}

		TSharedRef<FJsonObject> CapturePresence(const FString& Path)
		{
			const auto Value = MakeShared<FJsonObject>();
			Value->SetBoolField(TEXT("object_present"), FindObject<UObject>(nullptr, *Path) != nullptr);
			Value->SetBoolField(TEXT("disk_present"), FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path)));
			Value->SetBoolField(TEXT("registry_present"), FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Path)).IsValid());
			return Value;
		}

		bool CaptureRecord(const UDataTable& Table, const void* Data,
			TSharedPtr<FJsonObject>& Properties, TSharedPtr<FJsonObject>& Definitions, UE::UAI::Contract::FDiagnostic& OutError)
		{
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			if (!UeAgentAuthoringState::AppendEditableStructPropertyReadRequests(
				Table.RowStruct.Get(), Data, Table.GetPathName(), &Table,
				UeAgentAuthoringState::FEditablePropertyReadOptions(), Requests, OutError)) return false;
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
			{
				OutError = Capture.Diagnostic;
				return false;
			}
			HjsonWorkspace::FWorkspaceError Error;
			if (!HjsonWorkspace::TryProjectReflectedRecord(Capture.Snapshot, Properties, Definitions, Error))
			{
				OutError = Error.GetDiagnostic();
				return false;
			}
			return true;
		}

		bool BuildIdentity(const FString& AssetPath, const FString& RowName,
			const FString& PropertyPath,
			UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity, UE::UAI::Contract::FDiagnostic& OutError)
		{
			TMap<FString, FString> Selectors{{TEXT("asset_path"), AssetPath}};
			if (!RowName.IsEmpty()) Selectors.Add(TEXT("row_name"), RowName);
			if (!PropertyPath.IsEmpty()) Selectors.Add(TEXT("property_path"), PropertyPath);
			return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
				IdentityModel, TargetKind, Selectors, OutIdentity, OutError);
		}

		UeAgentAuthoringState::EReadOutcome ReadState(
			const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
		{
			using namespace UeAgentAuthoringState;
			const UDataTable* Table = Cast<UDataTable>(Request.RootObject);
			if (Request.SemanticId != PresenceSemantic && (!Table || !Table->GetRowStruct()))
			{
				OutError = MissingDataTableSchema(Table, Request.RootObject, Request.RootOwnerPath);
				return EReadOutcome::Failed;
			}
			OutValue = FStateValue();
			OutValue.TargetIdentity = Request.TargetIdentity;
			OutValue.Type.Kind = TEXT("object");
			OutValue.Provenance = AccessorId;
			TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			if (Request.SemanticId == PresenceSemantic) Value = CapturePresence(Request.RootOwnerPath);
			else if (Request.SemanticId == TargetSemantic)
			{
				Value->SetStringField(TEXT("asset_path"), Request.RootOwnerPath);
				Value->SetStringField(TEXT("asset_kind"), TargetKind);
			}
			else if (Request.SemanticId == RowSemantic)
			{
				TSharedPtr<FJsonObject> Schema;
				if (!BuildDataTableRowSchema(*Table, Schema, OutError))
				{
					SupplementDataTableStateOwner(OutError, Table->GetPathName(), Request.RootOwnerPath);
					return EReadOutcome::Failed;
				}
				Value->SetObjectField(TEXT("row_schema"), Schema);
				Value->SetStringField(TEXT("row_struct_ref"), Table->GetRowStruct()->GetPathName());
				const TSharedRef<FJsonObject> Rows = MakeShared<FJsonObject>();
				for (const auto& Pair : Table->GetRowMap())
				{
					TSharedPtr<FJsonObject> Properties;
					TSharedPtr<FJsonObject> RowDefinitions;
					if (!CaptureRecord(*Table, Pair.Value, Properties, RowDefinitions, OutError))
					{
						SupplementDataTableStateOwner(OutError, Table->GetPathName(), Request.RootOwnerPath);
						const FString RowPointer = TEXT("/rows/") + Pair.Key.ToString().Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
						if (!OutError.PropertyPath.StartsWith(TEXT("/rows/")))
							OutError.PropertyPath = RowPointer + (OutError.PropertyPath.IsEmpty() ? FString()
								: TEXT("/properties/") + OutError.PropertyPath.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
						if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("native_result");
						return EReadOutcome::Failed;
					}
					const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					UeAgentAuthoringEncodedTypes::FScope::SetNameField(Row, TEXT("name"), Pair.Key);
					Row->SetObjectField(TEXT("properties"), Properties);
					Rows->SetObjectField(Pair.Key.ToString(), Row);
				}
				UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(Rows);
				Value->SetObjectField(TEXT("rows"), Rows);
				OutValue.bEditable = true;
			}
			else
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("data_table_authoring_semantic_unsupported"),
					TEXT("The DataTable state accessor does not support the requested semantic."), TEXT("state_capture"));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Request.RootOwnerPath;
				OutError.PropertyPath = TEXT("/semantic_supported");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Request.SemanticId);
				OutError.ExpectedEvidence = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
					MakeShared<FJsonValueString>(TargetSemantic), MakeShared<FJsonValueString>(RowSemantic), MakeShared<FJsonValueString>(PresenceSemantic)});
				return EReadOutcome::Failed;
			}
			OutValue.Value = MakeShared<FJsonValueObject>(Value);
			return EReadOutcome::Resolved;
		}
	}

	bool RegisterDataTableAuthoringState(UE::UAI::Contract::FDiagnostic& OutError)
	{
		UeAgentAuthoringIdentity::FModelDescriptor Identity;
		Identity.ModelId = IdentityModel;
		Identity.TargetKind = TargetKind;
		Identity.Selectors = {{TEXT("asset_path"), 100, false},
			{TEXT("row_name"), 90, false}, {TEXT("property_path"), 80, true}};
		Identity.KeyPatterns = {
			{TEXT("property"), 0, {TEXT("asset_path"), TEXT("row_name"), TEXT("property_path")}},
			{TEXT("row"), 10, {TEXT("asset_path"), TEXT("row_name")}},
			{TEXT("asset"), 20, {TEXT("asset_path")}}};
		UeAgentAuthoringIdentity::ERegistrationResult IdentityResult;
		if (!UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(
			Identity, IdentityResult, OutError)) return false;
		using namespace UeAgentAuthoringSemanticRegistry;
		FDomainDescriptor Domain;
		Domain.DomainId = TEXT("data_table.authoring");
		Domain.TargetKinds = {TargetKind};
		Domain.MutationProviderId = TEXT("data_table.authoring.apply");
		Domain.CapabilityTags = {TEXT("table"), TEXT("editor_authoring")};
		ERegistrationResult DomainResult;
		if (!FRegistry::Get().RegisterDomain(Domain, DomainResult, OutError)) return false;
		for (const FString& SemanticId : {TargetSemantic, RowSemantic, PresenceSemantic})
		{
			FSemanticDescriptor Descriptor;
			Descriptor.SemanticId = SemanticId;
			Descriptor.DomainId = Domain.DomainId;
			Descriptor.TargetKind = TargetKind;
			Descriptor.ValueType = TEXT("object");
			Descriptor.OwnerModel = TEXT("data_table_asset_owner");
			Descriptor.IdentityModel = IdentityModel;
			Descriptor.StateAccessor = AccessorId;
			Descriptor.WriteCapability = SemanticId != RowSemantic
				? EWriteCapability::ReadOnly : EWriteCapability::DomainMutation;
			Descriptor.ConstraintSource = TEXT("data_table.row_struct");
			Descriptor.AvailabilitySource = AccessorId;
			Descriptor.Comparator = TEXT("uai.compare.canonical_value");
			Descriptor.CapabilityTags = Domain.CapabilityTags;
			ERegistrationResult Result;
			if (!FRegistry::Get().RegisterSemantic(Descriptor, Result, OutError)) return false;
		}
		UeAgentAuthoringState::FStateAccessorDescriptor Accessor;
		Accessor.AccessorId = AccessorId;
		Accessor.TargetKinds = {TargetKind};
		Accessor.CapabilityTags = Domain.CapabilityTags;
		UeAgentAuthoringState::ERegistrationResult StateResult;
		return UeAgentAuthoringState::RegisterBuiltInStateAccessors(
			UeAgentAuthoringState::FStateAccessorRegistry::Get(), OutError)
			&& UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(
				Accessor, &ReadState, StateResult, OutError);
	}

	bool BuildDataTableRowSchema(const UDataTable& Table, TSharedPtr<FJsonObject>& OutSchema, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutSchema.Reset();
		if (!Table.GetRowStruct()) { OutError = MissingDataTableSchema(&Table, &Table, Table.GetPathName()); return false; }
		FStructOnScope Defaults(Table.GetRowStruct());
		TSharedPtr<FJsonObject> Properties, Definitions;
		if (!CaptureRecord(Table, Defaults.GetStructMemory(), Properties, Definitions, OutError)) return false;
		OutSchema = MakeShared<FJsonObject>();
		OutSchema->SetStringField(TEXT("type"), Table.GetRowStruct()->GetName());
		OutSchema->SetObjectField(TEXT("properties"), Properties);
		OutSchema->SetObjectField(TEXT("definitions"), Definitions);
		return true;
	}

	bool IsDataTableTargetAbsent(const FString& AssetPath)
	{
		const auto Value = CapturePresence(AssetPath);
		return !Value->GetBoolField(TEXT("object_present")) && !Value->GetBoolField(TEXT("disk_present")) && !Value->GetBoolField(TEXT("registry_present"));
	}

	bool BuildDataTablePresenceReadRequests(const FString& AssetPath,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRequests.Reset();
		UeAgentAuthoringState::FReadRequest Request;
		Request.SemanticId = PresenceSemantic;
		Request.RootOwnerPath = AssetPath;
		if (!BuildIdentity(AssetPath, FString(), FString(), Request.TargetIdentity, OutError)) return false;
		Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
		Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
		Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
		OutRequests.Add(MoveTemp(Request));
		return true;
	}

	bool BuildDataTableDomainReadRequests(const UDataTable& Table,
		const FString& CanonicalAssetPath,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError)
	{
		using namespace UeAgentAuthoringState;
		OutRequests.Reset();
		if (!Table.GetRowStruct())
		{
			OutError = MissingDataTableSchema(&Table, &Table, CanonicalAssetPath);
			return false;
		}
		auto AddState = [&](const FString& SemanticId, const FString& RowName)
		{
			FReadRequest Request;
			Request.SemanticId = SemanticId;
			Request.RootObject = &Table;
			Request.RootOwnerPath = CanonicalAssetPath;
			if (!BuildIdentity(CanonicalAssetPath, RowName, FString(), Request.TargetIdentity, OutError)) return false;
			Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
			Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
			Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
			OutRequests.Add(MoveTemp(Request));
			return true;
		};
		return AddState(TargetSemantic, FString()) && AddState(RowSemantic, FString());
	}

	bool BuildDataTableAuthoringReadRequests(const UDataTable& Table,
		const FString& CanonicalAssetPath,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError)
	{
		return BuildDataTableDomainReadRequests(Table, CanonicalAssetPath, OutRequests, OutError)
			&& UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(&Table,
				UeAgentAuthoringState::FEditablePropertyReadOptions(), OutRequests, OutError);
	}
}
