// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Material/Authoring/UeAgentMaterialGraphMutationApplyProvider.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Domains/Material/Authoring/UeAgentMaterialAuthoringCompileAndPersistence.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringDescriptors.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringState.h"
#include "Domains/Material/Authoring/UeAgentMaterialExpressionResolver.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceApply.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace UeAgentMaterialAuthoring
{
	namespace
	{
		using namespace UeAgentApplyInputContract;
		using namespace UeAgentAuthoringOperationApply;

		UE::UAI::Contract::FDiagnostic MutationFailure(const TCHAR* Code, const FString& Reason,
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

		struct FInputBackup
		{
			FExpressionInput* Address = nullptr;
			FExpressionInput Value;
		};

		void SetFailure(FOperationFailure& OutFailure, UE::UAI::Contract::FDiagnostic Diagnostic,
    const FString& Subject = FString()) {
  OutFailure.Diagnostic = MoveTemp(Diagnostic);
  if (OutFailure.Diagnostic.SubjectId.IsEmpty()) OutFailure.Diagnostic.SubjectId = Subject;
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

		bool ReadInt(const TSharedPtr<const FJsonObject>& Object, const TCHAR* Field, const int32 DefaultValue,
			int32& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
		{
			double Value = DefaultValue;
			if (Object.IsValid() && Object->HasField(Field) && !Object->TryGetNumberField(Field, Value))
			{
				OutError = MutationFailure(TEXT("material_integer_number_required"), FString::Printf(TEXT("Field '%s' must be an integer number."), Field),
					Field, Object->TryGetField(Field), MakeShared<FJsonValueString>(TEXT("integer")), Object.Get());
				return false;
			}
			if (!UeAgentPropertyValueCodec::TryConvertInt32(Value, OutValue, OutError))
			{
				OutError.PropertyPath = Field;
				if (Object) UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Object, Field);
				return false;
			}
			return true;
		}

		bool ReadGuid(const TSharedPtr<const FJsonObject>& Object, const TCHAR* Field, FGuid& OutGuid, UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutGuid.Invalidate();
			FString Text;
			if (!ReadString(Object, Field, Text) || Text.IsEmpty()) return true;
			if (FGuid::Parse(Text, OutGuid)) return true;
			OutError = MutationFailure(TEXT("material_expression_guid_invalid"), FString::Printf(TEXT("Field '%s' is not a valid expression GUID."), Field),
				Field, MakeShared<FJsonValueString>(Text), MakeShared<FJsonValueString>(TEXT("GUID")), Object.Get());
			return false;
		}

		bool ReadSelector(
			const TSharedPtr<const FJsonObject>& Object,
			const FString& Prefix,
			FExpressionSelector& OutSelector,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutSelector = FExpressionSelector();
			if (!ReadGuid(Object, *(Prefix + TEXT("guid")), OutSelector.ExpressionGuid, OutError)) return false;
			ReadString(Object, *(Prefix + TEXT("name")), OutSelector.ExpressionName);
			if (!ReadInt(Object, *(Prefix + TEXT("index")), INDEX_NONE, OutSelector.ExpressionIndex, OutError)) return false;
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

		bool CompileOwner(UObject* Owner, UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			if (UMaterial* Material = Cast<UMaterial>(Owner))
			{
				UMaterialEditingLibrary::RecompileMaterial(Material);
				TArray<TSharedPtr<FJsonValue>> Messages;
				int32 ErrorCount = 0;
				CollectMaterialCompileMessages(
					Material,
					ECompileMessageSeverityFilter::Error,
					Messages,
					ErrorCount);
				if (ErrorCount > 0)
				{
					OutError = UeAgentAuthoringDiagnostics::MakeCompileFailure(
						TEXT("material_compile_failed"), Material->GetPathName(), Messages);
					return false;
				}
				return true;
			}
			if (UMaterialFunction* Function = Cast<UMaterialFunction>(Owner))
			{
				UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
				return true;
			}
			OutError = MutationFailure(TEXT("material_compile_target_invalid"), TEXT("Material graph compilation requires a live Material or MaterialFunction."),
				TEXT("target_class"), Owner ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Owner->GetClass()->GetPathName())) : nullptr,
				MakeShared<FJsonValueString>(TEXT("Material or MaterialFunction")));
			OutError.SourceKind = TEXT("native_result");
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
			UE::UAI::Contract::FDiagnostic NativeFailure(const TCHAR* Code, const FString& Reason, const FString& Field) const
			{
				auto Error = MutationFailure(Code, Reason, Field, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				Error.TargetPath = AssetPath;
				Error.SourceKind = TEXT("native_result");
				return Error;
			}
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
					SetFailure(OutFailure, NativeFailure(TEXT("material_graph_owner_missing"),
						TEXT("The material graph owner is no longer available for rollback capture."), TEXT("owner_present")));
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
						SetFailure(OutFailure, NativeFailure(TEXT("material_expression_remove_capture_failed"),
							!Collection ? TEXT("The graph has no expression collection for removal rollback.") : TEXT("The expression to remove is no longer available for rollback capture."),
							!Collection ? TEXT("expression_collection_present") : TEXT("expression_present")));
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
				UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(&SourceDocument.Get(),
					[this](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(SourceLocations, Pointer); });
				ON_SCOPE_EXIT {
					if (!OutFailure.Diagnostic.IsEmpty())
						UeAgentAuthoringDiagnostics::LocateFailure(OutFailure.Diagnostic, *SourceDocument, TEXT("authoring_apply"), AssetPath);
				};
				if (!Owner || !bRollbackCaptured)
				{
					SetFailure(OutFailure, NativeFailure(TEXT("material_graph_mutation_not_captured"),
						!Owner ? TEXT("The material graph owner is no longer available.") : TEXT("Graph mutation rollback state has not been captured."),
						!Owner ? TEXT("owner_present") : TEXT("rollback_captured")));
					return false;
				}
				if (Kind == EGraphMutationKind::Expression)
				{
					if (Operation == EOperation::Create)
					{
						if (!ScratchExpression.IsValid())
						{
							SetFailure(OutFailure, NativeFailure(TEXT("material_expression_seed_missing"),
								TEXT("The prepared expression seed is no longer available for creation."), TEXT("expression_seed_present")));
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
							SetFailure(OutFailure, NativeFailure(TEXT("material_expression_create_failed"),
								!CreatedExpression ? FString::Printf(TEXT("UE could not duplicate expression '%s' of class '%s' into the graph."), *DesiredName.ToString(), *ScratchExpression->GetClass()->GetName())
								    : TEXT("The destination graph has no expression collection."),
								!CreatedExpression ? TEXT("expression_created") : TEXT("expression_collection_present")));
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
							SetFailure(OutFailure, NativeFailure(TEXT("material_expression_remove_target_missing"),
								!Expression ? TEXT("The expression to remove is no longer available.") : TEXT("The graph expression collection is no longer available."),
								!Expression ? TEXT("expression_present") : TEXT("expression_collection_present")));
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
					UE::UAI::Contract::FDiagnostic Error;
					if (!FromExpression || !ToExpression
						|| !ConnectExpressions(*FromExpression, FromOutputName, *ToExpression, ToInputName, Error))
					{
						if (!FromExpression || !ToExpression) Error = NativeFailure(TEXT("connect_expressions_failed"),
							!FromExpression ? TEXT("The source expression is no longer available for connection.") : TEXT("The destination expression is no longer available for connection."),
							!FromExpression ? TEXT("source_expression_present") : TEXT("destination_expression_present"));
						SetFailure(OutFailure, Error);
						return false;
					}
				}
				else
				{
					UE::UAI::Contract::FDiagnostic Error;
					if (!Expression || !ConnectExpressionToProperty(*Expression, FromOutputName, MaterialProperty, Error))
					{
						if (!Expression) Error = NativeFailure(TEXT("connect_expression_failed"),
							TEXT("The source expression is no longer available for the material input connection."), TEXT("expression_present"));
						SetFailure(OutFailure, Error);
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
					SetFailure(OutFailure, NativeFailure(TEXT("material_graph_owner_missing"),
						TEXT("The material graph owner is no longer available for graph refresh."), TEXT("owner_present")));
					return false;
				}
				RefreshGraphOwner(*Owner);
				return true;
			}

			virtual bool Compile(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!bCompile) return true;
				UE::UAI::Contract::FDiagnostic Error;
				if (CompileOwner(Owner, Error)) return true;
				SetFailure(OutFailure, Error);
				return false;
			}

			virtual bool Save(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!bSave) return true;
				UE::UAI::Contract::FDiagnostic Error;
				if (SaveAssetPackage(Owner, Error)) return true;
				SetFailure(OutFailure, Error);
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
				UE::UAI::Contract::FDiagnostic Error;
				if (BuildAssetSnapshotReadRequest(
					Owner,
					AssetPath,
					GetAssetKind(Owner),
					false,
					OutRequests,
					Error)) return true;
				SetFailure(OutFailure, Error);
				return false;
			}

			FString AssetPath;
			TSharedRef<FJsonObject> SourceDocument = MakeShared<FJsonObject>();
			TMap<FString, UE::UAI::Contract::FSourceLocation> SourceLocations;
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
					SetFailure(OutFailure, MutationFailure(TEXT("material_graph_mutation_request_invalid"),
						TEXT("Graph mutation requires a valid mutation contract, target object, and provider_extension object."),
						TEXT("request_valid"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
					return false;
				}

				TUniquePtr<FGraphMutationLifecycleState> State = MakeUnique<FGraphMutationLifecycleState>();
				// Retain original object identity for source lookup; this wrapper never mutates the input objects.
				State->SourceDocument->SetObjectField(TEXT("target"), ConstCastSharedPtr<FJsonObject>(Request.Input->Target));
				State->SourceDocument->SetObjectField(TEXT("provider_extension"), ConstCastSharedPtr<FJsonObject>(Request.Input->ProviderExtension));
				State->SourceLocations = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*State->SourceDocument);
				for (const auto& Input : {Request.Input->Target, Request.Input->ProviderExtension}) {
					const auto Locations = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*Input);
					for (const auto& Pair : Locations) if (!Pair.Key.IsEmpty()) State->SourceLocations.Add(Pair.Key, Pair.Value);
				}
				for (const auto& Alias : TMap<FString, FString>{{TEXT("/from"), TEXT("/from_output_name")}, {TEXT("/to"), TEXT("/to_input_name")}})
					State->SourceLocations.Add(Alias.Key, UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(State->SourceLocations, Alias.Value));
				UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(&State->SourceDocument.Get(),
					[&State](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(State->SourceLocations, Pointer); });
				ON_SCOPE_EXIT {
					if (State && !OutFailure.Diagnostic.IsEmpty())
						UeAgentAuthoringDiagnostics::LocateFailure(OutFailure.Diagnostic, *State->SourceDocument, TEXT("authoring_apply"), State->AssetPath);
				};
				ReadString(Request.Input->Target, TEXT("asset_path"), State->AssetPath);
				State->AssetPath = NormalizePackagePath(State->AssetPath);
				if (!FPackageName::IsValidLongPackageName(State->AssetPath)
					|| (!State->AssetPath.StartsWith(TEXT("/Game/")) && !State->AssetPath.StartsWith(TEXT("/UeAgentInterface/Materials/"))))
				{
					SetFailure(OutFailure, MutationFailure(TEXT("invalid_asset_path"), TEXT("Material graph asset_path must be a valid package under /Game or /UeAgentInterface/Materials."),
						TEXT("asset_path"), Request.Input->Target->TryGetField(TEXT("asset_path")),
						MakeShared<FJsonValueString>(TEXT("/Game/<folder>/<asset> or /UeAgentInterface/Materials/<asset>")), Request.Input->Target.Get()));
					return false;
				}
				State->Owner = LoadGraphOwner(State->AssetPath);
				if (!State->Owner)
				{
					SetFailure(OutFailure, MutationFailure(TEXT("material_graph_owner_not_found"),
						FString::Printf(TEXT("Material graph asset '%s' could not be loaded as a Material or MaterialFunction."), *State->AssetPath),
						TEXT("asset_path"), MakeShared<FJsonValueString>(State->AssetPath),
						MakeShared<FJsonValueString>(TEXT("existing Material or MaterialFunction")), Request.Input->Target.Get()));
					return false;
				}

				FString KindText;
				ReadString(Request.Input->ProviderExtension, TEXT("mutation_kind"), KindText);
				if (!ParseKind(KindText, State->Kind))
				{
					SetFailure(OutFailure, MutationFailure(TEXT("material_graph_mutation_kind_invalid"),
						TEXT("mutation_kind must be expression, connection, or root_input."), TEXT("mutation_kind"),
						MakeShared<FJsonValueString>(KindText), MakeShared<FJsonValueString>(TEXT("expression, connection, root_input")), Request.Input->ProviderExtension.Get()));
					return false;
				}
				State->Operation = Request.Input->RequestedOperation == EOperation::Upsert
					? EOperation::Update
					: Request.Input->RequestedOperation;
				State->bCompile = ReadBool(Request.Input->ProviderExtension, TEXT("compile"), true);
				State->bSave = ReadBool(Request.Input->ProviderExtension, TEXT("save"), false);
				UE::UAI::Contract::FDiagnostic Error;
				FExpressionSelector ExpressionSelector;
				FExpressionSelector FromSelector;
				FExpressionSelector ToSelector;
				const auto ResolveSelected = [&](const FExpressionSelector& Selector, const FString& Prefix) {
					const auto OriginalSources = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*Request.Input->Target);
					auto SelectorDocument = MakeShared<FJsonObject>();
					TMap<FString, UE::UAI::Contract::FSourceLocation> SelectorSources;
					SelectorSources.Add(FString(), UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(OriginalSources, FString()));
					for (const TCHAR* Field : {TEXT("guid"), TEXT("name"), TEXT("index")}) {
						const FString SelectorField = TEXT("expression_") + FString(Field);
						if (const auto Value = Request.Input->Target->TryGetField(Prefix + Field)) SelectorDocument->SetField(SelectorField, Value);
						SelectorSources.Add(TEXT("/") + SelectorField,
							UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(OriginalSources, TEXT("/") + Prefix + Field));
					}
					UeAgentAuthoringDiagnostics::FScopedSourceLocations SelectorScope(&SelectorDocument.Get(),
						[&SelectorSources](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(SelectorSources, Pointer); });
					auto* Result = ResolveExpressionUnique(*State->Owner, Selector, Error);
					if (!Result) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *SelectorDocument);
					return Result;
				};
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
						SetFailure(OutFailure, MutationFailure(TEXT("material_expression_operation_unsupported"),
							TEXT("Expression mutation supports create, update, or remove operations."), TEXT("operation"),
							MakeShared<FJsonValueString>(State->Operation == EOperation::Clone ? TEXT("clone") : TEXT("unsupported operation")),
							MakeShared<FJsonValueString>(TEXT("create, update, remove"))));
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
							SetFailure(OutFailure, Error);
							return false;
						}
						int32 PositionX, PositionY;
						if (!ReadInt(Request.Input->ProviderExtension, TEXT("node_pos_x"), 0, PositionX, Error)
							|| !ReadInt(Request.Input->ProviderExtension, TEXT("node_pos_y"), 0, PositionY, Error))
						{ SetFailure(OutFailure, Error); return false; }
						UMaterialExpression* ScratchExpression = CreateExpression(
							*State->ScratchOwner,
							ResolveResult.ExpressionClass,
							PositionX, PositionY);
						State->ScratchExpression.Reset(ScratchExpression);
						if (!ScratchExpression)
						{
							SetFailure(OutFailure, State->NativeFailure(TEXT("material_expression_seed_create_failed"),
								FString::Printf(TEXT("UE could not create an expression seed of class '%s' at (%d, %d)."),
									*GetNameSafe(ResolveResult.ExpressionClass), PositionX, PositionY), TEXT("expression_seed_created")));
							return false;
						}
						if (ResolveResult.MaterialFunction)
						{
							UMaterialExpressionMaterialFunctionCall* FunctionCall =
								Cast<UMaterialExpressionMaterialFunctionCall>(ScratchExpression);
							if (!FunctionCall || !FunctionCall->SetMaterialFunction(ResolveResult.MaterialFunction))
							{
								SetFailure(OutFailure, MutationFailure(TEXT("material_function_assignment_failed"),
									FString::Printf(TEXT("Expression '%s' could not accept material function '%s': %s."),
										*ScratchExpression->GetClass()->GetName(), *ResolveResult.MaterialFunction->GetName(),
										FunctionCall ? TEXT("UE rejected the assignment") : TEXT("the expression is not a function call")),
									TEXT("material_function_path"), MakeShared<FJsonValueString>(ResolveResult.MaterialFunction->GetPathName()),
									MakeShared<FJsonValueString>(TEXT("function accepted by a function-call expression")), Request.Input->ProviderExtension.Get()));
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
						UE::UAI::Contract::FDiagnostic ResolveError;
						if (ResolveExpressionUnique(*State->Owner, GuidSelector, ResolveError))
						{
							SetFailure(OutFailure, MutationFailure(TEXT("material_expression_guid_conflict"),
								TEXT("new_expression_guid is already assigned to another expression in this graph."), TEXT("new_expression_guid"),
								MakeShared<FJsonValueString>(State->NewExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower)),
								MakeShared<FJsonValueString>(TEXT("unused expression GUID")), Request.Input->ProviderExtension.Get()));
							return false;
						}
						if (!ResolveError.IsEmpty() && ResolveError.Code != TEXT("material_expression_not_found")) {
							UeAgentAuthoringDiagnostics::LocateAuthoringSource(ResolveError, *Request.Input->ProviderExtension, TEXT("new_expression_guid"));
							SetFailure(OutFailure, ResolveError); return false;
						}
						ScratchExpression->MaterialExpressionGuid = State->NewExpressionGuid;
						ReadString(Request.Input->ProviderExtension, TEXT("suggested_name"), State->SuggestedName);
					}
					else
					{
						State->Expression = ResolveSelected(ExpressionSelector, TEXT("expression_"));
						if (!State->Expression)
						{
							SetFailure(OutFailure, Error);
							return false;
						}
						if (State->Operation == EOperation::Remove && Request.Input->HasProperties())
						{
							SetFailure(OutFailure, MutationFailure(TEXT("material_expression_remove_properties_forbidden"),
								TEXT("An expression remove operation cannot contain property edits."), TEXT("properties"),
								MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)));
							return false;
						}
					}
				}
				else if (State->Kind == EGraphMutationKind::Connection)
				{
					if (Request.Input->HasProperties())
					{
						SetFailure(OutFailure, MutationFailure(TEXT("material_connection_properties_forbidden"),
							TEXT("A connection mutation cannot contain property edits."), TEXT("properties"),
							MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)));
						return false;
					}
					State->FromExpression = ResolveSelected(FromSelector, TEXT("from_expression_"));
					if (!State->FromExpression)
					{
						SetFailure(OutFailure, Error);
						return false;
					}
					State->ToExpression = ResolveSelected(ToSelector, TEXT("to_expression_"));
					if (!State->ToExpression)
					{
						SetFailure(OutFailure, Error);
						return false;
					}
					ReadString(Request.Input->ProviderExtension, TEXT("from_output_name"), State->FromOutputName);
					ReadString(Request.Input->ProviderExtension, TEXT("to_input_name"), State->ToInputName);
					int32 OutputIndex = INDEX_NONE;
					if (!ResolveExpressionOutputIndex(*State->FromExpression, State->FromOutputName, OutputIndex))
					{
						SetFailure(OutFailure, DescribeExpressionPinFailure(
							*State->FromExpression, State->FromOutputName, false, TEXT("from_output_name"))); return false;
					}
					if (!ResolveExpressionInput(*State->ToExpression, State->ToInputName, State->MutatedInput))
					{
						SetFailure(OutFailure, DescribeExpressionPinFailure(
							*State->ToExpression, State->ToInputName, true, TEXT("to_input_name"))); return false;
					}
				}
				else
				{
					if (Request.Input->HasProperties() || !Cast<UMaterial>(State->Owner))
					{
						SetFailure(OutFailure, MutationFailure(Request.Input->HasProperties()
							? TEXT("material_root_input_properties_forbidden")
							: TEXT("material_function_root_inputs_not_allowed"),
							Request.Input->HasProperties() ? TEXT("A root-input mutation cannot contain property edits.") : TEXT("Material root inputs require a Material owner, not a MaterialFunction."),
							Request.Input->HasProperties() ? TEXT("properties") : TEXT("asset_path"),
							MakeShared<FJsonValueString>(Request.Input->HasProperties() ? TEXT("property edits present") : GetAssetKind(State->Owner)),
							MakeShared<FJsonValueString>(Request.Input->HasProperties() ? TEXT("no property edits") : TEXT("Material"))));
						return false;
					}
					State->Expression = ResolveSelected(ExpressionSelector, TEXT("expression_"));
					if (!State->Expression)
					{
						SetFailure(OutFailure, Error);
						return false;
					}
					FString PropertyName;
					ReadString(Request.Input->ProviderExtension, TEXT("material_property"), PropertyName);
					if (!ParseMaterialProperty(PropertyName, State->MaterialProperty))
					{
						SetFailure(OutFailure, MutationFailure(TEXT("invalid_material_property"),
							FString::Printf(TEXT("Material root property '%s' is not supported."), *PropertyName), TEXT("material_property"),
							MakeShared<FJsonValueString>(PropertyName), MakeShared<FJsonValueString>(TEXT("supported material root property")), Request.Input->ProviderExtension.Get()));
						return false;
					}
					ReadString(Request.Input->ProviderExtension, TEXT("from_output_name"), State->FromOutputName);
					int32 OutputIndex = INDEX_NONE;
					State->MutatedInput = CastChecked<UMaterial>(State->Owner)->GetExpressionInputForProperty(State->MaterialProperty);
					if (!State->MutatedInput)
					{
						TArray<TSharedPtr<FJsonValue>> Available;
						for (const auto Property : GetSupportedMaterialProperties())
							if (CastChecked<UMaterial>(State->Owner)->GetExpressionInputForProperty(Property))
								Available.Add(MakeShared<FJsonValueString>(MaterialPropertyToString(Property)));
						SetFailure(OutFailure, UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("material_root_input_unavailable"), TEXT("/material_property"),
								MakeShared<FJsonValueArray>(Available), MakeShared<FJsonValueString>(PropertyName), TEXT("authoring_apply"),
								TEXT("The current material does not expose the requested root input."))); return false;
					}
					if (!ResolveExpressionOutputIndex(*State->Expression, State->FromOutputName, OutputIndex))
					{
						SetFailure(OutFailure, DescribeExpressionPinFailure(
							*State->Expression, State->FromOutputName, false, TEXT("from_output_name"))); return false;
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
			Result.PropertyPayload.Mode = TEXT("reflection");
			Result.PropertyPayload.PropertyPathField = TEXT("property_path");
			Result.PropertyPayload.ValueField = TEXT("value");
			Result.PropertyPayload.OptionalAssertionFields = {
				TEXT("kind"), TEXT("struct_path"), TEXT("enum_path"), TEXT("element_type"),
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

	bool RegisterGraphMutationApplyContracts(UE::UAI::Contract::FDiagnostic& OutError)
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
			UE::UAI::Contract::FDiagnostic ResolveError;
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
			OutResult.Diagnostic = MutationFailure(TEXT("material_graph_mutation_workspace_contract_missing"),
				TEXT("The material graph mutation workspace contract is not registered."), TEXT("workspace_contract_registered"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutResult.Diagnostic.Stage = OutResult.Phase;
			OutResult.Diagnostic.SourceKind = TEXT("native_result");
			OutResult.Diagnostic.InternalContext = GraphMutationWorkspaceContractId;
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
			OutResult.Diagnostic = LoadResult.Diagnostic;
			return false;
		}
		FBackendOptions BackendOptions;
		BackendOptions.TransactionDescription = NSLOCTEXT(
			"UeAgentInterface", "MaterialGraphMutationApply", "Apply Material graph mutation");
		FApplyWorkspaceResult ApplyResult;
		UeAgentAuthoringAssetWorkspaceApply::FFailure ApplyFailure;
		const bool bApplied = UeAgentAuthoringAssetWorkspaceApply::Execute(
			WorkspaceContract,
			LoadResult.Workspace,
			FProviderRegistry::Get(),
			RequestId,
			EffectiveRequest.bSave,
			BackendOptions,
			ApplyResult,
			ApplyFailure);
		OutResult.bRollbackAttempted = ApplyResult.Execution.bRollbackAttempted;
		OutResult.bRollbackSucceeded = ApplyResult.Execution.bRollbackSucceeded;
		if (!bApplied)
		{
			OutResult.Phase = ApplyFailure.Phase;
			OutResult.Diagnostic = ApplyFailure.Diagnostic;
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
			UE::UAI::Contract::FDiagnostic ResolveError;
			CaptureExpressionIdentity(
				Owner ? ResolveExpressionUnique(*Owner, Selector, ResolveError) : nullptr,
				OutResult.ExpressionGuid,
				OutResult.ExpressionName);
		}
		return true;
	}
}
