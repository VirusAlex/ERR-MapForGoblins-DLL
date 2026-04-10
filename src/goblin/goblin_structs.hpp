#pragma once
#include <vector>

namespace goblin::mapPoint {
	struct MapTile {
		int X;
		int Y;
		int Z;
		MapTile(int x, int y, int z) :X(x), Y(y), Z(z) {}
		MapTile(int x, int y) :X(x), Y(y), Z(0) {}
		MapTile(int x) :X(x), Y(0), Z(0) {}

		bool operator==(const MapTile& other) const {
			return X == other.X && Y == other.Y && Z == other.Z;
		}

		bool operator!=(const MapTile& other) const {
			return !(*this == other);
		}
	};

	struct MapFragments {
		int mapFragmentId;
		std::vector<MapTile> mapFragmentTile;
		MapFragments(int fragmentId, std::vector<MapTile> mapChunks)
			: mapFragmentId(fragmentId), mapFragmentTile(mapChunks) {}
	};

	// [lowerBound, upperBound)
	struct ParamRange {
		int lowerBound;
		int upperBound;
		constexpr ParamRange(int lower, int upper) : lowerBound(lower), upperBound(upper) {}
		bool IsInRange(int id) const {
			return (id >= lowerBound && id < upperBound);
		}
	};
};