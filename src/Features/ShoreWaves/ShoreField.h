#pragma once

#include "Buffer.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief Per-worldspace shoreline signed distance field, baked from plugin terrain and water heights.
 *
 * For each texel (kTexelSize world units square) the field stores the signed distance to the
 * shoreline (positive in water, negative on land), a smoothed unit direction pointing toward
 * land, and a validity flag. It is generated on a worker thread from every cell's LAND VHGT
 * heightmap and CELL XCLW water height in reverse load order, mirroring UnifiedWater's cache
 * reader, then cached on disk keyed by worldspace and load order.
 *
 * Runtime use: call Update() from the render thread every frame with the active worldspace.
 * It launches bakes, picks up finished results, uploads the texture, and exposes an SRV.
 */
class ShoreField
{
public:
	static constexpr uint32_t kFormatVersion = 5;  // v2: bathymetry channel; v3: XCLW default marker; v4: 3x3 vertex average; v5: small islands ignored, wider direction blur
	static constexpr uint32_t kTexelSize = 256;    // world units per texel: 16 texels per 4096-unit cell
	static constexpr uint32_t kTexelsPerCell = 4096 / kTexelSize;
	static constexpr float kMaxRange = 8192.0f;    // signed distance is clamped to +/- this before encoding
	static constexpr float kBathyRange = 2048.0f;  // water depth (or land height above water) clamped to +/- this
	static constexpr int kDirBlurRadius = 14;      // texels (~50 m) of box blur before taking the direction gradient
	static constexpr uint32_t kMinLandTexels = 96;  // land components smaller than this (~1300 m^2) are not a shoreline: waves pass them
	static constexpr int32_t kMaxCellSpan = 512;   // refuse worldspaces larger than this many cells per axis

	enum class Status
	{
		None,     // no worldspace, or worldspace has no usable terrain
		Loading,  // reading cache or baking on the worker thread
		Ready,    // texture uploaded and bound
		Failed,   // bake or cache read failed for the requested worldspace
	};

#pragma pack(push, 1)
	struct Header
	{
		uint32_t label = 0;  // 'SWSF'
		uint32_t version = kFormatVersion;
		uint64_t loadOrderHash = 0;
		int32_t minCellX = 0;  // cell coordinate of texel column 0
		int32_t minCellY = 0;  // cell coordinate of texel row 0
		int32_t width = 0;     // texels
		int32_t height = 0;    // texels
		uint32_t texelSize = kTexelSize;
		float maxRange = kMaxRange;
		uint32_t waterTexels = 0;  // stats for the UI
		uint32_t landTexels = 0;
	};

	struct Texel
	{
		int16_t dist;   // signed distance / maxRange, SNORM
		int16_t dirX;   // unit direction toward land, SNORM
		int16_t dirY;
		int16_t bathy;  // (water height - land height) / kBathyRange * 0.5, so valid texels lie in [-0.5, 0.5]; -1 = no terrain data
	};
#pragma pack(pop)

	struct Field
	{
		std::string editorID;
		Header header;
		std::vector<Texel> texels;
	};

	ShoreField() = default;
	~ShoreField();
	ShoreField(const ShoreField&) = delete;
	ShoreField& operator=(const ShoreField&) = delete;

	/** @brief Render-thread tick: tracks the worldspace, starts work, and uploads finished fields. */
	void Update(RE::TESWorldSpace* worldSpace);

	/** @brief Deletes the cache for the active worldspace and bakes it again. */
	void RequestRebuild();

	ID3D11ShaderResourceView* GetSRV() const { return texture ? texture->srv.get() : nullptr; }
	Status GetStatus() const { return status; }
	bool IsReady() const { return status == Status::Ready && texture; }
	const Header& GetActiveHeader() const { return activeHeader; }
	const std::string& GetActiveEditorID() const { return activeEditorID; }
	const std::string& GetLastError() const { return lastError; }

	/** @brief World XY of the min corner of texel (0, 0). */
	float2 GetOrigin() const
	{
		return { static_cast<float>(activeHeader.minCellX) * 4096.0f, static_cast<float>(activeHeader.minCellY) * 4096.0f };
	}

	/** @brief Resolves a worldspace to the one whose terrain it actually uses. */
	static RE::TESWorldSpace* ResolveLandWorldSpace(RE::TESWorldSpace* worldSpace);

private:
	struct CellRecord
	{
		bool hasLand = false;
		float waterHeight = 0.0f;
		float heights[kTexelsPerCell * kTexelsPerCell] = {};  // land height at each texel centre
	};

	static std::filesystem::path GetCacheDir();
	static std::filesystem::path GetCachePath(const std::string& editorID);
	static uint64_t ComputeLoadOrderHash();

	static bool ReadCache(const std::filesystem::path& path, uint64_t expectedHash, Field& out, std::string& error);
	static bool WriteCache(const std::filesystem::path& path, const Field& field, std::string& error);

	static bool Bake(RE::TESWorldSpace* worldSpace, uint64_t loadOrderHash, Field& out, std::string& error);
	static bool ReadCellRecord(RE::TESWorldSpace* worldSpace, std::vector<RE::TESFile*>& files, int32_t x, int32_t y, CellRecord& out);
	static void ReadLandHeights(RE::TESFile* file, CellRecord& out);
	static void ReadWaterHeight(RE::TESFile* file, float& waterHeight);
	static void DistanceTransform(std::vector<float>& grid, int32_t width, int32_t height);
	static void BoxBlur(std::vector<float>& grid, int32_t width, int32_t height, int radius);

	void StartWorker(RE::TESWorldSpace* worldSpace, bool forceRebuild);
	void AdoptPending();
	bool UploadTexture(const Field& field);

	std::jthread worker;
	std::atomic<bool> workerRunning{ false };

	std::mutex pendingMutex;
	std::unique_ptr<Field> pending;   // produced by the worker, consumed by Update()
	std::string pendingError;
	bool pendingFailed = false;

	std::unique_ptr<Texture2D> texture;
	Header activeHeader{};
	std::string activeEditorID;     // worldspace whose field is uploaded
	std::string requestedEditorID;  // worldspace the worker is producing or last failed for
	std::string lastError;
	Status status = Status::None;
};
