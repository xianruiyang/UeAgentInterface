#include "DomainAdapters/LocalizationStringTable/UeAgentLocalizationStringTableHjsonProvider.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/LocalizationStringTable/UeAgentLocalizationSettingsAuthoringApply.h"
#include "DomainAdapters/LocalizationStringTable/UeAgentLocalizationSettingsAuthoringState.h"
#include "DomainAdapters/LocalizationStringTable/UeAgentLocalizationSettingsConfig.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "DomainAdapters/LocalizationStringTable/UeAgentStringTableAuthoringState.h"
#include "DomainAdapters/LocalizationStringTable/UeAgentStringTableAuthoringApply.h"
#include "DomainAdapters/ContentGraphRead/UeAgentExternalLocalizationContentGraphProvider.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "LocalizationSettings.h"
#include "LocalizationTargetTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainAdapters::LocalizationStringTable
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		const FString ProviderId = TEXT("localization-string-table-hjson-provider");
		const FString LocalizationSchema = TEXT("uai.localization.edit/1");
		const FString StringTableSchema = TEXT("uai.string_table.edit/1");

		bool Fail(FWorkspaceError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic,
            const FString& Source = FString(), const FString& Pointer = TEXT("/"))
        {
            OutError.SetFailure(MoveTemp(Diagnostic), Source, Pointer);
            return false;
        }

		FString Sha256Bytes(const TArray<uint8>& Bytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			SHA256(Bytes.GetData(), Bytes.Num(), Digest);
			return TEXT("sha256:")
				+ BytesToHex(Digest, SHA256_DIGEST_LENGTH).ToLower();
		}

		FString Sha256Text(const FString& Text)
		{
			FTCHARToUTF8 Utf8(*Text);
			TArray<uint8> Bytes;
			Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			return Sha256Bytes(Bytes);
		}


		UE::UAI::Contract::FDiagnostic ProviderFailure(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Field, const TSharedPtr<FJsonValue>& Actual,
			const TSharedPtr<FJsonValue>& Expected, const FString& SourceKind = TEXT("native_result"))
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
			Error.TargetPath = Target;
			Error.PropertyPath = Field;
			Error.ActualValue = Actual;
			Error.ExpectedValue = Expected;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			Error.SourceKind = SourceKind;
			if (SourceKind == TEXT("command_input")) Error.Source.JsonPath = TEXT("/params") + Field;
			return Error;
		}

		UE::UAI::Contract::FDiagnostic ProviderCondition(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Field, bool Actual = false, bool Expected = true)
		{
			return ProviderFailure(Code, Reason, Target, Field,
				MakeShared<FJsonValueBoolean>(Actual), MakeShared<FJsonValueBoolean>(Expected));
		}

		TSharedPtr<FJsonValue> ParamFact(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid()) return nullptr;
			if (Value->Type == EJson::Object || Value->Type == EJson::Array)
				return MakeShared<FJsonValueString>(Value->Type == EJson::Array ? TEXT("array") : TEXT("object"));
			return Value;
		}

		bool CanonicalObject(const TSharedPtr<FJsonObject>& Object, FString& Out, FWorkspaceError& OutError)
		{
			UE::UAI::Contract::FDiagnostic Error;
			if (!Object.IsValid() || !Json::TryCanonicalize(Object, Out, Error))
			{
				if (Error.IsEmpty()) Error = ProviderCondition(TEXT("localization_hjson_json_invalid"),
					TEXT("The generated localization response is unavailable for JSON encoding."), FString(), TEXT("/response_present"));
				if (Error.SourceKind.IsEmpty()) Error.SourceKind = TEXT("native_result");
				return Fail(OutError, MoveTemp(Error));
			}
			return true;
		}

		bool ParseParams(const FString& CanonicalParamsJson, TSharedPtr<FJsonObject>& OutParams, FWorkspaceError& OutError)
		{
			UE::UAI::Contract::FDiagnostic Error;
			if (!Json::TryParseObject(CanonicalParamsJson, OutParams, Error) || !OutParams.IsValid())
			{
				if (Error.IsEmpty()) Error = ProviderFailure(TEXT("localization_hjson_params_invalid"),
					TEXT("The localization command parameters must decode to a JSON object."), FString(), FString(),
					nullptr, MakeShared<FJsonValueString>(TEXT("object")), TEXT("command_input"));
				if (Error.SourceKind.IsEmpty()) Error.SourceKind = TEXT("command_input");
				return Fail(OutError, MoveTemp(Error));
			}
			return true;
		}

		TSharedPtr<FJsonValue> SupportedRootTypes()
		{
			return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
				MakeShared<FJsonValueString>(UStringTable::StaticClass()->GetPathName()),
				MakeShared<FJsonValueString>(ULocalizationSettings::StaticClass()->GetPathName())});
		}

		UE::UAI::Contract::FDiagnostic UnsupportedProjection(const FString& Code, const FString& Target,
			const Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection)
		{
			const auto Actual = MakeShared<FJsonObject>();
			Actual->SetNumberField(TEXT("structural_edits"), Projection.StructuralEdits.Num());
			Actual->SetNumberField(TEXT("root_object_properties"), Projection.RootObjectProperties.Num());
			Actual->SetNumberField(TEXT("property_change_units"), Projection.PropertyChangesByUnitId.Num());
			return ProviderFailure(Code, TEXT("This domain accepts its semantic state units and does not support structural or reflected property edits."),
				Target, TEXT("/change_projection"), MakeShared<FJsonValueObject>(Actual),
				MakeShared<FJsonValueString>(TEXT("no structural or reflected property edits")));
		}

		UE::UAI::Contract::FDiagnostic UnsupportedSemantic(const FString& Code, const FString& Target,
			const FString& Actual, const FString& Expected)
		{
			return ProviderFailure(Code, TEXT("The prepared change uses a semantic surface unsupported by this domain."),
				Target, TEXT("/semantic_id"), MakeShared<FJsonValueString>(Actual), MakeShared<FJsonValueString>(Expected));
		}

		UE::UAI::Contract::FSourceLocation ResolveProjectedSource(
			const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
			const Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
			const FString& SemanticId, const FString& Pointer)
		{
			TArray<FString> Identities;
			for (const auto& Change : Prepared.GetChangeSetDraft().GetChanges())
				if (Change.GetIdentity().GetSemanticId() == SemanticId)
					Identities.AddUnique(Change.GetIdentity().GetCanonicalKey());
			UE::UAI::Contract::FDiagnostic Location;
			Location.Source.JsonPath = Pointer;
			Location.SourceKind = TEXT("authored_value");
			Prepared.GetWorkspace()->LocateDocumentDiagnostic(Identities, Projection.DocumentSourceBindings, Location);
			return Location.Source;
		}

		FString NormalizeObjectPath(const FString& Target)
		{
			return FPackageName::IsValidLongPackageName(Target)
				? Target + TEXT(".") + FPackageName::GetLongPackageAssetName(Target)
				: Target;
		}

		UStringTable* LoadStringTable(const FString& Target)
		{
			const FString ObjectPath = NormalizeObjectPath(Target);
			if (UStringTable* const Loaded = FindObject<UStringTable>(
					nullptr,
					*ObjectPath))
			{
				return Loaded;
			}
			const FString PackageName =
				FPackageName::ObjectPathToPackageName(ObjectPath);
			const FString Filename = FPackageName::LongPackageNameToFilename(
				PackageName,
				FPackageName::GetAssetPackageExtension());
			return FPaths::FileExists(Filename)
				? LoadObject<UStringTable>(nullptr, *ObjectPath)
				: nullptr;
		}

		FString PackageRevision(const FString& Target)
		{
			const FString PackageName = Target.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Target) : Target;
			const FString Filename = FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetAssetPackageExtension());
			TArray<uint8> Bytes;
			return FFileHelper::LoadFileToArray(Bytes, *Filename)
				? Sha256Bytes(Bytes) : FString(TEXT("absent"));
		}

		TSharedPtr<FJsonObject> BuildStringTableState(UStringTable& Table)
		{
			const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("type"), UStringTable::StaticClass()->GetPathName());
			State->SetStringField(TEXT("namespace"), Table.GetStringTable()->GetNamespace());
			TArray<TSharedPtr<FJsonValue>> Entries;
			Table.GetStringTable()->EnumerateSourceStrings(
				[&Entries, &Table](const FString& Key, const FString& Source)
				{
					const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("key"), Key);
					Entry->SetStringField(TEXT("source"), Source);
					const auto Metadata = MakeShared<FJsonObject>();
					Table.GetStringTable()->EnumerateMetaData(Key, [&](FName Name, const FString& Value)
					{
						Metadata->SetStringField(Name.ToString(), Value);
						return true;
					});
					Entry->SetObjectField(TEXT("metadata"), Metadata);
					Entries.Add(MakeShared<FJsonValueObject>(Entry));
					return true;
				});
			Entries.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
			{
				return A->AsObject()->GetStringField(TEXT("key"))
					< B->AsObject()->GetStringField(TEXT("key"));
			});
			State->SetArrayField(TEXT("entries"), Entries);
			return State;
		}

		bool ImportStringTable(const FString& Target,
			const DomainAuthoring::FResolvedDomainHjsonDocument& Document,
			FString& OutPayload, FWorkspaceError& OutError)
		{
			const FString Path = DomainAuthoring::ObjectPathFromPackageIdentity(Target);
			if (!IsStringTableTargetAbsent(Path))
				return Fail(OutError, ProviderCondition(TEXT("string_table_semantic_workspace_required"),
					TEXT("An existing StringTable must be updated through its semantic workspace."), Path, TEXT("/target_present"), true, false));
			if (Document.ExpectedRevision != TEXT("missing") && Document.ExpectedRevision != TEXT("absent"))
				{
				auto Error = ProviderCondition(TEXT("string_table_stale_revision"),
					TEXT("Creating a StringTable requires an expected revision declaring that the target is absent."),
					Path, TEXT("/expected_revision_declares_absence"), false);
				Error.SourceKind = TEXT("command_input");
				Error.Source.JsonPath = TEXT("/params/expected_revision");
				Error.ActualEvidence = MakeShared<FJsonValueString>(Document.ExpectedRevision);
				return Fail(OutError, MoveTemp(Error));
			}
			if (!Document.AstRoot) return Fail(OutError, ProviderCondition(TEXT("string_table_document_missing"),
				TEXT("The resolved StringTable document has no parsed root."), Path, TEXT("/parsed_root_present")));
			FKeyedItemProjectionContract Contract;
			if (!TryGetKeyedItemProjectionContract(TEXT("entries"), Contract))
				return Fail(OutError, ProviderCondition(TEXT("string_table_keyed_contract_missing"),
					TEXT("The StringTable entries projection contract is not registered."), Path, TEXT("/entries_contract_registered")));
			for (const auto& Field : Document.AstRoot->GetEntries())
				if (!Field.Key.Equals(TEXT("type"), ESearchCase::CaseSensitive)
					&& !Field.Key.Equals(Contract.RootField, ESearchCase::CaseSensitive)
					&& !Contract.IsEditableRootField(Field.Key))
				{
					auto Error = ProviderFailure(TEXT("string_table_field_unknown"),
						TEXT("The StringTable root does not support this field."), Path,
						TEXT("/") + Field.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")),
						MakeShared<FJsonValueString>(Field.Key),
						MakeShared<FJsonValueString>(TEXT("type, entries or an editable root field")), TEXT("authored_value"));
					Error.Stage = TEXT("input_decode");
					return Fail(OutError, MoveTemp(Error), Document.SourceUnit, Field.Value->GetProvenance().GetJsonPointer());
				}
			auto Resources = MakeSemanticWorkspaceResourceTracker();
			FUeAgentCanonicalValuePtr Captured, Visible, DesiredEntries;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryArray({}, Resources, Captured, StateError))
			{
				if (StateError.IsEmpty()) StateError = ProviderCondition(TEXT("string_table_empty_state_failed"),
					TEXT("The canonical empty StringTable state could not be created."), Path, TEXT("/empty_state_created"));
				if (StateError.SourceKind.IsEmpty()) StateError.SourceKind = TEXT("native_result");
				return Fail(OutError, MoveTemp(StateError));
			}
			const auto* Entries = Document.AstRoot->Get(Contract.RootField);
			if (Entries)
			{
				bool bChanged = false;
				if (!TryProjectHjsonCanonicalValue(*Entries, Resources, Visible, OutError, Contract.bInterpretSemanticValues)
					|| !TryResolveKeyedItemDesiredState(*Visible, *Captured, Document.SourceUnit,
						Contract, nullptr, Resources, DesiredEntries, bChanged, OutError, nullptr,
						EKeyedItemInputKind::VisibleDesiredState, nullptr, Entries)) return false;
			}
			else DesiredEntries = Captured;
			TSharedPtr<FJsonValue> JsonEntries;
			UE::UAI::Contract::FDiagnostic Error;
			if (!CanonicalJson::TryToJsonValue(*DesiredEntries, JsonEntries, Error))
				{
				if (Error.IsEmpty()) Error = ProviderCondition(TEXT("string_table_projection_failed"),
					TEXT("The resolved StringTable entries could not be projected into the apply document."), Path, TEXT("/entries_projection_succeeded"));
				if (Error.SourceKind.IsEmpty()) Error.SourceKind = TEXT("native_result");
				return Fail(OutError, MoveTemp(Error));
			}
			const auto Desired = MakeShared<FJsonObject>();
			Desired->SetField(TEXT("entries"), JsonEntries);
			const auto* Namespace = Document.AstRoot->Get(TEXT("namespace"));
			if (Namespace && Namespace->GetKind() != EHjsonNodeKind::String)
			{
				const TCHAR* Kind = TEXT("unknown");
				switch (Namespace->GetKind())
				{
				case EHjsonNodeKind::Null: Kind = TEXT("null"); break;
				case EHjsonNodeKind::Boolean: Kind = TEXT("boolean"); break;
				case EHjsonNodeKind::String: Kind = TEXT("string"); break;
				case EHjsonNodeKind::Integer: Kind = TEXT("integer"); break;
				case EHjsonNodeKind::Decimal: Kind = TEXT("decimal"); break;
				case EHjsonNodeKind::Array: Kind = TEXT("array"); break;
				case EHjsonNodeKind::Object: Kind = TEXT("object"); break;
				}
				auto NamespaceError = ProviderFailure(TEXT("string_table_namespace_invalid"),
					TEXT("The StringTable namespace must be a string."), Path, TEXT("/namespace"),
					MakeShared<FJsonValueString>(Kind), MakeShared<FJsonValueString>(TEXT("string")), TEXT("authored_value"));
				NamespaceError.Stage = TEXT("input_decode");
				return Fail(OutError, MoveTemp(NamespaceError), Document.SourceUnit, Namespace->GetProvenance().GetJsonPointer());
			}
			Desired->SetStringField(TEXT("namespace"), Namespace ? Namespace->GetText()
				: FPackageName::GetLongPackageAssetName(FPackageName::ObjectPathToPackageName(Path)));
			// entries is a keyed visible map; the resolved array carries its formal key identity.
			// Bind that identity to the original node, never reuse the final array index as an input index.
			UeAgentAuthoringDiagnostics::FScopedSourceLocations DesiredSources(&Desired.Get(), [&](const FString& Pointer)
			{
				if (Pointer == TEXT("/namespace") && Namespace)
					return Document.ResolveSource(Namespace->GetProvenance().GetJsonPointer());
				if (Pointer.StartsWith(TEXT("/entries/")) && Entries && Entries->GetKind() == EHjsonNodeKind::Object)
				{
					FString IndexText, Suffix;
					const FString Tail = Pointer.RightChop(9);
					if (!Tail.Split(TEXT("/"), &IndexText, &Suffix)) IndexText = Tail;
					const int32 Index = FCString::Atoi(*IndexText);
					const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
					if (FString::FromInt(Index) == IndexText && Desired->TryGetArrayField(TEXT("entries"), Values) && Values->IsValidIndex(Index))
					{
						const auto& Value = (*Values)[Index];
						FString Key;
						if (Value.IsValid() && Value->Type == EJson::Object && Value->AsObject()->TryGetStringField(TEXT("key"), Key))
							if (const auto* Original = Entries->Get(Key))
							{
								const FString OriginalPointer = Original->GetProvenance().GetJsonPointer();
								return Document.ResolveSource(OriginalPointer + (Suffix.IsEmpty() || Suffix == TEXT("key") ? FString() : TEXT("/") + Suffix));
							}
					}
				}
				return Document.ResolveSource(Entries && Pointer.StartsWith(TEXT("/entries"))
					? Entries->GetProvenance().GetJsonPointer() : Document.AstRoot->GetProvenance().GetJsonPointer());
			});
			DomainAuthoring::FRootPropertyApplyOutcome Outcome;
			TSharedPtr<FJsonObject> Readback;
			FString Revision;
			Outcome.FinalReadback = [&](UeAgentAuthoringOperationApply::FOperationFailure& Failure)
			{
				UStringTable* Table = LoadStringTable(Path);
				if (!Table)
				{
					Failure.Diagnostic = ProviderCondition(TEXT("string_table_reload_failed"),
						TEXT("The saved StringTable could not be reloaded for independent readback."), Path, TEXT("/target_present"));
					return false;
				}
				Readback = BuildStringTableState(*Table);
				Revision = PackageRevision(Path);
				return true;
			};
			if (!ApplyStringTableAuthoring(Path, TEXT("string-table-create-") + FGuid::NewGuid().ToString(),
				Desired, Outcome, nullptr, true))
				return Fail(OutError, Outcome.Diagnostic);
			const auto Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Response->SetStringField(TEXT("provider_id"), ProviderId);
			Response->SetStringField(TEXT("target"), Target);
			Response->SetBoolField(TEXT("created"), true);
			Response->SetBoolField(TEXT("saved"), true);
			Response->SetStringField(TEXT("revision"), Revision);
			Response->SetObjectField(TEXT("independent_readback"), Readback);
			return CanonicalObject(Response, OutPayload, OutError);
		}
		bool ImportStringTableWorkspace(const FString& Target,
			const DomainAuthoring::FResolvedDomainHjsonDocument& Document,
			FString& OutPayload, FWorkspaceError& OutError)
		{
			using namespace DomainAuthoring;
			UStringTable* Table = LoadStringTable(Target);
			UE::UAI::Contract::FDiagnostic Error;
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!Table || !RegisterStringTableAuthoringState(Error)
				|| !BuildStringTableAuthoringReadRequests(*Table->GetStringTable(), Target, Requests, Error)
				|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
				return Fail(OutError, Error.IsValid() ? Error : (Capture.Diagnostic.IsValid() ? Capture.Diagnostic
					: ProviderCondition(TEXT("string_table_capture_failed"), TEXT("The native StringTable state could not be captured."), Target, TEXT("/state_captured"))));
			auto Resources = MakeSemanticWorkspaceResourceTracker();
			FUeAgentCanonicalSnapshotPtr Snapshot;
			FAuthoringModelPtr Model;
			if (!TryBuildCanonicalAuthoringCapture(TEXT("ue.string_table.asset"), TEXT("llm_authoring"),
				Capture.Snapshot, Resources, Snapshot, Model, OutError)) return false;
			Capture.Snapshot.ReleaseNativeValues();
			Requests.Reset();
			Table = nullptr;
			return TryCoordinateSemanticWorkspaceImport(Document.SemanticWorkspace.ToSharedRef(),
				Model.ToSharedRef(), Snapshot.ToSharedRef(),
				Contract::FUeAgentDescriptorIdentity(TEXT("string_table.semantic_workspace"), 1, Model->GetSemanticFingerprint()),
				Target, Resources,
				{ProviderId, Target, Target, TEXT("string-table-semantic"),
				 TEXT("string_table_prepare_failed"), TEXT("string_table_apply_failed"), TEXT("string_table_readback_invalid"), TEXT("string_table_schema_invalid"),
				 [Target](TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error) {
					 UStringTable* Native = LoadStringTable(Target);
					 if (!Native)
					 {
						 Error = ProviderCondition(TEXT("string_table_target_missing"), TEXT("The StringTable target could not be loaded for semantic readback."),
							 Target, TEXT("/target_present"));
						 return false;
					 }
					 return BuildStringTableAuthoringReadRequests(*Native->GetStringTable(), Target, Requests, Error);
				 }},
				[&Target](const FString& RequestId, const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
					Composition::HjsonImport::FSemanticWorkspaceChangeProjection&& Projection, FRootPropertyApplyOutcome& Outcome)
				{
					if (!Projection.StructuralEdits.IsEmpty() || !Projection.RootObjectProperties.IsEmpty()
						|| !Projection.PropertyChangesByUnitId.IsEmpty())
					{
						Outcome.Diagnostic = UnsupportedProjection(TEXT("string_table_change_surface_unsupported"), Target, Projection);
						return false;
					}
					for (const auto& Change : Prepared.GetChangeSetDraft().GetChanges())
						if (Change.GetIdentity().GetSemanticId() != TEXT("string_table.entries"))
						{
							Outcome.Diagnostic = UnsupportedSemantic(TEXT("string_table_change_surface_unsupported"), Target,
								Change.GetIdentity().GetSemanticId(), TEXT("string_table.entries"));
							return false;
						}
					for (const auto& Unit : Prepared.GetWorkspace()->GetModel()->GetUnits())
					{
						if (Unit.GetSemanticTypeRef() != TEXT("string_table.entries")) continue;
						const auto Desired = MakeShared<FJsonObject>();
						for (const auto& Field : Unit.GetValues())
						{
							TSharedPtr<FJsonValue> Value;
							UE::UAI::Contract::FDiagnostic Error;
							if (!CanonicalJson::TryToJsonValue(Field.Value.Get(), Value, Error))
							{
								Outcome.Diagnostic = Error;
								if (Outcome.Diagnostic.SourceKind.IsEmpty()) Outcome.Diagnostic.SourceKind = TEXT("native_result");
								if (Outcome.Diagnostic.TargetPath.IsEmpty()) Outcome.Diagnostic.TargetPath = Target;
								return false;
							}
							Desired->SetField(Field.Key, Value);
						}
						UeAgentAuthoringDiagnostics::FScopedSourceLocations DesiredSources(&Desired.Get(), [&](const FString& Pointer) {
							return ResolveProjectedSource(Prepared, Projection, TEXT("string_table.entries"), Pointer);
						});
						return ApplyStringTableAuthoring(Target, RequestId, Desired, Outcome);
					}
					Outcome.Diagnostic = ProviderCondition(TEXT("string_table_entries_missing"),
						TEXT("The prepared semantic workspace contains no StringTable entries unit."), Target, TEXT("/entries_unit_present"));
					return false;
				},
				[&Target](FString& Payload, FString& Revision, FWorkspaceError& Error)
				{
					UStringTable* Reloaded = LoadStringTable(Target);
					if (!Reloaded) return Fail(Error, ProviderCondition(TEXT("string_table_reload_failed"),
						TEXT("The saved StringTable could not be reloaded for readback."), Target, TEXT("/target_present")));
					const auto State = BuildStringTableState(*Reloaded);
					Revision = PackageRevision(Target);
					return CanonicalObject(State, Payload, Error);
				}, OutPayload, OutError);
		}


		TSharedPtr<FJsonObject> BuildLocalizationState()
		{
			const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("type"), ULocalizationSettings::StaticClass()->GetPathName());
			TArray<TSharedPtr<FJsonValue>> Targets;
			if (ULocalizationTargetSet* Set = ULocalizationSettings::GetGameTargetSet())
			{
				for (const ULocalizationTarget* Target : Set->TargetObjects)
				{
					if (!Target) continue;
					const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("operation"), TEXT("update"));
					Item->SetStringField(TEXT("name"), Target->Settings.Name);
					TArray<TSharedPtr<FJsonValue>> Cultures;
					for (const FCultureStatistics& Culture : Target->Settings.SupportedCulturesStatistics)
					{
						Cultures.Add(MakeShared<FJsonValueString>(Culture.CultureName));
					}
					Item->SetArrayField(TEXT("cultures"), Cultures);
					if (Target->Settings.SupportedCulturesStatistics.IsValidIndex(Target->Settings.NativeCultureIndex))
					{
						Item->SetStringField(
							TEXT("native_culture"),
							Target->Settings.SupportedCulturesStatistics[
								Target->Settings.NativeCultureIndex].CultureName);
					}
					Targets.Add(MakeShared<FJsonValueObject>(Item));
				}
			}
			Targets.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
			{
				return A->AsObject()->GetStringField(TEXT("name"))
					< B->AsObject()->GetStringField(TEXT("name"));
			});
			State->SetArrayField(TEXT("targets"), Targets);
			return State;
		}

		FString LocalizationRevision()
		{
			FWorkspaceError Error;
			FString Json;
			return CanonicalObject(BuildLocalizationState(), Json, Error)
				? Sha256Text(Json) : FString(TEXT("unavailable"));
		}

		bool ImportLocalizationWorkspace(const FString& Target,
			const DomainAuthoring::FResolvedDomainHjsonDocument& Document, FString& OutPayload, FWorkspaceError& OutError)
		{
			using namespace DomainAuthoring;
			const auto Settings = CaptureLocalizationGameSettings();
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UeAgentAuthoringState::FCaptureResult Capture;
			UE::UAI::Contract::FDiagnostic Error;
			if (!RegisterLocalizationSettingsAuthoringState(Error) || !BuildLocalizationSettingsReadRequests(Settings, Requests, Error)
				|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture)) return Fail(OutError, Error.IsValid() ? Error : (Capture.Diagnostic.IsValid() ? Capture.Diagnostic
				: ProviderCondition(TEXT("localization_capture_failed"), TEXT("The native localization target settings could not be captured."),
					Target, TEXT("/state_captured"))));
			auto Resources = MakeSemanticWorkspaceResourceTracker();
			FUeAgentCanonicalSnapshotPtr Snapshot;
			FAuthoringModelPtr Model;
			if (!TryBuildCanonicalAuthoringCapture(TEXT("ue.localization_settings.config"), TEXT("llm_authoring"),
				Capture.Snapshot, Resources, Snapshot, Model, OutError)) return false;
			return TryCoordinateSemanticWorkspaceImport(Document.SemanticWorkspace.ToSharedRef(), Model.ToSharedRef(), Snapshot.ToSharedRef(),
				Contract::FUeAgentDescriptorIdentity(TEXT("localization_settings.semantic_workspace"), 1, Model->GetSemanticFingerprint()), Target, Resources,
				{ProviderId, Target, Target, TEXT("localization-settings"), TEXT("localization_prepare_failed"), TEXT("localization_apply_failed"),
				 TEXT("localization_readback_invalid"), TEXT("localization_schema_invalid"),
				 [](TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error) {
					 return BuildLocalizationSettingsReadRequests(CaptureLocalizationGameSettings(), Requests, Error);
				 }},
				[](const FString& RequestId, const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
					Composition::HjsonImport::FSemanticWorkspaceChangeProjection&& Projection, FRootPropertyApplyOutcome& Outcome)
				{
					if (!Projection.StructuralEdits.IsEmpty() || !Projection.RootObjectProperties.IsEmpty() || !Projection.PropertyChangesByUnitId.IsEmpty())
					{ Outcome.Diagnostic = UnsupportedProjection(TEXT("localization_change_surface_unsupported"),
						GetDefault<ULocalizationSettings>()->GetPathName(), Projection); return false; }
					for (const auto& Change : Prepared.GetChangeSetDraft().GetChanges())
						if (Change.GetIdentity().GetSemanticId() != TEXT("localization_settings.targets"))
						{ Outcome.Diagnostic = UnsupportedSemantic(TEXT("localization_change_surface_unsupported"),
							GetDefault<ULocalizationSettings>()->GetPathName(), Change.GetIdentity().GetSemanticId(), TEXT("localization_settings.targets")); return false; }
					for (const auto& Unit : Prepared.GetWorkspace()->GetModel()->GetUnits())
					{
						if (Unit.GetSemanticTypeRef() != TEXT("localization_settings.targets")) continue;
						const auto Desired = MakeShared<FJsonObject>();
						for (const auto& Field : Unit.GetValues())
						{
							TSharedPtr<FJsonValue> Value;
							if (!CanonicalJson::TryToJsonValue(Field.Value.Get(), Value, Outcome.Diagnostic))
							{
								if (Outcome.Diagnostic.SourceKind.IsEmpty()) Outcome.Diagnostic.SourceKind = TEXT("native_result");
								if (Outcome.Diagnostic.TargetPath.IsEmpty()) Outcome.Diagnostic.TargetPath = GetDefault<ULocalizationSettings>()->GetPathName();
								return false;
							}
							Desired->SetField(Field.Key, Value);
						}
						UeAgentAuthoringDiagnostics::FScopedSourceLocations DesiredSources(&Desired.Get(), [&](const FString& Pointer) {
							return ResolveProjectedSource(Prepared, Projection, TEXT("localization_settings.targets"), Pointer);
						});
						return ApplyLocalizationSettings(RequestId, Desired, Outcome);
					}
					Outcome.Diagnostic = ProviderCondition(TEXT("localization_targets_missing"),
						TEXT("The prepared semantic workspace contains no localization targets unit."),
						GetDefault<ULocalizationSettings>()->GetPathName(), TEXT("/targets_unit_present"));
					return false;
				},
				[](FString& Payload, FString& Revision, FWorkspaceError& Error)
				{
					TArray<FLocalizationTargetSettings> Persisted;
					UE::UAI::Contract::FDiagnostic ReadError;
					if (!ReadLocalizationSettingsFile(GetDefault<ULocalizationSettings>()->GetDefaultConfigFilename(), Persisted, ReadError))
						return Fail(Error, ReadError.IsValid() ? ReadError : ProviderCondition(TEXT("localization_config_read_failed"),
							TEXT("The published localization settings config could not be read."),
							GetDefault<ULocalizationSettings>()->GetPathName(), TEXT("/config_read_succeeded")));
					const auto State = CaptureLocalizationSettingsState(Persisted, &ReadError);
					if (!State) return Fail(Error, ReadError.IsValid() ? ReadError : ProviderCondition(TEXT("localization_settings_capture_failed"),
						TEXT("The persisted localization settings could not be captured."),
						GetDefault<ULocalizationSettings>()->GetPathName(), TEXT("/state_captured")));
					if (!CanonicalObject(State, Payload, Error)) return false;
					Revision = Sha256Text(Payload);
					return true;
				}, OutPayload, OutError);
		}

		bool ExportWorkspace(
			const TSharedPtr<FJsonObject>& Params,
			const FString& Target,
			const FString& RootType,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FString WorkspacePath;
			if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
				|| WorkspacePath.IsEmpty())
			{
				return Fail(OutError, ProviderFailure(TEXT("localization_export_path_required"),
					TEXT("Export requires a nonempty workspace_path string."), Target, TEXT("/workspace_path"),
					ParamFact(Params->TryGetField(TEXT("workspace_path"))), MakeShared<FJsonValueString>(TEXT("nonempty path string")), TEXT("command_input")));
			}
			if (RootType == UStringTable::StaticClass()->GetPathName())
			{
				UStringTable* Table = LoadStringTable(Target);
				if (!Table) return Fail(OutError, ProviderCondition(TEXT("string_table_target_missing"),
					TEXT("The StringTable target could not be loaded."), Target, TEXT("/target_present")));
				UE::UAI::Contract::FDiagnostic Error;
				TArray<UeAgentAuthoringState::FReadRequest> Requests;
				UeAgentAuthoringState::FCaptureResult Capture;
				if (!RegisterStringTableAuthoringState(Error)
					|| !BuildStringTableAuthoringReadRequests(*Table->GetStringTable(), Target, Requests, Error)
					|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
					return Fail(OutError, Error.IsValid() ? Error : (Capture.Diagnostic.IsValid() ? Capture.Diagnostic
					: ProviderCondition(TEXT("string_table_capture_failed"), TEXT("The native StringTable state could not be captured."), Target, TEXT("/state_captured"))));
				FString Profile = TEXT("llm_authoring");
				Params->TryGetStringField(TEXT("profile_id"), Profile);
				UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
				if (!TryExportCompositionWorkspace(TEXT("ue.string_table.asset"), Profile, Capture.Snapshot, WorkspacePath, Result))
				{
					OutError.SetFailure(Result.Diagnostic, Target);
					return false;
				}
				return CanonicalObject(Result.ToPublicJsonObject(), OutPayload, OutError);
			}
			else if (RootType == ULocalizationSettings::StaticClass()->GetPathName())
			{
				const auto Settings = CaptureLocalizationGameSettings();
				UE::UAI::Contract::FDiagnostic Error;
				TArray<UeAgentAuthoringState::FReadRequest> Requests;
				UeAgentAuthoringState::FCaptureResult Capture;
				if (!RegisterLocalizationSettingsAuthoringState(Error) || !BuildLocalizationSettingsReadRequests(Settings, Requests, Error)
					|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture)) return Fail(OutError, Error.IsValid() ? Error : (Capture.Diagnostic.IsValid() ? Capture.Diagnostic
				: ProviderCondition(TEXT("localization_capture_failed"), TEXT("The native localization target settings could not be captured."),
					Target, TEXT("/state_captured"))));
				FString Profile = TEXT("llm_authoring");
				Params->TryGetStringField(TEXT("profile_id"), Profile);
				UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
				if (!TryExportCompositionWorkspace(TEXT("ue.localization_settings.config"), Profile, Capture.Snapshot, WorkspacePath, Result))
				{
					OutError.SetFailure(Result.Diagnostic, Target);
					return false;
				}
				return CanonicalObject(Result.ToPublicJsonObject(), OutPayload, OutError);
			}
			else
			{
				return Fail(OutError, ProviderFailure(TEXT("localization_export_type_unsupported"),
					TEXT("The localization workspace exporter does not support the requested root type."), Target, TEXT("/root_type"),
					MakeShared<FJsonValueString>(RootType), SupportedRootTypes()));
			}
		}
	}

	bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& RootTypeIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		FWorkspaceError& OutError)
	{
		OutPayloadJson.Reset();
		bOutReadOnly = false;
		OutError.Reset();
		TSharedPtr<FJsonObject> Params;
		if (!ParseParams(CanonicalParamsJson, Params, OutError)) return false;
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| Target != TargetIdentity)
		{
			return Fail(OutError, ProviderFailure(TEXT("localization_hjson_target_mismatch"),
				TEXT("The target parameter must match the resolved localization target."), TargetIdentity, TEXT("/target"),
				ParamFact(Params->TryGetField(TEXT("target"))), MakeShared<FJsonValueString>(TargetIdentity), TEXT("command_input")));
		}
		const bool bStringTable = RootTypeIdentity == UStringTable::StaticClass()->GetPathName();
		const bool bLocalization = RootTypeIdentity == ULocalizationSettings::StaticClass()->GetPathName();
		if (!bStringTable && !bLocalization)
		{
			return Fail(OutError, ProviderFailure(TEXT("localization_hjson_type_unsupported"),
				TEXT("The localization provider does not support the resolved root type."), TargetIdentity, TEXT("/root_type"),
				MakeShared<FJsonValueString>(RootTypeIdentity), SupportedRootTypes()));
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			FString ExpectedRevision;
			if (!Params->TryGetStringField(TEXT("expected_revision"), ExpectedRevision))
			{
				auto Error = ProviderFailure(TEXT("localization_hjson_expected_revision_required"),
					TEXT("Import requires an expected_revision string."), Target, TEXT("/expected_revision"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("revision string")), TEXT("command_input"));
				Error.ActualEvidence = Params->TryGetField(TEXT("expected_revision"));
				return Fail(OutError, MoveTemp(Error));
			}
			DomainAuthoring::FResolvedDomainHjsonDocument Document;
			if (!DomainAuthoring::TryDecodeResolvedDocument(Target, CanonicalParamsJson,
				{ProviderId, bStringTable ? StringTableSchema : LocalizationSchema, RootTypeIdentity},
				Document, OutError, bStringTable ? DomainAuthoring::EDomainHjsonDocumentProjection::Ast
					: DomainAuthoring::EDomainHjsonDocumentProjection::Json)) return false;
			UE::UAI::HjsonWorkspace::DomainAuthoring::FScopedDomainHjsonDiagnosticSource DiagnosticSource(Document, OutError);
			if (Document.bSemanticWorkspaceDirectory)
				return bStringTable ? ImportStringTableWorkspace(Target, Document, OutPayloadJson, OutError)
					: ImportLocalizationWorkspace(Target, Document, OutPayloadJson, OutError);
			return bStringTable
				? ImportStringTable(Target, Document, OutPayloadJson, OutError)
				: Fail(OutError, ProviderCondition(TEXT("localization_semantic_workspace_required"),
					TEXT("Localization settings must be imported through an exported semantic workspace directory."),
					Target, TEXT("/semantic_workspace_directory")));
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			bOutReadOnly = true;
			return ExportWorkspace(Params, Target, RootTypeIdentity, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.compound"))
		{
			const TSharedPtr<FJsonObject>* Input = nullptr;
			FString Capability;
			if (!Params->TryGetObjectField(TEXT("input"), Input) || !Input || !Input->IsValid())
				return Fail(OutError, ProviderFailure(TEXT("localization_content_graph_compound_input_invalid"),
					TEXT("The localization compound command requires an input object."), Target, TEXT("/input"),
					ParamFact(Params->TryGetField(TEXT("input"))), MakeShared<FJsonValueString>(TEXT("object")), TEXT("command_input")));
			if (!(*Input)->TryGetStringField(TEXT("localization_capability"), Capability))
				return Fail(OutError, ProviderFailure(TEXT("localization_content_graph_compound_input_invalid"),
					TEXT("The compound input must declare a localization_capability string."), Target, TEXT("/input/localization_capability"),
					ParamFact((*Input)->TryGetField(TEXT("localization_capability"))), MakeShared<FJsonValueString>(TEXT("string")), TEXT("command_input")));
			if (!DomainAdapters::ContentGraphRead::IsExternalLocalizationContentGraphRoute(TEXT("content_graph.read.compound"), Capability))
				return Fail(OutError, ProviderFailure(TEXT("localization_content_graph_compound_input_invalid"),
					TEXT("The capability is not a supported external localization read operation."), Target, TEXT("/input/localization_capability"),
					MakeShared<FJsonValueString>(Capability), MakeShared<FJsonValueString>(TEXT("supported localization read capability")), TEXT("command_input")));
			TSharedPtr<FJsonObject> CapabilityParameters = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject>* Supplied = nullptr;
			if ((*Input)->TryGetObjectField(TEXT("parameters"), Supplied)
				&& Supplied && Supplied->IsValid())
			{
				CapabilityParameters = MakeShared<FJsonObject>(**Supplied);
			}
			if (!CapabilityParameters->HasField(TEXT("target")))
			{
				CapabilityParameters->SetStringField(TEXT("target"), Target);
			}
			TSharedPtr<FJsonObject> Observation;
			UE::UAI::Contract::FDiagnostic NativeError;
			const bool bExecuted = DomainAdapters::ContentGraphRead::TryExecuteExternalLocalizationContentGraph(
				Capability, CapabilityParameters, Observation, NativeError);
			if (!bExecuted || !Observation.IsValid())
			{
				if (NativeError.IsEmpty()) NativeError = ProviderCondition(TEXT("localization_content_graph_compound_failed"),
					bExecuted ? TEXT("The localization capability returned no observation.") : TEXT("The native localization capability failed."),
					Target, bExecuted ? TEXT("/observation_present") : TEXT("/capability_executed"));
				if (NativeError.SourceKind.IsEmpty()) NativeError.SourceKind = TEXT("native_result");
				if (NativeError.TargetPath.IsEmpty()) NativeError.TargetPath = Target;
				if (NativeError.CandidateName.IsEmpty()) NativeError.CandidateName = Capability;
				return Fail(OutError, MoveTemp(NativeError));
			}
			const TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("command_id"), CommandId);
			Response->SetStringField(TEXT("provider_id"), ProviderId);
			Response->SetStringField(TEXT("capability"), Capability);
			Response->SetBoolField(TEXT("read_only"), true);
			Response->SetObjectField(TEXT("query_result"), Observation);
			bOutReadOnly = true;
			return CanonicalObject(Response, OutPayloadJson, OutError);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			const TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("target"), Target);
			Response->SetStringField(TEXT("root_type"), RootTypeIdentity);
			Response->SetBoolField(TEXT("complete"), true);
			Response->SetBoolField(TEXT("has_more"), false);
			if (bStringTable)
			{
				UStringTable* Table = LoadStringTable(Target);
				if (!Table) return Fail(OutError, ProviderCondition(TEXT("string_table_target_missing"),
					TEXT("The StringTable target could not be loaded for the query."), Target, TEXT("/target_present")));
				Response->SetStringField(TEXT("revision"), PackageRevision(Target));
				Response->SetObjectField(TEXT("query_result"), BuildStringTableState(*Table));
			}
			else
			{
				Response->SetStringField(TEXT("revision"), LocalizationRevision());
				Response->SetObjectField(TEXT("query_result"), BuildLocalizationState());
			}
			bOutReadOnly = true;
			return CanonicalObject(Response, OutPayloadJson, OutError);
		}
		auto Error = ProviderFailure(TEXT("localization_hjson_command_unsupported"),
			TEXT("The localization provider does not support this command."), Target, TEXT("/command_id"),
			MakeShared<FJsonValueString>(CommandId), MakeShared<FJsonValueString>(TEXT("content_graph.*")), TEXT("command_input"));
		Error.Source.JsonPath = TEXT("/command_id");
		return Fail(OutError, MoveTemp(Error));
	}

}
