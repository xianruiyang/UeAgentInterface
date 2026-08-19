// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Material/Authoring/UeAgentMaterialAuthoringApplyProvider.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Commands/Material/UeAgentHttpServer_MaterialOps.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringDescriptors.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringState.h"
#include "Domains/Material/Authoring/UeAgentMaterialGraphAuthoring.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRemovalStage.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace UeAgentMaterialAuthoring
{
	namespace
	{
		using namespace UeAgentApplyInputContract;
		using namespace UeAgentAuthoringOperationApply;

		void SetFailure(FOperationFailure& OutFailure, const FString& Code, const FString& Detail = FString())
		{
			OutFailure = FOperationFailure();
			OutFailure.ErrorCode = Code;
			OutFailure.ErrorDetail = Detail.IsEmpty() ? Code : Detail;
		}

		FString NormalizePackagePath(const FString& InPath)
		{
			FString Path = InPath.TrimStartAndEnd();
			if (Path.Contains(TEXT("."))) Path = FPackageName::ObjectPathToPackageName(Path);
			return Path;
		}

		FString ToObjectPath(const FString& PackagePath)
		{
			const FString Name = FPackageName::GetLongPackageAssetName(PackagePath);
			return Name.IsEmpty() ? PackagePath : FString::Printf(TEXT("%s.%s"), *PackagePath, *Name);
		}

		UObject* LoadAsset(const FString& InPath)
		{
			const FString PackagePath = NormalizePackagePath(InPath);
			if (PackagePath.IsEmpty()) return nullptr;
			UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *ToObjectPath(PackagePath));
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

		bool DeleteAsset(UObject* Asset, FString& OutError)
		{
			OutError.Reset();
			if (!Asset)
			{
				OutError = TEXT("material_delete_target_missing");
				return false;
			}
			TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
			if (!ObjectTools::DeleteSingleObject(Asset, true))
			{
				OutError = TEXT("material_delete_blocked_by_references");
				return false;
			}
			return true;
		}

		bool CompileAsset(UObject* Asset, FString& OutError)
		{
			OutError.Reset();
			if (UMaterial* Material = Cast<UMaterial>(Asset))
			{
				UMaterialEditingLibrary::RecompileMaterial(Material);
				TArray<TSharedPtr<FJsonValue>> Messages;
				int32 ErrorCount = 0;
				UeAgentMaterialOps::CollectMaterialCompileMessages(
					Material,
					UeAgentMaterialOps::ECompileMessageSeverityFilter::Error,
					64,
					Messages,
					ErrorCount);
				if (ErrorCount > 0)
				{
					OutError = FString::Printf(TEXT("material_compile_failed:%d"), ErrorCount);
					return false;
				}
				return true;
			}
			if (UMaterialFunction* Function = Cast<UMaterialFunction>(Asset))
			{
				UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
				return true;
			}
			OutError = TEXT("material_compile_target_invalid");
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
			FString& OutError)
		{
			OutSettings = FFunctionSettings();
			OutError.Reset();
			const TSharedPtr<FJsonObject>* Settings = nullptr;
			if (!Extension.IsValid() || !Extension->TryGetObjectField(TEXT("function_settings"), Settings)) return true;
			if (!Settings || !Settings->IsValid())
			{
				OutError = TEXT("material_function_settings_invalid");
				return false;
			}
			if ((*Settings)->HasField(TEXT("description")))
			{
				OutSettings.bHasDescription = (*Settings)->TryGetStringField(
					TEXT("description"), OutSettings.Description);
				if (!OutSettings.bHasDescription)
				{
					OutError = TEXT("material_function_description_invalid");
					return false;
				}
			}
			if ((*Settings)->HasField(TEXT("expose_to_library")))
			{
				OutSettings.bHasExposeToLibrary = (*Settings)->TryGetBoolField(
					TEXT("expose_to_library"), OutSettings.bExposeToLibrary);
				if (!OutSettings.bHasExposeToLibrary)
				{
					OutError = TEXT("material_function_expose_to_library_invalid");
					return false;
				}
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Settings)->Values)
			{
				if (Field.Key != TEXT("description") && Field.Key != TEXT("expose_to_library"))
				{
					OutError = TEXT("material_function_settings_unknown_field:") + Field.Key;
					return false;
				}
			}
			return true;
		}

		bool HasPropertyPath(
			const TArray<TSharedPtr<FJsonValue>>& Properties,
			const FString& PropertyPath)
		{
			for (const TSharedPtr<FJsonValue>& Value : Properties)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				FString Candidate;
				if (Value.IsValid() && Value->TryGetObject(Object) && Object && Object->IsValid()
					&& (*Object)->TryGetStringField(TEXT("property_path"), Candidate)
					&& Candidate.Equals(PropertyPath, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
			return false;
		}

		void ApplyFunctionSettings(UMaterialFunction& Function, const FFunctionSettings& Settings)
		{
			Function.Modify();
			if (Settings.bHasDescription) Function.Description = Settings.Description;
			if (Settings.bHasExposeToLibrary) Function.bExposeToLibrary = Settings.bExposeToLibrary;
			Function.MarkPackageDirty();
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
				FFunctionSettings InFunctionSettings,
				bool bInCompile,
				bool bInSave)
				: PackagePath(MoveTemp(InPackagePath))
				, AssetKind(MoveTemp(InAssetKind))
				, Operation(InOperation)
				, Existing(InExisting)
				, Proposed(InProposed)
				, Graph(MoveTemp(InGraph))
				, FunctionSettings(MoveTemp(InFunctionSettings))
				, bCompile(bInCompile)
				, bSave(bInSave)
			{
			}

			virtual FString GetTargetKey() const override { return PackagePath; }
			virtual EOperation GetResolvedOperation() const override { return Operation; }
			virtual FReflectionRoot GetProposedRoot() const override
			{
				return FReflectionRoot::MakeObject(Proposed.Get());
			}
			virtual FReflectionRoot GetActualRoot() const override
			{
				return FReflectionRoot::MakeObject(Actual ? Actual : Existing);
			}

			virtual bool CaptureRollback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation == EOperation::Update)
				{
					if (UMaterialFunction* Function = Cast<UMaterialFunction>(Existing))
					{
						OldDescription = Function->Description;
						bOldExposeToLibrary = Function->bExposeToLibrary;
					}
					if (Graph.IsValid())
					{
						FString Error;
						if (!Graph->CaptureRollback(*Existing, Error))
						{
							SetFailure(OutFailure, Error);
							return false;
						}
					}
					return true;
				}
				if (Operation != EOperation::Remove || !Existing) return true;
				FString Error;
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
				if (Operation == EOperation::Remove) return true;
				if (Operation == EOperation::Update)
				{
					if (Graph.IsValid())
					{
						FString Error;
						if (!Graph->ApplyTo(*Existing, Error))
						{
							SetFailure(OutFailure, Error);
							return false;
						}
					}
					if (UMaterialFunction* Function = Cast<UMaterialFunction>(Existing))
					{
						ApplyFunctionSettings(*Function, FunctionSettings);
					}
				}
				return true;
			}

			virtual bool ApplyLifecycle(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation == EOperation::Remove)
				{
					FString Error;
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
					return true;
				}
				if (!Proposed.IsValid())
				{
					SetFailure(OutFailure, TEXT("material_proposed_asset_missing"));
					return false;
				}
				UPackage* TargetPackage = CreatePackage(*PackagePath);
				Actual = DuplicatePublishedAsset(
					Proposed.Get(), TargetPackage, FPackageName::GetLongPackageAssetName(PackagePath));
				if (!Actual)
				{
					SetFailure(OutFailure, TEXT("material_asset_publish_failed"));
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
				FString Error;
				if (!CompileAsset(Actual ? Actual : Existing, Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}
				return true;
			}

			virtual bool Save(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!bSave || Operation == EOperation::Remove) return true;
				FString Error;
				if (!UeAgentMaterialOps::SaveAssetPackage(Actual ? Actual : Existing, Error))
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
				return BuildPackageDurablePlan(
					PackagePath,
					PackagePath,
					Operation == EOperation::Remove ? Existing
						: (Existing ? Existing : Proposed.Get()),
					Operation == EOperation::Remove ? EPackageDurableIntent::Delete
						: EPackageDurableIntent::Save,
					OutPlans,
					OutFailure);
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
					FString Error;
					if (!DeleteAsset(RemovalStage.GetAsset(), Error))
					{
						SetFailure(OutFailure, Error);
						return false;
					}
					RemovalStage.Reset();
					Existing = nullptr;
				}
				return true;
			}

			virtual bool Rollback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				FString Error;
				if (Operation == EOperation::Update)
				{
					if (Graph.IsValid() && !Graph->Rollback(*Existing, Error))
					{
						SetFailure(OutFailure, Error);
						return false;
					}
					if (UMaterialFunction* Function = Cast<UMaterialFunction>(Existing))
					{
						Function->Description = OldDescription;
						Function->bExposeToLibrary = bOldExposeToLibrary;
						Function->MarkPackageDirty();
					}
					Actual = Existing;
					return true;
				}
				if (Operation == EOperation::Create || Operation == EOperation::Clone)
				{
					if (Actual && !DeleteAsset(Actual, Error))
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
			bool BuildReadRequests(
				UObject* Root,
				const bool bExpectedAbsent,
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure) const
			{
				OutFailure = FOperationFailure();
				FString Error;
				if (!BuildAssetSnapshotReadRequest(
					Root, PackagePath, AssetKind, bExpectedAbsent, OutRequests, Error))
				{
					SetFailure(OutFailure, TEXT("material_snapshot_request_failed"), Error);
					return false;
				}
				if (!bExpectedAbsent && Root && Graph.IsValid()
					&& !Graph->AppendPropertyReadRequests(*Root, PackagePath, OutRequests, OutFailure))
				{
					return false;
				}
				return true;
			}

			FString PackagePath;
			FString AssetKind;
			EOperation Operation;
			UObject* Existing = nullptr;
			TStrongObjectPtr<UObject> Proposed;
			FAssetRemovalStage RemovalStage;
			UObject* Actual = nullptr;
			TUniquePtr<FPreparedGraph> Graph;
			FFunctionSettings FunctionSettings;
			FString OldDescription;
			bool bOldExposeToLibrary = false;
			bool bCompile = true;
			bool bSave = false;
		};

		class FAssetLifecycle final : public IReflectionOperationLifecycle
		{
		public:
			virtual bool PrepareLifecycle(
				const FPrepareRequest& Request,
				TUniquePtr<IReflectionOperationLifecycleState>& OutState,
				FOperationFailure& OutFailure) const override
			{
				OutState.Reset();
				OutFailure = FOperationFailure();
				if (!Request.IsValid() || !Request.Input->Target.IsValid())
				{
					SetFailure(OutFailure, TEXT("material_apply_request_invalid"));
					return false;
				}

				FString PackagePath;
				Request.Input->Target->TryGetStringField(TEXT("asset_path"), PackagePath);
				PackagePath = NormalizePackagePath(PackagePath);
				if (!FPackageName::IsValidLongPackageName(PackagePath)
					|| FPackageName::GetLongPackageAssetName(PackagePath).IsEmpty()
					|| (!PackagePath.Equals(TEXT("/Game")) && !PackagePath.StartsWith(TEXT("/Game/"))))
				{
					SetFailure(OutFailure, TEXT("invalid_asset_path"), PackagePath);
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
					SetFailure(OutFailure, TEXT("asset_already_exists"), PackagePath);
					return false;
				}
				if ((Operation == EOperation::Update || Operation == EOperation::Remove) && !Existing)
				{
					SetFailure(OutFailure, TEXT("asset_not_found"), PackagePath);
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
						SetFailure(OutFailure, TEXT("source_asset_not_found"), SourcePath);
						return false;
					}
				}
				const FString InferredKind = GetAssetKind(Existing ? Existing : Source);
				if (AssetKind.IsEmpty()) AssetKind = InferredKind;
				if (!InferredKind.IsEmpty() && AssetKind != InferredKind)
				{
					SetFailure(OutFailure, TEXT("material_asset_kind_conflict"), AssetKind + TEXT(" != ") + InferredKind);
					return false;
				}
				if (AssetKind != TEXT("material") && AssetKind != TEXT("material_function"))
				{
					SetFailure(OutFailure, TEXT("material_asset_kind_invalid"), AssetKind);
					return false;
				}

				FFunctionSettings FunctionSettings;
				FString Error;
				if (!ParseFunctionSettings(Request.Input->ProviderExtension, FunctionSettings, Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}
				if (FunctionSettings.HasAny() && AssetKind != TEXT("material_function"))
				{
					SetFailure(OutFailure, TEXT("material_function_settings_target_conflict"));
					return false;
				}
				if ((FunctionSettings.bHasDescription && HasPropertyPath(Request.Input->Properties, TEXT("Description")))
					|| (FunctionSettings.bHasExposeToLibrary
						&& HasPropertyPath(Request.Input->Properties, TEXT("bExposeToLibrary"))))
				{
					SetFailure(OutFailure, TEXT("material_function_settings_property_conflict"));
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
					if (!Proposed)
					{
						SetFailure(OutFailure, Error.IsEmpty() ? TEXT("material_proposed_asset_create_failed") : Error);
						return false;
					}
					if (UMaterialFunction* Function = Cast<UMaterialFunction>(Proposed))
					{
						if (Operation != EOperation::Update) ApplyFunctionSettings(*Function, FunctionSettings);
					}
				}

				TUniquePtr<FPreparedGraph> Graph;
				const TSharedPtr<FJsonObject>* GraphDocument = nullptr;
				if (Request.Input->ProviderExtension.IsValid()
					&& Request.Input->ProviderExtension->TryGetObjectField(TEXT("graph"), GraphDocument))
				{
					if (!GraphDocument || !GraphDocument->IsValid() || Operation == EOperation::Remove)
					{
						SetFailure(OutFailure, TEXT("material_graph_extension_invalid"));
						return false;
					}
					UObject* ValidationOwner = Proposed;
					TStrongObjectPtr<UObject> ValidationSeed;
					if (Operation == EOperation::Update)
					{
						ValidationSeed.Reset(CreateFactorySeed(
							AssetKind, ScratchPackage, NAME_None, RF_Transactional, Error));
						ValidationOwner = ValidationSeed.Get();
					}
					Graph = MakeUnique<FPreparedGraph>();
					if (!ValidationOwner || !Graph->Prepare(
						*ValidationOwner,
						*GraphDocument,
						PackagePath,
						AssetKind == TEXT("material") ? TEXT("MaterialGraph") : TEXT("MaterialFunctionGraph"),
						Error))
					{
						SetFailure(OutFailure, Error.IsEmpty() ? TEXT("material_graph_preflight_failed") : Error);
						return false;
					}
				}

				OutState = MakeUnique<FAssetLifecycleState>(
					PackagePath,
					AssetKind,
					Operation,
					Existing,
					Proposed,
					MoveTemp(Graph),
					MoveTemp(FunctionSettings),
					ReadBool(Request.Input->ProviderExtension, TEXT("compile"), true),
					ReadBool(Request.Input->ProviderExtension, TEXT("save"), false));
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
			Result.ReflectionBaseline.RuntimeLimitsSource = TEXT("shared_runtime_limits");
			Result.PropertyPayload.Mode = TEXT("reflection");
			Result.PropertyPayload.PropertyPathField = TEXT("property_path");
			Result.PropertyPayload.ValueField = TEXT("value");
			Result.PropertyPayload.OptionalAssertionFields = {
				TEXT("value_kind"), TEXT("struct_path"), TEXT("enum_path"), TEXT("element_type"),
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
			const FString& RequestId)
		{
			OutResult = FAssetApplyResult();
			OutResult.Status = TEXT("failed");
			UeAgentApplyWorkspaceContract::FContract WorkspaceContract;
			if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(
				AssetWorkspaceContractId, WorkspaceContract))
			{
				OutResult.Phase = TEXT("registration");
				OutResult.ErrorCode = TEXT("material_workspace_contract_missing");
				return false;
			}
			UeAgentAuthoringWorkspaceLoader::FLoadResult LoadResult;
			if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(
				WorkspaceContract,
				Source,
				LoadResult))
			{
				OutResult.Phase = TEXT("load");
				OutResult.ErrorCode = LoadResult.ErrorCode;
				OutResult.ErrorDetail = LoadResult.ErrorDetail;
				return false;
			}

			const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit* AssetUnit =
				LoadResult.Workspace.FindSingleUnit(TEXT("asset"));
			const TSharedPtr<const FJsonObject> Document = AssetUnit ? AssetUnit->Document : nullptr;
			const TSharedPtr<FJsonObject>* Target = nullptr;
			FString OperationText;
			FAssetApplyRequest Request;
			if (!Document.IsValid()
				|| !Document->TryGetStringField(TEXT("operation"), OperationText)
				|| !ParseOperation(OperationText, Request.Operation)
				|| !Document->TryGetObjectField(TEXT("target"), Target)
				|| !Target || !Target->IsValid())
			{
				OutResult.Phase = TEXT("load");
				OutResult.ErrorCode = TEXT("material_workspace_entry_invalid");
				OutResult.ErrorDetail = OutResult.ErrorCode;
				return false;
			}
			(*Target)->TryGetStringField(TEXT("asset_path"), Request.AssetPath);
			Request.AssetPath = NormalizePackagePath(Request.AssetPath);
			(*Target)->TryGetStringField(TEXT("asset_kind"), Request.AssetKind);
			Request.AssetKind.TrimStartAndEndInline();
			Request.AssetKind.ToLowerInline();
			(*Target)->TryGetStringField(TEXT("source_asset_path"), Request.SourceAssetPath);
			const FString NormalizedExpectedPath = NormalizePackagePath(ExpectedAssetPath);
			FString NormalizedExpectedKind = ExpectedAssetKind.TrimStartAndEnd();
			NormalizedExpectedKind.ToLowerInline();
			if (!NormalizedExpectedPath.IsEmpty()
				&& !Request.AssetPath.Equals(NormalizedExpectedPath, ESearchCase::IgnoreCase))
			{
				OutResult.Phase = TEXT("preflight");
				OutResult.ErrorCode = TEXT("material_workspace_asset_path_conflict");
				OutResult.ErrorDetail = Request.AssetPath + TEXT(" != ") + NormalizedExpectedPath;
				return false;
			}
			UObject* ExistingBeforeApply = LoadAsset(Request.AssetPath);
			if (Request.AssetKind.IsEmpty()) Request.AssetKind = GetAssetKind(ExistingBeforeApply);
			if (!NormalizedExpectedKind.IsEmpty()
				&& !Request.AssetKind.Equals(NormalizedExpectedKind, ESearchCase::IgnoreCase))
			{
				OutResult.Phase = TEXT("preflight");
				OutResult.ErrorCode = TEXT("material_workspace_asset_kind_conflict");
				OutResult.ErrorDetail = Request.AssetKind + TEXT(" != ") + NormalizedExpectedKind;
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			if (Document->TryGetArrayField(TEXT("properties"), Properties) && Properties)
			{
				Request.Properties = *Properties;
			}
			const TSharedPtr<FJsonObject>* ProviderExtension = nullptr;
			if (Document->TryGetObjectField(TEXT("provider_extension"), ProviderExtension)
				&& ProviderExtension && ProviderExtension->IsValid())
			{
				(*ProviderExtension)->TryGetBoolField(TEXT("compile"), Request.bCompile);
				(*ProviderExtension)->TryGetBoolField(TEXT("save"), Request.bSave);
				const TSharedPtr<FJsonObject>* GraphObject = nullptr;
				if ((*ProviderExtension)->TryGetObjectField(TEXT("graph"), GraphObject)
					&& GraphObject && GraphObject->IsValid())
				{
					Request.Graph = *GraphObject;
				}
				const TSharedPtr<FJsonObject>* FunctionSettings = nullptr;
				if ((*ProviderExtension)->TryGetObjectField(TEXT("function_settings"), FunctionSettings)
					&& FunctionSettings && FunctionSettings->IsValid())
				{
					Request.FunctionSettings = *FunctionSettings;
				}
			}
			UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
			UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure GraphFailure;
			if (!UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(
				WorkspaceContract,
				LoadResult.Workspace,
				UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs(),
				Graph,
				GraphFailure))
			{
				OutResult.Phase = TEXT("preflight");
				OutResult.ErrorCode = GraphFailure.ErrorCode;
				OutResult.ErrorDetail = GraphFailure.ErrorDetail;
				return false;
			}

			const bool bExisted = LoadAsset(Request.AssetPath) != nullptr;
			FBackendOptions BackendOptions;
			BackendOptions.bUseEditorTransaction = Request.Operation != EOperation::Remove;
			BackendOptions.TransactionDescription = NSLOCTEXT(
				"UeAgentInterface", "MaterialAssetWorkspaceApply", "Apply Material asset workspace");
			UeAgentAuthoringWorkspaceTransaction::FExecutionOptions ExecutionOptions;
			FOperationFailure RequestFailure;
			if (!BuildApplyEntryExecutionOptions(RequestId, ExecutionOptions, RequestFailure))
			{
				OutResult.Phase = TEXT("preflight");
				OutResult.ErrorCode = RequestFailure.ErrorCode;
				OutResult.ErrorDetail = RequestFailure.ErrorDetail;
				return false;
			}
			ExecutionOptions.bSaveWhenCallerControlled = Request.bSave;
			FApplyWorkspaceResult ApplyResult;
			const bool bApplied = ApplyWorkspace(
				WorkspaceContract,
				LoadResult.Workspace,
				Graph,
				UeAgentApplyInputContract::FRegistry::Get(),
				FProviderRegistry::Get(),
				ExecutionOptions,
				BackendOptions,
				ApplyResult);
			OutResult.UnitsChecked = ApplyResult.Preflight.UnitsChecked;
			OutResult.MutationsApplied = ApplyResult.Execution.MutationsApplied;
			OutResult.bRollbackAttempted = ApplyResult.Execution.bRollbackAttempted;
			OutResult.bRollbackSucceeded = ApplyResult.Execution.bRollbackSucceeded;
			if (!bApplied)
			{
				const bool bPreflight = !ApplyResult.Preflight.IsReady();
				OutResult.Phase = bPreflight ? TEXT("preflight") : ApplyResult.Execution.Phase;
				OutResult.ErrorCode = bPreflight ? ApplyResult.Preflight.CauseCode : ApplyResult.Execution.CauseCode;
				if (OutResult.ErrorCode.IsEmpty())
				{
					OutResult.ErrorCode = bPreflight ? ApplyResult.Preflight.ErrorCode : ApplyResult.Execution.ErrorCode;
				}
				OutResult.ErrorDetail = bPreflight ? ApplyResult.Preflight.ErrorDetail : ApplyResult.Execution.ErrorDetail;
				return false;
			}

			OutResult.Status = TEXT("applied");
			OutResult.Phase = TEXT("complete");
			const EOperation ResolvedOperation = Request.Operation == EOperation::Upsert
				? (bExisted ? EOperation::Update : EOperation::Create)
				: Request.Operation;
			OutResult.ResolvedOperation = OperationToString(ResolvedOperation);
			OutResult.AssetPath = NormalizePackagePath(Request.AssetPath);
			OutResult.Asset = ResolvedOperation == EOperation::Remove ? nullptr : LoadAsset(OutResult.AssetPath);
			OutResult.ObjectPath = OutResult.Asset ? OutResult.Asset->GetPathName() : ToObjectPath(OutResult.AssetPath);
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

	bool RegisterApplyContracts(FString& OutError)
	{
		UeAgentApplyInputContract::ERegistrationResult InputResult;
		if (!UeAgentApplyInputContract::FRegistry::Get().RegisterContract(
			MakeInputContract(), InputResult, OutError)) return false;
		UeAgentApplyWorkspaceContract::ERegistrationResult WorkspaceResult;
		return UeAgentApplyWorkspaceContract::FRegistry::Get().RegisterContract(
			MakeWorkspaceContract(), WorkspaceResult, OutError);
	}

	TSharedRef<IApplyProvider> MakeApplyProvider()
	{
		static const TSharedRef<IReflectionOperationLifecycle> Lifecycle = MakeShared<FAssetLifecycle>();
		UeAgentAuthoringObjectApply::FOptions Options;
		Options.bAllowDomainAuthorizedOwners = true;
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
			RequestId);
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
