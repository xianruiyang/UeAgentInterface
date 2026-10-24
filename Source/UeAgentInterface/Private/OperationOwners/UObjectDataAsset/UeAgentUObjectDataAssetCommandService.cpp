#include "OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h"

#include "Dom/JsonValue.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntimeResolvers.h"
#include "Composition/HjsonImport/UeAgentReflectionRootPropertyImport.h"
#include "Core/ArtifactPublisher/UeAgentVerifiedExport.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/BindingGateway/Operation/UeAgentOperationLifecycleService.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonImportInitialization.h"
#include "Core/HjsonWorkspace/SemanticModel/UeAgentUnifiedHjsonSemanticModel.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/HjsonWorkspace/UeAgentSnapshotComposition.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadNative.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "DomainAdapters/UObjectDataAsset/UeAgentRecursiveDataAsset.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"
#include "Dom/JsonObject.h"
#include "Factories/DataAssetFactory.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Core/AssetDurable/UeAgentPackageSave.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringPackageReadback.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::OperationOwners::UObjectDataAsset
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::BindingGateway::Operation;
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		const FString RootType =
			TEXT("/Script/UeAgentInterface.UeAgentRecursiveDataAsset");
		const FString OwnedType =
			TEXT("/Script/UeAgentInterface.UeAgentRecursiveOwnedObject");
		const FString ContractId =
			TEXT("uai.uobject_dataasset.recursive/1");
		const FString RootInitializationContractId =
			TEXT("uai.init.uobject_dataasset.root/1");
		const FString OwnedInitializationContractId =
			TEXT("uai.init.uobject_dataasset.owned/1");
		const FString DomainProviderId =
			TEXT("uobject-dataasset-domain-provider");
		const FString ReflectionProfileId = TEXT("recursive_dataasset");
		FString FailureAfterNode;
		bool bFailFinalProjection = false;
		TFunction<void(const UObject&)> FinalProjectionObserver;

		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		FString Sha256Bytes(const TArray<uint8>& Bytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const unsigned char*>(Bytes.GetData()),
				static_cast<size_t>(Bytes.Num()),
				Digest);
			FString Result(TEXT("sha256:"));
			for (const uint8 Byte : Digest)
			{
				Result += FString::Printf(TEXT("%02x"), Byte);
			}
			return Result;
		}

		FString Sha256(const FString& Value)
		{
			FTCHARToUTF8 Utf8(*Value);
			TArray<uint8> Bytes;
			Bytes.Append(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length());
			return Sha256Bytes(Bytes);
		}

        bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
        { OutError = MoveTemp(Diagnostic); if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject; return false; }
        bool FailWithTerminalFacts(FUeAgentUObjectDataAssetCommandError& OutError, const UE::UAI::Contract::FDiagnostic& Diagnostic,
            FString TerminalStatus = TEXT("rejected"), bool bMutationStarted = false, bool bRetrySafe = true)
        {
            OutError = Diagnostic;
            OutError.TerminalStatus = MoveTemp(TerminalStatus);
            OutError.MutationStarted = bMutationStarted;
            OutError.bRetrySafe = bRetrySafe;
            return false;
        }

		UE::UAI::Contract::FDiagnostic CommandInputFailure(
			const TCHAR* Code, const FString& Reason, const TSharedPtr<FJsonObject>& Params,
			const FString& Field, const FString& Expected, const FString& Target = FString(),
			const bool bPrivateValue = false)
		{
			const auto* Value = Params.IsValid() ? Params->Values.Find(Field) : nullptr;
			auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(Code,
				Field.IsEmpty() ? FString() : TEXT("/") + Field,
				MakeShared<FJsonValueString>(Expected), Value ? *Value : nullptr,
				TEXT("input_decode"), Reason);
			Diagnostic.ActualValuePresent = Value && Value->IsValid();
			Diagnostic.ExpectedValuePresent = true;
			Diagnostic.SourceKind = TEXT("command_input");
			Diagnostic.TargetPath = Target;
			if (bPrivateValue)
			{
				Diagnostic.ActualEvidence = MoveTemp(Diagnostic.ActualValue);
			}
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic NativeConditionFailure(
			const TCHAR* Code, const FString& Reason, const TCHAR* Condition,
			const bool bActual, const bool bExpected, const FString& Target = FString(),
			const FString& Stage = TEXT("command_execute"))
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, Stage);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = Condition;
			Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(bActual);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(bExpected);
			Diagnostic.TargetPath = Target;
			return Diagnostic;
		}


		bool FailUnresolvedTarget(FUeAgentUObjectDataAssetCommandError& OutError,
			const FString& Code, const FString& Target, const FString& WorkspaceSource = FString())
		{
			UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
			Diagnostic.Code = Code;
			Diagnostic.Stage = TEXT("target_resolution");
			Diagnostic.TargetPath = Target;
			Diagnostic.Source.JsonPath = TEXT("/target");
			Diagnostic.Source.WorkspaceSource = WorkspaceSource;
			Diagnostic.SourceKind = TEXT("command_input");
			Diagnostic.PublicMessage = TEXT("The requested asset cannot be loaded at this target path.");
			Diagnostic.AssetApplied = false;
			Diagnostic.RetryImport = false;
			Diagnostic.RecoveryAction = TEXT("Locate the current asset with query.content.search or asset.registry_record. After a move or rename, export its current path to a new workspace and reapply the preserved edits; do not retarget the old workspace.");
			return FailWithTerminalFacts(OutError, Diagnostic);
		}

		bool ParseObjectPath(
			const FString& Target,
			FString& OutPackageName,
			FString& OutAssetName)
		{
			const FString Normalized = Normalize(Target);
			int32 Dot = INDEX_NONE;
			if (!Normalized.FindLastChar(TEXT('.'), Dot)
				|| Dot <= 0
				|| Dot + 1 >= Normalized.Len())
			{
				return false;
			}
			OutPackageName = Normalized.Left(Dot);
			OutAssetName = Normalized.Mid(Dot + 1);
			return FPackageName::IsValidLongPackageName(OutPackageName)
				&& !OutAssetName.IsEmpty()
				&& FName::IsValidXName(
					OutAssetName,
					INVALID_OBJECTNAME_CHARACTERS);
		}

		bool TargetExists(const FString& Target)
		{
			if (StaticFindObject(UObject::StaticClass(), nullptr, *Target))
			{
				return true;
			}
			FString PackageName;
			FString AssetName;
			return ParseObjectPath(Target, PackageName, AssetName)
				&& FPackageName::DoesPackageExist(PackageName);
		}

		FUeAgentStateRevision TargetRevision(const FString& Target)
		{
			return TargetExists(Target)
				? FUeAgentStateRevision(
					TEXT("uobject-dataasset"), TEXT("present"), 1)
				: FUeAgentStateRevision(
					TEXT("uobject-dataasset"), TEXT("absent"), 0);
		}

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker();
		}

		FSemanticUnit Unit(
			FString UnitId,
			const ESemanticUnitKind Kind,
			FString Owner,
			FString Role)
		{
			return FSemanticUnit(
				MoveTemp(UnitId),
				Kind,
				MoveTemp(Owner),
				TEXT("uai.uobject_dataasset.recursive.semantic/1"),
				MoveTemp(Role),
				FString(),
				{},
				{},
				{},
				FString(),
				FString(),
				{},
				{TEXT("state"), TEXT("minimal_create"), TEXT("local_edit")},
				TEXT("strict_v2:t13:current-design"));
		}

		bool TryBuildSemanticModel(
			FUnifiedHjsonSemanticModelPtr& OutModel,
			FWorkspaceError& OutError)
		{
			const TArray<FString> Ids{
				TEXT("target"), TEXT("property"), TEXT("member"),
				TEXT("structure"), TEXT("item"), TEXT("endpoint"),
				TEXT("relation"), TEXT("order"), TEXT("layout"),
				TEXT("temporal"), TEXT("spatial"), TEXT("external"),
			};
			TArray<FSemanticUnit> Units{
				Unit(Ids[0], ESemanticUnitKind::TargetState, FString(), TEXT("target")),
				Unit(Ids[1], ESemanticUnitKind::PropertySet, Ids[0], TEXT("property_set")),
				Unit(Ids[2], ESemanticUnitKind::EntityState, Ids[0], TEXT("member")),
				Unit(Ids[3], ESemanticUnitKind::StructureState, Ids[0], TEXT("structure")),
				Unit(Ids[4], ESemanticUnitKind::ItemState, Ids[3], TEXT("item")),
				Unit(Ids[5], ESemanticUnitKind::EntityState, Ids[4], TEXT("endpoint")),
				Unit(Ids[6], ESemanticUnitKind::RelationSet, Ids[3], TEXT("relation")),
				Unit(Ids[7], ESemanticUnitKind::OrderingState, Ids[3], TEXT("order")),
				Unit(Ids[8], ESemanticUnitKind::LayoutState, Ids[3], TEXT("layout")),
				Unit(Ids[9], ESemanticUnitKind::PropertySet, Ids[0], TEXT("temporal_contract")),
				Unit(Ids[10], ESemanticUnitKind::PropertySet, Ids[0], TEXT("spatial_contract")),
				Unit(Ids[11], ESemanticUnitKind::ExternalPayloadState, Ids[0], TEXT("external_input")),
			};
			FUeAgentResourceTracker CoreResources = MakeResources();
			FAuthoringModelPtr Core;
			if (!TryCompileAuthoringModel(
					TEXT("uai.uobject_dataasset.recursive.hjson"),
					1,
					TEXT("unified"),
					TEXT("strict-v2-r25"),
					MoveTemp(Units),
					{Ids[0]},
					CoreResources,
					Core,
					OutError)
				|| !Core.IsValid())
			{
				return false;
			}
			TMap<EUnifiedHjsonCompositionKind, TArray<FString>> Compositions{
				{EUnifiedHjsonCompositionKind::Target, {Ids[0]}},
				{EUnifiedHjsonCompositionKind::PropertySet, {Ids[1]}},
				{EUnifiedHjsonCompositionKind::MemberSet, {Ids[2]}},
				{EUnifiedHjsonCompositionKind::Structure, {Ids[3]}},
				{EUnifiedHjsonCompositionKind::Item, {Ids[4]}},
				{EUnifiedHjsonCompositionKind::Endpoint, {Ids[5]}},
				{EUnifiedHjsonCompositionKind::Relation, {Ids[6]}},
				{EUnifiedHjsonCompositionKind::Order, {Ids[7]}},
				{EUnifiedHjsonCompositionKind::Layout, {Ids[8]}},
				{EUnifiedHjsonCompositionKind::TemporalContract, {Ids[9]}},
				{EUnifiedHjsonCompositionKind::SpatialContract, {Ids[10]}},
				{EUnifiedHjsonCompositionKind::ExternalInput, {Ids[11]}},
			};
			FUeAgentResourceTracker ModelResources = MakeResources();
			return TryCreateUnifiedHjsonSemanticModel(
				Core.ToSharedRef(),
				EUnifiedHjsonAuthoringIntent::MinimalCreate,
				MoveTemp(Compositions),
				ModelResources,
				OutModel,
				OutError);
		}

		struct FNodeValues final
		{
			TSharedPtr<FJsonObject> Properties;
			TSharedPtr<FJsonObject> ExplicitValues;
			DomainAuthoring::FResolvedDomainHjsonDocument Source;
			FString ValuesPointer;
			TMap<FString, FString> InputPointers;
			TMap<FString, FString> PropertyPointers;

			FString ResolvePointer(const FString& Pointer) const
			{
				const FString* Best = nullptr;
				int32 PrefixLength = -1;
				for (const auto& Binding : InputPointers)
					if ((Pointer == Binding.Key || Pointer.StartsWith(Binding.Key + TEXT("/"))) && Binding.Key.Len() > PrefixLength)
					{ Best = &Binding.Value; PrefixLength = Binding.Key.Len(); }
				return Best ? *Best + Pointer.Mid(PrefixLength) : ValuesPointer;
			}

			void Locate(UE::UAI::Contract::FDiagnostic& Diagnostic) const
			{
				for (auto& Child : Diagnostic.Diagnostics) Locate(Child);
				if (Diagnostic.PublicCause)
				{
					auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Diagnostic.PublicCause);
					Locate(*Cause); Diagnostic.PublicCause = MoveTemp(Cause);
				}
				if (!Diagnostic.Source.SourceFile.IsEmpty() && Diagnostic.Source.Line > 0) return;
				const FString NativePointer = !Diagnostic.Source.InternalJsonPath.IsEmpty()
					? Diagnostic.Source.InternalJsonPath : Diagnostic.Source.JsonPath;
				FString Pointer = ResolvePointer(NativePointer);
				if (Pointer == ValuesPointer)
				{
					if (const auto* Property = PropertyPointers.Find(Diagnostic.PropertyPath)) Pointer = *Property;
					else Diagnostic.SourceKind = TEXT("related_input");
				}
				Diagnostic.SemanticPointer = Pointer;
				Source.LocateDiagnostic(Diagnostic);
			}
		};

		struct FNodeSpec final
		{
			FString Id;
			FString Name;
			FString Type;
			FString OwnerId;
			FString InputPointer;
			FNodeValues Values;
			bool bRoot = false;
		};

		struct FReferenceSpec final
		{
			FString From;
			FString Field;
			FString To;
			FString InputPointer;
		};

		struct FPreparedImport final
		{
			FString Target;
			FString PackageName;
			FString AssetName;
			FString AuthorizationId;
			FString SourceFingerprint;
			FString SemanticFingerprint;
			EImportSourceCarrier SourceCarrier = EImportSourceCarrier::Inline;
			TArray<FNodeSpec> Nodes;
			TMap<FString, int32> NodeIndexById;
			TArray<FInitializationEdge> OwnershipEdges;
			TArray<FReferenceSpec> References;
			FImportInitializationPlanPtr InitializationPlan;
			DomainAuthoring::FResolvedDomainHjsonDocument Source;

			void Locate(UE::UAI::Contract::FDiagnostic& Diagnostic) const
			{
				for (auto& Child : Diagnostic.Diagnostics) Locate(Child);
				if (Diagnostic.PublicCause)
				{
					auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Diagnostic.PublicCause);
					Locate(*Cause); Diagnostic.PublicCause = MoveTemp(Cause);
				}
				if (Diagnostic.SemanticPointer.IsEmpty() && Diagnostic.Source.JsonPath.IsEmpty())
					if (const auto* Index = NodeIndexById.Find(Diagnostic.SubjectId))
					{ Diagnostic.SemanticPointer = Nodes[*Index].InputPointer; Diagnostic.SourceKind = TEXT("related_input"); }
				Source.LocateDiagnostic(Diagnostic);
			}
		};

		UE::UAI::Contract::FDiagnostic InputFailure(const TCHAR* Code, const FString& Reason,
			const FHjsonNode& Node, const FString& Field = FString(), const FString& Expected = FString())
		{
			const FHjsonNode* Value = Field.IsEmpty() ? &Node : Node.Get(Field);
			const auto& Provenance = (Value ? *Value : Node).GetProvenance();
			TSharedPtr<FJsonValue> Actual;
			if (Value)
			{
				if (Value->GetKind() == EHjsonNodeKind::String) Actual = MakeShared<FJsonValueString>(Value->GetText());
				else if (Value->GetKind() == EHjsonNodeKind::Boolean) Actual = MakeShared<FJsonValueBoolean>(Value->GetBoolean());
				else if (Value->GetKind() == EHjsonNodeKind::Integer || Value->GetKind() == EHjsonNodeKind::Decimal) Actual = MakeShared<FJsonValueNumber>(FCString::Atod(*Value->GetLexeme()));
				else if (Value->GetKind() == EHjsonNodeKind::Null) Actual = MakeShared<FJsonValueNull>();
				else Actual = MakeShared<FJsonValueString>(Value->GetKind() == EHjsonNodeKind::Array ? TEXT("array") : TEXT("object"));
			}
			const FString Pointer = Value ? Provenance.GetJsonPointer() : Provenance.GetJsonPointer()
				+ TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
			TSharedPtr<FJsonValue> ExpectedValue;
			if (!Expected.IsEmpty()) ExpectedValue = MakeShared<FJsonValueString>(Expected);
			auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Pointer,
				ExpectedValue, Actual, TEXT("input_decode"), Reason);
			Diagnostic.Source.SourceFile = Provenance.GetSourceUnit();
			Diagnostic.Source.SourceUnit = Provenance.GetSourceUnit();
			Diagnostic.Source.JsonPath = Provenance.GetJsonPointer();
			Diagnostic.Source.InternalJsonPath = Pointer;
			Diagnostic.Source.Line = Provenance.GetLine();
			Diagnostic.Source.Column = Provenance.GetColumn();
			Diagnostic.SourceKind = Value ? TEXT("authored_value") : TEXT("related_input");
			return Diagnostic;
		}

		bool ReadString(
			const FHjsonNode& Object,
			const TCHAR* Key,
			FString& Out,
			FUeAgentUObjectDataAssetCommandError& OutError,
			const bool bRequired = true)
		{
			const FHjsonNode* Value = Object.Get(Key);
			if (!Value)
			{
				return !bRequired || Fail(OutError, InputFailure(TEXT("uobject_dataasset_field_required"), TEXT("A required field is missing."), Object, Key, TEXT("non-empty string")), Key);
			}
			if (Value->GetKind() != EHjsonNodeKind::String)
			{
				return Fail(OutError, InputFailure(TEXT("uobject_dataasset_field_type_invalid"), TEXT("This field must be a string."), Object, Key, TEXT("string")), Key);
			}
			Out = Normalize(Value->GetText());
			return !Out.IsEmpty() || !bRequired || Fail(OutError, InputFailure(TEXT("uobject_dataasset_field_required"), TEXT("This required string is empty."), Object, Key, TEXT("non-empty string")), Key);
		}

		bool ReadValues(
			const FHjsonNode& Node,
			const bool bRoot,
			FNodeValues& OutValues,
			FUeAgentUObjectDataAssetCommandError& OutError)
		{
			const FHjsonNode* Values = Node.Get(TEXT("values"));
			OutValues.ValuesPointer = Node.GetProvenance().GetJsonPointer() + TEXT("/values");
			if (!Values)
			{
				return true;
			}
			if (Values->GetKind() != EHjsonNodeKind::Object)
			{
				return Fail(OutError, InputFailure(TEXT("uobject_dataasset_values_invalid"), TEXT("Node values must be an object."), *Values, FString(), TEXT("object")));
			}
			const TMap<FString, FString> PropertyNames = bRoot
				? TMap<FString, FString>{
					{TEXT("label"), TEXT("Label")},
					{TEXT("explicit_number"), TEXT("ExplicitNumber")},
					{TEXT("numbers"), TEXT("Numbers")},
					{TEXT("named_numbers"), TEXT("NamedNumbers")}}
				: TMap<FString, FString>{{TEXT("label"), TEXT("Label")}};
			TArray<TSharedPtr<FJsonValue>> Properties;
			OutValues.ExplicitValues = MakeShared<FJsonObject>();
			for (const TPair<FString, FHjsonNodeRef>& Entry
				: Values->GetEntries())
			{
				const FString* PropertyName = PropertyNames.Find(Entry.Key);
				if (!PropertyName)
				{
					return Fail(OutError, InputFailure(TEXT("uobject_dataasset_value_unsupported"), TEXT("This value is not supported by the node type."), *Values, Entry.Key), Entry.Key);
				}
				FUeAgentResourceTracker Resources = MakeResources();
				TSharedPtr<FJsonValue> Value;
				FWorkspaceError WorkspaceError;
				if (!TryProjectHjsonNodeToJsonValue(
						*Entry.Value, Resources, Value, WorkspaceError)
					|| !Value.IsValid())
				{
					return Fail(OutError, WorkspaceError.HasErrors() ? WorkspaceError.GetDiagnostic()
						: InputFailure(TEXT("uobject_dataasset_value_type_invalid"), TEXT("This authored value could not be projected."), *Values, Entry.Key), Entry.Key);
				}
				OutValues.ExplicitValues->SetField(Entry.Key, Value);
				const FString DescriptorPointer = TEXT("/properties/") + ::LexToString(Properties.Num());
				const FString InputPointer = Entry.Value->GetProvenance().GetJsonPointer();
				OutValues.InputPointers.Add(DescriptorPointer, InputPointer);
				OutValues.InputPointers.Add(DescriptorPointer + TEXT("/value"), InputPointer);
				OutValues.PropertyPointers.Add(*PropertyName, InputPointer);
				if (Entry.Key == TEXT("numbers"))
				{
					OutValues.InputPointers.Add(DescriptorPointer + TEXT("/value/items"), InputPointer);
					const TSharedPtr<FJsonObject> Container = MakeShared<FJsonObject>();
					Container->SetField(TEXT("items"), Value);
					Value = MakeShared<FJsonValueObject>(Container);
				}
				else if (Entry.Key == TEXT("named_numbers"))
				{
					const TSharedPtr<FJsonObject> Input =
						Value->Type == EJson::Object ? Value->AsObject() : nullptr;
					if (!Input.IsValid())
					{
						return Fail(OutError, InputFailure(TEXT("uobject_dataasset_value_type_invalid"), TEXT("Named numbers must be an object keyed by name."), *Values, Entry.Key, TEXT("object")), Entry.Key);
					}
					TArray<TSharedPtr<FJsonValue>> Entries;
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Input->Values)
					{
						const FString MapPointer = DescriptorPointer + TEXT("/value/entries/") + ::LexToString(Entries.Num());
						const FString AuthoredPointer = InputPointer + TEXT("/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
						OutValues.InputPointers.Add(MapPointer, AuthoredPointer);
						OutValues.InputPointers.Add(MapPointer + TEXT("/key"), AuthoredPointer);
						OutValues.InputPointers.Add(MapPointer + TEXT("/value"), AuthoredPointer);
						const TSharedPtr<FJsonObject> MapEntry = MakeShared<FJsonObject>();
						MapEntry->SetStringField(TEXT("key"), Pair.Key);
						MapEntry->SetField(TEXT("value"), Pair.Value);
						Entries.Add(MakeShared<FJsonValueObject>(MapEntry));
					}
					const TSharedPtr<FJsonObject> Container = MakeShared<FJsonObject>();
					Container->SetArrayField(TEXT("entries"), MoveTemp(Entries));
					Value = MakeShared<FJsonValueObject>(Container);
				}
				const TSharedPtr<FJsonObject> Descriptor = MakeShared<FJsonObject>();
				Descriptor->SetStringField(TEXT("property_path"), *PropertyName);
				Descriptor->SetField(TEXT("value"), Value);
				Properties.Add(MakeShared<FJsonValueObject>(Descriptor));
			}
			OutValues.Properties = MakeShared<FJsonObject>();
			OutValues.Properties->SetStringField(
				TEXT("schema"), TEXT("uai.uobject_properties.v2"));
			OutValues.Properties->SetArrayField(TEXT("properties"), MoveTemp(Properties));
			return true;
		}

		class FPreflight final : public IImportInitializationPreflight
		{
		public:
			FPreflight(FString InTarget, FString InAuthorizationId)
				: Target(MoveTemp(InTarget))
				, AuthorizationId(MoveTemp(InAuthorizationId))
			{
			}

			virtual bool TryReadCurrentTargetRevision(
				const FString& TargetIdentity,
				FUeAgentStateRevision& OutRevision,
				FWorkspaceError& OutError) override
			{
				if (TargetIdentity != Target)
				{
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("uobject_dataasset_target_identity_changed"),
						TEXT("The requested target differs from the target captured for import preflight."), TEXT("preflight"));
					Detail.TargetPath = Target;
					Detail.ActualValue = MakeShared<FJsonValueString>(TargetIdentity);
					Detail.ExpectedValue = MakeShared<FJsonValueString>(Target);
					OutError.SetFailure(MoveTemp(Detail));
					return false;
				}
				OutRevision = TargetRevision(Target);
				return true;
			}

			virtual bool TryResolveCandidate(
				const FString& CandidateName,
				const FString& BindingRevision,
				FString& OutResolvedName,
				FWorkspaceError& OutError) override
			{
				OutResolvedName = CandidateName;
				return true;
			}

			virtual bool SupportsChange(
				const FImportChange& Change,
				FWorkspaceError& OutError) override
			{
				return Change.IsValid();
			}

			virtual bool TryAuthorizeImpact(
				const TArray<FString>& ImpactScopes,
				const FString& PlanFingerprint,
				const FString& InAuthorizationId,
				FWorkspaceError& OutError) override
			{
				if (InAuthorizationId != AuthorizationId
					|| AuthorizationId.IsEmpty()
					|| ImpactScopes != TArray<FString>{Target}
					|| !PlanFingerprint.StartsWith(TEXT("sha256:")))
				{
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("uobject_dataasset_impact_unauthorized"),
						AuthorizationId.IsEmpty() ? TEXT("Import preflight has no impact authorization.")
						: InAuthorizationId != AuthorizationId ? TEXT("The supplied impact authorization does not match this preflight.")
						: ImpactScopes != TArray<FString>{Target} ? TEXT("The requested impact scopes differ from the authorized target scope.")
						: TEXT("The import plan fingerprint is missing or invalid."), TEXT("preflight"));
					Detail.TargetPath = Target;
					if (ImpactScopes != TArray<FString>{Target})
					{
						TArray<TSharedPtr<FJsonValue>> Scopes;
						for (const FString& Scope : ImpactScopes) Scopes.Add(MakeShared<FJsonValueString>(Scope));
						Detail.ActualValue = MakeShared<FJsonValueArray>(MoveTemp(Scopes));
						Detail.ExpectedValue = MakeShared<FJsonValueString>(Target);
					}
					Detail.InternalContext = PlanFingerprint;
					OutError.SetFailure(MoveTemp(Detail));
					return false;
				}
				return true;
			}

		private:
			FString Target;
			FString AuthorizationId;
		};

		bool TryPrepareImport(
			const TSharedPtr<FJsonObject>& Params,
			FPreparedImport& OutPrepared,
			FUeAgentUObjectDataAssetCommandError& OutError)
		{
			OutPrepared = {};
			ON_SCOPE_EXIT { if (OutError.IsValid()) OutPrepared.Locate(OutError); };
			FString ExpectedRevision;
			if (!Params.IsValid())
			{
				return Fail(OutError, CommandInputFailure(TEXT("uobject_dataasset_import_params_invalid"),
					TEXT("Data asset import parameters must be an object."), Params, FString(), TEXT("object")));
			}
			if (!Params->TryGetStringField(TEXT("target"), OutPrepared.Target))
			{
				return Fail(OutError, CommandInputFailure(TEXT("uobject_dataasset_import_params_invalid"),
					TEXT("The target parameter must be an asset path string."), Params, TEXT("target"), TEXT("asset object path string")));
			}
			if (!Params->TryGetStringField(TEXT("expected_revision"), ExpectedRevision))
			{
				return Fail(OutError, CommandInputFailure(TEXT("uobject_dataasset_import_params_invalid"),
					TEXT("The expected_revision parameter must be a string."), Params, TEXT("expected_revision"), TEXT("string"), OutPrepared.Target, true));
			}
			if (!Params->TryGetStringField(TEXT("authorization_id"), OutPrepared.AuthorizationId))
			{
				return Fail(OutError, CommandInputFailure(TEXT("uobject_dataasset_import_params_invalid"),
					TEXT("The authorization_id parameter must be a string."), Params, TEXT("authorization_id"), TEXT("string"), OutPrepared.Target, true));
			}
			ExpectedRevision = Normalize(MoveTemp(ExpectedRevision));
			if (ExpectedRevision.IsEmpty())
			{
				return Fail(OutError, CommandInputFailure(TEXT("uobject_dataasset_expected_revision_invalid"),
					TEXT("The expected_revision parameter must not be empty."), Params, TEXT("expected_revision"), TEXT("non-empty string"), OutPrepared.Target, true));
			}
			if (!ParseObjectPath(
					OutPrepared.Target,
					OutPrepared.PackageName,
					OutPrepared.AssetName))
			{
				return Fail(OutError, CommandInputFailure(TEXT("uobject_dataasset_target_invalid"),
					TEXT("The target must be a valid asset object path."), Params, TEXT("target"), TEXT("valid asset object path"), OutPrepared.Target));
			}
			if (Params->HasField(TEXT("carrier"))
				|| Params->HasField(TEXT("hjson_document")))
			{
				const FString Field = Params->HasField(TEXT("carrier")) ? TEXT("carrier") : TEXT("hjson_document");
				auto Diagnostic = CommandInputFailure(TEXT("content_graph_legacy_source_contract_rejected"),
					TEXT("Use the source object; legacy carrier and hjson_document parameters are not supported."), Params, Field, TEXT("absent"), OutPrepared.Target);
				Diagnostic.ActualValue.Reset();
				Diagnostic.ExpectedValuePresent = false;
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			const TSharedPtr<FJsonObject>* SourceObject = nullptr;
			if (!Params->TryGetObjectField(TEXT("source"), SourceObject)
				|| !SourceObject || !SourceObject->IsValid())
			{
				return Fail(OutError, CommandInputFailure(TEXT("content_graph_source_required"),
					TEXT("Data asset import requires a source object."), Params, TEXT("source"), TEXT("object"), OutPrepared.Target));
			}
			FUeAgentResourceTracker SourceResources =
				MakeSemanticWorkspaceResourceTracker();
			FResolvedContentGraphImportSourcePtr ResolvedSource;
			FWorkspaceError WorkspaceError;
			if (!TryLoadNormalizedContentGraphImportSource(
					*SourceObject,
					OutPrepared.Target,
					ExpectedRevision,
					SourceResources,
					ResolvedSource,
					WorkspaceError)
				|| !ResolvedSource.IsValid())
			{
				return Fail(OutError, WorkspaceError.HasErrors() ? WorkspaceError.GetDiagnostic()
					: UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_graph_resolved_source_invalid"), TEXT("The import source did not resolve to a document."), TEXT("source_resolution")), OutPrepared.Target);
			}
			if (ResolvedSource->GetDocuments().Num() != 1)
			{
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("uobject_dataasset_multi_document_unsupported"),
					TEXT("Data asset import requires exactly one source document."), TEXT("source_resolution"));
				Diagnostic.SourceKind = TEXT("native_result");
				Diagnostic.PropertyPath = TEXT("document_count");
				Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(ResolvedSource->GetDocuments().Num());
				Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				Diagnostic.TargetPath = OutPrepared.Target;
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			const FContentGraphImportDocument* EntryDocument =
				ResolvedSource->GetEntryDocument();
			if (!EntryDocument)
			{
				return Fail(OutError, NativeConditionFailure(TEXT("content_graph_normalized_entry_document_missing"),
					TEXT("The normalized import source has no entry document."), TEXT("entry_document_available"), false, true,
					OutPrepared.Target, TEXT("source_resolution")));
			}
			OutPrepared.SourceFingerprint =
				ResolvedSource->GetSourceFingerprint();
			OutPrepared.SemanticFingerprint =
				ResolvedSource->GetSemanticFingerprint();
			OutPrepared.SourceCarrier =
				ResolvedSource->GetKind() == EWorkspaceSourceKind::Directory
					? EImportSourceCarrier::Directory
					: ResolvedSource->GetKind() == EWorkspaceSourceKind::SingleFile
						? EImportSourceCarrier::SingleFile
						: EImportSourceCarrier::Inline;

			FUeAgentResourceTracker ParseResources = MakeResources();
			FHjsonDocumentPtr Document;
			WorkspaceError.Reset();
			if (!TryParseHjsonDocument(
					EntryDocument->GetDocument(),
					ResolvedSource->GetSourceUnit(),
					ParseResources,
					Document,
					WorkspaceError)
				|| !Document.IsValid())
			{
				return FailWithTerminalFacts(OutError, WorkspaceError.GetDiagnostic());
			}
			const FHjsonNode& DocumentRoot = *Document->GetRoot();
			OutPrepared.Source.AstDocument = Document;
			OutPrepared.Source.SourceUnit = Document->GetSourceUnit();
			OutPrepared.Source.AstRoot = &DocumentRoot;
			if (DocumentRoot.GetKind() != EHjsonNodeKind::Object)
			{
				return Fail(OutError, InputFailure(TEXT("uobject_dataasset_hjson_root_invalid"), TEXT("The import document must be an object."), DocumentRoot, FString(), TEXT("object")));
			}
			FString Schema;
			if (!ReadString(DocumentRoot, TEXT("schema"), Schema, OutError)
				|| Schema != ContractId)
			{
				return Schema.IsEmpty()
					? false
					: Fail(OutError, InputFailure(TEXT("uobject_dataasset_contract_unsupported"), TEXT("The document schema is not supported by this importer."), DocumentRoot, TEXT("schema"), ContractId), Schema);
			}
			const FHjsonNode* Root = DocumentRoot.Get(TEXT("root"));
			if (!Root || Root->GetKind() != EHjsonNodeKind::Object)
			{
				return Fail(OutError, InputFailure(TEXT("uobject_dataasset_root_required"), TEXT("The document requires a root object."), DocumentRoot, TEXT("root"), TEXT("object")));
			}

			struct FPending final
			{
				const FHjsonNode* Node = nullptr;
				FString OwnerId;
				bool bRoot = false;
			};
			TArray<FPending> Worklist{{Root, FString(), true}};
			TArray<FInitializationEdge> InferredOwnership;
			for (int32 Cursor = 0; Cursor < Worklist.Num(); ++Cursor)
			{
				const FPending Pending = Worklist[Cursor];
				FNodeSpec Spec;
				Spec.OwnerId = Pending.OwnerId;
				Spec.bRoot = Pending.bRoot;
				Spec.InputPointer = Pending.Node->GetProvenance().GetJsonPointer();
				Spec.Values.Source = OutPrepared.Source;
				if (!ReadString(*Pending.Node, TEXT("id"), Spec.Id, OutError)
					|| !ReadString(*Pending.Node, TEXT("type"), Spec.Type, OutError))
				{
					return false;
				}
				Spec.Name = Pending.bRoot ? OutPrepared.AssetName : FString();
				if (!Pending.bRoot
					&& !ReadString(*Pending.Node, TEXT("name"), Spec.Name, OutError))
				{
					return false;
				}
				if ((Pending.bRoot && Spec.Type != RootType)
					|| (!Pending.bRoot && Spec.Type != OwnedType))
				{
					return Fail(OutError, InputFailure(TEXT("uobject_dataasset_type_unknown"), TEXT("This node type is not supported at this position."), *Pending.Node, TEXT("type"), Pending.bRoot ? RootType : OwnedType), Spec.Type);
				}
				if (OutPrepared.NodeIndexById.Contains(Spec.Id))
				{
					return Fail(OutError, InputFailure(TEXT("uobject_dataasset_node_duplicate"), TEXT("Node ids must be unique within the document."), *Pending.Node, TEXT("id")), Spec.Id);
				}
				if (!ReadValues(*Pending.Node, Pending.bRoot, Spec.Values, OutError))
				{
					return false;
				}
				OutPrepared.NodeIndexById.Add(Spec.Id, OutPrepared.Nodes.Num());
				OutPrepared.Nodes.Add(MoveTemp(Spec));
				if (!Pending.OwnerId.IsEmpty())
				{
					InferredOwnership.Emplace(
						Pending.OwnerId,
						OutPrepared.Nodes.Last().Id,
						EInitializationEdgeKind::Ownership);
				}
				const FHjsonNode* Owned = Pending.Node->Get(TEXT("owned"));
				if (!Owned)
				{
					continue;
				}
				if (Owned->GetKind() != EHjsonNodeKind::Array)
				{
					return Fail(OutError, InputFailure(TEXT("uobject_dataasset_owned_invalid"), TEXT("Owned nodes must be an array."), *Owned, FString(), TEXT("array")), OutPrepared.Nodes.Last().Id);
				}
				for (const FHjsonNodeRef& Child : Owned->GetItems())
				{
					if (Child->GetKind() != EHjsonNodeKind::Object)
					{
						return Fail(OutError, InputFailure(TEXT("uobject_dataasset_owned_item_invalid"), TEXT("Each owned node must be an object."), *Child, FString(), TEXT("object")));
					}
					Worklist.Add({&Child.Get(), OutPrepared.Nodes.Last().Id, false});
				}
			}

			OutPrepared.OwnershipEdges = InferredOwnership;
			const FHjsonNode* ExplicitOwnership = Root->Get(TEXT("ownership"));
			if (ExplicitOwnership)
			{
				if (ExplicitOwnership->GetKind() != EHjsonNodeKind::Array)
				{
					return Fail(OutError, InputFailure(TEXT("uobject_dataasset_ownership_invalid"), TEXT("Ownership edges must be an array."), *ExplicitOwnership, FString(), TEXT("array")));
				}
				TArray<FInitializationEdge> ExplicitEdges;
				TSet<FString> ExplicitChildren;
				for (const FHjsonNodeRef& Edge : ExplicitOwnership->GetItems())
				{
					if (Edge->GetKind() != EHjsonNodeKind::Object)
					{
						return Fail(OutError, InputFailure(TEXT("uobject_dataasset_ownership_item_invalid"), TEXT("Each ownership edge must be an object."), *Edge, FString(), TEXT("object")));
					}
					FString Owner;
					FString Child;
					if (!ReadString(*Edge, TEXT("owner"), Owner, OutError)
						|| !ReadString(*Edge, TEXT("child"), Child, OutError))
					{
						return false;
					}
					if (!OutPrepared.NodeIndexById.Contains(Owner)
						|| !OutPrepared.NodeIndexById.Contains(Child))
					{
						return Fail(OutError, InputFailure(TEXT("uobject_dataasset_ownership_endpoint_unknown"), TEXT("The ownership endpoint does not identify a declared node."), *Edge,
							OutPrepared.NodeIndexById.Contains(Owner) ? TEXT("child") : TEXT("owner")));
					}
					ExplicitChildren.Add(Child);
					ExplicitEdges.Emplace(Owner, Child, EInitializationEdgeKind::Ownership);
				}
				OutPrepared.OwnershipEdges.RemoveAll(
					[&ExplicitChildren](const FInitializationEdge& Edge)
					{
						return ExplicitChildren.Contains(Edge.GetToNodeId());
					});
				OutPrepared.OwnershipEdges.Append(ExplicitEdges);
			}

			const FHjsonNode* References = Root->Get(TEXT("references"));
			if (References)
			{
				if (References->GetKind() != EHjsonNodeKind::Array)
				{
					return Fail(OutError, InputFailure(TEXT("uobject_dataasset_references_invalid"), TEXT("References must be an array."), *References, FString(), TEXT("array")));
				}
				for (const FHjsonNodeRef& Edge : References->GetItems())
				{
					if (Edge->GetKind() != EHjsonNodeKind::Object)
					{
						return Fail(OutError, InputFailure(TEXT("uobject_dataasset_reference_item_invalid"), TEXT("Each reference must be an object."), *Edge, FString(), TEXT("object")));
					}
					FReferenceSpec Reference;
					Reference.InputPointer = Edge->GetProvenance().GetJsonPointer();
					if (!ReadString(*Edge, TEXT("from"), Reference.From, OutError)
						|| !ReadString(*Edge, TEXT("field"), Reference.Field, OutError)
						|| !ReadString(*Edge, TEXT("to"), Reference.To, OutError))
					{
						return false;
					}
					const int32* FromIndex = OutPrepared.NodeIndexById.Find(Reference.From);
					const int32* ToIndex = OutPrepared.NodeIndexById.Find(Reference.To);
					if (!FromIndex || !ToIndex || OutPrepared.Nodes[*FromIndex].bRoot || OutPrepared.Nodes[*ToIndex].bRoot)
					{
						return Fail(OutError, InputFailure(TEXT("uobject_dataasset_reference_invalid"), TEXT("A reference endpoint must identify a declared owned object."), *Edge,
							!FromIndex || OutPrepared.Nodes[*FromIndex].bRoot ? TEXT("from") : TEXT("to")));
					}
					if (Reference.Field != TEXT("peer") && Reference.Field != TEXT("shared"))
						return Fail(OutError, InputFailure(TEXT("uobject_dataasset_reference_invalid"), TEXT("Only peer and shared reference fields are supported."), *Edge, TEXT("field"), TEXT("peer or shared")));
					OutPrepared.References.Add(MoveTemp(Reference));
				}
			}

			FUnifiedHjsonSemanticModelPtr Model;
			if (!TryBuildSemanticModel(Model, WorkspaceError)
				|| !Model.IsValid())
			{
				return Fail(OutError, WorkspaceError.HasErrors() ? WorkspaceError.GetDiagnostic()
					: UE::UAI::Contract::FDiagnostic::Failure(TEXT("uobject_dataasset_semantic_model_failed"),
						TEXT("The data asset input could not be converted into a semantic model."), TEXT("command_execute")));
			}

			TArray<FImportChange> Changes;
			TArray<FInitializationNode> InitializationNodes;
			TArray<FInitializationEdge> AllEdges = OutPrepared.OwnershipEdges;
			for (const FNodeSpec& Spec : OutPrepared.Nodes)
			{
				Changes.Emplace(
					TEXT("create:") + Spec.Id,
					EImportChangeKind::Create,
					Spec.Id);
				InitializationNodes.Emplace(
					Spec.Id,
					Spec.bRoot
						? EInitializationNodeRole::RootTarget
						: EInitializationNodeRole::OwnedObject,
					Spec.Type,
					Spec.bRoot
						? RootInitializationContractId
						: OwnedInitializationContractId);
			}
			TMap<FString, TSet<FString>> PlannedReferenceTargets;
			for (const FReferenceSpec& Reference : OutPrepared.References)
			{
				Changes.Emplace(
					TEXT("connect:") + Reference.From + TEXT(":") + Reference.Field,
					EImportChangeKind::Connect,
					Reference.From);
				// The planner models object dependency edges, not individual fields.
				// Peer and Shared may bind the same pair while remaining distinct writes.
				TSet<FString>& Targets = PlannedReferenceTargets.FindOrAdd(Reference.From);
				if (!Targets.Contains(Reference.To))
				{
					AllEdges.Emplace(Reference.From, Reference.To, EInitializationEdgeKind::Reference);
					Targets.Add(Reference.To);
				}
			}

			const FImportInitializationPlanningRequest PlanningRequest(
				OutPrepared.SourceCarrier,
				Model.ToSharedRef(),
				OutPrepared.Target,
				FUeAgentStateRevision(TEXT("uobject-dataasset"), TEXT("absent"), 0),
				OutPrepared.SemanticFingerprint,
				Sha256(ContractId + TEXT("|") + OutPrepared.Target),
				MoveTemp(Changes),
				MoveTemp(InitializationNodes),
				MoveTemp(AllEdges),
				{OutPrepared.Target},
				OutPrepared.AuthorizationId);
			FUeAgentResourceTracker PlanningResources =
				MakeResources();
			if (!TryPlanHjsonImportInitialization(
					PlanningRequest,
					MakeShared<FPreflight>(
						OutPrepared.Target,
						OutPrepared.AuthorizationId),
					PlanningResources,
					OutPrepared.InitializationPlan,
					WorkspaceError)
				|| !OutPrepared.InitializationPlan.IsValid())
			{
				return FailWithTerminalFacts(OutError, WorkspaceError.GetDiagnostic());
			}
			// Materialization consumes the planner's accepted ownership, including overrides.
			for (const FInitializationEdge& Edge : OutPrepared.OwnershipEdges)
			{
				OutPrepared.Nodes[OutPrepared.NodeIndexById[Edge.GetToNodeId()]].OwnerId = Edge.GetFromNodeId();
			}
			return true;
		}

		struct FExecutionContext final
		{
			explicit FExecutionContext(FPreparedImport InPrepared)
				: Prepared(MoveTemp(InPrepared))
			{
			}

			FPreparedImport Prepared;
			UUeAgentRecursiveDataAsset* Asset = nullptr;
			TMap<FString, UUeAgentRecursiveOwnedObject*> OwnedById;
			TSharedPtr<FJsonObject> ExpectedReadback;
			TSharedPtr<FJsonObject> IndependentReadback;
			FString SuccessPayloadJson;
		};

		TSharedPtr<FJsonObject> BuildReadback(
			UUeAgentRecursiveDataAsset& Asset,
			bool bReloaded);
		TSharedPtr<FJsonObject> BuildExpectedReadback(const FPreparedImport& Prepared);

		bool PrepareValues(
			UObject& Object,
			const FNodeValues& Values,
			const bool bAllowTemplateOwner,
			UeAgentAuthoringObjectApply::FPreparedApply& OutPrepared,
			FOperationLifecycleError& OutError)
		{
			if (!Values.Properties.IsValid()) return true;
			ON_SCOPE_EXIT { if (OutError.IsValid()) Values.Locate(OutError); };
			UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(Values.Properties.Get(),
				[&Values](const FString& Pointer) { return Values.Source.ResolveSource(Values.ResolvePointer(Pointer)); });
			UeAgentAuthoringObjectApply::FOptions Options;
			Options.bCompactValues = true;
			Options.bAllowTemplateOwners = bAllowTemplateOwner;
			// Only the read-only preflight uses this for the registered native CDO.
			Options.bAllowDomainAuthorizedOwners = bAllowTemplateOwner;
			TArray<TSharedPtr<FJsonValue>> Descriptors;
			UE::UAI::Contract::FDiagnostic Error;
			if (!UeAgentDetailsAuthoringService::DecodePropertiesDocument(
					Values.Properties, Descriptors, Error))
			{
				OutError = Error;
				return false;
			}
			UeAgentAuthoringObjectApply::FFailure Failure;
			if (!UeAgentAuthoringObjectApply::PrepareObject(
					&Object, Descriptors, Options, OutPrepared, Failure))
			{
				OutError = Failure.Diagnostic;
				return false;
			}
			return true;
		}

		bool ApplyValues(
			UObject& Object,
			const FNodeValues& Values,
			FOperationLifecycleError& OutError)
		{
			if (!Values.Properties.IsValid()) return true;
			// This graph already owns its recoverable lifecycle. Do not commit
			// a separate editor undo transaction for each newly created object.
			UeAgentAuthoringObjectApply::FPreparedApply Prepared;
			if (!PrepareValues(Object, Values, false, Prepared, OutError)) return false;
			UeAgentAuthoringObjectApply::FApplyResult Result;
			if (!UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, Result))
			{
				OutError = FOperationLifecycleError::WrapFailure(Result.Diagnostic.IsValid() ? Result.Diagnostic.Code : TEXT("uobject_dataasset_value_apply_failed"), Result.Diagnostic, TEXT("operation_lifecycle"));
				Values.Locate(OutError);
				return false;
			}
			return true;
		}

		class FVersionFence final : public IOperationVersionFencePort
		{
		public:
			explicit FVersionFence(TSharedRef<FExecutionContext> InContext)
				: Context(MoveTemp(InContext)) {}

			virtual bool Check(
				const FOperationLifecyclePlan& Plan,
				const EOperationLifecycleStage Stage,
				bool& bOutCurrent,
				FOperationLifecycleError& OutError) override
			{
				bOutCurrent = Stage == EOperationLifecycleStage::PreExecuteFence
					? !TargetExists(Context->Prepared.Target)
					: Context->Asset != nullptr
						&& Context->Asset->GetPathName() == Context->Prepared.Target;
				if (!bOutCurrent)
				{
					const bool bBeforeExecute = Stage == EOperationLifecycleStage::PreExecuteFence;
					OutError = NativeConditionFailure(TEXT("operation_version_conflict"),
						bBeforeExecute ? TEXT("The target asset already exists; this import requires an unused target.")
							: !Context->Asset ? TEXT("The created asset is no longer available before persistence.")
								: TEXT("The created asset path changed before persistence."),
						bBeforeExecute ? TEXT("target_exists") : TEXT("target_identity_matches"),
						bBeforeExecute, !bBeforeExecute, Context->Prepared.Target, LexToString(Stage));
					if (!bBeforeExecute && Context->Asset)
					{
						OutError.ActualValue = MakeShared<FJsonValueString>(Context->Asset->GetPathName());
						OutError.ExpectedValue = MakeShared<FJsonValueString>(Context->Prepared.Target);
					}
				}
				return true;
			}

		private:
			TSharedRef<FExecutionContext> Context;
		};

		class FMutationPort final : public IOperationDomainMutationPort
		{
		public:
			explicit FMutationPort(TSharedRef<FExecutionContext> InContext)
				: Context(MoveTemp(InContext)) {}

			virtual bool Execute(
				const FOperationLifecyclePlan& Plan,
				FOperationMutationFacts& OutFacts,
				FOperationLifecycleError& OutError) override
			{
				Context->ExpectedReadback = BuildExpectedReadback(Context->Prepared);
				FAssetToolsModule& AssetToolsModule =
					FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
				UDataAssetFactory* Factory = NewObject<UDataAssetFactory>();
				Factory->DataAssetClass = UUeAgentRecursiveDataAsset::StaticClass();
				UObject* Created = AssetToolsModule.Get().CreateAsset(
					Context->Prepared.AssetName,
					FPackageName::GetLongPackagePath(Context->Prepared.PackageName),
					UUeAgentRecursiveDataAsset::StaticClass(),
					Factory);
				Context->Asset = Cast<UUeAgentRecursiveDataAsset>(Created);
				if (!Context->Asset)
				{
					OutError = NativeConditionFailure(TEXT("uobject_dataasset_factory_create_failed"),
						TEXT("The asset factory did not return a recursive data asset."), TEXT("created_asset_class"),
						false, true, Context->Prepared.Target, TEXT("domain_execute"));
					OutError.ActualValue = Created ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Created->GetClass()->GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(UUeAgentRecursiveDataAsset::StaticClass()->GetPathName());
					return false;
				}
				OutFacts = FOperationMutationFacts(
					FGuid::NewGuid().ToString(EGuidFormats::Digits),
					{Context->Prepared.Target},
					true);
				const FNodeSpec* RootSpec = Context->Prepared.Nodes.FindByPredicate(
					[](const FNodeSpec& Spec) { return Spec.bRoot; });
				if (!RootSpec)
				{
					OutError = NativeConditionFailure(TEXT("uobject_dataasset_root_plan_missing"),
						TEXT("The prepared data asset plan has no root node."), TEXT("root_node_available"), false, true,
						Context->Prepared.Target, TEXT("domain_execute"));
					return false;
				}
				Context->Asset->Modify();
				if (!ApplyValues(
					*Context->Asset,
					RootSpec->Values,
					OutError)) return false;

				for (const FString& NodeId
					: Context->Prepared.InitializationPlan->GetCreationOrder())
				{
					const int32* Index = Context->Prepared.NodeIndexById.Find(NodeId);
					if (!Index)
					{
						OutError = NativeConditionFailure(TEXT("uobject_dataasset_plan_node_unknown"),
							TEXT("The initialization order refers to a node absent from the prepared import."), TEXT("node_id"), false, true,
							Context->Prepared.Target, TEXT("domain_execute"));
						OutError.ActualValue = MakeShared<FJsonValueString>(NodeId);
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("declared import node id"));
						return false;
					}
					const FNodeSpec& Spec = Context->Prepared.Nodes[*Index];
					if (Spec.bRoot)
					{
						continue;
					}
					UObject* Outer = Spec.OwnerId == RootSpec->Id
						? static_cast<UObject*>(Context->Asset)
						: static_cast<UObject*>(Context->OwnedById.FindRef(Spec.OwnerId));
					if (!Outer)
					{
						OutError = NativeConditionFailure(TEXT("uobject_dataasset_owner_missing"),
							FString::Printf(TEXT("The owner object for node '%s' was not created."), *Spec.Id), TEXT("owner_id"), false, true,
							Context->Prepared.Target, TEXT("domain_execute"));
						OutError.ActualValue = MakeShared<FJsonValueString>(Spec.OwnerId);
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("created owner object"));
						OutError.SemanticPointer = Spec.InputPointer;
						OutError.SourceKind = TEXT("related_input");
						return false;
					}
					UUeAgentRecursiveOwnedObject* Object =
						NewObject<UUeAgentRecursiveOwnedObject>(
							Outer,
							UUeAgentRecursiveOwnedObject::StaticClass(),
							FName(*Spec.Name),
							RF_Transactional);
					if (!Object)
					{
						OutError = NativeConditionFailure(TEXT("uobject_dataasset_child_create_failed"),
							FString::Printf(TEXT("The owned object for node '%s' could not be created."), *Spec.Id), TEXT("owned_object_created"), false, true,
							Context->Prepared.Target, TEXT("domain_execute"));
						OutError.SemanticPointer = Spec.InputPointer;
						OutError.SourceKind = TEXT("related_input");
						return false;
					}
					Object->NodeId = Spec.Id;
					if (!ApplyValues(*Object, Spec.Values, OutError)) return false;
					if (UUeAgentRecursiveDataAsset* Root = Cast<UUeAgentRecursiveDataAsset>(Outer))
					{
						Root->OwnedObjects.Add(Object);
					}
					else if (UUeAgentRecursiveOwnedObject* Owner = Cast<UUeAgentRecursiveOwnedObject>(Outer))
					{
						Owner->Children.Add(Object);
					}
					Context->OwnedById.Add(Spec.Id, Object);
					if (!FailureAfterNode.IsEmpty()
						&& FailureAfterNode == Spec.Id)
					{
						OutError = NativeConditionFailure(TEXT("uobject_dataasset_child_create_failed"),
							FString::Printf(TEXT("The test-injected failure occurred after creating node '%s'."), *Spec.Id), TEXT("node_creation_stage_succeeded"), false, true,
							Context->Prepared.Target, TEXT("domain_execute"));
						return false;
					}
				}

				for (const FReferenceSpec& Reference
					: Context->Prepared.References)
				{
					UUeAgentRecursiveOwnedObject* From =
						Context->OwnedById.FindRef(Reference.From);
					UUeAgentRecursiveOwnedObject* To =
						Context->OwnedById.FindRef(Reference.To);
					if (!From || !To)
					{
						OutError = FOperationLifecycleError::Failure(TEXT("uobject_dataasset_reference_endpoint_missing"), TEXT("An owned reference endpoint was not created."), TEXT("operation_lifecycle"), !From ? Reference.From : Reference.To);
						OutError.SemanticPointer = Reference.InputPointer + (!From ? TEXT("/from") : TEXT("/to"));
						return false;
					}
					if (Reference.Field == TEXT("peer")) From->Peer = To;
					else From->Shared = To;
				}
				Context->Asset->GetOutermost()->SetDirtyFlag(true);
				Context->Asset->PostEditChange();
				return true;
			}

			virtual bool Rollback(
				const FOperationLifecyclePlan& Plan,
				const FOperationMutationFacts& Facts,
				bool& bOutConfirmed,
				FOperationLifecycleError& OutError) override
			{
				UUeAgentRecursiveDataAsset* Asset = Context->Asset;
				if (!Asset)
				{
					Asset = LoadObject<UUeAgentRecursiveDataAsset>(
						nullptr, *Context->Prepared.Target);
				}
				Context->OwnedById.Reset();
				Context->Asset = nullptr;
				if (Asset)
				{
					ObjectTools::DeleteObjectsUnchecked({Asset});
				}
				const FString Filename = FPackageName::LongPackageNameToFilename(
					Context->Prepared.PackageName,
					FPackageName::GetAssetPackageExtension());
				if (IFileManager::Get().FileExists(*Filename))
				{
					IFileManager::Get().Delete(*Filename, false, true, true);
				}
				const bool bObjectRemoved = StaticFindObject(
					UObject::StaticClass(),
					nullptr,
					*Context->Prepared.Target) == nullptr;
				const bool bPackageRemoved = !IFileManager::Get().FileExists(*Filename);
				const bool bRegistryRemoved = !FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
						.Get().GetAssetByObjectPath(FSoftObjectPath(Context->Prepared.Target)).IsValid();
				bOutConfirmed = bObjectRemoved && bPackageRemoved && bRegistryRemoved;
				if (!bOutConfirmed)
				{
					OutError = NativeConditionFailure(TEXT("uobject_dataasset_rollback_unconfirmed"),
						!bObjectRemoved ? TEXT("The created asset object still exists after rollback.")
							: !bPackageRemoved ? TEXT("The created asset package file still exists after rollback.")
								: TEXT("The created asset is still registered after rollback."),
						!bObjectRemoved ? TEXT("asset_object_absent") : !bPackageRemoved ? TEXT("package_file_absent") : TEXT("registry_entry_absent"),
						false, true, Context->Prepared.Target, TEXT("rollback"));
				}
				return bOutConfirmed;
			}

		private:
			TSharedRef<FExecutionContext> Context;
		};

		class FStabilityPort final : public IOperationStabilityPort
		{
		public:
			explicit FStabilityPort(TSharedRef<FExecutionContext> InContext)
				: Context(MoveTemp(InContext)) {}

			virtual bool Settle(
				const FOperationLifecyclePlan& Plan,
				const FOperationMutationFacts& Facts,
				FOperationLifecycleError& OutError) override
			{
				if (!Context->Asset
					|| !Context->Asset->NativeDefaultSubobject
					|| Context->Asset->NativeDefaultSubobject->GetOuter()
						!= Context->Asset)
				{
					OutError = NativeConditionFailure(TEXT("uobject_dataasset_settle_root_invalid"),
						!Context->Asset ? TEXT("The root data asset is unavailable while settling the import.")
							: !Context->Asset->NativeDefaultSubobject ? TEXT("The root data asset has no native default subobject.")
								: TEXT("The root native default subobject has the wrong outer."),
						!Context->Asset ? TEXT("root_asset_available") : !Context->Asset->NativeDefaultSubobject
							? TEXT("native_default_subobject_available") : TEXT("native_default_subobject_outer"),
						false, true, Context->Prepared.Target, TEXT("settle"));
					if (Context->Asset && Context->Asset->NativeDefaultSubobject)
					{
						OutError.ActualValue = MakeShared<FJsonValueString>(GetPathNameSafe(Context->Asset->NativeDefaultSubobject->GetOuter()));
						OutError.ExpectedValue = MakeShared<FJsonValueString>(Context->Asset->GetPathName());
					}
					return false;
				}
				for (const TPair<FString, UUeAgentRecursiveOwnedObject*>& Pair
					: Context->OwnedById)
				{
					if (!Pair.Value
						|| !Pair.Value->NativeDefaultSubobject
						|| Pair.Value->NativeDefaultSubobject->GetOuter()
							!= Pair.Value)
					{
						OutError = NativeConditionFailure(TEXT("uobject_dataasset_settle_child_invalid"),
							FString::Printf(TEXT("Owned node '%s' does not have a valid native default subobject and outer."), *Pair.Key),
							!Pair.Value ? TEXT("owned_object_available") : !Pair.Value->NativeDefaultSubobject
								? TEXT("native_default_subobject_available") : TEXT("native_default_subobject_outer"),
							false, true, Context->Prepared.Target, TEXT("settle"));
						if (Pair.Value && Pair.Value->NativeDefaultSubobject)
						{
							OutError.ActualValue = MakeShared<FJsonValueString>(GetPathNameSafe(Pair.Value->NativeDefaultSubobject->GetOuter()));
							OutError.ExpectedValue = MakeShared<FJsonValueString>(Pair.Value->GetPathName());
						}
						return false;
					}
				}
				return true;
			}

		private:
			TSharedRef<FExecutionContext> Context;
		};

		class FPersistencePort final : public IOperationPersistencePort
		{
		public:
			explicit FPersistencePort(TSharedRef<FExecutionContext> InContext)
				: Context(MoveTemp(InContext)) {}

			virtual bool Persist(
				const FOperationLifecyclePlan& Plan,
				const FOperationMutationFacts& Facts,
				FString& OutPersistenceReceiptId,
				FOperationLifecycleError& OutError) override
			{
				if (!Context->Asset)
				{
					OutError = NativeConditionFailure(TEXT("uobject_dataasset_persist_asset_missing"),
						TEXT("The data asset is unavailable for package persistence."), TEXT("asset_available"), false, true,
						Context->Prepared.Target, TEXT("persist"));
					return false;
				}
				UPackage* Package = Context->Asset->GetOutermost();
				const FString Filename = FPackageName::LongPackageNameToFilename(
					Context->Prepared.PackageName,
					FPackageName::GetAssetPackageExtension());
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				UE::UAI::Contract::FDiagnostic SaveDetail;
				if (!UE::UAI::AssetDurable::TrySaveLoadedPackage(
						*Package,
						Context->Asset,
						Filename,
						Args, SaveDetail))
				{
					OutError = SaveDetail;
					return false;
				}
				OutPersistenceReceiptId = Sha256(
					Context->Prepared.Target + TEXT("|") + Filename);
				return true;
			}

		private:
			TSharedRef<FExecutionContext> Context;
		};

		TSharedPtr<FJsonObject> LeafReadback(
			const UUeAgentRecursiveLeafObject* Leaf, const FString& ExpectedOuter = FString())
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			if (Leaf)
			{
				Result->SetStringField(TEXT("class"), Leaf->GetClass()->GetPathName());
				Result->SetStringField(TEXT("name"), Leaf->GetName());
				Result->SetStringField(TEXT("outer"), ExpectedOuter.IsEmpty()
					? Leaf->GetOuter()->GetPathName() : ExpectedOuter);
				Result->SetNumberField(TEXT("native_default_value"), Leaf->NativeDefaultValue);
			}
			return Result;
		}

		TSharedPtr<FJsonObject> BuildReadback(
			UUeAgentRecursiveDataAsset& Asset,
			const bool bReloaded)
		{
			TMap<FString, UUeAgentRecursiveOwnedObject*> ById;
			TArray<TSharedPtr<FJsonValue>> Nodes;
			const UUeAgentRecursiveDataAsset* RootDefault = GetDefault<UUeAgentRecursiveDataAsset>();
			const UUeAgentRecursiveOwnedObject* OwnedDefault = GetDefault<UUeAgentRecursiveOwnedObject>();
			bool bDefaults = Asset.NativeDefaultSubobject
				&& Asset.NativeDefaultSubobject->GetOuter() == &Asset
				&& CanonicalJson::CompareJsonValues(
					MakeShared<FJsonValueObject>(LeafReadback(Asset.NativeDefaultSubobject)),
					MakeShared<FJsonValueObject>(LeafReadback(RootDefault->NativeDefaultSubobject, Asset.GetPathName()))).bEquivalent;
			TArray<UUeAgentRecursiveOwnedObject*> Pending = Asset.OwnedObjects;
			for (int32 Cursor = 0; Cursor < Pending.Num(); ++Cursor)
			{
				UUeAgentRecursiveOwnedObject* Object = Pending[Cursor];
				if (!Object || ById.Contains(Object->NodeId))
				{
					bDefaults = false;
					continue;
				}
				ById.Add(Object->NodeId, Object);
				bDefaults = bDefaults
					&& Object->NativeCounter == OwnedDefault->NativeCounter
					&& Object->NativeDefaultSubobject
					&& Object->NativeDefaultSubobject->GetOuter() == Object
					&& CanonicalJson::CompareJsonValues(
						MakeShared<FJsonValueObject>(LeafReadback(Object->NativeDefaultSubobject)),
						MakeShared<FJsonValueObject>(LeafReadback(OwnedDefault->NativeDefaultSubobject, Object->GetPathName()))).bEquivalent;
				Pending.Append(Object->Children);
				const TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
				Node->SetStringField(TEXT("id"), Object->NodeId);
				Node->SetStringField(TEXT("name"), Object->GetName());
				Node->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());
				Node->SetStringField(TEXT("outer"), Object->GetOuter()->GetPathName());
				Node->SetObjectField(TEXT("native_default_subobject"), LeafReadback(Object->NativeDefaultSubobject));
				Node->SetStringField(TEXT("label"), Object->Label);
				Node->SetNumberField(TEXT("native_counter"), Object->NativeCounter);
				Node->SetStringField(TEXT("peer"), Object->Peer ? Object->Peer->NodeId : FString());
				Node->SetStringField(TEXT("shared"), Object->Shared ? Object->Shared->NodeId : FString());
				Node->SetStringField(TEXT("peer_path"), Object->Peer ? Object->Peer->GetPathName() : FString());
				Node->SetStringField(TEXT("shared_path"), Object->Shared ? Object->Shared->GetPathName() : FString());
				Nodes.Add(MakeShared<FJsonValueObject>(Node));
			}
			Nodes.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
			{
				return A->AsObject()->GetStringField(TEXT("id"))
					< B->AsObject()->GetStringField(TEXT("id"));
			});
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("reloaded"), bReloaded);
			Result->SetStringField(TEXT("asset_class"), Asset.GetClass()->GetPathName());
			Result->SetStringField(TEXT("root_outer"), Asset.GetOuter()->GetPathName());
			Result->SetObjectField(TEXT("native_default_subobject"), LeafReadback(Asset.NativeDefaultSubobject));
			Result->SetNumberField(TEXT("native_counter"), Asset.NativeCounter);
			Result->SetStringField(TEXT("label"), Asset.Label);
			Result->SetNumberField(TEXT("explicit_number"), Asset.ExplicitNumber);
			TArray<TSharedPtr<FJsonValue>> Numbers;
			for (const int32 Value : Asset.Numbers)
			{
				Numbers.Add(MakeShared<FJsonValueNumber>(Value));
			}
			Result->SetArrayField(TEXT("numbers"), MoveTemp(Numbers));
			const TSharedPtr<FJsonObject> NamedNumbers = MakeShared<FJsonObject>();
			for (const TPair<FString, int32>& Pair : Asset.NamedNumbers)
			{
				NamedNumbers->SetNumberField(Pair.Key, Pair.Value);
			}
			Result->SetObjectField(TEXT("named_numbers"), NamedNumbers);
			Result->SetNumberField(TEXT("owned_count"), Asset.OwnedObjects.Num());
			Result->SetNumberField(TEXT("planned_node_count"), 1 + Nodes.Num());
			Result->SetArrayField(TEXT("nodes"), MoveTemp(Nodes));
			Result->SetBoolField(TEXT("native_default_subobjects_preserved"), bDefaults);
			return Result;
		}

		TSharedPtr<FJsonObject> BuildExpectedReadback(const FPreparedImport& Prepared)
		{
			// Expected values come from the immutable request and native CDOs, never
			// from the object being mutated or from the persisted readback.
			UUeAgentRecursiveDataAsset* RootDefault = GetMutableDefault<UUeAgentRecursiveDataAsset>();
			const UUeAgentRecursiveOwnedObject* OwnedDefault = GetDefault<UUeAgentRecursiveOwnedObject>();
			TSharedPtr<FJsonObject> Expected = BuildReadback(*RootDefault, false);
			Expected->SetStringField(TEXT("root_outer"), Prepared.PackageName);
			Expected->SetObjectField(TEXT("native_default_subobject"),
				LeafReadback(RootDefault->NativeDefaultSubobject, Prepared.Target));
			TMap<FString, FString> Paths;
			TMap<FString, TSharedPtr<FJsonObject>> NodesById;
			int32 RootChildren = 0;
			for (const FString& Id : Prepared.InitializationPlan->GetCreationOrder())
			{
				const FNodeSpec& Spec = Prepared.Nodes[Prepared.NodeIndexById[Id]];
				if (Spec.bRoot)
				{
					Paths.Add(Id, Prepared.Target);
					if (Spec.Values.ExplicitValues.IsValid())
						for (const auto& Pair : Spec.Values.ExplicitValues->Values)
							Expected->SetField(Pair.Key, Pair.Value);
					continue;
				}
				const FString Outer = Paths.FindRef(Spec.OwnerId);
				const bool bRootChild = Outer == Prepared.Target;
				const FString Path = Outer + (bRootChild ? TEXT(":") : TEXT(".")) + Spec.Name;
				Paths.Add(Id, Path);
				RootChildren += bRootChild ? 1 : 0;
				const TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
				Node->SetStringField(TEXT("id"), Spec.Id);
				Node->SetStringField(TEXT("name"), Spec.Name);
				Node->SetStringField(TEXT("class"), Spec.Type);
				Node->SetStringField(TEXT("outer"), Outer);
				Node->SetStringField(TEXT("label"), OwnedDefault->Label);
				Node->SetNumberField(TEXT("native_counter"), OwnedDefault->NativeCounter);
				Node->SetObjectField(TEXT("native_default_subobject"),
					LeafReadback(OwnedDefault->NativeDefaultSubobject, Path));
				for (const TCHAR* Field : {TEXT("peer"), TEXT("shared"), TEXT("peer_path"), TEXT("shared_path")})
					Node->SetStringField(Field, FString());
				if (Spec.Values.ExplicitValues.IsValid())
					for (const auto& Pair : Spec.Values.ExplicitValues->Values)
						Node->SetField(Pair.Key, Pair.Value);
				NodesById.Add(Id, Node);
			}
			for (const FReferenceSpec& Reference : Prepared.References)
			{
				if (TSharedPtr<FJsonObject>* Node = NodesById.Find(Reference.From))
				{
					(*Node)->SetStringField(Reference.Field, Reference.To);
					(*Node)->SetStringField(Reference.Field + TEXT("_path"), Paths.FindRef(Reference.To));
				}
			}
			TArray<FString> Ids;
			NodesById.GetKeys(Ids);
			Ids.Sort();
			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (const FString& Id : Ids) Nodes.Add(MakeShared<FJsonValueObject>(NodesById[Id]));
			Expected->SetArrayField(TEXT("nodes"), MoveTemp(Nodes));
			Expected->SetNumberField(TEXT("owned_count"), RootChildren);
			Expected->SetNumberField(TEXT("planned_node_count"), Prepared.Nodes.Num());
			return Expected;
		}

		class FIndependentVerificationPort final
			: public IOperationIndependentVerificationPort
		{
		public:
			explicit FIndependentVerificationPort(
				TSharedRef<FExecutionContext> InContext)
				: Context(MoveTemp(InContext)) {}

			virtual bool Verify(
				const FOperationLifecyclePlan& Plan,
				const FOperationMutationFacts& Facts,
				const FString& PersistenceReceiptId,
				bool& bOutEquivalent,
				FOperationLifecycleError& OutError) override
			{
				UPackage* Package = Context->Asset
					? Context->Asset->GetOutermost()
					: nullptr;
				if (!Package)
				{
					OutError = NativeConditionFailure(TEXT("uobject_dataasset_readback_package_missing"),
						TEXT("The data asset package is unavailable for independent verification."), TEXT("package_available"), false, true,
						Context->Prepared.Target, TEXT("independent_verify"));
					return false;
				}
				Context->OwnedById.Reset();
				Context->Asset = nullptr;
				UE::UAI::Contract::FDiagnostic ReloadError;
				if (!UeAgentAuthoringOperationApply::TryReloadPersistedPackage(Context->Prepared.Target, ReloadError))
				{
					OutError = ReloadError;
					return false;
				}
				UUeAgentRecursiveDataAsset* Reloaded =
					LoadObject<UUeAgentRecursiveDataAsset>(
						nullptr,
						*Context->Prepared.Target);
				if (!Reloaded)
				{
					OutError = NativeConditionFailure(TEXT("uobject_dataasset_reload_failed"),
						TEXT("The persisted data asset could not be reloaded for independent verification."),
						TEXT("asset_reloaded"), false, true, Context->Prepared.Target, TEXT("independent_verify"));
					return false;
				}
				Context->Asset = Reloaded;
				Context->IndependentReadback = BuildReadback(*Reloaded, true);
				const TSharedPtr<FJsonObject> Expected = MakeShared<FJsonObject>();
				Expected->Values = Context->ExpectedReadback.IsValid()
					? Context->ExpectedReadback->Values
					: TMap<FString, TSharedPtr<FJsonValue>>();
				Expected->RemoveField(TEXT("reloaded"));
				const TSharedPtr<FJsonObject> Actual = MakeShared<FJsonObject>();
				Actual->Values = Context->IndependentReadback->Values;
				Actual->RemoveField(TEXT("reloaded"));
				auto Difference = CanonicalJson::CompareJsonValues(
					Context->ExpectedReadback.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(Expected)) : nullptr,
					MakeShared<FJsonValueObject>(Actual));
				if (Difference.bEquivalent && !Context->IndependentReadback->GetBoolField(TEXT("native_default_subobjects_preserved")))
					Difference = CanonicalJson::FValueComparison::Different(TEXT("/native_default_subobjects_preserved"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				bOutEquivalent = Difference.bEquivalent;
				if (!bOutEquivalent)
				{
					OutError = FOperationLifecycleError::Failure(TEXT("uobject_dataasset_readback_mismatch"),
						TEXT("Reloaded data asset differs from the expected native state."), TEXT("operation_lifecycle"));
					Difference.ApplyTo(OutError);
				}
				{
					if (bFailFinalProjection)
					{
						if (FinalProjectionObserver) FinalProjectionObserver(*Reloaded);
						bOutEquivalent = false;
						OutError = NativeConditionFailure(TEXT("uobject_dataasset_result_projection_failed"),
							TEXT("The test-injected final result projection failed after the data asset was reloaded."),
							TEXT("result_projection_succeeded"), false, true, Context->Prepared.Target, TEXT("independent_verify"));
						return false;
					}
					DomainAuthoring::FDomainHjsonImportLifecycleReceipt LifecycleReceipt;
					LifecycleReceipt.bMutationApplied = true;
					LifecycleReceipt.bNativeUpdateSettled = true;
					LifecycleReceipt.bPackageSaved = true;
					LifecycleReceipt.PersistenceReceiptId = PersistenceReceiptId;
					const TSharedPtr<FJsonObject> Mutation = MakeShared<FJsonObject>();
					Mutation->SetStringField(TEXT("plan_fingerprint"), Plan.GetPlanFingerprint());
					FString CanonicalActual;
					UE::UAI::Contract::FDiagnostic EncodingError;
					if (!Json::TryCanonicalize(Actual, CanonicalActual, EncodingError))
					{
						bOutEquivalent = false;
						EncodingError.SourceKind = TEXT("native_result");
						OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("uobject_dataasset_revision_encoding_failed"),
							EncodingError, TEXT("independent_verify"), TEXT("The persisted data asset readback could not be encoded to compute its revision."));
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Context->Prepared.Target;
						return false;
					}
					const FString Revision = Sha256(CanonicalActual);
					FWorkspaceError WorkspaceError;
					if (!DomainAuthoring::TryBuildImportSuccessPayload(
							DomainProviderId, Context->Prepared.Target, Revision,
							LifecycleReceipt, Context->IndependentReadback, Mutation,
							Context->SuccessPayloadJson, WorkspaceError))
					{
						bOutEquivalent = false;
						OutError = WorkspaceError.HasErrors() ? WorkspaceError.GetDiagnostic()
							: NativeConditionFailure(TEXT("uobject_dataasset_result_projection_failed"),
								TEXT("The persisted data asset could not be projected into the import result."), TEXT("result_projected"), false, true,
								Context->Prepared.Target, TEXT("result_projection"));
						return false;
					}
				}
				return true;
			}

		private:
			TSharedRef<FExecutionContext> Context;
		};

		class FProductionService final
			: public IUeAgentUObjectDataAssetCommandService
		{
		public:
			virtual bool Execute(
				const FUeAgentUObjectDataAssetCommandRequest& Request,
				FUeAgentUObjectDataAssetCommandResponse& OutResponse,
				FUeAgentUObjectDataAssetCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return Fail(OutError, NativeConditionFailure(TEXT("uobject_dataasset_cancelled"),
						TEXT("The data asset command was cancelled before execution."), TEXT("cancellation_requested"), true, false), Request.CommandId);
				}
				TSharedPtr<FJsonObject> Params;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!Json::TryParseObject(
						Request.CanonicalParamsJson,
						Params,
						JsonError))
				{
					return FailWithTerminalFacts(OutError, JsonError);
				}
				const FUeAgentUObjectDataAssetCommandRoute* Route =
					GetUObjectDataAssetCommandRoutes().FindByPredicate(
						[&Request](const FUeAgentUObjectDataAssetCommandRoute& Candidate)
						{
							return Candidate.CommandId == Request.CommandId;
						});
				if (!Route)
				{
					return Fail(OutError, NativeConditionFailure(TEXT("uobject_dataasset_command_not_supported"),
						TEXT("The data asset service has no route for this command."), TEXT("command_supported"), false, true), Request.CommandId);
				}
				if (Route->Kind == EUeAgentUObjectDataAssetRouteKind::Import)
				{
					return ExecuteImport(Request, Params, OutResponse, OutError);
				}
				FString Target;
				if (!Params->TryGetStringField(TEXT("target"), Target))
				{
					return Fail(OutError, CommandInputFailure(TEXT("uobject_dataasset_target_required"),
						TEXT("The target parameter must be an asset path string."), Params, TEXT("target"), TEXT("asset object path string")), Request.CommandId);
				}
				UUeAgentRecursiveDataAsset* Asset =
					LoadObject<UUeAgentRecursiveDataAsset>(nullptr, *Target);
				if (!Asset)
				{
					return FailUnresolvedTarget(OutError, TEXT("content_graph_read_target_unresolved"), Target);
				}
				if (Route->Kind == EUeAgentUObjectDataAssetRouteKind::Export)
				{
					return ExecuteExport(Request, Params, *Asset, OutResponse, OutError);
				}
				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("command_id"), Request.CommandId);
				Payload->SetStringField(TEXT("mechanics_command_id"), Route->MechanicsCommandId);
				Payload->SetBoolField(TEXT("read_only"), true);
				Payload->SetObjectField(TEXT("query_result"), BuildReadback(*Asset, false));
				UE::UAI::Contract::FDiagnostic EncodingError;
				if (!Json::TryCanonicalize(Payload, OutResponse.PayloadJson, EncodingError))
				{
					EncodingError.SourceKind = TEXT("native_result");
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("uobject_dataasset_query_encoding_failed"),
						EncodingError, TEXT("result_projection"), TEXT("The data asset query result could not be encoded."));
					if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Target;
					return FailWithTerminalFacts(OutError, Diagnostic);
				}
				OutResponse.bReadOnly = true;
				return true;
			}

		private:
			bool ExecuteImport(
				const FUeAgentUObjectDataAssetCommandRequest& Request,
				const TSharedPtr<FJsonObject>& Params,
				FUeAgentUObjectDataAssetCommandResponse& OutResponse,
				FUeAgentUObjectDataAssetCommandError& OutError)
			{
				FPreparedImport Prepared;
				if (!TryPrepareImport(Params, Prepared, OutError))
				{
					return false;
				}
				// The registered types have native constructor defaults. Validate all
				// ordinary values before creating the root, including descendant values.
				// Prepare only reads defaults and decodes into private value storage;
				// never apply these plans to a CDO or reuse its bound addresses.
				for (const FNodeSpec& Node : Prepared.Nodes)
				{
					UObject* Defaults = Node.bRoot
						? static_cast<UObject*>(GetMutableDefault<UUeAgentRecursiveDataAsset>())
						: static_cast<UObject*>(GetMutableDefault<UUeAgentRecursiveOwnedObject>());
					UeAgentAuthoringObjectApply::FPreparedApply ValuePlan;
					FOperationLifecycleError ValueError;
					if (!PrepareValues(*Defaults, Node.Values, true, ValuePlan, ValueError))
					{
						return FailWithTerminalFacts(OutError, ValueError);
					}
				}
				const TSharedRef<FExecutionContext> Context =
					MakeShared<FExecutionContext>(MoveTemp(Prepared));
				const FOperationLifecyclePlan Plan(
					TEXT("t13:") + Request.RequestId,
					Context->Prepared.InitializationPlan->GetPlanFingerprint(),
					{Context->Prepared.Target},
					{{Context->Prepared.Target, TEXT("absent")}},
					{TEXT("asset_registered"), TEXT("owned_objects_created")});
				const FOperationImpactAuthorization Authorization(
					Plan.GetPlanFingerprint(),
					{Context->Prepared.Target});
				const FOperationLifecycleService Lifecycle(
					MakeShared<FVersionFence>(Context),
					MakeShared<FMutationPort>(Context),
					MakeShared<FStabilityPort>(Context),
					MakeShared<FPersistencePort>(Context),
					MakeShared<FIndependentVerificationPort>(Context));
				FOperationLifecycleReceipt Receipt;
				if (!Lifecycle.Run(Plan, Authorization, Receipt))
				{
					auto Diagnostic = Receipt.GetDiagnostic();
					if (Diagnostic.IsEmpty()) Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(
						TEXT("uobject_dataasset_operation_failed"), TEXT("The data asset operation did not complete."), TEXT("command_execute"));
					Diagnostic.TerminalStatus = LexToString(Receipt.GetStatus());
					Diagnostic.MutationStarted = Receipt.DidMutationStart();
					Diagnostic.bRetrySafe = Receipt.IsRetrySafe();
					if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Context->Prepared.Target;
					Context->Prepared.Locate(Diagnostic);
					return Fail(OutError, MoveTemp(Diagnostic), Context->Prepared.Target);
				}
				OutResponse.PayloadJson = Context->SuccessPayloadJson;
				OutResponse.bReadOnly = false;
				return true;
			}

			bool ExecuteExport(
				const FUeAgentUObjectDataAssetCommandRequest& Request,
				const TSharedPtr<FJsonObject>& Params,
				UUeAgentRecursiveDataAsset& Asset,
				FUeAgentUObjectDataAssetCommandResponse& OutResponse,
				FUeAgentUObjectDataAssetCommandError& OutError)
			{
				FString WorkspacePath;
				if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
					|| Normalize(WorkspacePath).IsEmpty())
				{
					return Fail(OutError, CommandInputFailure(TEXT("uobject_dataasset_export_path_required"),
						TEXT("The workspace_path parameter must be a non-empty export directory path."),
						Params, TEXT("workspace_path"), TEXT("non-empty directory path string"), Asset.GetPathName()));
				}
				UE::UAI::Contract::FDiagnostic RegistrationError;
				if (!UeAgentReflectionAuthoring::RegisterAuthoring(RegistrationError))
					return Fail(OutError, RegistrationError, Asset.GetPathName());
				const auto* Profile = UeAgentReflectionAuthoring::FindProfileByTypeId(ReflectionProfileId);
				if (!Profile) return Fail(OutError, NativeConditionFailure(TEXT("recursive_dataasset_profile_missing"),
					TEXT("The reflection profile for recursive data assets is not registered."),
					TEXT("reflection_profile_registered"), false, true, Asset.GetPathName()), ReflectionProfileId);
				FWorkspaceError WorkspaceError;
				if (!Composition::HjsonImport::TryExportReflectionRootWorkspace(
					*Profile, Asset.GetPathName(), WorkspacePath, OutResponse.PayloadJson, WorkspaceError))
					return FailWithTerminalFacts(OutError, WorkspaceError.GetDiagnostic());
				OutResponse.bReadOnly = true;
				return true;
			}
		};

		bool TryBuildDomainRequest(
			const FUeAgentUObjectDataAssetCommandRequest& Request,
			const FImportInitializationDomainProviderRegistry& Registry,
			TOptional<FImportInitializationDomainRequest>& OutRequest,
			FUeAgentUObjectDataAssetCommandError& OutError)
		{
			OutRequest.Reset();
			TSharedPtr<FJsonObject> Params;
			UE::UAI::Contract::FDiagnostic JsonError;
			if (!Json::TryParseObject(
					Request.CanonicalParamsJson,
					Params,
					JsonError))
			{
				return FailWithTerminalFacts(OutError, JsonError);
			}
			const FUeAgentUObjectDataAssetCommandRoute* Route =
				GetUObjectDataAssetCommandRoutes().FindByPredicate(
					[&Request](
						const FUeAgentUObjectDataAssetCommandRoute& Candidate)
					{
						return Candidate.CommandId == Request.CommandId;
					});
			if (!Route)
			{
				return Fail(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_domain_command_unregistered"),
					FString::Printf(TEXT("Command '%s' has no registered domain route."), *Request.CommandId), TEXT("command_execute")), Request.CommandId);
			}
			FString Target;
			if (!Params->TryGetStringField(TEXT("target"), Target)
				|| Normalize(Target).IsEmpty())
			{
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_domain_target_required"),
					TEXT("Command field 'target' must be a non-empty asset reference."), TEXT("command_execute"));
				Diagnostic.SourceKind = TEXT("command_input");
				Diagnostic.Source.JsonPath = TEXT("/target");
				Diagnostic.ActualValue = Params->Values.FindRef(TEXT("target"));
				return FailWithTerminalFacts(OutError, Diagnostic);
			}
			FString SchemaId = TEXT("uai.content_graph.resolved_target/1");
			FString RootTypeIdentity;
			TOptional<FSourceProvenance> SchemaSource;
			TOptional<FSourceProvenance> RootTypeSource;
			if (Route->Kind == EUeAgentUObjectDataAssetRouteKind::Import
				|| (Route->Kind == EUeAgentUObjectDataAssetRouteKind::Export && Params->HasField(TEXT("source"))))
			{
				if (Params->HasField(TEXT("carrier"))
					|| Params->HasField(TEXT("hjson_document")))
				{
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_graph_legacy_source_contract_rejected"),
						TEXT("Legacy carrier/hjson_document fields are unsupported; provide the document through 'source'."), TEXT("command_execute"));
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.Source.JsonPath = Params->HasField(TEXT("carrier")) ? TEXT("/carrier") : TEXT("/hjson_document");
					return FailWithTerminalFacts(OutError, Diagnostic);
				}
				const TSharedPtr<FJsonObject>* SourceObject = nullptr;
				if (!Params->TryGetObjectField(TEXT("source"), SourceObject)
					|| !SourceObject || !SourceObject->IsValid())
				{
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_graph_source_required"),
						TEXT("Command field 'source' must be an input source object."), TEXT("command_execute"));
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.Source.JsonPath = TEXT("/source");
					Diagnostic.ActualValue = Params->Values.FindRef(TEXT("source"));
					return FailWithTerminalFacts(OutError, Diagnostic);
				}
				FUeAgentResourceTracker Resources =
					MakeSemanticWorkspaceResourceTracker();
				FResolvedContentGraphImportSourcePtr ResolvedSource;
				FWorkspaceError WorkspaceError;
				FString SourceKind;
				const bool bHasSourceKind =
					(*SourceObject)->TryGetStringField(TEXT("kind"), SourceKind);
				FString ExpectedRevision;
				const bool bHasExpectedRevision =
					Params->TryGetStringField(
						TEXT("expected_revision"), ExpectedRevision);
				const bool bLoaded =
					(bHasSourceKind
						&& (SourceKind == TEXT("resolved")
							|| SourceKind == TEXT("semantic_workspace")))
					? TryLoadNormalizedContentGraphImportSource(
						*SourceObject,
						Target,
						bHasExpectedRevision ? ExpectedRevision : FString(),
						Resources,
						ResolvedSource,
						WorkspaceError)
					: TryLoadContentGraphImportSource(
						*SourceObject,
						Target,
						Resources,
						ResolvedSource,
						WorkspaceError);
				if (!bLoaded
					|| !ResolvedSource.IsValid())
				{
					return FailWithTerminalFacts(OutError, WorkspaceError.GetDiagnostic());
				}
				if (ResolvedSource->IsSemanticWorkspaceDirectory())
				{
					FResolvedSemanticWorkspacePtr Workspace;
					if (!TryLoadHjsonImportSource(FWorkspaceSource::Directory(ResolvedSource->GetSourceUnit()),
						ESemanticWorkspaceMode::State, Resources, Workspace, WorkspaceError)
						|| !Registry.TryResolveWorkspaceRootType(Workspace->GetModel().Get(), Target, RootTypeIdentity, WorkspaceError))
						return FailWithTerminalFacts(OutError, WorkspaceError.GetDiagnostic());
					if (RootTypeIdentity.IsEmpty())
					{
					UObject* TargetObject = StaticLoadObject(
						UObject::StaticClass(),
						nullptr,
						*Target);
					if (!TargetObject)
					{
						return FailUnresolvedTarget(
							OutError,
							TEXT("content_graph_semantic_target_unresolved"),
							Target, ResolvedSource->GetSourceUnit());
					}
					RootTypeIdentity = TargetObject->GetClass()->GetPathName();
					}
					SchemaId = GetSemanticWorkspaceSchemaId();
				}
				else
				{
					const FContentGraphImportDocument* EntryDocument =
						ResolvedSource->GetEntryDocument();
					if (!EntryDocument)
					{
						return Fail(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_graph_source_invalid"),
							TEXT("The resolved import source has no entry document."), TEXT("command_execute")), Target);
					}
					const FString& DocumentText =
						EntryDocument->GetDocument();
					FHjsonDocumentPtr Document;
					if (!TryParseHjsonDocument(
						DocumentText,
						ResolvedSource->GetSourceUnit(),
						Resources,
						Document,
						WorkspaceError))
				{
					return FailWithTerminalFacts(OutError, WorkspaceError.GetDiagnostic());
				}
					const auto FailDocument = [&](const TCHAR* Code, const TCHAR* Reason, const TCHAR* Pointer,
						const TSharedPtr<FJsonValue>& Actual = nullptr)
					{
						auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("import_initialization"));
						Diagnostic.Source.JsonPath = Pointer;
						Diagnostic.ActualValue = Actual;
						if (!Actual.IsValid())
						{
							const auto* Value = Document->Find(Pointer);
							Diagnostic.ActualValuePresent = Value != nullptr;
							if (Value && Value->GetKind() != EHjsonNodeKind::Object && Value->GetKind() != EHjsonNodeKind::Array)
							{
								FWorkspaceError ValueError;
								if (!TryProjectHjsonNodeToJsonValue(*Value, Resources, Diagnostic.ActualValue, ValueError))
									Diagnostic.Append(ValueError.GetDiagnostic());
							}
						}
						WorkspaceError.SetFailure(MoveTemp(Diagnostic));
						WorkspaceError.Locate(*Document->GetRoot());
						return FailWithTerminalFacts(OutError, WorkspaceError.GetDiagnostic());
					};
					const FHjsonNode& DocumentRoot = *Document->GetRoot();
					if (DocumentRoot.GetKind() != EHjsonNodeKind::Object)
						return FailDocument(TEXT("hjson_domain_identity_required"), TEXT("The import document root must be an object."), TEXT("/"));
					const FHjsonNode* Schema = DocumentRoot.Get(TEXT("schema"));
					const FHjsonNode* Root = DocumentRoot.Get(TEXT("root"));
					const FHjsonNode* Type = Root
						&& Root->GetKind() == EHjsonNodeKind::Object
						? Root->Get(TEXT("type"))
						: nullptr;
					const FHjsonNode* Candidate = Root && Root->GetKind() == EHjsonNodeKind::Object
						? Root->Get(TEXT("candidate")) : nullptr;
					if (!Schema || Schema->GetKind() != EHjsonNodeKind::String)
						return FailDocument(TEXT("hjson_domain_identity_required"), TEXT("Import field 'schema' must be a string."), TEXT("/schema"));
					if (!Root || Root->GetKind() != EHjsonNodeKind::Object)
						return FailDocument(TEXT("hjson_domain_identity_required"), TEXT("Import field 'root' must be an object."), TEXT("/root"));
					if (!Candidate && (!Type || Type->GetKind() != EHjsonNodeKind::String))
						return FailDocument(TEXT("hjson_domain_identity_required"), TEXT("Import root requires a string 'type' or a 'candidate' selector."), TEXT("/root/type"));
					SchemaId = Normalize(Schema->GetText());
					SchemaSource = Schema->GetProvenance();
					RootTypeSource = Candidate ? Candidate->GetProvenance() : Type->GetProvenance();
					if (Candidate)
					{
						if (Type) return FailDocument(TEXT("hjson_root_candidate_type_conflict"),
							TEXT("Import root cannot specify both 'candidate' and 'type'; choose one selector."), TEXT("/root/candidate"));
						TSharedPtr<FJsonValue> CandidateJson;
						DomainAuthoring::FDomainHjsonCandidateSelector Selector;
						if (!TryProjectHjsonNodeToJsonValue(*Candidate, Resources, CandidateJson, WorkspaceError))
						{
							WorkspaceError.Locate(*Candidate);
							return FailWithTerminalFacts(OutError, WorkspaceError.GetDiagnostic());
						}
						if (CandidateJson->Type != EJson::Object)
							return FailDocument(TEXT("hjson_root_candidate_invalid"),
								TEXT("Import root 'candidate' must be an object selector."), TEXT("/root/candidate"), CandidateJson);
						if (!DomainAuthoring::TryReadCandidateSelector(CandidateJson->AsObject(),
							TEXT("/root/candidate"), Selector, WorkspaceError))
						{
							WorkspaceError.Locate(*Document->GetRoot());
							return FailWithTerminalFacts(OutError, WorkspaceError.GetDiagnostic());
						}
						DomainAdapters::AssetDataSimpleEffect::FUeAgentAssetDataSimpleEffectFactoryBinding Binding;
						ExecutionMutation::FExecutionMutationError FactoryError;
						if (!Composition::SimpleAuthoring::TryResolveGenericAssetFactory(Selector.Name, Selector.Select, Binding, FactoryError))
						{
							WorkspaceError.SetFailure(FactoryError, FString(), TEXT("/root/candidate"));
							WorkspaceError.Locate(*Candidate);
							return FailWithTerminalFacts(OutError, WorkspaceError.GetDiagnostic());
						}
						RootTypeIdentity = Binding.GetConcreteClass()->GetPathName();
					}
					else RootTypeIdentity = Normalize(Type->GetText());
				}
				const TSharedRef<FJsonObject> NormalizedParams =
					MakeShared<FJsonObject>();
				NormalizedParams->Values = Params->Values;
				if (Route->Kind == EUeAgentUObjectDataAssetRouteKind::Export)
					NormalizedParams->SetStringField(TEXT("expected_revision"), TEXT("missing"));
				NormalizedParams->SetObjectField(
					TEXT("source"),
					ResolvedSource->MakeNormalizedSourceObject());
				FString NormalizedJson;
				UE::UAI::Contract::FDiagnostic NormalizedError;
				if (!Json::TryCanonicalize(
						NormalizedParams,
						NormalizedJson,
						NormalizedError))
				{
					return Fail(OutError, NormalizedError, Target);
				}
				OutRequest.Emplace(
					Request.CommandId,
					MoveTemp(SchemaId),
					MoveTemp(Target),
					MoveTemp(RootTypeIdentity),
					EInitializationNodeRole::RootTarget,
					MoveTemp(NormalizedJson), Request.RequestId, MoveTemp(SchemaSource), MoveTemp(RootTypeSource));
			}
			else
			{
				UE::UAI::ContentDiscovery::FContentDiscoveryError ResolveError;
				if (!DomainAdapters::ContentDiscovery::TryResolveEditorContentReference(Target, TEXT("asset"), ResolveError))
					return FailWithTerminalFacts(OutError, ResolveError);
				UObject* TargetObject = StaticLoadObject(
					UObject::StaticClass(),
					nullptr,
					*Target);
				if (!TargetObject)
				{
					return FailUnresolvedTarget(
						OutError,
						TEXT("content_graph_read_target_unresolved"),
						Target);
				}
				RootTypeIdentity = TargetObject->GetClass()->GetPathName();
				Params->SetStringField(TEXT("target"), Target);
			}
			if (!OutRequest.IsSet())
			{
				FString ResolvedParamsJson;
				UE::UAI::Contract::FDiagnostic CanonicalError;
				if (!Json::TryCanonicalize(Params, ResolvedParamsJson, CanonicalError))
					return Fail(OutError, CanonicalError, Target);
				OutRequest.Emplace(
					Request.CommandId,
					MoveTemp(SchemaId),
					MoveTemp(Target),
					MoveTemp(RootTypeIdentity),
					EInitializationNodeRole::RootTarget,
					MoveTemp(ResolvedParamsJson), Request.RequestId);
			}
			if (!OutRequest->IsValid())
			{
				const auto Diagnostic = OutRequest->GetValidationDiagnostic();
				OutRequest.Reset();
				return Fail(OutError, Diagnostic, Request.CommandId);
			}
			return true;
		}

		class FUObjectDataAssetDomainProvider final
			: public IImportInitializationDomainProvider
		{
		public:
			virtual bool SupportsNativeOwnedCreationContext(
				const FString& OwnerType, const FString& PropertyPath) const override
			{
				return (OwnerType == RootType && PropertyPath == TEXT("OwnedObjects"))
					|| (OwnerType == OwnedType && PropertyPath == TEXT("Children"));
			}
			virtual const FString& GetProviderId() const override
			{
				return DomainProviderId;
			}

			virtual bool Matches(
				const FImportInitializationDomainRequest& Request)
				const override
			{
				if (Request.GetRootTypeIdentity() != RootType
					&& Request.GetRootTypeIdentity() != OwnedType)
				{
					return false;
				}
				return MatchesDomainImportSchema(Request, ContractId);
			}

			virtual bool TryResolveInitializationContract(
				const FImportInitializationDomainRequest& Request,
				FString& OutInitializationContractId,
				FWorkspaceError& OutError) const override
			{
				OutInitializationContractId.Reset();
				OutError.Reset();
				if (Request.GetRole() == EInitializationNodeRole::RootTarget
					&& Request.GetRootTypeIdentity() == RootType)
				{
					OutInitializationContractId =
						RootInitializationContractId;
					return true;
				}
				if (Request.GetRole() == EInitializationNodeRole::OwnedObject
					&& Request.GetRootTypeIdentity() == OwnedType)
				{
					OutInitializationContractId =
						OwnedInitializationContractId;
					return true;
				}
				return false;
			}

			virtual bool TryExecute(
				const FImportInitializationDomainRequest& Request,
				const FString& InitializationContractId,
				FImportInitializationDomainResponse& OutResponse,
				FWorkspaceError& OutError) const override
			{
				OutResponse = {};
				OutError.Reset();
				if (InitializationContractId
					!= RootInitializationContractId)
				{
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_domain_contract_not_executable"),
						TEXT("The initialization contract does not support recursive data asset root execution."), TEXT("domain_execute"));
					Detail.TargetPath = Request.GetTargetIdentity();
					Detail.InternalContext = InitializationContractId;
					Detail.SourceKind = TEXT("native_result");
					OutError.SetFailure(MoveTemp(Detail));
					return false;
				}
				if (Request.GetCommandId() == TEXT("content_graph.import_workspace")
					&& Request.GetSchemaId() == GetSemanticWorkspaceSchemaId())
				{
					using namespace HjsonWorkspace::DomainAuthoring;
					UE::UAI::Contract::FDiagnostic RegistrationError;
					if (!UeAgentReflectionAuthoring::RegisterAuthoring(RegistrationError))
					{ OutError.SetFailure(RegistrationError, Request.GetTargetIdentity()); return false; }
					const auto* Profile = UeAgentReflectionAuthoring::FindProfileByTypeId(ReflectionProfileId);
					if (!Profile)
					{
						auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("recursive_dataasset_profile_missing"),
							TEXT("The recursive data asset authoring profile is not registered."), TEXT("domain_execute"));
						Detail.TargetPath = Request.GetTargetIdentity();
						Detail.InternalContext = ReflectionProfileId;
						Detail.SourceKind = TEXT("native_result");
						OutError.SetFailure(MoveTemp(Detail));
						return false;
					}
					const FString& Target = Request.GetTargetIdentity();
					FResolvedDomainHjsonDocument Document;
					if (!TryDecodeResolvedDocument(Target, Request.GetCanonicalParamsJson(),
						FDomainHjsonDocumentContract(ReflectionProfileId, TEXT("uai.recursive_dataasset.edit/1"), RootType),
						Document, OutError)) return false;
					if (!Document.bSemanticWorkspaceDirectory || !Document.SemanticWorkspace.IsValid())
					{
						auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("recursive_dataasset_semantic_workspace_required"),
							TEXT("Recursive data asset import requires a resolved exported semantic workspace directory."), TEXT("document_restore"));
						Detail.TargetPath = Target;
						Detail.SourceKind = TEXT("command_input");
						Detail.Source.JsonPath = TEXT("/source");
						OutError.SetFailure(MoveTemp(Detail));
						return false;
					}
					FString Payload;
					if (!Composition::HjsonImport::TryImportReflectionWorkspace(*Profile,
						Document.SemanticWorkspace.ToSharedRef(),
						{DomainProviderId, Target, ObjectPathFromPackageIdentity(Target), TEXT("recursive-dataasset-semantic"),
						 TEXT("recursive_dataasset_prepare_failed"), TEXT("recursive_dataasset_apply_failed"), TEXT("recursive_dataasset_readback_invalid"),
						 TEXT("recursive_dataasset_compile_not_settled")},
						[](TArray<TSharedPtr<FJsonValue>>&) {}, Payload, OutError)) return false;
					OutResponse = FImportInitializationDomainResponse(MoveTemp(Payload), false);
					if (!OutResponse.IsValid())
					{
						auto Diagnostic = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("uobject_dataasset_domain_response_invalid"),
							OutResponse.GetValidationDiagnostic(), TEXT("result_projection"), TEXT("The completed semantic import returned an invalid result payload."));
						Diagnostic.TerminalStatus = TEXT("state_unknown");
						Diagnostic.MutationMayHaveStarted = true;
						Diagnostic.bRetrySafe = false;
						OutError.SetFailure(MoveTemp(Diagnostic), Target);
						return false;
					}
					return true;
				}
				FUeAgentUObjectDataAssetCommandRequest DomainRequest;
				DomainRequest.CommandId = Request.GetCommandId();
				DomainRequest.RequestId = Sha256(
					Request.GetCommandId()
					+ TEXT("|")
					+ Request.GetCanonicalParamsJson());
				DomainRequest.CanonicalParamsJson =
					Request.GetCanonicalParamsJson();
				FUeAgentUObjectDataAssetCommandResponse DomainResponse;
				FUeAgentUObjectDataAssetCommandError DomainError;
				FProductionService Service;
				if (!Service.Execute(
						DomainRequest,
						DomainResponse,
						DomainError))
				{
					OutError.SetFailure(DomainError, Request.GetTargetIdentity());
					return false;
				}
				OutResponse = FImportInitializationDomainResponse(
					MoveTemp(DomainResponse.PayloadJson),
					DomainResponse.bReadOnly);
				if (!OutResponse.IsValid())
				{
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("uobject_dataasset_domain_response_invalid"),
						OutResponse.GetValidationDiagnostic(), TEXT("result_projection"), TEXT("The data asset command returned an invalid result payload."));
					Diagnostic.TerminalStatus = DomainResponse.bReadOnly ? TEXT("rejected") : TEXT("state_unknown");
					Diagnostic.MutationMayHaveStarted = !DomainResponse.bReadOnly;
					Diagnostic.bRetrySafe = DomainResponse.bReadOnly;
					OutError.SetFailure(MoveTemp(Diagnostic), Request.GetTargetIdentity());
					return false;
				}
				return true;
			}
		};

		bool TryCreateUObjectDataAssetDomainProviderInternal(
			TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
			FWorkspaceError& OutError)
		{
			OutProvider = MakeShared<FUObjectDataAssetDomainProvider>();
			OutError.Reset();
			return true;
		}

		class FRegistryRoutedProductionService final
			: public IUeAgentUObjectDataAssetCommandService
		{
		public:
			explicit FRegistryRoutedProductionService(
				TSharedRef<const FImportInitializationDomainProviderRegistry>
					InRegistry)
				: Registry(MoveTemp(InRegistry))
			{
			}

			virtual bool Execute(
				const FUeAgentUObjectDataAssetCommandRequest& Request,
				FUeAgentUObjectDataAssetCommandResponse& OutResponse,
				FUeAgentUObjectDataAssetCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return Fail(OutError, NativeConditionFailure(TEXT("uobject_dataasset_cancelled"),
						TEXT("The data asset command was cancelled before execution."), TEXT("cancellation_requested"), true, false), Request.CommandId);
				}
				TOptional<FImportInitializationDomainRequest> DomainRequest;
				if (!TryBuildDomainRequest(Request, Registry.Get(), DomainRequest, OutError))
				{
					return false;
				}
				FImportInitializationDomainResolution Resolution;
				FWorkspaceError WorkspaceError;
				if (!Registry->TryResolve(
						DomainRequest.GetValue(),
						Resolution,
						WorkspaceError))
				{
					if (Request.CommandId
							== TEXT("content_graph.export_workspace")
						&& DomainRequest->GetSchemaId() == TEXT("uai.content_graph.resolved_target/1")
						&& WorkspaceError.GetCode()
							== TEXT("hjson_domain_provider_unresolved"))
					{
						return ExecuteSharedExport(
							Request, OutResponse, OutError);
					}
					auto Diagnostic = WorkspaceError.GetDiagnostic();
					if (Diagnostic.IsEmpty()) Diagnostic = NativeConditionFailure(TEXT("hjson_domain_provider_resolution_failed"),
						TEXT("The data asset domain provider could not be resolved and no diagnostic was provided."),
						TEXT("domain_provider_resolved"), false, true, DomainRequest->GetTargetIdentity(), TEXT("provider_resolution"));
					return FailWithTerminalFacts(OutError, Diagnostic);
				}
				const FUeAgentUObjectDataAssetCommandRoute* ActiveRoute =
					GetUObjectDataAssetCommandRoutes().FindByPredicate(
						[&Request](
							const FUeAgentUObjectDataAssetCommandRoute& Candidate)
						{
							return Candidate.CommandId == Request.CommandId;
						});
				if (!ActiveRoute)
				{
					return Fail(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_domain_command_unregistered"),
						FString::Printf(TEXT("Command '%s' has no registered domain route."), *Request.CommandId), TEXT("command_execute")), Request.CommandId);
				}
				if (Request.CommandId == TEXT("content_graph.export_workspace")
					&& DomainRequest->GetSchemaId() != TEXT("uai.content_graph.resolved_target/1")
					&& !Resolution.GetProvider()->SupportsWorkspacePreparation())
				{
					return Fail(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_workspace_preparation_unsupported"),
						TEXT("The selected target domain does not support preparing an authoring workspace."), TEXT("command_execute"),
						Resolution.GetProvider()->GetProviderId()), DomainRequest->GetTargetIdentity());
				}
				if (ActiveRoute->bRequiresExplicitDomainSupport
					&& !Resolution.GetProvider()->SupportsExplicitCommand(
						Request.CommandId))
				{
					if (Request.CommandId
						== TEXT("content_graph.export_workspace"))
					{
						return ExecuteSharedExport(
							Request, OutResponse, OutError);
					}
					return Fail(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_domain_command_capability_unavailable"),
						FString::Printf(TEXT("The selected target domain does not support command '%s'."), *Request.CommandId),
						TEXT("command_execute"), Resolution.GetProvider()->GetProviderId()), DomainRequest->GetTargetIdentity());
				}
				FImportInitializationDomainResponse DomainResponse;
				if (!Resolution.GetProvider()->TryExecute(
						DomainRequest.GetValue(),
						Resolution.GetInitializationContractId(),
						DomainResponse,
						WorkspaceError))
				{
					if (!WorkspaceError.GetDiagnostic().IsEmpty())
						return Fail(OutError, WorkspaceError.GetDiagnostic());
					return Fail(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_domain_execution_diagnostic_missing"),
						FString::Printf(TEXT("The selected domain provider failed to execute command '%s' without reporting a reason."), *Request.CommandId),
						TEXT("command_execute"), Resolution.GetProvider()->GetProviderId()), DomainRequest->GetTargetIdentity());
				}
				if (!DomainResponse.IsValid())
				{
					auto Diagnostic = DomainResponse.GetValidationDiagnostic();
					if (!WorkspaceError.GetDiagnostic().IsEmpty()) Diagnostic.Append(WorkspaceError.GetDiagnostic());
					return Fail(OutError, MoveTemp(Diagnostic), Resolution.GetProvider()->GetProviderId());
				}
				OutResponse.PayloadJson = DomainResponse.GetPayloadJson();
				OutResponse.bReadOnly = DomainResponse.IsReadOnly();
				return true;
			}

		private:
			bool ExecuteSharedExport(
				const FUeAgentUObjectDataAssetCommandRequest& Request,
				FUeAgentUObjectDataAssetCommandResponse& OutResponse,
				FUeAgentUObjectDataAssetCommandError& OutError) const
			{
				TSharedPtr<FJsonObject> Params;
				UE::UAI::Contract::FDiagnostic JsonError;
				FString Target;
				FString WorkspacePath;
				FString ProfileId(TEXT("llm_authoring"));
				if (!Json::TryParseObject(
						Request.CanonicalParamsJson, Params, JsonError)
					|| !Params.IsValid()
					|| !Params->TryGetStringField(TEXT("target"), Target)
					|| !Params->TryGetStringField(
						TEXT("workspace_path"), WorkspacePath))
				{
					return FailWithTerminalFacts(OutError, JsonError);
				}
				Params->TryGetStringField(TEXT("profile_id"), ProfileId);
				Target = Normalize(Target);
				UE::UAI::ContentDiscovery::FContentDiscoveryError ResolveError;
				if (!DomainAdapters::ContentDiscovery::TryResolveEditorContentReference(Target, TEXT("asset"), ResolveError))
					return FailWithTerminalFacts(OutError, ResolveError);
				WorkspacePath = Normalize(WorkspacePath);
				ProfileId = Normalize(ProfileId);
				FString LoadPath = Target;
				if (!LoadPath.Contains(TEXT(".")))
				{
					const FString AssetName =
						FPackageName::GetLongPackageAssetName(LoadPath);
					LoadPath = AssetName.IsEmpty()
						? LoadPath : LoadPath + TEXT(".") + AssetName;
				}
				UObject* Object = StaticLoadObject(
					UObject::StaticClass(), nullptr, *LoadPath);
				if (!Object)
					return FailUnresolvedTarget(OutError, TEXT("content_graph_read_target_unresolved"), Target, WorkspacePath);
				if (WorkspacePath.IsEmpty() || ProfileId.IsEmpty())
				{
					const TCHAR* Field = WorkspacePath.IsEmpty() ? TEXT("workspace_path") : TEXT("profile_id");
					auto Error = CommandInputFailure(TEXT("content_graph_export_path_required"),
						WorkspacePath.IsEmpty() ? TEXT("A nonempty workspace path is required for content graph export.")
							: TEXT("A nonempty profile ID is required for content graph export."),
						Params, Field, TEXT("nonempty string"), Target);
					Error.Stage = TEXT("command_execute");
					return Fail(OutError, MoveTemp(Error), Target);
				}

				TSharedPtr<const DomainCapability::FFrozenDomainCapabilitySet>
					FrozenCapabilities;
				UE::UAI::Contract::FDiagnostic CapabilityError;
				if (!DomainCapability::TryFreezeProcessDomainCapabilities(
						FrozenCapabilities, CapabilityError)
					|| !FrozenCapabilities.IsValid())
				{
					return Fail(OutError, CapabilityError, Target);
				}
				DomainCapability::FDomainCapabilityAuthorities Authorities;
				if (!FrozenCapabilities->TryBuildAuthorities(
						Authorities, CapabilityError))
				{
					return Fail(OutError, CapabilityError, Target);
				}
				const FString ClassPath = Object->GetClass()->GetPathName();
				FString TargetKind;
				for (const TSharedRef<const DomainCapability::
					FDomainCapabilityContribution>& Contribution
					: FrozenCapabilities->GetContributions())
				{
					for (const DomainCapability::FDomainHjsonRoute& Route
						: Contribution->GetDeclaration().Hjson.Routes)
					{
						if (Route.CommandId
								== TEXT("content_graph.import_workspace")
							&& Route.RootTypeIdentity == ClassPath)
						{
							if (!TargetKind.IsEmpty()
								&& TargetKind != Route.TargetKind)
							{
								auto Actual = MakeShared<FJsonObject>();
								Actual->SetStringField(TEXT("class"), ClassPath);
								TArray<TSharedPtr<FJsonValue>> Kinds{MakeShared<FJsonValueString>(TargetKind), MakeShared<FJsonValueString>(Route.TargetKind)};
								Actual->SetArrayField(TEXT("target_kinds"), Kinds);
								auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(
									TEXT("content_graph_export_target_kind_ambiguous"), TEXT("target_kind"),
									MakeShared<FJsonValueString>(TEXT("one target kind per root class")),
									MakeShared<FJsonValueObject>(Actual), TEXT("command_execute"),
									TEXT("The target class has conflicting content graph workspace routes."));
								Error.SourceKind = TEXT("native_result");
								return Fail(OutError, MoveTemp(Error), Target);
							}
							TargetKind = Route.TargetKind;
						}
					}
				}
				if (TargetKind.IsEmpty())
				{
					auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(
						TEXT("content_graph_export_target_kind_unresolved"), TEXT("target_class"),
						MakeShared<FJsonValueString>(TEXT("class with a registered content_graph.import_workspace route")),
						MakeShared<FJsonValueString>(ClassPath), TEXT("command_execute"),
						TEXT("No content graph workspace route identifies a target kind for this class."));
					Error.SourceKind = TEXT("native_result");
					return Fail(OutError, MoveTemp(Error), Target);
				}

				DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture
					Capture;
				DomainAdapters::ContentGraphRead::FUeAgentContentGraphReadError
					CaptureError;
				if (!DomainAdapters::ContentGraphRead::
						FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
							*Object, Capture, CaptureError))
				{
					return FailWithTerminalFacts(OutError, CaptureError);
				}

				const FUeAgentResourceTrackerOwner Resources =
					MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
				TSharedPtr<const StateSemantic::FUeAgentCanonicalSnapshot> Snapshot;
				if (!DomainAdapters::ContentGraphRead::
						TryBuildContentGraphAuthoringSnapshot(
							Capture,
							Resources,
							Snapshot,
							CaptureError)
					|| !Snapshot.IsValid())
				{
					return FailWithTerminalFacts(OutError, CaptureError);
				}

				FExportWorkspacePlanPtr Plan;
				FWorkspaceError WorkspaceError;
				if (!TryBuildSnapshotExportPlan(
						FSnapshotCompositionRequest(
							TargetKind, ProfileId, Snapshot.ToSharedRef()),
						{{TEXT("state_entry_count"), Snapshot->GetEntries().Num()}},
						*Resources,
						Plan,
						WorkspaceError)
					|| !Plan.IsValid())
				{
					return FailWithTerminalFacts(OutError, WorkspaceError.GetDiagnostic());
				}
				FArtifactPublishReceipt Receipt;
				FArtifactPublishError PublishError;
				if (!TryPublishExportPlan(
						FArtifactPublishRequest(
							Plan.ToSharedRef(), WorkspacePath,
							TEXT("content_graph.export_workspace")),
						*Resources,
						Receipt,
						PublishError))
				{
					OutError = PublishError;
					return false;
				}
				OutResponse.Warnings = Receipt.GetCleanupWarnings();
				const TSharedPtr<FJsonObject> Payload =
					UeAgentExportWorkspaceWriter::
						MakePublicWorkspaceSuccessResult(OutResponse.Warnings);
				UE::UAI::Contract::FDiagnostic ResultEncodingError;
				if (!Json::TryCanonicalize(Payload, OutResponse.PayloadJson, ResultEncodingError))
				{
					OutError = Receipt.MakeResultEncodingFailure(ResultEncodingError);
					return false;
				}
				OutResponse.bReadOnly = true;
				return true;
			}

			TSharedRef<const FImportInitializationDomainProviderRegistry>
				Registry;
		};
	}

	bool FUeAgentUObjectDataAssetCommandRoute::IsValid() const
	{
		UE::UAI::Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentUObjectDataAssetCommandRoute::TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (CommandId.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("uobject_dataasset_route_invalid"),
				TEXT("The route requires a non-empty command_id."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("command_id");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty string"));
			OutError.ActualValue = MakeShared<FJsonValueString>(CommandId);
			return false;
		}
		if (PythonFunction.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("uobject_dataasset_route_invalid"),
				TEXT("The route requires a non-empty python_function."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("python_function");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty string"));
			OutError.ActualValue = MakeShared<FJsonValueString>(PythonFunction);
			return false;
		}
		if (MechanicsCommandId.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("uobject_dataasset_route_invalid"),
				TEXT("The route requires a non-empty mechanics_command_id."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("mechanics_command_id");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty string"));
			OutError.ActualValue = MakeShared<FJsonValueString>(MechanicsCommandId);
			return false;
		}
		return true;
	}

	const TArray<FUeAgentUObjectDataAssetCommandRoute>&
	GetUObjectDataAssetCommandRoutes()
	{
		static const TArray<FUeAgentUObjectDataAssetCommandRoute> Routes{
			{TEXT("content_graph.export_workspace"), TEXT("content_graph_export_workspace"), TEXT("content_graph.export.verified"), EUeAgentUObjectDataAssetRouteKind::Export, true},
			{TEXT("content_graph.import_workspace"), TEXT("content_graph_import_workspace"), TEXT("content_graph.import.recursive"), EUeAgentUObjectDataAssetRouteKind::Import, true},
		};
		return Routes;
	}

	bool TryCreateUObjectDataAssetDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		return TryCreateUObjectDataAssetDomainProviderInternal(
			OutProvider,
			OutError);
	}

	bool TryCreateProductionUObjectDataAssetCommandService(
		TSharedPtr<IUeAgentUObjectDataAssetCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		TSharedPtr<const FImportInitializationDomainProviderRegistry>
			Registry;
		FWorkspaceError WorkspaceError;
		if (!TryFreezeProcessImportInitializationDomainProviders(
				Registry,
				WorkspaceError)
			|| !Registry.IsValid())
		{
			OutService.Reset();
			OutError = WorkspaceError.GetDiagnostic();
			if (OutError.IsEmpty()) OutError = NativeConditionFailure(TEXT("hjson_domain_provider_registry_unavailable"),
				TEXT("The HJSON domain provider registry was not created and no diagnostic was provided."),
				TEXT("domain_provider_registry_ready"), false, true, FString(), TEXT("authoring"));
			return false;
		}
		OutService = MakeShared<FRegistryRoutedProductionService>(
			Registry.ToSharedRef());
		return true;
	}

	namespace TestHooks
	{
		FScopedFailureAfterNode::FScopedFailureAfterNode(FString NodeId)
			: Previous(FailureAfterNode)
		{
			FailureAfterNode = Normalize(MoveTemp(NodeId));
		}

		FScopedFailureAfterNode::~FScopedFailureAfterNode()
		{
			FailureAfterNode = MoveTemp(Previous);
		}

		FScopedFinalProjectionFailure::FScopedFinalProjectionFailure(TFunction<void(const UObject&)> ObserveReloaded)
			: bPrevious(bFailFinalProjection)
			, PreviousObserver(MoveTemp(FinalProjectionObserver))
		{
			bFailFinalProjection = true;
			FinalProjectionObserver = MoveTemp(ObserveReloaded);
		}

		FScopedFinalProjectionFailure::~FScopedFinalProjectionFailure()
		{
			bFailFinalProjection = bPrevious;
			FinalProjectionObserver = MoveTemp(PreviousObserver);
		}
	}
}
