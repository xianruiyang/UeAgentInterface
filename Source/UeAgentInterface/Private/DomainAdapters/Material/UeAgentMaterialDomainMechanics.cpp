#include "DomainAdapters/Material/UeAgentMaterialDomainMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringApplyProvider.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringDescriptors.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringExport.h"
#include "Domains/Material/Authoring/UeAgentMaterialGraphAuthoring.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringRegistration.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialFunctionMaterialLayerBlend.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "OperationOwners/MaterialCommands/UeAgentMaterialCommandOperationOwner.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::Material
{
	using namespace CommandContract;
	using namespace ContextResource;
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		const FString SchemaId = TEXT("uai.material.edit/1");
		const FString MaterialType = TEXT("/Script/Engine.Material");
		const FString FunctionType = TEXT("/Script/Engine.MaterialFunction");
		const FString LayerType = TEXT("/Script/Engine.MaterialFunctionMaterialLayer");
		const FString BlendType = TEXT("/Script/Engine.MaterialFunctionMaterialLayerBlend");
		const FString InstanceType = TEXT("/Script/Engine.MaterialInstanceConstant");

		bool Fail(FWorkspaceError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic,
            const FString& Source = FString(), const FString& Pointer = TEXT("/"))
        {
            OutError.SetFailure(MoveTemp(Diagnostic), Source, Pointer);
            return false;
        }

		FString PointerKey(const FString& Key)
		{ return Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")); }

		void CompleteNative(UE::UAI::Contract::FDiagnostic& Error, const FString& Target)
		{
			if (Error.SourceKind.IsEmpty()) Error.SourceKind = TEXT("native_result");
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target;
		}

		UE::UAI::Contract::FDiagnostic ValueFailure(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Field, const TSharedPtr<FJsonValue>& Actual,
			const TSharedPtr<FJsonValue>& Expected, const FString& SourceKind = TEXT("native_result"))
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
			Error.TargetPath = Target;
			Error.PropertyPath = Field;
			Error.SourceKind = SourceKind;
			Error.ActualValue = Actual;
			Error.ExpectedValue = Expected;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			if (SourceKind != TEXT("native_result")) Error.Source.JsonPath = Field;
			return Error;
		}

		UE::UAI::Contract::FDiagnostic NativeCondition(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Field, bool Actual = false, bool Expected = true)
		{
			return ValueFailure(Code, Reason, Target, Field, MakeShared<FJsonValueBoolean>(Actual), MakeShared<FJsonValueBoolean>(Expected));
		}

		UE::UAI::Contract::FDiagnostic InternalCondition(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Field, const FString& Identity)
		{
			auto Error = NativeCondition(Code, Reason, Target, Field);
			Error.ActualEvidence = MakeShared<FJsonValueString>(Identity);
			return Error;
		}

		TSharedPtr<FJsonValue> JsonFact(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid()) return nullptr;
			if (Value->Type == EJson::Object || Value->Type == EJson::Array)
				return MakeShared<FJsonValueString>(Value->Type == EJson::Array ? TEXT("array") : TEXT("object"));
			return Value;
		}

		bool TryBuildMaterialGraphDocument(
			const HjsonWorkspace::FAuthoringModelRef& Model,
			TSharedPtr<FJsonObject>& OutGraph,
			UeAgentMaterialAuthoring::FGraphApplyContext& OutContext,
			FWorkspaceError& OutError,
			const FResolvedSemanticWorkspace* Workspace, const FString& Target)
		{
			OutGraph.Reset();
			OutContext = {};
			OutContext.Policy = UeAgentMaterialAuthoring::EGraphApplyPolicy::PreserveExisting;
			for (const HjsonWorkspace::FSemanticUnit& Unit : Model->GetUnits())
			{
				if (Unit.GetSemanticTypeRef() != UeAgentMaterialAuthoring::ExportGraphSemanticId)
					continue;
				TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
				auto Connections = MakeShared<FGraphAuthoringConnectionContext>();
				if (!TryProjectGraphAuthoringConsumerDocument(
						Unit.GetUnitId(), Unit.GetValues(), Graph, OutError, Workspace, &Connections.Get()))
				{
					return false;
				}
				const auto* ResolvedNodes = Unit.GetValues().Find(TEXT("nodes"));
				if (!ResolvedNodes || (*ResolvedNodes)->GetKind() != StateSemantic::EUeAgentValueKind::Object)
					return Fail(OutError, NativeCondition(TEXT("material_resolved_nodes_missing"),
						ResolvedNodes ? TEXT("The resolved material node set is not an object.") : TEXT("The resolved material graph has no node set."),
						Target, TEXT("/nodes/object_present")));
				for (const auto& Item : (*ResolvedNodes)->GetObjectItems())
				{
					TSharedPtr<FJsonValue> NodeValue; UE::UAI::Contract::FDiagnostic Error;
					if (!StateSemantic::CanonicalJson::TryToJsonValue(Item.Value.Get(), NodeValue, Error)
						|| !NodeValue.IsValid() || NodeValue->Type != EJson::Object)
						{
						if (Error.IsEmpty()) Error = NativeCondition(TEXT("material_node_binding_invalid"),
							TEXT("The resolved material node binding did not project to an object."), Target,
							TEXT("/nodes/") + PointerKey(Item.Key) + TEXT("/object_present"));
						CompleteNative(Error, Target);
						if (Error.CandidateName.IsEmpty()) Error.CandidateName = Item.Key;
						return Fail(OutError, MoveTemp(Error));
					}
					const auto Node = NodeValue->AsObject(); FString Id, Operation;
					Node->TryGetStringField(TEXT("id"), Id); Node->TryGetStringField(TEXT("operation"), Operation);
					if (Id == TEXT("material_root") || Operation == TEXT("create") || Operation == TEXT("remove")) continue;
					UeAgentMaterialAuthoring::FGraphExpressionBinding Binding;
					if (!FGuid::Parse(Id, Binding.Guid))
					{
						auto BindingError = NativeCondition(TEXT("material_node_binding_identity_invalid"),
							TEXT("The existing material node binding has no valid native expression identity."), Target,
							TEXT("/nodes/") + PointerKey(Item.Key) + TEXT("/native_identity_valid"));
						BindingError.CandidateName = Item.Key;
						BindingError.ActualEvidence = MakeShared<FJsonValueString>(Id);
						return Fail(OutError, MoveTemp(BindingError));
					}
					const TSharedPtr<FJsonObject>* Context = nullptr;
					if (Node->TryGetObjectField(TEXT("candidate_context"), Context) && Context && Context->IsValid())
					{
						(*Context)->TryGetStringField(TEXT("expression_class"), Binding.ClassPath);
						(*Context)->TryGetStringField(TEXT("material_function_path"), Binding.FunctionPath);
					}
					OutContext.ExpressionsByAlias.Add(Item.Key, MoveTemp(Binding));
				}
				OutGraph = Graph;
				OutContext.Connections = MoveTemp(Connections);
				return true;
			}
			return Fail(OutError, NativeCondition(TEXT("material_semantic_graph_unit_missing"),
				TEXT("The semantic model contains no material graph unit."), Target, TEXT("/material_graph_unit_present")));
		}

		bool AppendResolvedExpressionProperties(
			const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
			const Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
			const TSharedPtr<FJsonObject>& Graph,
			const UeAgentMaterialAuthoring::FGraphApplyContext& Context, FWorkspaceError& OutError, const FString& Target)
		{
			const TSharedPtr<FJsonObject>* Nodes = nullptr;
			if (!Graph->TryGetObjectField(TEXT("nodes"), Nodes) || !Nodes || !Nodes->IsValid())
				return Fail(OutError, NativeCondition(TEXT("material_semantic_nodes_missing"),
					TEXT("The projected material graph has no nodes object."), Target, TEXT("/nodes/object_present")));
			TMap<FGuid, const FString*> AliasesByGuid;
			TSet<FGuid> AmbiguousGuids;
			for (const auto& Binding : Context.ExpressionsByAlias)
			{
				if (AliasesByGuid.Contains(Binding.Value.Guid)) AmbiguousGuids.Add(Binding.Value.Guid);
				else AliasesByGuid.Add(Binding.Value.Guid, &Binding.Key);
			}
			for (const auto& Pair : Projection.PropertyChangesByUnitId)
			{
				const auto* Unit = Prepared.GetWorkspace()->GetModel()->FindUnit(Pair.Key);
				if (!Unit) return Fail(OutError, InternalCondition(TEXT("material_property_unit_missing"),
					TEXT("A material property change refers to a semantic unit absent from the prepared model."),
					Target, TEXT("/property_change/unit_present"), Pair.Key));
				const auto* Change = Prepared.FindChangeForTargetIdentity(Unit->GetTargetIdentityKey());
				if (!Change) return Fail(OutError, InternalCondition(TEXT("material_property_change_missing"),
					TEXT("The material property unit has no matching prepared change."), Target, TEXT("/property_change/prepared_change_present"),
					Unit->GetTargetIdentityKey()));
				const FString ExpressionId = Change->GetIdentity().GetSelector(TEXT("expression_guid"));
				if (ExpressionId.IsEmpty()) continue; // Root descriptors use the original reflection wrapper.
				FGuid Guid;
				if (!FGuid::Parse(ExpressionId, Guid)) return Fail(OutError, InternalCondition(TEXT("material_property_identity_invalid"),
					TEXT("The prepared property change has an invalid native expression identity."),
					Target, TEXT("/property_change/native_expression_identity_valid"), ExpressionId));
				if (AmbiguousGuids.Contains(Guid))
				{
					TArray<TSharedPtr<FJsonValue>> Aliases;
					for (const auto& Binding : Context.ExpressionsByAlias)
						if (Binding.Value.Guid == Guid) Aliases.Add(MakeShared<FJsonValueString>(Binding.Key));
					auto Error = ValueFailure(TEXT("material_property_identity_ambiguous"),
						TEXT("The native expression identity is shared by multiple material node aliases."), Target,
						TEXT("/property_change/matching_node_count"), MakeShared<FJsonValueNumber>(Aliases.Num()), MakeShared<FJsonValueNumber>(1));
					Error.CandidateChoices = MakeShared<FJsonValueArray>(MoveTemp(Aliases));
					Error.ActualEvidence = MakeShared<FJsonValueString>(ExpressionId);
					return Fail(OutError, MoveTemp(Error));
				}
				const auto* AliasEntry = AliasesByGuid.Find(Guid);
				const FString* Alias = AliasEntry ? *AliasEntry : nullptr;
				const TSharedPtr<FJsonObject>* Node = nullptr;
				if (!Alias || !(*Nodes)->TryGetObjectField(*Alias, Node) || !Node || !Node->IsValid())
					{
					auto Error = InternalCondition(TEXT("material_property_expression_missing"),
						Alias ? TEXT("The property change's material node alias has no projected node object.")
							: TEXT("The property change's native expression identity has no bound material node alias."),
						Target, Alias ? TEXT("/nodes/") + PointerKey(*Alias) + TEXT("/object_present") : TEXT("/property_change/node_alias_present"), ExpressionId);
					if (Alias) Error.CandidateName = *Alias;
					return Fail(OutError, MoveTemp(Error));
				}
				const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
				(*Node)->TryGetArrayField(TEXT("properties"), Existing);
				TArray<TSharedPtr<FJsonValue>> Properties = Existing ? *Existing : TArray<TSharedPtr<FJsonValue>>();
				Properties.Add(Pair.Value);
				(*Node)->SetArrayField(TEXT("properties"), MoveTemp(Properties));
			}
			return true;
		}

		bool IsSupportedRootType(const FString& Type)
		{
			return Type == MaterialType || Type == FunctionType || Type == LayerType || Type == BlendType
				|| Type == InstanceType;
		}

		FString ObjectPathFromPackage(const FString& Target)
		{
			return ObjectPathFromPackageIdentity(Target);
		}

		bool CanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FString& Out,
			FWorkspaceError& OutError)
		{
			UE::UAI::Contract::FDiagnostic Error;
			if (!Object.IsValid() || !Json::TryCanonicalize(Object, Out, Error))
			{
				if (Error.IsEmpty()) Error = NativeCondition(TEXT("material_domain_json_invalid"),
					TEXT("The generated material response is unavailable for JSON encoding."), FString(), TEXT("/response_present"));
				CompleteNative(Error, FString());
				return Fail(OutError, MoveTemp(Error));
			}
			return true;
		}

		bool EnsureAuthoringRegistered(FWorkspaceError& OutError)
		{
			static const TPair<bool, UE::UAI::Contract::FDiagnostic> Registration = []()
			{
				UE::UAI::Contract::FDiagnostic Error;
				const bool bRegistered =
					UeAgentMaterialAuthoring::RegisterAuthoring(Error);
				return TPair<bool, UE::UAI::Contract::FDiagnostic>(bRegistered, MoveTemp(Error));
			}();
			if (!Registration.Key)
			{
				auto Error = Registration.Value.IsValid() ? Registration.Value : NativeCondition(TEXT("material_domain_authoring_registration_failed"),
					TEXT("The material authoring provider could not register its dependencies."), FString(), TEXT("/authoring_registered"));
				CompleteNative(Error, FString());
				return Fail(OutError, MoveTemp(Error));
			}
			return true;
		}

		UClass* AssetClassForRootType(const FString& RootType)
		{
			if (RootType == MaterialType) return UMaterial::StaticClass();
			if (RootType == FunctionType) return UMaterialFunction::StaticClass();
			if (RootType == LayerType) return UMaterialFunctionMaterialLayer::StaticClass();
			if (RootType == BlendType) return UMaterialFunctionMaterialLayerBlend::StaticClass();
			if (RootType == InstanceType)
			{
				return UMaterialInstanceConstant::StaticClass();
			}
			return nullptr;
		}

		FString AssetKindForRootType(const FString& RootType)
		{
			if (RootType == MaterialType) return TEXT("material");
			if (RootType == FunctionType || RootType == LayerType || RootType == BlendType) return TEXT("material_function");
			if (RootType == InstanceType) return TEXT("material_instance");
			return FString();
		}

		FString PackagePathFromIdentity(const FString& Target)
		{
			return Target.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Target)
				: Target;
		}

		bool SubmitMaterialCommand(
			const FString& CommandId,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError,
			const FResolvedDomainHjsonDocument& Document, const FString& SourcePrefix)
		{
			FScopedDomainHjsonDiagnosticSource NativeSource(Document, OutError, SourcePrefix);
			FString ParamsJson;
			if (!CanonicalObject(Params, ParamsJson, OutError)) return false;
			UE::UAI::Contract::FDiagnostic CommandDiagnostic;
			if (!OperationOwners::MaterialCommands::TryExecuteInternal(
					CommandId, ParamsJson,
					Contract::FUeAgentCallerContext(
					TEXT("material-domain-provider"),
					{TEXT("uai:query"), TEXT("uai:operate"),
						TEXT("material_niagara_graph.read"),
						TEXT("material_niagara_graph.write")},
					TEXT("material-domain-provider")),
					OutPayload, CommandDiagnostic))
			{
				if (CommandDiagnostic.IsEmpty()) CommandDiagnostic = NativeCondition(TEXT("material_domain_command_failed"),
					TEXT("The delegated material command reported failure without a diagnostic."),
					Document.TargetIdentity, TEXT("/delegated_command_succeeded"));
				if (CommandDiagnostic.TargetPath.IsEmpty()) CommandDiagnostic.TargetPath = Document.TargetIdentity;
				if (CommandDiagnostic.CandidateName.IsEmpty()) CommandDiagnostic.CandidateName = CommandId;
				OutError.SetFailure(MoveTemp(CommandDiagnostic));
				return false;
			}
			return true;
		}

		bool TryRead(
			const FString& Target,
			const FString& RootType,
			FString& OutPayload,
			FString& OutRevision,
			TSharedPtr<FJsonObject>* OutSnapshot,
			FWorkspaceError& OutError)
		{
			UObject* Owner = nullptr;
			FUeAgentContentGraphCapture Capture;
			if (!TryCaptureObject(
					Target, RootType, Owner, Capture, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Snapshot;
			if (!TryBuildCaptureSnapshot(
					Target, RootType, Capture, Snapshot, OutError))
			{
				return false;
			}
			OutRevision = Capture.Revision;
			if (OutSnapshot)
			{
				*OutSnapshot = Snapshot;
			}
			return TryCanonicalizeObject(Snapshot, OutPayload, OutError);
		}

		bool TryImportSemanticWorkspace(
			const FString& Target,
			const FString& RootType,
			const FResolvedDomainHjsonDocument& Document,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			using namespace UE::UAI::Composition::HjsonImport;

			if (!Document.SemanticWorkspace.IsValid())
			{
				return Fail(OutError, NativeCondition(TEXT("material_semantic_workspace_missing"),
					TEXT("The resolved material import has no semantic workspace."), Target, TEXT("/semantic_workspace_present")));
			}
			if (!EnsureAuthoringRegistered(OutError)) return false;

			UClass* AssetClass = AssetClassForRootType(RootType);
			UObject* Asset = AssetClass
				? StaticLoadObject(
					AssetClass, nullptr, *ObjectPathFromPackage(Target))
				: nullptr;
			if (!Asset || Asset->GetClass() != AssetClass)
			{
				return Fail(OutError, ValueFailure(TEXT("material_domain_target_unresolved"),
					Asset ? TEXT("The loaded material target has a different native class.") : TEXT("The material target could not be loaded."),
					Target, TEXT("/asset_class"), Asset ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Asset->GetClass()->GetPathName())) : TSharedPtr<FJsonValue>(),
					MakeShared<FJsonValueString>(AssetClass ? AssetClass->GetPathName() : RootType)));
			}

			UeAgentMaterialAuthoring::FMaterialAuthoringCapture Capture;
			UE::UAI::Contract::FDiagnostic CaptureError;
			if (!UeAgentMaterialAuthoring::TryCaptureMaterialAuthoringState(
					*Asset, TEXT("llm_authoring"), Capture, CaptureError))
			{
				CompleteNative(CaptureError, Target);
				OutError.SetFailure(MoveTemp(CaptureError));
				return false;
			}

			Capture.Snapshot.ReleaseNativeValues();
			FUeAgentResourceTracker Resources =
				MakeSemanticWorkspaceResourceTracker();
			const FString TargetObjectPath = ObjectPathFromPackage(Target);
			const FString TargetPackage = PackagePathFromIdentity(Target);
			const FString AssetKind = AssetKindForRootType(RootType);
			return TryCoordinateSemanticWorkspaceImport(
				Document.SemanticWorkspace.ToSharedRef(),
				Capture.Model.ToSharedRef(),
				Capture.CanonicalSnapshot.ToSharedRef(),
				Contract::FUeAgentDescriptorIdentity(
					TEXT("material.semantic_workspace"),
					1,
					Capture.Model->GetSemanticFingerprint()),
				Asset->GetPathName(),
				Resources,
				{
					TEXT("material-domain-provider"),
					Target,
					TargetObjectPath,
					TEXT("material-semantic"),
					TEXT("material_semantic_prepare_failed"),
					TEXT("material_semantic_apply_failed"),
					TEXT("material_domain_json_invalid"),
					TEXT("material_domain_compile_not_settled"),
					[TargetObjectPath](TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error)
					{
						UObject* Native = StaticLoadObject(UObject::StaticClass(), nullptr, *TargetObjectPath);
						if (!Native)
						{
							Error = NativeCondition(TEXT("material_domain_target_unresolved"),
								TEXT("The material target could not be loaded for semantic readback."), TargetObjectPath, TEXT("/target_present"));
							return false;
						}
						return UeAgentMaterialAuthoring::BuildAssetExportSnapshotReadRequests(*Native, Requests, Error);
					}},
				[AssetKind, TargetPackage, &Asset, &Document](
					const FString& RequestId,
					const UE::UAI::Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
					UE::UAI::Composition::HjsonImport::FSemanticWorkspaceChangeProjection&& Projection,
					FRootPropertyApplyOutcome& OutOutcome)
				{
					if (Asset == nullptr || !Prepared.IsValid())
					{
						OutOutcome.Phase = TEXT("preflight");
						OutOutcome.Diagnostic = Asset == nullptr
							? NativeCondition(TEXT("material_semantic_asset_missing"),
								TEXT("The native material target is unavailable before semantic apply."), TargetPackage, TEXT("/target_present"))
							: Prepared.GetValidationDiagnostic();
						CompleteNative(OutOutcome.Diagnostic, TargetPackage);
						return false;
					}
					TSharedPtr<FJsonObject> GraphDocument;
					TArray<TSharedPtr<FJsonValue>> InstanceParameters;
					TSharedPtr<FJsonValue> InstanceLayers;
					UeAgentMaterialAuthoring::FGraphApplyContext GraphContext;
					GraphContext.Policy = UeAgentMaterialAuthoring::EGraphApplyPolicy::PreserveExisting;
					FWorkspaceError GraphError;
					if (AssetKind == TEXT("material_instance"))
					{
						const auto& Units = Prepared.GetWorkspace()->GetModel()->GetUnits();
						TMap<FString, int32> UnitsByTarget;
						for (int32 Index = 0; Index < Units.Num(); ++Index)
						{
							const auto& Unit = Units[Index];
							if ((Unit.GetSemanticTypeRef() == UeAgentMaterialAuthoring::ExportInstanceParameterSemanticId
								|| Unit.GetSemanticTypeRef() == UeAgentMaterialAuthoring::ExportInstanceLayersSemanticId)
								&& !UnitsByTarget.Contains(Unit.GetTargetIdentityKey()))
								UnitsByTarget.Add(Unit.GetTargetIdentityKey(), Index);
						}
						for (const auto& Change : Prepared.GetChangeSetDraft().GetChanges())
						{
							const int32* Canonical = UnitsByTarget.Find(Change.GetIdentity().GetCanonicalKey());
							const int32* Selector = UnitsByTarget.Find(Change.GetIdentity().GetSelector(CanonicalAuthoringTargetIdentitySelector));
							const int32 UnitIndex = Canonical && Selector ? FMath::Min(*Canonical, *Selector)
								: Canonical ? *Canonical : Selector ? *Selector : INDEX_NONE;
							const auto* Unit = UnitIndex == INDEX_NONE ? nullptr : &Units[UnitIndex];
							if (!Unit) continue;
							TSharedPtr<FJsonValue> Value;
							UE::UAI::Contract::FDiagnostic Error;
							if (!Change.GetProposedAfter().IsValid() || !StateSemantic::CanonicalJson::TryToJsonValue(*Change.GetProposedAfter(), Value, Error)
								|| !Value.IsValid() || Value->Type != EJson::Object)
							{
								if (Error.IsEmpty()) Error = NativeCondition(TEXT("material_instance_parameter_projection_failed"),
									!Change.GetProposedAfter().IsValid() ? TEXT("The material instance change has no proposed value.")
										: TEXT("The material instance proposed value did not project to an object."),
									TargetPackage, !Change.GetProposedAfter().IsValid() ? TEXT("/proposed_value_present") : TEXT("/projected_object_present"));
								CompleteNative(Error, TargetPackage);
								if (Error.CandidateName.IsEmpty()) Error.CandidateName = Change.GetIdentity().GetSelector(TEXT("parameter_name"));
								OutOutcome.Diagnostic = MoveTemp(Error);
								return false;
							}
							if (Unit->GetSemanticTypeRef() == UeAgentMaterialAuthoring::ExportInstanceLayersSemanticId)
							{ InstanceLayers = MoveTemp(Value); continue; }
							const auto Parameter = Value->AsObject();
							for (const TCHAR* Field : {TEXT("parameter_name"), TEXT("parameter_kind"), TEXT("association")})
							{
								FString FieldValue;
								if (!Parameter->TryGetStringField(Field, FieldValue) || FieldValue != Change.GetIdentity().GetSelector(Field))
								{
									OutOutcome.Diagnostic = ValueFailure(TEXT("material_instance_parameter_identity_read_only"),
										TEXT("The material instance parameter identity field must match its captured selector."),
										TargetPackage, TEXT("/") + FString(Field), JsonFact(Parameter->TryGetField(Field)),
										MakeShared<FJsonValueString>(Change.GetIdentity().GetSelector(Field)), TEXT("authored_value"));
									OutOutcome.Diagnostic.CandidateName = Change.GetIdentity().GetSelector(TEXT("parameter_name"));
									Prepared.GetWorkspace()->LocateDocumentDiagnostic({Change.GetIdentity().GetCanonicalKey()},
										Projection.DocumentSourceBindings, OutOutcome.Diagnostic);
									return false;
								}
							}
							double Index;
							if (!Parameter->TryGetNumberField(TEXT("parameter_index"), Index)
								|| Index != FCString::Atoi(*Change.GetIdentity().GetSelector(TEXT("parameter_index"))))
							{
								OutOutcome.Diagnostic = ValueFailure(TEXT("material_instance_parameter_identity_read_only"),
									TEXT("The material instance parameter index must match its captured selector."), TargetPackage, TEXT("/parameter_index"),
									JsonFact(Parameter->TryGetField(TEXT("parameter_index"))),
									MakeShared<FJsonValueNumber>(FCString::Atoi(*Change.GetIdentity().GetSelector(TEXT("parameter_index")))), TEXT("authored_value"));
								OutOutcome.Diagnostic.CandidateName = Change.GetIdentity().GetSelector(TEXT("parameter_name"));
								Prepared.GetWorkspace()->LocateDocumentDiagnostic({Change.GetIdentity().GetCanonicalKey()},
									Projection.DocumentSourceBindings, OutOutcome.Diagnostic);
								return false;
							}
							InstanceParameters.Add(MoveTemp(Value));
						}
					}
					else if (!TryBuildMaterialGraphDocument(
							Document.SemanticWorkspace->GetModel(), GraphDocument, GraphContext, GraphError, &Prepared.GetWorkspace().Get(), TargetPackage)
						|| !GraphDocument.IsValid()
						|| !AppendResolvedExpressionProperties(Prepared, Projection, GraphDocument, GraphContext, GraphError, TargetPackage))
					{
						OutOutcome.Phase = TEXT("semantic_projection");
						OutOutcome.Diagnostic = GraphError.HasErrors() ? GraphError.GetDiagnostic() : NativeCondition(TEXT("material_semantic_graph_projection_failed"),
							TEXT("The material semantic projection produced no usable graph document."), TargetPackage, TEXT("/graph_document_present"));
						CompleteNative(OutOutcome.Diagnostic, TargetPackage);
						return false;
					}
					UeAgentMaterialAuthoring::FAssetApplyRequest ApplyRequest;
					ApplyRequest.Operation =
						UeAgentApplyInputContract::EOperation::Update;
					ApplyRequest.AssetPath = TargetPackage;
					ApplyRequest.AssetKind = AssetKind;
					ApplyRequest.Graph = GraphDocument;
					ApplyRequest.InstanceParameters = MoveTemp(InstanceParameters);
					ApplyRequest.InstanceLayers = MoveTemp(InstanceLayers);
					ApplyRequest.Properties = MoveTemp(Projection.RootObjectProperties);
					ApplyRequest.GraphContext = MoveTemp(GraphContext);
					ApplyRequest.bCompile = true;
					ApplyRequest.bSave = true;
					ApplyRequest.FinalReadback = OutOutcome.FinalReadback;
					Asset = nullptr;
					UeAgentMaterialAuthoring::FAssetApplyResult ApplyResult;
					if (!UeAgentMaterialAuthoring::ApplyAsset(
							ApplyRequest, ApplyResult, RequestId)
						|| !ApplyResult.IsApplied())
					{
						OutOutcome.Phase = ApplyResult.Phase.IsEmpty()
							? TEXT("domain_execute") : ApplyResult.Phase;
						OutOutcome.Diagnostic = ApplyResult.Diagnostic.IsValid() ? MoveTemp(ApplyResult.Diagnostic)
							: NativeCondition(TEXT("material_semantic_graph_apply_failed"),
								TEXT("The native material operation did not confirm that its changes were applied."), TargetPackage, TEXT("/applied"));
						CompleteNative(OutOutcome.Diagnostic, TargetPackage);
						return false;
					}
					OutOutcome.bApplied = true;
					OutOutcome.Phase = TEXT("domain_execute");
					ApplyResult.Asset = nullptr;
					Asset = nullptr;
					return true;
				},
				[&Target, &RootType](FString& OutReadbackPayload,
					FString& OutRevision, FWorkspaceError& OutReadError)
				{
					return TryRead(
						Target,
						RootType,
						OutReadbackPayload,
						OutRevision,
						nullptr,
						OutReadError);
				},
				OutPayload,
				OutError);
		}

		bool TryImport(
			const FString& Target,
			const FString& RootType,
			const FString& CanonicalParamsJson,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FResolvedDomainHjsonDocument Document;
			if (!TryDecodeResolvedDocument(
					Target,
					CanonicalParamsJson,
					FDomainHjsonDocumentContract(
						TEXT("material"), SchemaId, RootType),
					Document,
					OutError))
			{
				return false;
			}
			UE::UAI::HjsonWorkspace::DomainAuthoring::FScopedDomainHjsonDiagnosticSource DiagnosticSource(Document, OutError);
			if (Document.bSemanticWorkspaceDirectory)
			{
				return TryImportSemanticWorkspace(
					Target, RootType, Document, OutPayload, OutError);
			}
			const TSharedPtr<FJsonObject>& Root = Document.Root;
			const FString& ExpectedRevision = Document.ExpectedRevision;
			if (RootType != MaterialType && RootType != FunctionType && RootType != InstanceType)
				return Fail(OutError, ValueFailure(TEXT("material_domain_initialization_unsupported"),
					TEXT("This material function type requires an existing native factory asset; export that asset and import its semantic workspace."),
					Target, TEXT("/root/type"), MakeShared<FJsonValueString>(RootType),
					MakeShared<FJsonValueString>(TEXT("Material, MaterialFunction or MaterialInstanceConstant for direct creation")), TEXT("authored_value")));
			if (ExpectedRevision != TEXT("missing"))
			{
				auto Error = NativeCondition(TEXT("material_domain_semantic_workspace_required"),
					TEXT("Updating an existing material target requires its exported semantic workspace."),
					Target, TEXT("/expected_revision_declares_missing"));
				Error.SourceKind = TEXT("command_input");
				Error.Source.JsonPath = TEXT("/params/expected_revision");
				Error.ActualEvidence = MakeShared<FJsonValueString>(ExpectedRevision);
				return Fail(OutError, MoveTemp(Error));
			}

			{
				bool bPresent = false;
				if (!TryInspectObjectTargetPresence(Target, bPresent))
				{
					auto Error = ValueFailure(TEXT("material_domain_target_invalid"),
						TEXT("The material target does not resolve to a valid long package name."), Target, TEXT("/target"),
						MakeShared<FJsonValueString>(Target), MakeShared<FJsonValueString>(TEXT("valid long package or object path")), TEXT("command_input"));
					Error.Source.JsonPath = TEXT("/params/target");
					return Fail(OutError, MoveTemp(Error));
				}
				if (bPresent)
				{
					return Fail(OutError, NativeCondition(TEXT("material_domain_stale_revision"),
						TEXT("The material create target already exists, so the expected missing state is stale."),
						Target, TEXT("/target_present"), true, false));
				}
			}
			FString MutationPayload;
			{
				const TSharedPtr<FJsonObject>* Create = nullptr;
				if (!Root->TryGetObjectField(TEXT("create"), Create)
					|| !Create || !Create->IsValid())
				{
					return Fail(OutError, ValueFailure(TEXT("material_domain_create_contract_required"),
						TEXT("Direct material creation requires a create object."), Target, TEXT("/root/create"),
						JsonFact(Root->TryGetField(TEXT("create"))), MakeShared<FJsonValueString>(TEXT("object")), TEXT("authored_value")));
				}
				FString DeclaredName;
				FString DeclaredPath;
				if (!(*Create)->TryGetStringField(TEXT("asset_name"), DeclaredName))
					return Fail(OutError, ValueFailure(TEXT("material_domain_create_contract_required"),
						TEXT("The create object must contain an asset_name string."), Target, TEXT("/root/create/asset_name"),
						JsonFact((*Create)->TryGetField(TEXT("asset_name"))), MakeShared<FJsonValueString>(TEXT("string")), TEXT("authored_value")));
				if (!(*Create)->TryGetStringField(TEXT("package_path"), DeclaredPath))
					return Fail(OutError, ValueFailure(TEXT("material_domain_create_contract_required"),
						TEXT("The create object must contain a package_path string."), Target, TEXT("/root/create/package_path"),
						JsonFact((*Create)->TryGetField(TEXT("package_path"))), MakeShared<FJsonValueString>(TEXT("string")), TEXT("authored_value")));
				DeclaredName.TrimStartAndEndInline();
				DeclaredPath.TrimStartAndEndInline();
				while (DeclaredPath.EndsWith(TEXT("/")))
				{
					DeclaredPath.LeftChopInline(1);
				}
				if (ObjectPathFromPackageIdentity(DeclaredPath + TEXT("/") + DeclaredName)
					!= ObjectPathFromPackageIdentity(Target))
				{
					return Fail(OutError, ValueFailure(TEXT("material_domain_create_target_mismatch"),
						TEXT("The create object's package_path and asset_name resolve to a different target."), Target, TEXT("/root/create"),
						MakeShared<FJsonValueString>(ObjectPathFromPackageIdentity(DeclaredPath + TEXT("/") + DeclaredName)),
						MakeShared<FJsonValueString>(ObjectPathFromPackageIdentity(Target)), TEXT("related_input")));
				}
				if (RootType == MaterialType || RootType == FunctionType)
				{
					if (Root->HasField(TEXT("edits"))
						|| Root->HasField(TEXT("graph")))
					{
						return Fail(OutError, ValueFailure(TEXT("material_domain_create_then_edit_not_atomic"),
							TEXT("Direct creation cannot atomically apply graph or edit changes; create the asset before importing its semantic workspace."),
							Target, Root->HasField(TEXT("edits")) ? TEXT("/root/edits") : TEXT("/root/graph"),
							MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TEXT("authored_value")));
					}
					const FString ExpectedKind = RootType == FunctionType
						? FString(TEXT("material_function"))
						: FString(TEXT("material"));
					FString DeclaredKind;
					if ((*Create)->TryGetStringField(
							TEXT("material_kind"), DeclaredKind)
						&& DeclaredKind != ExpectedKind)
					{
						return Fail(OutError, UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("material_domain_create_kind_mismatch"), TEXT("/root/create/material_kind"),
							MakeShared<FJsonValueString>(ExpectedKind), MakeShared<FJsonValueString>(DeclaredKind)), Document.SourceUnit, TEXT("/root/create/material_kind"));
					}
					const TSharedPtr<FJsonObject> NativeCreate =
						MakeShared<FJsonObject>(**Create);
					NativeCreate->SetStringField(
						TEXT("material_kind"), ExpectedKind);
					if (!SubmitMaterialCommand(TEXT("material.asset.create"), NativeCreate,
							MutationPayload, OutError, Document, TEXT("/root/create"))) return false;
				}
				else if (RootType == InstanceType)
				{
					if (Root->HasField(TEXT("parameters")))
					{
						return Fail(OutError, ValueFailure(TEXT("material_domain_create_then_edit_not_atomic"),
							TEXT("Direct material instance creation cannot atomically apply parameter changes; use the created asset's semantic workspace."),
							Target, TEXT("/root/parameters"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TEXT("authored_value")));
					}
					if (!SubmitMaterialCommand(TEXT("material_instance.asset.create"), *Create,
							MutationPayload, OutError, Document, TEXT("/root/create"))) return false;
				}
			}
			TSharedPtr<FJsonObject> Mutation;
			if (!TryReadNativeMutationPayload(Target, MutationPayload, Mutation, OutError)) return false;

			// This create owner has already completed native update, durable save and
			// independent readback. Consume its receipt, not current snapshot freshness.
			const TSharedPtr<FJsonObject>* OperationStages = nullptr;
			UeAgentAuthoringWorkspaceTransaction::FFinalReadbackEvidence OperationEvidence;
			if (!Mutation->TryGetObjectField(TEXT("operation_stages"), OperationStages) || !OperationStages || !OperationStages->IsValid())
				return Fail(OutError, ValueFailure(TEXT("material_domain_lifecycle_receipt_invalid"),
					TEXT("The native material create result must contain an operation_stages object."), Target, TEXT("/operation_stages"),
					JsonFact(Mutation->TryGetField(TEXT("operation_stages"))), MakeShared<FJsonValueString>(TEXT("object"))));
			UE::UAI::Contract::FDiagnostic ReceiptError;
			if (!UeAgentAuthoringWorkspaceTransaction::FFinalReadbackEvidence::TryFromJsonObject(**OperationStages, OperationEvidence, ReceiptError))
			{
				CompleteNative(ReceiptError, Target);
				return Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("material_domain_lifecycle_receipt_invalid"), ReceiptError));
			}
			return TryReadbackImportPayload(
				TEXT("material-domain-provider"),
				Target,
				ObjectPathFromPackageIdentity(Target),
				Mutation,
				TEXT("material_domain_readback_payload_invalid"),
				TEXT("material_domain_compile_not_settled"),
				[&Target, &RootType](FString& Payload, FString& Revision, FWorkspaceError& Error)
				{
					return TryRead(Target, RootType, Payload, Revision, nullptr, Error);
				},
				OutPayload,
				OutError,
				&OperationEvidence);
		}

		bool TryExport(
			const FString& Target,
			const FString& RootType,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FString WorkspacePath;
			if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
				|| WorkspacePath.IsEmpty())
			{
				auto Error = ValueFailure(TEXT("material_domain_export_path_required"),
					TEXT("Material export requires a nonempty workspace_path string."), Target, TEXT("/workspace_path"),
					JsonFact(Params->TryGetField(TEXT("workspace_path"))), MakeShared<FJsonValueString>(TEXT("nonempty path string")), TEXT("command_input"));
				Error.Source.JsonPath = TEXT("/params/workspace_path");
				return Fail(OutError, MoveTemp(Error));
			}

			UClass* AssetClass = AssetClassForRootType(RootType);
			UObject* Asset = AssetClass
				? StaticLoadObject(
					AssetClass, nullptr, *ObjectPathFromPackage(Target))
				: nullptr;
			if (!Asset || Asset->GetClass() != AssetClass)
			{
				return Fail(OutError, ValueFailure(TEXT("material_domain_target_unresolved"),
					Asset ? TEXT("The loaded material target has a different native class.") : TEXT("The material target could not be loaded."),
					Target, TEXT("/asset_class"), Asset ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Asset->GetClass()->GetPathName())) : TSharedPtr<FJsonValue>(),
					MakeShared<FJsonValueString>(AssetClass ? AssetClass->GetPathName() : RootType)));
			}
			if (!EnsureAuthoringRegistered(OutError)) return false;

			UeAgentExportWorkspaceWriter::FWorkspaceResult ExportResult;
						if (!UeAgentMaterialAuthoring::ExportAssetWorkspace(*Asset, WorkspacePath, TEXT("llm_authoring"), ExportResult))
			{
				OutError.SetFailure(ExportResult.Diagnostic, Target);
				return false;
			}
			const TSharedPtr<FJsonObject> Result = ExportResult.ToPublicJsonObject();
			return CanonicalObject(Result, OutPayload, OutError);
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
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			if (JsonError.IsEmpty()) JsonError = ValueFailure(TEXT("material_domain_params_invalid"),
				TEXT("The material command parameters must decode to a JSON object."), TargetIdentity, TEXT("/params"),
				nullptr, MakeShared<FJsonValueString>(TEXT("object")), TEXT("command_input"));
			if (JsonError.SourceKind.IsEmpty()) JsonError.SourceKind = TEXT("command_input");
			if (JsonError.TargetPath.IsEmpty()) JsonError.TargetPath = TargetIdentity;
			return Fail(OutError, MoveTemp(JsonError));
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| Target != TargetIdentity)
		{
			auto Error = ValueFailure(TEXT("material_domain_target_identity_mismatch"),
				TEXT("The target parameter must match the resolved material target."), TargetIdentity, TEXT("/target"),
				JsonFact(Params->TryGetField(TEXT("target"))), MakeShared<FJsonValueString>(TargetIdentity), TEXT("command_input"));
			Error.Source.JsonPath = TEXT("/params/target");
			return Fail(OutError, MoveTemp(Error));
		}
		if (!IsSupportedRootType(RootTypeIdentity))
		{
			return Fail(OutError, ValueFailure(TEXT("material_domain_root_type_invalid"),
				TEXT("The material provider does not support the resolved root type."), TargetIdentity, TEXT("/root_type"),
				MakeShared<FJsonValueString>(RootTypeIdentity),
				MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
					MakeShared<FJsonValueString>(MaterialType), MakeShared<FJsonValueString>(FunctionType),
					MakeShared<FJsonValueString>(LayerType), MakeShared<FJsonValueString>(BlendType), MakeShared<FJsonValueString>(InstanceType)})));
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(
				Target,
				RootTypeIdentity,
				CanonicalParamsJson,
				OutPayloadJson,
				OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			bOutReadOnly = true;
			return TryExport(
				Target, RootTypeIdentity, Params, OutPayloadJson, OutError);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString Revision;
			bOutReadOnly = true;
			return TryRead(
				Target, RootTypeIdentity, OutPayloadJson, Revision,
				nullptr, OutError);
		}
		return Fail(OutError, ValueFailure(TEXT("material_domain_command_not_supported"),
			TEXT("The material domain does not support this command."), Target, TEXT("/command_id"),
			MakeShared<FJsonValueString>(CommandId), MakeShared<FJsonValueString>(TEXT("content_graph.*")), TEXT("command_input")));
	}
}
