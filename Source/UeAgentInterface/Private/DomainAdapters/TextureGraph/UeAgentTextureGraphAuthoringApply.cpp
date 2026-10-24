#include "DomainAdapters/TextureGraph/UeAgentTextureGraphAuthoringApply.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/TextureGraph/UeAgentTextureGraphDomainMechanics.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonImportInitialization.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "ObjectTools.h"
#include "Factories/Factory.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntimeResolvers.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceApply.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "TextureGraph.h"
#include "TG_Graph.h"
#include "TG_Node.h"
#include "Expressions/TG_Expression.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::UAI::DomainAdapters::TextureGraph
{
	namespace
	{
		using namespace UeAgentAuthoringOperationApply;
		using namespace Composition::GraphCompound;
		const FString ProviderId = TEXT("texture_graph.semantic.apply");
		const FString InputId = TEXT("uai.apply_input.texture_graph.semantic");
		const FString WorkspaceId = TEXT("uai.apply_workspace.texture_graph.semantic");
		const FString SemanticId = TEXT("texture_graph.semantic.native_graph");
		const FString IdentityModel = TEXT("texture_graph.semantic.asset_identity");
		const FString AccessorId = TEXT("texture_graph.semantic.native_capture");
		const FString TargetKind = TEXT("texture_graph");

		UE::UAI::Contract::FDiagnostic TextureApplyFact(const FString& Target, const FString& Code,
			const FString& Reason, const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& SourceKind = TEXT("native_result"))
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("texture_graph_apply"));
			Error.TargetPath = Target;
			Error.SourceKind = SourceKind;
			if (SourceKind == TEXT("command_input")) Error.Source.JsonPath = Path;
			else if (SourceKind == TEXT("native_result")) Error.PropertyPath = Path;
			else Error.SemanticPointer = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		TSharedPtr<FJsonValue> TextureApplyShape(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value) return MakeShared<FJsonValueString>(TEXT("<missing>"));
			if (Value->Type != EJson::Object && Value->Type != EJson::Array) return Value;
			return MakeShared<FJsonValueString>(Value->Type == EJson::Object ? TEXT("object") : TEXT("array"));
		}

		TSharedPtr<FJsonValue> TextureApplyFlags(const TCHAR* First, bool bFirst, const TCHAR* Second, bool bSecond)
		{
			auto Facts = MakeShared<FJsonObject>();
			Facts->SetBoolField(First, bFirst);
			Facts->SetBoolField(Second, bSecond);
			return MakeShared<FJsonValueObject>(Facts);
		}

		void SupplementTextureApplyNativeContext(UE::UAI::Contract::FDiagnostic& Error, const FString& Target)
		{
			if (Error.IsEmpty()) return;
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target;
			if (Error.SourceKind.IsEmpty() && Error.Source.SourceFile.IsEmpty()) Error.SourceKind = TEXT("native_result");
			for (auto& Child : Error.Diagnostics) SupplementTextureApplyNativeContext(Child, Target);
			if (Error.PublicCause)
			{
				auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Error.PublicCause);
				SupplementTextureApplyNativeContext(*Cause, Target);
				Error.PublicCause = MoveTemp(Cause);
			}
		}

		bool Missing(const FString& Target, UE::UAI::Contract::FDiagnostic& Error)
		{
			bool Present = false;
			const bool bInspected = HjsonWorkspace::DomainAuthoring::TryInspectObjectTargetPresence(Target, Present);
			if (!bInspected || Present)
			{
				Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("texture_graph_expected_missing_target_present"),
					bInspected ? TEXT("TextureGraph creation requires a missing target, but this target already exists.")
						: TEXT("The TextureGraph target presence could not be inspected."), TEXT("preflight"));
				Error.TargetPath = Target;
				Error.SourceKind = TEXT("native_result");
				if (bInspected)
				{
					Error.ActualValue = MakeShared<FJsonValueBoolean>(Present);
					Error.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
				}
				return false;
			}
			return true;
		}

		struct FInitialization
		{
			AssetDataSimpleEffect::FUeAgentAssetDataSimpleEffectFactoryBinding Factory;
			HjsonWorkspace::FImportInitializationPlanPtr Plan;
		};

		bool Fail(FOperationFailure& Failure, UE::UAI::Contract::FDiagnostic Diagnostic) { Failure.Diagnostic = MoveTemp(Diagnostic); return false; }

		using FSettingPlans = TMap<FString, TUniquePtr<UeAgentAuthoringObjectApply::FPreparedApply>>;
		using FSettingIntents = TMap<FString, UeAgentAuthoringObjectApply::FPropertyIntent>;
		void InitializeFactoryContext(UTextureGraph& Asset, const FString& Target)
		{
			if (Asset.Graph()) Asset.Graph()->ForEachNodes([&](const UTG_Node* Node, uint32) {
				if (Node) DomainAdapters::AudioMediaGraph::FAudioMediaGraphTextureGraphNativeHook::InitializeCreatedNode(
					*Node, Target);
			});
		}
		UTG_Expression* FindExpression(UTextureGraph& Asset, int32 Index)
		{
			UTG_Expression* Result = nullptr;
			if (Asset.Graph()) Asset.Graph()->ForEachNodes([&](const UTG_Node* Node, uint32) {
				if (Node && Node->GetId().NodeIdx() == Index) Result = Node->GetExpression(); });
			return Result;
		}
		bool PrepareSettings(UTextureGraph& Asset, const FTextureGraphSemanticCompositeRequest& Request,
			const DomainAdapters::AudioMediaGraph::FAudioMediaGraphTextureGraphNodeBindings& Bindings,
			FSettingPlans& Plans, FOperationFailure& Failure)
		{
			Plans.Reset();
			auto SettingsByElement = Request.ElementSettings;
			for (const auto& Pair : Request.Settings)
			{
				// Settings retain the captured identity. Structural replacement can assign
				// a different native index; follow the same binding as nodes and links.
				const FString* Element = Request.NodeBindings.NodeIndices.FindKey(Pair.Key);
				if (!Element)
				{
					auto Diagnostic = TextureApplyFact(Request.TargetRef, TEXT("texture_graph_setting_identity_unresolved"),
						TEXT("The captured setting node index has no element binding."), TEXT("/settings/owner"), MakeShared<FJsonValueNumber>(Pair.Key), MakeShared<FJsonValueString>(TEXT("a bound node index")));
					return Fail(Failure, MoveTemp(Diagnostic));
				}
				SettingsByElement.FindOrAdd(*Element).Append(Pair.Value);
			}
			for (const auto& Pair : SettingsByElement)
			{
				const int32* Index = Bindings.NodeIndices.Find(Pair.Key);
				if (!Index)
				{
					auto Diagnostic = TextureApplyFact(Request.TargetRef, TEXT("texture_graph_setting_identity_unresolved"),
						TEXT("The setting owner has no binding in the resulting graph."), TEXT("/settings/owner"), MakeShared<FJsonValueString>(Pair.Key), MakeShared<FJsonValueString>(TEXT("an existing element binding")));
					return Fail(Failure, MoveTemp(Diagnostic));
				}
				UTG_Expression* Expression = FindExpression(Asset, *Index);
				if (!Expression)
				{
					auto Diagnostic = TextureApplyFact(Request.TargetRef, TEXT("texture_graph_setting_node_missing"),
						TEXT("The setting owner has no native expression in the resulting graph."), TEXT("/settings/owner"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					Diagnostic.CandidateName = Pair.Key;
					return Fail(Failure, MoveTemp(Diagnostic));
				}
				for (const auto& Descriptor : Pair.Value)
				{
					FString Path;
					if (!Descriptor.IsValid() || Descriptor->Type != EJson::Object
						|| !Descriptor->AsObject()->TryGetStringField(TEXT("property_path"), Path))
						{
							auto Diagnostic = TextureApplyFact(Request.TargetRef, TEXT("texture_graph_setting_descriptor_invalid"),
								TEXT("Each setting descriptor requires a string property_path."), TEXT("/settings/property_path"), TextureApplyShape(Descriptor && Descriptor->Type == EJson::Object ? Descriptor->AsObject()->TryGetField(TEXT("property_path")) : Descriptor), MakeShared<FJsonValueString>(TEXT("string")));
							return Fail(Failure, MoveTemp(Diagnostic));
						}
					FProperty* Property = FindFProperty<FProperty>(Expression->GetClass(), *Path);
					if (!Property || !DomainAdapters::AudioMediaGraph::FAudioMediaGraphTextureGraphNativeHook::UsesReflectedExpressionProperty(*Expression, *Property))
						{
							auto Diagnostic = TextureApplyFact(Request.TargetRef, TEXT("texture_graph_setting_property_invalid"),
								TEXT("The selected property is missing or is not an authorable reflected TextureGraph expression property."), Path, TextureApplyFlags(TEXT("property_present"), Property != nullptr, TEXT("reflected_expression_property"), Property != nullptr && DomainAdapters::AudioMediaGraph::FAudioMediaGraphTextureGraphNativeHook::UsesReflectedExpressionProperty(*Expression, *Property)), TextureApplyFlags(TEXT("property_present"), true, TEXT("reflected_expression_property"), true));
							Diagnostic.CandidateName = Pair.Key;
							return Fail(Failure, MoveTemp(Diagnostic));
						}
				}
				UeAgentAuthoringObjectApply::FOptions Options;
				Options.bCompactValues = true; Options.bAllowTemplateOwners = true;
				Options.bAllowDomainAuthorizedOwners = true;
				UeAgentAuthoringObjectApply::FFailure Error;
				auto Prepared = MakeUnique<UeAgentAuthoringObjectApply::FPreparedApply>();
				if (!UeAgentAuthoringObjectApply::PrepareObject(Expression, Pair.Value, Options, *Prepared, Error))
					return Fail(Failure, Error.Diagnostic);
				Plans.Add(Pair.Key, MoveTemp(Prepared));
			}
			return true;
		}

		bool VerifySettingIntents(UTextureGraph& Asset, const FString& Target,
			const DomainAdapters::AudioMediaGraph::FAudioMediaGraphTextureGraphNodeBindings& Bindings,
			const FSettingIntents& Intents, FOperationFailure& Failure)
		{
			for (const auto& Pair : Intents)
			{
				const int32* Index = Bindings.NodeIndices.Find(Pair.Key);
				UTG_Expression* Expression = Index ? FindExpression(Asset, *Index) : nullptr;
				if (!Expression)
				{
					auto Diagnostic = TextureApplyFact(Target, TEXT("texture_graph_setting_identity_unresolved"),
						TEXT("The setting intent owner could not be found in the resulting graph."), TEXT("/settings/owner"), MakeShared<FJsonValueString>(Pair.Key), MakeShared<FJsonValueString>(TEXT("an existing node expression")));
					return Fail(Failure, MoveTemp(Diagnostic));
				}
				UeAgentAuthoringObjectApply::FApplyResult Result;
				if (!Pair.Value.VerifyObject(Expression, Result))
					return Fail(Failure, Result.Diagnostic);
			}
			return true;
		}
		bool VerifySettings(FSettingPlans& Plans, FOperationFailure& Failure)
		{
			for (auto& Plan : Plans)
			{
				UeAgentAuthoringObjectApply::FApplyResult Result;
				if (!UeAgentAuthoringObjectApply::VerifyPrepared(*Plan.Value, Result))
					UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Result.Diagnostic.Code, Result.Diagnostic, TEXT("texture_graph_settings_observation"));
			}
			return true;
		}
		bool ApplySettings(FSettingPlans& Plans, FSettingIntents& Intents, FOperationFailure& Failure)
		{
			Intents.Reset();
			for (auto& Plan : Plans)
			{
				UeAgentAuthoringObjectApply::FApplyResult Result;
				if (!UeAgentAuthoringObjectApply::ApplyPrepared(*Plan.Value, Result))
					return Fail(Failure, Result.Diagnostic);
				UeAgentAuthoringObjectApply::FPropertyIntent Intent;
				UeAgentAuthoringObjectApply::FFailure Error;
				if (!Plan.Value->CaptureIntent(Intent, Error))
				{
					UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Error.Diagnostic.Code, Error.Diagnostic, TEXT("texture_graph_intent_capture"));
					continue;
				}
				Intents.Add(Plan.Key, MoveTemp(Intent));
			}
			return VerifySettings(Plans, Failure);
		}

		// The original capture owns node/pin value encoding. Graph node and edge states use
		// native graph IDs, independent of the scratch asset's package/object identity.
		UeAgentAuthoringState::EReadOutcome ReadState(
			const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& OutValue, UE::UAI::Contract::FDiagnostic& Error)
		{
			using namespace UeAgentAuthoringState;
			using namespace ContentGraphRead;
			if (Request.Context.FindRef(TEXT("observe_absence")) == TEXT("true"))
			{
				if (!Missing(Request.RootOwnerPath, Error)) return EReadOutcome::Failed;
				OutValue = FStateValue(); OutValue.TargetIdentity = Request.TargetIdentity;
				OutValue.Type.Kind = TEXT("object"); OutValue.Provenance = AccessorId;
				OutValue.bAvailable = false; OutValue.AvailabilityReason = TEXT("target_not_found");
				return EReadOutcome::Unavailable;
			}
			ON_SCOPE_EXIT { SupplementTextureApplyNativeContext(Error, Request.RootOwnerPath); };
			UTextureGraph* Asset = Cast<UTextureGraph>(const_cast<UObject*>(Request.RootObject));
			FUeAgentContentGraphCapture Capture;
			FUeAgentContentGraphReadError CaptureError;
			if (!Asset)
			{
				Error = TextureApplyFact(Request.RootOwnerPath, TEXT("texture_graph_capture_target_invalid"),
					TEXT("The capture target is missing or is not a TextureGraph asset."), TEXT("/class"),
					MakeShared<FJsonValueString>(Request.RootObject ? Request.RootObject->GetClass()->GetPathName() : TEXT("<missing>")),
					MakeShared<FJsonValueString>(UTextureGraph::StaticClass()->GetPathName()));
				return EReadOutcome::Failed;
			}
			if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(*Asset, Capture, CaptureError))
			{
				Error = MoveTemp(CaptureError);
				if (Error.IsEmpty()) Error = TextureApplyFact(Request.RootOwnerPath, TEXT("texture_graph_capture_target_invalid"),
					TEXT("Native TextureGraph capture failed without returning a diagnostic."), TEXT("/capture"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return EReadOutcome::Failed;
			}
			const auto Graph = MakeShared<FJsonObject>();
			const auto Nodes = MakeShared<FJsonObject>();
			const auto Edges = MakeShared<FJsonObject>();
			for (const auto& Item : Capture.Items)
			{
				if (Item.ItemKind != TEXT("texture_graph_node")) continue;
				TSharedPtr<FJsonValue> Value;
				if (!Item.State.IsValid())
				{
					Error = TextureApplyFact(Request.RootOwnerPath, TEXT("texture_graph_capture_state_missing"),
						TEXT("The captured graph entry has no canonical state value."), TEXT("/nodes"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					Error.CandidateName = Item.ItemId;
					return EReadOutcome::Failed;
				}
				if (!StateSemantic::CanonicalJson::TryToJsonValue(*Item.State, Value, Error)) return EReadOutcome::Failed;
				Nodes->SetField(Item.ItemId, Value);
			}
			for (const auto& Edge : Capture.Relations)
			{
				if (Edge.RelationKind != TEXT("texture_graph_edge")) continue;
				TSharedPtr<FJsonValue> Value;
				if (!Edge.State.IsValid())
				{
					Error = TextureApplyFact(Request.RootOwnerPath, TEXT("texture_graph_capture_state_missing"),
						TEXT("The captured graph entry has no canonical state value."), TEXT("/edges"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					Error.CandidateName = Edge.RelationId;
					return EReadOutcome::Failed;
				}
				if (!StateSemantic::CanonicalJson::TryToJsonValue(*Edge.State, Value, Error)) return EReadOutcome::Failed;
				Edges->SetField(Edge.RelationId, Value);
			}
			const auto Settings = MakeShared<FJsonObject>();
			const auto SettingTypes = MakeShared<FJsonObject>();
			for (const auto& Property : Capture.Properties)
			{
				if (Property.SemanticIdentity.SemanticId != TEXT("uai.semantic.texture_graph.setting")) continue;
				TSharedPtr<FJsonValue> Value;
				if (!Property.State.Value.IsValid())
				{
					Error = TextureApplyFact(Request.RootOwnerPath, TEXT("texture_graph_capture_state_missing"),
						TEXT("The captured graph entry has no canonical state value."), TEXT("/settings"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					Error.CandidateName = Property.FieldId;
					return EReadOutcome::Failed;
				}
				if (!StateSemantic::CanonicalJson::TryToJsonValue(*Property.State.Value, Value, Error)) return EReadOutcome::Failed;
				const FString Key = Property.SemanticIdentity.Selectors.FindRef(TEXT("node_index")) + TEXT(".") + Property.FieldId;
				Settings->SetField(Key, Value);
				if (Property.State.AuthoringDescriptor.IsValid())
				{
					TSharedPtr<FJsonValue> Descriptor;
					if (!StateSemantic::CanonicalJson::TryToJsonValue(*Property.State.AuthoringDescriptor, Descriptor, Error)) return EReadOutcome::Failed;
					if (const auto Comparison = Descriptor->AsObject()->TryGetField(TEXT("comparison_value_definition")))
						Descriptor->AsObject()->SetField(TEXT("value_definition"), Comparison);
					SettingTypes->SetField(Key, Descriptor);
				}
			}
			Graph->SetObjectField(TEXT("settings"), Settings);
			Graph->SetObjectField(TEXT("nodes"), Nodes);
			Graph->SetObjectField(TEXT("edges"), Edges);
			OutValue = FStateValue();
			OutValue.TargetIdentity = Request.TargetIdentity;
			OutValue.Type.Kind = TEXT("object");
			OutValue.Value = MakeShared<FJsonValueObject>(Graph);
			const auto SettingsDefinition = MakeShared<FJsonObject>(); SettingsDefinition->SetObjectField(TEXT("fields"), SettingTypes);
			const auto SettingsDescriptor = MakeShared<FJsonObject>(); SettingsDescriptor->SetObjectField(TEXT("value_definition"), SettingsDefinition);
			SettingsDescriptor->SetObjectField(TEXT("type"), OutValue.Type.ToJsonObject());
			const auto Fields = MakeShared<FJsonObject>(); Fields->SetObjectField(TEXT("settings"), SettingsDescriptor);
			const auto Definition = MakeShared<FJsonObject>(); Definition->SetObjectField(TEXT("fields"), Fields);
			OutValue.ValueDefinition = MakeShared<FJsonValueObject>(Definition);
			OutValue.Provenance = AccessorId;
			OutValue.bEditable = true;
			return EReadOutcome::Resolved;
		}

		bool RegisterState(UE::UAI::Contract::FDiagnostic& Error)
		{
			UeAgentAuthoringIdentity::FModelDescriptor Identity;
			Identity.ModelId = IdentityModel;
			Identity.TargetKind = TargetKind;
			Identity.Selectors = {{TEXT("asset_path"), 100, false}};
			Identity.KeyPatterns = {{TEXT("asset"), 0, {TEXT("asset_path")}}};
			UeAgentAuthoringIdentity::ERegistrationResult IdentityResult;
			if (!UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(Identity, IdentityResult, Error)) return false;
			using namespace UeAgentAuthoringSemanticRegistry;
			FDomainDescriptor Domain;
			Domain.DomainId = TEXT("texture_graph.semantic.authoring");
			Domain.TargetKinds = {TargetKind};
			Domain.MutationProviderId = ProviderId;
			Domain.CapabilityTags = {TEXT("graph"), TEXT("editor_authoring")};
			UeAgentAuthoringSemanticRegistry::ERegistrationResult Registered;
			if (!FRegistry::Get().RegisterDomain(Domain, Registered, Error)) return false;
			FSemanticDescriptor Descriptor;
			Descriptor.SemanticId = SemanticId;
			Descriptor.DomainId = Domain.DomainId;
			Descriptor.TargetKind = TargetKind;
			Descriptor.ValueType = TEXT("object");
			Descriptor.OwnerModel = TEXT("texture_graph_asset_owner");
			Descriptor.IdentityModel = IdentityModel;
			Descriptor.StateAccessor = AccessorId;
			Descriptor.WriteCapability = EWriteCapability::DomainMutation;
			Descriptor.ConstraintSource = TEXT("texture_graph.native_graph");
			Descriptor.AvailabilitySource = AccessorId;
			Descriptor.Comparator = TEXT("uai.compare.canonical_value");
			Descriptor.CapabilityTags = Domain.CapabilityTags;
			if (!FRegistry::Get().RegisterSemantic(Descriptor, Registered, Error)) return false;
			UeAgentAuthoringState::FStateAccessorDescriptor Accessor;
			Accessor.AccessorId = AccessorId;
			Accessor.TargetKinds = {TargetKind};
			Accessor.CapabilityTags = Domain.CapabilityTags;
			UeAgentAuthoringState::ERegistrationResult StateResult;
			return UeAgentAuthoringState::RegisterBuiltInStateAccessors(
				UeAgentAuthoringState::FStateAccessorRegistry::Get(), Error)
				&& UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(
					Accessor, &ReadState, StateResult, Error);
		}

		void Release(TStrongObjectPtr<UTextureGraph>& Asset)
		{
			if (Asset.IsValid())
			{ Asset->ClearFlags(RF_Public | RF_Standalone); Asset->MarkAsGarbage(); Asset.Reset(); }
		}

		class FGraphState final : public IPreparedOperationState
		{
		public:
			FGraphState(FTextureGraphSemanticCompositeRequest InRequest,
				PlanningPreflight::FPlanningPreflightPlanPtr InPlan,
				TSharedRef<Contract::IUeAgentCancellationSignal> InCancellation,
				UTextureGraph* InActual, UTextureGraph* InProposed,
				DomainAdapters::AudioMediaGraph::FAudioMediaGraphTextureGraphNodeBindings InProposedBindings,
				FSettingIntents InProposedIntents, TSharedPtr<FInitialization> InInitialization = nullptr)
				: Request(MoveTemp(InRequest)), Plan(MoveTemp(InPlan)), Cancellation(MoveTemp(InCancellation)),
				  Actual(InActual), Proposed(InProposed), ActualBindings(Request.NodeBindings),
				  ProposedBindings(MoveTemp(InProposedBindings)), Initialization(MoveTemp(InInitialization)),
				  ProposedIntents(MoveTemp(InProposedIntents)) {}
			~FGraphState() override { Release(Proposed); Release(Before); }
			FString GetTargetKey() const override { return Request.TargetRef; }
			UeAgentApplyInputContract::EOperation GetResolvedOperation() const override
			{ return Initialization ? UeAgentApplyInputContract::EOperation::Create : UeAgentApplyInputContract::EOperation::Update; }
			void CollectRollbackObjects(TArray<UObject*>& Objects) const override
			{ if (Actual) Objects.Add(Actual); }
			bool CaptureRollback(FOperationFailure& Failure) override
			{
				if (Initialization) return true;
				Before.Reset(DuplicateObject<UTextureGraph>(Actual, GetTransientPackage()));
				return Before.IsValid() || Fail(Failure, TextureApplyFact(Request.TargetRef, TEXT("texture_graph_before_capture_failed"),
					TEXT("Unreal did not duplicate the TextureGraph asset for rollback."), TEXT("/rollback_snapshot"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			}
			bool ApplyProposed(FOperationFailure&) override { return true; }
			bool BuildExpectedReadRequests(TArray<UeAgentAuthoringState::FReadRequest>& R, FOperationFailure& E) const override
			{ return Read(Proposed.Get(), ProposedBindings, ProposedIntents, R, E); }
			bool BuildActualReadRequests(TArray<UeAgentAuthoringState::FReadRequest>& R, FOperationFailure& E) const override
			{ return Read(Actual, ActualBindings, ActualIntents, R, E); }
			bool ApplyLifecycle(FOperationFailure& Failure) override
			{
				FGraphCompoundRuntimeError Error;
				if (Initialization)
				{
					UE::UAI::Contract::FDiagnostic PresenceError;
					if (!Missing(Request.TargetRef, PresenceError)) return Fail(Failure, PresenceError);
					ExecutionMutation::FExecutionMutationError CreateError;
					if (!Composition::SimpleAuthoring::TryValidateGenericAssetFactoryBinding(Initialization->Factory, CreateError))
						return Fail(Failure, CreateError);
					const FString PackageName = FPackageName::ObjectPathToPackageName(Request.TargetRef);
					UObject* Created = AssetDataSimpleEffect::CreateNativeFactoryObject(CreatePackage(*PackageName),
						FName(*FPackageName::GetLongPackageAssetName(PackageName)), Initialization->Factory.GetConcreteClass(), Initialization->Factory.GetFactory(), false, CreateError);
					Actual = Cast<UTextureGraph>(Created);
					if (!Actual)
					{
						if (Created) CreateError = TextureApplyFact(Request.TargetRef, TEXT("texture_graph_initial_factory_type_invalid"),
							TEXT("The native factory returned an object of a different class."), TEXT("/created_class"),
							MakeShared<FJsonValueString>(Created->GetClass()->GetPathName()), MakeShared<FJsonValueString>(UTextureGraph::StaticClass()->GetPathName()));
						return Fail(Failure, MoveTemp(CreateError));
					}
				}
				else if (!TryValidateTextureGraphSemanticComposite(Request, Plan.ToSharedRef(), Cancellation, Error))
					return Fail(Failure, Error);
				if (!Actual)
				{
					auto Diagnostic = TextureApplyFact(Request.TargetRef, TEXT("texture_graph_actual_missing"),
						TEXT("The TextureGraph target is unavailable before native graph mutation."), TEXT("/target"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return Fail(Failure, MoveTemp(Diagnostic));
				}
				Actual->Modify();
				if (!TryApplyTextureGraphSemanticCompositeNative(*Actual, Request, Error, ActualBindings))
					return Fail(Failure, Error);
				if (!PrepareSettings(*Actual, Request, ActualBindings, SettingPlans, Failure)
					|| !ApplySettings(SettingPlans, ActualIntents, Failure)) return false;
				Actual->MarkPackageDirty();
				return true;
			}
			bool Compile(FOperationFailure& Failure) override
			{
				if (!VerifySettings(SettingPlans, Failure)) {
					UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Failure.Diagnostic);
					Failure = FOperationFailure();
				}
				UE::UAI::Contract::FDiagnostic Error;
				if (!Actual)
					Error = TextureApplyFact(Request.TargetRef, TEXT("texture_graph_actual_missing"),
						TEXT("The TextureGraph target is unavailable during native readback."), TEXT("/target"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				if (!Actual || !TryVerifyTextureGraphSemanticCompositeNative(*Actual, Request, Error, ActualBindings))
					UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("texture_graph_native_readback_failed"), Error, TEXT("texture_graph_observation"));
				return true;
			}
			bool Save(FOperationFailure&) override { return true; }
			bool BuildDurablePackagePlans(TArray<FPackageDurablePlan>& Plans, FOperationFailure& Failure) const override
			{
				return BuildPackageDurablePlan(Request.TargetRef,
					FPackageName::ObjectPathToPackageName(Request.TargetRef), Actual,
					EPackageDurableIntent::Save, Plans, Failure, true,
					Initialization ? EPackageBeforePresence::Absent : EPackageBeforePresence::Present);
			}
			bool Rollback(FOperationFailure& Failure) override
			{
				if (!Actual || !Before.IsValid() || !Before->Graph())
					{
						auto Diagnostic = TextureApplyFact(Request.TargetRef, TEXT("texture_graph_rollback_state_missing"),
							TEXT("TextureGraph rollback requires the actual target, a before snapshot and its native graph."), TEXT("/rollback"), TextureApplyFlags(TEXT("actual_present"), Actual != nullptr, TEXT("before_graph_present"), Before.IsValid() && Before->Graph() != nullptr), TextureApplyFlags(TEXT("actual_present"), true, TEXT("before_graph_present"), true));
						return Fail(Failure, MoveTemp(Diagnostic));
					}
				UTG_Graph* Restored = DuplicateObject<UTG_Graph>(Before->Graph(), Actual);
				if (!Restored)
				{
					auto Diagnostic = TextureApplyFact(Request.TargetRef, TEXT("texture_graph_rollback_graph_failed"),
						TEXT("Unreal did not duplicate the captured native graph during TextureGraph rollback."), TEXT("/rollback/graph"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return Fail(Failure, MoveTemp(Diagnostic));
				}
				Actual->TextureGraph = Restored;
				return true;
			}
			bool RequiresCheckpointRollback() const override { return Initialization.IsValid(); }
			bool SupportsPersistentReadback() const override { return true; }
			bool ReleaseForPersistentReadback(FOperationFailure&) override
			{ SettingPlans.Reset(); Actual = nullptr; Release(Proposed); Release(Before); return true; }
			bool RebindAfterPersistentReadback(FOperationFailure& Failure) override
			{
				Actual = LoadObject<UTextureGraph>(nullptr, *Request.TargetRef);
				return Actual || Fail(Failure, TextureApplyFact(Request.TargetRef, TEXT("texture_graph_reload_failed"),
					TEXT("The TextureGraph target could not be loaded after persistent readback."), TEXT("/target"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			}
			bool PrepareCheckpointRestore(FOperationFailure& Failure) override
			{
				if (!Initialization) return ReleaseForPersistentReadback(Failure);
				UTextureGraph* Created = Actual;
				ReleaseForPersistentReadback(Failure);
				if (Created)
				{
					const int32 DeletedCount = ObjectTools::DeleteObjects({Created}, false);
					if (DeletedCount != 1) return Fail(Failure, TextureApplyFact(Request.TargetRef, TEXT("texture_graph_created_restore_delete_failed"),
						TEXT("Checkpoint restore did not delete exactly one created TextureGraph asset."), TEXT("/deleted_count"),
						MakeShared<FJsonValueNumber>(DeletedCount), MakeShared<FJsonValueNumber>(1)));
				}
				return true;
			}
			bool RebindAfterCheckpointRestore(FOperationFailure& Failure) override
			{
				if (!Initialization) return RebindAfterPersistentReadback(Failure);
				UE::UAI::Contract::FDiagnostic Error; return Missing(Request.TargetRef, Error) || Fail(Failure, Error);
			}
		private:
			bool Read(UTextureGraph* Asset,
				const DomainAdapters::AudioMediaGraph::FAudioMediaGraphTextureGraphNodeBindings& Bindings,
				const FSettingIntents& Intents, TArray<UeAgentAuthoringState::FReadRequest>& Requests, FOperationFailure& Failure) const
			{
				if (!Asset && !Initialization)
				{
					auto Diagnostic = TextureApplyFact(Request.TargetRef, TEXT("texture_graph_capture_asset_missing"),
						TEXT("The TextureGraph target is missing while building a state read request."), TEXT("/target"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return Fail(Failure, MoveTemp(Diagnostic));
				}
				if (Asset && !VerifySettingIntents(*Asset, Request.TargetRef, Bindings, Intents, Failure)) return false;
				UeAgentAuthoringState::FReadRequest ReadRequest;
				ReadRequest.SemanticId = SemanticId;
				ReadRequest.RootObject = Asset;
				ReadRequest.RootOwnerPath = Request.TargetRef;
				if (!Asset && Initialization) ReadRequest.Context.Add(TEXT("observe_absence"), TEXT("true"));
				UE::UAI::Contract::FDiagnostic Error;
				if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(IdentityModel, TargetKind,
					{{TEXT("asset_path"), Request.TargetRef}}, ReadRequest.TargetIdentity, Error))
					return Fail(Failure, MoveTemp(Error));
				ReadRequest.TargetQuery.ModelId = ReadRequest.TargetIdentity.ModelId;
				ReadRequest.TargetQuery.TargetKind = ReadRequest.TargetIdentity.TargetKind;
				ReadRequest.TargetQuery.CanonicalKey = ReadRequest.TargetIdentity.CanonicalKey;
				Requests.Add(MoveTemp(ReadRequest));
				return true;
			}
			FTextureGraphSemanticCompositeRequest Request;
			PlanningPreflight::FPlanningPreflightPlanPtr Plan;
			TSharedRef<Contract::IUeAgentCancellationSignal> Cancellation;
			UTextureGraph* Actual;
			TStrongObjectPtr<UTextureGraph> Proposed, Before;
			DomainAdapters::AudioMediaGraph::FAudioMediaGraphTextureGraphNodeBindings ActualBindings, ProposedBindings;
			TSharedPtr<FInitialization> Initialization;
			FSettingPlans SettingPlans;
			FSettingIntents ProposedIntents, ActualIntents;
		};

		class FGraphProvider final : public IApplyProvider
		{
		public:
			FGraphProvider(FTextureGraphSemanticCompositeRequest InRequest,
				PlanningPreflight::FPlanningPreflightPlanPtr InPlan,
				TSharedRef<Contract::IUeAgentCancellationSignal> InCancellation, TSharedPtr<FInitialization> InInitialization = nullptr)
				: Request(MoveTemp(InRequest)), Plan(MoveTemp(InPlan)), Cancellation(MoveTemp(InCancellation)), Initialization(MoveTemp(InInitialization)) {}
			FString GetProviderId() const override { return ProviderId; }
			bool PrepareOperation(const FPrepareRequest& Input, TUniquePtr<IPreparedOperationState>& OutState,
				FOperationFailure& Failure) const override
			{
				ON_SCOPE_EXIT { SupplementTextureApplyNativeContext(Failure.Diagnostic, Request.TargetRef); };
				FGraphCompoundRuntimeError Error;
				if (!Input.IsValid())
				{
					auto Facts = MakeShared<FJsonObject>();
					Facts->SetBoolField(TEXT("contract_present"), Input.Contract != nullptr);
					Facts->SetBoolField(TEXT("unit_present"), Input.Unit != nullptr);
					Facts->SetBoolField(TEXT("input_present"), Input.Input != nullptr);
					return Fail(Failure, TextureApplyFact(Request.TargetRef, TEXT("texture_graph_prepare_invalid"),
						TEXT("TextureGraph preparation requires an input contract, workspace unit and operation input."),
						TEXT("/prepare_request"), MakeShared<FJsonValueObject>(Facts), MakeShared<FJsonValueString>(TEXT("contract, unit and input present"))));
				}
				if (!Initialization && !TryValidateTextureGraphSemanticComposite(Request, Plan.ToSharedRef(), Cancellation, Error))
					return Fail(Failure, MoveTemp(Error));
				UTextureGraph* Actual = nullptr;
				TStrongObjectPtr<UTextureGraph> Proposed;
				if (Initialization)
				{
					UE::UAI::Contract::FDiagnostic PresenceError;
					if (!Initialization->Plan.IsValid()) return Fail(Failure, TextureApplyFact(Request.TargetRef,
						TEXT("texture_graph_initialization_fence_failed"), TEXT("TextureGraph initialization has no prepared plan."),
						TEXT("/initialization_plan"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
					if (!Missing(Request.TargetRef, PresenceError)) return Fail(Failure, MoveTemp(PresenceError));
					ExecutionMutation::FExecutionMutationError CreateError;
					UObject* Created = AssetDataSimpleEffect::CreateNativeFactoryObject(GetTransientPackage(),
						MakeUniqueObjectName(GetTransientPackage(), Initialization->Factory.GetConcreteClass()),
						Initialization->Factory.GetConcreteClass(), Initialization->Factory.GetFactory(), true, CreateError);
					Proposed.Reset(Cast<UTextureGraph>(Created));
					if (!Proposed.IsValid())
					{
						if (Created) CreateError = TextureApplyFact(Request.TargetRef, TEXT("texture_graph_initial_factory_type_invalid"),
							TEXT("The native factory returned an unpublished object of a different class."), TEXT("/created_class"),
							MakeShared<FJsonValueString>(Created->GetClass()->GetPathName()), MakeShared<FJsonValueString>(UTextureGraph::StaticClass()->GetPathName()));
						return Fail(Failure, MoveTemp(CreateError));
					}
					InitializeFactoryContext(*Proposed, Request.TargetRef);
				}
				else
				{
					Actual = LoadObject<UTextureGraph>(nullptr, *Request.TargetRef);
					if (!Actual)
					{
						auto Diagnostic = TextureApplyFact(Request.TargetRef, TEXT("texture_graph_target_missing"),
							TEXT("The TextureGraph target selected for update could not be loaded."), TEXT("/target"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						return Fail(Failure, MoveTemp(Diagnostic));
					}
					Proposed.Reset(DuplicateObject<UTextureGraph>(Actual, GetTransientPackage()));
				}
				if (!Proposed.IsValid())
				{
					auto Diagnostic = TextureApplyFact(Request.TargetRef, TEXT("texture_graph_prepare_copy_failed"),
						TEXT("Unreal did not duplicate the TextureGraph target for preparation."), TEXT("/proposed_copy"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return Fail(Failure, MoveTemp(Diagnostic));
				}
				Proposed->ClearFlags(RF_Public | RF_Standalone);
				auto ProposedBindings = Request.NodeBindings;
				if (!TryApplyTextureGraphSemanticCompositeNative(*Proposed, Request, Error, ProposedBindings))
				{ Release(Proposed); return Fail(Failure, Error); }
				FSettingPlans ProposedPlans;
				FSettingIntents ProposedIntents;
				if (!PrepareSettings(*Proposed, Request, ProposedBindings, ProposedPlans, Failure) || !ApplySettings(ProposedPlans, ProposedIntents, Failure))
				{ Release(Proposed); return false; }
				OutState = MakeUnique<FGraphState>(Request, Plan, Cancellation, Actual, Proposed.Get(),
					MoveTemp(ProposedBindings), MoveTemp(ProposedIntents), Initialization);
				Proposed.Reset();
				return true;
			}
		private:
			FTextureGraphSemanticCompositeRequest Request;
			PlanningPreflight::FPlanningPreflightPlanPtr Plan;
			TSharedRef<Contract::IUeAgentCancellationSignal> Cancellation;
			TSharedPtr<FInitialization> Initialization;
		};
		UeAgentApplyInputContract::FContract MakeInputContract()
		{
			using namespace UeAgentApplyInputContract;
			FContract Result;
			Result.Schema = TEXT("uai.apply_input_contract.v1");
			Result.ContractId = InputId;
			Result.ContractVersion = TEXT("1.0.0");
			Result.ProfileId = TEXT("texture_graph.semantic");
			Result.TargetKind = TargetKind;
			Result.SemanticDescriptorRefs = {SemanticId};
			Result.ReflectionBaseline.RootKinds = {TEXT("uobject"), TEXT("ustruct")};
			Result.ReflectionBaseline.RecursiveNodes = {TEXT("scalar"), TEXT("struct"), TEXT("owned_object"),
				TEXT("reference"), TEXT("array"), TEXT("set"), TEXT("map"), TEXT("fixed_array"), TEXT("optional")};
			Result.ReflectionBaseline.UnknownFieldPolicy = TEXT("reject");
			Result.ReflectionBaseline.ExternalReferencePolicy = TEXT("replace_reference_only");
			Result.ReflectionBaseline.OwnedObjectPolicy = TEXT("create_or_locate_with_real_outer_then_recurse");
			Result.PropertyPayload.Mode = TEXT("reflection");
			Result.PropertyPayload.PropertyPathField = TEXT("property_path");
			Result.PropertyPayload.ValueField = TEXT("value");
			Result.PropertyPayload.OmittedPatchFieldPolicy = TEXT("preserve_current");
			Result.InputFields = {
				{TEXT("asset_path"), TEXT("identity"), TEXT("ue_asset_path"), {EOperation::Update, EOperation::Create}},
				{TEXT("operation"), TEXT("operation_control"), TEXT("apply_operation"), {EOperation::Update, EOperation::Create}},
				{TEXT("plan_fingerprint"), TEXT("provider_extension"), TEXT("string")}};
			FOperationDescriptor Operation;
			Operation.Operation = EOperation::Update;
			Operation.IdentityOneOf = {{TEXT("asset_path")}};
			Operation.AcceptedSections = {TEXT("target"), TEXT("provider_extension")};
			Operation.OmittedFieldPolicy = TEXT("preserve_current");
			Operation.NativeDefaultPolicy = TEXT("forbidden");
			Result.Operations = {Operation};
			Operation.Operation = EOperation::Create; Operation.NativeDefaultPolicy = TEXT("required"); Result.Operations.Add(Operation);
			Result.ProviderId = ProviderId;
			Result.UnknownInputFieldPolicy = TEXT("reject");
			Result.Verification.StateSource = TEXT("shared_state_snapshot");
			Result.Verification.CompareSource = TEXT("shared_semantic_compare");
			return Result;
		}

		UeAgentApplyWorkspaceContract::FContract MakeWorkspaceContract()
		{
			using namespace UeAgentApplyWorkspaceContract;
			UeAgentApplyWorkspaceContract::FContract Result;
			Result.Schema = TEXT("uai.apply_workspace_contract.v1");
			Result.ContractId = WorkspaceId;
			Result.ContractVersion = TEXT("1.0.0");
			Result.ProfileId = TEXT("texture_graph.semantic");
			Result.Sources = {
				{ESourceKind::Directory, TEXT("physical"), TEXT("index")},
				{ESourceKind::SingleFile, TEXT("virtual"), TEXT("object")},
				{ESourceKind::Inline, TEXT("virtual"), TEXT("object")}};
			Result.Entry.DirectoryEntry = TEXT("index.hjson");
			Result.Entry.VirtualUnitId = TEXT("graph");
			FUnitDescriptor Unit;
			Unit.UnitId = TEXT("graph");
			Unit.PathPattern = TEXT("index.hjson");
			Unit.Role = TEXT("object");
			Unit.Cardinality = TEXT("one");
			Unit.bRequired = true;
			Unit.RouteId = TEXT("texture_graph.semantic");
			Unit.ApplyContractRef = InputId;
			Unit.MergePolicy = TEXT("replace");
			Unit.AbsentSemantics = TEXT("error");
			Unit.EmptySemantics = TEXT("error");
			Result.Units = {Unit};
			Result.ReferencePolicy.Base = TEXT("workspace_root");
			Result.ReferencePolicy.RootContainment = TEXT("required");
			Result.ReferencePolicy.ExternalReferences = TEXT("reject");
			Result.ReferencePolicy.CaseCollision = TEXT("reject");
			Result.DependencyPolicy.EdgeSource = TEXT("contract_and_references");
			Result.DependencyPolicy.CyclePolicy = TEXT("reject");
			Result.DependencyPolicy.UnresolvedPolicy = TEXT("reject");
			Result.DependencyPolicy.Order = TEXT("stable_topological");
			Result.Preflight.Scope = TEXT("whole_workspace");
			Result.Preflight.MutationBeforeSuccess = TEXT("forbid");
			Result.Preflight.bDiagnosticsIncludeSource = true;
			Result.Transaction.Mode = TEXT("atomic");
			Result.Transaction.RollbackOn = {TEXT("mutation_failed"), TEXT("compile_failed"),
				TEXT("save_failed"), TEXT("verification_failed")};
			Result.Execution.Compile = TEXT("provider_hook");
			Result.Execution.Save = TEXT("caller_controlled");
			Result.Execution.Verification = TEXT("shared_snapshot_required");
			Result.UnknownFilePolicy = TEXT("reject");
			Result.DuplicateTargetPolicy = TEXT("reject");
			return Result;
		}
	}

	static bool ExecuteComposite(const FTextureGraphSemanticCompositeRequest& Request,
		const PlanningPreflight::FPlanningPreflightPlanPtr& Plan, const TSharedPtr<FInitialization>& Initialization,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
		const FBackendOptions& Options, FApplyWorkspaceResult& OutResult, UE::UAI::Contract::FDiagnostic& Error)
	{
		const auto WorkspaceContract = MakeWorkspaceContract();
		UeAgentApplyInputContract::ERegistrationResult Registered;
		UeAgentAuthoringOperationApply::ERegistrationResult ProviderRegistered;
		FProviderRegistry Providers;
		if (!RegisterState(Error)
			|| !UeAgentApplyInputContract::FRegistry::Get().RegisterContract(MakeInputContract(), Registered, Error)
			|| !Providers.RegisterProvider(MakeShared<FGraphProvider>(Request, Plan, Cancellation, Initialization), ProviderRegistered, Error))
			return false;
		const auto Target = MakeShared<FJsonObject>();
		Target->SetStringField(TEXT("asset_path"), Request.TargetRef);
		const auto Extension = MakeShared<FJsonObject>();
		Extension->SetStringField(TEXT("plan_fingerprint"), Initialization ? Initialization->Plan->GetPlanFingerprint() : Plan->GetFingerprint());
		const auto Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("operation"), Initialization ? TEXT("create") : TEXT("update"));
		Document->SetObjectField(TEXT("target"), Target);
		Document->SetObjectField(TEXT("provider_extension"), Extension);
		UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
		if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(WorkspaceContract,
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(Document, TEXT("inline://texture-graph-semantic")), Load))
		{ Error = Load.Diagnostic; return false; }
		UeAgentAuthoringAssetWorkspaceApply::FFailure Failure;
		const bool bApplied = UeAgentAuthoringAssetWorkspaceApply::Execute(WorkspaceContract,
			Load.Workspace, Providers, Initialization ? TEXT("texture-graph-initialize") : Plan->GetRequestId(), true, Options, OutResult, Failure);
		if (!bApplied)
		{
			auto Diagnostic = Failure.Diagnostic;
			Diagnostic.RollbackAttempted = OutResult.Execution.bRollbackAttempted;
			if (OutResult.Execution.bRollbackAttempted)
				Diagnostic.RollbackSucceeded = OutResult.Execution.bRollbackSucceeded;
			Error = MoveTemp(Diagnostic);
		}
		return bApplied;
	}
	bool ApplySemanticComposite(const FTextureGraphSemanticCompositeRequest& Request,
		const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
		const FBackendOptions& Options, FApplyWorkspaceResult& OutResult, UE::UAI::Contract::FDiagnostic& Error)
	{
		return ExecuteComposite(Request, Plan, nullptr, Cancellation, Options, OutResult, Error);
	}

	bool ApplyInitialGraph(const FString& Target, const TSharedRef<FJsonObject>& Graph,
		const AssetDataSimpleEffect::FUeAgentAssetDataSimpleEffectFactoryBinding& Factory, const FString& AuthorizationId,
		const FBackendOptions& Options, FApplyWorkspaceResult& OutResult, UE::UAI::Contract::FDiagnostic& Error,
		const HjsonWorkspace::FHjsonDocument* SourceDocument)
	{
		using namespace HjsonWorkspace;
		using namespace StateSemantic;
		OutResult = {}; Error.Reset();
		if (AuthorizationId.IsEmpty())
		{ Error = TextureApplyFact(Target, TEXT("texture_graph_initialization_authorization_missing"), TEXT("TextureGraph initialization requires an impact authorization."),
			TEXT("/authorization_id"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("command_input")); return false; }
		if (!Missing(Target, Error)) return false;
		if (!Factory.IsValid())
		{
			auto Facts = MakeShared<FJsonObject>();
			const auto* Candidate = Factory.GetCandidateBinding();
			Facts->SetBoolField(TEXT("candidate_binding_valid"), Candidate && Candidate->IsValid());
			Facts->SetBoolField(TEXT("profile_present"), !Factory.GetProfileId().IsEmpty());
			Facts->SetBoolField(TEXT("concrete_class_present"), Factory.GetConcreteClass() != nullptr);
			Facts->SetBoolField(TEXT("factory_mechanic_present"), !Factory.GetFactoryMechanicId().IsEmpty());
			Error = TextureApplyFact(Target, TEXT("texture_graph_initial_factory_invalid"),
				TEXT("The prepared TextureGraph factory binding has missing or invalid required data."), TEXT("/factory_binding"),
				MakeShared<FJsonValueObject>(Facts), MakeShared<FJsonValueString>(TEXT("valid candidate binding, profile, concrete class and factory mechanic")));
			return false;
		}
		if (!Factory.IsGenericReady())
		{
			Error = TextureApplyFact(Target, TEXT("texture_graph_initial_factory_invalid"),
				TEXT("The TextureGraph factory binding is not ready for generic candidate creation."), TEXT("/factory/generic_ready"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		if (Factory.GetConcreteClass() != UTextureGraph::StaticClass())
		{
			Error = TextureApplyFact(Target, TEXT("texture_graph_initial_factory_invalid"),
				TEXT("TextureGraph initialization requires the concrete TextureGraph class."), TEXT("/factory/class"),
				MakeShared<FJsonValueString>(Factory.GetConcreteClass() ? Factory.GetConcreteClass()->GetPathName() : TEXT("<missing>")),
				MakeShared<FJsonValueString>(UTextureGraph::StaticClass()->GetPathName()));
			return false;
		}
		if (!Factory.GetFactory())
		{
			Error = TextureApplyFact(Target, TEXT("texture_graph_initial_factory_invalid"),
				TEXT("The native factory object is unavailable for TextureGraph initialization."), TEXT("/factory/object"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		for (const auto& Field : Graph->Values)
			if (Field.Key != TEXT("nodes") && Field.Key != TEXT("connections"))
			{ Error = TextureApplyFact(Target, TEXT("texture_graph_initial_graph_field_unsupported"), TEXT("The initial TextureGraph graph only accepts nodes and connections fields."),
				TEXT("/root/graph/") + HjsonWorkspace::EscapeJsonPointerSegment(Field.Key), MakeShared<FJsonValueString>(Field.Key), MakeShared<FJsonValueString>(TEXT("nodes or connections")), TEXT("related_input")); return false; }
		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		if (!Graph->TryGetObjectField(TEXT("nodes"), Nodes) || !Nodes)
		{ Error = TextureApplyFact(Target, TEXT("texture_graph_initial_nodes_invalid"), TEXT("The initial TextureGraph nodes field must be an object."),
			TEXT("/root/graph/nodes"), TextureApplyShape(Graph->TryGetField(TEXT("nodes"))), MakeShared<FJsonValueString>(TEXT("object")), TEXT("related_input")); return false; }
		for (const auto& Entry : (*Nodes)->Values)
		{
			if (Entry.Value->Type != EJson::Object)
			{ Error = TextureApplyFact(Target, TEXT("texture_graph_initial_node_invalid"), TEXT("Each initial TextureGraph node must be an object."),
				TEXT("/root/graph/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Entry.Key), TextureApplyShape(Entry.Value), MakeShared<FJsonValueString>(TEXT("object")), TEXT("related_input")); return false; }
			for (const auto& Field : Entry.Value->AsObject()->Values)
			{
				if (Field.Key == TEXT("name") || Field.Key == TEXT("candidate")) continue;
				if (Field.Key == TEXT("properties"))
				{
					TSharedPtr<FJsonObject> Properties;
					if (!UeAgentDetailsAuthoringService::NormalizePropertyPayload(Field.Value, Properties, Error))
					{
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error,
							TEXT("/root/graph/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Entry.Key) + TEXT("/properties"));
						return false;
					}
					continue;
				}
				if (Field.Key != TEXT("inputs") || Field.Value->Type != EJson::Object)
				{ Error = TextureApplyFact(Target, TEXT("texture_graph_initial_node_field_unsupported"), TEXT("An initial TextureGraph node accepts name, candidate, properties and an inputs object."),
					TEXT("/root/graph/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Entry.Key) + TEXT("/") + HjsonWorkspace::EscapeJsonPointerSegment(Field.Key), TextureApplyShape(Field.Value), MakeShared<FJsonValueString>(TEXT("name, candidate, properties or inputs object")), TEXT("related_input")); return false; }
				for (const auto& Input : Field.Value->AsObject()->Values)
				{
					if (Input.Value->Type != EJson::Object)
					{ Error = TextureApplyFact(Target, TEXT("texture_graph_initial_input_invalid"), TEXT("Each initial TextureGraph input must be an object."),
						TEXT("/root/graph/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Entry.Key) + TEXT("/inputs/") + HjsonWorkspace::EscapeJsonPointerSegment(Input.Key), TextureApplyShape(Input.Value), MakeShared<FJsonValueString>(TEXT("object")), TEXT("related_input")); return false; }
					const auto InputObject = Input.Value->AsObject();
					if (!InputObject->HasField(TEXT("value")) || (InputObject->HasField(TEXT("alias")) && !InputObject->HasTypedField<EJson::String>(TEXT("alias"))))
					{ Error = TextureApplyFact(Target, TEXT("texture_graph_initial_input_value_required"), TEXT("An initial TextureGraph input requires a value and a string alias when supplied."),
						TEXT("/root/graph/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Entry.Key) + TEXT("/inputs/") + HjsonWorkspace::EscapeJsonPointerSegment(Input.Key) + (!InputObject->HasField(TEXT("value")) ? TEXT("/value") : TEXT("/alias")), TextureApplyShape(InputObject->TryGetField(!InputObject->HasField(TEXT("value")) ? TEXT("value") : TEXT("alias"))), MakeShared<FJsonValueString>(!InputObject->HasField(TEXT("value")) ? TEXT("value") : TEXT("string")), TEXT("related_input")); return false; }
					for (const auto& InputField : Input.Value->AsObject()->Values)
						if (InputField.Key != TEXT("alias") && InputField.Key != TEXT("value"))
						{ Error = TextureApplyFact(Target, TEXT("texture_graph_initial_input_field_unsupported"), TEXT("An initial TextureGraph input only accepts value and alias fields."),
							TEXT("/root/graph/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Entry.Key) + TEXT("/inputs/") + HjsonWorkspace::EscapeJsonPointerSegment(Input.Key) + TEXT("/") + HjsonWorkspace::EscapeJsonPointerSegment(InputField.Key), MakeShared<FJsonValueString>(InputField.Key), MakeShared<FJsonValueString>(TEXT("value or alias")), TEXT("related_input")); return false; }
				}
			}
		}
		ExecutionMutation::FExecutionMutationError FactoryError;
		UObject* CreatedTemplate = AssetDataSimpleEffect::CreateNativeFactoryObject(
			GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UTextureGraph::StaticClass()),
			Factory.GetConcreteClass(), Factory.GetFactory(), true, FactoryError);
		TStrongObjectPtr<UTextureGraph> Template(Cast<UTextureGraph>(CreatedTemplate));
		if (!Template.IsValid())
		{
			Error = CreatedTemplate ? TextureApplyFact(Target, TEXT("texture_graph_initial_factory_type_invalid"),
				TEXT("The native factory returned an unpublished object of a different class."), TEXT("/created_class"),
				MakeShared<FJsonValueString>(CreatedTemplate->GetClass()->GetPathName()), MakeShared<FJsonValueString>(UTextureGraph::StaticClass()->GetPathName())) : FactoryError;
			return false;
		}
		ON_SCOPE_EXIT { Release(Template); };
		InitializeFactoryContext(*Template, Target);
		auto Resources = MakeSemanticWorkspaceResourceTrackerOwner();
		auto Empty = MakeShared<FJsonObject>(); Empty->SetObjectField(TEXT("nodes"), MakeShared<FJsonObject>()); Empty->SetObjectField(TEXT("edges"), MakeShared<FJsonObject>());
		FUeAgentCanonicalValuePtr Visible, Captured;
		TMap<FString, FUeAgentCanonicalValueRef> Values;
		FWorkspaceError WorkspaceError;
		if (!CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueObject>(Graph), *Resources, Visible, Error))
		{
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/root/graph"));
			return false;
		}
		if (!CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueObject>(Empty), *Resources, Captured, Error)) return false;
		if (!TryResolveGraphAuthoringProjection(*Visible, *Captured, TEXT("root.graph"), *Resources, Values, WorkspaceError))
		{
			Error = WorkspaceError.GetDiagnostic();
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/root/graph"));
			return false;
		}
		TArray<FSemanticUnit> Units;
		Units.Emplace(TEXT("target"), ESemanticUnitKind::TargetState, FString(), TEXT("/Script/TextureGraph.TextureGraph"),
			TEXT("target"), FString(), TMap<FString, FUeAgentCanonicalValueRef>{}, TArray<FString>{}, TArray<FString>{}, FString(), FString(), TArray<FString>{}, TArray<FString>{TEXT("llm_authoring")}, TEXT("texture_graph.initialization"));
		Units.Emplace(TEXT("graph"), ESemanticUnitKind::StructureState, TEXT("target"), TEXT("uai.semantic.texture_graph.graph"),
			TEXT("graph"), FString(), MoveTemp(Values), TArray<FString>{}, TArray<FString>{}, FString(), FString(), TArray<FString>{}, TArray<FString>{TEXT("llm_authoring")}, TEXT("texture_graph.initialization"));
		FAuthoringModelPtr Model;
		if (!TryCompileAuthoringModel(TEXT("ue.texture_graph.asset"), 1, TEXT("llm_authoring"), TEXT("missing"), MoveTemp(Units), {TEXT("target")}, *Resources, Model, WorkspaceError))
		{
			Error = WorkspaceError.GetDiagnostic();
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/root/graph"));
			return false;
		}
		ContentGraphRead::FUeAgentContentGraphCapture EmptyCapture;
		FTextureGraphSemanticCompositeRequest Composite;
		if (!TryBuildSemanticComposite(Model.ToSharedRef(), EmptyCapture, *Template, Target, Composite, WorkspaceError))
		{ Error = WorkspaceError.GetDiagnostic(); return false; }
		if (SourceDocument)
		{
			for (auto& Settings : Composite.ElementSettings)
			{
				FJsonObject Payload;
				Payload.SetArrayField(TEXT("properties"), Settings.Value);
				HjsonWorkspace::AttachInlinePropertySources(*SourceDocument,
					TEXT("/root/graph/nodes/") + EscapeJsonPointerSegment(Settings.Key), Payload);
			}
		}
		// Native content validation runs on the unpublished factory template before target creation.
		FGraphCompoundRuntimeError NativeError;
		auto Bindings = Composite.NodeBindings;
		if (!TryApplyTextureGraphSemanticCompositeNative(*Template, Composite, NativeError, Bindings))
		{ Error = NativeError; return false; }
		TMap<EUnifiedHjsonCompositionKind, TArray<FString>> Compositions;
		for (auto Kind : GetAllUnifiedHjsonCompositionKinds()) Compositions.Add(Kind, {});
		Compositions[EUnifiedHjsonCompositionKind::Target] = {TEXT("target")};
		Compositions[EUnifiedHjsonCompositionKind::Structure] = {TEXT("graph")};
		FUnifiedHjsonSemanticModelPtr Unified;
		if (!TryCreateUnifiedHjsonSemanticModel(Model.ToSharedRef(), EUnifiedHjsonAuthoringIntent::MinimalCreate, MoveTemp(Compositions), *Resources, Unified, WorkspaceError))
		{ Error = WorkspaceError.GetDiagnostic(); return false; }
		class FPreflight final : public IImportInitializationPreflight
		{
		public:
			FPreflight(FString InTarget, FString InAuthorizationId) : Target(MoveTemp(InTarget)), AuthorizationId(MoveTemp(InAuthorizationId)) {}
			bool TryReadCurrentTargetRevision(const FString& Identity, FUeAgentStateRevision& Revision, FWorkspaceError& Failure) override
			{
				UE::UAI::Contract::FDiagnostic Error;
				if (Identity != Target)
				{
					Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("texture_graph_initialization_target_present"),
						TEXT("The requested initialization target differs from the prepared TextureGraph target."), TEXT("preflight"));
					Error.TargetPath = Target;
					Error.ActualValue = MakeShared<FJsonValueString>(Identity);
					Error.ExpectedValue = MakeShared<FJsonValueString>(Target);
					Failure.SetFailure(Error);
					return false;
				}
				if (!Missing(Target, Error)) { Failure.SetFailure(Error); return false; }
				Revision = FUeAgentStateRevision(TEXT("texture_graph"), TEXT("missing"), 0); return true;
			}
			bool TryResolveCandidate(const FString& Candidate, const FString&, FString&, FWorkspaceError& Failure) override
			{
				auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("texture_graph_unbound_initialization_candidate"),
					TEXT("The TextureGraph initialization plan has no binding for this candidate."), TEXT("preflight"));
				Detail.CandidateName = Candidate;
				Detail.TargetPath = Target;
				Failure.SetFailure(MoveTemp(Detail));
				return false;
			}
			bool SupportsChange(const FImportChange& Change, FWorkspaceError&) override
			{ return Change.GetKind() == EImportChangeKind::Create && Change.GetTargetIdentity() == Target; }
			bool TryAuthorizeImpact(const TArray<FString>& Scopes, const FString& Fingerprint, const FString& Authorization, FWorkspaceError& Failure) override
			{
				if (Scopes != TArray<FString>{Target} || Authorization != AuthorizationId || AuthorizationId.IsEmpty() || !Fingerprint.StartsWith(TEXT("sha256:")))
				{
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("texture_graph_initialization_impact_unauthorized"),
						Scopes != TArray<FString>{Target} ? TEXT("The requested impact scopes differ from the prepared TextureGraph target scope.")
						: AuthorizationId.IsEmpty() ? TEXT("TextureGraph initialization has no impact authorization.")
						: Authorization != AuthorizationId ? TEXT("The supplied authorization does not match this initialization plan.")
						: TEXT("The initialization plan fingerprint is missing or invalid."), TEXT("preflight"));
					Detail.TargetPath = Target;
					Detail.InternalContext = Fingerprint;
					if (Scopes != TArray<FString>{Target})
					{
						TArray<TSharedPtr<FJsonValue>> Values;
						for (const auto& Scope : Scopes) Values.Add(MakeShared<FJsonValueString>(Scope));
						Detail.ActualValue = MakeShared<FJsonValueArray>(MoveTemp(Values));
						Detail.ExpectedValue = MakeShared<FJsonValueString>(Target);
					}
					Failure.SetFailure(MoveTemp(Detail));
					return false;
				}
				return true;
			}
		private: FString Target, AuthorizationId;
		};
		auto Initialization = MakeShared<FInitialization>(); Initialization->Factory = Factory;
		const FImportInitializationPlanningRequest Planning(EImportSourceCarrier::Inline, Unified.ToSharedRef(), Target,
			FUeAgentStateRevision(TEXT("texture_graph"), TEXT("missing"), 0), Model->GetSemanticFingerprint(), Model->GetSemanticFingerprint(),
			{FImportChange(TEXT("create-root"), EImportChangeKind::Create, Target)},
			{FInitializationNode(Target, EInitializationNodeRole::RootTarget, Factory.GetConcreteClass()->GetPathName(), TEXT("uai.init.texture_graph.root/1"))}, {}, {Target}, AuthorizationId);
		if (!TryPlanHjsonImportInitialization(Planning, MakeShared<FPreflight>(Target, AuthorizationId), *Resources, Initialization->Plan, WorkspaceError))
		{ Error = WorkspaceError.GetDiagnostic(); return false; }
		class FNeverCancelled final : public Contract::IUeAgentCancellationSignal { public: bool IsCancelled() const override { return false; } };
		return ExecuteComposite(Composite, nullptr, Initialization, MakeShared<FNeverCancelled>(), Options, OutResult, Error);
	}

}
