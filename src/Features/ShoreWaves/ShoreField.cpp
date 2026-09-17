#include "ShoreField.h"

#include <cmath>
#include <fstream>

#include "Utils/FileSystem.h"
#include "Utils/Game.h"

namespace
{
	constexpr uint32_t kLabel = Util::FCC("SWSF");
	constexpr float kMaxValidHeight = 50000.0f;  // beyond any real terrain, below sentinel garbage
	constexpr float kFarSquared = 1e12f;         // "no seed" value for the distance transform

	bool IsValidHeight(const float h)
	{
		return std::isfinite(h) && h != FLT_MIN && h != FLT_MAX && std::fabs(h) < kMaxValidHeight;
	}

	int16_t ToSnorm(float v)
	{
		return static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
	}
}

ShoreField::~ShoreField()
{
	if (worker.joinable()) {
		worker.request_stop();
		worker.join();
	}
}

RE::TESWorldSpace* ShoreField::ResolveLandWorldSpace(RE::TESWorldSpace* worldSpace)
{
	while (worldSpace && worldSpace->parentWorld && worldSpace->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLandData))
		worldSpace = worldSpace->parentWorld;
	return worldSpace;
}

std::filesystem::path ShoreField::GetCacheDir()
{
	return Util::PathHelpers::GetCommunityShaderPath() / "ShoreWavesCache";
}

std::filesystem::path ShoreField::GetCachePath(const std::string& editorID)
{
	return GetCacheDir() / std::format("{}.swf", editorID);
}

// FNV-1a over the load order plus the plugin and format versions, same idea as UnifiedWater's
// hash but stored inside each cache file so every worldspace validates independently.
uint64_t ShoreField::ComputeLoadOrderHash()
{
	uint64_t hash = 14695981039346656037ull;
	auto addBytes = [&](const char* p) {
		for (; p && *p; ++p) {
			hash ^= static_cast<unsigned char>(*p);
			hash *= 1099511628211ull;
		}
	};

	if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
		if (const auto mods = dataHandler->GetLoadedMods()) {
			for (uint32_t i = 0, n = dataHandler->GetLoadedModCount(); i < n; ++i)
				if (mods[i])
					addBytes(mods[i]->fileName);
		}
		if (const auto lightMods = dataHandler->GetLoadedLightMods()) {
			for (uint32_t i = 0, n = dataHandler->GetLoadedLightModCount(); i < n; ++i)
				if (lightMods[i])
					addBytes(lightMods[i]->fileName);
		}
	}

	addBytes(Plugin::VERSION.string().c_str());
	const auto formatVersion = std::to_string(kFormatVersion) + "/" + std::to_string(kTexelSize) + "/" + std::to_string(kDirBlurRadius);
	addBytes(formatVersion.c_str());
	return hash;
}

// ------------------------------------------------------------------------------------------------
// Render-thread side

void ShoreField::Update(RE::TESWorldSpace* worldSpace)
{
	AdoptPending();

	worldSpace = ResolveLandWorldSpace(worldSpace);
	const char* editorID = worldSpace ? worldSpace->GetFormEditorID() : nullptr;
	if (!worldSpace || !editorID || !*editorID || !worldSpace->worldWater) {
		// Interiors and worldspaces without water keep the last field bound; it is harmless
		// since the shader also checks the sampled texel's validity and bounds.
		return;
	}

	if (activeEditorID == editorID && status == Status::Ready)
		return;

	if (workerRunning.load(std::memory_order_acquire))
		return;

	if (requestedEditorID == editorID && status == Status::Failed)
		return;  // do not retry a failed bake every frame; RequestRebuild() clears this

	StartWorker(worldSpace, false);
}

void ShoreField::RequestRebuild()
{
	if (workerRunning.load(std::memory_order_acquire))
		return;
	auto* tes = RE::TES::GetSingleton();
	auto* worldSpace = tes ? ResolveLandWorldSpace(tes->GetRuntimeData2().worldSpace) : nullptr;
	if (!worldSpace)
		return;
	StartWorker(worldSpace, true);
}

void ShoreField::StartWorker(RE::TESWorldSpace* worldSpace, bool forceRebuild)
{
	if (worker.joinable())
		worker.join();

	requestedEditorID = worldSpace->GetFormEditorID();
	status = Status::Loading;
	lastError.clear();
	workerRunning.store(true, std::memory_order_release);

	const std::string editorID = requestedEditorID;
	worker = std::jthread([this, worldSpace, editorID, forceRebuild](const std::stop_token&) {
		auto field = std::make_unique<Field>();
		field->editorID = editorID;
		std::string error;
		bool ok = false;

		try {
			const uint64_t hash = ComputeLoadOrderHash();
			const auto path = GetCachePath(editorID);

			if (!forceRebuild && ReadCache(path, hash, *field, error)) {
				ok = true;
				logger::info("[Shore Waves] [Field] Loaded cache for {} ({}x{} texels)", editorID, field->header.width, field->header.height);
			} else {
				if (!error.empty())
					logger::info("[Shore Waves] [Field] {}: {} - baking", editorID, error);
				error.clear();

				const auto t0 = std::chrono::steady_clock::now();
				ok = Bake(worldSpace, hash, *field, error);
				const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

				if (ok) {
					logger::info("[Shore Waves] [Field] Baked {} in {} ms: {}x{} texels, {} water, {} land", editorID, ms,
						field->header.width, field->header.height, field->header.waterTexels, field->header.landTexels);
					std::string writeError;
					if (!WriteCache(path, *field, writeError))
						logger::warn("[Shore Waves] [Field] {}", writeError);
				}
			}
		} catch (const std::exception& e) {
			ok = false;
			error = std::format("exception: {}", e.what());
		}

		{
			std::scoped_lock lock(pendingMutex);
			pendingFailed = !ok;
			pendingError = ok ? std::string{} : error;
			pending = ok ? std::move(field) : nullptr;
		}
		if (!ok)
			logger::error("[Shore Waves] [Field] {} failed: {}", editorID, error);
		workerRunning.store(false, std::memory_order_release);
	});
}

void ShoreField::AdoptPending()
{
	std::unique_ptr<Field> field;
	bool failed = false;
	std::string error;
	{
		std::scoped_lock lock(pendingMutex);
		if (!pending && !pendingFailed)
			return;
		field = std::move(pending);
		failed = pendingFailed;
		error = std::move(pendingError);
		pendingFailed = false;
		pendingError.clear();
	}

	if (failed) {
		status = Status::Failed;
		lastError = error;
		return;
	}

	if (field->header.width <= 0 || field->header.height <= 0) {
		status = Status::None;
		activeEditorID = field->editorID;
		activeHeader = {};
		texture.reset();
		return;
	}

	if (!UploadTexture(*field)) {
		status = Status::Failed;
		lastError = "texture upload failed";
		return;
	}

	activeHeader = field->header;
	activeEditorID = field->editorID;
	status = Status::Ready;
}

bool ShoreField::UploadTexture(const Field& field)
{
	auto device = globals::d3d::device;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = static_cast<UINT>(field.header.width);
	desc.Height = static_cast<UINT>(field.header.height);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
	desc.SampleDesc = { 1, 0 };
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA init{};
	init.pSysMem = field.texels.data();
	init.SysMemPitch = static_cast<UINT>(field.header.width * sizeof(Texel));

	ID3D11Texture2D* raw = nullptr;
	if (FAILED(device->CreateTexture2D(&desc, &init, &raw)) || !raw)
		return false;

	texture = std::make_unique<Texture2D>(raw, "ShoreWaves::ShoreField");

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	texture->CreateSRV(srvDesc);
	return true;
}

// ------------------------------------------------------------------------------------------------
// Cache files

bool ShoreField::ReadCache(const std::filesystem::path& path, uint64_t expectedHash, Field& out, std::string& error)
{
	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) {
		error = "no cache file";
		return false;
	}

	std::ifstream ifs(path, std::ios::binary);
	if (!ifs) {
		error = std::format("cannot open '{}'", path.string());
		return false;
	}

	Header header{};
	ifs.read(reinterpret_cast<char*>(&header), sizeof(header));
	if (!ifs || header.label != kLabel || header.version != kFormatVersion) {
		error = "cache header invalid or from another format version";
		return false;
	}
	if (header.loadOrderHash != expectedHash) {
		error = "load order changed";
		return false;
	}
	if (header.width < 0 || header.height < 0 || header.width > kMaxCellSpan * static_cast<int32_t>(kTexelsPerCell) || header.height > kMaxCellSpan * static_cast<int32_t>(kTexelsPerCell)) {
		error = "cache dimensions out of range";
		return false;
	}

	const size_t count = static_cast<size_t>(header.width) * static_cast<size_t>(header.height);
	out.header = header;
	out.texels.resize(count);
	if (count) {
		ifs.read(reinterpret_cast<char*>(out.texels.data()), count * sizeof(Texel));
		if (!ifs.good()) {
			error = "cache payload truncated";
			return false;
		}
	}
	return true;
}

bool ShoreField::WriteCache(const std::filesystem::path& path, const Field& field, std::string& error)
{
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);

	std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
	if (!ofs) {
		error = std::format("cannot write cache '{}'", path.string());
		return false;
	}
	ofs.write(reinterpret_cast<const char*>(&field.header), sizeof(field.header));
	ofs.write(reinterpret_cast<const char*>(field.texels.data()), field.texels.size() * sizeof(Texel));
	if (!ofs.good()) {
		error = std::format("failed writing cache '{}'", path.string());
		return false;
	}
	return true;
}

// ------------------------------------------------------------------------------------------------
// Plugin reading (worker thread)

void ShoreField::ReadWaterHeight(RE::TESFile* file, float& waterHeight)
{
	if (!file->SeekNextSubrecordType(Util::FCC("XCLW")))
		return;
	file->ReadData(&waterHeight, 4);
	if (file->isBigEndian)
		waterHeight = std::bit_cast<float>(_byteswap_ulong(std::bit_cast<uint32_t>(waterHeight)));
}

// VHGT: one float offset then 33x33 int8 deltas, row-major from the cell's south-west corner.
// Row delta accumulates down the first column, column deltas accumulate along each row.
// Height = accumulated * 8. Texel (i, j) centre sits on vertex (2i + 1, 2j + 1).
void ShoreField::ReadLandHeights(RE::TESFile* file, CellRecord& out)
{
	if (!file->SeekNextSubrecordType(Util::FCC("VHGT")))
		return;

	struct VHGTData
	{
		float offset;
		int8_t deltas[1089];
		uint8_t padding[3];
	};
	VHGTData data{};
	if (!file->ReadData(&data, sizeof(data)))
		return;

	float offset = data.offset;
	if (file->isBigEndian)
		offset = std::bit_cast<float>(_byteswap_ulong(std::bit_cast<uint32_t>(offset)));

	float vertices[33 * 33];
	for (size_t y = 0; y < 33; y++) {
		offset += static_cast<float>(data.deltas[y * 33]);
		float rowOffset = 0.0f;
		for (size_t x = 0; x < 33; x++) {
			if (x != 0)
				rowOffset += static_cast<float>(data.deltas[y * 33 + x]);
			vertices[y * 33 + x] = (rowOffset + offset) * 8.0f;
		}
	}

	// Average the 3x3 vertices around the texel centre (vertex 2i+1, 2j+1) instead of taking the
	// single centre vertex: one vertex per 256-unit texel aliased badly along sloping beaches.
	for (uint32_t j = 0; j < kTexelsPerCell; j++) {
		for (uint32_t i = 0; i < kTexelsPerCell; i++) {
			const int32_t vx = 2 * static_cast<int32_t>(i) + 1;
			const int32_t vy = 2 * static_cast<int32_t>(j) + 1;
			float sum = 0.0f;
			for (int32_t dy = -1; dy <= 1; ++dy)
				for (int32_t dx = -1; dx <= 1; ++dx)
					sum += vertices[(vy + dy) * 33 + (vx + dx)];
			out.heights[j * kTexelsPerCell + i] = sum / 9.0f;
		}
	}

	out.hasLand = true;
}

// Reverse load order: the last plugin that has the cell decides the water height; from that
// plugin backwards, the first one carrying LAND decides the heightmap. Same walk as
// UnifiedWater's TryGetCellData, but with one duplicated file handle per worker instead of
// one per cell.
bool ShoreField::ReadCellRecord(RE::TESWorldSpace* worldSpace, std::vector<RE::TESFile*>& files, int32_t x, int32_t y, CellRecord& out)
{
	out = {};
	float waterHeight = FLT_MAX;

	int32_t fileIndex = static_cast<int32_t>(files.size()) - 1;
	for (; fileIndex >= 0; --fileIndex) {
		auto* file = files[fileIndex];
		if (file && file->SeekCell(worldSpace, x, y)) {
			ReadWaterHeight(file, waterHeight);
			break;
		}
	}
	if (fileIndex < 0)
		return false;

	for (; fileIndex >= 0; --fileIndex) {
		auto* file = files[fileIndex];
		if (file && file->SeekCell(worldSpace, x, y) && file->SeekLandscapeForCurrentCell()) {
			ReadLandHeights(file, out);
			break;
		}
	}
	if (!out.hasLand)
		return false;

	// XCLW sentinels: absent, or the CK's 0x4F7FFFC9 (~4.29e9) "use the worldspace default"
	// marker, both mean the worldspace water height. A huge negative value (0xCF000000) or
	// NaN means the cell has no water at all, so nothing in it is submerged. Treating the
	// "default" marker as "no water" punched texel-aligned holes into the sea (2026-09-17).
	if (waterHeight == FLT_MAX || !std::isfinite(waterHeight) || waterHeight > 1e6f)
		waterHeight = worldSpace->defaultWaterHeight;
	else if (waterHeight < -1e6f)
		waterHeight = -1e9f;
	if (!IsValidHeight(waterHeight))
		waterHeight = -1e9f;
	out.waterHeight = waterHeight;
	return true;
}

// ------------------------------------------------------------------------------------------------
// Bake

// Felzenszwalb & Huttenlocher exact Euclidean distance transform, separable. grid holds 0 at
// seeds and kFarSquared elsewhere on input, squared distance in texels on output.
void ShoreField::DistanceTransform(std::vector<float>& grid, int32_t width, int32_t height)
{
	const int32_t n = std::max(width, height);
	std::vector<float> f(n), d(n), z(n + 1);
	std::vector<int32_t> v(n);

	auto edt1d = [&](int32_t count) {
		int32_t k = 0;
		v[0] = 0;
		z[0] = -1e30f;
		z[1] = 1e30f;
		for (int32_t q = 1; q < count; q++) {
			float s = ((f[q] + static_cast<float>(q) * q) - (f[v[k]] + static_cast<float>(v[k]) * v[k])) / (2.0f * q - 2.0f * v[k]);
			while (s <= z[k]) {
				k--;
				s = ((f[q] + static_cast<float>(q) * q) - (f[v[k]] + static_cast<float>(v[k]) * v[k])) / (2.0f * q - 2.0f * v[k]);
			}
			k++;
			v[k] = q;
			z[k] = s;
			z[k + 1] = 1e30f;
		}
		k = 0;
		for (int32_t q = 0; q < count; q++) {
			while (z[k + 1] < static_cast<float>(q))
				k++;
			const float dq = static_cast<float>(q - v[k]);
			d[q] = dq * dq + f[v[k]];
		}
	};

	// Columns
	for (int32_t x = 0; x < width; x++) {
		for (int32_t y = 0; y < height; y++)
			f[y] = grid[static_cast<size_t>(y) * width + x];
		edt1d(height);
		for (int32_t y = 0; y < height; y++)
			grid[static_cast<size_t>(y) * width + x] = d[y];
	}
	// Rows
	for (int32_t y = 0; y < height; y++) {
		float* row = grid.data() + static_cast<size_t>(y) * width;
		for (int32_t x = 0; x < width; x++)
			f[x] = row[x];
		edt1d(width);
		for (int32_t x = 0; x < width; x++)
			row[x] = d[x];
	}
}

// Separable box blur with clamped edges.
void ShoreField::BoxBlur(std::vector<float>& grid, int32_t width, int32_t height, int radius)
{
	if (radius <= 0)
		return;
	std::vector<float> tmp(grid.size());
	const float norm = 1.0f / static_cast<float>(2 * radius + 1);

	for (int32_t y = 0; y < height; y++) {
		const float* row = grid.data() + static_cast<size_t>(y) * width;
		float* out = tmp.data() + static_cast<size_t>(y) * width;
		float sum = 0.0f;
		for (int32_t x = -radius; x <= radius; x++)
			sum += row[std::clamp(x, 0, width - 1)];
		for (int32_t x = 0; x < width; x++) {
			out[x] = sum * norm;
			sum += row[std::clamp(x + radius + 1, 0, width - 1)] - row[std::clamp(x - radius, 0, width - 1)];
		}
	}
	for (int32_t x = 0; x < width; x++) {
		float sum = 0.0f;
		for (int32_t y = -radius; y <= radius; y++)
			sum += tmp[static_cast<size_t>(std::clamp(y, 0, height - 1)) * width + x];
		for (int32_t y = 0; y < height; y++) {
			grid[static_cast<size_t>(y) * width + x] = sum * norm;
			sum += tmp[static_cast<size_t>(std::clamp(y + radius + 1, 0, height - 1)) * width + x] - tmp[static_cast<size_t>(std::clamp(y - radius, 0, height - 1)) * width + x];
		}
	}
}

bool ShoreField::Bake(RE::TESWorldSpace* worldSpace, uint64_t loadOrderHash, Field& out, std::string& error)
{
	out.header = {};
	out.header.label = kLabel;
	out.header.loadOrderHash = loadOrderHash;
	out.texels.clear();

	// Cell bounds from the WRLD record, same validation as UnifiedWater.
	int32_t minX, minY, maxX, maxY;
	const auto wsMin = worldSpace->minimumCoords;
	const auto wsMax = worldSpace->maximumCoords;
	Util::WorldToCell(wsMin, minX, minY);
	Util::WorldToCell(wsMax, maxX, maxY);
	maxX -= 1;
	maxY -= 1;

	const bool invalidBounds = wsMin.x == FLT_MIN || wsMin.y == FLT_MIN || wsMax.x == FLT_MAX || wsMax.y == FLT_MAX ||
	                           wsMin.x == FLT_MAX || wsMin.y == FLT_MAX || wsMax.x == FLT_MIN || wsMax.y == FLT_MIN ||
	                           !std::isfinite(wsMin.x) || !std::isfinite(wsMin.y) || !std::isfinite(wsMax.x) || !std::isfinite(wsMax.y);
	if (invalidBounds || maxX < minX || maxY < minY || (maxX - minX + 1) > kMaxCellSpan || (maxY - minY + 1) > kMaxCellSpan) {
		// Empty field: the worldspace has no usable bounds. Not an error; the feature stays off here.
		logger::info("[Shore Waves] [Field] {}: no usable worldspace bounds, skipping", worldSpace->GetFormEditorID());
		return true;
	}

	const int32_t cellsW = maxX - minX + 1;
	const int32_t cellsH = maxY - minY + 1;
	auto* fileArray = worldSpace->sourceFiles.array;
	if (!fileArray || fileArray->empty()) {
		error = "worldspace has no source files";
		return false;
	}

	// Pass 1: read every cell's water height and texel-centre land heights.
	//
	// TESFile::Duplicate opens a file handle and a 16 KB buffer per call and there is no way to
	// free the object, only CloseTES() to release the handle. With thousands of plugins loaded a
	// worldspace can have hundreds of source files, so duplicates are made once per reader thread
	// for the whole bake (never per cell or per row), the thread count is capped so the handles in
	// flight stay in the low hundreds, and every duplicate is closed before returning. Leaking
	// them exhausted file handles process-wide and broke all later shader include reads.
	std::vector<CellRecord> cells(static_cast<size_t>(cellsW) * cellsH);
	std::atomic<uint32_t> cellsWithLand{ 0 };
	{
		const uint32_t fileCount = fileArray->size();
		// One reader thread. Concurrent TESFile::Duplicate/SeekCell on the same parents lost five
		// cells of terrain in the first in-game bake, and a single thread reads Tamriel in ~1.1 s.
		const uint32_t readerThreads = 1;
		logger::info("[Shore Waves] [Field] {}: {} source files, {} reader thread(s), {}x{} cells",
			worldSpace->GetFormEditorID(), fileCount, readerThreads, cellsW, cellsH);

		std::vector<std::jthread> readers;
		readers.reserve(readerThreads);
		for (uint32_t t = 0; t < readerThreads; ++t) {
			readers.emplace_back([&, t, readerThreads] {
				std::vector<RE::TESFile*> files;
				files.reserve(fileArray->size());
				for (uint32_t f = 0; f < fileArray->size(); ++f) {
					auto* file = fileArray->data()[f];
					files.push_back(file ? file->Duplicate() : nullptr);
				}

				for (int32_t cy = minY + static_cast<int32_t>(t); cy <= maxY; cy += static_cast<int32_t>(readerThreads)) {
					for (int32_t cx = minX; cx <= maxX; ++cx) {
						auto& rec = cells[static_cast<size_t>(cy - minY) * cellsW + (cx - minX)];
						if (ReadCellRecord(worldSpace, files, cx, cy, rec))
							cellsWithLand.fetch_add(1, std::memory_order_relaxed);
					}
				}

				for (auto* file : files)
					if (file)
						file->CloseTES(true);
			});
		}
		readers.clear();  // joins
	}

	if (cellsWithLand.load() == 0) {
		logger::info("[Shore Waves] [Field] {}: no cells with terrain, skipping", worldSpace->GetFormEditorID());
		return true;
	}

	// Tight bounds around cells that actually have terrain.
	int32_t tMinX = INT32_MAX, tMinY = INT32_MAX, tMaxX = INT32_MIN, tMaxY = INT32_MIN;
	for (int32_t cy = 0; cy < cellsH; ++cy)
		for (int32_t cx = 0; cx < cellsW; ++cx)
			if (cells[static_cast<size_t>(cy) * cellsW + cx].hasLand) {
				tMinX = std::min(tMinX, cx);
				tMaxX = std::max(tMaxX, cx);
				tMinY = std::min(tMinY, cy);
				tMaxY = std::max(tMaxY, cy);
			}

	const int32_t T = static_cast<int32_t>(kTexelsPerCell);
	const int32_t width = (tMaxX - tMinX + 1) * T;
	const int32_t height = (tMaxY - tMinY + 1) * T;
	const size_t count = static_cast<size_t>(width) * height;

	// Classification: 1 water, 0 land, -1 unknown. Bathymetry: water height minus land height.
	std::vector<int8_t> cls(count, -1);
	std::vector<float> bathy(count, 0.0f);
	uint32_t waterTexels = 0, landTexels = 0;
	for (int32_t cy = tMinY; cy <= tMaxY; ++cy) {
		for (int32_t cx = tMinX; cx <= tMaxX; ++cx) {
			const auto& rec = cells[static_cast<size_t>(cy) * cellsW + cx];
			if (!rec.hasLand)
				continue;
			for (int32_t j = 0; j < T; ++j) {
				for (int32_t i = 0; i < T; ++i) {
					const size_t idx = static_cast<size_t>((cy - tMinY) * T + j) * width + ((cx - tMinX) * T + i);
					const float depth = rec.waterHeight - rec.heights[j * T + i];
					const bool water = depth > 0.0f;
					cls[idx] = water ? 1 : 0;
					bathy[idx] = std::clamp(depth, -kBathyRange, kBathyRange);
					water ? ++waterTexels : ++landTexels;
				}
			}
		}
	}

	// Small detached land (sandbars, mounds, rocks the heightmap catches) must not act as a
	// shoreline: distance-to-nearest-land would ring every mound with converging crests. Flood
	// fill land components and reclassify the small ones as water for the distance field only;
	// their bathymetry stays, and the water mesh is clipped by the terrain anyway.
	std::vector<int8_t> shoreCls = cls;
	{
		std::vector<uint8_t> visited(count, 0);
		std::vector<size_t> stack, component;
		uint32_t dropped = 0;
		for (size_t seed = 0; seed < count; ++seed) {
			if (cls[seed] != 0 || visited[seed])
				continue;
			component.clear();
			stack.clear();
			stack.push_back(seed);
			visited[seed] = 1;
			while (!stack.empty()) {
				const size_t idx = stack.back();
				stack.pop_back();
				component.push_back(idx);
				const int32_t x = static_cast<int32_t>(idx % width);
				const int32_t y = static_cast<int32_t>(idx / width);
				const int32_t nx[4] = { x - 1, x + 1, x, x };
				const int32_t ny[4] = { y, y, y - 1, y + 1 };
				for (int n = 0; n < 4; ++n) {
					if (nx[n] < 0 || ny[n] < 0 || nx[n] >= width || ny[n] >= height)
						continue;
					const size_t nidx = static_cast<size_t>(ny[n]) * width + nx[n];
					if (cls[nidx] == 0 && !visited[nidx]) {
						visited[nidx] = 1;
						stack.push_back(nidx);
					}
				}
				if (component.size() > kMinLandTexels && stack.empty())
					break;
			}
			if (component.size() < kMinLandTexels) {
				for (size_t idx : component)
					shoreCls[idx] = 1;
				++dropped;
			}
		}
		logger::info("[Shore Waves] [Field] {}: {} small land components ignored for the shoreline", worldSpace->GetFormEditorID(), dropped);
	}

	// Distance to nearest land (for water texels) and to nearest water (for land texels).
	std::vector<float> toLand(count), toWater(count);
	for (size_t i = 0; i < count; ++i) {
		toLand[i] = shoreCls[i] == 0 ? 0.0f : kFarSquared;
		toWater[i] = shoreCls[i] == 1 ? 0.0f : kFarSquared;
	}
	DistanceTransform(toLand, width, height);
	DistanceTransform(toWater, width, height);

	const float maxRangeTexels = kMaxRange / static_cast<float>(kTexelSize);
	std::vector<float> sdf(count);
	for (size_t i = 0; i < count; ++i) {
		float d;
		if (shoreCls[i] == 1)
			d = std::sqrt(toLand[i]) - 0.5f;  // shoreline sits between the two texel centres
		else if (shoreCls[i] == 0)
			d = -(std::sqrt(toWater[i]) - 0.5f);
		else
			d = maxRangeTexels;  // unknown: treated as open water for the direction blur
		sdf[i] = std::clamp(d, -maxRangeTexels, maxRangeTexels);
	}

	// Smoothed direction toward land: negative gradient of the blurred signed distance.
	std::vector<float> blurred = sdf;
	BoxBlur(blurred, width, height, kDirBlurRadius);

	out.texels.resize(count);
	for (int32_t y = 0; y < height; ++y) {
		for (int32_t x = 0; x < width; ++x) {
			const size_t idx = static_cast<size_t>(y) * width + x;
			const float gx = blurred[static_cast<size_t>(y) * width + std::min(x + 1, width - 1)] - blurred[static_cast<size_t>(y) * width + std::max(x - 1, 0)];
			const float gy = blurred[static_cast<size_t>(std::min(y + 1, height - 1)) * width + x] - blurred[static_cast<size_t>(std::max(y - 1, 0)) * width + x];
			const float len = std::sqrt(gx * gx + gy * gy);
			float dx = 0.0f, dy = 0.0f;
			if (len > 1e-4f) {
				dx = -gx / len;
				dy = -gy / len;
			}
			Texel& t = out.texels[idx];
			t.dist = ToSnorm(sdf[idx] / maxRangeTexels);
			t.dirX = ToSnorm(dx);
			t.dirY = ToSnorm(dy);
			t.bathy = cls[idx] >= 0 ? ToSnorm(bathy[idx] / kBathyRange * 0.5f) : -32767;
		}
	}

	out.header.minCellX = minX + tMinX;
	out.header.minCellY = minY + tMinY;
	out.header.width = width;
	out.header.height = height;
	out.header.waterTexels = waterTexels;
	out.header.landTexels = landTexels;
	return true;
}
