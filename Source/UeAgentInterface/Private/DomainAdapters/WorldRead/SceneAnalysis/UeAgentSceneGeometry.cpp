#include "DomainAdapters/WorldRead/SceneAnalysis/UeAgentSceneGeometry.h"

#include "Curve/PolygonIntersectionUtils.h"
#include "Intersection/IntrTriangle3Triangle3.h"

namespace UE::UAI::SceneAnalysis
{
    using namespace UE::Geometry;

    namespace
    {
        FVector3d Cross(const FVector3d& A, const FVector3d& B) { return FVector3d::CrossProduct(A, B); }
        double Dot(const FVector3d& A, const FVector3d& B) { return FVector3d::DotProduct(A, B); }
        FVector3d Point(const FMatrix44d& T, const FVector3d& P) { return FVector3d(T.TransformPosition(P)); }
        bool Cancelled(const TFunction<bool()>& Fn) { return Fn && Fn(); }

        FSurfaceFrame Frame(const FVector3d& A, const FVector3d& B, const FVector3d& C)
        {
            FSurfaceFrame F;
            F.Origin = A;
            F.U = (B - A).GetSafeNormal();
            F.Normal = Cross(B - A, C - A).GetSafeNormal();
            F.V = Cross(F.Normal, F.U).GetSafeNormal();
            return F;
        }

        uint64 EdgeKey(int32 A, int32 B)
        {
            return (uint64(uint32(FMath::Min(A, B))) << 32) | uint32(FMath::Max(A, B));
        }

        FGeneralPolygon2d TriangleRegion(const FTriangleMesh& Mesh, int32 Id, const FSurfaceFrame& F)
        {
            FVector3d A, B, C; Mesh.GetTriVertices(Id, A, B, C);
            FPolygon2d Polygon(TArray<FVector2d>{F.Project(A), F.Project(B), F.Project(C)});
            if (Polygon.IsClockwise()) Polygon.Reverse();
            return FGeneralPolygon2d(Polygon);
        }

        FBox TransformBounds(const FBox& Box, const FMatrix44d& Matrix)
        {
            return Box.TransformBy(Matrix);
        }

        FSurfaceFrame WorldFrame(const FSurfaceFrame& Local, const FMatrix44d& Matrix)
        {
            const FVector3d O = Point(Matrix, Local.Origin);
            FSurfaceFrame F = Frame(O, Point(Matrix, Local.Origin + Local.U), Point(Matrix, Local.Origin + Local.V));
            // Mirroring reverses vertex winding; physical outward/front orientation also uses reverse culling.
            if (Matrix.Determinant() < 0) { F.Normal *= -1; F.V *= -1; }
            return F;
        }

        TArray<FGeneralPolygon2d> ProjectRegions(const FPlanarSurface& Surface,
            const FMatrix44d& Matrix, const FSurfaceFrame& Destination)
        {
            TArray<FGeneralPolygon2d> Result;
            auto Convert = [&](const FPolygon2d& Source)
            {
                TArray<FVector2d> Vertices;
                for (const FVector2d& P : Source.GetVertices())
                    Vertices.Add(Destination.Project(Point(Matrix, Surface.Frame.Lift(P))));
                return FPolygon2d(Vertices);
            };
            for (const FGeneralPolygon2d& Region : Surface.Regions)
            {
                FPolygon2d Outer = Convert(Region.GetOuter());
                if (Outer.IsClockwise()) Outer.Reverse();
                FGeneralPolygon2d Converted(Outer);
                for (const FPolygon2d& Hole : Region.GetHoles())
                {
                    FPolygon2d H = Convert(Hole);
                    if (!H.IsClockwise()) H.Reverse();
                    Converted.AddHole(H, false, false);
                }
                Result.Add(MoveTemp(Converted));
            }
            return Result;
        }

        double Area(const TArray<FGeneralPolygon2d>& Regions)
        {
            double Value = 0;
            for (const auto& R : Regions) Value += FMath::Abs(R.SignedArea());
            return Value;
        }

        bool InteriorOnBothSides(const FPlanarSurface& Surface, const FMatrix44d& Matrix,
            const FVector3d& Midpoint, const FVector3d& Direction, double Epsilon)
        {
            const FSurfaceFrame W = WorldFrame(Surface.Frame, Matrix);
            const auto Regions = ProjectRegions(Surface, Matrix, W);
            const FVector2d P = W.Project(Midpoint);
            const FVector3d Side = Cross(W.Normal, Direction).GetSafeNormal() * Epsilon * 8;
            const FVector2d Delta(Dot(Side,W.U),Dot(Side,W.V));
            auto Inside = [&](const FVector2d& Test) { for(const auto& R:Regions) if(R.Contains(Test)) return true; return false; };
            return Inside(P+Delta) && Inside(P-Delta);
        }

        bool InsideSolid(const FGeometry& Solid, const FVector3d& P)
        {
            if(Solid.Winding)
            {
                const double W=FMath::Abs(Solid.Winding->FastWindingNumber(P));
                if(W<.1)return false;if(W>.9)return true;
            }
            double Sum = 0;
            for(int32 Id:Solid.Mesh.TriangleIds)
            {
                FVector3d A,B,C;Solid.Mesh.GetTriVertices(Id,A,B,C); A-=P; B-=P; C-=P;
                const double LA=A.Size(),LB=B.Size(),LC=C.Size();
                const double Numerator=Dot(A,Cross(B,C));
                const double Denominator=LA*LB*LC+Dot(A,B)*LC+Dot(B,C)*LA+Dot(C,A)*LB;
                Sum+=2*FMath::Atan2(Numerator,Denominator);
            }
            return FMath::Abs(Sum)>2*UE_DOUBLE_PI;
        }

        MeshIntersection::FIntersectionsQueryResult Intersections(
            const FGeometry& A,const FMatrix44d& TA,const FGeometry& B,const FMatrix44d& TB,
            bool Self,double Epsilon,const TFunction<bool()>& IsCancelled,bool& Limited)
        {
            MeshIntersection::FIntersectionsQueryResult Result;
            const FMatrix44d InverseA=TA.Inverse(),InverseB=TB.Inverse();int32 Hits=0;
            for(int32 IA:A.Mesh.TriangleIds)
            {
                if(Limited || Cancelled(IsCancelled))break;
                FVector3d A0,A1,A2;A.Mesh.GetTriVertices(IA,A0,A1,A2);
                A0=Point(TA,A0);A1=Point(TA,A1);A2=Point(TA,A2);
                FBox Search(ForceInit);Search+=A0;Search+=A1;Search+=A2;
                Search=Search.ExpandBy(Epsilon).TransformBy(InverseB);
                TMeshAABBTree3<FTriangleMesh>::FTreeTraversal Traversal;
                Traversal.NextBoxF=[&](const FAxisAlignedBox3d& Box,int)
                {return !Limited&&!Cancelled(IsCancelled)&&Search.Intersect(FBox(Box.Min,Box.Max));};
                Traversal.NextTriangleF=[&](int32 IB)
                {
                    if(Limited||Cancelled(IsCancelled)||(Self&&IB<=IA))return;
                    FVector3d B0,B1,B2;B.Mesh.GetTriVertices(IB,B0,B1,B2);
                    B0=Point(TB,B0);B1=Point(TB,B1);B2=Point(TB,B2);
                    if(Self)
                    {
                        const FSurfaceFrame AF=Frame(A0,A1,A2),BF=Frame(B0,B1,B2);
                        const bool Coplanar=FMath::Abs(Dot(AF.Normal,BF.Normal))>1.0-1e-10
                            && FMath::Abs(Dot(B0-A0,AF.Normal))<=Epsilon
                            && FMath::Abs(Dot(B1-A0,AF.Normal))<=Epsilon
                            && FMath::Abs(Dot(B2-A0,AF.Normal))<=Epsilon;
                        if(Coplanar)
                        {
                            // A triangle intersector may return a degenerate polygon for a
                            // shared diagonal. Only positive planar intersection area is a
                            // duplicated surface; ordinary triangulation is not self-overlap.
                            auto MakeRegion=[&](FVector3d P0,FVector3d P1,FVector3d P2)
                            {FPolygon2d P(TArray<FVector2d>{AF.Project(P0),AF.Project(P1),AF.Project(P2)});if(P.IsClockwise())P.Reverse();return FGeneralPolygon2d(P);};
                            const TArray<FGeneralPolygon2d> AR{MakeRegion(A0,A1,A2)},BR{MakeRegion(B0,B1,B2)};
                            TArray<FGeneralPolygon2d> Overlap;
                            if(!PolygonsIntersection(MakeArrayView(AR),MakeArrayView(BR),Overlap)){Limited=true;return;}
                            const double EdgeScale=FMath::Max3((A1-A0).Size(),(A2-A0).Size(),(B2-B0).Size());
                            if(Area(Overlap)<=Epsilon*FMath::Max(Epsilon,EdgeScale))return;
                            for(const auto& Region:Overlap)
                            {
                                MeshIntersection::FPolygonIntersection P;P.TriangleID[0]=IA;P.TriangleID[1]=IB;
                                P.Quantity=FMath::Min(6,Region.GetOuter().GetVertices().Num());
                                for(int K=0;K<P.Quantity;++K)P.Point[K]=Point(InverseA,AF.Lift(Region.GetOuter().GetVertices()[K]));
                                Result.Polygons.Add(P);++Hits;
                            }
                            Limited=Hits>=20000;return;
                        }
                    }
                    FIntrTriangle3Triangle3d I(FTriangle3d(A0,A1,A2),FTriangle3d(B0,B1,B2));
                    I.SetReportCoplanarIntersection(true);I.SetTolerance(Epsilon);
                    if(!I.Find()||I.Quantity==0)return;
                    if(I.Quantity>2)
                    {
                        FVector3d TwiceArea=FVector3d::ZeroVector;
                        for(int32 K=1;K+1<I.Quantity;++K)TwiceArea+=Cross(I.Points[K]-I.Points[0],I.Points[K+1]-I.Points[0]);
                        if(TwiceArea.Size()>Epsilon*Epsilon)
                        {
                            if(Self)
                            {MeshIntersection::FPolygonIntersection P;P.TriangleID[0]=IA;P.TriangleID[1]=IB;P.Quantity=I.Quantity;for(int K=0;K<I.Quantity;++K)P.Point[K]=Point(InverseA,I.Points[K]);Result.Polygons.Add(P);++Hits;}
                            Limited=Hits>=20000;return; // cross-object planar area is handled by complete surfaces
                        }
                        I.SetResult(I.Points[0],I.Points[I.Quantity-1]);
                    }
                    if(I.Quantity==2 && (I.Points[1]-I.Points[0]).Size()>Epsilon)
                    {
                        if(Self)
                        {
                            const FVector3d AV[3]={A0,A1,A2},BV[3]={B0,B1,B2};int32 Common=0;
                            for(int X=0;X<3;++X)for(int Y=0;Y<3;++Y)if((AV[X]-BV[Y]).Size()<=Epsilon)++Common;
                            if(Common>=2)return;
                        }
                        MeshIntersection::FSegmentIntersection S;S.TriangleID[0]=IA;S.TriangleID[1]=IB;S.Point[0]=Point(InverseA,I.Points[0]);S.Point[1]=Point(InverseA,I.Points[1]);Result.Segments.Add(S);++Hits;
                    }
                    else if(!Self)
                    {MeshIntersection::FPointIntersection P;P.TriangleID[0]=IA;P.TriangleID[1]=IB;P.Point=Point(InverseA,I.Points[0]);Result.Points.Add(P);++Hits;}
                    Limited=Hits>=20000;
                };
                B.Tree->DoTraversal(Traversal);
            }
            return Result;
        }
    }

    FVector2d FSurfaceFrame::Project(const FVector3d& P) const
    {
        return FVector2d(Dot(P - Origin, U), Dot(P - Origin, V));
    }

    FVector3d FSurfaceFrame::Lift(const FVector2d& P) const { return Origin + U * P.X + V * P.Y; }

    bool UnionRegions(const TArray<FGeneralPolygon2d>& Input, TArray<FGeneralPolygon2d>& Output, double& OutAreaCm2)
    {
        Output.Reset(); OutAreaCm2 = 0;
        if (Input.IsEmpty()) return true;
        if (!PolygonsUnion(MakeArrayView(Input), Output, false)) return false;
        OutAreaCm2 = Area(Output);
        return true;
    }

    bool BuildGeometry(FGeometry& G, const TFunction<bool()>& IsCancelled, FString& OutError)
    {
        G.bPrepared = false;
        G.Winding.Reset();G.Tree.Reset(); G.Surfaces.Reset(); G.Mesh.TriangleIds.Reset();
        G.IslandSamples.Reset(); G.InternalDuplicateTriangles=0; G.InternalIntersectionSegments=0;G.InternalCoplanarRegions=0;
        G.InternalWitnesses.Reset();
        // Source or render seams may have distinct indices at exactly the same position.
        // Canonicalize geometric vertices; never weld merely nearby surfaces.
        TMap<FVector3d,int32> PositionIds;
        TArray<FVector3d> Positions; TArray<int32> Remap;
        for(const auto& P:G.Mesh.Vertices)
        {
            if(P.ContainsNaN()){OutError=TEXT("non_finite_vertex");return false;}
            const int32* Existing=PositionIds.Find(P);
            if(Existing) Remap.Add(*Existing);
            else {const int32 Id=Positions.Add(P);PositionIds.Add(P,Id);Remap.Add(Id);}
        }
        for(auto& T:G.Mesh.Triangles)
        {
            if(!Remap.IsValidIndex(T.A)||!Remap.IsValidIndex(T.B)||!Remap.IsValidIndex(T.C)){OutError=TEXT("invalid_triangle_index");return false;}
            T=FIndex3i(Remap[T.A],Remap[T.B],Remap[T.C]);
        }
        G.Mesh.Vertices=MoveTemp(Positions);
        G.Bounds = FBox(ForceInit); G.bClosedOriented = false;
        G.TriangleSurface.Init(INDEX_NONE, G.Mesh.Triangles.Num());
        G.TriangleIsland.Init(INDEX_NONE,G.Mesh.Triangles.Num());
        for (const FVector3d& V : G.Mesh.Vertices)
        {
            if (V.ContainsNaN()) { OutError = TEXT("non_finite_vertex"); return false; }
            G.Bounds += V;
        }
        if (G.Mesh.Triangles.IsEmpty()) { OutError = TEXT("no_triangles"); return false; }
        const double Epsilon = FMath::Max(1e-8, G.Bounds.GetSize().GetMax() * 1e-10);
        TMap<uint64, TArray<int32>> EdgeTriangles;
        TMap<uint64, int32> EdgeOrientation;
        TSet<FIntVector> TriangleKeys;
        TArray<FSurfaceFrame> Frames;
        Frames.SetNum(G.Mesh.Triangles.Num());
        for (int32 Id = 0; Id < G.Mesh.Triangles.Num(); ++Id)
        {
            if (Cancelled(IsCancelled)) { OutError = TEXT("cancelled"); return false; }
            const FIndex3i T = G.Mesh.Triangles[Id];
            if (!G.Mesh.Vertices.IsValidIndex(T.A) || !G.Mesh.Vertices.IsValidIndex(T.B) || !G.Mesh.Vertices.IsValidIndex(T.C))
            { OutError = TEXT("invalid_triangle_index"); return false; }
            const FVector3d A = G.Mesh.Vertices[T.A], B = G.Mesh.Vertices[T.B], C = G.Mesh.Vertices[T.C];
            if (Cross(B - A, C - A).Size() <= Epsilon * Epsilon)
            { G.Limitations.AddUnique(TEXT("degenerate_triangles")); continue; }
            G.Mesh.TriangleIds.Add(Id); Frames[Id] = Frame(A, B, C);
            int32 Sorted[3]={T.A,T.B,T.C}; if(Sorted[0]>Sorted[1])Swap(Sorted[0],Sorted[1]);if(Sorted[1]>Sorted[2])Swap(Sorted[1],Sorted[2]);if(Sorted[0]>Sorted[1])Swap(Sorted[0],Sorted[1]);
            const FIntVector Key(Sorted[0],Sorted[1],Sorted[2]);
            if(TriangleKeys.Contains(Key)){++G.InternalDuplicateTriangles;if(G.InternalWitnesses.Num()<8)G.InternalWitnesses.Add((A+B+C)/3.0);}else TriangleKeys.Add(Key);
            for (int32 E = 0; E < 3; ++E)
            {
                const int32 I = T[E], J = T[(E + 1) % 3];
                EdgeTriangles.FindOrAdd(EdgeKey(I, J)).Add(Id);
                EdgeOrientation.FindOrAdd(EdgeKey(I, J)) += I < J ? 1 : -1;
            }
        }
        if (G.Mesh.TriangleIds.IsEmpty()) { OutError = TEXT("no_valid_triangles"); return false; }
        bool bClosed = true;
        for (const auto& Edge : EdgeTriangles)
            if (Edge.Value.Num() != 2 || EdgeOrientation.FindChecked(Edge.Key) != 0) { bClosed = false; break; }
        // Closure alone does not establish absence of self-intersection or valid solid nesting.
        if (!bClosed) G.Limitations.AddUnique(TEXT("open_or_nonmanifold_or_inconsistent_winding"));
        G.Tree = MakeUnique<TMeshAABBTree3<FTriangleMesh>>(&G.Mesh, false);
        G.Tree->Build(G.Mesh.TriangleIds);
        bool SelfLimited=false;
        const auto Self=Intersections(G,FMatrix44d::Identity,G,FMatrix44d::Identity,true,Epsilon,IsCancelled,SelfLimited);
        if(SelfLimited)G.Limitations.AddUnique(TEXT("internal_intersection_incomplete"));
        G.InternalIntersectionSegments=Self.Segments.Num();
        G.InternalCoplanarRegions=Self.Polygons.Num();
        for(const auto& S:Self.Segments)if(G.InternalWitnesses.Num()<8)G.InternalWitnesses.Add((S.Point[0]+S.Point[1])*.5);
        for(const auto& P:Self.Polygons)if(G.InternalWitnesses.Num()<8 && P.Quantity>0)G.InternalWitnesses.Add(P.Point[0]);
        if(G.InternalIntersectionSegments>0)G.Limitations.AddUnique(TEXT("internal_surface_crossing"));
        if(G.InternalDuplicateTriangles>0)G.Limitations.AddUnique(TEXT("internal_duplicate_triangles"));
        if(G.InternalCoplanarRegions>0)G.Limitations.AddUnique(TEXT("internal_coplanar_overlap"));
        G.bClosedOriented=bClosed && !SelfLimited && G.InternalIntersectionSegments==0 && G.InternalDuplicateTriangles==0 && G.InternalCoplanarRegions==0 && G.Mesh.TriangleIds.Num()==G.Mesh.Triangles.Num();
        if(!G.bClosedOriented)G.Limitations.AddUnique(TEXT("solid_containment_unreliable"));
        else {G.Winding=MakeUnique<TFastWindingTree<FTriangleMesh>>(G.Tree.Get());G.Winding->Build();}
        TSet<int32> IslandVisited;
        for(int32 Seed:G.Mesh.TriangleIds)
        {
            if(IslandVisited.Contains(Seed))continue;
            const int32 Island=G.IslandSamples.Num();
            G.IslandSamples.Add(G.Mesh.Vertices[G.Mesh.Triangles[Seed].A]);
            TArray<int32> Pending{Seed};IslandVisited.Add(Seed);
            while(!Pending.IsEmpty())
            {
                const int32 Id=Pending.Pop(EAllowShrinking::No);G.TriangleIsland[Id]=Island;
                const auto T=G.Mesh.Triangles[Id];
                for(int32 E=0;E<3;++E)for(int32 N:EdgeTriangles.FindChecked(EdgeKey(T[E],T[(E+1)%3])))
                    if(!IslandVisited.Contains(N)){IslandVisited.Add(N);Pending.Add(N);}
            }
        }
        for (int32 Seed : G.Mesh.TriangleIds)
        {
            if (G.TriangleSurface[Seed] != INDEX_NONE) continue;
            if (Cancelled(IsCancelled)) { OutError = TEXT("cancelled"); return false; }
            const int32 SurfaceId = G.Surfaces.Num();
            FPlanarSurface Surface; Surface.Frame = Frames[Seed];
            TArray<int32> Pending{Seed}; G.TriangleSurface[Seed] = SurfaceId;
            while (!Pending.IsEmpty())
            {
                const int32 Id = Pending.Pop(EAllowShrinking::No);
                Surface.Triangles.Add(Id);
                const FIndex3i T = G.Mesh.Triangles[Id];
                for (int32 E = 0; E < 3; ++E)
                {
                    Surface.Bounds += G.Mesh.Vertices[T[E]];
                    for (int32 N : EdgeTriangles.FindChecked(EdgeKey(T[E], T[(E + 1) % 3])))
                    {
                        if (G.TriangleSurface[N] != INDEX_NONE || Dot(Frames[N].Normal, Surface.Frame.Normal) < 1.0 - 1e-10) continue;
                        const auto NT = G.Mesh.Triangles[N];
                        bool bPlanar = true;
                        for (int32 K = 0; K < 3; ++K)
                            bPlanar &= FMath::Abs(Dot(G.Mesh.Vertices[NT[K]] - Surface.Frame.Origin, Surface.Frame.Normal)) <= Epsilon;
                        if (bPlanar) { G.TriangleSurface[N] = SurfaceId; Pending.Add(N); }
                    }
                }
            }
            TArray<FGeneralPolygon2d> TriRegions;
            for (int32 Id : Surface.Triangles) TriRegions.Add(TriangleRegion(G.Mesh, Id, Surface.Frame));
            if (!UnionRegions(TriRegions, Surface.Regions, Surface.AreaCm2))
            { OutError = TEXT("surface_union_failed"); return false; }
            G.Surfaces.Add(MoveTemp(Surface));
        }
        G.bPrepared = true;
        return true;
    }

    FGeometryPairResult CompareGeometry(const FGeometry& A, const FMatrix44d& TA,
        const FGeometry& B, const FMatrix44d& TB, double NearDistanceCm, const TFunction<bool()>& IsCancelled)
    {
        FGeometryPairResult Result;
        Result.Limitations = A.Limitations;
        for (const FString& L : B.Limitations) Result.Limitations.AddUnique(L);
        if (!A.Tree || !B.Tree || FMath::Abs(TA.Determinant()) < UE_DOUBLE_SMALL_NUMBER || FMath::Abs(TB.Determinant()) < UE_DOUBLE_SMALL_NUMBER)
        { Result.Limitations.AddUnique(TEXT("unavailable_or_singular_geometry")); return Result; }
        const FBox BoundsA = TransformBounds(A.Bounds, TA), BoundsB = TransformBounds(B.Bounds, TB);
        const double Epsilon = FMath::Max(1e-7, FMath::Max(BoundsA.GetSize().GetMax(), BoundsB.GetSize().GetMax()) * 1e-10);
        const double Distance = FMath::Max(Epsilon, NearDistanceCm);
        if (!BoundsA.ExpandBy(Distance).Intersect(BoundsB)) { Result.bComplete = true; return Result; }
        const FMatrix44d InverseB = TB.Inverse();
        for (int32 SA = 0; SA < A.Surfaces.Num(); ++SA)
        {
            if (Cancelled(IsCancelled)) { Result.bCancelled = true; return Result; }
            const FPlanarSurface& FA = A.Surfaces[SA];
            const FSurfaceFrame WA = WorldFrame(FA.Frame, TA);
            const auto RegionsA = ProjectRegions(FA, TA, WA);
            const FBox Search = TransformBounds(TransformBounds(FA.Bounds, TA).ExpandBy(Distance), InverseB);
            TSet<int32> Candidates;
            TMeshAABBTree3<FTriangleMesh>::FTreeTraversal Traversal;
            Traversal.NextBoxF = [&](const FAxisAlignedBox3d& Box, int)
            { return !Cancelled(IsCancelled) && Search.Intersect(FBox(Box.Min, Box.Max)); };
            Traversal.NextTriangleF = [&](int32 T) { if (B.TriangleSurface[T] != INDEX_NONE) Candidates.Add(B.TriangleSurface[T]); };
            B.Tree->DoTraversal(Traversal);
            for (int32 SB : Candidates)
            {
                const FPlanarSurface& FB = B.Surfaces[SB];
                const FSurfaceFrame WB = WorldFrame(FB.Frame, TB);
                const double Alignment = Dot(WA.Normal, WB.Normal);
                if (FMath::Abs(Alignment) < 1.0 - 1e-8) continue;
                double MinDistance = TNumericLimits<double>::Max(), MaxDistance = -TNumericLimits<double>::Max();
                for (const auto& Region : FB.Regions)
                    for (const FVector2d& P : Region.GetOuter().GetVertices())
                    {
                        const double D = Dot(Point(TB, FB.Frame.Lift(P)) - WA.Origin, WA.Normal);
                        MinDistance = FMath::Min(MinDistance, D); MaxDistance = FMath::Max(MaxDistance, D);
                    }
                if (FMath::Max(FMath::Abs(MinDistance), FMath::Abs(MaxDistance)) > Distance) continue;
                const auto RegionsB = ProjectRegions(FB, TB, WA);
                TArray<FGeneralPolygon2d> Intersection;
                if (!PolygonsIntersection(MakeArrayView(RegionsA), MakeArrayView(RegionsB), Intersection))
                { Result.Limitations.AddUnique(TEXT("surface_intersection_failed")); continue; }
                const double OverlapArea = Area(Intersection);
                if (OverlapArea <= Epsilon * Epsilon) continue;
                FGeometryFact Fact;
                Fact.SurfaceA = SA; Fact.SurfaceB = SB; Fact.Frame = WA;
                Fact.Regions = MoveTemp(Intersection); Fact.AreaCm2 = OverlapArea;
                Fact.SurfaceAreaACm2 = Area(RegionsA);
                Fact.SurfaceAreaBCm2 = Area(ProjectRegions(FB, TB, WB));
                Fact.MinDistanceCm = MinDistance; Fact.MaxDistanceCm = MaxDistance;
                Fact.bSameFacing = Alignment > 0;
                const bool bCoplanar = FMath::Max(FMath::Abs(MinDistance), FMath::Abs(MaxDistance)) <= Epsilon;
                Fact.Kind = bCoplanar ? (Fact.bSameFacing ? TEXT("coplanar_overlap") : TEXT("touching")) : TEXT("near_coplanar_overlap");
                // Polygon clipping rounds coordinates. An area error scales with
                // edge length times the positional tolerance, not epsilon squared.
                const double AreaTolerance=Epsilon*FMath::Max(1.0,FMath::Max(BoundsA.GetSize().GetMax(),BoundsB.GetSize().GetMax()))*4.0;
                if (bCoplanar && Fact.bSameFacing && FMath::Abs(OverlapArea - Fact.SurfaceAreaACm2.GetValue()) <= AreaTolerance
                    && FMath::Abs(OverlapArea - Fact.SurfaceAreaBCm2.GetValue()) <= AreaTolerance) Fact.Kind = TEXT("duplicate_surface");
                if (!bCoplanar) Fact.Certainty = TEXT("projected_near_surface");
                for (const auto& P : Fact.Regions[0].GetOuter().GetVertices()) Fact.Witnesses.Add(WA.Lift(P));
                Result.Facts.Add(MoveTemp(Fact));
            }
        }
        bool Limited=false;
        const auto Hits = Intersections(A,TA,B,TB,false,Epsilon,IsCancelled,Limited);
        if(Limited)Result.Limitations.AddUnique(TEXT("pair_intersection_budget_exceeded"));
        auto AddContact=[&](FGeometryFact&& Fact)
        {
            // Full surface areas already describe their coplanar boundary contacts.
            if(Result.Facts.ContainsByPredicate([&](const FGeometryFact& Existing)
                {return Existing.AreaCm2.IsSet() && Existing.SurfaceA==Fact.SurfaceA && Existing.SurfaceB==Fact.SurfaceB;}))return;
            FGeometryFact* Existing=Result.Facts.FindByPredicate([&](const FGeometryFact& F)
                {return !F.AreaCm2.IsSet() && F.Kind==Fact.Kind && F.SurfaceA==Fact.SurfaceA && F.SurfaceB==Fact.SurfaceB;});
            if(!Existing){Fact.TriangleHitCount=1;Result.Facts.Add(MoveTemp(Fact));return;}
            ++Existing->TriangleHitCount;
            for(const auto& P:Fact.Witnesses)
            {
                if(Existing->Witnesses.ContainsByPredicate([&](const FVector3d& V){return (P-V).Size()<=Epsilon;}))continue;
                if(Existing->Witnesses.Num()<8)Existing->Witnesses.Add(P);else Existing->bWitnessesTruncated=true;
            }
        };
        for (const auto& Segment : Hits.Segments)
        {
            FGeometryFact Fact;
            Fact.Witnesses = {Point(TA, Segment.Point[0]), Point(TA, Segment.Point[1])};
            Fact.SurfaceA=A.TriangleSurface[Segment.TriangleID[0]];
            Fact.SurfaceB=B.TriangleSurface[Segment.TriangleID[1]];
            const FVector3d Direction=(Fact.Witnesses[1]-Fact.Witnesses[0]).GetSafeNormal();
            const FVector3d Mid=(Fact.Witnesses[0]+Fact.Witnesses[1])*.5;
            const bool InteriorA=Fact.SurfaceA!=INDEX_NONE && InteriorOnBothSides(A.Surfaces[Fact.SurfaceA],TA,Mid,Direction,Epsilon);
            const bool InteriorB=Fact.SurfaceB!=INDEX_NONE && InteriorOnBothSides(B.Surfaces[Fact.SurfaceB],TB,Mid,Direction,Epsilon);
            const bool NonParallel=Fact.SurfaceA!=INDEX_NONE && Fact.SurfaceB!=INDEX_NONE
                && FMath::Abs(Dot(WorldFrame(A.Surfaces[Fact.SurfaceA].Frame,TA).Normal,
                    WorldFrame(B.Surfaces[Fact.SurfaceB].Frame,TB).Normal))<1.0-1e-8;
            Fact.Kind = NonParallel && InteriorA && InteriorB ? TEXT("surface_crossing") : TEXT("touching");
            AddContact(MoveTemp(Fact));
        }
        for (const auto& PointHit : Hits.Points)
        {
            FGeometryFact Fact; Fact.Kind = TEXT("touching"); Fact.Witnesses.Add(Point(TA, PointHit.Point));
            Fact.SurfaceA=A.TriangleSurface[PointHit.TriangleID[0]];
            Fact.SurfaceB=B.TriangleSurface[PointHit.TriangleID[1]];
            AddContact(MoveTemp(Fact));
        }
        // With disjoint surfaces, one sample per connected island suffices for containment.
        // Boundary/intersection cases remain surface facts; no synthetic volume percentage.
        if(!Limited)
        {
            TSet<int32> TouchedA,TouchedB;
            for(const auto& H:Hits.Segments){TouchedA.Add(A.TriangleIsland[H.TriangleID[0]]);TouchedB.Add(B.TriangleIsland[H.TriangleID[1]]);}
            for(const auto& H:Hits.Points){TouchedA.Add(A.TriangleIsland[H.TriangleID[0]]);TouchedB.Add(B.TriangleIsland[H.TriangleID[1]]);}
            for(const auto& F:Result.Facts)
            {
                if(A.Surfaces.IsValidIndex(F.SurfaceA))TouchedA.Add(A.TriangleIsland[A.Surfaces[F.SurfaceA].Triangles[0]]);
                if(B.Surfaces.IsValidIndex(F.SurfaceB))TouchedB.Add(B.TriangleIsland[B.Surfaces[F.SurfaceB].Triangles[0]]);
            }
            if(B.bClosedOriented)for(int32 Island=0;Island<A.IslandSamples.Num();++Island)
            {
                if(TouchedA.Contains(Island))continue;const auto& P=A.IslandSamples[Island];
                if(Cancelled(IsCancelled))break;
                const auto W=Point(TA,P);if(InsideSolid(B,Point(TB.Inverse(),W)))
                {FGeometryFact F;F.Kind=TEXT("a_island_contained_in_b");F.Witnesses.Add(W);Result.Facts.Add(MoveTemp(F));}
            }
            if(A.bClosedOriented)for(int32 Island=0;Island<B.IslandSamples.Num();++Island)
            {
                if(TouchedB.Contains(Island))continue;const auto& P=B.IslandSamples[Island];
                if(Cancelled(IsCancelled))break;
                const auto W=Point(TB,P);if(InsideSolid(A,Point(TA.Inverse(),W)))
                {FGeometryFact F;F.Kind=TEXT("b_island_contained_in_a");F.Witnesses.Add(W);Result.Facts.Add(MoveTemp(F));}
            }
        }
        Result.bCancelled = Cancelled(IsCancelled);
        Result.bComplete = !Result.bCancelled && !Limited && !Result.Limitations.Contains(TEXT("surface_intersection_failed"));
        return Result;
    }
}
