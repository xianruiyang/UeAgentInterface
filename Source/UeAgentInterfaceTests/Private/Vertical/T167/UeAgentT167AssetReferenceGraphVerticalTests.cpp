#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Vertical/T167/UeAgentT167AssetReferenceTestTypes.h"

namespace
{
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::Tests::PublicCommand;

	struct FAssetIdentity
	{
		FString PackageName;
		FString AssetName;

		FString ObjectPath() const
		{
			return PackageName + TEXT(".") + AssetName;
		}

		FString Filename() const
		{
			return FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetAssetPackageExtension());
		}
	};

	class FScopedT167Assets final
	{
	public:
		FScopedT167Assets()
			: Root(TEXT("/Game/UAI_Automation/T167_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits))
			, A{Root + TEXT("/A"), TEXT("A")}
			, B{Root + TEXT("/B"), TEXT("B")}
			, C{Root + TEXT("/C"), TEXT("C")}
		{
			All = {A, B, C};
		}

		~FScopedT167Assets()
		{
			Cleanup();
		}

		bool Create()
		{
			AssetA = CreateAsset(A);
			AssetB = CreateAsset(B);
			AssetC = CreateAsset(C);
			if (!AssetA || !AssetB || !AssetC)
			{
				return false;
			}
			AssetA->HardReference = AssetB;
			AssetA->SoftReference = TSoftObjectPtr<UObject>(
				FSoftObjectPath(C.ObjectPath()));
			AssetB->HardReference = AssetC;
			AssetC->HardReference = AssetA;
			for (UUeAgentT167AssetReferenceData* Asset
				: {AssetC, AssetB, AssetA})
			{
				Asset->PostEditChange();
				Asset->MarkPackageDirty();
				const FAssetIdentity* Identity = Asset == AssetA
					? &A : Asset == AssetB ? &B : &C;
				if (!Save(*Identity, Asset))
				{
					return false;
				}
			}
			TArray<FString> Filenames;
			for (const FAssetIdentity& Identity : All)
			{
				Filenames.Add(Identity.Filename());
			}
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Filenames);
			return true;
		}

		bool Cleanup()
		{
			TArray<UObject*> Objects;
			for (const FAssetIdentity& Identity : All)
			{
				if (UObject* Object = LoadObject<UObject>(
					nullptr, *Identity.ObjectPath()))
				{
					Objects.Add(Object);
				}
			}
			if (!Objects.IsEmpty())
			{
				TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
				ObjectTools::ForceDeleteObjects(Objects, false);
			}
			AssetA = nullptr;
			AssetB = nullptr;
			AssetC = nullptr;
			CollectGarbage(RF_NoFlags, true);

			TArray<FString> Filenames;
			for (const FAssetIdentity& Identity : All)
			{
				Filenames.Add(Identity.Filename());
				if (IFileManager::Get().FileExists(*Identity.Filename()))
				{
					IFileManager::Get().Delete(
						*Identity.Filename(), false, true);
				}
			}
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Filenames);
			CollectGarbage(RF_NoFlags, true);
			const FString Directory =
				FPackageName::LongPackageNameToFilename(Root);
			return IFileManager::Get().DeleteDirectory(
				*Directory, false, true)
				|| !IFileManager::Get().DirectoryExists(*Directory);
		}

		FString Root;
		FAssetIdentity A;
		FAssetIdentity B;
		FAssetIdentity C;

	private:
		UUeAgentT167AssetReferenceData* CreateAsset(
			const FAssetIdentity& Identity)
		{
			UPackage* Package = CreatePackage(*Identity.PackageName);
			if (!Package)
			{
				return nullptr;
			}
			UUeAgentT167AssetReferenceData* Asset =
				NewObject<UUeAgentT167AssetReferenceData>(
					Package,
					*Identity.AssetName,
					RF_Public | RF_Standalone | RF_Transactional);
			if (!Asset)
			{
				return nullptr;
			}
			FAssetRegistryModule::AssetCreated(Asset);
			Package->MarkPackageDirty();
			return Save(Identity, Asset) ? Asset : nullptr;
		}

		bool Save(const FAssetIdentity& Identity, UObject* Asset) const
		{
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			return UPackage::SavePackage(
				Asset->GetOutermost(), Asset, *Identity.Filename(), Args);
		}

		TArray<FAssetIdentity> All;
		UUeAgentT167AssetReferenceData* AssetA = nullptr;
		UUeAgentT167AssetReferenceData* AssetB = nullptr;
		UUeAgentT167AssetReferenceData* AssetC = nullptr;
	};

	FString RelationParams(
		const FString& Target,
		const FString& Direction,
		const FString& Kind)
	{
		const TSharedRef<FJsonObject> Input = MakeShared<FJsonObject>();
		Input->SetStringField(TEXT("direction"), Direction);
		Input->SetStringField(TEXT("package_scope"), TEXT("all"));
		Input->SetArrayField(
			TEXT("relation_kinds"),
			{MakeShared<FJsonValueString>(Kind)});
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("target"), Target);
		Root->SetObjectField(TEXT("input"), Input);
		FString Json;
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Root, Writer);
		return Json;
	}

	bool ParseRelationGraph(
		const FJsonObject& Result,
		TSharedPtr<FJsonObject>& OutGraph)
	{
		OutGraph.Reset();
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		const TSharedPtr<FJsonObject>* QueryResult = nullptr;
		const TSharedPtr<FJsonObject>* Projection = nullptr;
		const TSharedPtr<FJsonObject>* ProjectedPayload = nullptr;
		const TSharedPtr<FJsonObject>* Snapshot = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Result.TryGetObjectField(TEXT("payload"), Payload)
			|| !Payload || !(*Payload)->TryGetObjectField(TEXT("query_result"), QueryResult)
			|| !QueryResult || !(*QueryResult)->TryGetObjectField(
				TEXT("projection"), Projection)
			|| !Projection || !(*Projection)->TryGetObjectField(
				TEXT("payload"), ProjectedPayload)
			|| !ProjectedPayload || !(*ProjectedPayload)->TryGetObjectField(
				TEXT("snapshot"), Snapshot)
			|| !Snapshot || !(*Snapshot)->TryGetArrayField(TEXT("entries"), Entries)
			|| !Entries)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Entries)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			const TSharedPtr<FJsonObject>* Identity = nullptr;
			const TSharedPtr<FJsonObject>* Graph = nullptr;
			if (Value.IsValid() && Value->TryGetObject(Entry) && Entry
				&& (*Entry)->TryGetObjectField(TEXT("identity"), Identity)
				&& Identity
				&& (*Identity)->GetStringField(TEXT("selector"))
					== TEXT("relation_graph")
				&& (*Entry)->TryGetObjectField(TEXT("value"), Graph)
				&& Graph)
			{
				OutGraph = *Graph;
				return true;
			}
		}
		return false;
	}

	bool TryCanonicalField(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Name,
		TSharedPtr<FJsonObject>& OutField)
	{
		OutField.Reset();
		const TSharedPtr<FJsonObject>* Fields = nullptr;
		const TSharedPtr<FJsonObject>* Field = nullptr;
		if (!Object.IsValid()
			|| Object->GetStringField(TEXT("kind")) != TEXT("object")
			|| !Object->TryGetObjectField(TEXT("fields"), Fields)
			|| !Fields || !(*Fields)->TryGetObjectField(Name, Field)
			|| !Field)
		{
			return false;
		}
		OutField = *Field;
		return OutField.IsValid();
	}

	bool TryCanonicalString(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Name,
		FString& OutValue)
	{
		TSharedPtr<FJsonObject> Field;
		return TryCanonicalField(Object, Name, Field)
			&& Field->GetStringField(TEXT("kind")) == TEXT("string")
			&& Field->TryGetStringField(TEXT("text"), OutValue);
	}

	bool TryCanonicalInteger(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Name,
		int32& OutValue)
	{
		TSharedPtr<FJsonObject> Field;
		FString Lexeme;
		if (!TryCanonicalField(Object, Name, Field)
			|| (Field->GetStringField(TEXT("kind")) != TEXT("integer")
				&& Field->GetStringField(TEXT("kind")) != TEXT("decimal"))
			|| !Field->TryGetStringField(TEXT("lexeme"), Lexeme)
			|| !Lexeme.IsNumeric())
		{
			return false;
		}
		const double Numeric = FCString::Atod(*Lexeme);
		if (!FMath::IsNearlyEqual(Numeric, FMath::RoundToDouble(Numeric)))
		{
			return false;
		}
		OutValue = FMath::RoundToInt(Numeric);
		return true;
	}

	bool TryCanonicalArray(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Name,
		const TArray<TSharedPtr<FJsonValue>>*& OutItems)
	{
		OutItems = nullptr;
		TSharedPtr<FJsonObject> Field;
		return TryCanonicalField(Object, Name, Field)
			&& Field->GetStringField(TEXT("kind")) == TEXT("array")
			&& Field->TryGetArrayField(TEXT("items"), OutItems)
			&& OutItems;
	}

	TMap<FString, FString> PackageKeys(const TSharedPtr<FJsonObject>& Graph)
	{
		TMap<FString, FString> Result;
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!TryCanonicalArray(Graph, TEXT("nodes"), Nodes))
		{
			return Result;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Nodes)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			FString Package;
			FString Key;
			if (Value.IsValid() && Value->TryGetObject(Node) && Node)
			{
				if (TryCanonicalString(*Node, TEXT("package"), Package)
					&& TryCanonicalString(*Node, TEXT("key"), Key))
				{
					Result.Add(MoveTemp(Package), MoveTemp(Key));
				}
			}
		}
		return Result;
	}

	bool HasEdge(
		const TSharedPtr<FJsonObject>& Graph,
		const FString& SourceId,
		const FString& TargetId,
		const FString& Traversal,
		const FString& Property,
		const int32 Depth,
		const bool bRequireCycle = false)
	{
		const TMap<FString, FString> Keys = PackageKeys(Graph);
		const FString* Source = Keys.Find(SourceId);
		const FString* Target = Keys.Find(TargetId);
		const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
		if (!Source || !Target
			|| !TryCanonicalArray(Graph, TEXT("edges"), Edges))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Edges)
		{
			const TSharedPtr<FJsonObject>* Edge = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			FString ActualSource;
			FString ActualTarget;
			FString ActualTraversal;
			int32 ActualDepth = 0;
			TSharedPtr<FJsonObject> Cycle;
			bool bCycle = false;
			if (Value.IsValid() && Value->TryGetObject(Edge) && Edge
				&& TryCanonicalString(*Edge, TEXT("source"), ActualSource)
				&& ActualSource == *Source
				&& TryCanonicalString(*Edge, TEXT("target"), ActualTarget)
				&& ActualTarget == *Target
				&& TryCanonicalString(*Edge, TEXT("traversal"), ActualTraversal)
				&& ActualTraversal == Traversal
				&& TryCanonicalInteger(*Edge, TEXT("depth"), ActualDepth)
				&& ActualDepth == Depth
				&& (!bRequireCycle
					|| (TryCanonicalField(*Edge, TEXT("cycle"), Cycle)
						&& Cycle->TryGetBoolField(TEXT("boolean"), bCycle)
						&& bCycle))
				&& TryCanonicalArray(*Edge, TEXT("properties"), Properties)
				&& Properties->ContainsByPredicate(
					[&Property](const TSharedPtr<FJsonValue>& Item)
					{
						const TSharedPtr<FJsonObject>* Canonical = nullptr;
						FString Text;
						return Item.IsValid() && Item->TryGetObject(Canonical)
							&& Canonical && (*Canonical)->TryGetStringField(
								TEXT("text"), Text) && Text == Property;
					}))
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT167AssetReferenceGraphVerticalTest,
	"UeAgentInterface.Vertical.T167.AssetReferenceGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT167AssetReferenceGraphVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FScopedT167Assets Fixture;
	if (!TestTrue(TEXT("fresh A-B-C relation fixture is saved"), Fixture.Create()))
	{
		return false;
	}
	AWorldSettings* WorldSettings = GEditor
		? GEditor->GetEditorWorldContext().World()->GetWorldSettings()
		: nullptr;
	if (!TestNotNull(TEXT("editor world settings"), WorldSettings))
	{
		return false;
	}
	FScopedPublicCommandComposition Harness(TEXT("t167-asset-reference-graph"));
	FString Error;
	if (!TestTrue(TEXT("formal public composition starts"),
			Harness.TryStart(*WorldSettings, Error)))
	{
		AddError(Error);
		return false;
	}

	auto Query = [&](const FAssetIdentity& Target, const FString& Direction,
		const FString& Kind, TSharedPtr<FJsonObject>& OutGraph)
	{
		TSharedPtr<FJsonObject> Result;
		const bool bInvoked = TryInvokeCommand(
			Harness.GetRouteHost(), Harness.GetConfig(), TEXT("asset.relations"),
			RelationParams(Target.ObjectPath(), Direction, Kind),
			Result, Error);
		return bInvoked && Result.IsValid() && Result->GetBoolField(TEXT("ok"))
			&& ParseRelationGraph(*Result, OutGraph);
	};

	TSharedPtr<FJsonObject> HardOutgoing;
	TestTrue(TEXT("A outgoing hard relation graph is public and recursive"),
		Query(Fixture.A, TEXT("dependencies"), TEXT("hard"), HardOutgoing));
	TestTrue(TEXT("A directly depends on B"),
		HasEdge(HardOutgoing, Fixture.A.PackageName, Fixture.B.PackageName,
			TEXT("outgoing"), TEXT("hard"), 1));
	TestTrue(TEXT("A reaches C through B"),
		HasEdge(HardOutgoing, Fixture.B.PackageName, Fixture.C.PackageName,
			TEXT("outgoing"), TEXT("hard"), 2));
	TestTrue(TEXT("C-to-A cycle is retained and bounded"),
		HasEdge(HardOutgoing, Fixture.C.PackageName, Fixture.A.PackageName,
			TEXT("outgoing"), TEXT("hard"), 3, true));

	TSharedPtr<FJsonObject> Incoming;
	TestTrue(TEXT("B incoming hard relation graph is public"),
		Query(Fixture.B, TEXT("referencers"), TEXT("hard"), Incoming));
	TestTrue(TEXT("B incoming edge preserves actual A-to-B direction"),
		HasEdge(Incoming, Fixture.A.PackageName, Fixture.B.PackageName,
			TEXT("incoming"), TEXT("hard"), 1));

	TSharedPtr<FJsonObject> SoftOutgoing;
	TestTrue(TEXT("A direct soft relation graph is public"),
		Query(Fixture.A, TEXT("dependencies"), TEXT("soft"), SoftOutgoing));
	TestTrue(TEXT("A soft reference reaches C with soft property"),
		HasEdge(SoftOutgoing, Fixture.A.PackageName, Fixture.C.PackageName,
			TEXT("outgoing"), TEXT("soft"), 1));

	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
	TArray<FAssetDependency> NativeHard;
	TArray<FAssetDependency> NativeSoft;
	const bool bNativeHard = Registry.GetDependencies(
		FAssetIdentifier(FName(*Fixture.A.PackageName)), NativeHard,
		UE::AssetRegistry::EDependencyCategory::Package,
		UE::AssetRegistry::FDependencyQuery(
			UE::AssetRegistry::EDependencyQuery::Hard));
	const bool bNativeSoft = Registry.GetDependencies(
		FAssetIdentifier(FName(*Fixture.A.PackageName)), NativeSoft,
		UE::AssetRegistry::EDependencyCategory::Package,
		UE::AssetRegistry::FDependencyQuery(
			UE::AssetRegistry::EDependencyQuery::NotHard));
	TestTrue(TEXT("independent Registry hard result contains B"),
		bNativeHard && NativeHard.ContainsByPredicate(
			[&Fixture](const FAssetDependency& Edge)
			{
				return Edge.AssetId.PackageName == FName(*Fixture.B.PackageName);
			}));
	TestTrue(TEXT("independent Registry soft result contains C"),
		bNativeSoft && NativeSoft.ContainsByPredicate(
			[&Fixture](const FAssetDependency& Edge)
			{
				return Edge.AssetId.PackageName == FName(*Fixture.C.PackageName);
			}));

	TestTrue(TEXT("fresh T167 assets and public composition clean up"),
		Fixture.Cleanup() && Harness.Stop());
	return !HasAnyErrors();
}
