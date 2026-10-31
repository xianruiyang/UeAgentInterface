#include "DomainAdapters/WorldRead/SceneAnalysis/UeAgentSceneAnalysis.h"

#include "DomainAdapters/WorldRead/SceneAnalysis/UeAgentSceneGeometry.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "DomainAdapters/WorldSpatial/UeAgentLevelWorldDomainMechanics.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringSpatial.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringApplyProvider.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "Debug/DebugDrawComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Misc/Paths.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/ObjectKey.h"
#include "Async/Async.h"
#include "Materials/MaterialInterface.h"

namespace UE::UAI::SceneAnalysis
{
namespace
{
    using FJson = TSharedPtr<FJsonObject>;
    using FValue = TSharedPtr<FJsonValue>;
    using FSharedGeometry = TSharedPtr<FGeometry, ESPMode::ThreadSafe>;
    constexpr int64 MaxCapturedVertices = 500000;
    constexpr int64 MaxGeometryBytes = 128 * 1024 * 1024;
    constexpr int32 MaxFacts = 20000;
    constexpr int32 MaxRecords = 8;
    constexpr int32 MaxGeometryCache = 64;
    constexpr int32 MaxInventoryObjects = 100000;
    constexpr int32 MaxSnapshotObjects = 20000;

    FJson Object(const FJson& Parent, const TCHAR* Key)
    {
        const FJson* Value = nullptr;
        return Parent && Parent->TryGetObjectField(Key, Value) ? *Value : nullptr;
    }
    FString String(const FJson& Parent, const TCHAR* Key)
    {
        FString Value; if (Parent) Parent->TryGetStringField(Key, Value); return Value;
    }
    TArray<FValue> Array(const FJson& Parent, const TCHAR* Key)
    {
        const TArray<FValue>* Value = nullptr;
        return Parent && Parent->TryGetArrayField(Key, Value) ? *Value : TArray<FValue>();
    }
    FValue Vec(const FVector& P) { return MakeShared<FJsonValueArray>(TArray<FValue>{
        MakeShared<FJsonValueNumber>(P.X), MakeShared<FJsonValueNumber>(P.Y), MakeShared<FJsonValueNumber>(P.Z)}); }
    FJson Box(const FBox& B)
    {
        auto O = MakeShared<FJsonObject>();
        O->SetField(TEXT("min_cm"), Vec(B.Min)); O->SetField(TEXT("max_cm"), Vec(B.Max)); return O;
    }
    FString Hash(const FString& Text)
    {
        FTCHARToUTF8 Utf8(*Text); uint8 Digest[20]; FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
        return BytesToHex(Digest, 20);
    }
    bool Fail(Contract::FDiagnostic& Error, const TCHAR* Code, const FString& Message)
    {
        Error = Contract::FDiagnostic::Failure(Code, Message, TEXT("scene_analysis")); return false;
    }
    struct FShape
    {
        FString Id, ActorId, ManagedId, ActorGuid, ComponentName, Class, MeshPath, Parent;
        FString Label, Folder, Representation, Unsupported, Dependencies, BindingDependencies, ContentFingerprint;
        FMatrix44d Matrix = FMatrix44d::Identity;
        FMatrix44d InstanceMatrix = FMatrix44d::Identity;
        FTransform ComponentWorld = FTransform::Identity, Relative = FTransform::Identity;
        FBox Bounds = FBox(ForceInit);
        FBox LocalBounds = FBox(ForceInit);
        FSharedGeometry Geometry;
        TWeakObjectPtr<USceneComponent> Component;
        TWeakObjectPtr<UStaticMesh> StaticMesh;
        uint64 ObjectVersion = 0;
        uint64 MeshVersion = 0;
        int32 Instance = INDEX_NONE;
        bool bVisible = true, bChanged = false, bDeleted = false, bGeometry = false;
        bool bRoot=false,bAbsoluteLocation=false,bAbsoluteRotation=false,bAbsoluteScale=false;
    };
    struct FRecord
    {
        FString Ref, RequestId, WorkspaceRoot, WorkspaceFingerprint, PreparedBundleFingerprint, WorldPath;
        TWeakObjectPtr<UWorld> World;
        TArray<FShape> Shapes;
        TArray<FBox> Regions;
        TMap<FString, FString> Baseline;
        TArray<FValue> Facts, SurfaceTotals;
        TArray<FValue> AppliedRecheck;
        FJson AppliedSummary;
        TArray<FString> Limitations;
        Composition::HjsonImport::FPreparedAuthoringImportPtr Prepared;
        double Margin = 0, Near = .01, CaptureMs = 0, ComputeMs = 0;
        int32 CandidatePairs = 0, AnalyzedPairs = 0, Changed = 0;
        bool bGeometryRequested = false, bPending = false, bComplete = false, bCancelled = false;
        bool bScopeUnknown = false;
        uint64 WorldVersion = 0, WorldSettingsVersion = 0;
        TAtomic<bool> bSuperseded{false};
        FString Failure;
        FCriticalSection Mutex;
    };
    struct FCache
    {
        struct FWorldIndex
        {
            TWeakObjectPtr<UWorld> World;
            TMap<FString,FShape> Shapes;
            TArray<FString> MinX;
        };
        struct FJob { FString RequestId; TFunction<bool()> IsCancelled; TFunction<void()> Complete; };
        FCriticalSection Mutex, Worker;
        TArray<FJob> Jobs;
        bool bWorkerActive=false;
        TAtomic<bool> bShuttingDown{false};
        TFuture<void> WorkerFuture;
        FDelegateHandle ModifiedHandle,PropertyHandle,TransactionHandle;
        TMap<FString, TSharedPtr<FRecord, ESPMode::ThreadSafe>> Records;
        TArray<FString> Order;
        TMap<FString, FString> Requests;
        TMap<FString, FSharedGeometry> Geometry;
        TArray<TPair<TWeakPtr<FGeometry,ESPMode::ThreadSafe>,int64>> Allocations;
        TMap<FString, FGeometryPairResult> PairResults;
        int64 PairBytes=0;
        TMap<FObjectKey, uint64> Versions;
        TArray<FWorldIndex> Worlds;
        uint64 Serial = 1;
        FCache()
        {
            ModifiedHandle=FCoreUObjectDelegates::OnObjectModified.AddLambda([this](UObject* Object)
            {
                if (Object) Versions.FindOrAdd(FObjectKey(Object)) = ++Serial;
            });
            PropertyHandle=FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([this](UObject* Object, FPropertyChangedEvent&)
            {
                if (Object) Versions.FindOrAdd(FObjectKey(Object)) = ++Serial;
            });
            TransactionHandle=FCoreUObjectDelegates::OnObjectTransacted.AddLambda([this](UObject* Object, const FTransactionObjectEvent&)
            {if(Object)Versions.FindOrAdd(FObjectKey(Object))=++Serial;});
        }
        uint64 Version(UObject* Object)
        {
            if(!Object)return 0;
            const FObjectKey Key(Object);if(const uint64* V=Versions.Find(Key))return *V;
            const uint64 V=++Serial;Versions.Add(Key,V);return V;
        }
    };
    FCache* GCache=nullptr;
    FCache& Cache() { if(!GCache)GCache=new FCache();return *GCache; }

    FString ShapeSignature(const FShape& S)
    {
        FString Text = S.Id + S.MeshPath + S.Parent + S.Class + S.Dependencies + S.ContentFingerprint
            + BytesToHex(reinterpret_cast<const uint8*>(&S.Matrix.M[0][0]),sizeof(double)*16)
            + BytesToHex(reinterpret_cast<const uint8*>(&S.Bounds.Min),sizeof(FVector))
            + BytesToHex(reinterpret_cast<const uint8*>(&S.Bounds.Max),sizeof(FVector))
            + FString::Printf(TEXT("|%llu|%d|%d|"), S.ObjectVersion, S.Instance, S.bVisible);
        return Hash(Text);
    }

    FString BindingSignature(const FShape& S)
    {
        // Persistent import reloads the World for independent readback. UObject
        // lifetimes invalidate caches, but are not semantic geometry changes.
        // Certify exact identities, measured state and asset dependencies instead.
        return Hash(S.Id+S.ActorGuid+S.ManagedId+S.MeshPath+S.Parent+S.Class+S.BindingDependencies+S.ContentFingerprint
            +BytesToHex(reinterpret_cast<const uint8*>(&S.Matrix.M[0][0]),sizeof(double)*16)
            +BytesToHex(reinterpret_cast<const uint8*>(&S.Bounds.Min),sizeof(FVector))
            +BytesToHex(reinterpret_cast<const uint8*>(&S.Bounds.Max),sizeof(FVector))
            +FString::Printf(TEXT("|%d|%d|"),S.Instance,S.bVisible));
    }

    bool CaptureShapes(UWorld& World, TArray<FShape>& Shapes)
    {
        check(IsInGameThread());
        for (TActorIterator<AActor> It(&World); It; ++It)
        {
            if(Shapes.Num()>=MaxInventoryObjects)return false;
            AActor* Actor = *It;
            TInlineComponentArray<USceneComponent*> Components(Actor);
            if(Components.IsEmpty())
            {
                FShape S;S.Id=Actor->GetPathName();S.ActorId=S.Id;S.ManagedId=UeAgentLevelAuthoring::GetManagedActorId(*Actor);
                S.ActorGuid=Actor->GetActorGuid().ToString(EGuidFormats::DigitsWithHyphens);S.Class=Actor->GetClass()->GetPathName();
                S.Label=Actor->GetActorLabel();S.Folder=Actor->GetFolderPath().ToString();S.ComponentWorld=Actor->GetActorTransform();
                S.Matrix=S.ComponentWorld.ToMatrixWithScale();S.Bounds=FBox(Actor->GetActorLocation(),Actor->GetActorLocation());
                S.ObjectVersion=Cache().Version(Actor);S.bRoot=true;Shapes.Add(MoveTemp(S));
            }
            for (USceneComponent* Component : Components)
            {
                if(Shapes.Num()>=MaxInventoryObjects)return false;
                if (!IsValid(Component) || Component->IsVisualizationComponent() || Component->IsA<UDebugDrawComponent>()) continue;
                FShape S;
                S.Id = Component->GetPathName(); S.ActorId = Actor->GetPathName();
                S.ManagedId = UeAgentLevelAuthoring::GetManagedActorId(*Actor);
                S.ActorGuid = Actor->GetActorGuid().ToString(EGuidFormats::DigitsWithHyphens);
                S.Label = Actor->GetActorLabel(); S.Folder = Actor->GetFolderPath().ToString();
                S.ComponentName = Component->GetName(); S.Class = Component->GetClass()->GetPathName();
                S.Component = Component; S.Parent = Component->GetAttachParent() ? Component->GetAttachParent()->GetPathName() : FString();
                S.ComponentWorld=Component->GetComponentTransform();S.Relative=Component->GetRelativeTransform();
                S.bRoot=Actor->GetRootComponent()==Component;
                S.bAbsoluteLocation=Component->IsUsingAbsoluteLocation();S.bAbsoluteRotation=Component->IsUsingAbsoluteRotation();S.bAbsoluteScale=Component->IsUsingAbsoluteScale();
                S.Matrix = Component->GetComponentTransform().ToMatrixWithScale();
                S.bVisible = Component->IsVisible() && !Actor->IsHiddenEd();
                S.ObjectVersion = HashCombineFast(Cache().Version(Actor), Cache().Version(Component));
                S.Dependencies=FString::Printf(TEXT("%llu|%llu"),Cache().Version(Actor),Cache().Version(Component));
                S.Dependencies+=FString::Printf(TEXT("|%llu|%llu"),Cache().Version(Actor->GetClass()),Cache().Version(Actor->GetClass()->ClassGeneratedBy));
                S.BindingDependencies=FString::Printf(TEXT("|%llu|%llu"),Cache().Version(Actor->GetClass()),Cache().Version(Actor->GetClass()->ClassGeneratedBy));
                S.Bounds = Component->Bounds.GetBox();
                if (!S.Bounds.IsValid) S.Bounds = FBox(Component->GetComponentLocation(), Component->GetComponentLocation());
                if (auto* Static = Cast<UStaticMeshComponent>(Component))
                {
                    S.StaticMesh = Static->GetStaticMesh();
                    if (UStaticMesh* Mesh = S.StaticMesh.Get())
                    {
                        S.MeshPath = Mesh->GetPathName(); S.LocalBounds = Mesh->GetBoundingBox();
                        S.ObjectVersion = HashCombineFast(S.ObjectVersion, Cache().Version(Mesh));
                        S.MeshVersion=Cache().Version(Mesh);
                        S.Dependencies+=FString::Printf(TEXT("|%llu"),Cache().Version(Mesh));
                        S.BindingDependencies+=FString::Printf(TEXT("|%llu"),Cache().Version(Mesh));
                        S.Bounds = S.LocalBounds.TransformBy(S.Matrix);
                        S.Representation = TEXT("static_mesh_source_lod0"); S.bGeometry = true;
                    }
                    for (int32 I = 0; I < Static->GetNumMaterials(); ++I)
                    {
                        S.ObjectVersion = HashCombineFast(S.ObjectVersion, Cache().Version(Static->GetMaterial(I)));
                        S.Dependencies+=FString::Printf(TEXT("|%llu"),Cache().Version(Static->GetMaterial(I)));
                        S.BindingDependencies+=FString::Printf(TEXT("|%llu"),Cache().Version(Static->GetMaterial(I)));
                    }
                    if (auto* Instances = Cast<UInstancedStaticMeshComponent>(Static))
                    {
                        for (int32 I = 0; I < Instances->GetInstanceCount(); ++I)
                        {
                            if(Shapes.Num()>=MaxInventoryObjects)return false;
                            FTransform Instance;
                            FShape Row = S; Row.Instance = I; Row.Id += FString::Printf(TEXT("#instance:%d"), I);
                            if (!Instances->GetInstanceTransform(I, Instance, false)) Row.Unsupported = TEXT("instance_transform_unavailable");
                            else { Row.InstanceMatrix=Instance.ToMatrixWithScale();Row.Matrix = Row.InstanceMatrix * S.Matrix; Row.Bounds = S.LocalBounds.TransformBy(Row.Matrix); }
                            Shapes.Add(MoveTemp(Row));
                        }
                        if (Instances->GetInstanceCount() == 0) { S.bGeometry = false; Shapes.Add(MoveTemp(S)); }
                        continue;
                    }
                }
                else if (Cast<UDynamicMeshComponent>(Component)) { S.bGeometry = true; S.Representation = TEXT("dynamic_mesh_snapshot"); }
                else if (Cast<UPrimitiveComponent>(Component)) { S.bGeometry = true; S.Unsupported = TEXT("component_geometry_provider_unavailable"); }
                Shapes.Add(MoveTemp(S));
            }
        }
        Shapes.Sort([](const FShape& A, const FShape& B) { return A.Id < B.Id; });
        return true;
    }

    // Keep component/instance membership and bounds independently of collision. A
    // cheap live inventory also catches edits that bypass UObject notifications;
    // reflection, mesh extraction and pair comparison stay local to the query.
    FCache::FWorldIndex* RefreshIndex(UWorld& World,TArray<FShape>& Snapshot)
    {
        if(!CaptureShapes(World,Snapshot))return nullptr;
        auto& Worlds=Cache().Worlds;
        Worlds.RemoveAll([](const auto& I){return !I.World.IsValid();});
        auto* Index=Worlds.FindByPredicate([&](const auto& I){return I.World.Get()==&World;});
        if(!Index){Index=&Worlds.AddDefaulted_GetRef();Index->World=&World;}
        TSet<FString> Seen;bool BoundsChanged=false;
        for(const auto& S:Snapshot)
        {
            Seen.Add(S.Id);const auto* Old=Index->Shapes.Find(S.Id);
            if(!Old || Old->Bounds!=S.Bounds)BoundsChanged=true;
            if(!Old || ShapeSignature(*Old)!=ShapeSignature(S))Index->Shapes.Add(S.Id,S);
        }
        for(auto It=Index->Shapes.CreateIterator();It;++It)if(!Seen.Contains(It.Key())){It.RemoveCurrent();BoundsChanged=true;}
        if(BoundsChanged)
        {
            Index->Shapes.GetKeys(Index->MinX);
            Index->MinX.Sort([&](const FString& A,const FString& B){return Index->Shapes.FindChecked(A).Bounds.Min.X<Index->Shapes.FindChecked(B).Bounds.Min.X;});
        }
        return Index;
    }

    TSet<FString> Neighbors(const FCache::FWorldIndex& Index,const TArray<FBox>& Regions,double Margin)
    {
        TSet<FString> Result;
        for(const auto& Region:Regions)
        {
            const FBox Search=Region.ExpandBy(Margin);
            for(const FString& Id:Index.MinX)
            {
                const auto& S=Index.Shapes.FindChecked(Id);
                if(S.Bounds.Min.X>Search.Max.X)break;
                if(S.Bounds.Intersect(Search))Result.Add(Id);
            }
        }
        return Result;
    }

    FSharedGeometry CaptureMesh(FShape& Shape, int64& CapturedVertices)
    {
        check(IsInGameThread());
        if (!Shape.bGeometry || !Shape.bVisible || !Shape.Unsupported.IsEmpty()) return nullptr;
        auto& C = Cache();
        const FString Key = Shape.MeshPath.IsEmpty() ? Shape.Id + FString::Printf(TEXT("|%llu"), Shape.ObjectVersion)
            : Shape.MeshPath + FString::Printf(TEXT("|%llu"), C.Version(Shape.StaticMesh.Get()));
        if (FSharedGeometry* Existing = C.Geometry.Find(Key)) return *Existing;
        FSharedGeometry G = MakeShared<FGeometry, ESPMode::ThreadSafe>();
        G->Mesh.Revision=++C.Serial;
        auto ReserveGeometry=[&](int64 Vertices,int64 Triangles)
        {
            C.Allocations.RemoveAll([](const auto& A){return !A.Key.IsValid();});
            const int64 Estimate=Vertices*32+Triangles*192;int64 Used=0;for(const auto& A:C.Allocations)Used+=A.Value;
            if(CapturedVertices+Vertices>MaxCapturedVertices || Used+Estimate>MaxGeometryBytes)
            {
                C.Geometry.Empty();
                C.Allocations.RemoveAll([](const auto& A){return !A.Key.IsValid();});Used=0;for(const auto& A:C.Allocations)Used+=A.Value;
                if(CapturedVertices+Vertices>MaxCapturedVertices || Used+Estimate>MaxGeometryBytes)
                {Shape.Unsupported=TEXT("geometry_memory_budget_exceeded");return false;}
            }
            C.Allocations.Add({G,Estimate});return true;
        };
        if (UStaticMesh* Static = Shape.StaticMesh.Get())
        {
            const FMeshDescription* Description = Static->GetMeshDescription(0);
            if (!Description) { Shape.Unsupported = TEXT("source_lod0_unavailable"); return nullptr; }
            if (!ReserveGeometry(Description->Vertices().Num(),Description->Triangles().Num())) return nullptr;
            FStaticMeshConstAttributes Attributes(*Description);
            auto Positions = Attributes.GetVertexPositions();
            TMap<FVertexID, int32> Indices;
            for (FVertexID Vertex : Description->Vertices().GetElementIDs())
                Indices.Add(Vertex, G->Mesh.Vertices.Add(FVector3d(Positions[Vertex])));
            for (FTriangleID Triangle : Description->Triangles().GetElementIDs())
            {
                const auto Vertices = Description->GetTriangleVertices(Triangle);
                G->Mesh.Triangles.Add(UE::Geometry::FIndex3i(Indices[Vertices[0]], Indices[Vertices[1]], Indices[Vertices[2]]));
            }
            G->Limitations.Add(TEXT("source_lod0_not_final_render_lod_nanite_or_material_deformation"));
        }
        else if (auto* Dynamic = Cast<UDynamicMeshComponent>(Shape.Component.Get()))
        {
            Dynamic->GetDynamicMesh()->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
            {
                if (!ReserveGeometry(Mesh.VertexCount(),Mesh.TriangleCount())) return;
                TMap<int32,int32> Indices;
                for (int32 V : Mesh.VertexIndicesItr()) Indices.Add(V, G->Mesh.Vertices.Add(Mesh.GetVertex(V)));
                for (int32 T : Mesh.TriangleIndicesItr()) { const auto V = Mesh.GetTriangle(T); G->Mesh.Triangles.Add({Indices[V.A], Indices[V.B], Indices[V.C]}); }
            });
            // Dynamic meshes can be mutated without UObject property notifications.
            G->Limitations.Add(TEXT("dynamic_snapshot_requires_recapture"));
        }
        if (G->Mesh.Vertices.IsEmpty()) { Shape.Unsupported = TEXT("empty_or_unavailable_geometry"); return nullptr; }
        CapturedVertices += G->Mesh.Vertices.Num();
        if (C.Geometry.Num() >= MaxGeometryCache) C.Geometry.Empty();
        if (!Shape.MeshPath.IsEmpty()) C.Geometry.Add(Key, G);
        return G;
    }

    bool CaptureMutableFingerprint(FShape& Shape)
    {
        auto* Dynamic=Cast<UDynamicMeshComponent>(Shape.Component.Get());if(!Dynamic)return true;
        bool Complete=true;FSHA1 Digest;
        Dynamic->GetDynamicMesh()->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
        {
            if(Mesh.VertexCount()>MaxCapturedVertices || Mesh.TriangleCount()>MaxCapturedVertices*4)
            {Complete=false;return;}
            for(int32 Id:Mesh.VertexIndicesItr())
            {const auto P=Mesh.GetVertex(Id);Digest.Update(reinterpret_cast<const uint8*>(&Id),sizeof(Id));Digest.Update(reinterpret_cast<const uint8*>(&P),sizeof(P));}
            for(int32 Id:Mesh.TriangleIndicesItr())
            {const auto T=Mesh.GetTriangle(Id);Digest.Update(reinterpret_cast<const uint8*>(&Id),sizeof(Id));Digest.Update(reinterpret_cast<const uint8*>(&T),sizeof(T));}
        });
        if(!Complete){Shape.Unsupported=TEXT("mutable_geometry_version_unavailable_within_budget");return false;}
        uint8 Bytes[20];Digest.Final();Digest.GetHash(Bytes);Shape.ContentFingerprint=BytesToHex(Bytes,20);return true;
    }

    bool PatchTransform(const FJson& Value, UeAgentLevelAuthoring::ETransformFrame Frame,
        FTransform& Transform, Contract::FDiagnostic& Error)
    {
        if (!Value) return true;
        UeAgentLevelAuthoring::FTransformPatch Patch;
        if (!UeAgentLevelAuthoring::ParseTransformPatch(Value, Frame, Patch, Error)) return false;
        UeAgentLevelAuthoring::ApplyTransformPatch(Patch, Transform); return true;
    }
    void Change(FRecord& R, FShape& S)
    {
        if (!S.bChanged) { R.Regions.Add(S.Bounds); S.bChanged = true; ++R.Changed; }
    }
    bool ProjectBundle(UWorld& World, const FJson& Bundle, FRecord& R, Contract::FDiagnostic& Error)
    {
        const auto Units = Object(Bundle, TEXT("units"));
        if (!Units) return Fail(Error, TEXT("scene_bundle_missing"), TEXT("Prepared Level bundle has no units."));
        // Actor transforms precede relative component transforms, independent of JSON ordering.
        TArray<FJson> Rows;
        TSet<FString> WorldOverrides,RelativeEdits;
        for (const auto& Pair : Units->Values) if (Pair.Value->Type == EJson::Object) Rows.Add(Pair.Value->AsObject());
        Rows.Sort([](const FJson& A, const FJson& B) { return String(Object(A,TEXT("target")),TEXT("component_name")).IsEmpty() && !String(Object(B,TEXT("target")),TEXT("component_name")).IsEmpty(); });
        for (const FJson& Row : Rows)
        {
            FJson Target = Object(Row,TEXT("target")), Extension = Object(Row,TEXT("provider_extension"));
            if (!Target) continue;
            const FString Component = String(Target,TEXT("component_name"));
            const FString Managed = String(Target, Component.IsEmpty() ? TEXT("managed_id") : TEXT("owner_managed_id"));
            const FString Guid = String(Target, Component.IsEmpty() ? TEXT("actor_guid") : TEXT("owner_actor_guid"));
            const FString Path = String(Target,TEXT("object_path"));
            const FString Operation = String(Row,TEXT("operation"));
            if (Managed.IsEmpty() && Guid.IsEmpty() && Path.IsEmpty())
            {
                // The unchanged root unit is also present in every bundle. Only
                // actual WorldSettings/topology edits have properties/extension.
                if(!Array(Row,TEXT("properties")).IsEmpty() || (Extension && !Extension->Values.IsEmpty()))
                {R.bScopeUnknown=true;R.Limitations.AddUnique(TEXT("world_settings_or_topology_effects_not_predicted"));}
                continue;
            }
            TArray<int32> Matches;
            for (int32 I = 0; I < R.Shapes.Num(); ++I)
            {
                const auto& S = R.Shapes[I];
                if ((!Managed.IsEmpty() && S.ManagedId != Managed) || (!Guid.IsEmpty() && !S.ActorGuid.Equals(Guid,ESearchCase::IgnoreCase))
                    || (!Path.IsEmpty() && S.ActorId != Path) || (!Component.IsEmpty() && S.ComponentName != Component)) continue;
                Matches.Add(I);
            }
            TMap<FString,FString> Placement;
            if (Component.IsEmpty() && !UeAgentLevelAuthoring::ResolvePlacementSelectors(Extension,Placement,Error)) return false;
            if (Matches.IsEmpty() && Operation != TEXT("remove"))
            {
                FShape S; S.ActorId = TEXT("proposed:") + (!Managed.IsEmpty() ? Managed : Guid);
                S.ManagedId = Managed; S.ActorGuid = Guid; S.ComponentName = Component;
                S.Id = S.ActorId + TEXT(".") + Component; S.Class = String(Target,Component.IsEmpty() ? TEXT("actor_class") : TEXT("component_class"));
                S.bGeometry = true; S.Unsupported = TEXT("native_creation_not_yet_predictable");
                if(Component.IsEmpty() && (S.Class==TEXT("/Script/Engine.StaticMeshActor") || Placement.FindRef(TEXT("actor_class"))==TEXT("/Script/Engine.StaticMeshActor")))
                {
                    S.ComponentName=TEXT("StaticMeshComponent0"); S.Id=S.ActorId+TEXT(".")+S.ComponentName;
                    S.Class=TEXT("/Script/Engine.StaticMeshComponent");S.bRoot=true;
                }
                S.Bounds = FBox(FVector::ZeroVector,FVector::ZeroVector); S.bChanged = true; ++R.Changed;
                Matches.Add(R.Shapes.Add(MoveTemp(S)));
            }
            for (int32 Index : Matches)
            {
                auto& S = R.Shapes[Index]; Change(R,S);
                if (Operation == TEXT("remove")) { S.bDeleted = true; continue; }
                if (Component.IsEmpty())
                {
                    AActor* Actor = S.Component.IsValid() ? S.Component->GetOwner() : nullptr;
                    FTransform Old = Actor ? Actor->GetActorTransform() : FTransform::Identity, New = Old;
                    if (!PatchTransform(Object(Extension,TEXT("transform")), UeAgentLevelAuthoring::ETransformFrame::ActorWorld, New, Error)) return false;
                    if(S.bRoot && Object(Extension,TEXT("transform"))) {S.ComponentWorld=New;WorldOverrides.Add(S.Id);}
                    if (const FJson Attachment=Object(Extension,TEXT("attachment")))
                    {
                        const FString ParentId=String(Attachment,TEXT("parent_managed_id")).TrimStartAndEnd();
                        const FString Socket=String(Attachment,TEXT("socket_name")).TrimStartAndEnd();
                        const AActor* CurrentParent=Actor?Actor->GetAttachParentActor():nullptr;
                        const FString CurrentId=CurrentParent?UeAgentLevelAuthoring::GetManagedActorId(*CurrentParent):FString();
                        // Formal Apply detaches an empty parent with KeepWorldTransform.
                        // An unchanged exported attachment must not turn an ordinary
                        // actor transform into an unsupported construction prediction.
                        if(ParentId.IsEmpty())
                        {
                            if(S.bRoot){S.Parent.Empty();S.ComponentWorld=New;WorldOverrides.Add(S.Id);}
                        }
                        else if(!Actor || !CurrentId.Equals(ParentId,ESearchCase::IgnoreCase)
                            || Actor->GetAttachParentSocketName()!=FName(*Socket))
                            S.Unsupported=TEXT("actor_attachment_preview_pending");
                    }
                    if (Object(Extension,TEXT("candidate")))
                    {
                        const FString Asset=Placement.FindRef(TEXT("asset_override"));
                        UStaticMesh* Mesh=LoadObject<UStaticMesh>(nullptr,*Asset);
                        if(Placement.FindRef(TEXT("actor_class"))==TEXT("/Script/Engine.StaticMeshActor") && Mesh)
                        { S.StaticMesh=Mesh;S.MeshPath=Asset;S.LocalBounds=Mesh->GetBoundingBox();S.Geometry.Reset();S.Unsupported.Empty();S.Representation=TEXT("static_mesh_source_lod0"); }
                        else S.Unsupported=TEXT("candidate_native_construction_unpredicted");
                    }
                    if (!String(Extension,TEXT("label")).IsEmpty()) S.Label = String(Extension,TEXT("label"));
                }
                else
                {
                    const FJson Attachment = Object(Extension,TEXT("attachment"));
                    if (Attachment)
                    {
                        if (!PatchTransform(Object(Attachment,TEXT("transform")), UeAgentLevelAuthoring::ETransformFrame::ComponentParentRelative, S.Relative, Error)) return false;
                        RelativeEdits.Add(S.Id);WorldOverrides.Remove(S.Id);
                        const FString ParentName = String(Attachment,TEXT("parent_component"));
                        const FShape* Parent = R.Shapes.FindByPredicate([&](const FShape& P) { return P.ActorId == S.ActorId && P.ComponentName == ParentName; });
                        if (!String(Attachment,TEXT("socket_name")).IsEmpty()) S.Unsupported = TEXT("socket_transform_preview_unavailable");
                        else if (Parent) S.Parent=Parent->Id;
                        else if (ParentName.IsEmpty()) S.Parent.Empty();
                        else S.Unsupported = TEXT("attachment_parent_unresolved");
                    }
                    const FString Asset = String(Extension,TEXT("asset_override"));
                    if (!Asset.IsEmpty())
                    {
                        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr,*Asset);
                        if (Mesh && S.Class.Contains(TEXT("StaticMeshComponent")))
                        { S.StaticMesh=Mesh; S.MeshPath=Asset; S.LocalBounds=Mesh->GetBoundingBox(); S.Geometry.Reset(); S.Unsupported.Empty(); S.Representation=TEXT("static_mesh_source_lod0"); }
                        else S.Unsupported=TEXT("asset_brokerage_preview_unavailable");
                    }
                }
                for (const auto& Property : Array(Row,TEXT("properties")))
                {
                    const auto P = Property->AsObject(); const FString Name = String(P,TEXT("property_path"));
                    const FValue Value = P->TryGetField(TEXT("value"));
                    if (Name==TEXT("bVisible") && Value && Value->Type==EJson::Boolean) S.bVisible=Value->AsBool();
                    else if (Name==TEXT("StaticMesh") && Value)
                    {
                        const FString Asset=Value->Type==EJson::String?Value->AsString():Value->Type==EJson::Object?String(Value->AsObject(),TEXT("path")):FString();
                        UStaticMesh* Mesh=Asset.IsEmpty()?nullptr:LoadObject<UStaticMesh>(nullptr,*Asset);
                        if(Mesh && S.Class.Contains(TEXT("StaticMeshComponent")))
                        {S.StaticMesh=Mesh;S.MeshPath=Asset;S.LocalBounds=Mesh->GetBoundingBox();S.Geometry.Reset();S.Unsupported.Empty();S.Representation=TEXT("static_mesh_source_lod0");}
                        else S.Unsupported=TEXT("reflected_mesh_reference_unresolved");
                    }
                    else if (Name.Contains(TEXT("Material"))) R.Limitations.AddUnique(TEXT("material_changes_not_final_render_geometry"));
                    else if (Name!=TEXT("bHiddenInGame") && Name!=TEXT("CastShadow") && !Name.Contains(TEXT("Collision"))) S.Unsupported=TEXT("native_property_effect_not_predictable");
                }
            }
        }
        // Use UE's SRT composition for SceneComponent attachment (instances retain
        // their native matrix composition). This also propagates across actor ownership.
        TArray<uint8> Visited;Visited.Init(0,R.Shapes.Num());
        TFunction<void(int32)> Resolve=[&](int32 Index)
        {
            if(Visited[Index]==2)return;
            auto& S=R.Shapes[Index];
            if(Visited[Index]==1){S.Unsupported=TEXT("attachment_cycle");return;}
            Visited[Index]=1;
            if(!WorldOverrides.Contains(S.Id))
            {
                const int32 Parent=R.Shapes.IndexOfByPredicate([&](const auto& P)
                {return !S.Parent.IsEmpty() && (P.Id==S.Parent || (P.Instance!=INDEX_NONE && P.Id.StartsWith(S.Parent+TEXT("#instance:"))));});
                if(Parent!=INDEX_NONE)
                {
                    Resolve(Parent);
                    const auto& P=R.Shapes[Parent];
                    if(P.bDeleted){Change(R,S);S.Unsupported=TEXT("attachment_parent_deleted_native_result_unpredicted");}
                    else if(P.bChanged || RelativeEdits.Contains(S.Id))
                    {
                        Change(R,S);S.ComponentWorld=S.Relative*P.ComponentWorld;
                        if(S.bAbsoluteLocation)S.ComponentWorld.SetTranslation(S.Relative.GetTranslation());
                        if(S.bAbsoluteRotation)S.ComponentWorld.SetRotation(S.Relative.GetRotation());
                        if(S.bAbsoluteScale)S.ComponentWorld.SetScale3D(S.Relative.GetScale3D());
                    }
                }
                else if(RelativeEdits.Contains(S.Id))S.ComponentWorld=S.Relative;
            }
            if(S.bChanged && !S.bDeleted)
            {
                if(S.StaticMesh.IsValid())S.MeshVersion=Cache().Version(S.StaticMesh.Get());
                S.Matrix=S.InstanceMatrix*S.ComponentWorld.ToMatrixWithScale();
                if(S.LocalBounds.IsValid)S.Bounds=S.LocalBounds.TransformBy(S.Matrix);
                R.Regions.Add(S.Bounds);
            }
            Visited[Index]=2;
        };
        for(int32 I=0;I<R.Shapes.Num();++I)Resolve(I);
        for(const auto& S:R.Shapes)if(S.bChanged &&
            (S.Unsupported.Contains(TEXT("construction")) || S.Unsupported.Contains(TEXT("creation")) || S.Unsupported==TEXT("native_property_effect_not_predictable")))
            R.bScopeUnknown=true;
        if(R.bScopeUnknown)R.Limitations.AddUnique(TEXT("affected_scope_not_fully_predicted"));
        return true;
    }

    bool InRegions(const FShape& S, const TArray<FBox>& Regions, double Margin)
    {
        for (const FBox& Region : Regions) if (Region.ExpandBy(Margin).Intersect(S.Bounds)) return true;
        return false;
    }
    bool ReadRegions(const FJson& Input, TArray<FBox>& Regions, Contract::FDiagnostic& Error)
    {
        for (const FValue& Value : Array(Input,TEXT("regions")))
        {
            const auto O=Value->AsObject(); const auto Min=Array(O,TEXT("min_cm")),Max=Array(O,TEXT("max_cm"));
            if (Min.Num()!=3 || Max.Num()!=3) return Fail(Error,TEXT("scene_region_invalid"),TEXT("Region requires min_cm/max_cm triples."));
            const FVector A(Min[0]->AsNumber(),Min[1]->AsNumber(),Min[2]->AsNumber()), B(Max[0]->AsNumber(),Max[1]->AsNumber(),Max[2]->AsNumber());
            if (A.ContainsNaN() || B.ContainsNaN() || A.X>B.X || A.Y>B.Y || A.Z>B.Z) return Fail(Error,TEXT("scene_region_invalid"),TEXT("Region endpoints must be finite and ordered."));
            Regions.Add(FBox(A,B));
        }
        return true;
    }

    bool Prepare(UWorld& World, const FString& RequestId, const FJson& Input, bool Geometry,
        TSharedPtr<FRecord,ESPMode::ThreadSafe>& Out, Contract::FDiagnostic& Error)
    {
        const double Started=FPlatformTime::Seconds();
        auto R=MakeShared<FRecord,ESPMode::ThreadSafe>(); R->World=&World; R->WorldPath=World.GetPathName();
        R->Limitations.Add(TEXT("editor_debug_draw_and_visualization_components_excluded"));
        if(World.IsPartitionedWorld())R->Limitations.Add(TEXT("loaded_world_partition_cells_only"));
        R->Ref=FGuid::NewGuid().ToString(EGuidFormats::Digits); R->RequestId=RequestId; R->bGeometryRequested=Geometry;
        R->WorkspaceRoot=String(Input,TEXT("workspace_root"));
        if(!R->WorkspaceRoot.IsEmpty())R->WorkspaceRoot=UeAgentAuthoringWorkspacePaths::NormalizeAbsolutePath(R->WorkspaceRoot);
        Input->TryGetNumberField(TEXT("margin_cm"),R->Margin);
        Input->TryGetNumberField(TEXT("near_distance_cm"),R->Near);
        auto* Index=RefreshIndex(World,R->Shapes);
        if(!Index)return Fail(Error,TEXT("scene_inventory_budget_exceeded"),TEXT("Component/instance inventory exceeded the bounded snapshot budget; no complete context or geometry result was produced."));
        TMap<FString,FShape> OriginalMutable;
        for(const auto& S:R->Shapes)if(S.Representation==TEXT("dynamic_mesh_snapshot"))OriginalMutable.Add(S.Id,S);
        for (const auto& S:R->Shapes) R->Baseline.Add(S.Id,BindingSignature(S));
        if (!R->WorkspaceRoot.IsEmpty())
        {
            auto Bundle=MakeShared<FJsonObject>(); HjsonWorkspace::FWorkspaceError E;
            if (!DomainAdapters::WorldSpatial::TryPrepareLevelWorkspace(World,R->WorkspaceRoot,TEXT("scene_editing"),R->Prepared,Bundle,E)) { Error=E.GetDiagnostic(); return false; }
            R->WorkspaceFingerprint=R->Prepared->GetWorkspace()->GetWorkspaceFingerprint();
            FString CanonicalBundle;
            if(!Contract::Json::TryCanonicalize(Bundle,CanonicalBundle,Error))return false;
            R->PreparedBundleFingerprint=Hash(CanonicalBundle);
            if (!ProjectBundle(World,Bundle,*R,Error)) return false;
        }
        R->WorldVersion=Cache().Version(&World);R->WorldSettingsVersion=Cache().Version(World.GetWorldSettings());
        if (!ReadRegions(Input,R->Regions,Error)) return false;
        const auto Targets=Array(Input,TEXT("targets"));
        for (const auto& Target:Targets)
        {
            bool Found=false;
            for (auto& S:R->Shapes) if (S.Id==Target->AsString() || S.ActorId==Target->AsString()) { Found=true; Change(*R,S); }
            if (!Found) return Fail(Error,TEXT("scene_target_not_found"),Target->AsString());
        }
        if (R->WorkspaceRoot.IsEmpty() && Targets.IsEmpty() && R->Regions.IsEmpty())
            return Fail(Error,TEXT("scene_scope_required"),TEXT("Supply workspace_root, exact targets, regions, or an existing analysis_ref."));
        if (R->WorkspaceRoot.IsEmpty() && Targets.IsEmpty()) for (auto& S:R->Shapes) if (InRegions(S,R->Regions,0)) { S.bChanged=true; ++R->Changed; }
        const auto Nearby=Neighbors(*Index,R->Regions,R->Margin+R->Near);
        R->Shapes.RemoveAll([&](const FShape& S){return !S.bChanged && !Nearby.Contains(S.Id);});
        if(R->Shapes.Num()>MaxSnapshotObjects)return Fail(Error,TEXT("scene_snapshot_budget_exceeded"),TEXT("Affected neighborhood exceeds the snapshot memory budget; split the edit or narrow the requested region."));
        for(auto& S:R->Shapes)
        {
            CaptureMutableFingerprint(S);
            if(!S.ContentFingerprint.IsEmpty())
            {
                // Bind the old live geometry, even when its proposed transform differs.
                if(FShape* Live=OriginalMutable.Find(S.Id))
                {Live->ContentFingerprint=S.ContentFingerprint;R->Baseline.Add(S.Id,BindingSignature(*Live));}
            }
        }
        int64 Vertices=0;
        if (Geometry) for (auto& S:R->Shapes) if (!S.bDeleted) S.Geometry=CaptureMesh(S,Vertices);
        R->bPending=Geometry; R->bComplete=!Geometry; R->CaptureMs=(FPlatformTime::Seconds()-Started)*1000;
        Out=R; return true;
    }

    bool CheckCurrent(FRecord& R, UWorld& CurrentWorld, Contract::FDiagnostic& Error)
    {
        UWorld* World=&CurrentWorld;
        if(World->GetPathName()!=R.WorldPath)return Fail(Error,TEXT("scene_analysis_stale"),TEXT("The current World identity differs from the analyzed World."));
        if(R.bScopeUnknown && (Cache().Version(World)!=R.WorldVersion || Cache().Version(World->GetWorldSettings())!=R.WorldSettingsVersion))
            return Fail(Error,TEXT("scene_analysis_stale"),TEXT("World state changed while the proposed effect has no predictable local scope."));
        TArray<FShape> Current;auto* Index=RefreshIndex(*World,Current);
        if(!Index)return Fail(Error,TEXT("scene_analysis_version_unverifiable"),TEXT("Live component/instance inventory exceeded the bounded snapshot budget."));
        const auto Nearby=Neighbors(*Index,R.Regions,R.Margin+R.Near);
        TSet<FString> Seen;
        for (const auto& S:Current)
        {
            const bool Captured=R.Shapes.ContainsByPredicate([&](const FShape& P){return P.Id==S.Id;});
            const bool Relevant=R.bScopeUnknown || Nearby.Contains(S.Id) || Captured;
            if (!Relevant) continue;
            if(R.bScopeUnknown && !Captured && S.Representation==TEXT("dynamic_mesh_snapshot"))
                return Fail(Error,TEXT("scene_analysis_version_unverifiable"),TEXT("An unbounded proposed effect may depend on mutable geometry outside the captured scope: ")+S.Id);
            FShape Live=S;
            if(!CaptureMutableFingerprint(Live))return Fail(Error,TEXT("scene_analysis_version_unverifiable"),TEXT("Cannot certify mutable geometry: ")+S.Id);
            Seen.Add(S.Id); const FString* Expected=R.Baseline.Find(S.Id);
            if (!Expected || *Expected!=BindingSignature(Live)) return Fail(Error,TEXT("scene_analysis_stale"),TEXT("Related live object or neighborhood membership changed: ")+S.Id);
            // Preserve proposed state while rebinding only its native providers.
            for(auto& Proposed:R.Shapes)if(Proposed.Id==S.Id)
            {Proposed.Component=Live.Component;Proposed.StaticMesh=Live.StaticMesh;}
        }
        for (const auto& S:R.Shapes) if (R.Baseline.Contains(S.Id) && !Seen.Contains(S.Id)) return Fail(Error,TEXT("scene_analysis_stale"),TEXT("Related live object disappeared: ")+S.Id);
        if(R.bScopeUnknown)for(const auto& Entry:R.Baseline)if(!Seen.Contains(Entry.Key))
            return Fail(Error,TEXT("scene_analysis_stale"),TEXT("World membership changed while the proposed effect has no predictable local scope."));
        R.World=World;
        return true;
    }
    FJson ShapeJson(const FShape& S)
    {
        auto O=MakeShared<FJsonObject>(); O->SetStringField(TEXT("object_ref"),S.Id); O->SetStringField(TEXT("actor_ref"),S.ActorId);
        O->SetStringField(TEXT("label"),S.Label); O->SetStringField(TEXT("folder"),S.Folder); O->SetStringField(TEXT("class"),S.Class);
        O->SetStringField(TEXT("mesh"),S.MeshPath); O->SetStringField(TEXT("parent_ref"),S.Parent); O->SetBoolField(TEXT("visible"),S.bVisible);
        O->SetStringField(TEXT("change"),S.bDeleted?TEXT("removed"):S.bChanged?TEXT("changed"):TEXT("neighbor"));
        O->SetObjectField(TEXT("bounds_cm"),Box(S.Bounds));
        TArray<FValue> Matrix; for(int32 I=0;I<4;++I) for(int32 J=0;J<4;++J) Matrix.Add(MakeShared<FJsonValueNumber>(S.Matrix.M[I][J]));
        O->SetArrayField(TEXT("world_matrix_row_major"),Matrix);
        O->SetStringField(TEXT("representation"),S.Representation); O->SetStringField(TEXT("unsupported"),S.Unsupported);
        if(S.Instance!=INDEX_NONE) O->SetNumberField(TEXT("instance_index"),S.Instance);
        return O;
    }
    FJson Project(const FRecord& R,const FJson& Input,bool Geometry)
    {
        auto O=MakeShared<FJsonObject>(); O->SetStringField(TEXT("analysis_ref"),R.Ref);
        O->SetStringField(TEXT("status"),R.bCancelled?TEXT("cancelled"):!R.Failure.IsEmpty()?TEXT("failed"):R.bPending?TEXT("pending"):TEXT("complete"));
        O->SetStringField(TEXT("world"),R.WorldPath); O->SetStringField(TEXT("state"),R.WorkspaceRoot.IsEmpty()?TEXT("current"):TEXT("proposed"));
        O->SetStringField(TEXT("interpretation"),TEXT("geometry_facts_not_design_validity_or_rendered_z_fighting"));
        O->SetBoolField(TEXT("affected_scope_unknown"),R.bScopeUnknown);
        O->SetNumberField(TEXT("changed_count"),R.Changed); O->SetNumberField(TEXT("object_count"),R.Shapes.Num());
        O->SetNumberField(TEXT("candidate_pairs"),R.CandidatePairs); O->SetNumberField(TEXT("analyzed_pairs"),R.AnalyzedPairs);
        O->SetNumberField(TEXT("capture_ms"),R.CaptureMs); O->SetNumberField(TEXT("compute_ms"),R.ComputeMs);
        TArray<FValue> Limitations; for(const FString& L:R.Limitations) Limitations.Add(MakeShared<FJsonValueString>(L));
        O->SetArrayField(TEXT("limitations"),Limitations); if(!R.Failure.IsEmpty()) O->SetStringField(TEXT("failure"),R.Failure);
        int32 Offset=0,Limit=40; Input->TryGetNumberField(TEXT("offset"),Offset); Input->TryGetNumberField(TEXT("limit"),Limit);
        Offset=FMath::Max(0,Offset); Limit=FMath::Clamp(Limit,1,200);
        TArray<FValue> Rows;
        const FString Exact=String(Input,TEXT("object_ref"));
        if(R.AppliedSummary)
        {
            O->SetObjectField(TEXT("applied_verification"),R.AppliedSummary);
            TArray<FValue> All;for(const auto& Row:R.AppliedRecheck)
                if(Exact.IsEmpty() || String(Row->AsObject(),TEXT("object_ref"))==Exact)All.Add(Row);
            TArray<FValue> Recheck;for(int32 I=Offset;I<FMath::Min(Offset+Limit,All.Num());++I)Recheck.Add(All[I]);
            O->SetNumberField(TEXT("recheck_total"),All.Num());O->SetArrayField(TEXT("recheck_objects"),Recheck);
            O->SetBoolField(TEXT("recheck_has_more"),Offset+Recheck.Num()<All.Num());
            O->SetBoolField(TEXT("historical_analyzed_snapshot"),true);
        }
        if(Geometry) { for(const FValue& Fact:R.Facts) if(Exact.IsEmpty() || String(Fact->AsObject(),TEXT("a"))==Exact || String(Fact->AsObject(),TEXT("b"))==Exact) Rows.Add(Fact); }
        else for(const FShape& S:R.Shapes) if(Exact.IsEmpty() || S.Id==Exact) Rows.Add(MakeShared<FJsonValueObject>(ShapeJson(S)));
        TArray<FValue> Page; for(int32 I=Offset;I<FMath::Min(Offset+Limit,Rows.Num());++I) Page.Add(Rows[I]);
        O->SetNumberField(TEXT("total"),Rows.Num()); O->SetNumberField(TEXT("offset"),Offset); O->SetNumberField(TEXT("returned"),Page.Num());
        O->SetBoolField(TEXT("has_more"),Offset+Page.Num()<Rows.Num()); O->SetArrayField(Geometry?TEXT("facts"):TEXT("objects"),Page);
        if(Geometry)
        {
            TArray<FValue> Totals;for(const auto& Total:R.SurfaceTotals)
                if(Exact.IsEmpty() || String(Total->AsObject(),TEXT("object_ref"))==Exact) Totals.Add(Total);
            TArray<FValue> TotalPage;for(int32 I=Offset;I<FMath::Min(Offset+Limit,Totals.Num());++I)TotalPage.Add(Totals[I]);
            O->SetNumberField(TEXT("surface_total_count"),Totals.Num());O->SetArrayField(TEXT("surface_totals"),TotalPage);
            O->SetBoolField(TEXT("surface_totals_has_more"),Offset+TotalPage.Num()<Totals.Num());
            TArray<FValue> Coverage;
            for(const auto& S:R.Shapes)
            {
                if(!Exact.IsEmpty() && S.Id!=Exact)continue;
                auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("object_ref"),S.Id);
                Row->SetStringField(TEXT("state"),S.bDeleted?TEXT("removed_from_proposed_state"):
                    !S.Unsupported.IsEmpty()?TEXT("unsupported"):!S.bVisible?TEXT("not_editor_visible"):
                    !S.bGeometry?TEXT("no_geometry"):R.bPending?TEXT("pending"):
                    R.bCancelled?TEXT("cancelled"):!R.Failure.IsEmpty()?TEXT("incomplete"):
                    !S.Geometry?TEXT("geometry_unavailable"):TEXT("source_geometry_analyzed"));
                Row->SetStringField(TEXT("reason"),S.Unsupported);Row->SetStringField(TEXT("representation"),S.Representation);
                Coverage.Add(MakeShared<FJsonValueObject>(Row));
            }
            TArray<FValue> CoveragePage;for(int32 I=Offset;I<FMath::Min(Offset+Limit,Coverage.Num());++I)CoveragePage.Add(Coverage[I]);
            O->SetNumberField(TEXT("coverage_total"),Coverage.Num());O->SetArrayField(TEXT("coverage"),CoveragePage);
            O->SetBoolField(TEXT("coverage_has_more"),Offset+CoveragePage.Num()<Coverage.Num());
        }
        int32 Unsupported=0; for(const auto& S:R.Shapes) if(!S.Unsupported.IsEmpty() && !S.bDeleted) ++Unsupported;
        O->SetNumberField(TEXT("unsupported_objects"),Unsupported);
        O->SetBoolField(TEXT("geometry_coverage_complete"),Geometry && R.bComplete && !R.bPending && !R.bCancelled && R.Failure.IsEmpty() && Unsupported==0 && R.Limitations.IsEmpty());
        return O;
    }
}

bool IsAnalysisCommand(const FString& Command) {return Command==TEXT("world.scene_context") || Command==TEXT("world.geometry_overlap");}

void Shutdown()
{
    if(!GCache)return;
    auto& C=*GCache;C.bShuttingDown.Store(true);
    FCoreUObjectDelegates::OnObjectModified.Remove(C.ModifiedHandle);
    FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(C.PropertyHandle);
    FCoreUObjectDelegates::OnObjectTransacted.Remove(C.TransactionHandle);
    if(C.WorkerFuture.IsValid())C.WorkerFuture.Wait();
    delete GCache;GCache=nullptr;
}

bool Query(UWorld& World,const FString& Command,const FString& RequestId,const FJson& Input,FJson& Output,Contract::FDiagnostic& Error)
{
    check(IsInGameThread()); const bool Geometry=Command.EndsWith(TEXT("geometry_overlap")); auto& C=Cache();
    FString Ref=String(Input,TEXT("analysis_ref")); if(Ref.IsEmpty()) Ref=C.Requests.FindRef(RequestId);
    if(!String(Input,TEXT("analysis_ref")).IsEmpty())
        for(const TCHAR* Field:{TEXT("workspace_root"),TEXT("targets"),TEXT("regions"),TEXT("margin_cm"),TEXT("near_distance_cm")})
            if(Input->HasField(Field))return Fail(Error,TEXT("scene_analysis_ref_scope_conflict"),TEXT("An analysis_ref fixes its source, scope and tolerance; only paging and object_ref may change."));
    TSharedPtr<FRecord,ESPMode::ThreadSafe> R;
    { FScopeLock Lock(&C.Mutex); R=C.Records.FindRef(Ref); }
    if(!Ref.IsEmpty() && !R) return Fail(Error,TEXT("scene_analysis_expired"),TEXT("Analysis reference expired; prepare from the current source again."));
    if(!R)
    {
        if(!Prepare(World,RequestId,Input,Geometry,R,Error)) return false;
        FScopeLock Lock(&C.Mutex);
        if(!R->WorkspaceRoot.IsEmpty())for(const auto& Entry:C.Records)
        {
            const auto& Previous=Entry.Value;
            if(Previous->World==R->World && Previous->WorkspaceRoot==R->WorkspaceRoot &&
                (Previous->WorkspaceFingerprint!=R->WorkspaceFingerprint || Previous->PreparedBundleFingerprint!=R->PreparedBundleFingerprint))
                Previous->bSuperseded.Store(true);
        }
        while(C.Order.Num()>=MaxRecords)
        {
            const FString Old=C.Order[0]; const auto Existing=C.Records.FindRef(Old);
            if(Existing && Existing->bPending) return Fail(Error,TEXT("scene_analysis_busy"),TEXT("Analysis snapshot capacity is occupied; finish/cancel outstanding operations."));
            C.Records.Remove(Old); C.Order.RemoveAt(0);
            for(auto It=C.Requests.CreateIterator();It;++It)if(It.Value()==Old)It.RemoveCurrent();
        }
        C.Records.Add(R->Ref,R); C.Order.Add(R->Ref);
    }
    else
    {
        FScopeLock RecordLock(&R->Mutex);
        if(R->bSuperseded.Load())return Fail(Error,TEXT("scene_analysis_superseded"),TEXT("A newer edit replaced this analysis; retrieve the newer operation."));
        if(!R->AppliedSummary && !CheckCurrent(*R,World,Error)) return false;
        if(Geometry && !R->bGeometryRequested)
        {
            if(R->AppliedSummary)return Fail(Error,TEXT("scene_analysis_already_applied"),TEXT("Analyze the current scene for geometry; this reference records an applied context-only snapshot."));
            int64 Vertices=0; for(auto& S:R->Shapes) if(!S.bDeleted) S.Geometry=CaptureMesh(S,Vertices);
            R->bGeometryRequested=true; R->bPending=true; R->bComplete=false;
        }
    }
    C.Requests.Add(RequestId,R->Ref); FScopeLock Lock(&R->Mutex); Output=Project(*R,Input,Geometry); return true;
}

bool HasPending(const FString& RequestId)
{
    auto& C=Cache(); TSharedPtr<FRecord,ESPMode::ThreadSafe> R;
    {FScopeLock Lock(&C.Mutex);R=C.Records.FindRef(C.Requests.FindRef(RequestId));}
    if(!R)return false;FScopeLock Lock(&R->Mutex);return R->bPending;
}

void DeferPending(const FString& RequestId,TFunction<bool()> IsCancelled,TFunction<void()> Complete)
{
    auto& C=Cache();
    {
        FScopeLock Lock(&C.Mutex);C.Jobs.Add({RequestId,MoveTemp(IsCancelled),MoveTemp(Complete)});
        if(C.bWorkerActive)return;C.bWorkerActive=true;
    }
    C.WorkerFuture=Async(EAsyncExecution::ThreadPool,[]()
    {
        auto& C=Cache();
        for(;;)
        {
            FCache::FJob Job;
            {FScopeLock Lock(&C.Mutex);if(C.Jobs.IsEmpty()){C.bWorkerActive=false;return;}Job=MoveTemp(C.Jobs[0]);C.Jobs.RemoveAt(0);}
            RunPending(Job.RequestId,Job.IsCancelled);
            if(C.bShuttingDown.Load())return;
            AsyncTask(ENamedThreads::GameThread,[Complete=MoveTemp(Job.Complete)]() mutable {Complete();});
        }
    });
}

void RunPending(const FString& RequestId,const TFunction<bool()>& IsCancelled)
{
    auto& C=Cache(); TSharedPtr<FRecord,ESPMode::ThreadSafe> R;
    { FScopeLock Lock(&C.Mutex); R=C.Records.FindRef(C.Requests.FindRef(RequestId)); }
    if(!R) return;
    // One mathematics worker per Editor. Cancellation is checked before and inside expensive loops.
    FScopeLock Worker(&C.Worker);
    const auto Destination=R;
    const auto Cancelled=[&](){return C.bShuttingDown.Load() || Destination->bSuperseded.Load() || (IsCancelled && IsCancelled());};
    {
        FScopeLock Lock(&Destination->Mutex);if(!Destination->bPending)return;
        R=MakeShared<FRecord,ESPMode::ThreadSafe>();R->Shapes=Destination->Shapes;R->Near=Destination->Near;R->Limitations=Destination->Limitations;
    }
    const double Started=FPlatformTime::Seconds();
    struct FAggregate {FString Object;int32 Surface=INDEX_NONE;double Area=0;FSurfaceFrame Frame;TArray<UE::Geometry::FGeneralPolygon2d> Regions;};
    TMap<FString,FAggregate> Aggregates;
    auto Aggregate=[&](const FString& Id,int32 Surface,double Area,const FGeometryFact& F)
    {
        auto& Total=Aggregates.FindOrAdd(Id+TEXT("|")+FString::FromInt(Surface));
        if(Total.Object.IsEmpty()){Total.Object=Id;Total.Surface=Surface;Total.Area=Area;Total.Frame=F.Frame;}
        auto Convert=[&](const UE::Geometry::FPolygon2d& P)
        {TArray<FVector2d> Vertices;for(const auto& V:P.GetVertices())Vertices.Add(Total.Frame.Project(F.Frame.Lift(V)));return UE::Geometry::FPolygon2d(Vertices);};
        for(const auto& Region:F.Regions)
        {
            auto Outer=Convert(Region.GetOuter());if(Outer.IsClockwise())Outer.Reverse();
            UE::Geometry::FGeneralPolygon2d Out(Outer);
            for(const auto& Hole:Region.GetHoles()){auto H=Convert(Hole);if(!H.IsClockwise())H.Reverse();Out.AddHole(H,false,false);}
            Total.Regions.Add(MoveTemp(Out));
        }
    };
    for(auto& S:R->Shapes)
    {
        if(Cancelled()) {R->bCancelled=true;break;}
        if(S.Geometry && !S.Geometry->bPrepared)
        {
            FString Error; if(!BuildGeometry(*S.Geometry,Cancelled,Error)) {S.Unsupported=Error;S.Geometry.Reset();}
        }
        if(S.Geometry) for(const auto& L:S.Geometry->Limitations) R->Limitations.AddUnique(L);
        if(S.Geometry && (S.Geometry->InternalDuplicateTriangles>0 || S.Geometry->InternalIntersectionSegments>0 || S.Geometry->InternalCoplanarRegions>0))
        {
            auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("a"),S.Id);O->SetStringField(TEXT("b"),S.Id);
            O->SetStringField(TEXT("kind"),TEXT("asset_internal_geometry"));O->SetStringField(TEXT("mesh"),S.MeshPath);
            O->SetNumberField(TEXT("duplicate_triangles"),S.Geometry->InternalDuplicateTriangles);
            O->SetNumberField(TEXT("intersection_segments"),S.Geometry->InternalIntersectionSegments);
            O->SetNumberField(TEXT("coplanar_regions"),S.Geometry->InternalCoplanarRegions);
            TArray<FValue> Points;for(const auto& P:S.Geometry->InternalWitnesses)Points.Add(Vec(FVector(S.Matrix.TransformPosition(P))));
            O->SetArrayField(TEXT("witnesses_cm"),Points);R->Facts.Add(MakeShared<FJsonValueObject>(O));
        }
    }
    // Sweep by minimum X; component/instance boxes narrow pairs without collision filtering.
    TArray<int32> Ordered; for(int32 I=0;I<R->Shapes.Num();++I) if(!R->Shapes[I].bDeleted && R->Shapes[I].bVisible && R->Shapes[I].bGeometry) Ordered.Add(I);
    Ordered.Sort([&](int32 A,int32 B){return R->Shapes[A].Bounds.Min.X<R->Shapes[B].Bounds.Min.X;});
    for(int32 I=0;I<Ordered.Num() && !R->bCancelled && R->Failure.IsEmpty();++I)
    {
        const auto& A=R->Shapes[Ordered[I]];
        for(int32 J=I+1;J<Ordered.Num();++J)
        {
            if(Cancelled()){R->bCancelled=true;break;}
            const auto& B=R->Shapes[Ordered[J]];
            if(B.Bounds.Min.X>A.Bounds.Max.X+R->Near) break;
            if((!A.bChanged&&!B.bChanged) || !A.Bounds.ExpandBy(R->Near).Intersect(B.Bounds)) continue;
            ++R->CandidatePairs;
            if(!A.Geometry || !B.Geometry) continue;
            const FString PairKey=ShapeSignature(A)+TEXT("|")+ShapeSignature(B)+FString::Printf(TEXT("|%.17g|%llu|%llu"),R->Near,A.Geometry->Mesh.Revision,B.Geometry->Mesh.Revision);
            FGeometryPairResult Pair;
            if(const auto* Cached=C.PairResults.Find(PairKey))Pair=*Cached;
            else
            {
                Pair=CompareGeometry(*A.Geometry,A.Matrix,*B.Geometry,B.Matrix,R->Near,Cancelled);
                if(Pair.bComplete && A.Representation!=TEXT("dynamic_mesh_snapshot") && B.Representation!=TEXT("dynamic_mesh_snapshot"))
                {
                    int64 Bytes=Pair.Facts.Num()*512;
                    for(const auto& F:Pair.Facts){Bytes+=F.Witnesses.Num()*24;for(const auto& P:F.Regions)Bytes+=P.GetOuter().GetVertices().Num()*32;}
                    if(Bytes<1024*1024)
                    {
                        if(C.PairResults.Num()>=256 || C.PairBytes+Bytes>16*1024*1024){C.PairResults.Empty();C.PairBytes=0;}
                        C.PairResults.Add(PairKey,Pair);C.PairBytes+=Bytes;
                    }
                }
            }
            if(Pair.bCancelled){R->bCancelled=true;break;}
            if(Pair.bComplete) ++R->AnalyzedPairs;
            for(const FString& L:Pair.Limitations) R->Limitations.AddUnique(L);
            for(const auto& F:Pair.Facts)
            {
                auto O=MakeShared<FJsonObject>(); O->SetStringField(TEXT("a"),A.Id);O->SetStringField(TEXT("b"),B.Id);O->SetStringField(TEXT("kind"),F.Kind);
                O->SetNumberField(TEXT("surface_a"),F.SurfaceA);O->SetNumberField(TEXT("surface_b"),F.SurfaceB);O->SetStringField(TEXT("certainty"),F.Certainty);
                O->SetBoolField(TEXT("same_facing"),F.bSameFacing);
                if(F.AreaCm2.IsSet())
                {
                    O->SetNumberField(TEXT("area_cm2"),F.AreaCm2.GetValue());
                    if(F.SurfaceAreaACm2.IsSet()) {O->SetNumberField(TEXT("surface_a_cm2"),F.SurfaceAreaACm2.GetValue());O->SetNumberField(TEXT("fraction_a"),F.AreaCm2.GetValue()/F.SurfaceAreaACm2.GetValue());}
                    if(F.SurfaceAreaBCm2.IsSet()) {O->SetNumberField(TEXT("surface_b_cm2"),F.SurfaceAreaBCm2.GetValue());O->SetNumberField(TEXT("fraction_b"),F.AreaCm2.GetValue()/F.SurfaceAreaBCm2.GetValue());}
                    if(F.Kind==TEXT("coplanar_overlap") || F.Kind==TEXT("duplicate_surface"))
                    {
                        Aggregate(A.Id,F.SurfaceA,F.SurfaceAreaACm2.GetValue(),F);
                        Aggregate(B.Id,F.SurfaceB,F.SurfaceAreaBCm2.GetValue(),F);
                    }
                }
                O->SetNumberField(TEXT("min_distance_cm"),F.MinDistanceCm);O->SetNumberField(TEXT("max_distance_cm"),F.MaxDistanceCm);
                TArray<FValue> Witnesses;for(const auto& P:F.Witnesses)Witnesses.Add(Vec(P));O->SetArrayField(TEXT("witnesses_cm"),Witnesses);
                if(F.TriangleHitCount>0)O->SetNumberField(TEXT("triangle_hit_count"),F.TriangleHitCount);
                if(F.bWitnessesTruncated)O->SetBoolField(TEXT("witnesses_truncated"),true);
                R->Facts.Add(MakeShared<FJsonValueObject>(O));
                if(R->Facts.Num()>=MaxFacts){R->Failure=TEXT("analysis_result_memory_budget_exceeded");break;}
            }
            if(!R->Failure.IsEmpty())break;
        }
    }
    for(auto& Entry:Aggregates)
    {
        auto& T=Entry.Value;TArray<UE::Geometry::FGeneralPolygon2d> Union;double Area=0;
        if(!UnionRegions(T.Regions,Union,Area)){R->Limitations.AddUnique(TEXT("multi_object_surface_union_failed"));continue;}
        auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("object_ref"),T.Object);O->SetNumberField(TEXT("surface"),T.Surface);
        O->SetNumberField(TEXT("union_area_cm2"),Area);O->SetNumberField(TEXT("surface_area_cm2"),T.Area);O->SetNumberField(TEXT("fraction"),Area/T.Area);
        R->SurfaceTotals.Add(MakeShared<FJsonValueObject>(O));
    }
    R->ComputeMs=(FPlatformTime::Seconds()-Started)*1000;R->bPending=false;R->bComplete=!R->bCancelled&&R->Failure.IsEmpty();
    {
        FScopeLock Lock(&Destination->Mutex);
        Destination->Shapes=MoveTemp(R->Shapes);Destination->Facts=MoveTemp(R->Facts);
        Destination->SurfaceTotals=MoveTemp(R->SurfaceTotals);Destination->Limitations=MoveTemp(R->Limitations);
        Destination->CandidatePairs=R->CandidatePairs;Destination->AnalyzedPairs=R->AnalyzedPairs;
        Destination->ComputeMs=R->ComputeMs;Destination->bCancelled=R->bCancelled;
        Destination->bComplete=R->bComplete;Destination->bPending=false;Destination->Failure=MoveTemp(R->Failure);
    }
}

bool ValidateForApply(UWorld& World,const FString& WorkspaceRoot,const FString& AnalysisRef,
    const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,const TSharedRef<FJsonObject>& PreparedBundle,Contract::FDiagnostic& Error)
{
    check(IsInGameThread());auto& C=Cache();TSharedPtr<FRecord,ESPMode::ThreadSafe> R;
    {FScopeLock Lock(&C.Mutex);R=C.Records.FindRef(AnalysisRef);}
    if(!R)return Fail(Error,TEXT("scene_analysis_binding_invalid"),TEXT("Analysis reference is unavailable or expired."));
    if(R->WorldPath!=World.GetPathName())return Fail(Error,TEXT("scene_analysis_binding_invalid"),TEXT("The analyzed World identity differs from the World being applied."));
    if(!FPaths::IsSamePath(R->WorkspaceRoot,WorkspaceRoot))
        return Fail(Error,TEXT("scene_analysis_binding_invalid"),TEXT("The analyzed workspace directory differs from the directory being applied."));
    FScopeLock RecordLock(&R->Mutex);
    if(R->AppliedSummary)return Fail(Error,TEXT("scene_analysis_already_applied"),TEXT("Recover the original import operation, or analyze a new edit before applying again."));
    if(R->bSuperseded.Load())return Fail(Error,TEXT("scene_analysis_superseded"),TEXT("The analyzed edit has been superseded."));
    if(R->bPending || R->bCancelled || !R->Failure.IsEmpty()) return Fail(Error,TEXT("scene_analysis_not_ready"),TEXT("Selected analysis is pending, cancelled or failed."));
    if(!CheckCurrent(*R,World,Error))return false;
    if(Prepared.GetWorkspace()->GetWorkspaceFingerprint()!=R->WorkspaceFingerprint) return Fail(Error,TEXT("scene_analysis_stale"),TEXT("Edited workspace changed after analysis."));
    FString CanonicalBundle;
    if(!Contract::Json::TryCanonicalize(PreparedBundle,CanonicalBundle,Error))return false;
    if(Hash(CanonicalBundle)!=R->PreparedBundleFingerprint)
        return Fail(Error,TEXT("scene_analysis_stale"),TEXT("Prepared native changes, including structural edits, differ from the analyzed operation."));
    return true;
}

TSharedPtr<FJsonObject> ReadApplied(UWorld& World,const FString& AnalysisRef)
{
    check(IsInGameThread());auto& C=Cache();TSharedPtr<FRecord,ESPMode::ThreadSafe> R;
    {FScopeLock Lock(&C.Mutex);R=C.Records.FindRef(AnalysisRef);}
    auto Summary=MakeShared<FJsonObject>();Summary->SetStringField(TEXT("analysis_ref"),AnalysisRef);
    if(!R){Summary->SetStringField(TEXT("status"),TEXT("analysis_expired"));return Summary;}
    FScopeLock Lock(&R->Mutex);
    TArray<FShape> Actual;
    if(!CaptureShapes(World,Actual))
    {
        Summary->SetStringField(TEXT("status"),TEXT("verification_inventory_budget_exceeded"));
        Summary->SetBoolField(TEXT("geometry_recheck_required"),true);R->AppliedSummary=Summary;return Summary;
    }
    TSet<FString> Matched;
    int32 Verified=0;R->AppliedRecheck.Reset();
    auto Recheck=[&](const FString& Id,const FString& Reason)
    {auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("object_ref"),Id);O->SetStringField(TEXT("reason"),Reason);R->AppliedRecheck.Add(MakeShared<FJsonValueObject>(O));};
    if(R->bScopeUnknown)Recheck(World.GetPathName(),TEXT("world_or_construction_effect_requires_actual_scope_review"));
    for(const FShape& Expected:R->Shapes)
    {
        const FShape* Live=Actual.FindByPredicate([&](const FShape& S)
        {
            const bool Identity=(!Expected.ManagedId.IsEmpty() && S.ManagedId==Expected.ManagedId)
                || (!Expected.ActorGuid.IsEmpty() && S.ActorGuid==Expected.ActorGuid) || S.ActorId==Expected.ActorId;
            return Identity && S.ComponentName==Expected.ComponentName && S.Instance==Expected.Instance;
        });
        if(Live)Matched.Add(Live->Id);
        if(Expected.bDeleted){if(Live)Recheck(Live->Id,TEXT("expected_removal_still_present"));else ++Verified;continue;}
        if(!Live){Recheck(Expected.Id,TEXT("expected_component_missing_or_reconstructed"));continue;}
        if(!Expected.Unsupported.IsEmpty()){Recheck(Live->Id,Expected.Unsupported);continue;}
        if(Expected.MeshPath!=Live->MeshPath || Expected.Class!=Live->Class || Expected.bVisible!=Live->bVisible
            || !Expected.Matrix.Equals(Live->Matrix,1e-6) || Expected.MeshVersion!=Live->MeshVersion)
        {Recheck(Live->Id,TEXT("applied_geometry_or_transform_differs_from_prediction"));continue;}
        FShape Mutable=*Live;
        if(!CaptureMutableFingerprint(Mutable) || Mutable.ContentFingerprint!=Expected.ContentFingerprint)
        {Recheck(Live->Id,TEXT("mutable_geometry_changed_or_unverifiable"));continue;}
        ++Verified;
    }
    for(const auto& Live:Actual)if(!Matched.Contains(Live.Id) && InRegions(Live,R->Regions,R->Margin+R->Near))
        Recheck(Live.Id,TEXT("native_created_or_changed_neighborhood_member"));
    Summary->SetStringField(TEXT("status"),R->AppliedRecheck.IsEmpty()?TEXT("predicted_source_state_matches"):TEXT("geometry_recheck_required"));
    Summary->SetBoolField(TEXT("geometry_was_analyzed"),R->bGeometryRequested && R->bComplete);
    Summary->SetNumberField(TEXT("verified_objects"),Verified);Summary->SetNumberField(TEXT("recheck_count"),R->AppliedRecheck.Num());
    Summary->SetStringField(TEXT("detail"),TEXT("Reuse scene_context with this analysis_ref to page the applied verification; it is a historical snapshot, not a new current-scene analysis."));
    R->AppliedSummary=Summary;return Summary;
}
}
