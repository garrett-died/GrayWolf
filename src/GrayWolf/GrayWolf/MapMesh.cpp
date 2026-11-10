#include "stdafx.h"

#include "MapMesh.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>
#include <set>
#include <stdexcept>

// See MapMesh.h. The algorithms, constants and report texts are HydraX's. What differs is the storage: HydraX keeps
// Dictionary<tuple, List> lookups, which at 1,000,000 triangles cost minutes and gigabytes; here every lookup is a sorted
// array (GroupIndex), an open-addressing table with an O(1) clear (FlatMap64) or a k-d tree (nearest vertex). Where HydraX
// walks a hash set, the order of the result never depends on it (the self-test varies the table seed to prove that).
namespace Maps
{
using namespace RadiantGeometry;

namespace
{
    // ---- text

    // "0.###"-style text: the value with at most Decimals decimals, trailing zeros dropped (.NET's custom format)
    // .NET (Core 3.0 and later) takes 15 significant digits of the double and rounds that decimal text half away from zero, so 2.0005 (stored a hair
    // below) prints 2.001 and 0.0625 prints 0.063; printf would print 2.000 and 0.062. Checked against a C# run, see docs/hydrax-port/m2a.md.
    std::string Fixed(double Value, int Decimals)
    {
        if (std::isnan(Value)) return "NaN";
        if (std::isinf(Value)) return Value < 0 ? "-Infinity" : "Infinity";
        const bool Negative = std::signbit(Value);
        const double A = std::fabs(Value);
        std::string Digits;   // the significant digits, the first one at decimal position Pos - 1 (Pos digits lie before the point)
        int Pos = 1;
        if (A == 0)
        {
            Digits = "0";
        }
        else
        {
            char Buf[48];
            std::snprintf(Buf, sizeof(Buf), "%.14e", A);
            const std::string Text = Buf;   // d.dddddddddddddde+XXX
            Digits = Text.substr(0, 1) + Text.substr(2, 14);
            Pos = std::stoi(Text.substr(Text.find('e') + 1)) + 1;
        }
        const int Keep = Pos + Decimals;   // digits that stay
        if (Keep < 0)
        {
            Digits = "0";
            Pos = 1;
        }
        else if (Keep < static_cast<int>(Digits.size()))
        {
            const bool Up = Digits[static_cast<size_t>(Keep)] >= '5';
            Digits.resize(static_cast<size_t>(Keep));
            if (Up)
            {
                size_t I = Digits.size();
                while (I > 0 && Digits[I - 1] == '9') Digits[--I] = '0';
                if (I > 0)
                {
                    Digits[I - 1]++;
                }
                else
                {
                    Digits.insert(Digits.begin(), '1');
                    Pos++;
                }
            }
        }
        const auto DigitAt = [&](int Index) { return Index >= 0 && Index < static_cast<int>(Digits.size()) ? Digits[static_cast<size_t>(Index)] : '0'; };
        std::string Whole, Fraction;
        for (int I = 0; I < Pos; I++) Whole += DigitAt(I);
        if (Whole.empty()) Whole = "0";
        for (int K = 0; K < Decimals; K++) Fraction += DigitAt(Pos + K);
        while (!Fraction.empty() && Fraction.back() == '0') Fraction.pop_back();
        return (Negative ? "-" : "") + Whole + (Fraction.empty() ? "" : "." + Fraction);
    }

    // The shortest text that reads back as the same value, never in exponent form (.NET's default and "R" formats)
    template <class T>
    std::string Shortest(T Value)
    {
        std::array<char, 400> Storage{};
        const std::span<char> Buf(Storage);
        const auto Result = std::to_chars(Buf.data(), Buf.last(0).data(), Value, std::chars_format::fixed);   // last(0) is the end pointer, no arithmetic
        return std::string(Buf.data(), Result.ptr);
    }

    std::string Int(int64_t V) { return std::to_string(V); }

    // ---- tables

    uint64_t Mix(uint64_t X)
    {
        X += 0x9E3779B97F4A7C15ull;
        X = (X ^ (X >> 30)) * 0xBF58476D1CE4E5B9ull;
        X = (X ^ (X >> 27)) * 0x94D049BB133111EBull;
        return X ^ (X >> 31);
    }

    // uint64 -> int32, open addressing, Clear is O(1) (a stamp), so one table serves many small regions
    class FlatMap64
    {
    public:
        explicit FlatMap64(uint64_t SeedIn = 0) : Seed(SeedIn), Slots(64), Mask(63) {}

        void Clear()
        {
            Count = 0;
            if (++Stamp == 0)
            {
                for (Slot& S : Slots) S.Stamp = 0;
                Stamp = 1;
            }
        }

        bool Find(uint64_t Key, int& Value) const
        {
            size_t I = Mix(Key ^ Seed) & Mask;
            while (Slots[I].Stamp == Stamp)
            {
                if (Slots[I].Key == Key)
                {
                    Value = Slots[I].Value;
                    return true;
                }
                I = (I + 1) & Mask;
            }
            return false;
        }

        bool Contains(uint64_t Key) const
        {
            int Ignored = 0;
            return Find(Key, Ignored);
        }

        void Set(uint64_t Key, int Value)
        {
            if ((Count + 1) * 2 > Slots.size()) Grow();
            size_t I = Mix(Key ^ Seed) & Mask;
            while (Slots[I].Stamp == Stamp)
            {
                if (Slots[I].Key == Key)
                {
                    Slots[I].Value = Value;
                    return;
                }
                I = (I + 1) & Mask;
            }
            Slots[I] = { Key, Value, Stamp };
            Count++;
        }

    private:
        struct Slot
        {
            uint64_t Key = 0;
            int32_t Value = 0;
            uint32_t Stamp = 0;
        };

        void Grow()
        {
            std::vector<Slot> Live;
            Live.reserve(Count);
            for (const Slot& S : Slots)
                if (S.Stamp == Stamp) Live.push_back(S);
            Slots.assign(Slots.size() * 2, Slot{});
            Mask = Slots.size() - 1;
            Stamp = 1;
            Count = 0;
            for (const Slot& S : Live) Set(S.Key, S.Value);
        }

        uint64_t Seed;
        std::vector<Slot> Slots;
        size_t Mask;
        size_t Count = 0;
        uint32_t Stamp = 1;
    };

    // Items sorted by (key, id) with a table from a key to its first item: a multimap that is one array. Find gives the items of
    // a key, ascending by id, as a span of the sorted array.
    template <size_t N>
    class GroupIndex
    {
    public:
        using Key = std::array<int32_t, N>;
        struct Item
        {
            Key K;
            int32_t Id;
        };

        void Build(std::vector<Item>&& Source, uint64_t SeedIn)
        {
            Seed = SeedIn;
            Items = std::move(Source);
            std::sort(Items.begin(), Items.end(), [](const Item& A, const Item& B) { return A.K != B.K ? A.K < B.K : A.Id < B.Id; });
            size_t Groups = 0;
            for (size_t I = 0; I < Items.size(); I++)
                if (I == 0 || Items[I].K != Items[I - 1].K) Groups++;
            size_t Capacity = 16;
            while (Capacity < Groups * 2) Capacity *= 2;
            Table.assign(Capacity, -1);
            Mask = Capacity - 1;
            for (size_t I = 0; I < Items.size(); I++)
            {
                if (I != 0 && Items[I].K == Items[I - 1].K) continue;
                size_t Slot = Hash(Items[I].K) & Mask;
                while (Table[Slot] >= 0) Slot = (Slot + 1) & Mask;
                Table[Slot] = static_cast<int32_t>(I);
            }
        }

        std::span<const Item> Find(const Key& K) const
        {
            if (Table.empty()) return {};
            size_t Slot = Hash(K) & Mask;
            while (Table[Slot] >= 0)
            {
                const size_t First = static_cast<size_t>(Table[Slot]);
                if (Items[First].K == K)
                {
                    size_t End = First + 1;
                    while (End < Items.size() && Items[End].K == K) End++;
                    return std::span<const Item>(Items).subspan(First, End - First);
                }
                Slot = (Slot + 1) & Mask;
            }
            return {};
        }

        const std::vector<Item>& All() const { return Items; }

    private:
        uint64_t Hash(const Key& K) const
        {
            uint64_t H = Seed;
            for (const int32_t V : K) H = Mix(H ^ static_cast<uint64_t>(static_cast<uint32_t>(V)));
            return H;
        }

        uint64_t Seed = 0;
        std::vector<Item> Items;
        std::vector<int32_t> Table;
        size_t Mask = 0;
    };

    std::array<int32_t, 2> EdgeKey(int A, int B) { return A < B ? std::array<int32_t, 2>{ A, B } : std::array<int32_t, 2>{ B, A }; }

    uint64_t PackXY(int X, int Y) { return (static_cast<uint64_t>(static_cast<uint32_t>(Y)) << 32) | static_cast<uint32_t>(X); }

    V3 Point(std::span<const float> P, int I)
    {
        const size_t B = static_cast<size_t>(I) * 3;
        return { P[B], P[B + 1], P[B + 2] };
    }

    // An index outside the positions is a caller bug and is refused. A non-finite position is not: HydraX computes with it (NaN compares false, so such a
    // triangle simply finds no partner), and one bad vertex must not stop an export.
    void CheckInput(std::span<const float> Positions, std::span<const int32_t> Indices, const char* Who)
    {
        const size_t Count = Positions.size() / 3;
        for (const int32_t V : Indices)
        {
            if (V < 0 || static_cast<size_t>(V) >= Count) throw std::invalid_argument(std::string(Who) + ": a vertex index is outside the positions");
        }
    }

    // ---- k-d tree over the vertices of distinct positions: nearest other vertex, and all vertices within a radius

    class KdTree
    {
    public:
        KdTree(std::span<const float> PositionsIn, std::vector<int32_t> Ids) : Positions(PositionsIn), Order(std::move(Ids)), Axis(Order.size(), 0)
        {
            BuildRange(0, Order.size());
        }

        // The distance to the nearest vertex other than Self, searched up to Limit; Limit when none is that near
        double Nearest(const V3& P, int Self, double Limit) const
        {
            double Best = std::nextafter(Limit, std::numeric_limits<double>::infinity());
            NearestRange(0, Order.size(), P, Self, Best);
            return Best > Limit ? Limit * 2 : Best;
        }

        template <class Fn>
        void ForEachWithin(const V3& P, double Radius, const Fn& Visit) const
        {
            WithinRange(0, Order.size(), P, Radius, Visit);
        }

    private:
        static constexpr size_t Leaf = 8;

        double Coord(int32_t Id, int A) const { return Positions[static_cast<size_t>(Id) * 3 + A]; }

        void BuildRange(size_t Lo, size_t Hi)
        {
            if (Hi - Lo <= Leaf) return;
            float Min[3] = { INFINITY, INFINITY, INFINITY }, Max[3] = { -INFINITY, -INFINITY, -INFINITY };
            for (size_t I = Lo; I < Hi; I++)
                for (int A = 0; A < 3; A++)
                {
                    const float C = Positions[static_cast<size_t>(Order[I]) * 3 + A];
                    Min[A] = (std::min)(Min[A], C);
                    Max[A] = (std::max)(Max[A], C);
                }
            int Widest = 0;
            for (int A = 1; A < 3; A++)
                if (Max[A] - Min[A] > Max[Widest] - Min[Widest]) Widest = A;
            const size_t Mid = Lo + (Hi - Lo) / 2;
            std::nth_element(Order.begin() + Lo, Order.begin() + Mid, Order.begin() + Hi, [&](int32_t A, int32_t B) {
                const float Ca = Positions[static_cast<size_t>(A) * 3 + Widest], Cb = Positions[static_cast<size_t>(B) * 3 + Widest];
                return Ca != Cb ? Ca < Cb : A < B;
            });
            Axis[Mid] = static_cast<uint8_t>(Widest);
            BuildRange(Lo, Mid);
            BuildRange(Mid + 1, Hi);
        }

        void Consider(int32_t Id, const V3& P, int Self, double& Best) const
        {
            if (Id == Self) return;
            const double D = Length(Sub(Point(Positions, Id), P));
            if (D < Best) Best = D;
        }

        void NearestRange(size_t Lo, size_t Hi, const V3& P, int Self, double& Best) const
        {
            if (Hi - Lo <= Leaf)
            {
                for (size_t I = Lo; I < Hi; I++) Consider(Order[I], P, Self, Best);
                return;
            }
            const size_t Mid = Lo + (Hi - Lo) / 2;
            Consider(Order[Mid], P, Self, Best);
            const double Diff = P[Axis[Mid]] - Coord(Order[Mid], Axis[Mid]);
            if (Diff < 0)
            {
                NearestRange(Lo, Mid, P, Self, Best);
                if (std::fabs(Diff) <= Best) NearestRange(Mid + 1, Hi, P, Self, Best);
            }
            else
            {
                NearestRange(Mid + 1, Hi, P, Self, Best);
                if (std::fabs(Diff) <= Best) NearestRange(Lo, Mid, P, Self, Best);
            }
        }

        template <class Fn>
        void WithinRange(size_t Lo, size_t Hi, const V3& P, double Radius, const Fn& Visit) const
        {
            if (Hi - Lo <= Leaf)
            {
                for (size_t I = Lo; I < Hi; I++)
                    if (Length(Sub(Point(Positions, Order[I]), P)) <= Radius) Visit(Order[I]);
                return;
            }
            const size_t Mid = Lo + (Hi - Lo) / 2;
            if (Length(Sub(Point(Positions, Order[Mid]), P)) <= Radius) Visit(Order[Mid]);
            const double Diff = P[Axis[Mid]] - Coord(Order[Mid], Axis[Mid]);
            if (Diff <= Radius) WithinRange(Lo, Mid, P, Radius, Visit);
            if (-Diff <= Radius) WithinRange(Mid + 1, Hi, P, Radius, Visit);
        }

        std::span<const float> Positions;
        std::vector<int32_t> Order;
        std::vector<uint8_t> Axis;
    };

    // The distinct valid vertex ids the indices use, ascending
    std::vector<int32_t> UsedVertices(std::span<const int32_t> Indices, size_t VertexCount)
    {
        std::vector<int32_t> Used;
        Used.reserve(Indices.size());
        for (const int32_t V : Indices)
            if (V >= 0 && static_cast<size_t>(V) < VertexCount) Used.push_back(V);
        std::sort(Used.begin(), Used.end());
        Used.erase(std::unique(Used.begin(), Used.end()), Used.end());
        return Used;
    }

    // Vertex ids grouped by exact position (numeric equality, so -0 equals 0); each group ascending by id. Vertices with a non-finite position are left out.
    std::vector<std::vector<int32_t>> GroupByPosition(std::span<const float> Positions, const std::vector<int32_t>& Used)
    {
        std::vector<int32_t> Sorted;
        Sorted.reserve(Used.size());
        for (const int32_t V : Used)
        {
            const V3 P = Point(Positions, V);
            if (std::isfinite(P[0]) && std::isfinite(P[1]) && std::isfinite(P[2])) Sorted.push_back(V);
        }
        const auto Less = [&](int32_t A, int32_t B) {
            for (int K = 0; K < 3; K++)
            {
                const float Ca = Positions[static_cast<size_t>(A) * 3 + K], Cb = Positions[static_cast<size_t>(B) * 3 + K];
                if (Ca < Cb) return true;
                if (Cb < Ca) return false;
            }
            return A < B;
        };
        std::sort(Sorted.begin(), Sorted.end(), Less);
        std::vector<std::vector<int32_t>> Groups;
        for (const int32_t V : Sorted)
        {
            if (!Groups.empty() && Point(Positions, Groups.back().front()) == Point(Positions, V))
                Groups.back().push_back(V);
            else
                Groups.push_back({ V });
        }
        return Groups;
    }

    // ---- rectangles

    struct CutEntry
    {
        int64_t Area;
        int Y, X;
        size_t Start;     // the corner's record
        uint32_t Version; // the entry is current only while it matches the record's
    };

    // The heap's top is the largest area, then the first corner in (Y, X) order
    struct CutLater
    {
        bool operator()(const CutEntry& A, const CutEntry& B) const
        {
            if (A.Area != B.Area) return A.Area < B.Area;
            if (A.Y != B.Y) return A.Y > B.Y;
            return A.X > B.X;
        }
    };
}

// ---- grids

QuadGrid QuadGrid::FromGroups(const std::vector<std::vector<int32_t>>& Groups, bool IsTriangleIn)
{
    QuadGrid Grid;
    Grid.GroupCount = static_cast<int>(Groups.size());
    Grid.GroupSize = Groups.empty() ? 0 : static_cast<int>(Groups.front().size());
    Grid.IsTriangle = IsTriangleIn;
    for (const std::vector<int32_t>& G : Groups)
    {
        if (static_cast<int>(G.size()) != Grid.GroupSize) throw std::invalid_argument("QuadGrid: the groups differ in length");
        Grid.Points.insert(Grid.Points.end(), G.begin(), G.end());
    }
    return Grid;
}

// ---- the pair and border models

namespace PatchPairModel
{
    namespace
    {
        constexpr std::array<double, 9> Mean = { 2.111825, 0.945528, 4.210319, 2.768298, 2.440621, 0.777133, 1.165604, 2.583641, -0.667657 };
        constexpr std::array<double, 9> Deviation = { 1.79853, 0.181901, 2.505281, 2.509819, 2.606603, 0.353357, 1.135382, 2.466457, 1.040667 };
        constexpr std::array<double, 9> Weight = { -1.272366, 1.467635, -1.611836, 1.641744, -0.096286, 2.917866, -0.19378, -0.056371, 0.110424 };
        constexpr double Bias = -2.856788;

        // |cos| of the angle between the planes of the triangles (a, b, c) and (a, b, d). HydraX subtracts the float positions in float
        // before widening; so does this, because the logit was fitted on that.
        double NormalDotOf(std::span<const float> P, int A, int B, int C, int D)
        {
            const auto Cross3 = [&](int O, int U, int Vv) {
                const size_t Oo = static_cast<size_t>(O) * 3, Uu = static_cast<size_t>(U) * 3, Vw = static_cast<size_t>(Vv) * 3;
                const double X[3] = { static_cast<double>(P[Uu] - P[Oo]), static_cast<double>(P[Uu + 1] - P[Oo + 1]), static_cast<double>(P[Uu + 2] - P[Oo + 2]) };
                const double Y[3] = { static_cast<double>(P[Vw] - P[Oo]), static_cast<double>(P[Vw + 1] - P[Oo + 1]), static_cast<double>(P[Vw + 2] - P[Oo + 2]) };
                return V3{ (X[1] * Y[2]) - (X[2] * Y[1]), (X[2] * Y[0]) - (X[0] * Y[2]), (X[0] * Y[1]) - (X[1] * Y[0]) };
            };
            const V3 N1 = Cross3(A, B, C), N2 = Cross3(A, B, D);
            const double Dt = (N1[0] * N2[0]) + (N1[1] * N2[1]) + (N1[2] * N2[2]);
            const double L = std::sqrt((N1[0] * N1[0]) + (N1[1] * N1[1]) + (N1[2] * N1[2])) * std::sqrt((N2[0] * N2[0]) + (N2[1] * N2[1]) + (N2[2] * N2[2]));
            return std::fabs(Dt / (std::max)(L, 1e-12));
        }
    }

    double Logit(std::span<const float> Positions, int First, int Second, int A, int B, int C, int D, double Squareness)
    {
        const auto Dist = [&](int U, int Vv) { return Length(Sub(Point(Positions, U), Point(Positions, Vv))); };
        const std::array<int, 4> Ring = { C, A, D, B };
        std::array<double, 4> Sides{};
        for (int I = 0; I < 4; I++) Sides[I] = Dist(Ring[I], Ring[(I + 1) % 4]);
        const double Diagonal = Dist(A, B);
        const double Other = Dist(C, D);
        std::array<int, 4> Ids = Ring;
        std::sort(Ids.begin(), Ids.end());
        const double MaxSide = *std::max_element(Sides.begin(), Sides.end());
        const double MinSide = *std::min_element(Sides.begin(), Sides.end());
        const std::array<double, 9> F = {
            Squareness,
            NormalDotOf(Positions, A, B, C, D),
            std::log(1.0 + (Ids[3] - Ids[0])),
            std::log(1.0 + std::abs(A - B)),
            std::log(1.0 + std::abs(First - Second)),
            Diagonal / (std::max)(MaxSide, 1e-6),
            std::log(MaxSide / (std::max)(MinSide, 1e-6)),
            std::log(1.0 + (Ids[2] - Ids[1])),
            std::log((Diagonal + 1e-6) / (Other + 1e-6)),
        };
        double Z = Bias;
        for (size_t I = 0; I < F.size(); I++) Z += (F[I] - Mean[I]) / Deviation[I] * Weight[I];
        return Z;
    }
}

namespace PatchBorderModel
{
    namespace
    {
        constexpr std::array<double, 15> Mean = { 2.205076, 2.045133, 4.612473, -0.001681, -0.00247, 0.025369, 1.176918, 1.161033, 0.918424, 0.30259, 2.69149, 2.810326, 2.108676, 0.651587, 2.250559 };
        constexpr std::array<double, 15> Deviation = { 2.430574, 2.070477, 2.436717, 0.187943, 0.184997, 1.093339, 1.061487, 1.044315, 0.223318, 0.276071, 2.441204, 2.477739, 2.071752, 0.121454, 2.321043 };
        constexpr std::array<double, 15> Weight = { 0.446952, 2.620297, -1.383463, 0.022576, 0.003007, 0.073464, 0.106049, 0.038544, -0.506654, 0.361705, 0.873969, 0.550812, 0.036948, 1.19586, -0.243002 };
        constexpr double Bias = -3.393718;
        constexpr double Eps = 1e-6;

        // The vertices next to a and b in the ring other than b and a: the far side of the cell
        bool Far(const std::array<int32_t, 4>& Ring, int A, int B, int& Fa, int& Fb)
        {
            Fa = Fb = -1;
            const auto Ia = std::find(Ring.begin(), Ring.end(), A);
            const auto Ib = std::find(Ring.begin(), Ring.end(), B);
            if (Ia == Ring.end() || Ib == Ring.end()) return false;
            const int Pa = static_cast<int>(Ia - Ring.begin()), Pb = static_cast<int>(Ib - Ring.begin());
            if ((Pa + 1) % 4 == Pb)
            {
                Fa = Ring[(Pa + 3) % 4];
                Fb = Ring[(Pb + 1) % 4];
                return true;
            }
            if ((Pb + 1) % 4 == Pa)
            {
                Fa = Ring[(Pa + 1) % 4];
                Fb = Ring[(Pb + 3) % 4];
                return true;
            }
            return false;
        }

        V3 UnitNormal(const V3& P, const V3& Q, const V3& S)
        {
            const V3 N = Cross(Sub(Q, P), Sub(S, P));
            const double L = Length(N);
            return L > 1e-9 ? V3{ N[0] / L, N[1] / L, N[2] / L } : V3{ 0, 0, 0 };
        }

        double CosNormal(std::span<const float> Positions, const std::array<int32_t, 4>& X, const std::array<int32_t, 4>& Y)
        {
            const auto Normal = [&](const std::array<int32_t, 4>& R) {
                const V3 N1 = UnitNormal(Point(Positions, R[0]), Point(Positions, R[1]), Point(Positions, R[2]));
                const V3 N2 = UnitNormal(Point(Positions, R[0]), Point(Positions, R[2]), Point(Positions, R[3]));
                return Add(N1, N2);
            };
            const V3 Nx = Normal(X), Ny = Normal(Y);
            const double Lens = std::sqrt((Nx[0] * Nx[0]) + (Nx[1] * Nx[1]) + (Nx[2] * Nx[2])) * std::sqrt((Ny[0] * Ny[0]) + (Ny[1] * Ny[1]) + (Ny[2] * Ny[2]));
            return Dot(Nx, Ny) / (std::max)(Lens, 1e-9);
        }
    }

    std::optional<double> Logit(std::span<const float> Positions, const std::array<int32_t, 4>& RingX, const std::array<int32_t, 4>& RingY, int A, int B)
    {
        int Xa = 0, Xb = 0, Ya = 0, Yb = 0;
        if (!Far(RingX, A, B, Xa, Xb) || !Far(RingY, A, B, Ya, Yb)) return std::nullopt;

        const auto Dist = [](const V3& P, const V3& Q) { return Length(Sub(P, Q)); };
        const V3 Pa = Point(Positions, A), Pb = Point(Positions, B);
        const V3 Pxa = Point(Positions, Xa), Pxb = Point(Positions, Xb), Pya = Point(Positions, Ya), Pyb = Point(Positions, Yb);
        const double We = Dist(Pa, Pb);
        const double Fx = Dist(Pxa, Pxb);
        const double Fy = Dist(Pya, Pyb);
        const double Hx = (Dist(Pxa, Pa) + Dist(Pxb, Pb)) / 2;
        const double Hy = (Dist(Pya, Pa) + Dist(Pyb, Pb)) / 2;

        const auto Aspect = [&](const std::array<int32_t, 4>& Ring) {
            double Max = 0, Min = 0;
            for (int I = 0; I < 4; I++)
            {
                const double S = Dist(Point(Positions, Ring[I]), Point(Positions, Ring[(I + 1) % 4]));
                Max = I == 0 ? S : (std::max)(Max, S);
                Min = I == 0 ? S : (std::min)(Min, S);
            }
            return (std::max)(Max / (std::max)(Min, Eps), 0.0);
        };
        const auto Kink = [&](const V3& F1, const V3& G1, const V3& P) {
            const double Dx = F1[0] + G1[0] - (2 * P[0]), Dy = F1[1] + G1[1] - (2 * P[1]), Dz = F1[2] + G1[2] - (2 * P[2]);
            const double D = std::sqrt((Dx * Dx) + (Dy * Dy) + (Dz * Dz));
            return D / (std::max)(Dist(F1, P) + Dist(G1, P), Eps);
        };

        std::array<int32_t, 4> IdsX = RingX, IdsY = RingY;
        std::sort(IdsX.begin(), IdsX.end());
        std::sort(IdsY.begin(), IdsY.end());
        std::array<int32_t, 8> All{};
        std::copy(IdsX.begin(), IdsX.end(), All.begin());
        std::copy(IdsY.begin(), IdsY.end(), All.begin() + 4);
        const auto Median = [](const std::array<int32_t, 4>& S) { return (S[1] + S[2]) / 2.0; };
        const auto Mean4 = [](const std::array<int32_t, 4>& S) { return (static_cast<int64_t>(S[0]) + S[1] + S[2] + S[3]) / 4.0; };
        const auto Gap = [](double U, double Vv) { return std::log(1.0 + std::fabs(U - Vv)); };
        const int32_t Lo = IdsY[0], Hi = IdsY[3], Lo2 = IdsX[0], Hi2 = IdsX[3];
        int Inside = 0;
        for (const int32_t V : IdsX) Inside += (V >= Lo && V <= Hi) ? 1 : 0;
        for (const int32_t V : IdsY) Inside += (V >= Lo2 && V <= Hi2) ? 1 : 0;
        const double InsideShare = Inside / 8.0;
        const int32_t AllMax = *std::max_element(All.begin(), All.end()), AllMin = *std::min_element(All.begin(), All.end());
        const std::array<double, 15> F = {
            Gap(IdsX[0], IdsY[0]),
            Gap(IdsX[3], IdsY[3]),
            std::log(1.0 + (static_cast<double>(AllMax) - AllMin)),
            std::log((Fx + Eps) / (We + Eps)),
            std::log((Fy + Eps) / (We + Eps)),
            std::log((Hx + Eps) / (Hy + Eps)),
            std::log(Aspect(RingX)),
            std::log(Aspect(RingY)),
            std::fabs(CosNormal(Positions, RingX, RingY)),
            (std::max)(Kink(Pxa, Pya, Pa), Kink(Pxb, Pyb, Pb)),
            Gap(Median(IdsX), Median(IdsY)),
            Gap(Mean4(IdsX), Mean4(IdsY)),
            Gap(IdsX[2], IdsY[2]),
            InsideShare,
            Gap(IdsX[1], IdsY[1]),
        };
        double Z = Bias;
        for (size_t I = 0; I < F.size(); I++) Z += (F[I] - Mean[I]) / Deviation[I] * Weight[I];
        return Z;
    }
}

// ---- rectangles

namespace MeshInternal
{
    std::vector<CellRect> CutRectangles(std::span<const std::pair<int, int>> Cells)
    {
        // WHY a heap: HydraX measures every remaining corner cell again each round, which is quadratic on a region with many holes. A corner's
        // best rectangle depends only on the cells in a box that reaches right and down from it, so each corner keeps its measure and the box
        // it read; a round that removes a rectangle measures again only the corners whose box it touched (the area can grow as well as shrink:
        // a shorter column lets a broader rectangle fit). The winner is the one HydraX's scan picks: the largest area, ties to the first corner
        // in (Y, X) order, and within one corner the broad-first rectangle only when it is strictly larger than the wide-first one.
        struct Cell
        {
            int X, Y;
            bool Alive;
        };
        std::vector<Cell> List;
        List.reserve(Cells.size());
        FlatMap64 Index;
        for (const auto& [X, Y] : Cells)
        {
            if (Index.Contains(PackXY(X, Y))) continue;
            Index.Set(PackXY(X, Y), static_cast<int>(List.size()));
            List.push_back({ X, Y, true });
        }
        // coordinates are widened for the neighbours of a cell at the edge of int, which are never in the set
        const auto Alive = [&](int64_t X, int64_t Y) {
            if (X < std::numeric_limits<int>::min() || X > std::numeric_limits<int>::max() || Y < std::numeric_limits<int>::min() || Y > std::numeric_limits<int>::max()) return false;
            int N = 0;
            return Index.Find(PackXY(static_cast<int>(X), static_cast<int>(Y)), N) && List[static_cast<size_t>(N)].Alive;
        };
        struct Measured
        {
            CellRect Rect;
            int64_t ReachX, ReachY;   // the last column and row the measure read
        };
        const auto Measure = [&](int X, int Y) {
            const int64_t X0 = X, Y0 = Y;
            int64_t Wide = 1;
            while (Alive(X0 + Wide, Y0)) Wide++;
            int64_t High = 1;
            for (;;)
            {
                bool All = true;
                for (int64_t I = 0; I < Wide && All; I++) All = Alive(X0 + I, Y0 + High);
                if (!All) break;
                High++;
            }
            int64_t Tall = 1;
            while (Alive(X0, Y0 + Tall)) Tall++;
            int64_t Broad = 1;
            for (;;)
            {
                bool All = true;
                for (int64_t J = 0; J < Tall && All; J++) All = Alive(X0 + Broad, Y0 + J);
                if (!All) break;
                Broad++;
            }
            const CellRect Rect = Broad * Tall > Wide * High ? CellRect{ X, Y, static_cast<int>(Broad), static_cast<int>(Tall) } : CellRect{ X, Y, static_cast<int>(Wide), static_cast<int>(High) };
            return Measured{ Rect, X0 + (std::max)(Wide, Broad), Y0 + (std::max)(High, Tall) };
        };

        struct Corner
        {
            Measured M;
            uint32_t Version;
            bool Active;
        };
        std::vector<Corner> Corners;
        std::vector<size_t> Active;   // indices into Corners
        std::priority_queue<CutEntry, std::vector<CutEntry>, CutLater> Heap;
        const auto Enter = [&](int X, int Y) {
            Corners.push_back({ Measure(X, Y), 0, true });
            Active.push_back(Corners.size() - 1);
            const Corner& C = Corners.back();
            Heap.push({ static_cast<int64_t>(C.M.Rect.W) * C.M.Rect.H, Y, X, Corners.size() - 1, 0 });
        };
        for (const Cell& C : List)
            if (!Alive(int64_t{ C.X } - 1, C.Y) && !Alive(C.X, int64_t{ C.Y } - 1)) Enter(C.X, C.Y);

        std::vector<CellRect> Result;
        size_t Remaining = List.size();
        while (Remaining > 0 && !Heap.empty())
        {
            const CutEntry Top = Heap.top();
            Heap.pop();
            if (!Corners[Top.Start].Active || Corners[Top.Start].Version != Top.Version) continue;
            const CellRect R = Corners[Top.Start].M.Rect;
            for (int I = 0; I < R.W; I++)
                for (int J = 0; J < R.H; J++)
                {
                    int N = 0;
                    Index.Find(PackXY(R.X + I, R.Y + J), N);
                    List[static_cast<size_t>(N)].Alive = false;
                }
            Remaining -= static_cast<size_t>(R.W) * R.H;
            Result.push_back(R);

            // Corners whose cell went are done; corners whose box the rectangle touched are measured again
            size_t Kept = 0;
            for (const size_t Id : Active)
            {
                Corner& C = Corners[Id];
                const CellRect& At = C.M.Rect;
                if (!Alive(At.X, At.Y))
                {
                    C.Active = false;
                    continue;
                }
                Active[Kept++] = Id;
                if (R.X <= C.M.ReachX && int64_t{ R.X } + R.W - 1 >= At.X && R.Y <= C.M.ReachY && int64_t{ R.Y } + R.H - 1 >= At.Y)
                {
                    C.M = Measure(At.X, At.Y);
                    C.Version++;
                    Heap.push({ static_cast<int64_t>(C.M.Rect.W) * C.M.Rect.H, At.Y, At.X, Id, C.Version });
                }
            }
            Active.resize(Kept);

            // Cells that had a rectangle cell to their left or above may be corners now
            for (int J = 0; J < R.H; J++)
            {
                const int64_t X = int64_t{ R.X } + R.W, Y = int64_t{ R.Y } + J;
                if (Alive(X, Y) && !Alive(X - 1, Y) && !Alive(X, Y - 1)) Enter(static_cast<int>(X), static_cast<int>(Y));
            }
            for (int I = 0; I < R.W; I++)
            {
                const int64_t X = int64_t{ R.X } + I, Y = int64_t{ R.Y } + R.H;
                if (Alive(X, Y) && !Alive(X - 1, Y) && !Alive(X, Y - 1)) Enter(static_cast<int>(X), static_cast<int>(Y));
            }
        }
        return Result;
    }
}

// ---- the mesher

namespace QuadMesher
{
    namespace
    {
        constexpr double MinimumFlatness = 0.0;     // the least dot product of the two triangle normals for a leftover pair to become one cell
        constexpr double AugmentSquareness = 3.5;   // pairs worse than this squareness are never used by the augmenting step
        constexpr int AugmentDepth = 6;             // depth of its alternating paths
        constexpr int MaximumFanTriangles = 99;     // a row of 100 points (the compiler's limit is 128 per dimension)

        constexpr std::array<std::pair<int, int>, 4> Corners = { { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } } };

        struct Candidate
        {
            int First, Second;
            double Score;
            std::array<int32_t, 4> Ring;
        };

        struct Neighbour
        {
            int Other, Candidate;
        };

        struct RingCell
        {
            std::array<int32_t, 4> V;
            int Group;
            bool IsTriangle;
            int First, Second;
        };

        // Dot product of the two triangle normals (z,x,y and q,y,x) when the quad z, x, q, y is convex; -1 when it is not
        double Flatness(std::span<const float> Positions, int Z, int X, int Q, int Y)
        {
            const V3 Pz = Point(Positions, Z), Px = Point(Positions, X), Pq = Point(Positions, Q), Py = Point(Positions, Y);
            const V3 N1 = Cross(Sub(Px, Pz), Sub(Py, Pz));
            const V3 N2 = Cross(Sub(Py, Pq), Sub(Px, Pq));
            if (Length(N1) < 1e-9 || Length(N2) < 1e-9) return -1;
            const V3 Normal = Add(N1, N2);
            const std::array<V3, 4> Ring = { Pz, Px, Pq, Py };
            for (int I = 0; I < 4; I++)
                if (Dot(Cross(Sub(Ring[(I + 1) % 4], Ring[I]), Sub(Ring[(I + 2) % 4], Ring[(I + 1) % 4])), Normal) <= 0) return -1;
            return Dot(N1, N2) / (Length(N1) * Length(N2));
        }

        // Sum of the corners' deviation from a right angle (radians) of the quad: a cell of a patch is nearly rectangular, a pair of triangles from two neighbouring cells is not
        double Squareness(std::span<const float> Positions, const std::array<int32_t, 4>& Ring)
        {
            double Total = 0;
            for (int I = 0; I < 4; I++)
            {
                const V3 Pi = Point(Positions, Ring[I]);
                const V3 A = Sub(Point(Positions, Ring[(I + 3) % 4]), Pi);
                const V3 B = Sub(Point(Positions, Ring[(I + 1) % 4]), Pi);
                const double Cos = Dot(A, B) / (std::max)(Length(A) * Length(B), 1e-12);
                Total += std::fabs(std::acos(std::clamp(Cos, -1.0, 1.0)) - (std::acos(-1.0) / 2));
            }
            return Total;
        }

        // The rim edge of a triangle seen from a pivot corner: the two other corners in winding order
        std::pair<int, int> RimOf(std::span<const int32_t> Indices, int Triangle, int Pivot)
        {
            int K = 0;
            while (K < 3 && Indices[static_cast<size_t>(Triangle) * 3 + K] != Pivot) K++;
            return { Indices[static_cast<size_t>(Triangle) * 3 + (K + 1) % 3], Indices[static_cast<size_t>(Triangle) * 3 + (K + 2) % 3] };
        }

        // The triangle of a cell that has the edge a-b (a cell is two triangles; each of its four sides belongs to exactly one of them)
        int TriangleOnEdge(const RingCell& Cell, std::span<const int32_t> Indices, int A, int B)
        {
            for (const int T : { Cell.First, Cell.Second })
            {
                const size_t Base = static_cast<size_t>(T) * 3;
                const bool HasA = Indices[Base] == A || Indices[Base + 1] == A || Indices[Base + 2] == A;
                const bool HasB = Indices[Base] == B || Indices[Base + 1] == B || Indices[Base + 2] == B;
                if (HasA && HasB) return T;
            }
            return Cell.First;
        }

        struct FanItem
        {
            int Pivot;
            std::vector<int> Triangles;
            int Count;
            int Order;
        };

        // The longest fan first, then the one queued first
        struct FanLater
        {
            bool operator()(const FanItem& A, const FanItem& B) const { return A.Count != B.Count ? A.Count < B.Count : A.Order > B.Order; }
        };

        // Fans among the lone triangles: triangles of one label that share a pivot vertex and follow each other around it (the next one has the
        // directed edge pivot -> previous rim end, so the winding agrees and no gap or overlap is possible). A fan is cut where the caller reports a
        // border on the shared edge, where a rim vertex would repeat (a closed ring), where the angles at the pivot would reach a full turn, and
        // at MaximumFanTriangles. The longest fans are taken first, a triangle belongs to one fan only, a fan has at least two triangles.
        std::vector<std::pair<int, std::vector<int>>> FindFans(std::span<const float> Positions, std::span<const int32_t> Indices, std::span<const int32_t> Group, const std::vector<int>& Lone,
                                                               const std::function<bool(int, int, int, int)>& IsBorder, uint64_t Seed)
        {
            const size_t SlotCount = Indices.size();
            std::vector<int> Candidates;
            for (const int T : Lone)
            {
                const size_t B = static_cast<size_t>(T) * 3;
                if (Indices[B] != Indices[B + 1] && Indices[B + 1] != Indices[B + 2] && Indices[B] != Indices[B + 2]) Candidates.push_back(T);
            }
            GroupIndex<3> ByStart;
            {
                std::vector<GroupIndex<3>::Item> Items;
                Items.reserve(Candidates.size() * 3);
                for (const int T : Candidates)
                    for (int K = 0; K < 3; K++)
                        Items.push_back({ { Group[T], Indices[static_cast<size_t>(T) * 3 + K], Indices[static_cast<size_t>(T) * 3 + (K + 1) % 3] }, T });
                ByStart.Build(std::move(Items), Seed);
            }

            // The slot 3 * triangle + k names the link (triangle, pivot = its k-th corner). Next[slot] is the one triangle that continues the fan; a link that two triangles claim is dropped.
            const auto SlotOf = [&](int T, int Pivot) {
                for (int K = 0; K < 3; K++)
                    if (Indices[static_cast<size_t>(T) * 3 + K] == Pivot) return static_cast<int>(static_cast<size_t>(T) * 3 + K);
                return -1;
            };
            std::vector<int> Next(SlotCount, -1), Claims(SlotCount, 0);
            for (const int T : Candidates)
                for (int K = 0; K < 3; K++)
                {
                    const int Pivot = Indices[static_cast<size_t>(T) * 3 + K];
                    const int RimEnd = Indices[static_cast<size_t>(T) * 3 + (K + 2) % 3];
                    const auto Following = ByStart.Find({ Group[T], Pivot, RimEnd });
                    int Others = 0, Other = -1;
                    for (const auto& F : Following)
                        if (F.Id != T)
                        {
                            Others++;
                            Other = Other < 0 ? F.Id : Other;
                        }
                    if (Others == 1)
                    {
                        Next[static_cast<size_t>(T) * 3 + K] = Other;
                        Claims[static_cast<size_t>(SlotOf(Other, Pivot))]++;
                    }
                }
            for (size_t Slot = 0; Slot < SlotCount; Slot++)
                if (Next[Slot] >= 0 && Claims[static_cast<size_t>(SlotOf(Next[Slot], Indices[Slot]))] != 1) Next[Slot] = -1;

            const auto Point3 = [&](int I) { return Point(Positions, I); };
            const auto Angle = [&](int Pivot, int A, int B) {
                const V3 U = Sub(Point3(A), Point3(Pivot)), Vv = Sub(Point3(B), Point3(Pivot));
                return std::acos(std::clamp(Dot(U, Vv) / (std::max)(Length(U) * Length(Vv), 1e-12), -1.0, 1.0));
            };

            std::priority_queue<FanItem, std::vector<FanItem>, FanLater> Queue;
            int Order = 0;
            const auto Enqueue = [&](int Pivot, std::vector<int>&& Run) {
                if (Run.size() >= 2)
                {
                    const int Count = static_cast<int>(Run.size());
                    Queue.push({ Pivot, std::move(Run), Count, Order++ });
                }
            };

            // Walk every chain once, from a triangle that nothing leads into (else from any triangle of a closed chain), and cut it into valid fans.
            std::vector<char> Target(SlotCount, 0), Seen(SlotCount, 0);
            std::vector<size_t> Keys;
            for (size_t Slot = 0; Slot < SlotCount; Slot++)
                if (Next[Slot] >= 0)
                {
                    Keys.push_back(Slot);
                    Target[static_cast<size_t>(SlotOf(Next[Slot], Indices[Slot]))] = 1;
                }
            const auto ByTriangleThenPivot = [&](size_t A, size_t B) {
                if (A / 3 != B / 3) return A / 3 < B / 3;
                return Indices[A] < Indices[B];
            };
            std::sort(Keys.begin(), Keys.end(), ByTriangleThenPivot);
            std::vector<size_t> Starts;
            for (const size_t Slot : Keys)
                if (!Target[Slot]) Starts.push_back(Slot);
            Starts.insert(Starts.end(), Keys.begin(), Keys.end());

            for (const size_t Start : Starts)
            {
                if (Seen[Start]) continue;
                const int Pivot = Indices[Start];
                std::vector<int> Chain;
                size_t At = Start;
                while (!Seen[At])
                {
                    Seen[At] = 1;
                    Chain.push_back(static_cast<int>(At / 3));
                    if (Next[At] < 0) break;
                    At = static_cast<size_t>(SlotOf(Next[At], Pivot));
                }

                std::vector<int> Run;
                std::vector<int> Rim;
                double Total = 0;
                for (const int T : Chain)
                {
                    const auto [First, Second] = RimOf(Indices, T, Pivot);
                    const double Turn = Angle(Pivot, First, Second);
                    if (!Run.empty())
                    {
                        const bool Cut = static_cast<int>(Run.size()) >= MaximumFanTriangles || std::find(Rim.begin(), Rim.end(), Second) != Rim.end()
                                         || Total + Turn >= (2 * std::acos(-1.0)) - 1e-6 || (IsBorder && IsBorder(Run.back(), T, Pivot, First));
                        if (Cut)
                        {
                            Enqueue(Pivot, std::move(Run));
                            Run.clear();
                        }
                    }
                    if (Run.empty())
                    {
                        Rim = { First };
                        Total = 0;
                    }
                    Run.push_back(T);
                    Rim.push_back(Second);
                    Total += Turn;
                }
                Enqueue(Pivot, std::move(Run));
            }

            std::vector<char> Taken(SlotCount / 3 + 1, 0);
            std::vector<std::pair<int, std::vector<int>>> Fans;
            while (!Queue.empty())
            {
                FanItem Item = Queue.top();
                Queue.pop();
                const bool Any = std::any_of(Item.Triangles.begin(), Item.Triangles.end(), [&](int T) { return Taken[static_cast<size_t>(T)] != 0; });
                if (!Any)
                {
                    for (const int T : Item.Triangles) Taken[static_cast<size_t>(T)] = 1;
                    Fans.emplace_back(Item.Pivot, std::move(Item.Triangles));
                    continue;
                }

                // Part of it went to a longer fan; what is left in unbroken stretches is still a valid fan.
                std::vector<int> Piece;
                for (const int T : Item.Triangles)
                {
                    if (Taken[static_cast<size_t>(T)])
                    {
                        Enqueue(Item.Pivot, std::move(Piece));
                        Piece.clear();
                    }
                    else
                    {
                        Piece.push_back(T);
                    }
                }
                Enqueue(Item.Pivot, std::move(Piece));
            }
            return Fans;
        }

        // A cyclic shift of the ring whose corners fit the cell: points that already have coordinates must land on the cell's matching corner,
        // and at least the two points of the shared edge must be anchored. Shifts 4 to 7 walk the ring the other way round.
        template <class Lookup>
        bool OrderForCell(const Lookup& CoordinateOf, const std::array<int32_t, 4>& Ring, int CellX, int CellY, std::array<int32_t, 4>& Out)
        {
            for (int Shift = 0; Shift < 8; Shift++)
            {
                std::array<int32_t, 4> Candidate;
                if (Shift < 4)
                    Candidate = { Ring[Shift], Ring[(Shift + 1) % 4], Ring[(Shift + 2) % 4], Ring[(Shift + 3) % 4] };
                else
                    Candidate = { Ring[Shift - 4], Ring[((Shift - 4) + 3) % 4], Ring[((Shift - 4) + 2) % 4], Ring[((Shift - 4) + 1) % 4] };
                bool Fits = true;
                int Anchored = 0;
                for (int K = 0; K < 4 && Fits; K++)
                {
                    std::pair<int, int> Have;
                    if (CoordinateOf(Candidate[K], Have))
                    {
                        Anchored++;
                        Fits = Have == std::make_pair(CellX + Corners[K].first, CellY + Corners[K].second);
                    }
                }
                if (Fits && Anchored >= 2)
                {
                    Out = Candidate;
                    return true;
                }
            }
            return false;
        }

        // ---- strips

        // One cell of the strip step: a quad cell (two triangles) or one triangle. L holds its corners in the direction its triangles wind (a triangle: L[0..2]).
        struct StripCell
        {
            int Ring = 0;
            int Origin = 0;        // 0: a cell of a small rectangle, 1: a lone triangle, 2: a triangle of a fan
            bool IsTri = false;
            bool Chain = false;    // false: the two triangles of a quad wind against each other, the cell cannot be chained
            std::array<int32_t, 4> L{};
            int Corners() const { return IsTri ? 3 : 4; }
        };

        using StripColumn = std::array<int32_t, 2>;

        // A cell of the pool: its ring, where it came from (0 a cell of a small rectangle, 1 a lone triangle, 2 a triangle of a fan) and the piece (a held grid) it belongs to
        struct PoolCell
        {
            int Ring, Origin, Piece;
        };

        // A chain the strip step made, before it is decided whether to keep it
        struct StripChain
        {
            BuiltGrid Built;
            int FirstCell = 0;
            int64_t Cells = 0, Lone = 0, FanTriangles = 0;
            bool Strip = false;   // two cells or more
        };

        int UnionRoot(std::vector<int>& Parent, int X)
        {
            while (Parent[static_cast<size_t>(X)] != X)
            {
                Parent[static_cast<size_t>(X)] = Parent[static_cast<size_t>(Parent[static_cast<size_t>(X)])];
                X = Parent[static_cast<size_t>(X)];
            }
            return X;
        }

        // The strip step. A strip is a list of columns (u, v); the cell between column i and i + 1 is the loop u(i) -> u(i+1) -> v(i+1) -> v(i), which runs the way the
        // triangles wind. The next cell therefore has the directed edge v -> u of the last column among its own (every cell is wound the same way as the one before: the
        // shared edge is run in opposite directions), and its new column is the rest of its loop. A triangle keeps one of the column's points, (u, r) or (r, v), which is a
        // degenerate cell (the same point twice in a row of the mesh, as a fan has), or ends the strip with (r, r). Nothing is bridged: every cell shares an edge with the one before.
        // Cells are walked from the cells with the fewest neighbours (a seed), always into the neighbour that has the fewest neighbours left, first on, then, turned round, on the other end.
        void BuildStrips(std::span<const float> Positions, std::span<const int32_t> Indices, const std::vector<RingCell>& Rings, const std::vector<PoolCell>& Pool, std::vector<BuiltGrid>& Held, const BuildOptions& Options,
                         uint64_t Seed, BuildResult& Result)
        {
            const size_t Count = Pool.size();
            if (Count == 0) return;
            const auto IdAt = [&](int T, int K) { return Indices[static_cast<size_t>(T) * 3 + static_cast<size_t>(K)]; };
            // 1 when the triangle runs a -> b, -1 when it runs b -> a, 0 when it has no such edge
            const auto Directed = [&](int T, int A, int B) {
                for (int K = 0; K < 3; K++)
                {
                    const int X = IdAt(T, K), Y = IdAt(T, (K + 1) % 3);
                    if (X == A && Y == B) return 1;
                    if (X == B && Y == A) return -1;
                }
                return 0;
            };

            std::vector<StripCell> Cells(Count);
            for (size_t I = 0; I < Count; I++)
            {
                const RingCell& Ring = Rings[static_cast<size_t>(Pool[I].Ring)];
                StripCell& C = Cells[I];
                C.Ring = Pool[I].Ring;
                C.Origin = Pool[I].Origin;
                C.IsTri = Ring.IsTriangle;
                if (C.IsTri)
                {
                    C.L = { IdAt(Ring.First, 0), IdAt(Ring.First, 1), IdAt(Ring.First, 2), IdAt(Ring.First, 2) };
                    C.Chain = true;
                    continue;
                }
                int Way = 0;
                bool Ok = true;
                for (const int T : { Ring.First, Ring.Second })
                {
                    int Mine = 0;
                    for (int K = 0; K < 4; K++)
                    {
                        const int H = Directed(T, Ring.V[static_cast<size_t>(K)], Ring.V[static_cast<size_t>((K + 1) % 4)]);
                        if (H == 0) continue;
                        if (Mine == 0)
                            Mine = H;
                        else if (Mine != H)
                            Ok = false;
                    }
                    if (Mine == 0)
                        Ok = false;
                    else if (Way == 0)
                        Way = Mine;
                    else if (Way != Mine)
                        Ok = false;
                }
                C.L = Ring.V;
                if (Way < 0) C.L = { Ring.V[3], Ring.V[2], Ring.V[1], Ring.V[0] };
                C.Chain = Ok;
            }

            // the cells by the directed edges of their loops
            GroupIndex<2> ByEdge;
            {
                std::vector<GroupIndex<2>::Item> Items;
                Items.reserve(Count * 4);
                for (size_t I = 0; I < Count; I++)
                {
                    if (!Cells[I].Chain) continue;
                    const int N = Cells[I].Corners();
                    for (int K = 0; K < N; K++) Items.push_back({ { Cells[I].L[static_cast<size_t>(K)], Cells[I].L[static_cast<size_t>((K + 1) % N)] }, static_cast<int>(I) });
                }
                ByEdge.Build(std::move(Items), Seed);
            }
            std::vector<char> Used(Count, 0);

            const auto SameCorners = [&](const StripCell& A, const StripCell& B) {
                if (A.Corners() != B.Corners()) return false;
                for (int I = 0; I < A.Corners(); I++)
                {
                    bool Found = false;
                    for (int J = 0; J < B.Corners(); J++) Found = Found || A.L[static_cast<size_t>(I)] == B.L[static_cast<size_t>(J)];
                    if (!Found) return false;
                }
                return true;
            };
            const std::function<bool(int, int, int, int)>& Border = Options.IsBorderQuiet ? Options.IsBorderQuiet : Options.IsBorder;
            // May cell Next follow cell Last across the edge a-b? Next must be free and pass every rule the grid walk applies across an edge.
            const auto Join = [&](int Last, int Next, int A, int B, bool IgnoreUsed = false) {
                if ((!IgnoreUsed && Used[static_cast<size_t>(Next)]) || Last == Next) return false;
                const StripCell &Cl = Cells[static_cast<size_t>(Last)], &Cn = Cells[static_cast<size_t>(Next)];
                if (!Cn.Chain || Rings[static_cast<size_t>(Cl.Ring)].Group != Rings[static_cast<size_t>(Cn.Ring)].Group) return false;
                // the other side of a two-sided surface has the same corners; it is never the next cell
                if (SameCorners(Cl, Cn)) return false;
                if (Border && Border(TriangleOnEdge(Rings[static_cast<size_t>(Cl.Ring)], Indices, A, B), TriangleOnEdge(Rings[static_cast<size_t>(Cn.Ring)], Indices, A, B), A, B)) return false;
                if (Options.VertexOrderBorders && !Cl.IsTri && !Cn.IsTri)
                    if (const std::optional<double> Logit = PatchBorderModel::Logit(Positions, Cl.L, Cn.L, A, B))
                        if (*Logit > PatchBorderModel::CutLogit) return false;
                return true;
            };
            // the cells that could follow the column (u, v) of cell From
            const auto ExitCount = [&](int From, const StripColumn& Col) {
                int N = 0;
                for (const auto& E : ByEdge.Find({ Col[1], Col[0] }))
                    if (Join(From, E.Id, Col[0], Col[1])) N++;
                return N;
            };

            struct Pick
            {
                int Cell = -1;
                StripColumn Col{};
                std::array<int, 3> Rank{};   // lower is better: no exit at all, the exit's cell count, the option
                int Tie = 0;
                bool Better(const Pick& O) const { return Cell < 0 || (O.Cell >= 0 && (O.Rank < Rank || (O.Rank == Rank && O.Tie < Tie))); }
            };
            // The best way to take cell Nc once its edge V -> U is the last column (U, V). Prev: the column before, to keep a fan's pivot when nothing follows.
            const auto Consider = [&](int Nc, int32_t V, int32_t U, const StripColumn* Prev, int Tie, Pick& Best) {
                const StripCell& C = Cells[static_cast<size_t>(Nc)];
                const int N = C.Corners();
                int K = 0;
                while (K < N && !(C.L[static_cast<size_t>(K)] == V && C.L[static_cast<size_t>((K + 1) % N)] == U)) K++;
                if (K == N) return;
                std::array<StripColumn, 3> Choices{};
                std::array<int, 3> Preference{};
                int ChoiceCount = 0;
                if (!C.IsTri)
                {
                    Choices[0] = { C.L[static_cast<size_t>((K + 2) % 4)], C.L[static_cast<size_t>((K + 3) % 4)] };
                    ChoiceCount = 1;
                }
                else
                {
                    const int32_t R = C.L[static_cast<size_t>((K + 2) % 3)];
                    Choices = { StripColumn{ U, R }, StripColumn{ R, V }, StripColumn{ R, R } };
                    // keeps u, keeps v, ends; with no exit the one that keeps the pivot of the strip so far is better
                    Preference = { Prev && (*Prev)[0] == U ? 0 : 3, Prev && (*Prev)[1] == V ? 0 : 3, 2 };
                    ChoiceCount = 3;
                }
                for (int O = 0; O < ChoiceCount; O++)
                {
                    const int Exits = ExitCount(Nc, Choices[static_cast<size_t>(O)]);
                    Pick P;
                    P.Cell = Nc;
                    P.Col = Choices[static_cast<size_t>(O)];
                    P.Rank = { Exits > 0 ? 0 : 1, Exits, Preference[static_cast<size_t>(O)] };
                    P.Tie = Tie;
                    if (Best.Better(P)) Best = P;
                }
            };

            // the cells with the fewest neighbours first
            std::vector<int> Order(Count);
            std::iota(Order.begin(), Order.end(), 0);
            std::vector<int> Degree(Count, 0);
            for (size_t I = 0; I < Count; I++)
            {
                if (!Cells[I].Chain) continue;
                const int N = Cells[I].Corners();
                for (int K = 0; K < N; K++)
                    Degree[I] += static_cast<int>(ByEdge.Find({ Cells[I].L[static_cast<size_t>((K + 1) % N)], Cells[I].L[static_cast<size_t>(K)] }).size());
            }
            std::stable_sort(Order.begin(), Order.end(), [&](int A, int B) { return Degree[static_cast<size_t>(A)] < Degree[static_cast<size_t>(B)]; });

            const size_t MaxColumns = static_cast<size_t>((std::max)(Options.MaxStripPoints, 3));
            std::vector<StripColumn> Col;
            std::vector<int> CellOf;
            const auto Extend = [&]() {
                while (Col.size() < MaxColumns)
                {
                    const int Last = CellOf.back();
                    const StripColumn End = Col.back();
                    Pick Best;
                    int Tie = 0;
                    const StripColumn* Prev = Col.size() >= 2 ? &Col[Col.size() - 2] : nullptr;
                    for (const auto& E : ByEdge.Find({ End[1], End[0] }))
                    {
                        if (!Join(Last, E.Id, End[0], End[1])) continue;
                        Consider(E.Id, End[1], End[0], Prev, Tie++, Best);
                    }
                    if (Best.Cell < 0) break;
                    Used[static_cast<size_t>(Best.Cell)] = 1;
                    CellOf.push_back(Best.Cell);
                    Col.push_back(Best.Col);
                }
            };
            // the same strip walked from its other end
            const auto Flip = [&]() {
                std::reverse(Col.begin(), Col.end());
                for (StripColumn& C : Col) std::swap(C[0], C[1]);
                std::reverse(CellOf.begin(), CellOf.end());
            };

            std::vector<StripChain> Chains;
            for (const int Start : Order)
            {
                if (Used[static_cast<size_t>(Start)]) continue;
                const StripCell& S = Cells[static_cast<size_t>(Start)];
                Used[static_cast<size_t>(Start)] = 1;
                const RingCell& StartRing = Rings[static_cast<size_t>(S.Ring)];
                QuadGrid Grid;
                StripChain Chain;
                Chain.FirstCell = Start;
                Chain.Cells = 1;
                if (!S.Chain)
                {
                    // the two triangles wind against each other: one cell by itself
                    Grid = QuadGrid::FromGroups({ { StartRing.V[0], StartRing.V[1] }, { StartRing.V[3], StartRing.V[2] } });
                    Grid.Triangles = { StartRing.First, StartRing.Second };
                    Chain.Built = { std::move(Grid), StartRing.Group };
                    Chains.push_back(std::move(Chain));
                    continue;
                }
                // the entry edge: the side whose best way out leads on to the fewest cells
                Pick Best;
                for (int K = 0; K < S.Corners(); K++) Consider(Start, S.L[static_cast<size_t>(K)], S.L[static_cast<size_t>((K + 1) % S.Corners())], nullptr, K, Best);
                if (Best.Cell < 0) throw std::logic_error("QuadMesher.Build: a strip cell has no entry");
                {
                    const int K = Best.Tie;
                    Col.assign({ StripColumn{ S.L[static_cast<size_t>((K + 1) % S.Corners())], S.L[static_cast<size_t>(K)] }, Best.Col });
                    CellOf.assign(1, Start);
                }
                Extend();
                Flip();
                Extend();

                if (CellOf.size() == 1)
                {
                    // the layouts the rectangle and lone triangle paths wrote
                    if (S.IsTri)
                    {
                        const size_t B = static_cast<size_t>(StartRing.First) * 3;
                        Grid = QuadGrid::FromGroups({ { Indices[B], Indices[B + 1] }, { Indices[B + 2], Indices[B + 2] } }, true);
                        Grid.Triangles = { StartRing.First };
                    }
                    else
                    {
                        Grid = QuadGrid::FromGroups({ { Col[0][0], Col[1][0] }, { Col[0][1], Col[1][1] } });
                        Grid.Triangles = { StartRing.First, StartRing.Second };
                    }
                    Chain.Built = { std::move(Grid), StartRing.Group };
                    Chains.push_back(std::move(Chain));
                    continue;
                }

                Grid.GroupCount = 2;
                Grid.GroupSize = static_cast<int>(Col.size());
                Grid.Points.reserve(Col.size() * 2);
                for (const StripColumn& C : Col) Grid.Points.push_back(C[0]);
                for (const StripColumn& C : Col) Grid.Points.push_back(C[1]);
                bool AllTriangles = true, ConstantU = true, ConstantV = true;
                Grid.CellStart.push_back(0);
                for (size_t I = 0; I < CellOf.size(); I++)
                {
                    const StripCell& C = Cells[static_cast<size_t>(CellOf[I])];
                    const RingCell& Ring = Rings[static_cast<size_t>(C.Ring)];
                    Grid.Triangles.push_back(Ring.First);
                    if (!C.IsTri) Grid.Triangles.push_back(Ring.Second);
                    Grid.CellStart.push_back(static_cast<int32_t>(Grid.Triangles.size()));
                    AllTriangles = AllTriangles && C.IsTri;
                    ConstantU = ConstantU && Col[I + 1][0] == Col[0][0];
                    ConstantV = ConstantV && Col[I + 1][1] == Col[0][1];
                    if (C.IsTri && C.Origin == 1) Chain.Lone++;
                    if (C.IsTri && C.Origin == 2) Chain.FanTriangles++;
                }
                if (AllTriangles && (ConstantU || ConstantV))
                {
                    Grid.IsFan = true;   // triangles around one pivot: the layout the fan path wrote
                    Grid.CellStart.clear();
                }
                else
                {
                    Grid.IsStrip = true;
                }
                Chain.Strip = true;
                Chain.Cells = static_cast<int64_t>(CellOf.size());
                Chain.Built = { std::move(Grid), StartRing.Group };
                Chains.push_back(std::move(Chain));
            }

            // Keep the chains only where they make fewer grids than the pieces they replace: the cells that can follow each other (the rules of Join, used or not) and the cells of
            // one piece are one component, and a component with as many chains as pieces keeps its pieces.
            std::vector<int> Parent(Count);
            std::iota(Parent.begin(), Parent.end(), 0);
            const auto Unite = [&](int A, int B) {
                const int Ra = UnionRoot(Parent, A), Rb = UnionRoot(Parent, B);
                if (Ra != Rb) Parent[static_cast<size_t>((std::max)(Ra, Rb))] = (std::min)(Ra, Rb);
            };
            std::vector<int> PieceCell(Held.size(), -1);
            for (size_t I = 0; I < Count; I++)
            {
                int& First = PieceCell[static_cast<size_t>(Pool[I].Piece)];
                if (First < 0)
                    First = static_cast<int>(I);
                else
                    Unite(First, static_cast<int>(I));
                if (!Cells[I].Chain) continue;
                const int N = Cells[I].Corners();
                for (int K = 0; K < N; K++)
                {
                    const int32_t A = Cells[I].L[static_cast<size_t>(K)], B = Cells[I].L[static_cast<size_t>((K + 1) % N)];
                    for (const auto& E : ByEdge.Find({ B, A }))
                        if (E.Id > static_cast<int>(I) && Join(static_cast<int>(I), E.Id, A, B, true)) Unite(static_cast<int>(I), E.Id);
                }
            }
            std::vector<int64_t> PiecesIn(Count, 0), ChainsIn(Count, 0);
            // a piece over the point limit becomes several meshes when it is written (SplitToLimit); a chain never does
            const auto MeshesOf = [&](const QuadGrid& G) {
                const int64_t Step = (std::max)(Options.MaxStripPoints, 3) - 1;
                return (std::max<int64_t>(1, (G.GroupCount - 1 + Step - 1) / Step)) * (std::max<int64_t>(1, (G.GroupSize - 1 + Step - 1) / Step));
            };
            for (size_t P = 0; P < Held.size(); P++) PiecesIn[static_cast<size_t>(UnionRoot(Parent, PieceCell[P]))] += MeshesOf(Held[P].Grid);
            for (const StripChain& C : Chains) ChainsIn[static_cast<size_t>(UnionRoot(Parent, C.FirstCell))]++;
            const auto Keep = [&](int Cell) {
                const size_t R = static_cast<size_t>(UnionRoot(Parent, Cell));
                return ChainsIn[R] < PiecesIn[R];
            };
            for (size_t P = 0; P < Held.size(); P++)
                if (!Keep(PieceCell[P])) Result.Grids.push_back(std::move(Held[P]));
            for (StripChain& C : Chains)
            {
                if (!Keep(C.FirstCell)) continue;
                if (C.Strip)
                {
                    Result.StripsMade++;
                    Result.StripCells += C.Cells;
                    Result.StripLoneAbsorbed += C.Lone;
                    Result.StripFanTrianglesAbsorbed += C.FanTriangles;
                }
                Result.Grids.push_back(std::move(C.Built));
            }
        }
    }

    V3 GridFacing(const QuadGrid& Grid, const std::function<V3(int)>& Position)
    {
        V3 Sum = { 0, 0, 0 };
        for (int G = 0; G + 1 < Grid.GroupCount; G++)
            for (int I = 0; I + 1 < Grid.GroupSize; I++)
            {
                const V3 A = Position(Grid.At(G, I));
                Sum = Add(Sum, Cross(Sub(Position(Grid.At(G + 1, I)), A), Sub(Position(Grid.At(G, I + 1)), A)));
            }
        return Sum;
    }

    QuadGrid Reversed(const QuadGrid& Grid)
    {
        QuadGrid Out = Grid;
        for (int G = 0; G < Grid.GroupCount; G++)
            std::copy_n(Grid.Points.begin() + static_cast<size_t>(Grid.GroupCount - 1 - G) * Grid.GroupSize, Grid.GroupSize, Out.Points.begin() + static_cast<size_t>(G) * Grid.GroupSize);
        return Out;
    }

    BuildResult Build(std::span<const float> Positions, std::span<const int32_t> Indices, std::span<const int32_t> Group, const BuildOptions& Options)
    {
        BuildResult Result;
        const int TriangleCount = static_cast<int>(Indices.size() / 3);
        if (Group.size() < static_cast<size_t>(TriangleCount)) throw std::invalid_argument("QuadMesher.Build: fewer labels than triangles");
        CheckInput(Positions, Indices.first(static_cast<size_t>(TriangleCount) * 3), "QuadMesher.Build");
        const size_t VertexCount = Positions.size() / 3;
        const bool Vob = Options.VertexOrderBorders;
        const uint64_t Seed = Options.HashSeed;
        const auto IdAt = [&](int T, int K) { return Indices[static_cast<size_t>(T) * 3 + static_cast<size_t>(K)]; };

        GroupIndex<2> Edges;
        {
            std::vector<GroupIndex<2>::Item> Items;
            Items.reserve(static_cast<size_t>(TriangleCount) * 3);
            for (int T = 0; T < TriangleCount; T++)
                for (int K = 0; K < 3; K++) Items.push_back({ EdgeKey(IdAt(T, K), IdAt(T, (K + 1) % 3)), T });
            Edges.Build(std::move(Items), Seed);
        }

        // Candidate cap: the pairs of triangles that share an edge bound the candidates. A soup that puts tens of thousands of triangles on one edge would
        // need gigabytes, so above the cap no pairing is tried: every triangle stays lone (fans are still looked for), the result says so, and the export survives.
        // Below the cap nothing changes.
        int64_t Potential = 0;
        {
            const auto& All = Edges.All();
            for (size_t I = 0; I < All.size();)
            {
                size_t End = I + 1;
                while (End < All.size() && All[End].K == All[I].K) End++;
                const int64_t G = static_cast<int64_t>(End - I);
                Potential += G * (G - 1) / 2;
                I = End;
            }
        }
        Result.CandidatePotential = Potential;
        const bool Capped = Potential > std::clamp<int64_t>(Options.MaxCandidates, 0, int64_t{ 1 } << 30);
        if (Capped)
        {
            Result.CandidatesCapped = true;
            Result.TrianglesLeftLone = TriangleCount;
        }

        // Every pair of neighbouring triangles that forms a convex quad is a candidate cell; the pairs whose quad is closest to a rectangle are taken first,
        // and a triangle left over becomes a degenerate mesh. (A "longest edge of both" rule was tried first and measured worse: on the Giant 13% of the
        // patch cells have a diagonal shorter than a side.)
        std::vector<Candidate> Candidates;
        for (int T = 0; T < TriangleCount && !Capped; T++)
            for (int K = 0; K < 3; K++)
            {
                const int X = IdAt(T, K), Y = IdAt(T, (K + 1) % 3), Z = IdAt(T, (K + 2) % 3);
                for (const auto& Shared : Edges.Find(EdgeKey(X, Y)))
                {
                    const int O = Shared.Id;
                    if (O <= T || Group[O] != Group[T]) continue;
                    int Q = -1;
                    for (int J = 0; J < 3 && Q < 0; J++)
                        if (IdAt(O, J) != X && IdAt(O, J) != Y) Q = IdAt(O, J);
                    if (Q < 0) continue;
                    if ((!Options.CanPair || Options.CanPair(T, O)) && Flatness(Positions, Z, X, Q, Y) > MinimumFlatness)
                    {
                        const std::array<int32_t, 4> Ring = { Z, X, Q, Y };
                        const double Square = Squareness(Positions, Ring);
                        // With the vertex numbering known to follow the compiler's patch order, the pair classifier ranks the pairs; otherwise the squareness does.
                        const double Score = Vob ? -PatchPairModel::Logit(Positions, T, O, Ring[1], Ring[3], Ring[0], Ring[2], Square) : Square;
                        Candidates.push_back({ T, O, Score, Ring });
                    }
                }
            }

        // Greedy first (most rectangular pairs first), then augmenting paths: an unmatched triangle takes a neighbour from its partner and the partner looks for
        // another, up to AugmentDepth steps, using only pairs that are rectangular enough. On the Giant this recovers 94.5% of the stock cells as exact pairs
        // (greedy alone 89.0%) and leaves 1,597 unmatched triangles (7,055 before).
        const double Limit = Vob ? -PatchPairModel::MinimumLogit : AugmentSquareness;
        std::vector<int64_t> NeighbourStart(static_cast<size_t>(TriangleCount) + 1, 0);
        for (const Candidate& C : Candidates)
        {
            NeighbourStart[static_cast<size_t>(C.First) + 1]++;
            NeighbourStart[static_cast<size_t>(C.Second) + 1]++;
        }
        for (size_t T = 0; T < static_cast<size_t>(TriangleCount); T++) NeighbourStart[T + 1] += NeighbourStart[T];
        std::vector<Neighbour> Neighbours(static_cast<size_t>(NeighbourStart.back()));
        {
            std::vector<int64_t> Cursor(NeighbourStart.begin(), NeighbourStart.end() - 1);
            for (size_t C = 0; C < Candidates.size(); C++)
            {
                Neighbours[static_cast<size_t>(Cursor[static_cast<size_t>(Candidates[C].First)]++)] = { Candidates[C].Second, static_cast<int>(C) };
                Neighbours[static_cast<size_t>(Cursor[static_cast<size_t>(Candidates[C].Second)]++)] = { Candidates[C].First, static_cast<int>(C) };
            }
        }

        std::vector<int> Mate(static_cast<size_t>(TriangleCount), -1);
        {
            std::vector<int> Order(Candidates.size());
            std::iota(Order.begin(), Order.end(), 0);
            std::sort(Order.begin(), Order.end(), [&](int A, int B) {
                const Candidate &Ca = Candidates[static_cast<size_t>(A)], &Cb = Candidates[static_cast<size_t>(B)];
                if (Ca.Score != Cb.Score) return Ca.Score < Cb.Score;
                if (Ca.First != Cb.First) return Ca.First < Cb.First;
                if (Ca.Second != Cb.Second) return Ca.Second < Cb.Second;
                return A < B;
            });
            for (const int Ci : Order)
            {
                const Candidate& C = Candidates[static_cast<size_t>(Ci)];
                if (Mate[static_cast<size_t>(C.First)] < 0 && Mate[static_cast<size_t>(C.Second)] < 0 && (!Vob || C.Score < Limit))
                {
                    Mate[static_cast<size_t>(C.First)] = C.Second;
                    Mate[static_cast<size_t>(C.Second)] = C.First;
                }
            }
        }

        std::vector<uint32_t> SeenStamp(static_cast<size_t>(TriangleCount), 0);
        uint32_t Stamp = 0;
        const auto SeenAdd = [&](int T) {
            if (SeenStamp[static_cast<size_t>(T)] == Stamp) return false;
            SeenStamp[static_cast<size_t>(T)] = Stamp;
            return true;
        };
        std::function<bool(int, int)> Augment = [&](int T, int Depth) {
            std::vector<Neighbour> Sorted(Neighbours.begin() + NeighbourStart[static_cast<size_t>(T)], Neighbours.begin() + NeighbourStart[static_cast<size_t>(T) + 1]);
            std::stable_sort(Sorted.begin(), Sorted.end(), [&](const Neighbour& A, const Neighbour& B) { return Candidates[static_cast<size_t>(A.Candidate)].Score < Candidates[static_cast<size_t>(B.Candidate)].Score; });
            for (const Neighbour& N : Sorted)
            {
                if (Candidates[static_cast<size_t>(N.Candidate)].Score >= Limit || !SeenAdd(N.Other)) continue;
                if (Mate[static_cast<size_t>(N.Other)] < 0)
                {
                    Mate[static_cast<size_t>(N.Other)] = T;
                    Mate[static_cast<size_t>(T)] = N.Other;
                    return true;
                }
                const int Partner = Mate[static_cast<size_t>(N.Other)];
                if (Depth > 0 && SeenAdd(Partner))
                {
                    Mate[static_cast<size_t>(N.Other)] = -1;
                    Mate[static_cast<size_t>(Partner)] = -1;
                    if (Augment(Partner, Depth - 1))
                    {
                        Mate[static_cast<size_t>(N.Other)] = T;
                        Mate[static_cast<size_t>(T)] = N.Other;
                        return true;
                    }
                    Mate[static_cast<size_t>(N.Other)] = Partner;
                    Mate[static_cast<size_t>(Partner)] = N.Other;
                }
            }
            return false;
        };
        for (int T = 0; T < TriangleCount; T++)
            if (Mate[static_cast<size_t>(T)] < 0 && NeighbourStart[static_cast<size_t>(T) + 1] > NeighbourStart[static_cast<size_t>(T)])
            {
                Stamp++;
                SeenAdd(T);
                Augment(T, AugmentDepth);
            }

        std::vector<RingCell> Rings;
        std::vector<char> Paired(static_cast<size_t>(TriangleCount), 0);
        for (int T = 0; T < TriangleCount; T++)
        {
            const int Other = Mate[static_cast<size_t>(T)];
            if (Other > T)
            {
                std::array<int32_t, 4> Ring{};
                for (int64_t N = NeighbourStart[static_cast<size_t>(T)]; N < NeighbourStart[static_cast<size_t>(T) + 1]; N++)
                    if (Neighbours[static_cast<size_t>(N)].Other == Other)
                    {
                        Ring = Candidates[static_cast<size_t>(Neighbours[static_cast<size_t>(N)].Candidate)].Ring;
                        break;
                    }
                Paired[static_cast<size_t>(T)] = 1;
                Paired[static_cast<size_t>(Other)] = 1;
                Rings.push_back({ Ring, Group[T], false, T, Other });
            }
        }
        std::vector<int> Lone;           // lone triangles, ascending
        std::vector<int> LoneRing;       // the ring index of each
        for (int T = 0; T < TriangleCount; T++)
            if (!Paired[static_cast<size_t>(T)])
            {
                Paired[static_cast<size_t>(T)] = 1;
                Lone.push_back(T);
                LoneRing.push_back(static_cast<int>(Rings.size()));
                Rings.push_back({ { IdAt(T, 0), IdAt(T, 1), IdAt(T, 2), IdAt(T, 2) }, Group[T], true, T, T });
            }

        GroupIndex<2> CellsByEdge;
        {
            std::vector<GroupIndex<2>::Item> Items;
            for (size_t R = 0; R < Rings.size(); R++)
            {
                if (Rings[R].IsTriangle) continue;
                for (int K = 0; K < 4; K++) Items.push_back({ EdgeKey(Rings[R].V[K], Rings[R].V[(K + 1) % 4]), static_cast<int>(R) });
            }
            CellsByEdge.Build(std::move(Items), Seed);
        }

        // Triangles left without a partner that turn around one vertex are written as one fan mesh each instead of one 2x2 mesh each.
        std::vector<char> Fanned(Rings.size(), 0);
        std::vector<std::pair<int, QuadGrid>> FanAt;   // by the lowest ring index of its members
        std::vector<int> RingOfTriangle(static_cast<size_t>(TriangleCount), -1);
        for (size_t I = 0; I < Lone.size(); I++) RingOfTriangle[static_cast<size_t>(Lone[I])] = LoneRing[I];
        for (auto& [Pivot, Fan] : FindFans(Positions, Indices, Group, Lone, Options.IsBorder, Seed))
        {
            std::vector<int32_t> Rim = { RimOf(Indices, Fan.front(), Pivot).first };
            int Lowest = RingOfTriangle[static_cast<size_t>(Fan.front())];
            for (const int T : Fan)
            {
                Rim.push_back(RimOf(Indices, T, Pivot).second);
                Fanned[static_cast<size_t>(RingOfTriangle[static_cast<size_t>(T)])] = 1;
                Lowest = (std::min)(Lowest, RingOfTriangle[static_cast<size_t>(T)]);
            }
            QuadGrid Grid;
            Grid.GroupCount = 2;
            Grid.GroupSize = static_cast<int>(Rim.size());
            Grid.Points = Rim;
            Grid.Points.insert(Grid.Points.end(), Rim.size(), Pivot);
            Grid.Triangles = Fan;
            Grid.IsFan = true;
            FanAt.emplace_back(Lowest, std::move(Grid));
        }
        std::sort(FanAt.begin(), FanAt.end(), [](const auto& A, const auto& B) { return A.first < B.first; });

        // The strip step (BuildOptions.Strips): the cells of rectangles one cell wide or high, of fans and of lone triangles go to a pool (ring index, where it came from, which piece)
        // as well as being kept as the grid they would have been (Held, by piece); BuildStrips chains the pool afterwards and keeps the chains only where they make fewer grids than the
        // pieces did. Rectangles at least two cells wide and high stay as they are.
        const bool Strips = Options.Strips && Options.MaxStripPoints >= 3;
        std::vector<PoolCell> Pool;
        std::vector<BuiltGrid> Held;
        std::vector<int> FanOfRing(Rings.size(), -1), FanPiece(FanAt.size(), -1);
        for (size_t K = 0; K < FanAt.size(); K++)
            for (const int T : FanAt[K].second.Triangles) FanOfRing[static_cast<size_t>(RingOfTriangle[static_cast<size_t>(T)])] = static_cast<int>(K);

        // The region walk: lay cells out on integer coordinates from a seed cell, then cut the laid-out cells into rectangles.
        std::vector<char> Used(Rings.size(), 0);
        std::vector<int> CoordX(VertexCount, 0), CoordY(VertexCount, 0);
        std::vector<uint32_t> CoordStamp(VertexCount, 0);
        uint32_t RegionStamp = 0;
        FlatMap64 Occupied(Seed), CellAt(Seed);

        const auto Region = [&](int SeedCell, std::vector<BuiltGrid>& Out) {
            RegionStamp++;
            Occupied.Clear();
            CellAt.Clear();
            std::vector<std::pair<int, int>> Cells;
            std::vector<std::array<int, 3>> Queue;   // cell, x, y
            const auto CoordinateOf = [&](int32_t V, std::pair<int, int>& Have) {
                if (CoordStamp[static_cast<size_t>(V)] != RegionStamp) return false;
                Have = { CoordX[static_cast<size_t>(V)], CoordY[static_cast<size_t>(V)] };
                return true;
            };

            const auto Place = [&](int Cell, int X, int Y, const std::array<int32_t, 4>& Ring) {
                // A cell made of the same four vertices as one already laid out (the other side of a two-sided surface) would land on its lattice cell: the cut would
                // keep one of the two and lose the other's triangles, so it starts a region of its own. HydraX has no such guard and loses them.
                if (CellAt.Contains(PackXY(X, Y))) return false;
                for (int K = 0; K < 4; K++)
                {
                    const std::pair<int, int> Want = { X + Corners[K].first, Y + Corners[K].second };
                    std::pair<int, int> Have;
                    if (CoordinateOf(Ring[K], Have) ? Have != Want : Occupied.Contains(PackXY(Want.first, Want.second))) return false;
                }
                for (int K = 0; K < 4; K++)
                {
                    const int Wx = X + Corners[K].first, Wy = Y + Corners[K].second;
                    CoordStamp[static_cast<size_t>(Ring[K])] = RegionStamp;
                    CoordX[static_cast<size_t>(Ring[K])] = Wx;
                    CoordY[static_cast<size_t>(Ring[K])] = Wy;
                    Occupied.Set(PackXY(Wx, Wy), Ring[K]);
                }
                Cells.emplace_back(X, Y);
                CellAt.Set(PackXY(X, Y), Cell);
                // Later steps across this cell's sides read its ring: keep the corner order it was placed with.
                Rings[static_cast<size_t>(Cell)].V = Ring;
                Used[static_cast<size_t>(Cell)] = 1;
                Queue.push_back({ Cell, X, Y });
                return true;
            };

            const int SeedGroup = Rings[static_cast<size_t>(SeedCell)].Group;
            Place(SeedCell, 0, 0, Rings[static_cast<size_t>(SeedCell)].V);
            for (size_t Head = 0; Head < Queue.size(); Head++)
            {
                const auto [Cell, Cx, Cy] = Queue[Head];
                const std::array<int32_t, 4> Ring = Rings[static_cast<size_t>(Cell)].V;
                for (int K = 0; K < 4; K++)
                {
                    const auto Neighbourhood = CellsByEdge.Find(EdgeKey(Ring[K], Ring[(K + 1) % 4]));
                    // The step across edge k points from the cell centre through the edge midpoint.
                    const int StepX = Corners[K].first + Corners[(K + 1) % 4].first - 1;
                    const int StepY = Corners[K].second + Corners[(K + 1) % 4].second - 1;
                    for (const auto& Entry : Neighbourhood)
                    {
                        const int N = Entry.Id;
                        if (Used[static_cast<size_t>(N)] || Rings[static_cast<size_t>(N)].Group != SeedGroup) continue;

                        if (Vob)
                        {
                            if (const std::optional<double> Logit = PatchBorderModel::Logit(Positions, Ring, Rings[static_cast<size_t>(N)].V, Ring[K], Ring[(K + 1) % 4]))
                            {
                                Result.OrderScored++;
                                Result.OrderCut += *Logit > PatchBorderModel::CutLogit ? 1 : 0;
                                if (*Logit > PatchBorderModel::CutLogit) continue;
                            }
                        }

                        if (Options.IsBorder
                            && Options.IsBorder(TriangleOnEdge(Rings[static_cast<size_t>(Cell)], Indices, Ring[K], Ring[(K + 1) % 4]), TriangleOnEdge(Rings[static_cast<size_t>(N)], Indices, Ring[K], Ring[(K + 1) % 4]), Ring[K], Ring[(K + 1) % 4]))
                            continue;

                        std::array<int32_t, 4> Ordered;
                        if (OrderForCell(CoordinateOf, Rings[static_cast<size_t>(N)].V, Cx + StepX, Cy + StepY, Ordered)) Place(N, Cx + StepX, Cy + StepY, Ordered);
                    }
                }
            }

            // Cut the laid-out cells into rectangles: fewer, larger pieces are closer to a patch the mapper drew than the row-by-row cut was.
            for (const MeshInternal::CellRect& R : MeshInternal::CutRectangles(Cells))
            {
                const bool Small = Strips && (R.W < 2 || R.H < 2);
                QuadGrid Grid;
                Grid.GroupCount = R.W + 1;
                Grid.GroupSize = R.H + 1;
                Grid.Points.reserve(static_cast<size_t>(Grid.GroupCount) * Grid.GroupSize);
                for (int Gx = 0; Gx <= R.W; Gx++)
                    for (int Gy = 0; Gy <= R.H; Gy++)
                    {
                        int Vertex = -1;
                        // every corner of a laid-out rectangle was placed; a miss would be a bug here, and a -1 must never become a vertex index
                        if (!Occupied.Find(PackXY(R.X + Gx, R.Y + Gy), Vertex) || Vertex < 0) throw std::logic_error("QuadMesher.Build: a rectangle corner has no vertex");
                        Grid.Points.push_back(Vertex);
                    }
                for (int I = 0; I < R.W; I++)
                    for (int J = 0; J < R.H; J++)
                    {
                        int Which = -1;
                        if (!CellAt.Find(PackXY(R.X + I, R.Y + J), Which) || Which < 0) throw std::logic_error("QuadMesher.Build: a rectangle cell has no triangles");
                        const RingCell& Ring = Rings[static_cast<size_t>(Which)];
                        Grid.Triangles.push_back(Ring.First);
                        if (Ring.Second != Ring.First) Grid.Triangles.push_back(Ring.Second);
                        if (Small) Pool.push_back({ Which, 0, static_cast<int>(Held.size()) });
                    }
                if (Small)
                    Held.push_back({ std::move(Grid), SeedGroup });
                else
                    Out.push_back({ std::move(Grid), SeedGroup });
            }
        };

        size_t NextFan = 0;
        for (size_t SeedCell = 0; SeedCell < Rings.size(); SeedCell++)
        {
            if (Used[SeedCell]) continue;
            if (Rings[SeedCell].IsTriangle)
            {
                Used[SeedCell] = 1;
                if (Strips)
                {
                    const int First = Rings[SeedCell].First;
                    const int A = IdAt(First, 0), B = IdAt(First, 1), C = IdAt(First, 2);
                    if (A != B && B != C && A != C)
                    {
                        if (Fanned[SeedCell])
                        {
                            // the fan's first member (lowest ring index) keeps the fan grid; the others point at it
                            const size_t Fan = static_cast<size_t>(FanOfRing[SeedCell]);
                            if (FanPiece[Fan] < 0)
                            {
                                FanPiece[Fan] = static_cast<int>(Held.size());
                                Held.push_back({ std::move(FanAt[Fan].second), Rings[SeedCell].Group });
                            }
                            Pool.push_back({ static_cast<int>(SeedCell), 2, FanPiece[Fan] });
                        }
                        else
                        {
                            const std::array<int32_t, 4>& V = Rings[SeedCell].V;
                            QuadGrid Lone = QuadGrid::FromGroups({ { V[0], V[1] }, { V[2], V[2] } }, true);
                            Lone.Triangles = { Rings[SeedCell].First };
                            Pool.push_back({ static_cast<int>(SeedCell), 1, static_cast<int>(Held.size()) });
                            Held.push_back({ std::move(Lone), Rings[SeedCell].Group });
                        }
                        continue;
                    }
                }
                if (Fanned[SeedCell])
                {
                    if (NextFan < FanAt.size() && FanAt[NextFan].first == static_cast<int>(SeedCell)) Result.Grids.push_back({ std::move(FanAt[NextFan++].second), Rings[SeedCell].Group });
                    continue;
                }
                const std::array<int32_t, 4>& R = Rings[SeedCell].V;
                QuadGrid Grid = QuadGrid::FromGroups({ { R[0], R[1] }, { R[2], R[2] } }, true);
                Grid.Triangles = { Rings[SeedCell].First };
                Result.Grids.push_back({ std::move(Grid), Rings[SeedCell].Group });
                continue;
            }
            Region(static_cast<int>(SeedCell), Result.Grids);
        }
        if (Strips)
        {
            Result.GridsBeforeStrips = static_cast<int64_t>(Result.Grids.size() + Held.size());
            BuildStrips(Positions, Indices, Rings, Pool, Held, Options, Seed, Result);
        }
        return Result;
    }
}

// ---- vertex weld

std::string NearestDistances::ToText() const
{
    std::string Text = Int(Vertices) + " used vertices, " + Int(SharedPosition) + " share an exact position with another vertex id; nearest vertex at another position: ";
    for (size_t I = 0; I < Edges.size(); I++) Text += "<= " + Fixed(Edges[I], 7) + ": " + Int(Counts[I]) + ", ";
    Text += "> " + Fixed(Edges.back(), 7) + " (searched to " + Fixed(Maximum, 3) + "): " + Int(Counts.back());
    return Text;
}

namespace VertexWeld
{
    namespace
    {
        constexpr std::array<double, 13> BucketEdges = { 0.0001, 0.001, 0.005, 0.01, 0.05, 0.1, 0.25, 0.5, 1, 2, 4, 8, 16 };
        constexpr double NoiseLimit = 0.0001;
        constexpr double SpacingLimit = 0.1;
        constexpr double WeldTolerance = 0.01;
    }

    NearestDistances Measure(std::span<const float> Positions, std::span<const int32_t> Indices)
    {
        // WHY a k-d tree over distinct positions: HydraX searches the 27 cells of a 16 unit grid around each vertex, which on a dense map is
        // thousands of distance tests per vertex. The answer only needs the nearest distinct position (bucketed) and whether another id
        // shares the position, so vertices at one position are grouped, and each group asks the tree once.
        const std::vector<int32_t> Used = UsedVertices(Indices, Positions.size() / 3);
        const std::vector<std::vector<int32_t>> Groups = GroupByPosition(Positions, Used);
        std::vector<int32_t> Representatives;
        Representatives.reserve(Groups.size());
        for (const auto& G : Groups) Representatives.push_back(G.front());
        const KdTree Tree(Positions, Representatives);
        const double Maximum = BucketEdges.back();

        NearestDistances Result;
        Result.Vertices = static_cast<int>(Used.size());
        Result.Edges.assign(BucketEdges.begin(), BucketEdges.end());
        Result.Counts.assign(BucketEdges.size() + 1, 0);
        Result.Maximum = Maximum;
        size_t Finite = 0;
        for (const auto& G : Groups)
        {
            Finite += G.size();
            const double Best = Tree.Nearest(Point(Positions, G.front()), G.front(), Maximum);
            size_t Bucket = BucketEdges.size();
            for (size_t I = 0; I < BucketEdges.size(); I++)
                if (Best <= BucketEdges[I])
                {
                    Bucket = I;
                    break;
                }
            Result.Counts[Bucket] += static_cast<int>(G.size());
            if (G.size() > 1) Result.SharedPosition += static_cast<int>(G.size());
        }
        // a used vertex with a non-finite position has no neighbour
        Result.Counts.back() += static_cast<int>(Used.size() - Finite);
        return Result;
    }

    ToleranceChoice ChooseTolerance(const NearestDistances& Distances)
    {
        // Vertices at the same position or within float noise (at most 0.0001) are one vertex written twice. Real vertices of this data are no closer than a
        // tenth of a unit. If nothing lies between, any tolerance in between welds the same vertices; 0.01 is picked (100 times the noise, a tenth of the first
        // real spacing). If something does lie between, the data does not separate noise from spacing and only identical positions are welded.
        int Between = 0;
        for (size_t I = 0; I < Distances.Edges.size(); I++)
            if (Distances.Edges[I] > NoiseLimit && Distances.Edges[I] <= SpacingLimit) Between += Distances.Counts[I];
        const int Noise = Distances.Counts[0];
        if (Between == 0)
            return { WeldTolerance, "nearest-vertex distances: " + Int(Noise) + " vertices within " + Shortest(NoiseLimit) + " of another position, " + Int(Distances.SharedPosition) + " at an identical position, none between "
                                        + Shortest(NoiseLimit) + " and " + Shortest(SpacingLimit) + ": tolerance " + Shortest(WeldTolerance) + " (the same set is welded by any value in the gap)" };
        return { 0, "nearest-vertex distances: " + Int(Between) + " vertices between " + Shortest(NoiseLimit) + " and " + Shortest(SpacingLimit) + ", so noise and real spacing are not separated: only identical positions are welded" };
    }

    WeldResult Weld(std::span<const float> Positions, std::span<const int32_t> Indices, double Tolerance)
    {
        const size_t Count = Positions.size() / 3;
        std::vector<int32_t> Parent(Count);
        std::iota(Parent.begin(), Parent.end(), 0);
        const auto Find = [&](int32_t V) {
            while (Parent[static_cast<size_t>(V)] != V)
            {
                Parent[static_cast<size_t>(V)] = Parent[static_cast<size_t>(Parent[static_cast<size_t>(V)])];
                V = Parent[static_cast<size_t>(V)];
            }
            return V;
        };
        const auto Union = [&](int32_t A, int32_t B) {
            const int32_t Ra = Find(A), Rb = Find(B);
            if (Ra != Rb) Parent[static_cast<size_t>((std::max)(Ra, Rb))] = (std::min)(Ra, Rb);
        };

        const std::vector<int32_t> Used = UsedVertices(Indices, Count);
        const std::vector<std::vector<int32_t>> Groups = GroupByPosition(Positions, Used);
        std::vector<int32_t> Representatives;
        Representatives.reserve(Groups.size());
        for (const auto& G : Groups)
        {
            Representatives.push_back(G.front());
            // the same position is within any tolerance of 0 or more; a negative or NaN tolerance welds nothing, not even these (HydraX tests distance <= tolerance)
            if (Tolerance >= 0)
                for (size_t I = 1; I < G.size(); I++) Union(G.front(), G[I]);
        }
        const KdTree Tree(Positions, Representatives);
        for (const int32_t V : Representatives)
            Tree.ForEachWithin(Point(Positions, V), Tolerance, [&](int32_t O) {
                if (O > V) Union(V, O);
            });

        WeldResult Result;
        Result.Representative.resize(Count);
        for (size_t V = 0; V < Count; V++) Result.Representative[V] = Find(static_cast<int32_t>(V));

        std::vector<std::pair<int32_t, int32_t>> ByRoot;   // root, member
        ByRoot.reserve(Used.size());
        for (const int32_t V : Used) ByRoot.emplace_back(Result.Representative[static_cast<size_t>(V)], V);
        std::sort(ByRoot.begin(), ByRoot.end());
        for (size_t I = 0; I < ByRoot.size();)
        {
            size_t End = I;
            while (End < ByRoot.size() && ByRoot[End].first == ByRoot[I].first) End++;
            const size_t Size = End - I;
            if (Size > 1)
            {
                Result.Clusters++;
                Result.Welded += static_cast<int>(Size - 1);
                // the span is measured for clusters of 64 or fewer (it is quadratic)
                if (Size <= 64)
                    for (size_t A = I; A < End; A++)
                        for (size_t B = A + 1; B < End; B++)
                            Result.LargestSpan = (std::max)(Result.LargestSpan, Length(Sub(Point(Positions, ByRoot[A].second), Point(Positions, ByRoot[B].second))));
            }
            I = End;
        }
        return Result;
    }
}

// ---- exact copies and overlaps

namespace TriangleOverlap
{
    namespace
    {
        constexpr double CellSize = 64;

        // Math.Min and Math.Max of .NET: a NaN wins
        float MinNan(float A, float B) { return (A != A || B != B) ? std::nanf("") : (std::min)(A, B); }   // x != x is NaN; std::isnan is a call here, and this is a hot loop
        float MaxNan(float A, float B) { return (A != A || B != B) ? std::nanf("") : (std::max)(A, B); }
        constexpr int CellLimit = 1 << 20;            // cell coordinates are clamped to +-this so a cell packs into 63 bits
        constexpr int64_t MaxCellsPerTriangle = 1024; // a triangle reaching more cells is tested against everything instead of being put in a grid

        int CellOf(double Coordinate)
        {
            if (Coordinate != Coordinate) return 0;   // NaN; .NET's float to int cast gives 0
            const double F = std::floor(Coordinate / CellSize);
            return static_cast<int>(std::clamp(F, -static_cast<double>(CellLimit - 1), static_cast<double>(CellLimit - 1)));
        }

        std::array<int32_t, 3> Bits(std::span<const float> Positions, int V)
        {
            std::array<int32_t, 3> B{};
            for (int K = 0; K < 3; K++) std::memcpy(&B[K], &Positions[static_cast<size_t>(V) * 3 + K], sizeof(int32_t));
            return B;
        }

        struct Box
        {
            float Low[3], High[3];
            V3 Normal;
            double Area;
        };

        double Side(const std::pair<double, double>& A, const std::pair<double, double>& B, const std::pair<double, double>& P)
        {
            return ((B.first - A.first) * (P.second - A.second)) - ((B.second - A.second) * (P.first - A.first));
        }

        double Shoelace(const std::vector<std::pair<double, double>>& Polygon)
        {
            double Sum = 0;
            for (size_t I = 0; I < Polygon.size(); I++)
            {
                const auto& P = Polygon[I];
                const auto& Q = Polygon[(I + 1) % Polygon.size()];
                Sum += (P.first * Q.second) - (Q.first * P.second);
            }
            return Sum / 2;
        }

        std::vector<std::pair<double, double>> Ccw(std::vector<std::pair<double, double>> Polygon)
        {
            if (Shoelace(Polygon) < 0) std::reverse(Polygon.begin(), Polygon.end());
            return Polygon;
        }

        // Area of the intersection of two coplanar triangles: project both into the plane of the first and clip one against the other (Sutherland-Hodgman, both made counter-clockwise)
        double OverlapArea(const V3& A0, const V3& A1, const V3& A2, const V3& B0, const V3& B1, const V3& B2, const V3& Normal)
        {
            const V3 Axis = std::fabs(Normal[0]) < 0.9 ? V3{ 1, 0, 0 } : V3{ 0, 1, 0 };
            V3 U = Cross(Normal, Axis);
            const double UL = Length(U);
            U = { U[0] / UL, U[1] / UL, U[2] / UL };
            const V3 Vv = Cross(Normal, U);
            const auto Project = [&](const V3& P) { return std::make_pair(Dot(P, U), Dot(P, Vv)); };
            std::vector<std::pair<double, double>> Output = Ccw({ Project(B0), Project(B1), Project(B2) });
            const std::vector<std::pair<double, double>> Clip = Ccw({ Project(A0), Project(A1), Project(A2) });
            for (size_t I = 0; I < Clip.size() && !Output.empty(); I++)
            {
                const auto& From = Clip[I];
                const auto& To = Clip[(I + 1) % Clip.size()];
                const std::vector<std::pair<double, double>> Input = std::move(Output);
                Output.clear();
                for (size_t K = 0; K < Input.size(); K++)
                {
                    const auto& Current = Input[K];
                    const auto& Previous = Input[(K + Input.size() - 1) % Input.size()];
                    const bool CurrentInside = Side(From, To, Current) >= 0;
                    const bool PreviousInside = Side(From, To, Previous) >= 0;
                    if (CurrentInside != PreviousInside)
                    {
                        const double D1 = Side(From, To, Previous), D2 = Side(From, To, Current);
                        const double F = D1 / (D1 - D2);
                        Output.emplace_back(Previous.first + ((Current.first - Previous.first) * F), Previous.second + ((Current.second - Previous.second) * F));
                    }
                    if (CurrentInside) Output.push_back(Current);
                }
            }
            return std::fabs(Shoelace(Output));
        }
    }

    OverlapResult Find(std::span<const float> Positions, std::span<const int32_t> Indices, std::span<const int32_t> Labels, bool FindPartial)
    {
        const int Count = static_cast<int>(Indices.size() / 3);
        CheckInput(Positions, Indices.first(static_cast<size_t>(Count) * 3), "TriangleOverlap.Find");
        if (Labels.size() < static_cast<size_t>(Count)) throw std::invalid_argument("TriangleOverlap.Find: fewer labels than triangles");
        OverlapResult Result;

        // Exact copies: the corner positions as float bits, sorted within a triangle, sorted across triangles; the first of equal keys is the one that stays.
        struct Keyed
        {
            std::array<int32_t, 9> K;
            int32_t T;
        };
        std::vector<Keyed> Keys;
        Keys.reserve(static_cast<size_t>(Count));
        const auto IdAt = [&](int T, int K) { return Indices[static_cast<size_t>(T) * 3 + static_cast<size_t>(K)]; };
        for (int T = 0; T < Count; T++)
        {
            // Compare positions by their float bits, so different vertex ids with identical positions are found too.
            std::array<std::array<int32_t, 3>, 3> Corner = { Bits(Positions, IdAt(T, 0)), Bits(Positions, IdAt(T, 1)), Bits(Positions, IdAt(T, 2)) };
            if (Corner[0] == Corner[1] || Corner[1] == Corner[2] || Corner[0] == Corner[2]) continue;
            std::sort(Corner.begin(), Corner.end());
            Keyed Item{};
            for (int C = 0; C < 3; C++)
                for (int K = 0; K < 3; K++) Item.K[static_cast<size_t>(C * 3 + K)] = Corner[static_cast<size_t>(C)][static_cast<size_t>(K)];
            Item.T = T;
            Keys.push_back(Item);
        }
        std::sort(Keys.begin(), Keys.end(), [](const Keyed& A, const Keyed& B) { return A.K != B.K ? A.K < B.K : A.T < B.T; });
        for (size_t I = 0; I < Keys.size();)
        {
            size_t End = I + 1;
            while (End < Keys.size() && Keys[End].K == Keys[I].K) End++;
            const int Kept = Keys[I].T;
            const auto A = std::array{ Bits(Positions, IdAt(Kept, 0)), Bits(Positions, IdAt(Kept, 1)), Bits(Positions, IdAt(Kept, 2)) };
            for (size_t J = I + 1; J < End; J++)
            {
                const int T = Keys[J].T;
                const auto B = std::array{ Bits(Positions, IdAt(T, 0)), Bits(Positions, IdAt(T, 1)), Bits(Positions, IdAt(T, 2)) };
                const bool Same = B == A || B == std::array{ A[1], A[2], A[0] } || B == std::array{ A[2], A[0], A[1] };
                Result.Duplicates.push_back({ Kept, T, Same, Labels[static_cast<size_t>(T)] == Labels[static_cast<size_t>(Kept)] });
            }
            I = End;
        }
        Keys.clear();
        Keys.shrink_to_fit();
        std::sort(Result.Duplicates.begin(), Result.Duplicates.end(), [](const TriangleDuplicate& A, const TriangleDuplicate& B) { return A.Removed < B.Removed; });
        if (!FindPartial) return Result;

        // Partial overlaps. A pair is tested when the bounding boxes of the two triangles meet; the cell grid only finds those pairs without trying all of them,
        // so the result is the same whatever the cell size. A triangle that reaches more than MaxCellsPerTriangle cells is tested against all the others instead.
        std::vector<char> Skip(static_cast<size_t>(Count), 0);
        for (const TriangleDuplicate& D : Result.Duplicates) Skip[static_cast<size_t>(D.Removed)] = 1;
        std::vector<Box> Boxes(static_cast<size_t>(Count));
        std::vector<char> Active(static_cast<size_t>(Count), 0), Big(static_cast<size_t>(Count), 0), Odd(static_cast<size_t>(Count), 0);
        struct Entry
        {
            uint64_t Cell;
            int32_t T;
        };
        std::vector<Entry> Entries;
        const auto Pack = [](int X, int Y, int Z) {
            return (static_cast<uint64_t>(X + CellLimit) << 42) | (static_cast<uint64_t>(Y + CellLimit) << 21) | static_cast<uint64_t>(Z + CellLimit);
        };
        std::vector<int> BigList;
        for (int T = 0; T < Count; T++)
        {
            if (Skip[static_cast<size_t>(T)]) continue;
            const V3 A = Point(Positions, IdAt(T, 0)), B = Point(Positions, IdAt(T, 1)), C = Point(Positions, IdAt(T, 2));
            const V3 N = Cross(Sub(B, A), Sub(C, A));
            const double Len = Length(N);
            Box& Bx = Boxes[static_cast<size_t>(T)];
            Bx.Area = Len / 2;
            if (Bx.Area < MinimumArea) continue;
            Bx.Normal = { N[0] / Len, N[1] / Len, N[2] / Len };
            for (int K = 0; K < 3; K++)
            {
                const float Fa = static_cast<float>(A[static_cast<size_t>(K)]), Fb = static_cast<float>(B[static_cast<size_t>(K)]), Fc = static_cast<float>(C[static_cast<size_t>(K)]);
                Bx.Low[K] = MinNan(Fa, MinNan(Fb, Fc));
                Bx.High[K] = MaxNan(Fa, MaxNan(Fb, Fc));
            }
            Active[static_cast<size_t>(T)] = 1;
            Odd[static_cast<size_t>(T)] = std::isnan(Bx.Low[0]) || std::isnan(Bx.Low[1]) || std::isnan(Bx.Low[2]);
            const int X0 = CellOf(Bx.Low[0]), X1 = CellOf(Bx.High[0]), Y0 = CellOf(Bx.Low[1]), Y1 = CellOf(Bx.High[1]), Z0 = CellOf(Bx.Low[2]), Z1 = CellOf(Bx.High[2]);
            const int64_t Cells = static_cast<int64_t>(X1 - X0 + 1) * (Y1 - Y0 + 1) * (Z1 - Z0 + 1);
            if (Cells > MaxCellsPerTriangle)
            {
                Big[static_cast<size_t>(T)] = 1;
                BigList.push_back(T);
                continue;
            }
            for (int X = X0; X <= X1; X++)
                for (int Y = Y0; Y <= Y1; Y++)
                    for (int Z = Z0; Z <= Z1; Z++) Entries.push_back({ Pack(X, Y, Z), T });
        }
        std::sort(Entries.begin(), Entries.end(), [](const Entry& A, const Entry& B) { return A.Cell != B.Cell ? A.Cell < B.Cell : A.T < B.T; });

        int Tested = 0;
        const auto TestPair = [&](int S, int T) {
            const Box &Bs = Boxes[static_cast<size_t>(S)], &Bt = Boxes[static_cast<size_t>(T)];
            if (Bs.High[0] < Bt.Low[0] || Bt.High[0] < Bs.Low[0] || Bs.High[1] < Bt.Low[1] || Bt.High[1] < Bs.Low[1] || Bs.High[2] < Bt.Low[2] || Bt.High[2] < Bs.Low[2]) return;
            if (std::fabs(Dot(Bs.Normal, Bt.Normal)) < NormalDot) return;
            const V3 Origin = Point(Positions, IdAt(S, 0));
            for (int K = 0; K < 3; K++)
                if (std::fabs(Dot(Bs.Normal, Sub(Point(Positions, IdAt(T, K)), Origin))) > PlaneDistance) return;
            Tested++;
            const double Overlap = OverlapArea(Point(Positions, IdAt(S, 0)), Point(Positions, IdAt(S, 1)), Point(Positions, IdAt(S, 2)), Point(Positions, IdAt(T, 0)), Point(Positions, IdAt(T, 1)), Point(Positions, IdAt(T, 2)), Bs.Normal);
            if (Overlap >= MinimumArea)
            {
                TrianglePartialOverlap O;
                O.First = S;
                O.Second = T;
                O.Area = Overlap;
                O.FractionOfSmaller = Overlap / (std::min)(Bs.Area, Bt.Area);
                O.SameLabel = Labels[static_cast<size_t>(S)] == Labels[static_cast<size_t>(T)];
                O.SameSide = Dot(Bs.Normal, Bt.Normal) > 0;
                O.Smaller = Bs.Area < Bt.Area ? S : Bt.Area < Bs.Area ? T : (std::max)(S, T);
                Result.PartialOverlaps.push_back(O);
            }
        };

        for (size_t I = 0; I < Entries.size();)
        {
            size_t End = I + 1;
            while (End < Entries.size() && Entries[End].Cell == Entries[I].Cell) End++;
            const uint64_t Cell = Entries[I].Cell;
            const int Cx = static_cast<int>(Cell >> 42) - CellLimit, Cy = static_cast<int>((Cell >> 21) & 0x1FFFFF) - CellLimit, Cz = static_cast<int>(Cell & 0x1FFFFF) - CellLimit;
            for (size_t A = I; A < End; A++)
                for (size_t B = A + 1; B < End; B++)
                {
                    const int S = Entries[A].T, T = Entries[B].T;   // ascending
                    // A pair meets in every cell both boxes reach; it is handled once, in the cell that holds the lower corner of the overlap of the two boxes.
                    if (CellOf(MaxNan(Boxes[static_cast<size_t>(S)].Low[0], Boxes[static_cast<size_t>(T)].Low[0])) != Cx || CellOf(MaxNan(Boxes[static_cast<size_t>(S)].Low[1], Boxes[static_cast<size_t>(T)].Low[1])) != Cy
                        || CellOf(MaxNan(Boxes[static_cast<size_t>(S)].Low[2], Boxes[static_cast<size_t>(T)].Low[2])) != Cz)
                        continue;
                    TestPair(S, T);
                }
            I = End;
        }
        for (const int B : BigList)
            for (int T = 0; T < Count; T++)
            {
                if (T == B || !Active[static_cast<size_t>(T)] || (Big[static_cast<size_t>(T)] && T < B)) continue;
                // a triangle with a NaN coordinate sits in cell 0 only (HydraX meets it only where a box reaches that cell)
                if (Odd[static_cast<size_t>(T)] && !(CellOf(Boxes[static_cast<size_t>(B)].Low[0]) <= 0 && CellOf(Boxes[static_cast<size_t>(B)].High[0]) >= 0 && CellOf(Boxes[static_cast<size_t>(B)].Low[1]) <= 0 && CellOf(Boxes[static_cast<size_t>(B)].High[1]) >= 0
                                                   && CellOf(Boxes[static_cast<size_t>(B)].Low[2]) <= 0 && CellOf(Boxes[static_cast<size_t>(B)].High[2]) >= 0))
                    continue;
                TestPair((std::min)(B, T), (std::max)(B, T));
            }

        std::sort(Result.PartialOverlaps.begin(), Result.PartialOverlaps.end(), [](const TrianglePartialOverlap& X, const TrianglePartialOverlap& Y) { return X.First != Y.First ? X.First < Y.First : X.Second < Y.Second; });
        Result.CoplanarPairsTested = Tested;
        return Result;
    }
}

std::string OverlapResult::Summary(const std::string& Label, bool Applied) const
{
    int Deletable = 0, BackToBack = 0, OtherLabel = 0;
    for (const TriangleDuplicate& D : Duplicates)
    {
        Deletable += D.IsDeletable() ? 1 : 0;
        BackToBack += !D.SameWinding ? 1 : 0;
        OtherLabel += (D.SameWinding && !D.SameLabel) ? 1 : 0;
    }
    double Area = 0;
    int SameLabel = 0, Contained = 0, ContainedSame = 0, Back = 0;
    for (const TrianglePartialOverlap& O : PartialOverlaps)
    {
        Area += O.Area;
        SameLabel += O.SameLabel ? 1 : 0;
        Contained += O.IsContained() ? 1 : 0;
        ContainedSame += (O.IsContained() && O.SameLabel && O.SameSide) ? 1 : 0;
        Back += !O.SameSide ? 1 : 0;
    }
    return "Duplicates and overlaps in the " + Label + ": " + Int(static_cast<int64_t>(Duplicates.size())) + " exact copies (identical corner positions): " + Int(Deletable) + (Applied ? " deleted" : " deletable but NOT deleted")
           + " (same winding, same label), " + Int(BackToBack) + " back-to-back (opposite winding) kept, " + Int(OtherLabel) + " with another label kept; " + Int(static_cast<int64_t>(PartialOverlaps.size()))
           + " partly overlapping coplanar pairs (of " + Int(CoplanarPairsTested) + " coplanar candidates; area " + Fixed(Area, 3) + ", " + Int(SameLabel) + " with the same label; " + Int(Contained)
           + " with the smaller triangle entirely inside the larger, of which " + Int(ContainedSame) + " same label and same side; " + Int(Back) + " back-to-back) counted and listed, " + Int(static_cast<int64_t>(ContainedRemovals().size()))
           + (Applied ? " of them removed as contained (proof listed in the removed lines), the rest only counted and listed." : " of them would be removable as contained; nothing was removed.");
}

std::string OverlapResult::ToText(size_t Limit) const
{
    std::string Text = "exact copies: kind, kept triangle, copy, winding, label\n";
    for (size_t I = 0; I < Duplicates.size() && I < Limit; I++)
    {
        const TriangleDuplicate& D = Duplicates[I];
        Text += std::string(D.IsDeletable() ? "deleted" : "kept") + "\t" + Int(D.Kept) + "\t" + Int(D.Removed) + "\t" + (D.SameWinding ? "same winding" : "opposite winding") + "\t" + (D.SameLabel ? "same label" : "other label") + "\n";
    }
    Text += "partial overlaps (only the contained ones named in the removed lines are removed): triangle, triangle, overlap area, fraction of the smaller, label\n";
    for (size_t I = 0; I < PartialOverlaps.size() && I < Limit; I++)
    {
        const TrianglePartialOverlap& O = PartialOverlaps[I];
        Text += "overlap\t" + Int(O.First) + "\t" + Int(O.Second) + "\t" + Fixed(O.Area, 3) + "\t" + Fixed(O.FractionOfSmaller, 3) + "\t" + (O.SameLabel ? "same label" : "other label") + "\t" + (O.SameSide ? "same side" : "back-to-back")
                + (O.IsContained() ? "\tcontained" : "") + "\n";
    }
    if (PartialOverlaps.size() > Limit) Text += "... and " + Int(static_cast<int64_t>(PartialOverlaps.size() - Limit)) + " more overlaps\n";
    return Text;
}

std::vector<ContainedRemoval> OverlapResult::ContainedRemovals() const
{
    std::vector<const TrianglePartialOverlap*> Contained;
    int MaxIndex = -1;
    for (const TrianglePartialOverlap& O : PartialOverlaps)
        if (O.IsContained() && O.SameLabel && O.SameSide && O.Smaller >= 0)
        {
            Contained.push_back(&O);
            MaxIndex = (std::max)(MaxIndex, (std::max)(O.First, O.Second));
        }
    const auto OtherOf = [](const TrianglePartialOverlap& O) { return O.First == O.Smaller ? O.Second : O.First; };

    // The smaller triangles, in the order they first appear: the order the fixpoint below walks them in (HydraX's set enumerates in insertion order, and the
    // result of one pass depends on it, so it is kept).
    std::vector<char> InRemove(static_cast<size_t>(MaxIndex + 1), 0);
    std::vector<int> Order;
    for (const TrianglePartialOverlap* O : Contained)
        if (!InRemove[static_cast<size_t>(O->Smaller)])
        {
            InRemove[static_cast<size_t>(O->Smaller)] = 1;
            Order.push_back(O->Smaller);
        }
    std::vector<std::pair<int, int>> BySmaller;   // smaller triangle, container
    BySmaller.reserve(Contained.size());
    for (const TrianglePartialOverlap* O : Contained) BySmaller.emplace_back(O->Smaller, OtherOf(*O));
    std::sort(BySmaller.begin(), BySmaller.end());
    const auto Containers = [&](int R) {
        return std::equal_range(BySmaller.begin(), BySmaller.end(), std::make_pair(R, std::numeric_limits<int>::min()), [](const auto& A, const auto& B) { return A.first < B.first; });
    };

    // A triangle stays in the removal set only while it has a container outside the set.
    bool Changed;
    do
    {
        Changed = false;
        for (const int R : Order)
        {
            if (!InRemove[static_cast<size_t>(R)]) continue;
            const auto [Lo, Hi] = Containers(R);
            bool Free = false;
            for (auto It = Lo; It != Hi && !Free; ++It) Free = !InRemove[static_cast<size_t>(It->second)];
            if (!Free)
            {
                InRemove[static_cast<size_t>(R)] = 0;
                Changed = true;
            }
        }
    } while (Changed);

    std::vector<std::pair<int, size_t>> Picked;   // smaller triangle, position in Contained
    for (size_t I = 0; I < Contained.size(); I++)
    {
        const TrianglePartialOverlap& O = *Contained[I];
        if (InRemove[static_cast<size_t>(O.Smaller)] && !InRemove[static_cast<size_t>(OtherOf(O))]) Picked.emplace_back(O.Smaller, I);
    }
    std::stable_sort(Picked.begin(), Picked.end(), [](const auto& A, const auto& B) { return A.first < B.first; });
    std::vector<ContainedRemoval> Result;
    for (const auto& [Smaller, At] : Picked)
        if (Result.empty() || Result.back().Triangle != Smaller) Result.push_back({ Smaller, *Contained[At] });
    return Result;
}

ProofList OverlapResult::Proofs(std::span<const float> Positions, std::span<const int32_t> Indices) const
{
    ProofList Proofs;
    const auto P = [&](int V) {
        const size_t B = static_cast<size_t>(V) * 3;
        return "(" + Shortest(Positions[B]) + " " + Shortest(Positions[B + 1]) + " " + Shortest(Positions[B + 2]) + ")";
    };
    for (const TriangleDuplicate& D : Duplicates)
    {
        if (!D.IsDeletable()) continue;
        const size_t B = static_cast<size_t>(D.Removed) * 3;
        Proofs.emplace_back(D.Removed, "exact duplicate of triangle " + Int(D.Kept) + ": identical corner positions " + P(Indices[B]) + " " + P(Indices[B + 1]) + " " + P(Indices[B + 2])
                                           + ", same winding, same label; the kept triangle covers the same area");
    }
    for (const ContainedRemoval& R : ContainedRemovals())
    {
        const TrianglePartialOverlap& O = R.Overlap;
        const int Container = O.First == R.Triangle ? O.Second : O.First;
        Proofs.emplace_back(R.Triangle, "contained in triangle " + Int(Container) + ": " + Fixed(O.Area, 3) + " square units of its own area lie inside that triangle (fraction " + Fixed(O.FractionOfSmaller, 4)
                                            + ", coplanar within " + Shortest(TriangleOverlap::PlaneDistance) + " units), same label, same facing; the larger triangle covers the same surface");
    }
    // a copy's proof comes first and a triangle has one proof
    std::stable_sort(Proofs.begin(), Proofs.end(), [](const auto& A, const auto& B) { return A.first < B.first; });
    Proofs.erase(std::unique(Proofs.begin(), Proofs.end(), [](const auto& A, const auto& B) { return A.first == B.first; }), Proofs.end());
    return Proofs;
}

// ---- coverage

std::string CoverageResult::Summary(const std::string& Label) const
{
    return "Coverage of the " + Label + ": " + Int(InputTriangles) + " input triangles (" + Int(ZeroAreaInput) + " with zero area) -> " + Int(Meshes) + " meshes (any size), " + Int(LoneTriangles) + " lone triangles, " + Int(Cells)
           + " cells; covered " + Int(Covered) + ", removed on purpose " + Int(Removed) + ", MISSING " + Int(Missing) + ", LEFTOVER " + Int(Leftover) + "; area in " + Fixed(InputArea, 3) + " (" + Fixed(RemovedArea, 3)
           + " removed on purpose), area out " + Fixed(OutputArea, 3) + " (cut along the other diagonal " + Fixed(OtherDiagonalArea, 3) + ").";
}

std::string CoverageResult::ToText(const std::string& Label) const
{
    std::string Text = Summary(Label) + "\n";
    Text += IsComplete() ? "Nothing is missing and nothing is left over.\n" : "Every missing or leftover item is listed below, one line each: kind, input triangle (-1 = an output cell with no input triangle), reason.\n";
    for (const CoverageLine& L : Lines) Text += L.Kind + "\t" + Int(L.Triangle) + "\t" + L.Reason + "\n";
    return Text;
}

namespace PatchCoverage
{
    namespace
    {
        constexpr double ZeroArea = 1e-9;

        double Area(std::span<const float> Positions, int A, int B, int C)
        {
            if (A == B || B == C || A == C) return 0;
            const V3 Pa = Point(Positions, A);
            return Length(Cross(Sub(Point(Positions, B), Pa), Sub(Point(Positions, C), Pa))) / 2;
        }

        std::array<int32_t, 3> SortedKey(int A, int B, int C)
        {
            std::array<int32_t, 3> K = { A, B, C };
            std::sort(K.begin(), K.end());
            return K;
        }
    }

    CoverageResult Check(std::span<const float> Positions, std::span<const int32_t> Indices, std::span<const QuadGrid> Grids, const ProofList& Removed, std::span<const int32_t> Representative)
    {
        const int TriangleCount = static_cast<int>(Indices.size() / 3);
        CheckInput(Positions, Indices.first(static_cast<size_t>(TriangleCount) * 3), "PatchCoverage.Check");
        const size_t VertexTotal = Positions.size() / 3;
        if (!Representative.empty() && Representative.size() < VertexTotal) throw std::invalid_argument("PatchCoverage.Check: the weld has fewer entries than there are vertices");
        for (const QuadGrid& Grid : Grids)
        {
            if (Grid.GroupCount < 0 || Grid.GroupSize < 0 || Grid.Points.size() != static_cast<size_t>(Grid.GroupCount) * static_cast<size_t>(Grid.GroupSize))
                throw std::invalid_argument("PatchCoverage.Check: a grid does not hold GroupCount x GroupSize points");
            for (const int32_t V : Grid.Points)
                if (V < 0 || static_cast<size_t>(V) >= VertexTotal) throw std::invalid_argument("PatchCoverage.Check: a grid point is outside the positions");
            if (!Representative.empty())
                for (const int32_t V : Grid.Points)
                    if (Representative[static_cast<size_t>(V)] < 0 || static_cast<size_t>(Representative[static_cast<size_t>(V)]) >= VertexTotal) throw std::invalid_argument("PatchCoverage.Check: the weld names a vertex outside the positions");
        }
        // The removed list in any order: sorted by triangle (the first proof of a triangle kept) in a copy when it is not strictly ascending already
        ProofList SortedCopy;
        if (std::adjacent_find(Removed.begin(), Removed.end(), [](const auto& A, const auto& B) { return A.first >= B.first; }) != Removed.end())
        {
            SortedCopy = Removed;
            std::stable_sort(SortedCopy.begin(), SortedCopy.end(), [](const auto& A, const auto& B) { return A.first < B.first; });
            SortedCopy.erase(std::unique(SortedCopy.begin(), SortedCopy.end(), [](const auto& A, const auto& B) { return A.first == B.first; }), SortedCopy.end());
        }
        const ProofList& Proofs = SortedCopy.empty() ? Removed : SortedCopy;
        const auto R = [&](int V) { return Representative.empty() ? V : Representative[static_cast<size_t>(V)]; };
        const auto IdAt = [&](int T, int K) { return Indices[static_cast<size_t>(T) * 3 + static_cast<size_t>(K)]; };

        std::vector<char> IsRemoved;
        if (!Proofs.empty())
        {
            IsRemoved.assign(static_cast<size_t>(TriangleCount), 0);
            for (const auto& Entry : Proofs)
                if (Entry.first >= 0 && Entry.first < TriangleCount) IsRemoved[static_cast<size_t>(Entry.first)] = 1;
        }
        const auto RemovedHas = [&](int T) { return !IsRemoved.empty() && IsRemoved[static_cast<size_t>(T)] != 0; };

        // Input triangles by their (welded) vertex set: duplicates share a key, each gets used once.
        GroupIndex<3> ByKey;
        double InputArea = 0;
        int Zero = 0;
        {
            std::vector<GroupIndex<3>::Item> Items;
            Items.reserve(static_cast<size_t>(TriangleCount));
            for (int T = 0; T < TriangleCount; T++)
            {
                const double A = Area(Positions, IdAt(T, 0), IdAt(T, 1), IdAt(T, 2));
                InputArea += A;
                if (A < ZeroArea) Zero++;
                Items.push_back({ SortedKey(R(IdAt(T, 0)), R(IdAt(T, 1)), R(IdAt(T, 2))), T });
            }
            ByKey.Build(std::move(Items), 0);
        }

        std::vector<int> Used(static_cast<size_t>(TriangleCount), 0);
        CoverageResult Result;
        int Meshes = 0, Lone = 0, Cells = 0, Leftover = 0;
        double OutputArea = 0, OtherArea = 0;
        using Half = std::array<int, 3>;
        const auto Real = [&](const Half& H) { return R(H[0]) != R(H[1]) && R(H[1]) != R(H[2]) && R(H[0]) != R(H[2]); };
        const auto KeyOf = [&](const Half& H) { return SortedKey(R(H[0]), R(H[1]), R(H[2])); };
        const auto HalfArea = [&](const Half& H) { return Area(Positions, H[0], H[1], H[2]); };
        for (const QuadGrid& Grid : Grids)
        {
            if (Grid.IsTriangle)
                Lone++;
            else
                Meshes++;
        }

        // A cell is cut two ways. Cells are taken in two passes so that the answer does not depend on the order the grids are listed in:
        //  - pass 0, the cells whose two cuts differ in how many halves are input triangles: they take the cut with more (the true cut always has both halves among the input,
        //    so a cut with fewer cannot be the true one); nothing here reads which triangles are still free, so the order of these cells changes nothing;
        //  - pass 1, the cells whose cuts tie (the surface was tessellated twice over these vertices): they take the cut whose halves are still free, after every cell of pass 0
        //    has taken its own, and in an order set by their corners (sorted vertex ids), not by where they are listed. HydraX takes any cut whose halves exist, so one such
        //    cell takes the same diagonal as another and both report a leftover and a missing triangle.
        // What remains order dependent: cells of pass 1 that share a triangle with each other and have the same corners (identical cells are interchangeable).
        struct CellRef
        {
            const QuadGrid* Grid;
            int G, I;
            std::array<int, 4> Key;   // the cell's welded corners, sorted
        };
        std::vector<CellRef> Ambiguous;
        using Cut = std::array<Half, 2>;
        // The two ways to cut a cell: the diagonal P00-P11 (First) and the diagonal P10-P01 (Second).
        const auto CutsOf = [&](const QuadGrid& Grid, int G, int I) {
            const int P00 = Grid.At(G, I), P10 = Grid.At(G + 1, I), P11 = Grid.At(G + 1, I + 1), P01 = Grid.At(G, I + 1);
            return std::array<Cut, 2>{ Cut{ { { P00, P10, P11 }, { P00, P11, P01 } } }, Cut{ { { P00, P10, P01 }, { P10, P11, P01 } } } };
        };
        struct Decision
        {
            const QuadGrid* Grid;
            int G, I;
            bool Second;
        };
        std::vector<Decision> Decisions;
        // Takes a cell's halves out of the input for the cut it is given.
        const auto Apply = [&](const QuadGrid& Grid, int G, int I, bool UseSecond) {
            Cells++;
            const std::array<Cut, 2> Cuts = CutsOf(Grid, G, I);
            const Cut& Chosen = Cuts[UseSecond ? 1 : 0];
            const Cut& Other = Cuts[UseSecond ? 0 : 1];
            int Matched = 0;
            for (const Half& H : Chosen)
            {
                if (!Real(H)) continue;
                OutputArea += HalfArea(H);
                const auto Candidates = ByKey.Find(KeyOf(H));
                if (Candidates.empty())
                {
                    Leftover++;
                    Result.Lines.push_back({ "leftover", -1, "output cell half (" + Int(H[0]) + "," + Int(H[1]) + "," + Int(H[2]) + ") is no input triangle" });
                    continue;
                }
                Matched++;
                // A triangle taken out on purpose never stands in for a kept one that has the same (welded) corners.
                int Free = -1;
                for (const auto& C : Candidates)
                    if (Used[static_cast<size_t>(C.Id)] == 0 && !RemovedHas(C.Id))
                    {
                        Free = C.Id;
                        break;
                    }
                if (Free >= 0)
                {
                    Used[static_cast<size_t>(Free)]++;
                }
                else
                {
                    Leftover++;
                    Result.Lines.push_back({ "leftover", Candidates.front().Id, "input triangle held by more output cells than it exists (" + Int(static_cast<int64_t>(Candidates.size())) + " input, used again)" });
                }
            }
            for (const Half& H : Other)
                if (Real(H)) OtherArea += HalfArea(H);

            if (Matched == 0 && std::none_of(Chosen.begin(), Chosen.end(), [&](const Half& H) { return Real(H); }))
            {
                // A cell with no area at all.
                Leftover++;
                Result.Lines.push_back({ "leftover", -1, "output cell with no area (all corners the same vertex or collinear)" });
            }
        };

        // The greedy choice: the cut whose halves are input triangles (and, for a tied cell, still free).
        const auto Handle = [&](const QuadGrid& Grid, int G, int I, bool Tied) {
            const std::array<Cut, 2> Cuts = CutsOf(Grid, G, I);
            const auto Score = [&](const Cut& Cut_) {
                int Free = 0, Exist = 0;
                for (const Half& H : Cut_)
                {
                    if (!Real(H)) continue;
                    const auto Found = ByKey.Find(KeyOf(H));
                    if (Found.empty()) continue;
                    Exist++;
                    Free += std::any_of(Found.begin(), Found.end(), [&](const auto& C) { return Used[static_cast<size_t>(C.Id)] == 0 && !RemovedHas(C.Id); }) ? 1 : 0;
                }
                return std::make_pair(Tied ? Free : 0, Exist);
            };
            const bool UseSecond = Score(Cuts[1]) > Score(Cuts[0]);
            Decisions.push_back({ &Grid, G, I, UseSecond });
            Apply(Grid, G, I, UseSecond);
        };

        int BothDiagonals = 0, BothNonPlanar = 0;
        for (const QuadGrid& Grid : Grids)
            for (int G = 0; G + 1 < Grid.GroupCount; G++)
                for (int I = 0; I + 1 < Grid.GroupSize; I++)
                {
                    const int P00 = Grid.At(G, I), P10 = Grid.At(G + 1, I), P11 = Grid.At(G + 1, I + 1), P01 = Grid.At(G, I + 1);
                    const auto Exists = [&](const Half& H) { return Real(H) && !ByKey.Find(KeyOf(H)).empty() ? 1 : 0; };
                    const int E1 = Exists({ P00, P10, P11 }) + Exists({ P00, P11, P01 }), E2 = Exists({ P00, P10, P01 }) + Exists({ P10, P11, P01 });
                    if (E1 == E2 && E1 > 0)
                    {
                        std::array<int, 4> Key = { R(P00), R(P10), R(P11), R(P01) };
                        std::sort(Key.begin(), Key.end());
                        Ambiguous.push_back({ &Grid, G, I, Key });
                        if (E1 == 2)
                        {
                            BothDiagonals++;
                            // planar: the fourth point lies within 0.01 of the plane of the other three
                            const V3 A = Point(Positions, P00), B = Point(Positions, P10), C = Point(Positions, P11), D = Point(Positions, P01);
                            const V3 N = Cross(Sub(B, A), Sub(C, A));
                            const double L = Length(N);
                            const bool Planar = L > 1e-12 && std::fabs(Dot(N, Sub(D, A))) / L <= 0.01;
                            if (!Planar)
                            {
                                BothNonPlanar++;
                                if (BothNonPlanar <= 20)
                                    Result.Lines.push_back({ "both_diagonals", -1, "cell (" + Int(P00) + "," + Int(P10) + "," + Int(P11) + "," + Int(P01) + ") has both diagonals as input triangles and is not planar" });
                            }
                        }
                        continue;
                    }
                    Handle(Grid, G, I, false);
                }
        std::stable_sort(Ambiguous.begin(), Ambiguous.end(), [](const CellRef& A, const CellRef& B) { return A.Key < B.Key; });
        for (const CellRef& C : Ambiguous) Handle(*C.Grid, C.G, C.I, true);

        // Repair, only when the greedy passes left something over or missing. Greedy takes cells one at a time, so a cell can take the cut a later cell needed; where several cells
        // share input triangles (five cells around a convex pentagon that all have two possible cuts) the order can leave two triangles missing and two over on correct output.
        // Each cell with two possible cuts (every real half of both cuts is an input triangle) is a choice; an input triangle that the chosen cuts use too often or too rarely is
        // unbalanced. The search flips a cell that touches an unbalanced triangle and, when that does not lower the total imbalance, flips one that touches what it unbalanced in
        // turn (a chain at most RepairDepth long, at most RepairBudget flips in all), and keeps the chain as soon as the imbalance is lower. Nothing is changed when no chain
        // helps, so a surface with a real loss reports it as before. The cells are tried in the order of their sorted corner ids, not the order they are listed in.
        const auto CountMissing = [&]() {
            int N = 0;
            for (int T = 0; T < TriangleCount; T++)
                if (Used[static_cast<size_t>(T)] == 0 && !RemovedHas(T)) N++;
            return N;
        };
        if (Leftover > 0 || CountMissing() > 0)
        {
            constexpr int RepairDepth = 12;
            constexpr int64_t RepairBudget = 200000;
            const size_t N = Decisions.size();
            std::vector<char> Choice(N, 0);
            std::vector<std::array<std::array<int, 2>, 2>> Keys(N);   // [decision][cut][half]: the input triangle the half is (its first copy's id), -1 for none
            std::vector<char> Flexible(N, 0);
            std::vector<int> Count(static_cast<size_t>(TriangleCount), 0), Use(static_cast<size_t>(TriangleCount), 0);
            std::vector<char> Counted(static_cast<size_t>(TriangleCount), 0);
            std::vector<std::vector<int>> Touching(static_cast<size_t>(TriangleCount));   // the flexible decisions with a half on that triangle
            for (size_t D = 0; D < N; D++)
            {
                Choice[D] = Decisions[D].Second ? 1 : 0;
                const std::array<Cut, 2> Cuts = CutsOf(*Decisions[D].Grid, Decisions[D].G, Decisions[D].I);
                bool Both = true;
                for (size_t C = 0; C < 2; C++)
                {
                    int RealHalves = 0, FoundHalves = 0;
                    for (size_t H = 0; H < 2; H++)
                    {
                        Keys[D][C][H] = -1;
                        const Half& Piece = Cuts[C][H];
                        if (!Real(Piece)) continue;
                        RealHalves++;
                        const auto Found = ByKey.Find(KeyOf(Piece));
                        if (Found.empty()) continue;
                        FoundHalves++;
                        const int Id = Found.front().Id;
                        Keys[D][C][H] = Id;
                        if (!Counted[static_cast<size_t>(Id)])
                        {
                            Counted[static_cast<size_t>(Id)] = 1;
                            for (const auto& Copy : Found)
                                if (!RemovedHas(Copy.Id)) Count[static_cast<size_t>(Id)]++;
                        }
                    }
                    Both = Both && RealHalves > 0 && RealHalves == FoundHalves;
                }
                Flexible[D] = Both ? 1 : 0;
                if (Both)
                    for (size_t C = 0; C < 2; C++)
                        for (size_t H = 0; H < 2; H++)
                            if (Keys[D][C][H] >= 0) Touching[static_cast<size_t>(Keys[D][C][H])].push_back(static_cast<int>(D));
            }
            // the order the flexible cells are tried in: their sorted corner ids, then the order met (identical cells are interchangeable)
            std::vector<std::pair<std::array<int, 4>, int>> Order;
            for (size_t D = 0; D < N; D++)
                if (Flexible[D])
                {
                    const std::array<Cut, 2> Cuts = CutsOf(*Decisions[D].Grid, Decisions[D].G, Decisions[D].I);
                    std::array<int, 4> Corner = { R(Cuts[0][0][0]), R(Cuts[0][0][1]), R(Cuts[0][0][2]), R(Cuts[0][1][2]) };
                    std::sort(Corner.begin(), Corner.end());
                    Order.push_back({ Corner, static_cast<int>(D) });
                }
            std::sort(Order.begin(), Order.end());
            std::vector<int> Rank(N, -1);
            for (size_t K = 0; K < Order.size(); K++) Rank[static_cast<size_t>(Order[K].second)] = static_cast<int>(K);

            int64_t Imbalance = 0;
            std::set<int> Unbalanced;
            // nothing is used yet: every triangle some cell can take is short by its count
            for (int T = 0; T < TriangleCount; T++)
                if (Counted[static_cast<size_t>(T)] && Count[static_cast<size_t>(T)] > 0)
                {
                    Imbalance += Count[static_cast<size_t>(T)];
                    Unbalanced.insert(T);
                }
            const auto Bump = [&](int Key, int By) {
                int& U = Use[static_cast<size_t>(Key)];
                const int C = Count[static_cast<size_t>(Key)];
                Imbalance += std::abs(U + By - C) - std::abs(U - C);
                U += By;
                if (U == C)
                    Unbalanced.erase(Key);
                else
                    Unbalanced.insert(Key);
            };
            for (size_t D = 0; D < N; D++)
                for (size_t H = 0; H < 2; H++)
                {
                    const int Key = Keys[D][static_cast<size_t>(Choice[D])][H];
                    if (Key >= 0) Bump(Key, 1);
                }
            const int64_t Start = Imbalance;
            const auto Flip = [&](size_t D) {
                for (size_t H = 0; H < 2; H++)
                {
                    const int Old = Keys[D][static_cast<size_t>(Choice[D])][H], New = Keys[D][static_cast<size_t>(1 - Choice[D])][H];
                    if (Old >= 0) Bump(Old, -1);
                    if (New >= 0) Bump(New, 1);
                }
                Choice[D] = static_cast<char>(1 - Choice[D]);
            };
            std::vector<char> InChain(N, 0);
            // one budget for the whole repair, charged for candidates looked at as well as flips: a soup with many separate failures stops early and reports them
            int64_t Budget = RepairBudget;
            std::function<bool(int, int64_t)> Try = [&](int Depth, int64_t Before) {
                std::vector<int> Candidates;
                for (const int Key : Unbalanced)
                    for (const int D : Touching[static_cast<size_t>(Key)])
                        if (!InChain[static_cast<size_t>(D)]) Candidates.push_back(D);
                std::sort(Candidates.begin(), Candidates.end(), [&](int A, int B) { return Rank[static_cast<size_t>(A)] < Rank[static_cast<size_t>(B)]; });
                Candidates.erase(std::unique(Candidates.begin(), Candidates.end()), Candidates.end());
                Budget -= static_cast<int64_t>(Candidates.size());
                if (Budget < 0) return false;
                for (const int D : Candidates)
                {
                    if (--Budget < 0) return false;
                    Flip(static_cast<size_t>(D));
                    InChain[static_cast<size_t>(D)] = 1;
                    if (Imbalance < Before) return true;
                    if (Depth < RepairDepth && Try(Depth + 1, Before)) return true;
                    InChain[static_cast<size_t>(D)] = 0;
                    Flip(static_cast<size_t>(D));
                }
                return false;
            };
            while (Imbalance > 0)
            {
                std::fill(InChain.begin(), InChain.end(), 0);
                if (!Try(1, Imbalance)) break;
            }
            if (Imbalance < Start)
            {
                // account again from nothing with the repaired cuts (the lines of the leftovers are written again, the other lines stay)
                std::fill(Used.begin(), Used.end(), 0);
                Leftover = 0;
                Cells = 0;
                OutputArea = 0;
                OtherArea = 0;
                Result.Lines.erase(std::remove_if(Result.Lines.begin(), Result.Lines.end(), [](const CoverageLine& L) { return L.Kind == "leftover"; }), Result.Lines.end());
                for (size_t D = 0; D < N; D++) Apply(*Decisions[D].Grid, Decisions[D].G, Decisions[D].I, Choice[D] != 0);
            }
        }
        Result.CellsBothDiagonals = BothDiagonals;
        Result.CellsBothDiagonalsNonPlanar = BothNonPlanar;

        int Missing = 0, Covered = 0, RemovedCount = 0;
        double RemovedArea = 0;
        for (int T = 0; T < TriangleCount; T++)
        {
            if (Used[static_cast<size_t>(T)] > 0)
            {
                Covered++;
                continue;
            }
            const auto Proof = std::lower_bound(Proofs.begin(), Proofs.end(), T, [](const auto& E, int Key) { return E.first < Key; });
            if (Proof != Proofs.end() && Proof->first == T)
            {
                RemovedCount++;
                RemovedArea += Area(Positions, IdAt(T, 0), IdAt(T, 1), IdAt(T, 2));
                Result.Lines.push_back({ "removed", T, Proof->second });
                continue;
            }
            Missing++;
            const double A = Area(Positions, IdAt(T, 0), IdAt(T, 1), IdAt(T, 2));
            Result.Lines.push_back({ "missing", T, A < ZeroArea ? "zero-area input triangle, no cell holds it" : "no output cell has the three corners " + Int(IdAt(T, 0)) + "," + Int(IdAt(T, 1)) + "," + Int(IdAt(T, 2)) + " as one half" });
        }

        Result.InputTriangles = TriangleCount;
        Result.ZeroAreaInput = Zero;
        Result.Meshes = Meshes;
        Result.LoneTriangles = Lone;
        Result.Cells = Cells;
        Result.Covered = Covered;
        Result.Removed = RemovedCount;
        Result.Missing = Missing;
        Result.Leftover = Leftover;
        Result.InputArea = InputArea;
        Result.RemovedArea = RemovedArea;
        Result.OutputArea = OutputArea;
        Result.OtherDiagonalArea = OtherArea;
        return Result;
    }
}

// ---- degenerate texture coordinates

namespace
{
    constexpr double DegenerateRatioCap = 1e9;   // ratios are capped so a fully collapsed axis prints as a number

    struct UvSample
    {
        double Area, Ratio;
        size_t Index;   // the order the triangles were met in, the last tie-break
    };

    void AddUvTriangle(std::vector<UvSample>& Samples, const V3& Pa, const MeshPointData& Da, const V3& Pb, const MeshPointData& Db, const V3& Pc, const MeshPointData& Dc)
    {
        const V3 E1 = Sub(Pb, Pa), E2 = Sub(Pc, Pa);
        const V3 Cr = Cross(E1, E2);
        const double TwiceArea = Length(Cr);
        const double L1 = Length(E1);
        if (TwiceArea < 1e-9 || L1 < 1e-9) return;

        // 2D frame in the triangle's plane: e1 = (l1, 0), e2 = (x2, y2).
        const double X2 = Dot(E1, E2) / L1;
        const double Y2 = TwiceArea / L1;
        const double Du1 = Db.U - Da.U, Dv1 = Db.V - Da.V, Du2 = Dc.U - Da.U, Dv2 = Dc.V - Da.V;

        // J = [d1 d2] * inverse([e1 e2]) with the 2D columns (l1, 0) and (x2, y2).
        const double J00 = Du1 / L1;
        const double J10 = Dv1 / L1;
        const double J01 = (Du2 - (Du1 * X2 / L1)) / Y2;
        const double J11 = (Dv2 - (Dv1 * X2 / L1)) / Y2;

        // Singular values of a 2x2 matrix from its Frobenius norm and determinant.
        const double F = (J00 * J00) + (J01 * J01) + (J10 * J10) + (J11 * J11);
        const double Det = std::fabs((J00 * J11) - (J01 * J10));
        const double Disc = std::sqrt((std::max)((F * F) - (4 * Det * Det), 0.0));
        const double Big = std::sqrt((F + Disc) / 2);
        const double Small = std::sqrt((std::max)((F - Disc) / 2, 0.0));

        // WHY: near-collapsed values lose the small singular value to rounding in f - disc, so a determinant that is tiny against the norm is taken as zero.
        const double Ratio = Big <= 0 || Det <= DegenerateMeshUv::CollapseFraction * Big * Big ? DegenerateRatioCap : (std::min)(Big / Small, DegenerateRatioCap);
        Samples.push_back({ TwiceArea / 2, Ratio, Samples.size() });
    }
}

void DegenerateMeshUv::Add(const DegenerateMeshUv& Other)
{
    Replaced += Other.Replaced;
    MaxSeen = (std::isnan(MaxSeen) || std::isnan(Other.MaxSeen)) ? std::nan("") : (std::max)(MaxSeen, Other.MaxSeen);   // Math.Max: a NaN wins
    for (const auto& [Material, Count] : Other.ByMaterial) ByMaterial[Material] += Count;
}

bool DegenerateMeshUv::Check(const MapMesh& Mesh)
{
    double Median = 0, Collapsed = 0;
    if (!Mesh.HasData() || !Measure(Mesh, Median, Collapsed) || (Median <= MaxRatio && Collapsed <= 0.5)) return false;
    Replaced++;
    MaxSeen = (std::isnan(MaxSeen) || std::isnan(Median)) ? std::nan("") : (std::max)(MaxSeen, Median);
    ByMaterial[Mesh.Material]++;
    return true;
}

bool DegenerateMeshUv::Measure(const MapMesh& Mesh, double& Median, double& CollapsedShare)
{
    Median = 1;
    CollapsedShare = 0;
    if (!Mesh.HasData()) return false;
    std::vector<UvSample> Samples;
    for (int G = 0; G + 1 < Mesh.GroupCount; G++)
        for (int I = 0; I + 1 < Mesh.GroupSize; I++)
        {
            AddUvTriangle(Samples, Mesh.At(G, I), Mesh.DataAt(G, I), Mesh.At(G + 1, I), Mesh.DataAt(G + 1, I), Mesh.At(G, I + 1), Mesh.DataAt(G, I + 1));
            AddUvTriangle(Samples, Mesh.At(G + 1, I + 1), Mesh.DataAt(G + 1, I + 1), Mesh.At(G, I + 1), Mesh.DataAt(G, I + 1), Mesh.At(G + 1, I), Mesh.DataAt(G + 1, I));
        }
    double Total = 0;
    for (const UvSample& S : Samples) Total += S.Area;
    if (Samples.empty() || Total <= 0) return false;

    // A strict total order: ascending ratio with a NaN before every number (as .NET's CompareTo orders them), equal ratios by the order met in.
    std::sort(Samples.begin(), Samples.end(), [](const UvSample& A, const UvSample& B) {
        const bool Na = std::isnan(A.Ratio), Nb = std::isnan(B.Ratio);
        if (Na != Nb) return Na;
        if (!Na && A.Ratio != B.Ratio) return A.Ratio < B.Ratio;
        return A.Index < B.Index;
    });
    // WHY: when the cumulative area reaches exactly half at one triangle (two equal triangles of a 2x2 cell), the median is the mean of that ratio and the next,
    // as an unweighted median of an even count is.
    double Run = 0;
    Median = Samples.back().Ratio;
    for (size_t I = 0; I < Samples.size(); I++)
    {
        Run += Samples[I].Area;
        if (Run >= (Total / 2) - (1e-9 * Total))
        {
            Median = Run - (Total / 2) < 1e-9 * Total && I + 1 < Samples.size() ? (Samples[I].Ratio + Samples[I + 1].Ratio) / 2 : Samples[I].Ratio;
            break;
        }
    }
    double CollapsedArea = 0;
    for (const UvSample& S : Samples)
        if (S.Ratio >= 1 / CollapseFraction) CollapsedArea += S.Area;
    CollapsedShare = CollapsedArea / Total;
    return true;
}

std::string DegenerateMeshUv::Line() const
{
    std::vector<std::pair<std::string, int>> Top(ByMaterial.begin(), ByMaterial.end());
    std::stable_sort(Top.begin(), Top.end(), [](const auto& A, const auto& B) { return A.second > B.second; });   // the map is ordinal by name already
    std::string List;
    for (size_t I = 0; I < Top.size() && I < 10; I++) List += (I == 0 ? "" : ", ") + Top[I].first + " " + Int(Top[I].second);
    return "Degenerate texture coordinates (the area-weighted median singular-value ratio of the 3D to UV Jacobian is above " + Shortest(MaxRatio)
           + ", or a UV axis is collapsed over more than half the mesh): " + Int(Replaced) + " meshes flagged, largest ratio " + Fixed(MaxSeen, 3)
           + (Replaced == 0 ? std::string() : "; by material (top 10): " + List) + ".\n";
}

// ---- brush faces

void BrushUvReport::Add(const BrushUvReport& Other)
{
    Solved += Other.Solved;
    NonAxial += Other.NonAxial;
    NoRenderPoints += Other.NoRenderPoints;
}

std::string BrushUvReport::Line() const
{
    return "Brush face textures: " + Int(Solved) + " faces solved from render vertices, " + Int(NonAxial) + " with render vertices but no clean axial fit, " + Int(NoRenderPoints)
           + " with no render vertices";
}

namespace BrushFaceUv
{
    namespace
    {
        constexpr double FitTolerance = 1e-3;
        constexpr double SeedTolerance = 0.02;
        constexpr int MaxSeeds = 24;

        struct BaseAxes
        {
            V3 Normal;
            int Ps;
            double Bs;
            int Pt;
            double Bt;
        };

        // Q3 base axes in table order: normal, S axis, T axis, with the world index of S and T and their signs. The first strictly largest dot wins.
        const std::array<BaseAxes, 6> Base = { {
            { { 0, 0, 1 }, 0, 1, 1, -1 },
            { { 0, 0, -1 }, 0, 1, 1, -1 },
            { { 1, 0, 0 }, 1, 1, 2, -1 },
            { { -1, 0, 0 }, 1, 1, 2, -1 },
            { { 0, 1, 0 }, 0, 1, 2, -1 },
            { { 0, -1, 0 }, 0, 1, 2, -1 },
        } };

        using Coefficients = std::array<double, 6>;

        // The base axes of the plane through the face points: normal = (p3 - p1) x (p2 - p1)
        const BaseAxes& Axes(const V3& P1, const V3& P2, const V3& P3)
        {
            V3 N = Cross(Sub(P3, P1), Sub(P2, P1));
            const double L = Length(N);
            if (L > 0) N = Mul(N, 1 / L);
            double Best = -1.0;
            size_t Index = 0;
            for (size_t I = 0; I < Base.size(); I++)
            {
                const double D = Dot(N, Base[I].Normal);
                if (D > Best)
                {
                    Best = D;
                    Index = I;
                }
            }
            return Base[Index];
        }

        double Mod(double Value, double Modulus) { return Value - (std::floor(Value / Modulus) * Modulus); }

        // Math.Round: halves go to the even number
        double RoundEven(double V) { return std::nearbyint(V); }

        std::pair<double, double> Wrapped(const Coefficients& X, double S, double T, const FaceSample& Sample)
        {
            const double Du = Sample.U - ((X[0] * S) + (X[1] * T) + X[2]);
            const double Dv = Sample.V - ((X[3] * S) + (X[4] * T) + X[5]);
            return { Du - RoundEven(Du), Dv - RoundEven(Dv) };
        }

        // Exact solution through the three corners of the triangle starting at 'first'; nullopt when they are nearly collinear
        std::optional<Coefficients> SolveExact(const std::vector<double>& S, const std::vector<double>& T, std::span<const FaceSample> Samples, size_t First)
        {
            const double S0 = S[First], T0 = T[First];
            const double Ds1 = S[First + 1] - S0, Dt1 = T[First + 1] - T0, Ds2 = S[First + 2] - S0, Dt2 = T[First + 2] - T0;
            const double Det = (Ds1 * Dt2) - (Ds2 * Dt1);
            if (std::fabs(Det) < 1e-6) return std::nullopt;
            const double Du1 = Samples[First + 1].U - Samples[First].U, Du2 = Samples[First + 2].U - Samples[First].U;
            const double Dv1 = Samples[First + 1].V - Samples[First].V, Dv2 = Samples[First + 2].V - Samples[First].V;
            const double A = ((Du1 * Dt2) - (Du2 * Dt1)) / Det;
            const double B = ((Ds1 * Du2) - (Ds2 * Du1)) / Det;
            const double C = ((Dv1 * Dt2) - (Dv2 * Dt1)) / Det;
            const double D = ((Ds1 * Dv2) - (Ds2 * Dv1)) / Det;
            return Coefficients{ A, B, Samples[First].U - (A * S0) - (B * T0), C, D, Samples[First].V - (C * S0) - (D * T0) };
        }

        // Least squares over the inliers, with each sample taken to the whole-number offset nearest to the current solution; nullopt when the inliers do not span the plane
        std::optional<Coefficients> Refit(const std::vector<double>& S, const std::vector<double>& T, std::span<const FaceSample> Samples, const std::vector<char>& Use, const Coefficients& Current)
        {
            double Cs = 0, Ct = 0;
            int N = 0;
            for (size_t I = 0; I < Use.size(); I++)
                if (Use[I])
                {
                    Cs += S[I];
                    Ct += T[I];
                    N++;
                }
            if (N < 3) return std::nullopt;
            Cs /= N;
            Ct /= N;
            double Sss = 0, Sst = 0, Stt = 0;
            double Bu[2] = { 0, 0 }, Bv[2] = { 0, 0 };
            double SumU = 0, SumV = 0;
            for (size_t I = 0; I < Use.size(); I++)
            {
                if (!Use[I]) continue;
                const double Ls = S[I] - Cs, Lt = T[I] - Ct;
                const double OffU = RoundEven(Samples[I].U - ((Current[0] * S[I]) + (Current[1] * T[I]) + Current[2]));
                const double OffV = RoundEven(Samples[I].V - ((Current[3] * S[I]) + (Current[4] * T[I]) + Current[5]));
                const double U = Samples[I].U - OffU, V = Samples[I].V - OffV;
                Sss += Ls * Ls;
                Sst += Ls * Lt;
                Stt += Lt * Lt;
                Bu[0] += Ls * U;
                Bu[1] += Lt * U;
                Bv[0] += Ls * V;
                Bv[1] += Lt * V;
                SumU += U;
                SumV += V;
            }
            const double Det = (Sss * Stt) - (Sst * Sst);
            if (std::fabs(Det) < 1e-9) return std::nullopt;
            const double A = ((Bu[0] * Stt) - (Bu[1] * Sst)) / Det;
            const double B = ((Bu[1] * Sss) - (Bu[0] * Sst)) / Det;
            const double C = ((Bv[0] * Stt) - (Bv[1] * Sst)) / Det;
            const double D = ((Bv[1] * Sss) - (Bv[0] * Sst)) / Det;
            return Coefficients{ A, B, (SumU / N) - (A * Cs) - (B * Ct), C, D, (SumV / N) - (C * Cs) - (D * Ct) };
        }

        // Coefficients are [Su_s, Su_t, u0, Tv_s, Tv_t, v0] with u = Su_s*s + Su_t*t + u0 (s, t the face's S and T world components)
        std::optional<BrushFaceNumbers> Canonical(const Coefficients& X, const BaseAxes& Ax)
        {
            const double Ln = std::sqrt((X[3] * X[3]) + (X[4] * X[4]));
            if (Ln < 1e-12) return std::nullopt;
            const double Sy = 1 / Ln;
            const double Pi = std::acos(-1.0);
            const double Rot = std::atan2(-Ax.Bt * X[3], Ax.Bt * X[4]) * 180 / Pi;
            const double C = std::cos(Rot * Pi / 180), Sn = std::sin(Rot * Pi / 180);
            const double Se = (X[0] * C) + (X[1] * Sn);
            const double Sq = (X[0] * -Sn) + (X[1] * C);
            if (std::fabs(Se) < 1e-12) return std::nullopt;
            const double Sx = Ax.Bs / Se;
            const double Shear = Sq * Sy / Ax.Bt;
            const BrushFaceNumbers Result = { Sx, Sy, Mod(-X[2] * Sx, std::fabs(Sx)), Mod(-X[5] * Sy, std::fabs(Sy)), Rot, Shear };
            if (std::isfinite(Result.ScaleX) && std::isfinite(Result.ShiftX) && std::isfinite(Result.Rotation) && std::isfinite(Result.Shear)) return Result;
            return std::nullopt;
        }
    }

    std::pair<double, double> Forward(const V3& P1, const V3& P2, const V3& P3, const BrushFaceNumbers& Numbers, const V3& Point)
    {
        const BaseAxes& Ax = Axes(P1, P2, P3);
        const double A = Numbers.Rotation * std::acos(-1.0) / 180;
        const double C = std::cos(A), S = std::sin(A);
        V3 SVec = { 0, 0, 0 }, TVec = { 0, 0, 0 };
        SVec[static_cast<size_t>(Ax.Ps)] = Ax.Bs * C / Numbers.ScaleX;
        SVec[static_cast<size_t>(Ax.Pt)] = Ax.Bs * S / Numbers.ScaleX;
        TVec[static_cast<size_t>(Ax.Ps)] = -Ax.Bt * S / Numbers.ScaleY;
        TVec[static_cast<size_t>(Ax.Pt)] = Ax.Bt * C / Numbers.ScaleY;
        for (size_t I = 0; I < 3; I++) SVec[I] += Numbers.Shear * TVec[I];
        const double U = (SVec[0] * Point[0]) + (SVec[1] * Point[1]) + (SVec[2] * Point[2]) - (Numbers.ShiftX / Numbers.ScaleX);
        const double V = (TVec[0] * Point[0]) + (TVec[1] * Point[1]) + (TVec[2] * Point[2]) - (Numbers.ShiftY / Numbers.ScaleY);
        return { U, V };
    }

    std::optional<BrushFaceNumbers> Solve(const V3& P1, const V3& P2, const V3& P3, std::span<const FaceSample> Samples)
    {
        if (Samples.size() < 3) return std::nullopt;
        const BaseAxes& Ax = Axes(P1, P2, P3);
        const size_t Count = Samples.size();
        std::vector<double> S(Count), T(Count);
        for (size_t I = 0; I < Count; I++)
        {
            S[I] = Samples[I].Position[static_cast<size_t>(Ax.Ps)];
            T[I] = Samples[I].Position[static_cast<size_t>(Ax.Pt)];
        }

        // WHY: a surface restarts its coordinates at whole numbers, so the fit works modulo 1: a seed triangle (one offset) gives a first solution, then every
        // sample is taken to the nearest whole number of that solution and the inliers are refitted.
        std::vector<char> Best;
        Coefficients BestCoefficients{};
        bool HaveBest = false;
        size_t BestCount = 0;
        for (size_t Seed = 0; Seed < MaxSeeds && (Seed * 3) + 2 < Count; Seed++)
        {
            const size_t First = Seed * 3;
            std::optional<Coefficients> Start = SolveExact(S, T, Samples, First);
            if (!Start) continue;
            Coefficients Current = *Start;
            std::vector<char> Inliers(Count, 0);
            for (int Pass = 0; Pass < 3; Pass++)
            {
                const double Tolerance = Pass == 0 ? SeedTolerance : FitTolerance * 2;
                for (size_t I = 0; I < Count; I++)
                {
                    const auto [Du, Dv] = Wrapped(Current, S[I], T[I], Samples[I]);
                    Inliers[I] = std::fabs(Du) <= Tolerance && std::fabs(Dv) <= Tolerance;
                }
                const std::optional<Coefficients> Refit_ = Refit(S, T, Samples, Inliers, Current);
                if (!Refit_) break;
                Current = *Refit_;
            }

            size_t N = 0;
            for (size_t I = 0; I < Count; I++)
            {
                const auto [Du, Dv] = Wrapped(Current, S[I], T[I], Samples[I]);
                Inliers[I] = std::fabs(Du) <= FitTolerance && std::fabs(Dv) <= FitTolerance;
                N += Inliers[I] ? 1 : 0;
            }
            if (N > BestCount)
            {
                BestCount = N;
                Best = Inliers;
                BestCoefficients = Current;
                HaveBest = true;
            }
            if (N == Count) break;
        }

        if (!HaveBest || BestCount < 3 || (Count > 3 && BestCount * 4 < Count * 3)) return std::nullopt;
        const std::optional<Coefficients> Final = Refit(S, T, Samples, Best, BestCoefficients);
        return Final ? Canonical(*Final, Ax) : std::nullopt;
    }
}
}
