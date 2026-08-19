// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Material/Authoring/UeAgentMaterialGraphMutationApplyProvider.h"

#include "Commands/Material/UeAgentHttpServer_MaterialOps.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringDescriptors.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringState.h"
#include "Domains/Material/Authoring/UeAgentMaterialExpressionResolver.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"
#include "Misc/PackageName.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
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

		struct FInputBackup
		{
			FExpressionInput* Address = nullptr;
			FExpressionInput Value;
		};

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

		UObject* LoadGraphOwner(const FString& InPath)
		{
			const FString PackagePath = NormalizePackagePath(InPath);
			if (PackagePath.IsEmpty()) return nullptr;
			UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *ToObjectPath(PackagePath));
			return Object && GetExpressionCollection(*Object) ? Object : nullptr;
		}

		bool ReadString(const TSharedPtr<const FJsonObject>& Object, const TCHAR* Field, FString& OutValue)
		{
			OutValue.Reset();
			return Object.IsValid() && Object->TryGetStringField(Field, OutValue);
		}

		bool ReadBool(const TSharedPtr<const FJsonObject>& Object, const TCHAR* Field, const bool DefaultValue)
		{
			bool Value = DefaultValue;
			if (Object.IsValid()) Object->TryGetBoolField(Field, Value);
			return Value;
		}

		int32 ReadInt(const TSharedPtr<const FJsonObject>& Object, const TCHAR* Field, const int32 DefaultValue)
		{
			double Value = DefaultValue;
			if (Object.IsValid()) Object->TryGetNumberField(Field, Value);
			return static_cast<int32>(Value);
		}

		bool ReadGuid(const TSharedPtr<const FJsonObject>& Object, const TCHAR* Field, FGuid& OutGuid, FString& OutError)
		{
			OutGuid.Invalidate();
			FString Text;
			if (!ReadString(Object, Field, Text) || Text.IsEmpty()) return true;
			if (FGuid::Parse(Text, OutGuid)) return true;
			OutError = FString(TEXT("material_expression_guid_invalid:")) + Field;
			return false;
		}

		bool ReadSelector(
			const TSharedPtr<const FJsonObject>& Object,
			const FString& Prefix,
			FExpressionSelector& OutSelector,
			FString& OutError)
		{
			OutSelector = FExpressionSelector();
			if (!ReadGuid(Object, *(Prefix + TEXT("guid")), OutSelector.ExpressionGuid, OutError)) return false;
			ReadString(Object, *(Prefix + TEXT("name")), OutSelector.ExpressionName);
			OutSelector.ExpressionIndex = ReadInt(Object, *(Prefix + TEXT("index")), INDEX_NONE);
			return true;
		}

		void WriteSelector(
			const TSharedRef<FJsonObject>& Target,
			const FString& Prefix,
			const FExpressionSelector& Selector)
		{
			if (Selector.ExpressionGuid.IsValid())
			{
				Target->SetStringField(Prefix + TEXT("guid"), Selector.ExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			}
			if (!Selector.ExpressionName.IsEmpty()) Target->SetStringField(Prefix + TEXT("name"), Selector.ExpressionName);
			if (Selector.ExpressionIndex != INDEX_NONE) Target->SetNumberField(Prefix + TEXT("index"), Selector.ExpressionIndex);
		}

		FString KindToString(const EGraphMutationKind Kind)
		{
			switch (Kind)
			{
			case EGraphMutationKind::Expression: return TEXT("expression");
			case EGraphMutationKind::Connection: return TEXT("connection");
			case EGraphMutationKind::RootInput: return TEXT("root_input");
			default: return TEXT("unknown");
			}
		}

		bool ParseKind(const FString& Text, EGraphMutationKind& OutKind)
		{
			if (Text.Equals(TEXT("expression"), ESearchCase::IgnoreCase))
			{
				OutKind = EGraphMutationKind::Expression;
				return true;
			}
			if (Text.Equals(TEXT("connection"), ESearchCase::IgnoreCase))
			{
				OutKind = EGraphMutationKind::Connection;
				return true;
			}
			if (Text.Equals(TEXT("root_input"), ESearchCase::IgnoreCase))
			{
				OutKind = EGraphMutationKind::RootInput;
				return true;
			}
			return false;
		}

		bool CompileOwner(UObject* Owner, FString& OutError)
		{
			OutError.Reset();
			if (UMaterial* Material = Cast<UMaterial>(Owner))
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
			if (UMaterialFunction* Function = Cast<UMaterialFunction>(Owner))
			{
				UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
				return true;
			}
			OutError = TEXT("material_compile_target_invalid");
			return false;
		}

		void CaptureInput(FExpressionInput* Input, TArray<FInputBackup>& OutBackups, TSet<FExpressionInput*>& Seen)
		{
			if (!Input || Seen.Contains(Input)) return;
			Seen.Add(Input);
			OutBackups.Add({ Input, *Input });
		}

		void CaptureAllInputs(UObject& Owner, TArray<FInputBackup>& OutBackups)
		{
			OutBackups.Reset();
			TSet<FExpressionInput*> Seen;
			const FMaterialExpressionCollection* Collection = GetExpressionCollection(Owner);
			if (Collection)
			{
				for (UMaterialExpression* Expression : Collection->Expressions)
				{
					if (!Expression) continue;
					for (FExpressionInputIterator It{ Expression }; It; ++It) CaptureInput(It.Input, OutBackups, Seen);
				}
			}
			if (UMaterial* Material = Cast<UMaterial>(&Owner))
			{
				for (const EMaterialProperty Property : GetSupportedMaterialProperties())
				{
					CaptureInput(Material->GetExpressionInputForProperty(Property), OutBackups, Seen);
				}
			}
		}

		void RestoreInputs(const TArray<FInputBackup>& Backups)
		{
			for (const FInputBackup& Backup : Backups)
			{
				if (Backup.Address) *Backup.Address = Backup.Value;
			}
		}

		class FGraphMutationLifecycleState final : public IReflectionOperationLifecycleState
		{
		public:
			virtual FString GetTargetKey() const override { return TargetKey; }
			virtual EOperation GetResolvedOperation() const override { return Operation; }

			virtual FReflectionRoot GetProposedRoot() const override
			{
				if (Kind == EGraphMutationKind::Expression)
				{
					return FReflectionRoot::MakeObject(Operation == EOperation::Create ? ScratchExpression.Get() : Expression);
				}
				return FReflectionRoot::MakeObject(Owner);
			}

			virtual FReflectionRoot GetActualRoot() const override
			{
				if (Kind == EGraphMutationKind::Expression)
				{
					return FReflectionRoot::MakeObject(Operation == EOperation::Create ? CreatedExpression : Expression);
				}
				return FReflectionRoot::MakeObject(Owner);
			}

			virtual bool CaptureRollback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!Owner)
				{
					SetFailure(OutFailure, TEXT("material_graph_owner_missing"));
					return false;
				}
				if (Kind == EGraphMutationKind::Connection || Kind == EGraphMutationKind::RootInput)
				{
					TSet<FExpressionInput*> Seen;
					CaptureInput(MutatedInput, InputBackups, Seen);
				}
				else if (Kind == EGraphMutationKind::Expression && Operation == EOperation::Remove)
				{
					FMaterialExpressionCollection* Collection = GetExpressionCollection(*Owner);
					if (!Collection || !Expression)
					{
						SetFailure(OutFailure, TEXT("material_expression_remove_capture_failed"));
						return false;
					}
					OldCollection = *Collection;
					RemovedExpression.Reset(Expression);
					bRemovedExpressionWasRooted = Expression->IsRooted();
					CaptureAllInputs(*Owner, InputBackups);
				}
				bRollbackCaptured = true;
				return true;
			}

			virtual bool ApplyProposed(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!Owner || !bRollbackCaptured)
				{
					SetFailure(OutFailure, TEXT("material_graph_mutation_not_captured"));
					return false;
				}
				if (Kind == EGraphMutationKind::Expression)
				{
					if (Operation == EOperation::Create)
					{
						if (!ScratchExpression.IsValid())
						{
							SetFailure(OutFailure, TEXT("material_expression_seed_missing"));
							return false;
						}
						const FName DesiredName = SuggestedName.IsEmpty()
							? MakeUniqueObjectName(Owner, ScratchExpression->GetClass())
							: MakeUniqueObjectName(Owner, ScratchExpression->GetClass(), FName(*SuggestedName));
						CreatedExpression = Cast<UMaterialExpression>(StaticDuplicateObject(
							ScratchExpression.Get(), Owner, DesiredName));
						FMaterialExpressionCollection* Collection = GetExpressionCollection(*Owner);
						if (!CreatedExpression || !Collection)
						{
							SetFailure(OutFailure, TEXT("material_expression_create_failed"));
							return false;
						}
						CreatedExpression->MaterialExpressionGuid = NewExpressionGuid;
						Collection->AddExpression(CreatedExpression);
						if (UMaterialExpressionMaterialFunctionCall* FunctionCall =
							Cast<UMaterialExpressionMaterialFunctionCall>(CreatedExpression))
						{
							FunctionCall->UpdateFromFunctionResource();
						}
					}
					else if (Operation == EOperation::Remove)
					{
						FMaterialExpressionCollection* Collection = GetExpressionCollection(*Owner);
						if (!Expression || !Collection)
						{
							SetFailure(OutFailure, TEXT("material_expression_remove_target_missing"));
							return false;
						}
						Collection->RemoveExpression(Expression);
						for (const FInputBackup& Backup : InputBackups)
						{
							if (Backup.Address && Backup.Address->Expression == Expression) *Backup.Address = FExpressionInput();
						}
					}
				}
				else if (Kind == EGraphMutationKind::Connection)
				{
					FString Error;
					if (!FromExpression || !ToExpression
						|| !ConnectExpressions(*FromExpression, FromOutputName, *ToExpression, ToInputName, Error))
					{
						SetFailure(OutFailure, Error.IsEmpty() ? TEXT("connect_expressions_failed") : Error);
						return false;
					}
				}
				else
				{
					FString Error;
					if (!Expression || !ConnectExpressionToProperty(*Expression, FromOutputName, MaterialProperty, Error))
					{
						SetFailure(OutFailure, Error.IsEmpty() ? TEXT("connect_expression_failed") : Error);
						return false;
					}
				}
				bMutationApplied = true;
				return true;
			}

			virtual bool ApplyLifecycle(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!Owner)
				{
					SetFailure(OutFailure, TEXT("material_graph_owner_missing"));
					return false;
				}
				RefreshGraphOwner(*Owner);
				return true;
			}

			virtual bool Compile(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!bCompile) return true;
				FString Error;
				if (CompileOwner(Owner, Error)) return true;
				SetFailure(OutFailure, Error);
				return false;
			}

			virtual bool Save(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!bSave) return true;
				FString Error;
				if (UeAgentMaterialOps::SaveAssetPackage(Owner, Error)) return true;
				SetFailure(OutFailure, Error.IsEmpty() ? TEXT("material_save_failed") : Error);
				return false;
			}

			virtual bool BuildDurablePackagePlans(
				TArray<FPackageDurablePlan>& OutPlans,
				FOperationFailure& OutFailure) const override
			{
				return BuildPackageDurablePlan(
					TargetKey,
					Owner ? Owner->GetOutermost()->GetName() : FString(),
					Owner,
					EPackageDurableIntent::Save,
					OutPlans,
					OutFailure);
			}

			virtual bool BuildDomainExpectedReadRequests(
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure) const override
			{
				return BuildReadRequests(OutRequests, OutFailure);
			}

			virtual bool BuildDomainActualReadRequests(
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure) const override
			{
				return BuildReadRequests(OutRequests, OutFailure);
			}

			virtual bool Commit(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Kind == EGraphMutationKind::Expression && Operation == EOperation::Remove && RemovedExpression.IsValid())
				{
					if (bRemovedExpressionWasRooted) RemovedExpression->RemoveFromRoot();
					RemovedExpression->MarkAsGarbage();
					RemovedExpression.Reset();
				}
				InputBackups.Reset();
				return true;
			}

			virtual bool Rollback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!Owner || !bRollbackCaptured) return true;
				if (Kind == EGraphMutationKind::Expression && Operation == EOperation::Create)
				{
					if (CreatedExpression)
					{
						if (FMaterialExpressionCollection* Collection = GetExpressionCollection(*Owner))
						{
							Collection->RemoveExpression(CreatedExpression);
						}
						CreatedExpression->MarkAsGarbage();
						CreatedExpression = nullptr;
					}
				}
				else if (Kind == EGraphMutationKind::Expression && Operation == EOperation::Remove)
				{
					if (FMaterialExpressionCollection* Collection = GetExpressionCollection(*Owner)) *Collection = OldCollection;
					RestoreInputs(InputBackups);
				}
				else if (bMutationApplied)
				{
					RestoreInputs(InputBackups);
				}
				RefreshGraphOwner(*Owner);
				return true;
			}

			bool BuildReadRequests(
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure) const
			{
				OutFailure = FOperationFailure();
				FString Error;
				if (BuildAssetSnapshotReadRequest(
					Owner,
					AssetPath,
					GetAssetKind(Owner),
					false,
					OutRequests,
					Error)) return true;
				SetFailure(OutFailure, TEXT("material_snapshot_request_failed"), Error);
				return false;
			}

			FString AssetPath;
			FString TargetKey;
			EGraphMutationKind Kind = EGraphMutationKind::Expression;
			EOperation Operation = EOperation::Update;
			UObject* Owner = nullptr;
			UMaterialExpression* Expression = nullptr;
			UMaterialExpression* FromExpression = nullptr;
			UMaterialExpression* ToExpression = nullptr;
			TStrongObjectPtr<UObject> ScratchOwner;
			TStrongObjectPtr<UMaterialExpression> ScratchExpression;
			UMaterialExpression* CreatedExpression = nullptr;
			TStrongObjectPtr<UMaterialExpression> RemovedExpression;
			FGuid NewExpressionGuid;
			FString SuggestedName;
			FString FromOutputName;
			FString ToInputName;
			EMaterialProperty MaterialProperty = MP_MAX;
			FExpressionInput* MutatedInput = nullptr;
			FMaterialExpressionCollection OldCollection;
			TArray<FInputBackup> InputBackups;
			bool bCompile = true;
			bool bSave = false;
			bool bRollbackCaptured = false;
			bool bMutationApplied = false;
			bool bRemovedExpressionWasRooted = false;
		};

		class FGraphMutationLifecycle final : public IReflectionOperationLifecycle
		{
		public:
			virtual bool PrepareLifecycle(
				const FPrepareRequest& Request,
				TUniquePtr<IReflectionOperationLifecycleState>& OutState,
				FOperationFailure& OutFailure) const override
			{
				OutState.Reset();
				OutFailure = FOperationFailure();
				if (!Request.IsValid() || Request.Contract->ContractId != GraphMutationInputContractId
					|| !Request.Input->Target.IsValid() || !Request.Input->ProviderExtension.IsValid())
				{
					SetFailure(OutFailure, TEXT("material_graph_mutation_request_invalid"));
					return false;
				}

				TUniquePtr<FGraphMutationLifecycleState> State = MakeUnique<FGraphMutationLifecycleState>();
				ReadString(Request.Input->Target, TEXT("asset_path"), State->AssetPath);
				State->AssetPath = NormalizePackagePath(State->AssetPath);
				if (!FPackageName::IsValidLongPackageName(State->AssetPath)
					|| (!State->AssetPath.Equals(TEXT("/Game")) && !State->AssetPath.StartsWith(TEXT("/Game/"))))
				{
					SetFailure(OutFailure, TEXT("invalid_asset_path"), State->AssetPath);
					return false;
				}
				State->Owner = LoadGraphOwner(State->AssetPath);
				if (!State->Owner)
				{
					SetFailure(OutFailure, TEXT("material_graph_owner_not_found"), State->AssetPath);
					return false;
				}

				FString KindText;
				ReadString(Request.Input->ProviderExtension, TEXT("mutation_kind"), KindText);
				if (!ParseKind(KindText, State->Kind))
				{
					SetFailure(OutFailure, TEXT("material_graph_mutation_kind_invalid"), KindText);
					return false;
				}
				State->Operation = Request.Input->RequestedOperation == EOperation::Upsert
					? EOperation::Update
					: Request.Input->RequestedOperation;
				State->bCompile = ReadBool(Request.Input->ProviderExtension, TEXT("compile"), true);
				State->bSave = ReadBool(Request.Input->ProviderExtension, TEXT("save"), false);
				FString Error;
				FExpressionSelector ExpressionSelector;
				FExpressionSelector FromSelector;
				FExpressionSelector ToSelector;
				if (!ReadSelector(Request.Input->Target, TEXT("expression_"), ExpressionSelector, Error)
					|| !ReadSelector(Request.Input->Target, TEXT("from_expression_"), FromSelector, Error)
					|| !ReadSelector(Request.Input->Target, TEXT("to_expression_"), ToSelector, Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}

				if (State->Kind == EGraphMutationKind::Expression)
				{
					if (State->Operation != EOperation::Create
						&& State->Operation != EOperation::Update
						&& State->Operation != EOperation::Remove)
					{
						SetFailure(OutFailure, TEXT("material_expression_operation_unsupported"));
						return false;
					}
					if (State->Operation == EOperation::Create)
					{
						FExpressionResolveRequest ResolveRequest;
						ReadString(Request.Input->ProviderExtension, TEXT("expression_class"), ResolveRequest.ExpressionClassPath);
						ReadString(Request.Input->ProviderExtension, TEXT("path"), ResolveRequest.Path);
						ReadString(Request.Input->ProviderExtension, TEXT("catalog_name"), ResolveRequest.CatalogName);
						ReadString(Request.Input->ProviderExtension, TEXT("material_function_path"), ResolveRequest.MaterialFunctionPath);
						ResolveRequest.ContextAssetPath = State->AssetPath;
						ResolveRequest.ContextGraphName = TEXT("MaterialGraph");
						FExpressionResolveResult ResolveResult;
						if (!ResolveExpression(ResolveRequest, ResolveResult, Error))
						{
							SetFailure(OutFailure, Error);
							return false;
						}
						UPackage* ScratchPackage = CreatePackage(*FString::Printf(
							TEXT("/Temp/UAI/Material/GraphMutation_%s"),
							*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
						State->ScratchOwner.Reset(CreateFactorySeed(
							GetAssetKind(State->Owner), ScratchPackage, NAME_None, RF_Transactional, Error));
						if (!State->ScratchOwner.IsValid())
						{
							SetFailure(OutFailure, Error.IsEmpty() ? TEXT("material_expression_seed_owner_failed") : Error);
							return false;
						}
						UMaterialExpression* ScratchExpression = CreateExpression(
							*State->ScratchOwner,
							ResolveResult.ExpressionClass,
							ReadInt(Request.Input->ProviderExtension, TEXT("node_pos_x"), 0),
							ReadInt(Request.Input->ProviderExtension, TEXT("node_pos_y"), 0));
						State->ScratchExpression.Reset(ScratchExpression);
						if (!ScratchExpression)
						{
							SetFailure(OutFailure, TEXT("material_expression_seed_create_failed"));
							return false;
						}
						if (ResolveResult.MaterialFunction)
						{
							UMaterialExpressionMaterialFunctionCall* FunctionCall =
								Cast<UMaterialExpressionMaterialFunctionCall>(ScratchExpression);
							if (!FunctionCall || !FunctionCall->SetMaterialFunction(ResolveResult.MaterialFunction))
							{
								SetFailure(OutFailure, TEXT("material_function_assignment_failed"));
								return false;
							}
							FunctionCall->UpdateFromFunctionResource();
						}
						if (!ReadGuid(Request.Input->ProviderExtension, TEXT("new_expression_guid"), State->NewExpressionGuid, Error))
						{
							SetFailure(OutFailure, Error);
							return false;
						}
						if (!State->NewExpressionGuid.IsValid()) State->NewExpressionGuid = FGuid::NewGuid();
						FExpressionSelector GuidSelector;
						GuidSelector.ExpressionGuid = State->NewExpressionGuid;
						FString ResolveError;
						if (ResolveExpressionUnique(*State->Owner, GuidSelector, ResolveError))
						{
							SetFailure(OutFailure, TEXT("material_expression_guid_conflict"));
							return false;
						}
						ScratchExpression->MaterialExpressionGuid = State->NewExpressionGuid;
						ReadString(Request.Input->ProviderExtension, TEXT("suggested_name"), State->SuggestedName);
					}
					else
					{
						State->Expression = ResolveExpressionUnique(*State->Owner, ExpressionSelector, Error);
						if (!State->Expression)
						{
							SetFailure(OutFailure, Error.IsEmpty() ? TEXT("material_expression_not_found") : Error);
							return false;
						}
						if (State->Operation == EOperation::Remove && Request.Input->HasProperties())
						{
							SetFailure(OutFailure, TEXT("material_expression_remove_properties_forbidden"));
							return false;
						}
					}
				}
				else if (State->Kind == EGraphMutationKind::Connection)
				{
					if (Request.Input->HasProperties())
					{
						SetFailure(OutFailure, TEXT("material_connection_properties_forbidden"));
						return false;
					}
					State->FromExpression = ResolveExpressionUnique(*State->Owner, FromSelector, Error);
					if (!State->FromExpression)
					{
						SetFailure(OutFailure, Error.IsEmpty() ? TEXT("material_from_expression_not_found") : Error);
						return false;
					}
					State->ToExpression = ResolveExpressionUnique(*State->Owner, ToSelector, Error);
					if (!State->ToExpression)
					{
						SetFailure(OutFailure, Error.IsEmpty() ? TEXT("material_to_expression_not_found") : Error);
						return false;
					}
					ReadString(Request.Input->ProviderExtension, TEXT("from_output_name"), State->FromOutputName);
					ReadString(Request.Input->ProviderExtension, TEXT("to_input_name"), State->ToInputName);
					int32 OutputIndex = INDEX_NONE;
					if (!ResolveExpressionOutputIndex(*State->FromExpression, State->FromOutputName, OutputIndex)
						|| !ResolveExpressionInput(*State->ToExpression, State->ToInputName, State->MutatedInput))
					{
						SetFailure(OutFailure, TEXT("material_connection_pin_not_found"));
						return false;
					}
				}
				else
				{
					if (Request.Input->HasProperties() || !Cast<UMaterial>(State->Owner))
					{
						SetFailure(OutFailure, Request.Input->HasProperties()
							? TEXT("material_root_input_properties_forbidden")
							: TEXT("material_function_root_inputs_not_allowed"));
						return false;
					}
					State->Expression = ResolveExpressionUnique(*State->Owner, ExpressionSelector, Error);
					if (!State->Expression)
					{
						SetFailure(OutFailure, Error.IsEmpty() ? TEXT("material_expression_not_found") : Error);
						return false;
					}
					FString PropertyName;
					ReadString(Request.Input->ProviderExtension, TEXT("material_property"), PropertyName);
					if (!ParseMaterialProperty(PropertyName, State->MaterialProperty))
					{
						SetFailure(OutFailure, TEXT("invalid_material_property"), PropertyName);
						return false;
					}
					ReadString(Request.Input->ProviderExtension, TEXT("from_output_name"), State->FromOutputName);
					int32 OutputIndex = INDEX_NONE;
					State->MutatedInput = CastChecked<UMaterial>(State->Owner)->GetExpressionInputForProperty(State->MaterialProperty);
					if (!State->MutatedInput
						|| !ResolveExpressionOutputIndex(*State->Expression, State->FromOutputName, OutputIndex))
					{
						SetFailure(OutFailure, TEXT("material_root_input_pin_not_found"));
						return false;
					}
				}

				State->TargetKey = State->AssetPath + TEXT("#") + KindToString(State->Kind);
				if (State->Expression) State->TargetKey += TEXT(":") + State->Expression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
				if (State->FromExpression) State->TargetKey += TEXT(":") + State->FromExpression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
				if (State->ToExpression) State->TargetKey += TEXT(":") + State->ToExpression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
				if (State->Operation == EOperation::Create) State->TargetKey += TEXT(":") + State->NewExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
				OutState = MoveTemp(State);
				return true;
			}
		};

		FInputFieldDescriptor Field(const TCHAR* Name, const TCHAR* Role, const TCHAR* TypeId)
		{
			FInputFieldDescriptor Result;
			Result.Name = Name;
			Result.Role = Role;
			Result.TypeId = TypeId;
			return Result;
		}

		FOperationDescriptor OperationDescriptor(const EOperation Operation)
		{
			FOperationDescriptor Result;
			Result.Operation = Operation;
			Result.IdentityOneOf = { { TEXT("asset_path") } };
			Result.AcceptedSections = { TEXT("target"), TEXT("provider_extension") };
			if (Operation != EOperation::Remove) Result.AcceptedSections.Add(TEXT("properties"));
			Result.OmittedFieldPolicy = Operation == EOperation::Create
				? TEXT("ue_native_default")
				: Operation == EOperation::Remove ? TEXT("not_applicable") : TEXT("preserve_current");
			Result.NativeDefaultPolicy = Operation == EOperation::Create
				? TEXT("required")
				: Operation == EOperation::Remove ? TEXT("not_applicable") : TEXT("forbidden");
			Result.bDestructive = Operation == EOperation::Remove;
			return Result;
		}

		FContract MakeInputContract()
		{
			UeAgentApplyInputContract::FContract Result;
			Result.Schema = TEXT("uai.apply_input_contract.v1");
			Result.ContractId = GraphMutationInputContractId;
			Result.ContractVersion = TEXT("1.0.0");
			Result.ProfileId = TEXT("material.graph_mutation");
			Result.TargetKind = TEXT("material_graph_mutation");
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
			Result.InputFields = {
				Field(TEXT("asset_path"), TEXT("identity"), TEXT("ue_asset_path")),
				Field(TEXT("expression_guid"), TEXT("selector"), TEXT("guid")),
				Field(TEXT("expression_name"), TEXT("selector"), TEXT("string")),
				Field(TEXT("expression_index"), TEXT("selector"), TEXT("int32")),
				Field(TEXT("from_expression_guid"), TEXT("selector"), TEXT("guid")),
				Field(TEXT("from_expression_name"), TEXT("selector"), TEXT("string")),
				Field(TEXT("from_expression_index"), TEXT("selector"), TEXT("int32")),
				Field(TEXT("to_expression_guid"), TEXT("selector"), TEXT("guid")),
				Field(TEXT("to_expression_name"), TEXT("selector"), TEXT("string")),
				Field(TEXT("to_expression_index"), TEXT("selector"), TEXT("int32")),
				Field(TEXT("operation"), TEXT("operation_control"), TEXT("apply_operation")),
				Field(TEXT("properties"), TEXT("intent"), TEXT("property_descriptor_list")),
				Field(TEXT("mutation_kind"), TEXT("provider_extension"), TEXT("material_graph_mutation_kind")),
				Field(TEXT("expression_class"), TEXT("provider_extension"), TEXT("ue_class_path")),
				Field(TEXT("path"), TEXT("provider_extension"), TEXT("ue_class_or_asset_path")),
				Field(TEXT("catalog_name"), TEXT("provider_extension"), TEXT("string")),
				Field(TEXT("material_function_path"), TEXT("provider_extension"), TEXT("ue_asset_path")),
				Field(TEXT("new_expression_guid"), TEXT("provider_extension"), TEXT("guid")),
				Field(TEXT("suggested_name"), TEXT("provider_extension"), TEXT("string")),
				Field(TEXT("node_pos_x"), TEXT("provider_extension"), TEXT("int32")),
				Field(TEXT("node_pos_y"), TEXT("provider_extension"), TEXT("int32")),
				Field(TEXT("from_output_name"), TEXT("provider_extension"), TEXT("string")),
				Field(TEXT("to_input_name"), TEXT("provider_extension"), TEXT("string")),
				Field(TEXT("material_property"), TEXT("provider_extension"), TEXT("material_property")),
				Field(TEXT("compile"), TEXT("provider_extension"), TEXT("bool")),
				Field(TEXT("save"), TEXT("provider_extension"), TEXT("bool")),
			};
			for (const EOperation Operation : { EOperation::Create, EOperation::Update, EOperation::Upsert, EOperation::Remove })
			{
				Result.Operations.Add(OperationDescriptor(Operation));
			}
			Result.ProviderId = GraphMutationApplyProviderId;
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
			Result.ContractId = GraphMutationWorkspaceContractId;
			Result.ContractVersion = TEXT("1.0.0");
			Result.ProfileId = TEXT("material.graph_mutation");
			Result.Sources = {
				{ ESourceKind::Directory, TEXT("physical"), TEXT("index") },
				{ ESourceKind::SingleFile, TEXT("virtual"), TEXT("object") },
				{ ESourceKind::Inline, TEXT("virtual"), TEXT("object") },
			};
			Result.Entry.DirectoryEntry = TEXT("index.hjson");
			Result.Entry.VirtualUnitId = TEXT("mutation");
			FUnitDescriptor Unit;
			Unit.UnitId = TEXT("mutation");
			Unit.PathPattern = TEXT("index.hjson");
			Unit.Role = TEXT("object");
			Unit.Cardinality = TEXT("one");
			Unit.bRequired = true;
			Unit.RouteId = TEXT("material.graph_mutation");
			Unit.ApplyContractRef = GraphMutationInputContractId;
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

		TSharedRef<FJsonObject> MakeApplyDocument(const FGraphMutationRequest& Request)
		{
			TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
			Target->SetStringField(TEXT("asset_path"), Request.AssetPath);
			WriteSelector(Target, TEXT("expression_"), Request.Expression);
			WriteSelector(Target, TEXT("from_expression_"), Request.FromExpression);
			WriteSelector(Target, TEXT("to_expression_"), Request.ToExpression);

			TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
			Extension->SetStringField(TEXT("mutation_kind"), KindToString(Request.Kind));
			if (!Request.ExpressionClassPath.IsEmpty()) Extension->SetStringField(TEXT("expression_class"), Request.ExpressionClassPath);
			if (!Request.Path.IsEmpty()) Extension->SetStringField(TEXT("path"), Request.Path);
			if (!Request.CatalogName.IsEmpty()) Extension->SetStringField(TEXT("catalog_name"), Request.CatalogName);
			if (!Request.MaterialFunctionPath.IsEmpty()) Extension->SetStringField(TEXT("material_function_path"), Request.MaterialFunctionPath);
			if (Request.NewExpressionGuid.IsValid()) Extension->SetStringField(TEXT("new_expression_guid"), Request.NewExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			if (!Request.SuggestedName.IsEmpty()) Extension->SetStringField(TEXT("suggested_name"), Request.SuggestedName);
			Extension->SetNumberField(TEXT("node_pos_x"), Request.NodePosX);
			Extension->SetNumberField(TEXT("node_pos_y"), Request.NodePosY);
			if (!Request.FromOutputName.IsEmpty()) Extension->SetStringField(TEXT("from_output_name"), Request.FromOutputName);
			if (!Request.ToInputName.IsEmpty()) Extension->SetStringField(TEXT("to_input_name"), Request.ToInputName);
			if (!Request.MaterialProperty.IsEmpty()) Extension->SetStringField(TEXT("material_property"), Request.MaterialProperty);
			Extension->SetBoolField(TEXT("compile"), Request.bCompile);
			Extension->SetBoolField(TEXT("save"), Request.bSave);

			TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
			Document->SetStringField(TEXT("operation"), OperationToString(Request.Operation));
			Document->SetObjectField(TEXT("target"), Target);
			Document->SetObjectField(TEXT("provider_extension"), Extension);
			if (!Request.Properties.IsEmpty()) Document->SetArrayField(TEXT("properties"), Request.Properties);
			return Document;
		}

		void CaptureExpressionIdentity(UMaterialExpression* Expression, FString& OutGuid, FString& OutName)
		{
			if (!Expression) return;
			OutGuid = Expression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
			OutName = Expression->GetName();
		}
	}

	bool RegisterGraphMutationApplyContracts(FString& OutError)
	{
		UeAgentApplyInputContract::ERegistrationResult InputResult;
		if (!UeAgentApplyInputContract::FRegistry::Get().RegisterContract(
			MakeInputContract(), InputResult, OutError)) return false;
		UeAgentApplyWorkspaceContract::ERegistrationResult WorkspaceResult;
		return UeAgentApplyWorkspaceContract::FRegistry::Get().RegisterContract(
			MakeWorkspaceContract(), WorkspaceResult, OutError);
	}

	TSharedRef<IApplyProvider> MakeGraphMutationApplyProvider()
	{
		static const TSharedRef<IReflectionOperationLifecycle> Lifecycle = MakeShared<FGraphMutationLifecycle>();
		UeAgentAuthoringObjectApply::FOptions Options;
		Options.bAllowDomainAuthorizedOwners = true;
		return MakeShared<FReflectionApplyProvider>(GraphMutationApplyProviderId, Lifecycle, Options);
	}

	bool ApplyGraphMutation(const FGraphMutationRequest& Request, FGraphMutationResult& OutResult, const FString& RequestId)
	{
		OutResult = FGraphMutationResult();
		OutResult.Status = TEXT("failed");
		OutResult.AssetPath = NormalizePackagePath(Request.AssetPath);

		FGraphMutationRequest EffectiveRequest = Request;
		if (EffectiveRequest.Kind == EGraphMutationKind::Expression
			&& EffectiveRequest.Operation == EOperation::Create
			&& !EffectiveRequest.NewExpressionGuid.IsValid())
		{
			EffectiveRequest.NewExpressionGuid = FGuid::NewGuid();
		}

		UObject* ExistingOwner = LoadGraphOwner(OutResult.AssetPath);
		if (ExistingOwner)
		{
			FString ResolveError;
			if (EffectiveRequest.Operation != EOperation::Create
				&& EffectiveRequest.Expression.HasAny())
			{
				CaptureExpressionIdentity(
					ResolveExpressionUnique(*ExistingOwner, EffectiveRequest.Expression, ResolveError),
					OutResult.ExpressionGuid,
					OutResult.ExpressionName);
			}
			if (EffectiveRequest.FromExpression.HasAny())
			{
				FString FromName;
				CaptureExpressionIdentity(
					ResolveExpressionUnique(*ExistingOwner, EffectiveRequest.FromExpression, ResolveError),
					OutResult.FromExpressionGuid,
					FromName);
			}
			if (EffectiveRequest.ToExpression.HasAny())
			{
				FString ToName;
				CaptureExpressionIdentity(
					ResolveExpressionUnique(*ExistingOwner, EffectiveRequest.ToExpression, ResolveError),
					OutResult.ToExpressionGuid,
					ToName);
			}
		}

		UeAgentApplyWorkspaceContract::FContract WorkspaceContract;
		if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(
			GraphMutationWorkspaceContractId, WorkspaceContract))
		{
			OutResult.Phase = TEXT("registration");
			OutResult.ErrorCode = TEXT("material_graph_mutation_workspace_contract_missing");
			return false;
		}
		UeAgentAuthoringWorkspaceLoader::FLoadResult LoadResult;
		if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(
			WorkspaceContract,
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
				MakeApplyDocument(EffectiveRequest), TEXT("inline://material-graph-mutation-apply")),
			LoadResult))
		{
			OutResult.Phase = TEXT("load");
			OutResult.ErrorCode = LoadResult.ErrorCode;
			OutResult.ErrorDetail = LoadResult.ErrorDetail;
			return false;
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

		FBackendOptions BackendOptions;
		BackendOptions.TransactionDescription = NSLOCTEXT(
			"UeAgentInterface", "MaterialGraphMutationApply", "Apply Material graph mutation");
		UeAgentAuthoringWorkspaceTransaction::FExecutionOptions ExecutionOptions;
		FOperationFailure RequestFailure;
		if (!BuildApplyEntryExecutionOptions(RequestId, ExecutionOptions, RequestFailure))
		{
			OutResult.Phase = TEXT("preflight");
			OutResult.ErrorCode = RequestFailure.ErrorCode;
			OutResult.ErrorDetail = RequestFailure.ErrorDetail;
			return false;
		}
		ExecutionOptions.bSaveWhenCallerControlled = EffectiveRequest.bSave;
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
		OutResult.ResolvedOperation = OperationToString(EffectiveRequest.Operation == EOperation::Upsert ? EOperation::Update : EffectiveRequest.Operation);
		if (EffectiveRequest.Kind == EGraphMutationKind::Expression && EffectiveRequest.Operation == EOperation::Create)
		{
			UObject* Owner = LoadGraphOwner(OutResult.AssetPath);
			FExpressionSelector Selector;
			Selector.ExpressionGuid = EffectiveRequest.NewExpressionGuid;
			FString ResolveError;
			CaptureExpressionIdentity(
				Owner ? ResolveExpressionUnique(*Owner, Selector, ResolveError) : nullptr,
				OutResult.ExpressionGuid,
				OutResult.ExpressionName);
		}
		return true;
	}
}
