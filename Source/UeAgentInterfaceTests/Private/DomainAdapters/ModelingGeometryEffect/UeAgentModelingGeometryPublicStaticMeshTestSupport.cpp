#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryPublicStaticMeshTestSupport.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Composition/SimpleAuthoring/Families/Modeling/UeAgentModelingSimpleAuthoringProduction.h"
#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"
#include "Core/StateSemantic/UeAgentStateRevisionJson.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCarrierRevision.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMeshToMeshDescription.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshAttributes.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh
{
	using namespace UE::Geometry;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::DomainAdapters::ModelingGeometryEffect;
	using namespace UE::UAI::StateSemantic;

	static_assert(
		UAI_MODELING_SIMPLE_AUTHORING_ARTIFACT_READBACK_V1 == 1,
		"Modeling artifact readback contract changed");

	namespace
	{
		void BuildGroupedCube(FDynamicMesh3& Mesh)
		{
			Mesh.Clear();
			Mesh.EnableTriangleGroups();
			const int32 V0 = Mesh.AppendVertex(FVector3d(-50.0, -50.0, -50.0));
			const int32 V1 = Mesh.AppendVertex(FVector3d(50.0, -50.0, -50.0));
			const int32 V2 = Mesh.AppendVertex(FVector3d(50.0, 50.0, -50.0));
			const int32 V3 = Mesh.AppendVertex(FVector3d(-50.0, 50.0, -50.0));
			const int32 V4 = Mesh.AppendVertex(FVector3d(-50.0, -50.0, 50.0));
			const int32 V5 = Mesh.AppendVertex(FVector3d(50.0, -50.0, 50.0));
			const int32 V6 = Mesh.AppendVertex(FVector3d(50.0, 50.0, 50.0));
			const int32 V7 = Mesh.AppendVertex(FVector3d(-50.0, 50.0, 50.0));
			const FIndex3i Triangles[] = {
				{V0, V2, V1}, {V0, V3, V2},
				{V4, V5, V6}, {V4, V6, V7},
				{V0, V1, V5}, {V0, V5, V4},
				{V1, V2, V6}, {V1, V6, V5},
				{V2, V3, V7}, {V2, V7, V6},
				{V3, V0, V4}, {V3, V4, V7},
			};
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(Triangles); ++Index)
			{
				Mesh.AppendTriangle(Triangles[Index], Index / 2);
			}
			Mesh.ReverseOrientation(false);
			Mesh.EnableAttributes();
			Mesh.Attributes()->SetNumUVLayers(1);
		}
	}

	FScopedPublicStaticMesh::FScopedPublicStaticMesh(const FString& Scope)
	{
		const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		AssetName = TEXT("SM_") + Scope + TEXT("_") + Suffix;
		PackageName = TEXT("/Game/UAI/Tests/ModelingGeometry/") + AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		HeaderPath = FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension());
	}

	FScopedPublicStaticMesh::~FScopedPublicStaticMesh()
	{
		Cleanup();
	}

	UStaticMesh* FScopedPublicStaticMesh::Create()
	{
		UPackage* Package = CreatePackage(*PackageName);
		UStaticMesh* StaticMesh = Package
			? NewObject<UStaticMesh>(
				Package,
				*AssetName,
				RF_Public | RF_Standalone | RF_Transactional)
			: nullptr;
		if (!StaticMesh)
		{
			return nullptr;
		}
		FDynamicMesh3 Mesh;
		BuildGroupedCube(Mesh);
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		FDynamicMeshToMeshDescription Converter;
		Converter.Convert(&Mesh, Description);
		StaticMesh->SetNumSourceModels(1);
		StaticMesh->CreateMeshDescription(0, Description);
		StaticMesh->CommitMeshDescription(0);
		if (!StaticMesh->GetBodySetup())
		{
			StaticMesh->CreateBodySetup();
		}
		FAssetRegistryModule::AssetCreated(StaticMesh);
		return Save(*StaticMesh) ? StaticMesh : nullptr;
	}

	bool FScopedPublicStaticMesh::Save(UStaticMesh& StaticMesh) const
	{
		UPackage* const Package = StaticMesh.GetOutermost();
		if (!Package || Package->GetName() != PackageName)
		{
			return false;
		}
		Package->MarkPackageDirty();
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(
			Package,
			&StaticMesh,
			*HeaderPath,
			SaveArgs);
		if (bSaved)
		{
			Package->SetDirtyFlag(false);
		}
		return bSaved;
	}

	UStaticMesh* FScopedPublicStaticMesh::ReloadFromDisk(
		bool& bOutOriginalCollected)
	{
		bOutOriginalCollected = false;
		UStaticMesh* Original = FindLoaded();
		const TWeakObjectPtr<UStaticMesh> OriginalHandle(Original);
		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (!Original || !Package)
		{
			return nullptr;
		}
		Package->SetDirtyFlag(false);
		FText UnloadError;
		if (!UPackageTools::UnloadPackages({Package}, UnloadError, true))
		{
			return nullptr;
		}
		CollectGarbage(RF_NoFlags, true);
		bOutOriginalCollected = !OriginalHandle.IsValid();
		UPackage* ReloadedPackage = LoadPackage(nullptr, *PackageName, LOAD_None);
		UStaticMesh* Reloaded = ReloadedPackage ? FindLoaded() : nullptr;
		return bOutOriginalCollected && Reloaded ? Reloaded : nullptr;
	}

	UStaticMesh* FScopedPublicStaticMesh::FindLoaded() const
	{
		return Cast<UStaticMesh>(StaticFindObject(
			UStaticMesh::StaticClass(),
			nullptr,
			*ObjectPath));
	}

	FString FScopedPublicStaticMesh::FileHash() const
	{
		return IFileManager::Get().FileExists(*HeaderPath)
			? LexToString(FMD5Hash::HashFile(*HeaderPath)).ToLower()
			: FString();
	}

	void FScopedPublicStaticMesh::Cleanup()
	{
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			Package->SetDirtyFlag(false);
			FText UnloadError;
			UPackageTools::UnloadPackages({Package}, UnloadError, true);
			CollectGarbage(RF_NoFlags, true);
		}
		for (const FString& Path : {
			HeaderPath,
			FPaths::ChangeExtension(HeaderPath, TEXT("uexp")),
			FPaths::ChangeExtension(HeaderPath, TEXT("ubulk")),
			FPaths::ChangeExtension(HeaderPath, TEXT("uptnl")),
			FPaths::ChangeExtension(HeaderPath, TEXT("upayload"))})
		{
			IFileManager::Get().Delete(*Path, false, true, true);
		}
	}

	TSharedPtr<FJsonObject> ReadRevisionJson(
		const FString& TargetRef,
		UObject& Target)
	{
		TOptional<FUeAgentStateRevision> Revision;
		ExecutionMutation::FExecutionMutationError Error;
		if (!TryReadModelingGeometryCarrierRevision(
				TargetRef,
				Target,
				Revision,
				Error)
			|| !Revision.IsSet())
		{
			return nullptr;
		}
		return FUeAgentStateRevisionJson::ToJson(Revision.GetValue());
	}

	FString ReadCommandErrorCode(const FUeAgentCommandResult& Result)
	{
		TSharedPtr<FJsonObject> Error;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Result.GetErrorJson());
		FString Code;
		return FJsonSerializer::Deserialize(Reader, Error)
			&& Error.IsValid()
			&& Error->TryGetStringField(TEXT("code"), Code)
			? Code
			: FString();
	}

	TSharedRef<FJsonObject> BuildTargetLocalSpaceContract()
	{
		const TSharedRef<FJsonObject> Space = MakeShared<FJsonObject>();
		Space->SetStringField(
			TEXT("semantic_frame_id"),
			TEXT("agent.mesh.intent"));
		Space->SetStringField(
			TEXT("write_frame_id"),
			TEXT("ue.mesh.target_local"));
		Space->SetStringField(
			TEXT("read_frame_id"),
			TEXT("ue.mesh.target_local"));
		Space->SetStringField(
			TEXT("verify_frame_id"),
			TEXT("agent.mesh.intent"));
		Space->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Space->SetStringField(
			TEXT("basis_evidence"),
			TEXT("exact carrier transform and geometry revision"));
		Space->SetStringField(
			TEXT("conversion_chain"),
			TEXT(
				"semantic -> C4 normalized target local -> working mesh -> "
				"carrier -> independent target-local readback"));
		return Space;
	}

	TSharedRef<FJsonObject> BuildTargetLocalPlane(
		const FVector3d& Origin,
		const FVector3d& Normal)
	{
		auto VectorValues = [](const FVector3d& Value)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Add(MakeShared<FJsonValueNumber>(Value.X));
			Values.Add(MakeShared<FJsonValueNumber>(Value.Y));
			Values.Add(MakeShared<FJsonValueNumber>(Value.Z));
			return Values;
		};
		const TSharedRef<FJsonObject> Plane = MakeShared<FJsonObject>();
		Plane->SetStringField(TEXT("frame_id"), TEXT("ue.mesh.target_local"));
		Plane->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Plane->SetArrayField(TEXT("origin"), VectorValues(Origin));
		Plane->SetArrayField(TEXT("normal"), VectorValues(Normal));
		Plane->SetStringField(
			TEXT("basis_evidence"),
			TEXT("exact target-local plane from owned carrier geometry"));
		return Plane;
	}

	bool ReplaceStaticMeshGeometry(
		UStaticMesh& StaticMesh,
		const FDynamicMesh3& Mesh)
	{
		if (Mesh.VertexCount() == 0 || Mesh.TriangleCount() == 0)
		{
			return false;
		}
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		FDynamicMeshToMeshDescription Converter;
		Converter.Convert(&Mesh, Description);
		StaticMesh.CreateMeshDescription(0, Description);
		StaticMesh.CommitMeshDescription(0);
		return StaticMesh.GetMeshDescription(0) != nullptr;
	}

	bool IsVerifiedIsolatedPackageOutcome(const FString& PayloadJson)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(PayloadJson);
		const TSharedPtr<FJsonObject>* Projection = nullptr;
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* EvidenceIds = nullptr;
		FString Schema;
		FString Type;
		FString Status;
		FString EffectOutcome;
		if (!FJsonSerializer::Deserialize(Reader, Root)
			|| !Root.IsValid()
			|| !Root->TryGetObjectField(TEXT("projection"), Projection)
			|| !Projection || !Projection->IsValid()
			|| !(*Projection)->TryGetObjectField(TEXT("payload"), Payload)
			|| !Payload || !Payload->IsValid()
			|| !(*Payload)->TryGetStringField(TEXT("$schema"), Schema)
			|| !(*Payload)->TryGetStringField(TEXT("$type"), Type)
			|| !(*Payload)->TryGetStringField(TEXT("status"), Status)
			|| !(*Payload)->TryGetStringField(
				TEXT("effect_outcome"),
				EffectOutcome)
			|| !(*Payload)->TryGetArrayField(TEXT("evidence_ids"), EvidenceIds)
			|| !EvidenceIds)
		{
			return false;
		}
		const bool bHasIsolatedPackageEvidence = EvidenceIds->ContainsByPredicate(
			[](const TSharedPtr<FJsonValue>& Value)
			{
				return Value.IsValid()
					&& Value->Type == EJson::String
					&& Value->AsString().StartsWith(
						TEXT("modeling:persisted-package-instance:"));
			});
		return Schema == TEXT("uai.c9.canonical_outcome_view.v1")
			&& Type == TEXT("verification_outcome")
			&& Status == TEXT("verified")
			&& EffectOutcome == TEXT("complete")
			&& bHasIsolatedPackageEvidence;
	}

	bool RunCurrentStaticMeshProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& CommandId,
		const FString& Scope,
		const FCarrierStep& PrepareCarrier,
		const FBuildParams& BuildParams,
		const FCarrierStep& VerifyBefore,
		const FCarrierStep& VerifyAfter)
	{
		FString OwnedHeaderPath;
		{
			FScopedPublicStaticMesh Owned(Scope);
			OwnedHeaderPath = Owned.HeaderPath;
			UStaticMesh* StaticMesh = Owned.Create();
			if (!Test.TestNotNull(
					*(Scope + TEXT(" creates an owned asset")),
					StaticMesh))
			{
				return false;
			}
			if (!Test.TestTrue(
					*(Scope + TEXT(" prepares the exact baseline carrier")),
					PrepareCarrier(*StaticMesh) && Owned.Save(*StaticMesh)))
			{
				return false;
			}
			if (!Test.TestTrue(
					*(Scope + TEXT(" verifies the exact initial carrier state")),
					VerifyBefore(*StaticMesh)))
			{
				return false;
			}

			const TSharedPtr<FJsonObject> Revision = ReadRevisionJson(
				Owned.ObjectPath,
				*StaticMesh);
			if (!Test.TestNotNull(
					*(Scope + TEXT(" reads the carrier revision")),
					Revision.Get()))
			{
				return false;
			}
			TOptional<FUeAgentStateRevision> BeforeRevision;
			FUeAgentStateRevisionJson::TryParse(Revision, BeforeRevision);
			const FUeAgentCommandResult Result =
				UE::UAI::Tests::PublicCommand::SubmitActiveCommand(
					Registry,
					Dispatch,
					CommandId,
					BuildParams(Owned.ObjectPath, Revision.ToSharedRef()),
					Scope);
			if (!Test.TestTrue(
					*(Scope + TEXT(" public operation succeeds")),
					Result.IsOk()))
			{
				Test.AddError(Result.GetErrorJson());
				return false;
			}
			Test.TestTrue(
				*(Scope + TEXT(" C9 names isolated persisted-package readback")),
				IsVerifiedIsolatedPackageOutcome(Result.GetPayloadJson()));
			Test.TestTrue(
				*(Scope + TEXT(" durable package header exists")),
				IFileManager::Get().FileExists(*Owned.HeaderPath));

			Composition::SimpleAuthoring::OperationHost::
				SetAutomationWorldOverride(&World, true);
			bool bOriginalCollected = false;
			UStaticMesh* Reloaded = Owned.ReloadFromDisk(bOriginalCollected);
			Test.TestTrue(
				*(Scope + TEXT(" unloads the source package without a retained product object")),
				bOriginalCollected);
			if (Test.TestNotNull(
					*(Scope + TEXT(" reloads the exact package from disk")),
					Reloaded))
			{
				Test.TestTrue(
					*(Scope + TEXT(" independently verifies the persisted carrier state")),
					VerifyAfter(*Reloaded));
				TOptional<FUeAgentStateRevision> ReloadedRevision;
				ExecutionMutation::FExecutionMutationError RevisionError;
				Test.TestTrue(
					*(Scope + TEXT(" reloads an exact semantic revision")),
					TryReadModelingGeometryCarrierRevision(
						Owned.ObjectPath,
						*Reloaded,
						ReloadedRevision,
						RevisionError)
						&& ReloadedRevision.IsSet());
				Test.TestTrue(
					*(Scope + TEXT(" advances the persisted carrier revision")),
					BeforeRevision.IsSet()
						&& ReloadedRevision.IsSet()
						&& BeforeRevision.GetValue()
							!= ReloadedRevision.GetValue());
			}
		}
		Test.TestFalse(
			*(Scope + TEXT(" removes its package file")),
			IFileManager::Get().FileExists(*OwnedHeaderPath));
		return !Test.HasAnyErrors();
	}

	bool RunCurrentStaticMeshRejectedCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& CommandId,
		const FString& Scope,
		const FString& ExpectedErrorCode,
		const FCarrierStep& PrepareCarrier,
		const FBuildParams& BuildParams,
		const FCarrierStep& VerifyBefore,
		const FCarrierStep& VerifyUnchanged)
	{
		FString OwnedHeaderPath;
		{
			FScopedPublicStaticMesh Owned(Scope);
			OwnedHeaderPath = Owned.HeaderPath;
			UStaticMesh* StaticMesh = Owned.Create();
			if (!Test.TestNotNull(
					*(Scope + TEXT(" creates an owned asset")),
					StaticMesh))
			{
				return false;
			}
			if (!Test.TestTrue(
					*(Scope + TEXT(" prepares the exact baseline carrier")),
					PrepareCarrier(*StaticMesh) && Owned.Save(*StaticMesh))
				|| !Test.TestTrue(
					*(Scope + TEXT(" verifies the exact initial carrier state")),
					VerifyBefore(*StaticMesh)))
			{
				return false;
			}

			const TSharedPtr<FJsonObject> Revision = ReadRevisionJson(
				Owned.ObjectPath,
				*StaticMesh);
			if (!Test.TestNotNull(
					*(Scope + TEXT(" reads the carrier revision")),
					Revision.Get()))
			{
				return false;
			}
			TOptional<FUeAgentStateRevision> RevisionBefore;
			FUeAgentStateRevisionJson::TryParse(Revision, RevisionBefore);
			const FString FileHashBefore = Owned.FileHash();
			const bool bPackageDirtyBefore =
				StaticMesh->GetOutermost()->IsDirty();

			const FUeAgentCommandResult Result =
				UE::UAI::Tests::PublicCommand::SubmitActiveCommand(
					Registry,
					Dispatch,
					CommandId,
					BuildParams(Owned.ObjectPath, Revision.ToSharedRef()),
					Scope);
			Test.TestFalse(
				*(Scope + TEXT(" public operation is rejected")),
				Result.IsOk());
			Test.TestEqual(
				*(Scope + TEXT(" preserves the exact domain error")),
				ReadCommandErrorCode(Result),
				ExpectedErrorCode);
			Test.TestEqual(
				*(Scope + TEXT(" leaves the package file unchanged")),
				Owned.FileHash(),
				FileHashBefore);
			Test.TestEqual(
				*(Scope + TEXT(" leaves the package dirty state unchanged")),
				StaticMesh->GetOutermost()->IsDirty(),
				bPackageDirtyBefore);
			Test.TestTrue(
				*(Scope + TEXT(" leaves the live carrier unchanged")),
				VerifyUnchanged(*StaticMesh));
			TOptional<FUeAgentStateRevision> LiveRevisionAfter;
			ExecutionMutation::FExecutionMutationError RevisionError;
			Test.TestTrue(
				*(Scope + TEXT(" leaves the live carrier revision unchanged")),
				TryReadModelingGeometryCarrierRevision(
					Owned.ObjectPath,
					*StaticMesh,
					LiveRevisionAfter,
					RevisionError)
					&& RevisionBefore.IsSet()
					&& LiveRevisionAfter.IsSet()
					&& LiveRevisionAfter.GetValue()
						== RevisionBefore.GetValue());

			Composition::SimpleAuthoring::OperationHost::
				SetAutomationWorldOverride(&World, true);
			bool bOriginalCollected = false;
			UStaticMesh* Reloaded = Owned.ReloadFromDisk(bOriginalCollected);
			Test.TestTrue(
				*(Scope + TEXT(" unloads the rejected source package")),
				bOriginalCollected);
			if (Test.TestNotNull(
					*(Scope + TEXT(" reloads the rejected package from disk")),
					Reloaded))
			{
				Test.TestTrue(
					*(Scope + TEXT(" preserves the persisted carrier")),
					VerifyUnchanged(*Reloaded));
				TOptional<FUeAgentStateRevision> ReloadedRevision;
				RevisionError = {};
				Test.TestTrue(
					*(Scope + TEXT(" preserves the persisted carrier revision")),
					TryReadModelingGeometryCarrierRevision(
						Owned.ObjectPath,
						*Reloaded,
						ReloadedRevision,
						RevisionError)
						&& RevisionBefore.IsSet()
						&& ReloadedRevision.IsSet()
						&& ReloadedRevision.GetValue()
							== RevisionBefore.GetValue());
			}
		}
		Test.TestFalse(
			*(Scope + TEXT(" removes its package file")),
			IFileManager::Get().FileExists(*OwnedHeaderPath));
		return !Test.HasAnyErrors();
	}

	bool RunCurrentStaticMeshOperandProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& CommandId,
		const FString& Scope,
		const FCarrierStep& PrepareTarget,
		const FCarrierStep& PrepareOperand,
		const FBuildOperandParams& BuildParams,
		const FCarrierStep& VerifyTargetBefore,
		const FCarrierStep& VerifyOperandBefore,
		const FCarrierStep& VerifyTargetAfter,
		const FCarrierStep& VerifyOperandAfter)
	{
		FString TargetHeaderPath;
		FString OperandHeaderPath;
		{
			FScopedPublicStaticMesh Target(Scope + TEXT("Target"));
			FScopedPublicStaticMesh Operand(Scope + TEXT("Operand"));
			TargetHeaderPath = Target.HeaderPath;
			OperandHeaderPath = Operand.HeaderPath;
			UStaticMesh* TargetMesh = Target.Create();
			UStaticMesh* OperandMesh = Operand.Create();
			if (!Test.TestNotNull(
					*(Scope + TEXT(" creates the target asset")),
					TargetMesh)
				|| !Test.TestNotNull(
					*(Scope + TEXT(" creates the operand asset")),
					OperandMesh))
			{
				return false;
			}
			TStrongObjectPtr<UStaticMesh> OperandKeepAlive(OperandMesh);
			if (!Test.TestTrue(
					*(Scope + TEXT(" prepares both exact carriers")),
					PrepareTarget(*TargetMesh)
						&& Target.Save(*TargetMesh)
						&& PrepareOperand(*OperandMesh)
						&& Operand.Save(*OperandMesh)))
			{
				return false;
			}
			if (!Test.TestTrue(
					*(Scope + TEXT(" verifies the target baseline")),
					VerifyTargetBefore(*TargetMesh))
				|| !Test.TestTrue(
					*(Scope + TEXT(" verifies the operand baseline")),
					VerifyOperandBefore(*OperandMesh)))
			{
				return false;
			}

			const TSharedPtr<FJsonObject> TargetRevision = ReadRevisionJson(
				Target.ObjectPath,
				*TargetMesh);
			const TSharedPtr<FJsonObject> OperandRevision = ReadRevisionJson(
				Operand.ObjectPath,
				*OperandMesh);
			if (!Test.TestNotNull(
					*(Scope + TEXT(" reads the target revision")),
					TargetRevision.Get())
				|| !Test.TestNotNull(
					*(Scope + TEXT(" reads the operand revision")),
					OperandRevision.Get()))
			{
				return false;
			}
			TOptional<FUeAgentStateRevision> TargetRevisionBefore;
			TOptional<FUeAgentStateRevision> OperandRevisionBefore;
			FUeAgentStateRevisionJson::TryParse(
				TargetRevision,
				TargetRevisionBefore);
			FUeAgentStateRevisionJson::TryParse(
				OperandRevision,
				OperandRevisionBefore);
			const FString OperandFileHashBefore = Operand.FileHash();

			const FUeAgentCommandResult Result =
				UE::UAI::Tests::PublicCommand::SubmitActiveCommand(
					Registry,
					Dispatch,
					CommandId,
					BuildParams(
						Target.ObjectPath,
						TargetRevision.ToSharedRef(),
						Operand.ObjectPath,
						OperandRevision.ToSharedRef()),
					Scope);
			if (!Test.TestTrue(
					*(Scope + TEXT(" public operation succeeds")),
					Result.IsOk()))
			{
				Test.AddError(Result.GetErrorJson());
				return false;
			}
			Test.TestTrue(
				*(Scope + TEXT(" C9 names isolated target artifact readback")),
				IsVerifiedIsolatedPackageOutcome(Result.GetPayloadJson()));
			Test.TestEqual(
				*(Scope + TEXT(" leaves the operand file unchanged")),
				Operand.FileHash(),
				OperandFileHashBefore);
			TOptional<FUeAgentStateRevision> OperandRevisionAfterCommand;
			ExecutionMutation::FExecutionMutationError RevisionError;
			Test.TestTrue(
				*(Scope + TEXT(" leaves the live operand revision unchanged")),
				TryReadModelingGeometryCarrierRevision(
					Operand.ObjectPath,
					*OperandMesh,
					OperandRevisionAfterCommand,
					RevisionError)
					&& OperandRevisionBefore.IsSet()
					&& OperandRevisionAfterCommand.IsSet()
					&& OperandRevisionAfterCommand.GetValue()
						== OperandRevisionBefore.GetValue());

			Composition::SimpleAuthoring::OperationHost::
				SetAutomationWorldOverride(&World, true);
			bool bTargetOriginalCollected = false;
			UStaticMesh* ReloadedTarget = Target.ReloadFromDisk(
				bTargetOriginalCollected);
			Test.TestTrue(
				*(Scope + TEXT(" unloads the source target package")),
				bTargetOriginalCollected);
			if (Test.TestNotNull(
					*(Scope + TEXT(" reloads the exact target artifact")),
					ReloadedTarget))
			{
				Test.TestTrue(
					*(Scope + TEXT(" verifies the persisted target")),
					VerifyTargetAfter(*ReloadedTarget));
				TOptional<FUeAgentStateRevision> TargetRevisionAfter;
				RevisionError = {};
				Test.TestTrue(
					*(Scope + TEXT(" advances only the target revision")),
					TryReadModelingGeometryCarrierRevision(
						Target.ObjectPath,
						*ReloadedTarget,
						TargetRevisionAfter,
						RevisionError)
						&& TargetRevisionBefore.IsSet()
						&& TargetRevisionAfter.IsSet()
						&& TargetRevisionAfter.GetValue()
							!= TargetRevisionBefore.GetValue());
			}

			OperandKeepAlive.Reset();
			bool bOperandOriginalCollected = false;
			UStaticMesh* ReloadedOperand = Operand.ReloadFromDisk(
				bOperandOriginalCollected);
			Test.TestTrue(
				*(Scope + TEXT(" unloads the source operand package")),
				bOperandOriginalCollected);
			if (Test.TestNotNull(
					*(Scope + TEXT(" reloads the exact operand artifact")),
					ReloadedOperand))
			{
				Test.TestTrue(
					*(Scope + TEXT(" verifies the persisted operand is unchanged")),
					VerifyOperandAfter(*ReloadedOperand));
				TOptional<FUeAgentStateRevision> OperandRevisionAfterReload;
				RevisionError = {};
				Test.TestTrue(
					*(Scope + TEXT(" preserves the operand revision")),
					TryReadModelingGeometryCarrierRevision(
						Operand.ObjectPath,
						*ReloadedOperand,
						OperandRevisionAfterReload,
						RevisionError)
						&& OperandRevisionBefore.IsSet()
						&& OperandRevisionAfterReload.IsSet()
						&& OperandRevisionAfterReload.GetValue()
							== OperandRevisionBefore.GetValue());
			}
		}
		Test.TestFalse(
			*(Scope + TEXT(" removes the target package file")),
			IFileManager::Get().FileExists(*TargetHeaderPath));
		Test.TestFalse(
			*(Scope + TEXT(" removes the operand package file")),
			IFileManager::Get().FileExists(*OperandHeaderPath));
		return !Test.HasAnyErrors();
	}

	bool RunCurrentStaticMeshOperandRejectedCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& CommandId,
		const FString& Scope,
		const FString& ExpectedErrorCode,
		const FCarrierStep& PrepareTarget,
		const FCarrierStep& PrepareOperand,
		const FBuildOperandParams& BuildParams,
		const FCarrierStep& VerifyTargetBefore,
		const FCarrierStep& VerifyOperandBefore,
		const FCarrierStep& VerifyTargetUnchanged,
		const FCarrierStep& VerifyOperandUnchanged)
	{
		FString TargetHeaderPath;
		FString OperandHeaderPath;
		{
			FScopedPublicStaticMesh Target(Scope + TEXT("Target"));
			FScopedPublicStaticMesh Operand(Scope + TEXT("Operand"));
			TargetHeaderPath = Target.HeaderPath;
			OperandHeaderPath = Operand.HeaderPath;
			UStaticMesh* TargetMesh = Target.Create();
			UStaticMesh* OperandMesh = Operand.Create();
			if (!Test.TestNotNull(
					*(Scope + TEXT(" creates the target asset")),
					TargetMesh)
				|| !Test.TestNotNull(
					*(Scope + TEXT(" creates the operand asset")),
					OperandMesh))
			{
				return false;
			}
			TStrongObjectPtr<UStaticMesh> OperandKeepAlive(OperandMesh);
			if (!Test.TestTrue(
					*(Scope + TEXT(" prepares both exact carriers")),
					PrepareTarget(*TargetMesh)
						&& Target.Save(*TargetMesh)
						&& PrepareOperand(*OperandMesh)
						&& Operand.Save(*OperandMesh))
				|| !Test.TestTrue(
					*(Scope + TEXT(" verifies the target baseline")),
					VerifyTargetBefore(*TargetMesh))
				|| !Test.TestTrue(
					*(Scope + TEXT(" verifies the operand baseline")),
					VerifyOperandBefore(*OperandMesh)))
			{
				return false;
			}

			const TSharedPtr<FJsonObject> TargetRevision = ReadRevisionJson(
				Target.ObjectPath,
				*TargetMesh);
			const TSharedPtr<FJsonObject> OperandRevision = ReadRevisionJson(
				Operand.ObjectPath,
				*OperandMesh);
			if (!Test.TestNotNull(
					*(Scope + TEXT(" reads the target revision")),
					TargetRevision.Get())
				|| !Test.TestNotNull(
					*(Scope + TEXT(" reads the operand revision")),
					OperandRevision.Get()))
			{
				return false;
			}
			TOptional<FUeAgentStateRevision> TargetRevisionBefore;
			TOptional<FUeAgentStateRevision> OperandRevisionBefore;
			FUeAgentStateRevisionJson::TryParse(
				TargetRevision,
				TargetRevisionBefore);
			FUeAgentStateRevisionJson::TryParse(
				OperandRevision,
				OperandRevisionBefore);
			const FString TargetFileHashBefore = Target.FileHash();
			const FString OperandFileHashBefore = Operand.FileHash();
			const bool bTargetDirtyBefore =
				TargetMesh->GetOutermost()->IsDirty();
			const bool bOperandDirtyBefore =
				OperandMesh->GetOutermost()->IsDirty();

			const FUeAgentCommandResult Result =
				UE::UAI::Tests::PublicCommand::SubmitActiveCommand(
					Registry,
					Dispatch,
					CommandId,
					BuildParams(
						Target.ObjectPath,
						TargetRevision.ToSharedRef(),
						Operand.ObjectPath,
						OperandRevision.ToSharedRef()),
					Scope);
			Test.TestFalse(
				*(Scope + TEXT(" public operation is rejected")),
				Result.IsOk());
			Test.TestEqual(
				*(Scope + TEXT(" preserves the exact domain error")),
				ReadCommandErrorCode(Result),
				ExpectedErrorCode);
			Test.TestEqual(
				*(Scope + TEXT(" leaves the target file unchanged")),
				Target.FileHash(),
				TargetFileHashBefore);
			Test.TestEqual(
				*(Scope + TEXT(" leaves the operand file unchanged")),
				Operand.FileHash(),
				OperandFileHashBefore);
			Test.TestEqual(
				*(Scope + TEXT(" leaves the target dirty state unchanged")),
				TargetMesh->GetOutermost()->IsDirty(),
				bTargetDirtyBefore);
			Test.TestEqual(
				*(Scope + TEXT(" leaves the operand dirty state unchanged")),
				OperandMesh->GetOutermost()->IsDirty(),
				bOperandDirtyBefore);
			Test.TestTrue(
				*(Scope + TEXT(" leaves the live target unchanged")),
				VerifyTargetUnchanged(*TargetMesh));
			Test.TestTrue(
				*(Scope + TEXT(" leaves the live operand unchanged")),
				VerifyOperandUnchanged(*OperandMesh));

			TOptional<FUeAgentStateRevision> TargetLiveRevisionAfter;
			TOptional<FUeAgentStateRevision> OperandLiveRevisionAfter;
			ExecutionMutation::FExecutionMutationError RevisionError;
			Test.TestTrue(
				*(Scope + TEXT(" leaves the live target revision unchanged")),
				TryReadModelingGeometryCarrierRevision(
					Target.ObjectPath,
					*TargetMesh,
					TargetLiveRevisionAfter,
					RevisionError)
					&& TargetRevisionBefore.IsSet()
					&& TargetLiveRevisionAfter.IsSet()
					&& TargetLiveRevisionAfter.GetValue()
						== TargetRevisionBefore.GetValue());
			RevisionError = {};
			Test.TestTrue(
				*(Scope + TEXT(" leaves the live operand revision unchanged")),
				TryReadModelingGeometryCarrierRevision(
					Operand.ObjectPath,
					*OperandMesh,
					OperandLiveRevisionAfter,
					RevisionError)
					&& OperandRevisionBefore.IsSet()
					&& OperandLiveRevisionAfter.IsSet()
					&& OperandLiveRevisionAfter.GetValue()
						== OperandRevisionBefore.GetValue());

			Composition::SimpleAuthoring::OperationHost::
				SetAutomationWorldOverride(&World, true);
			bool bTargetOriginalCollected = false;
			UStaticMesh* ReloadedTarget = Target.ReloadFromDisk(
				bTargetOriginalCollected);
			Test.TestTrue(
				*(Scope + TEXT(" unloads the rejected target package")),
				bTargetOriginalCollected);
			if (Test.TestNotNull(
					*(Scope + TEXT(" reloads the rejected target package")),
					ReloadedTarget))
			{
				Test.TestTrue(
					*(Scope + TEXT(" preserves the persisted target")),
					VerifyTargetUnchanged(*ReloadedTarget));
				TOptional<FUeAgentStateRevision> TargetReloadedRevision;
				RevisionError = {};
				Test.TestTrue(
					*(Scope + TEXT(" preserves the persisted target revision")),
					TryReadModelingGeometryCarrierRevision(
						Target.ObjectPath,
						*ReloadedTarget,
						TargetReloadedRevision,
						RevisionError)
						&& TargetRevisionBefore.IsSet()
						&& TargetReloadedRevision.IsSet()
						&& TargetReloadedRevision.GetValue()
							== TargetRevisionBefore.GetValue());
			}

			OperandKeepAlive.Reset();
			bool bOperandOriginalCollected = false;
			UStaticMesh* ReloadedOperand = Operand.ReloadFromDisk(
				bOperandOriginalCollected);
			Test.TestTrue(
				*(Scope + TEXT(" unloads the rejected operand package")),
				bOperandOriginalCollected);
			if (Test.TestNotNull(
					*(Scope + TEXT(" reloads the rejected operand package")),
					ReloadedOperand))
			{
				Test.TestTrue(
					*(Scope + TEXT(" preserves the persisted operand")),
					VerifyOperandUnchanged(*ReloadedOperand));
				TOptional<FUeAgentStateRevision> OperandReloadedRevision;
				RevisionError = {};
				Test.TestTrue(
					*(Scope + TEXT(" preserves the persisted operand revision")),
					TryReadModelingGeometryCarrierRevision(
						Operand.ObjectPath,
						*ReloadedOperand,
						OperandReloadedRevision,
						RevisionError)
						&& OperandRevisionBefore.IsSet()
						&& OperandReloadedRevision.IsSet()
						&& OperandReloadedRevision.GetValue()
							== OperandRevisionBefore.GetValue());
			}
		}
		Test.TestFalse(
			*(Scope + TEXT(" removes the target package file")),
			IFileManager::Get().FileExists(*TargetHeaderPath));
		Test.TestFalse(
			*(Scope + TEXT(" removes the operand package file")),
			IFileManager::Get().FileExists(*OperandHeaderPath));
		return !Test.HasAnyErrors();
	}

	bool RunWithPublicStaticMeshEnvironment(
		FAutomationTestBase& Test,
		const FPublicProductBody& Body)
	{
		using namespace UE::UAI::Module::CommandRegistration::Active;
		TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
		TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
		FString CompositionError;
		if (!Test.TestTrue(
				TEXT("the active public composition freezes"),
				TryComposeProcessActiveCommandComposition(
					Registry,
					Dispatch,
					CompositionError))
			|| !Registry.IsValid()
			|| !Dispatch.IsValid())
		{
			Test.AddError(CompositionError);
			return false;
		}
		UWorld* const World = GEditor
			? GEditor->GetEditorWorldContext().World()
			: nullptr;
		if (!Test.TestNotNull(TEXT("the owned worker has an editor world"), World))
		{
			return false;
		}
		Composition::SimpleAuthoring::OperationHost::
			SetAutomationWorldOverride(World, true);
		ON_SCOPE_EXIT
		{
			Composition::SimpleAuthoring::OperationHost::
				SetAutomationWorldOverride(nullptr, true);
		};
		return Body(Registry, Dispatch, *World) && !Test.HasAnyErrors();
	}
}
