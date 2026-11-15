// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Material/Authoring/UeAgentMaterialAuthoringApplyProvider.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringCompileAndPersistence.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringDescriptors.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringState.h"
#include "Domains/Material/Authoring/UeAgentMaterialGraphAuthoring.h"
#include "Domains/Material/Authoring/UeAgentMaterialInstanceAuthoring.h"
#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphNativeHook.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringPropertyValues.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRemovalStage.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceApply.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceInput.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace UeAgentMaterialAuthoring
{
	namespace
	{
		using namespace UeAgentApplyInputContract;
		using namespace UeAgentAuthoringOperationApply;

		UE::UAI::Contract::FDiagnostic AssetFailure(const TCHAR* Code, const FString& Reason,
			const FString& Field, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected,
			const FJsonObject* Input = nullptr)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
			Error.PropertyPath = Field;
			Error.ActualValue = Actual;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValue = Expected;
			if (Input) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Input, Field);
			return Error;
		}

		void SetFailure(FOperationFailure& OutFailure, UE::UAI::Contract::FDiagnostic Diagnostic,
    const FString& Subject = FString()) {
  OutFailure.Diagnostic = MoveTemp(Diagnostic);
  if (OutFailure.Diagnostic.SubjectId.IsEmpty()) OutFailure.Diagnostic.SubjectId = Subject;
}







		UObject* LoadAsset(const FString& InPath)
		{
			const FString PackagePath =
				UeAgentAuthoringAssetIdentity::NormalizePackagePath(InPath);
			if (PackagePath.IsEmpty()) return nullptr;
			UObject* Object = StaticLoadObject(
				UObject::StaticClass(),
				nullptr,
				*UeAgentAuthoringAssetIdentity::ToObjectPath(PackagePath));
			return GetAssetKind(Object).IsEmpty() ? nullptr : Object;
		}

		UObject* DuplicatePublishedAsset(UObject* Source, UPackage* Package, const FString& Name)
		{
			if (!Source || !Package || Name.IsEmpty()) return nullptr;
			UObject* Published = StaticDuplicateObject(Source, Package, *Name, RF_AllFlags);
			if (Published)
			{
				Published->ClearFlags(RF_Transient);
				Published->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
			}
			return Published;
		}

		bool DeleteAsset(UObject* Asset, const FString& Target, UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			if (!Asset) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_delete_target_missing"),
					TEXT("No material asset object was supplied for deletion."), TEXT("authoring_apply"));
				OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = Target; OutError.PropertyPath = TEXT("asset");
				OutError.ActualValuePresent = true; OutError.ActualValue = MakeShared<FJsonValueNull>();
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("material asset object"));
				return false;
			}
			TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
			if (!ObjectTools::DeleteSingleObject(Asset, true)) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_delete_rejected"),
					TEXT("The editor rejected deletion of the material asset object."), TEXT("authoring_apply"),
					TEXT("DeleteSingleObject returned false with reference checking enabled; its bool API does not distinguish deletion-policy and reference-check rejection."));
				OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = Target; OutError.PropertyPath = TEXT("deletion_succeeded");
				OutError.ActualValuePresent = true; OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				return false;
			}
			return true;
		}

		bool CompileAsset(UObject* Asset, UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			if (Asset)
			{
				using namespace UE::UAI::DomainAdapters::MaterialNiagaraGraph;
				FMaterialNiagaraGraphCompileSettleReceipt Receipt;
				FMaterialNiagaraGraphAdapterError Error;
				if (!FMaterialNiagaraGraphNativeHook().TryCompileAndSettleMaterial(*Asset, Receipt, Error))
				{
					OutError = Cast<UMaterial>(Asset)
						? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("material_compile_failed"), Error)
						: Error;
					return false;
				}
				return true;
			}
			OutError = UE::UAI::Contract::FDiagnostic::Failure(
				TEXT("material_compile_target_invalid"), TEXT("The material compile target is unavailable."), TEXT("authoring_apply"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("/target/available");
			OutError.ActualValuePresent = true;
			OutError.ExpectedValuePresent = true;
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}

		bool ReadBool(const TSharedPtr<const FJsonObject>& Object, const TCHAR* Field, const bool DefaultValue)
		{
			bool Value = DefaultValue;
			if (Object.IsValid()) Object->TryGetBoolField(Field, Value);
			return Value;
		}

		struct FFunctionSettings
		{
			bool bHasDescription = false;
			FString Description;
			bool bHasExposeToLibrary = false;
			bool bExposeToLibrary = false;

			bool HasAny() const { return bHasDescription || bHasExposeToLibrary; }
		};

		bool ParseFunctionSettings(
			const TSharedPtr<const FJsonObject>& Extension,
			FFunctionSettings& OutSettings,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutSettings = FFunctionSettings();
			OutError.Reset();
			const TSharedPtr<FJsonObject>* Settings = nullptr;
			if (!Extension.IsValid() || !Extension->HasField(TEXT("function_settings"))) return true;
			if (!Extension->TryGetObjectField(TEXT("function_settings"), Settings) || !Settings || !Settings->IsValid())
			{
				OutError = AssetFailure(TEXT("material_function_settings_invalid"), TEXT("function_settings must be an object."),
					TEXT("function_settings"), Extension->TryGetField(TEXT("function_settings")), MakeShared<FJsonValueString>(TEXT("object")), Extension.Get());
				return false;
			}
			if ((*Settings)->HasField(TEXT("description")))
			{
				OutSettings.bHasDescription = (*Settings)->TryGetStringField(
					TEXT("description"), OutSettings.Description);
				if (!OutSettings.bHasDescription)
				{
					OutError = AssetFailure(TEXT("material_function_description_invalid"), TEXT("Material function description must be a string."),
						TEXT("description"), (*Settings)->TryGetField(TEXT("description")), MakeShared<FJsonValueString>(TEXT("string")), Settings->Get());
					return false;
				}
			}
			if ((*Settings)->HasField(TEXT("expose_to_library")))
			{
				OutSettings.bHasExposeToLibrary = (*Settings)->TryGetBoolField(
					TEXT("expose_to_library"), OutSettings.bExposeToLibrary);
				if (!OutSettings.bHasExposeToLibrary)
				{
					OutError = AssetFailure(TEXT("material_function_expose_to_library_invalid"), TEXT("Material function expose_to_library must be a boolean."),
						TEXT("expose_to_library"), (*Settings)->TryGetField(TEXT("expose_to_library")), MakeShared<FJsonValueString>(TEXT("boolean")), Settings->Get());
					return false;
				}
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Settings)->Values)
			{
				if (Field.Key != TEXT("description") && Field.Key != TEXT("expose_to_library"))
				{
					OutError = AssetFailure(TEXT("material_function_settings_unknown_field"),
						FString::Printf(TEXT("Unknown material function setting '%s'; allowed settings are description and expose_to_library."), *Field.Key),
						Field.Key, Field.Value, MakeShared<FJsonValueString>(TEXT("description or expose_to_library")), Settings->Get());
					return false;
				}
			}
			return true;
		}

		class FAssetLifecycleState final : public IReflectionOperationLifecycleState
		{
		public:
			FAssetLifecycleState(
				FString InPackagePath,
				FString InAssetKind,
				EOperation InOperation,
				UObject* InExisting,
				UObject* InProposed,
				TUniquePtr<FPreparedGraph> InGraph,
				TArray<FInstanceParameterEdit> InInstanceEdits,
				TSharedPtr<FJsonValue> InInstanceLayers,
				FFunctionSettings InFunctionSettings,
				bool bInCompile,
				bool bInSave, bool bInEditsInstanceParent, const TSharedPtr<const FJsonObject>& InTarget, const TSharedPtr<const FJsonObject>& InExtension)
				: PackagePath(MoveTemp(InPackagePath))
				, AssetKind(MoveTemp(InAssetKind))
				, Operation(InOperation)
				, Existing(InExisting)
				, Proposed(InProposed)
				, Graph(MoveTemp(InGraph))
				, InstanceEdits(MoveTemp(InInstanceEdits))
				, InstanceLayers(MoveTemp(InInstanceLayers))
				, FunctionSettings(MoveTemp(InFunctionSettings))
				, bCompile(bInCompile)
				, bSave(bInSave)
				, bEditsInstanceParent(bInEditsInstanceParent)
			{
				// This diagnostic wrapper only reads the original objects; retaining identity preserves source locations.
				if (InTarget) SourceDocument->SetObjectField(TEXT("target"), ConstCastSharedPtr<FJsonObject>(InTarget));
				if (InExtension) SourceDocument->SetObjectField(TEXT("provider_extension"), ConstCastSharedPtr<FJsonObject>(InExtension));
				SourceLocations = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*SourceDocument);
			}

			virtual FString GetTargetKey() const override { return PackagePath; }
			virtual bool ShouldApplyPropertiesToActualRoot() const override { return AssetKind == TEXT("material_instance"); }
			virtual EOperation GetResolvedOperation() const override { return Operation; }
			virtual FReflectionRoot GetProposedRoot() const override
			{
				return FReflectionRoot::MakeObject(Proposed.Get());
			}
			virtual FReflectionRoot GetActualRoot() const override
			{
				return FReflectionRoot::MakeObject(Actual ? Actual : Existing);
			}
			virtual bool CollectPropertyDescriptors(TArray<TSharedPtr<FJsonValue>>& Descriptors,
				FOperationFailure& OutFailure) const override
			{
				if (!FunctionSettings.HasAny()) return true;
				UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(&SourceDocument.Get(),
					[this](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(SourceLocations, Pointer); });
				auto Merge = [&](const TCHAR* Path, const TCHAR* InputField, const TSharedRef<FJsonValue>& Input)
				{
					FProperty* Property = FindFProperty<FProperty>(UMaterialFunction::StaticClass(), Path);
					UE::UAI::Contract::FDiagnostic Error;
					UeAgentPropertyValueCodec::FOptions Codec;
					Codec.bCompareStoredValues = true;
					auto Alias = UeAgentAuthoringPropertyValues::FPropertyValueStorage::CreateInitialized(Property, Error);
					if (!Alias || !UeAgentPropertyValueCodec::Decode(Property, Input, Alias->GetMutableValuePtr(), Proposed.Get(), Codec, Error))
					{
						if (Error.PropertyPath.IsEmpty()) Error.PropertyPath = Path;
						const TSharedPtr<FJsonObject>* Extension = nullptr;
						const TSharedPtr<FJsonObject>* Settings = nullptr;
						if (SourceDocument->TryGetObjectField(TEXT("provider_extension"), Extension) && Extension && *Extension
							&& (*Extension)->TryGetObjectField(TEXT("function_settings"), Settings) && Settings && *Settings)
							UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, **Settings, InputField);
						SetFailure(OutFailure, Error); return false;
					}
					bool bPresent = false;
					for (const auto& DescriptorValue : Descriptors)
					{
						const auto Descriptor = DescriptorValue && DescriptorValue->Type == EJson::Object ? DescriptorValue->AsObject() : nullptr;
						FString Candidate;
						if (!Descriptor || !Descriptor->TryGetStringField(TEXT("property_path"), Candidate)
							|| !Candidate.Equals(Path, ESearchCase::IgnoreCase)) continue;
						bPresent = true;
						auto Other = UeAgentAuthoringPropertyValues::FPropertyValueStorage::CreateInitialized(Property, Error);
						if (!Other || !UeAgentPropertyValueCodec::Decode(Property, Descriptor->TryGetField(TEXT("value")),
							Other->GetMutableValuePtr(), Proposed.Get(), Codec, Error))
						{
							if (Error.PropertyPath.IsEmpty()) Error.PropertyPath = Candidate;
							UeAgentAuthoringDiagnostics::LocatePropertyValueSource(Error, *Descriptor);
							SetFailure(OutFailure, Error); return false;
						}
						const auto Difference = UeAgentPropertyValueCodec::ComparePropertyValues(
							Property, Alias->GetValuePtr(), Other->GetValuePtr(), Proposed.Get(), Codec);
						if (!Difference.bEquivalent)
					{
						if (Difference.IsComparable())
							Error = AssetFailure(TEXT("material_function_settings_property_conflict"),
								FString::Printf(TEXT("Function setting and property '%s' specify different values."), *Candidate), Candidate, nullptr, nullptr);
						Difference.WithPrefix(TEXT("/value")).ApplyTo(Error);
						if (Error.PropertyPath.IsEmpty()) Error.PropertyPath = Candidate;
						UeAgentAuthoringDiagnostics::LocatePropertyValueSource(Error, *Descriptor);
						SetFailure(OutFailure, Error); return false;
					}
					}
					if (!bPresent)
					{
						auto Descriptor = MakeShared<FJsonObject>();
						Descriptor->SetStringField(TEXT("property_path"), Path);
						Descriptor->SetField(TEXT("value"), UeAgentPropertyValueCodec::Encode(Property, Alias->GetValuePtr(), Proposed.Get(), Codec));
						Descriptors.Add(MakeShared<FJsonValueObject>(Descriptor));
					}
					return true;
				};
				return (!FunctionSettings.bHasDescription || Merge(TEXT("Description"), TEXT("description"), MakeShared<FJsonValueString>(FunctionSettings.Description)))
					&& (!FunctionSettings.bHasExposeToLibrary || Merge(TEXT("bExposeToLibrary"), TEXT("expose_to_library"), MakeShared<FJsonValueBoolean>(FunctionSettings.bExposeToLibrary)));
			}

			virtual bool CaptureRollback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation == EOperation::Update)
				{
					if (Graph.IsValid())
					{
						UE::UAI::Contract::FDiagnostic Error;
						if (!Graph->CaptureRollback(*Existing, Error))
						{
							SetFailure(OutFailure, Error);
							return false;
						}
					}
					return true;
				}
				if (Operation != EOperation::Remove || !Existing) return true;
				UE::UAI::Contract::FDiagnostic Error;
				if (!RemovalStage.Capture(Existing, Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}
				return true;
			}

			virtual bool ApplyProposed(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(&SourceDocument.Get(),
					[this](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(SourceLocations, Pointer); });
				if (auto* Instance = Cast<UMaterialInstanceConstant>(Proposed.Get()))
				{
					UE::UAI::Contract::FDiagnostic Error;
					if (!ApplyInstanceParameters(*Instance, InstanceEdits, Error)
						|| (InstanceLayers && !ApplyInstanceLayers(*Instance, InstanceLayers, Error)))
					{ if (Error.TargetPath.IsEmpty()) Error.TargetPath = GetTargetKey(); SetFailure(OutFailure, Error); return false; }
					return true;
				}
				if (Operation == EOperation::Remove) return true;
				if (Operation == EOperation::Update)
				{
					if (Graph.IsValid())
					{
						UE::UAI::Contract::FDiagnostic Error;
						if (!Graph->ApplyTo(*Existing, Error))
						{
							SetFailure(OutFailure, Error);
							return false;
						}
					}
				}
				return true;
			}

			virtual bool ApplyLifecycle(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(&SourceDocument.Get(),
					[this](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(SourceLocations, Pointer); });
				if (Operation == EOperation::Remove)
				{
					UE::UAI::Contract::FDiagnostic Error;
					if (!RemovalStage.Stage(Error))
					{
						SetFailure(OutFailure, Error);
						return false;
					}
					Actual = nullptr;
					return true;
				}
				if (Operation == EOperation::Update)
				{
					Actual = Existing;
					if (auto* Instance = Cast<UMaterialInstanceConstant>(Actual))
					{
						UE::UAI::Contract::FDiagnostic Error;
						if (!ApplyInstanceParameters(*Instance, InstanceEdits, Error)
							|| (InstanceLayers && !ApplyInstanceLayers(*Instance, InstanceLayers, Error)))
						{ if (Error.TargetPath.IsEmpty()) Error.TargetPath = GetTargetKey(); SetFailure(OutFailure, Error); return false; }
					}
					return true;
				}
				if (!Proposed.IsValid())
				{
					auto Error = AssetFailure(TEXT("material_proposed_asset_missing"),
						FString::Printf(TEXT("The prepared material asset for '%s' is no longer available for publication."), *PackagePath),
						TEXT("proposed_asset_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					Error.TargetPath = PackagePath; Error.SourceKind = TEXT("native_result");
					SetFailure(OutFailure, Error);
					return false;
				}
				UPackage* TargetPackage = CreatePackage(*PackagePath);
				Actual = DuplicatePublishedAsset(
					Proposed.Get(), TargetPackage, FPackageName::GetLongPackageAssetName(PackagePath));
				if (!Actual)
				{
					auto Error = AssetFailure(TEXT("material_asset_publish_failed"),
						FString::Printf(TEXT("UE could not publish the prepared %s asset at '%s'%s."), *AssetKind, *PackagePath,
							TargetPackage ? TEXT("") : TEXT(" because the destination package could not be created")),
						TEXT("asset_published"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					Error.TargetPath = PackagePath; Error.SourceKind = TEXT("native_result");
					SetFailure(OutFailure, Error);
					return false;
				}
				FAssetRegistryModule::AssetCreated(Actual);
				Actual->MarkPackageDirty();
				return true;
			}

			virtual bool Compile(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!bCompile || Operation == EOperation::Remove) return true;
				UE::UAI::Contract::FDiagnostic Error;
				if (!CompileAsset(Actual ? Actual : Existing, Error))
				{
					if (Error.TargetPath.IsEmpty()) Error.TargetPath = GetTargetKey();
					SetFailure(OutFailure, Error);
					return false;
				}
				return true;
			}

			virtual bool Save(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!bSave || Operation == EOperation::Remove) return true;
				UE::UAI::Contract::FDiagnostic Error;
				if (!SaveAssetPackage(Actual ? Actual : Existing, Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}
				return true;
			}

			virtual bool BuildDurablePackagePlans(
				TArray<FPackageDurablePlan>& OutPlans,
				FOperationFailure& OutFailure) const override
			{
				if (!BuildPackageDurablePlan(
					PackagePath,
					PackagePath,
					Operation == EOperation::Remove ? Existing
						: (Existing ? Existing : Proposed.Get()),
					Operation == EOperation::Remove ? EPackageDurableIntent::Delete
						: EPackageDurableIntent::Save,
					OutPlans,
					OutFailure)) return false;
				// Materials and functions can remain reachable through parent,
				// function-call and layer references. Native package replacement
				// reloads the package and repoints those existing referencers.
				if (Operation == EOperation::Update)
					OutPlans.Last().ReadbackPolicy = EPackageReadbackPolicy::ReplaceResidentPackage;
				return true;
			}

			virtual bool BuildDomainExpectedReadRequests(
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure) const override
			{
				return BuildReadRequests(
					Operation == EOperation::Remove ? nullptr : Proposed.Get(),
					Operation == EOperation::Remove,
					OutRequests,
					OutFailure);
			}

			virtual bool BuildDomainActualReadRequests(
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure) const override
			{
				return BuildReadRequests(
					Operation == EOperation::Remove ? nullptr : (Actual ? Actual : Existing),
					Operation == EOperation::Remove,
					OutRequests,
					OutFailure);
			}

			virtual bool Commit(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Graph.IsValid()) Graph->Commit();
				if (Operation == EOperation::Remove && RemovalStage.IsStaged())
				{
					UE::UAI::Contract::FDiagnostic Error;
					if (!DeleteAsset(RemovalStage.GetAsset(), PackagePath, Error))
					{
						SetFailure(OutFailure, Error);
						return false;
					}
					RemovalStage.Reset();
					Existing = nullptr;
				}
				return true;
			}

			virtual bool RequiresCheckpointRollback() const override { return AssetKind == TEXT("material_instance") || (Graph.IsValid() && Graph->PreservesExisting()); }
			virtual bool SupportsPersistentReadback() const override { return Operation == EOperation::Update; }
			virtual bool ReleaseForPersistentReadback(FOperationFailure& OutFailure) override
			{
				if (Graph.IsValid()) Graph->ReleaseForPersistentReadback();
				if (Proposed.IsValid())
				{
					// In-place graph updates use the resident asset as their proposed
					// state. Only an independently owned scratch copy may be retired.
					if (Proposed.Get() != Existing && Proposed.Get() != Actual)
					{
						Proposed->ClearFlags(RF_Public | RF_Standalone);
						Proposed->MarkAsGarbage();
					}
					Proposed.Reset();
				}
				Existing = nullptr;
				Actual = nullptr;
				return true;
			}
			virtual bool RebindAfterPersistentReadback(FOperationFailure& OutFailure) override
			{
				Existing = LoadAsset(PackagePath);
				Actual = Existing;
				if (!Existing)
				{
					auto Error = AssetFailure(TEXT("material_persistent_reload_failed"),
						FString::Printf(TEXT("Saved material asset '%s' could not be loaded for persistent readback."), *PackagePath),
						TEXT("asset_loaded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					Error.TargetPath = PackagePath; Error.SourceKind = TEXT("native_result");
					SetFailure(OutFailure, Error);
					return false;
				}
				return true;
			}
			virtual bool BuildDomainBeforeReadRequests(
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure) const override
			{
				UObject* Owner = Actual ? Actual : Existing;
				UE::UAI::Contract::FDiagnostic Error;
				if (!BuildDomainSnapshotRequest(Owner, false, OutRequests, Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}
				const int32 FirstProperty = OutRequests.Num();
				const bool bOwnsPublicObservation = HasMaterialOrFunctionObservation(OutRequests.Last());
				const FMaterialExpressionCollection* Collection = Owner ? GetExpressionCollection(*Owner) : nullptr;
				if (Cast<UMaterialInstanceConstant>(Owner)) return true;
				if (!Collection) {
					Error = AssetFailure(TEXT("material_before_expression_collection_missing"),
						TEXT("Cannot capture the previous graph properties because the material expression collection is unavailable."),
						TEXT("expression_collection_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					Error.SourceKind = TEXT("native_result"); Error.TargetPath = PackagePath;
					SetFailure(OutFailure, Error); return false;
				}
				for (const UMaterialExpression* Expression : Collection->Expressions)
				{
					if (Expression && !UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
						Expression, UeAgentAuthoringState::FEditablePropertyReadOptions(), OutRequests, Error))
					{
						SetFailure(OutFailure, Error);
						return false;
					}
				}
				if (bOwnsPublicObservation) MarkVerificationProperties(FirstProperty, OutRequests);
				return true;
			}

			virtual bool Rollback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				UE::UAI::Contract::FDiagnostic Error;
				if (Operation == EOperation::Update)
				{
					if (Graph.IsValid() && !Graph->Rollback(*Existing, Error))
					{
						SetFailure(OutFailure, Error);
						return false;
					}
					Actual = Existing;
					return true;
				}
				if (Operation == EOperation::Create || Operation == EOperation::Clone)
				{
					if (Actual && !DeleteAsset(Actual, PackagePath, Error))
					{
						TArray<UObject*> Objects = { Actual };
						if (ObjectTools::DeleteObjectsUnchecked(Objects) <= 0)
						{
							SetFailure(OutFailure, Error);
							return false;
						}
					}
					Actual = nullptr;
					return true;
				}
				if (Operation == EOperation::Remove && RemovalStage.IsCaptured())
				{
					if (!RemovalStage.Rollback(Error))
					{
						SetFailure(OutFailure, Error);
						return false;
					}
					Actual = Existing;
					return true;
				}
				return true;
			}

			int32 GetExpressionCount() const { return Graph.IsValid() ? Graph->GetExpressionCount() : 0; }
			int32 GetEdgeCount() const { return Graph.IsValid() ? Graph->GetEdgeCount() : 0; }
			int32 GetRootInputCount() const { return Graph.IsValid() ? Graph->GetRootInputCount() : 0; }

		private:
			static bool HasMaterialOrFunctionObservation(const UeAgentAuthoringState::FReadRequest& Request)
			{
				return Request.Context.Contains(ObservedGraphName) || Request.Context.Contains(ObservedFunctionRoot);
			}
			static void MarkVerificationProperties(int32 First, TArray<UeAgentAuthoringState::FReadRequest>& Requests)
			{
				for (int32 Index = First; Index < Requests.Num(); ++Index) Requests[Index].MarkVerificationOnly();
			}
			bool BuildDomainSnapshotRequest(UObject* Root, bool bExpectedAbsent, TArray<UeAgentAuthoringState::FReadRequest>& Requests,
				UE::UAI::Contract::FDiagnostic& Error) const
			{
				if (!BuildAssetSnapshotReadRequest(Root, PackagePath, AssetKind, bExpectedAbsent, Requests, Error)) return false;
				// The prepared graph owns its expression and connection observations.
				// Authored root fields have separate read requests in the same capture.
				if (Operation == EOperation::Update && Root && !bExpectedAbsent && Graph && Graph->IsReady()
					&& !InstanceLayers && InstanceEdits.IsEmpty()
					&& (AssetKind == TEXT("material") || AssetKind == TEXT("material_function")))
				{
					Requests.Last().Context.Add(ObservedGraphName, GetAuthoringGraphName(AssetKind == TEXT("material_function")));
					return true;
				}
				if (Operation == EOperation::Update && !bExpectedAbsent && Cast<UMaterialFunction>(Root)
					&& !Graph && !InstanceLayers && InstanceEdits.IsEmpty())
				{
					Requests.Last().Context.Add(ObservedFunctionRoot, AssetKind);
					return true;
				}
				if (Operation != EOperation::Update || bExpectedAbsent || !Cast<UMaterialInstanceConstant>(Root) || Graph) return true;
				// Parent and layer edits can replace every inherited parameter, including the last one.
				return BindObservedInstanceParameters(InstanceEdits, bEditsInstanceParent || InstanceLayers.IsValid(), Requests.Last(), Error);
			}

			bool BuildReadRequests(
				UObject* Root,
				const bool bExpectedAbsent,
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure) const
			{
				OutFailure = FOperationFailure();
				UE::UAI::Contract::FDiagnostic Error;
				if (!bExpectedAbsent) {
					UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(&SourceDocument.Get(),
						[this](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(SourceLocations, Pointer); });
					if (auto* Instance = Cast<UMaterialInstanceConstant>(Root);
						Instance && !VerifyInstanceParameters(*Instance, InstanceEdits, Error)) {
						SetFailure(OutFailure, Error); return false;
					}
				}
				if (!BuildDomainSnapshotRequest(Root, bExpectedAbsent, OutRequests, Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}
				const int32 FirstProperty = OutRequests.Num();
				const bool bOwnsPublicObservation = HasMaterialOrFunctionObservation(OutRequests.Last());
				if (!bExpectedAbsent && Root && Graph.IsValid()
					&& !Graph->AppendPropertyReadRequests(*Root, PackagePath, OutRequests, OutFailure))
				{
					return false;
				}
				if (bOwnsPublicObservation) MarkVerificationProperties(FirstProperty, OutRequests);
				return true;
			}

			FString PackagePath;
			TSharedRef<FJsonObject> SourceDocument = MakeShared<FJsonObject>();
			TMap<FString, UE::UAI::Contract::FSourceLocation> SourceLocations;
			FString AssetKind;
			EOperation Operation;
			UObject* Existing = nullptr;
			TStrongObjectPtr<UObject> Proposed;
			FAssetRemovalStage RemovalStage;
			UObject* Actual = nullptr;
			TUniquePtr<FPreparedGraph> Graph;
			TArray<FInstanceParameterEdit> InstanceEdits;
			TSharedPtr<FJsonValue> InstanceLayers;
			FFunctionSettings FunctionSettings;
			bool bCompile = true;
			bool bSave = false;
			bool bEditsInstanceParent = false;
		};

		class FAssetLifecycle final : public IReflectionOperationLifecycle
		{
			FGraphApplyContext Context;
		public:
			explicit FAssetLifecycle(const FGraphApplyContext& InContext) : Context(InContext) {}
			virtual bool PrepareLifecycle(
				const FPrepareRequest& Request,
				TUniquePtr<IReflectionOperationLifecycleState>& OutState,
				FOperationFailure& OutFailure) const override
			{
				OutState.Reset();
				OutFailure = FOperationFailure();
				if (!Request.IsValid() || !Request.Input->Target.IsValid())
				{
					SetFailure(OutFailure, AssetFailure(TEXT("material_apply_request_invalid"),
						!Request.IsValid() ? TEXT("The material operation request is incomplete.") : TEXT("The material operation requires a target object."),
						!Request.IsValid() ? TEXT("request_valid") : TEXT("target_present"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
					return false;
				}

				FString PackagePath;
				Request.Input->Target->TryGetStringField(TEXT("asset_path"), PackagePath);
				PackagePath =
					UeAgentAuthoringAssetIdentity::NormalizePackagePath(PackagePath);
				if (!FPackageName::IsValidLongPackageName(PackagePath)
					|| FPackageName::GetLongPackageAssetName(PackagePath).IsEmpty()
					|| (!PackagePath.StartsWith(TEXT("/Game/")) && !PackagePath.StartsWith(TEXT("/UeAgentInterface/Materials/"))))
				{
					SetFailure(OutFailure, AssetFailure(TEXT("invalid_asset_path"),
						TEXT("Material asset_path must name a valid asset package under /Game or /UeAgentInterface/Materials."),
						TEXT("asset_path"), Request.Input->Target->TryGetField(TEXT("asset_path")),
						MakeShared<FJsonValueString>(TEXT("/Game/<folder>/<asset> or /UeAgentInterface/Materials/<asset>")), Request.Input->Target.Get()));
					return false;
				}

				UObject* Existing = LoadAsset(PackagePath);
				EOperation Operation = Request.Input->RequestedOperation;
				if (Operation == EOperation::Upsert)
				{
					Operation = Existing ? EOperation::Update : EOperation::Create;
				}
				if ((Operation == EOperation::Create || Operation == EOperation::Clone) && Existing)
				{
					SetFailure(OutFailure, AssetFailure(TEXT("asset_already_exists"),
						FString::Printf(TEXT("Cannot %s material asset '%s': the destination already exists."), Operation == EOperation::Clone ? TEXT("clone") : TEXT("create"), *PackagePath),
						TEXT("asset_path"), MakeShared<FJsonValueString>(PackagePath),
						MakeShared<FJsonValueString>(TEXT("unused destination asset path")), Request.Input->Target.Get()));
					return false;
				}
				if ((Operation == EOperation::Update || Operation == EOperation::Remove) && !Existing)
				{
					SetFailure(OutFailure, AssetFailure(TEXT("asset_not_found"),
						FString::Printf(TEXT("Cannot %s material asset '%s': it could not be loaded."), Operation == EOperation::Remove ? TEXT("remove") : TEXT("update"), *PackagePath),
						TEXT("asset_path"), MakeShared<FJsonValueString>(PackagePath),
						MakeShared<FJsonValueString>(TEXT("existing loadable material asset")), Request.Input->Target.Get()));
					return false;
				}

				FString AssetKind;
				Request.Input->Target->TryGetStringField(TEXT("asset_kind"), AssetKind);
				AssetKind.TrimStartAndEndInline();
				AssetKind.ToLowerInline();
				UObject* Source = nullptr;
				if (Operation == EOperation::Clone)
				{
					FString SourcePath;
					Request.Input->Target->TryGetStringField(TEXT("source_asset_path"), SourcePath);
					Source = LoadAsset(SourcePath);
					if (!Source)
					{
						SetFailure(OutFailure, AssetFailure(TEXT("source_asset_not_found"),
							FString::Printf(TEXT("Material clone source '%s' could not be loaded."), *SourcePath),
							TEXT("source_asset_path"), Request.Input->Target->TryGetField(TEXT("source_asset_path")),
							MakeShared<FJsonValueString>(TEXT("existing loadable material asset")), Request.Input->Target.Get()));
						return false;
					}
				}
				const FString InferredKind = GetAssetKind(Existing ? Existing : Source);
				if (AssetKind.IsEmpty()) AssetKind = InferredKind;
				if (!InferredKind.IsEmpty() && AssetKind != InferredKind)
				{
					SetFailure(OutFailure, AssetFailure(TEXT("material_asset_kind_conflict"),
						TEXT("The requested asset kind differs from the existing target or clone source."),
						TEXT("asset_kind"), MakeShared<FJsonValueString>(AssetKind), MakeShared<FJsonValueString>(InferredKind), Request.Input->Target.Get()));
					return false;
				}
				if (AssetKind != TEXT("material") && AssetKind != TEXT("material_function") && AssetKind != TEXT("material_instance"))
				{
					SetFailure(OutFailure, AssetFailure(TEXT("material_asset_kind_invalid"),
						TEXT("Material authoring requires asset_kind material, material_function, or material_instance."),
						TEXT("asset_kind"), MakeShared<FJsonValueString>(AssetKind),
						MakeShared<FJsonValueString>(TEXT("material, material_function, material_instance")), Request.Input->Target.Get()));
					return false;
				}
				if (AssetKind == TEXT("material_instance") && Operation != EOperation::Update)
				{
					SetFailure(OutFailure, AssetFailure(TEXT("material_instance_authoring_update_required"),
						TEXT("Material instance authoring supports only updating an existing instance."), TEXT("operation"),
						MakeShared<FJsonValueString>(Operation == EOperation::Create ? TEXT("create") : Operation == EOperation::Clone ? TEXT("clone") : TEXT("remove")),
						MakeShared<FJsonValueString>(TEXT("update"))));
					return false;
				}

				FFunctionSettings FunctionSettings;
				UE::UAI::Contract::FDiagnostic Error;
				TArray<FInstanceParameterEdit> InstanceEdits;
				const TArray<TSharedPtr<FJsonValue>>* Parameters = nullptr;
				if (Request.Input->ProviderExtension.IsValid() && Request.Input->ProviderExtension->TryGetArrayField(TEXT("instance_parameters"), Parameters))
				{
					auto* Instance = Cast<UMaterialInstanceConstant>(Existing);
					if (!Instance || !Parameters) {
						SetFailure(OutFailure, AssetFailure(TEXT("material_instance_parameters_invalid"),
							!Instance ? TEXT("instance_parameters requires an existing MaterialInstanceConstant target.") : TEXT("instance_parameters requires an array."),
							TEXT("instance_parameters"), MakeShared<FJsonValueString>(AssetKind),
							MakeShared<FJsonValueString>(!Instance ? TEXT("MaterialInstanceConstant") : TEXT("array")), Request.Input->ProviderExtension.Get()));
						return false;
					}
					if (!PrepareInstanceParameters(*Instance, *Parameters, InstanceEdits, Error))
					{ SetFailure(OutFailure, Error); return false; }
				}
				if (!ParseFunctionSettings(Request.Input->ProviderExtension, FunctionSettings, Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}
				if (FunctionSettings.HasAny() && AssetKind != TEXT("material_function"))
				{
					SetFailure(OutFailure, AssetFailure(TEXT("material_function_settings_target_conflict"),
						TEXT("function_settings can be applied only to a material_function asset."),
						TEXT("function_settings"), MakeShared<FJsonValueString>(AssetKind),
						MakeShared<FJsonValueString>(TEXT("material_function")), Request.Input->ProviderExtension.Get()));
					return false;
				}

				UObject* Proposed = Existing;
				UPackage* ScratchPackage = nullptr;
				if (Operation != EOperation::Remove)
				{
					ScratchPackage = CreatePackage(*FString::Printf(
						TEXT("/Temp/UAI/Material/Proposed_%s"),
						*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
					if (Operation == EOperation::Create)
					{
						Proposed = CreateFactorySeed(
							AssetKind, ScratchPackage, NAME_None, RF_Transactional, Error);
					}
					else if (Operation == EOperation::Clone)
					{
						Proposed = StaticDuplicateObject(Source, ScratchPackage, NAME_None, RF_Transactional);
					}
					else if (AssetKind == TEXT("material_instance"))
					{
						Proposed = StaticDuplicateObject(Existing, ScratchPackage, NAME_None, RF_Transactional);
					}
					if (!Proposed)
					{
						if (Error.IsEmpty()) {
							Error = AssetFailure(TEXT("material_proposed_asset_create_failed"),
								FString::Printf(TEXT("UE could not prepare a %s asset for '%s'%s."), *AssetKind, *PackagePath,
									ScratchPackage ? TEXT("") : TEXT(" because the temporary package could not be created")),
								TEXT("proposed_asset_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
							Error.SourceKind = TEXT("native_result");
						}
						Error.TargetPath = PackagePath;
						SetFailure(OutFailure, Error);
						return false;
					}
				}

				TUniquePtr<FPreparedGraph> Graph;
				const TSharedPtr<FJsonObject>* GraphDocument = nullptr;
				if (Request.Input->ProviderExtension.IsValid()
					&& Request.Input->ProviderExtension->TryGetObjectField(TEXT("graph"), GraphDocument))
				{
					if (!GraphDocument || !GraphDocument->IsValid() || Operation == EOperation::Remove)
					{
						SetFailure(OutFailure, AssetFailure(TEXT("material_graph_extension_invalid"),
							Operation == EOperation::Remove ? TEXT("A remove operation cannot include a material graph edit.") : TEXT("Material graph extension must be an object."),
							TEXT("graph"), Request.Input->ProviderExtension->TryGetField(TEXT("graph")),
							MakeShared<FJsonValueString>(Operation == EOperation::Remove ? TEXT("no graph edit") : TEXT("object")), Request.Input->ProviderExtension.Get()));
						return false;
					}
					UObject* ValidationOwner = Proposed;
					TStrongObjectPtr<UObject> ValidationSeed;
					if (Operation == EOperation::Update)
					{
						ValidationSeed.Reset(Context.Policy == EGraphApplyPolicy::PreserveExisting
							? StaticDuplicateObject(Existing, ScratchPackage, NAME_None, RF_Transactional)
							: CreateFactorySeed(AssetKind, ScratchPackage, NAME_None, RF_Transactional, Error));
						ValidationOwner = ValidationSeed.Get();
					}
					Graph = MakeUnique<FPreparedGraph>();
					if (!ValidationOwner || !Graph->Prepare(
						*ValidationOwner,
						*GraphDocument,
						PackagePath,
						GetAuthoringGraphName(AssetKind == TEXT("material_function")),
						Error, Context))
					{
						if (!ValidationOwner && Error.IsEmpty()) {
							Error = AssetFailure(TEXT("material_graph_preflight_failed"),
								FString::Printf(TEXT("UE could not create the validation owner for material graph '%s'."), *PackagePath),
								TEXT("validation_owner_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
							Error.SourceKind = TEXT("native_result");
						}
						SetFailure(OutFailure, Error);
						return false;
					}
				}

				const bool bEditsInstanceParent = Cast<UMaterialInstanceConstant>(Existing) && Request.Input->Properties.ContainsByPredicate(
					[](const TSharedPtr<FJsonValue>& Value) {
						FString Path;
						return Value && Value->Type == EJson::Object && Value->AsObject()->TryGetStringField(TEXT("property_path"), Path)
							&& FName(*Path) == GET_MEMBER_NAME_CHECKED(UMaterialInstance, Parent);
					});
				OutState = MakeUnique<FAssetLifecycleState>(
					PackagePath,
					AssetKind,
					Operation,
					Existing,
					Proposed,
					MoveTemp(Graph),
					MoveTemp(InstanceEdits),
					Request.Input->ProviderExtension.IsValid() ? Request.Input->ProviderExtension->TryGetField(TEXT("instance_layers")) : nullptr,
					MoveTemp(FunctionSettings),
					ReadBool(Request.Input->ProviderExtension, TEXT("compile"), true),
					ReadBool(Request.Input->ProviderExtension, TEXT("save"), false),
					bEditsInstanceParent, Request.Input->Target, Request.Input->ProviderExtension);
				return true;
			}
		};

		FInputFieldDescriptor Field(
			const TCHAR* Name,
			const TCHAR* Role,
			const TCHAR* TypeId,
			const TArray<EOperation>& RequiredFor = {})
		{
			FInputFieldDescriptor Result;
			Result.Name = Name;
			Result.Role = Role;
			Result.TypeId = TypeId;
			Result.RequiredFor = RequiredFor;
			return Result;
		}

		FOperationDescriptor OperationDescriptor(const EOperation Operation)
		{
			FOperationDescriptor Result;
			Result.Operation = Operation;
			Result.IdentityOneOf = { { TEXT("asset_path") } };
			Result.AcceptedSections = { TEXT("target"), TEXT("provider_extension") };
			if (Operation != EOperation::Remove) Result.AcceptedSections.Add(TEXT("properties"));
			if (Operation == EOperation::Create)
			{
				Result.OmittedFieldPolicy = TEXT("ue_native_default");
				Result.NativeDefaultPolicy = TEXT("required");
			}
			else if (Operation == EOperation::Update)
			{
				Result.OmittedFieldPolicy = TEXT("preserve_current");
				Result.NativeDefaultPolicy = TEXT("forbidden");
			}
			else if (Operation == EOperation::Clone || Operation == EOperation::Upsert)
			{
				Result.OmittedFieldPolicy = TEXT("ue_native_default");
				Result.NativeDefaultPolicy = TEXT("allowed");
			}
			else
			{
				Result.OmittedFieldPolicy = TEXT("not_applicable");
				Result.NativeDefaultPolicy = TEXT("not_applicable");
			}
			Result.bDestructive = Operation == EOperation::Remove;
			return Result;
		}

		FContract MakeInputContract()
		{
			FContract Result;
			Result.Schema = TEXT("uai.apply_input_contract.v1");
			Result.ContractId = AssetInputContractId;
			Result.ContractVersion = TEXT("1.0.0");
			Result.ProfileId = TEXT("material.asset");
			Result.TargetKind = TEXT("material_asset");
			Result.SemanticDescriptorRefs = { MaterialSemanticId, MaterialFunctionSemanticId };
			Result.ReflectionBaseline.bEnabled = true;
			Result.ReflectionBaseline.RootKinds = { TEXT("uobject"), TEXT("ustruct") };
			Result.ReflectionBaseline.RecursiveNodes = {
				TEXT("scalar"), TEXT("struct"), TEXT("owned_object"), TEXT("reference"),
				TEXT("array"), TEXT("set"), TEXT("map"), TEXT("fixed_array"), TEXT("optional")
			};
			Result.ReflectionBaseline.UnknownFieldPolicy = TEXT("reject");
			Result.ReflectionBaseline.ExternalReferencePolicy = TEXT("replace_reference_only");
			Result.ReflectionBaseline.OwnedObjectPolicy = TEXT("create_or_locate_with_real_outer_then_recurse");
			Result.PropertyPayload.Mode = TEXT("reflection");
			Result.PropertyPayload.PropertyPathField = TEXT("property_path");
			Result.PropertyPayload.ValueField = TEXT("value");
			Result.PropertyPayload.OptionalAssertionFields = {
				TEXT("kind"), TEXT("struct_path"), TEXT("enum_path"), TEXT("element_type"),
				TEXT("key_type"), TEXT("mapped_type"), TEXT("inner_type"), TEXT("fixed_length"), TEXT("allowed_class")
			};
			Result.PropertyPayload.OmittedPatchFieldPolicy = TEXT("preserve_current");
			const TArray<EOperation> All = {
				EOperation::Create, EOperation::Update, EOperation::Upsert, EOperation::Remove, EOperation::Clone
			};
			Result.InputFields = {
				Field(TEXT("asset_path"), TEXT("identity"), TEXT("ue_asset_path"), All),
				Field(TEXT("asset_kind"), TEXT("type_assertion"), TEXT("material_asset_kind"), { EOperation::Create }),
				Field(TEXT("source_asset_path"), TEXT("selector"), TEXT("ue_asset_path"), { EOperation::Clone }),
				Field(TEXT("operation"), TEXT("operation_control"), TEXT("apply_operation"), All),
				Field(TEXT("properties"), TEXT("intent"), TEXT("property_descriptor_list")),
				Field(TEXT("graph"), TEXT("provider_extension"), TEXT("material_graph")),
				Field(TEXT("function_settings"), TEXT("provider_extension"), TEXT("object")),
				Field(TEXT("instance_parameters"), TEXT("provider_extension"), TEXT("array")),
				Field(TEXT("instance_layers"), TEXT("provider_extension"), TEXT("object")),
				Field(TEXT("compile"), TEXT("provider_extension"), TEXT("bool")),
				Field(TEXT("save"), TEXT("provider_extension"), TEXT("bool")),
			};
			for (const EOperation Item : All) Result.Operations.Add(OperationDescriptor(Item));
			Result.ProviderId = ApplyProviderId;
			Result.UnknownInputFieldPolicy = TEXT("reject");
			Result.Verification.StateSource = TEXT("shared_state_snapshot");
			Result.Verification.CompareSource = TEXT("shared_semantic_compare");
			Result.Verification.bRequired = true;
			return Result;
		}

		UeAgentApplyWorkspaceContract::FContract MakeWorkspaceContract()
		{
			using namespace UeAgentApplyWorkspaceContract;
			UeAgentApplyWorkspaceContract::FContract Result;
			Result.Schema = TEXT("uai.apply_workspace_contract.v1");
			Result.ContractId = AssetWorkspaceContractId;
			Result.ContractVersion = TEXT("1.0.0");
			Result.ProfileId = TEXT("material.asset");
			Result.Sources = {
				{ ESourceKind::Directory, TEXT("physical"), TEXT("index") },
				{ ESourceKind::SingleFile, TEXT("virtual"), TEXT("object") },
				{ ESourceKind::Inline, TEXT("virtual"), TEXT("object") },
			};
			Result.Entry.DirectoryEntry = TEXT("index.hjson");
			Result.Entry.VirtualUnitId = TEXT("asset");
			FUnitDescriptor Unit;
			Unit.UnitId = TEXT("asset");
			Unit.PathPattern = TEXT("index.hjson");
			Unit.Role = TEXT("object");
			Unit.Cardinality = TEXT("one");
			Unit.bRequired = true;
			Unit.RouteId = TEXT("material.asset");
			Unit.ApplyContractRef = AssetInputContractId;
			Unit.MergePolicy = TEXT("replace");
			Unit.AbsentSemantics = TEXT("error");
			Unit.EmptySemantics = TEXT("error");
			Result.Units = { Unit };
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
			Result.Transaction.RollbackOn = {
				TEXT("mutation_failed"), TEXT("compile_failed"), TEXT("save_failed"), TEXT("verification_failed")
			};
			Result.Execution.Compile = TEXT("provider_hook");
			Result.Execution.Save = TEXT("caller_controlled");
			Result.Execution.Verification = TEXT("shared_snapshot_required");
			Result.UnknownFilePolicy = TEXT("reject");
			Result.DuplicateTargetPolicy = TEXT("reject");
			return Result;
		}

		TSharedRef<FJsonObject> MakeApplyDocument(const FAssetApplyRequest& Request)
		{
			TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
			Target->SetStringField(TEXT("asset_path"), Request.AssetPath);
			if (!Request.AssetKind.IsEmpty()) Target->SetStringField(TEXT("asset_kind"), Request.AssetKind);
			if (!Request.SourceAssetPath.IsEmpty()) Target->SetStringField(TEXT("source_asset_path"), Request.SourceAssetPath);
			TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
			Extension->SetBoolField(TEXT("compile"), Request.bCompile);
			Extension->SetBoolField(TEXT("save"), Request.bSave);
			if (Request.Graph.IsValid()) Extension->SetObjectField(TEXT("graph"), Request.Graph);
			if (!Request.InstanceParameters.IsEmpty()) Extension->SetArrayField(TEXT("instance_parameters"), Request.InstanceParameters);
			if (Request.InstanceLayers) Extension->SetField(TEXT("instance_layers"), Request.InstanceLayers);
			if (Request.FunctionSettings.IsValid())
			{
				Extension->SetObjectField(TEXT("function_settings"), Request.FunctionSettings);
			}
			TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
			Document->SetStringField(TEXT("operation"), OperationToString(Request.Operation));
			Document->SetObjectField(TEXT("target"), Target);
			Document->SetObjectField(TEXT("provider_extension"), Extension);
			if (!Request.Properties.IsEmpty()) Document->SetArrayField(TEXT("properties"), Request.Properties);
			return Document;
		}

		int32 CountGraphEntries(const TSharedPtr<FJsonObject>& Graph, const TCHAR* Field)
		{
			if (!Graph.IsValid()) return 0;
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (Graph->TryGetArrayField(Field, Array) && Array) return Array->Num();
			const TSharedPtr<FJsonObject>* Map = nullptr;
			return Graph->TryGetObjectField(Field, Map) && Map && Map->IsValid()
				? (*Map)->Values.Num()
				: 0;
		}

		bool ExecuteAssetWorkspaceSource(
			const UeAgentAuthoringWorkspaceLoader::FSourceRequest& Source,
			const FString& ExpectedAssetKind,
			const FString& ExpectedAssetPath,
			FAssetApplyResult& OutResult,
			const FString& RequestId,
			TFunction<bool(FOperationFailure&)> FinalReadback = {},
			const FGraphApplyContext& Context = {},
			const TSharedPtr<IDurablePackageStager>& PackageStager = nullptr)
		{
			OutResult = FAssetApplyResult();
			OutResult.Status = TEXT("failed");
			UeAgentAuthoringAssetWorkspaceInput::FRequest ResolveRequest;
			ResolveRequest.WorkspaceContractId = AssetWorkspaceContractId;
			ResolveRequest.Source = Source;
			ResolveRequest.ExpectedAssetPath = ExpectedAssetPath;
			ResolveRequest.Errors.ContractMissing = TEXT("material_workspace_contract_missing");
			ResolveRequest.Errors.EntryInvalid = TEXT("material_workspace_entry_invalid");
			ResolveRequest.Errors.AssetKindConflict =
				TEXT("material_workspace_asset_kind_conflict");
			ResolveRequest.Errors.AssetPathConflict =
				TEXT("material_workspace_asset_path_conflict");
			UeAgentAuthoringAssetWorkspaceInput::FResult Resolved;
			if (!UeAgentAuthoringAssetWorkspaceInput::Resolve(ResolveRequest, Resolved))
			{
				OutResult.Phase = Resolved.Phase;
				OutResult.Diagnostic = Resolved.Diagnostic;
				return false;
			}
			const UeAgentApplyWorkspaceContract::FContract& WorkspaceContract =
				Resolved.WorkspaceContract;
			const UeAgentAuthoringWorkspaceLoader::FLoadResult& LoadResult = Resolved.Load;
			const UeAgentApplyInputContract::FOperationInput& OperationInput = Resolved.Input;
			FAssetApplyRequest Request;
			Request.Operation = OperationInput.RequestedOperation;
			Request.AssetPath = Resolved.AssetPath;
			Request.AssetKind = Resolved.AssetKind;
			Request.SourceAssetPath = Resolved.SourceAssetPath;
			FString NormalizedExpectedKind = ExpectedAssetKind.TrimStartAndEnd();
			NormalizedExpectedKind.ToLowerInline();
			UObject* ExistingBeforeApply = LoadAsset(Request.AssetPath);
			if (Request.AssetKind.IsEmpty()) Request.AssetKind = GetAssetKind(ExistingBeforeApply);
			if (!NormalizedExpectedKind.IsEmpty()
				&& !Request.AssetKind.Equals(NormalizedExpectedKind, ESearchCase::IgnoreCase))
			{
				OutResult.Phase = TEXT("preflight");
				OutResult.Diagnostic = AssetFailure(TEXT("material_workspace_asset_kind_conflict"),
					TEXT("The workspace asset kind does not match the kind required by this command."), TEXT("asset_kind"),
					MakeShared<FJsonValueString>(Request.AssetKind), MakeShared<FJsonValueString>(NormalizedExpectedKind), OperationInput.Target.Get());
				OutResult.Diagnostic.Stage = OutResult.Phase;
				OutResult.Diagnostic.TargetPath = Request.AssetPath;
				for (const auto& Unit : LoadResult.Workspace.Units) {
					if (Unit.CanonicalUnitKey != Resolved.CanonicalUnitKey || !Unit.Document) continue;
					UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(Unit.Document.Get(),
						[&Unit](const FString& Pointer) { return Unit.ResolveDiagnosticSource(Pointer); });
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutResult.Diagnostic, *OperationInput.Target, TEXT("asset_kind"));
					break;
				}
				return false;
			}
			Request.Properties = OperationInput.Properties;
			const TSharedPtr<const FJsonObject> ProviderExtension =
				OperationInput.ProviderExtension;
			if (ProviderExtension.IsValid())
			{
				ProviderExtension->TryGetBoolField(TEXT("compile"), Request.bCompile);
				ProviderExtension->TryGetBoolField(TEXT("save"), Request.bSave);
				const TSharedPtr<FJsonObject>* GraphObject = nullptr;
				if (ProviderExtension->TryGetObjectField(TEXT("graph"), GraphObject)
					&& GraphObject && GraphObject->IsValid())
				{
					Request.Graph = *GraphObject;
				}
				const TSharedPtr<FJsonObject>* FunctionSettings = nullptr;
				if (ProviderExtension->TryGetObjectField(
						TEXT("function_settings"), FunctionSettings)
					&& FunctionSettings && FunctionSettings->IsValid())
				{
					Request.FunctionSettings = *FunctionSettings;
				}
			}
			const bool bExisted = LoadAsset(Request.AssetPath) != nullptr;
			FBackendOptions BackendOptions;
			BackendOptions.FinalReadback = MoveTemp(FinalReadback);
			if (PackageStager.IsValid()) BackendOptions.PackageStager = PackageStager;
			BackendOptions.bUseEditorTransaction = Request.Operation != EOperation::Remove;
			BackendOptions.TransactionDescription = NSLOCTEXT(
				"UeAgentInterface", "MaterialAssetWorkspaceApply", "Apply Material asset workspace");
			FApplyWorkspaceResult ApplyResult;
			UeAgentAuthoringAssetWorkspaceApply::FFailure ApplyFailure;
			FProviderRegistry LocalProviders;
			const FProviderRegistry* Providers = &FProviderRegistry::Get();
			if (Context.Policy == EGraphApplyPolicy::PreserveExisting)
			{
				UeAgentAuthoringOperationApply::ERegistrationResult Registered;
				UE::UAI::Contract::FDiagnostic RegistrationError;
				if (!LocalProviders.RegisterProvider(MakeApplyProvider(Context), Registered, RegistrationError))
				{ OutResult.Phase = TEXT("preflight"); OutResult.Diagnostic = RegistrationError; return false; }
				Providers = &LocalProviders;
			}
			const bool bApplied = UeAgentAuthoringAssetWorkspaceApply::Execute(
				WorkspaceContract,
				LoadResult.Workspace,
				*Providers,
				RequestId,
				Request.bSave,
				BackendOptions,
				ApplyResult,
				ApplyFailure);
			OutResult.UnitsChecked = ApplyResult.Preflight.UnitsChecked;
			OutResult.MutationsApplied = ApplyResult.Execution.MutationsApplied;
			OutResult.bRollbackAttempted = ApplyResult.Execution.bRollbackAttempted;
			OutResult.bRollbackSucceeded = ApplyResult.Execution.bRollbackSucceeded;
			if (!bApplied)
			{
				OutResult.Status = ApplyResult.Execution.Status;
				OutResult.Phase = ApplyFailure.Phase;
				OutResult.Diagnostic = ApplyFailure.Diagnostic;
				if (OutResult.bRollbackAttempted && !OutResult.bRollbackSucceeded)
					{
        const auto Cause = OutResult.Diagnostic;
        OutResult.Diagnostic.Code = TEXT("material_import_state_unknown");
        OutResult.Diagnostic.PublicMessage = TEXT("Rollback did not restore a confirmed asset state.");
        OutResult.Diagnostic.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(Cause);
      }
				return false;
			}

			OutResult.Status = TEXT("applied");
			OutResult.Phase = TEXT("complete");
			const EOperation ResolvedOperation = Request.Operation == EOperation::Upsert
				? (bExisted ? EOperation::Update : EOperation::Create)
				: Request.Operation;
			OutResult.ResolvedOperation = OperationToString(ResolvedOperation);
			OutResult.AssetPath =
				UeAgentAuthoringAssetIdentity::NormalizePackagePath(
					Request.AssetPath);
			OutResult.Asset = ResolvedOperation == EOperation::Remove ? nullptr : LoadAsset(OutResult.AssetPath);
			OutResult.ObjectPath = OutResult.Asset
				? OutResult.Asset->GetPathName()
				: UeAgentAuthoringAssetIdentity::ToObjectPath(
					OutResult.AssetPath);
			OutResult.AssetKind = OutResult.Asset ? GetAssetKind(OutResult.Asset) : Request.AssetKind;
			if (Request.Graph.IsValid())
			{
				OutResult.ExpressionsCreated = CountGraphEntries(Request.Graph, TEXT("expressions"));
				OutResult.EdgesCreated = CountGraphEntries(Request.Graph, TEXT("edges"));
				OutResult.RootInputsConnected = CountGraphEntries(Request.Graph, TEXT("root_inputs"));
			}
			return true;
		}
	}

	bool RegisterApplyContracts(UE::UAI::Contract::FDiagnostic& OutError)
	{
		UeAgentApplyInputContract::ERegistrationResult InputResult;
		if (!UeAgentApplyInputContract::FRegistry::Get().RegisterContract(
			MakeInputContract(), InputResult, OutError)) return false;
		UeAgentApplyWorkspaceContract::ERegistrationResult WorkspaceResult;
		return UeAgentApplyWorkspaceContract::FRegistry::Get().RegisterContract(
			MakeWorkspaceContract(), WorkspaceResult, OutError);
	}

	TSharedRef<IApplyProvider> MakeApplyProvider(const FGraphApplyContext& Context)
	{
		const TSharedRef<IReflectionOperationLifecycle> Lifecycle = MakeShared<FAssetLifecycle>(Context);
		UeAgentAuthoringObjectApply::FOptions Options;
		Options.bAllowDomainAuthorizedOwners = true;
		Options.bCompactValues = Context.Policy == EGraphApplyPolicy::PreserveExisting;
		return MakeShared<FReflectionApplyProvider>(ApplyProviderId, Lifecycle, Options);
	}

	bool ApplyAsset(const FAssetApplyRequest& Request, FAssetApplyResult& OutResult, const FString& RequestId)
	{
		return ExecuteAssetWorkspaceSource(
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
				MakeApplyDocument(Request), TEXT("inline://material-asset-apply")),
			Request.AssetKind,
			Request.AssetPath,
			OutResult,
			RequestId, Request.FinalReadback, Request.GraphContext, Request.PackageStager);
	}

	bool ApplyAssetWorkspace(
		const FString& WorkspaceRoot,
		const FString& ExpectedAssetKind,
		const FString& ExpectedAssetPath,
		FAssetApplyResult& OutResult,
		const FString& RequestId)
	{
		return ExecuteAssetWorkspaceSource(
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeDirectory(WorkspaceRoot),
			ExpectedAssetKind,
			ExpectedAssetPath,
			OutResult,
			RequestId);
	}
}
