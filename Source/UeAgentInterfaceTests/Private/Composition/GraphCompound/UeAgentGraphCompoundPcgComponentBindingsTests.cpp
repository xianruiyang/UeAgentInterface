#include "Misc/AutomationTest.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAiBindings.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "PCGComponent.h"
#include "PCGGraph.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	using namespace UE::UAI::Composition::GraphCompound;
	using namespace UE::UAI::Contract;

	const FString OwnerMismatchCode =
		TEXT("graph_compound_pcg_component_owner_mismatch");

	class FActiveCancellation final : public IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	class FScopedPcgGraphAsset final
	{
	public:
		explicit FScopedPcgGraphAsset(const FString& Stem)
			: PackagePath(TEXT("/Game/UAI/Tests/GraphCompound"))
			, AssetName(Stem + TEXT("_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits))
			, PackageName(PackagePath + TEXT("/") + AssetName)
		{
		}

		~FScopedPcgGraphAsset()
		{
			Unload();
			IFileManager::Get().Delete(*Filename(), false, true);
		}

		UPCGGraph* CreateAndSave(FAutomationTestBase& Test) const
		{
			UPackage* const Package = CreatePackage(*PackageName);
			UPCGGraph* const Graph = Package
				? NewObject<UPCGGraph>(Package, *AssetName,
					RF_Public | RF_Standalone | RF_Transactional)
				: nullptr;
			if (!Test.TestNotNull(TEXT("PCG graph fixture is created"), Graph))
			{
				return nullptr;
			}
			Graph->SetFlags(RF_Public | RF_Standalone);
			Package->MarkPackageDirty();
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			if (!Test.TestTrue(TEXT("PCG graph fixture is durable"),
				UPackage::SavePackage(Package, Graph, *Filename(), Args)))
			{
				return nullptr;
			}
			Package->SetDirtyFlag(false);
			return Graph;
		}

		bool Unload() const
		{
			UPackage* const Package = FindPackage(nullptr, *PackageName);
			if (!Package)
			{
				return true;
			}
			Package->SetDirtyFlag(false);
			FText Error;
			const bool bUnloaded =
				UPackageTools::UnloadPackages({Package}, Error, true);
			CollectGarbage(RF_NoFlags, true);
			return bUnloaded && !FindPackage(nullptr, *PackageName);
		}

		FString ObjectPath() const
		{
			return PackageName + TEXT(".") + AssetName;
		}

		FString Filename() const
		{
			return FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetAssetPackageExtension());
		}

		FString PackagePath;
		FString AssetName;
		FString PackageName;
	};

	class FScopedPcgWorldSession final
	{
	public:
		explicit FScopedPcgWorldSession(FAutomationTestBase& Test)
		{
			const FString Suffix =
				FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Package = CreatePackage(*(
				TEXT("/Game/UAI/Tests/GraphCompound/PCGW_PA013_") + Suffix));
			World = Package ? UWorld::CreateWorld(
				EWorldType::EditorPreview, false,
				FName(*(TEXT("PCGW_PA013_") + Suffix)), Package) : nullptr;
			if (!Test.TestNotNull(TEXT("owned PCG world exists"), World))
			{
				return;
			}
			if (GEngine && !GEngine->GetWorldContextFromWorld(World))
			{
				FWorldContext& Context =
					GEngine->CreateNewWorldContext(EWorldType::EditorPreview);
				Context.SetCurrentWorld(World);
				bOwnsWorldContext = true;
			}

			FActorSpawnParameters ActorParams;
			ActorParams.Name = FName(*(TEXT("PCGActor_PA013_") + Suffix));
			Actor = World->SpawnActor<AActor>(ActorParams);
			FActorSpawnParameters OtherActorParams;
			OtherActorParams.Name =
				FName(*(TEXT("PCGOtherActor_PA013_") + Suffix));
			OtherActor = World->SpawnActor<AActor>(OtherActorParams);
			Component = Actor ? NewObject<UPCGComponent>(
				Actor, TEXT("PCGComponent_PA013"), RF_Transactional) : nullptr;
			if (!Test.TestNotNull(TEXT("owned PCG actor exists"), Actor)
				|| !Test.TestNotNull(TEXT("owner mismatch actor exists"), OtherActor)
				|| !Test.TestNotNull(TEXT("owned PCG component exists"), Component))
			{
				return;
			}
			Actor->AddInstanceComponent(Component);
			Component->RegisterComponent();
			Package->SetDirtyFlag(false);
		}

		~FScopedPcgWorldSession()
		{
			if (Component && Component->IsRegistered())
			{
				Component->UnregisterComponent();
			}
			if (World)
			{
				if (bOwnsWorldContext && GEngine)
				{
					GEngine->DestroyWorldContext(World);
				}
				World->DestroyWorld(false);
				World->MarkAsGarbage();
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
			CollectGarbage(RF_NoFlags, true);
		}

		bool IsValid() const
		{
			return World && Actor && OtherActor && Component
				&& Component->GetOwner() == Actor
				&& Component->GetWorld() == World
				&& GEngine && GEngine->GetWorldContextFromWorld(World);
		}

		FString ActorRef() const { return Actor->GetPathName(); }
		FString OtherActorRef() const { return OtherActor->GetPathName(); }
		FString ComponentRef() const { return Component->GetPathName(); }
		bool IsPackageDirty() const { return Package && Package->IsDirty(); }

		UPackage* Package = nullptr;
		UWorld* World = nullptr;
		AActor* Actor = nullptr;
		AActor* OtherActor = nullptr;
		UPCGComponent* Component = nullptr;
		bool bOwnsWorldContext = false;
	};

	FUeAgentCallerContext Caller()
	{
		return FUeAgentCallerContext(
			TEXT("test.agent"),
			{TEXT("pcg_ai_node_graph.read"),
				TEXT("pcg_ai_node_graph.write")},
			TEXT("correlation:gc018:pa013"));
	}

	bool TryCreateProductionRuntime(
		FAutomationTestBase& Test,
		TSharedPtr<FGraphCompoundRuntime>& OutRuntime)
	{
		FGraphCompoundRuntimeConfig RuntimeConfig;
		RuntimeConfig.MaxFrameBytes = 1024 * 1024;
		FGraphCompoundPcgAiProductionConfig ProductionConfig;
		ProductionConfig.OperationTimeoutSeconds = 30.0;
		FGraphCompoundRuntimeError Error;
		if (!TryCreatePcgAiGraphProductionRuntime(
			RuntimeConfig, ProductionConfig, OutRuntime, Error))
		{
			Test.AddError(Error.Code + TEXT(":") + Error.Detail);
			return false;
		}
		return Test.TestNotNull(
			TEXT("production PCG/AI runtime exists"), OutRuntime.Get());
	}

	bool TryGetOperationError(
		const FUeAgentOperationResult& Result,
		FString& OutCode,
		FString& OutDetail)
	{
		OutCode.Reset();
		OutDetail.Reset();
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Result.GetValue().GetJsonValue());
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* Error = nullptr;
		if (!Root->TryGetObjectField(TEXT("operation_error"), Error)
			|| !Error || !Error->IsValid())
		{
			return false;
		}
		(*Error)->TryGetStringField(TEXT("code"), OutCode);
		(*Error)->TryGetStringField(TEXT("detail"), OutDetail);
		return !OutCode.IsEmpty();
	}

	FUeAgentOperationResult Submit(
		FGraphCompoundRuntime& Runtime,
		const FString& CapabilityId,
		const FString& RequestSuffix,
		const FString& Payload)
	{
		const FPcgAnimationGraphRouteProfile* const Profile =
			FindPcgAnimationGraphRouteProfile(CapabilityId);
		check(Profile);
		return Runtime.GetOperationPort()->Submit(
			FUeAgentOperationRequest(
				Profile->GetExecutionContract().GetDescriptorIdentity(),
				TEXT("request:pa013:") + RequestSuffix,
				TEXT("idempotency:pa013:") + RequestSuffix,
				FUeAgentContractValue(Payload)),
			Caller(), MakeShared<FActiveCancellation>());
	}

	bool TestSchemaRejected(
		FAutomationTestBase& Test,
		const FUeAgentOperationResult& Result)
	{
		FString Code;
		FString Detail;
		return Test.TestTrue(TEXT("invalid input returns operation_error"),
			TryGetOperationError(Result, Code, Detail))
			&& Test.TestEqual(TEXT("unknown field fails at public schema"),
				Code, FString(TEXT("contract_input_schema_mismatch")));
	}

	bool TestMutationSucceeded(
		FAutomationTestBase& Test,
		const FUeAgentOperationResult& Result)
	{
		if (!Test.TestTrue(TEXT("mutation result is valid"), Result.IsValid()))
		{
			return false;
		}
		FString Code;
		FString Detail;
		if (TryGetOperationError(Result, Code, Detail))
		{
			Test.AddError(Code + TEXT(":") + Detail);
			return false;
		}
		return true;
	}

	UPCGComponent* ReadComponent(const FString& ComponentRef)
	{
		return FindObject<UPCGComponent>(nullptr, *ComponentRef);
	}

	bool TestComponentReadback(
		FAutomationTestBase& Test,
		const FScopedPcgWorldSession& Session,
		UPCGGraph* ExpectedGraph,
		const EPCGComponentGenerationTrigger ExpectedPolicy,
		const FString& Prefix)
	{
		UPCGComponent* const Component = ReadComponent(Session.ComponentRef());
		if (!Test.TestNotNull(*(Prefix + TEXT(" reacquires component")), Component))
		{
			return false;
		}
		const AActor* const Owner = Component->GetOwner();
		return Test.TestTrue(*(Prefix + TEXT(" keeps exact actor owner")),
			Owner && Owner->GetPathName() == Session.ActorRef())
			&& Test.TestTrue(*(Prefix + TEXT(" keeps exact world session")),
				Component->GetWorld() == Session.World
				&& GEngine
				&& GEngine->GetWorldContextFromWorld(Component->GetWorld()))
			&& Test.TestTrue(*(Prefix + TEXT(" reads native graph binding")),
				Component->GetGraph() == ExpectedGraph)
			&& Test.TestEqual(*(Prefix + TEXT(" reads native generation policy")),
				Component->GenerationTrigger, ExpectedPolicy);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundPcgComponentBindingProductionTest,
	"UeAgentInterface.Composition.GraphCompound."
		"PcgAiPcgComponentBindings.Production.PcgComponentApplyRefresh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundPcgComponentBindingProductionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FScopedPcgGraphAsset GraphAsset(TEXT("PCG_PA013_ComponentGraph"));
	FScopedPcgWorldSession Session(*this);
	TSharedPtr<FGraphCompoundRuntime> Runtime;
	UPCGGraph* const Graph = GraphAsset.CreateAndSave(*this);
	if (!Session.IsValid() || !Graph
		|| !TryCreateProductionRuntime(*this, Runtime))
	{
		return false;
	}
	const EPCGComponentGenerationTrigger BaselinePolicy =
		Session.Component->GenerationTrigger;
	const FString Payload = FString::Printf(
		TEXT("{\"generation_policy\":\"on_demand\",\"graph_ref\":\"%s\",\"target_ref\":\"%s\"}"),
		*GraphAsset.ObjectPath(), *Session.ComponentRef());
	const FString InvalidPayload = Payload.LeftChop(1)
		+ TEXT(",\"unexpected\":true}");
	if (!TestSchemaRejected(*this, Submit(*Runtime,
		TEXT("pcg.component.apply"),
		TEXT("component-invalid:") + GraphAsset.AssetName, InvalidPayload)))
	{
		return false;
	}
	TestComponentReadback(*this, Session, nullptr, BaselinePolicy,
		TEXT("schema rejection"));
	TestFalse(TEXT("schema rejection does not dirty world package"),
		Session.IsPackageDirty());

	const FUeAgentOperationResult Result = Submit(*Runtime,
		TEXT("pcg.component.apply"),
		TEXT("component-valid:") + GraphAsset.AssetName, Payload);
	if (!TestMutationSucceeded(*this, Result))
	{
		return false;
	}
	TestComponentReadback(*this, Session, Graph,
		EPCGComponentGenerationTrigger::GenerateOnDemand,
		TEXT("component apply"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundPcgWorldBindingProductionTest,
	"UeAgentInterface.Composition.GraphCompound."
		"PcgAiPcgComponentBindings.Production.PcgWorldBindingApplySessionOwner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundPcgWorldBindingProductionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FScopedPcgGraphAsset GraphAsset(TEXT("PCG_PA013_WorldGraph"));
	FScopedPcgWorldSession Session(*this);
	TSharedPtr<FGraphCompoundRuntime> Runtime;
	UPCGGraph* const Graph = GraphAsset.CreateAndSave(*this);
	if (!Session.IsValid() || !Graph
		|| !TryCreateProductionRuntime(*this, Runtime))
	{
		return false;
	}
	const EPCGComponentGenerationTrigger BaselinePolicy =
		Session.Component->GenerationTrigger;
	const FString ValidPayload = FString::Printf(
		TEXT("{\"actor_ref\":\"%s\",\"component_ref\":\"%s\",\"generation_policy\":\"runtime\",\"graph_ref\":\"%s\"}"),
		*Session.ActorRef(), *Session.ComponentRef(), *GraphAsset.ObjectPath());
	const FString InvalidPayload = ValidPayload.LeftChop(1)
		+ TEXT(",\"unexpected\":true}");
	if (!TestSchemaRejected(*this, Submit(*Runtime,
		TEXT("pcg.component.world_binding.apply"),
		TEXT("world-invalid:") + GraphAsset.AssetName, InvalidPayload)))
	{
		return false;
	}
	TestComponentReadback(*this, Session, nullptr, BaselinePolicy,
		TEXT("world schema rejection"));

	const FString MismatchedOwnerPayload = FString::Printf(
		TEXT("{\"actor_ref\":\"%s\",\"component_ref\":\"%s\",\"generation_policy\":\"runtime\",\"graph_ref\":\"%s\"}"),
		*Session.OtherActorRef(), *Session.ComponentRef(),
		*GraphAsset.ObjectPath());
	const FUeAgentOperationResult MismatchedOwner = Submit(*Runtime,
		TEXT("pcg.component.world_binding.apply"),
		TEXT("world-owner-mismatch:") + GraphAsset.AssetName,
		MismatchedOwnerPayload);
	FString MismatchCode;
	FString MismatchDetail;
	if (!TestTrue(TEXT("mismatched owner returns operation_error"),
		TryGetOperationError(
			MismatchedOwner, MismatchCode, MismatchDetail)))
	{
		return false;
	}
	TestEqual(TEXT("world binding rejects mismatched owner"),
		MismatchCode, OwnerMismatchCode);
	TestComponentReadback(*this, Session, nullptr, BaselinePolicy,
		TEXT("owner mismatch"));
	TestFalse(TEXT("owner mismatch does not dirty world package"),
		Session.IsPackageDirty());

	const FUeAgentOperationResult Result = Submit(*Runtime,
		TEXT("pcg.component.world_binding.apply"),
		TEXT("world-valid:") + GraphAsset.AssetName, ValidPayload);
	if (!TestMutationSucceeded(*this, Result))
	{
		return false;
	}
	TestComponentReadback(*this, Session, Graph,
		EPCGComponentGenerationTrigger::GenerateAtRuntime,
		TEXT("world binding apply"));
	return !HasAnyErrors();
}
