#include <vector>

namespace goblin::mapPoint {
	struct MapTile {
		int X;
		int Y;
		int Z;
		// Constructor for initializing MapChunk
		MapTile(int x, int y, int z) :X(x), Y(y), Z(z) {}
		MapTile(int x, int y) :X(x), Y(y), Z(0) {}
		MapTile(int x) :X(x), Y(0), Z(0) {}

		// If is all same
		bool operator==(const MapTile& other) const {
			return X == other.X && Y == other.Y && Z == other.Z;
		}

		// Check if all same, return reverse
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

	/// <summary>
	/// Range with lowerBound (inclusive) and upperBound (exclusive)
	/// </summary>
	struct ParamRange {
		int lowerBound;
		int upperBound;
		// lowerBound is inclusive, upperBound is exclusive
		constexpr ParamRange(int lower, int upper) : lowerBound(lower), upperBound(upper) {}
		// lowerBound is inclusive, upperBound is exclusive
		bool IsInRange(int id) const {
			return (id >= lowerBound && id < upperBound);
		}
	};
};