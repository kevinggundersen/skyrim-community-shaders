#pragma once

#include "ShoreWaves/ShoreField.h"

#include <unordered_map>
#include <winrt/base.h>

/**
 * @brief Shoreline waves: crests that shoal, break and foam where water meets land.
 *
 * Phase 1: per-pixel vertical water depth and a screen-space shore direction estimate in the
 * water shader, plus debug views.
 * Phase 2: a per-worldspace shoreline distance field baked from plugin terrain data
 * (ShoreField), bound at t66, giving a camera-independent shore distance, direction and
 * bathymetry.
 * Phase 3: the wave model (ShoreWaveModel.hlsli). Crests advance toward land with
 * shallow-water speed, grow by Green's law, cap at the breaking ratio, and reach the pixel as a
 * reoriented normal (and a parallax offset when the geometry is flat).
 * Phase 3b: tessellation. The main water technique is drawn as 3-control-point patches with a
 * hull shader that subdivides near the shore and camera, and a domain shader that displaces the
 * surface with the same wave function, so crests get real silhouettes.
 * Phase 4: foam. Breaking crests and their trailing wash from the wave model, plus a waterline
 * band from the depth buffer, drawn with a tiling foam texture (t67) and lit as a diffuse surface.
 * Later phases add terrain-side swash.
 */
struct ShoreWaves : Feature
{
	virtual inline std::string GetName() override { return "Shore Waves"; }
	virtual std::string GetDisplayName() override { return T("feature.shore_waves.name", "Shore Waves"); }
	/** @brief Returns the short identifier used for file paths, JSON and logging. */
	virtual inline std::string GetShortName() override { return "ShoreWaves"; }
	virtual inline std::string_view GetShaderDefineName() override { return "SHORE_WAVES"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kWater; }

	/** @brief Returns a summary description and list of key features for the UI. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.shore_waves.description", "Adds shoreline waves to coastal water: crests that run parallel to the shore, grow and bunch up in the shallows, then break into foam."),
			{ T("feature.shore_waves.key_feature_1", "Depth-aware wave crests aligned with the coastline"),
				T("feature.shore_waves.key_feature_2", "Shoaling and breaking driven by shallow-water physics"),
				T("feature.shore_waves.key_feature_3", "Tessellated, displaced water surface near the shore"),
				T("feature.shore_waves.key_feature_4", "Breaking foam and a shore foam line"),
				T("feature.shore_waves.key_feature_5", "Works on every coast without hand-placed meshes") } };
	};

	virtual inline bool HasShaderDefine(RE::BSShader::Type type) override { return type == RE::BSShader::Type::Water; }

	static constexpr uint32_t kFieldTextureSlot = 66;
	static constexpr uint32_t kFoamTextureSlot = 67;
	static constexpr uint32_t kTessSamplerSlot = 0;  // hull/domain stage sampler for the field

	enum class DebugMode : uint32_t
	{
		Off = 0,
		ScreenDepth = 1,
		ScreenDirection = 2,
		ScreenDepthAndDirection = 3,
		FieldDistance = 4,
		FieldDirection = 5,
		FieldBathymetry = 6,
		WaveHeight = 7,
		WaveBreaking = 8,
		FoamMask = 9,
	};

	/** User settings, persisted to JSON. Layout mirrors the first 96 bytes of ShoreWavesSettings in SharedData.hlsli. */
	struct Settings
	{
		uint32_t Enabled = 1;
		uint32_t DebugMode = 0;      // ShoreWaves::DebugMode
		float MaxDepth = 400.0f;     // Bathymetry depth (game units) at which the effect has fully faded out
		float Intensity = 1.0f;
		float WavePeriod = 6.0f;     // seconds between crests in deep water
		float WaveAmplitude = 20.0f; // units at RefDepth; crest-to-trough is twice this
		float WaveSharpness = 3.0f;  // >= 1; higher makes narrow, peaked crests
		float RefDepth = 200.0f;     // depth at which WaveAmplitude applies
		float MinDepth = 6.0f;       // shallowest depth used for wave speed
		float NoiseStrength = 0.25f; // along-shore phase wobble in wave periods
		float NoiseScale = 0.0006f;  // 1 / world units for the wobble noise (~1700 units per feature)
		float ParallaxScale = 1.0f;
		float NormalStrength = 2.0f;
		uint32_t WaveTrains = 3;     // 1..3 summed trains
		float FoamAmount = 1.0f;     // multiplier on wave-driven foam
		float FoamScale = 384.0f;    // world units per foam texture tile (~5.5 m)
		float FoamDecay = 2.0f;      // seconds for the wash behind a crest to fade
		float ShoreFoamWidth = 16.0f; // screen-depth band (units) of the waterline foam; peaks at half
		float FoamSoftness = 0.3f;   // threshold softness of the foam pattern
		float FoamBrightness = 0.6f; // foam albedo multiplier
		float CrestGlow = 0.0f;      // crest translucency strength (off by default; read as unnatural)
		uint32_t EnableTessellation = 1;  // displace the water mesh with the waves (hull/domain shaders)
		float MaxTessFactor = 48.0f;      // subdivisions per patch edge at the waterline next to the camera
		float TessDistance = 6000.0f;     // camera distance (units) at which tessellation has fallen to 1
	};
	STATIC_ASSERT_ALIGNAS_16(Settings);

	/** Packed into SharedData::FeatureData (b6); layout must match ShoreWavesSettings in SharedData.hlsli. */
	struct alignas(16) PerFrame
	{
		Settings settings;
		float2 FieldOrigin;    // world XY of the min corner of field texel (0, 0)
		float2 FieldSize;      // field dimensions in texels
		float FieldTexelSize;  // world units per texel
		float FieldMaxRange;   // world units represented by a stored distance of 1.0
		uint32_t FieldValid;   // 1 when a field texture is bound for the active worldspace
		float FieldBathyRange; // world units represented by a stored bathymetry of 0.5
		uint32_t TessellationActive;  // 1 when water draws are being tessellated this frame
		float pad1[3];
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);

	Settings settings;
	ShoreField shoreField;
	winrt::com_ptr<ID3D11ShaderResourceView> foamView;
	winrt::com_ptr<ID3D11SamplerState> tessSampler;

	/** Hull + domain shader pair compiled for one water vertex-shader descriptor. */
	struct TessellationShaders
	{
		winrt::com_ptr<ID3D11HullShader> hull;
		winrt::com_ptr<ID3D11DomainShader> domain;
		bool failed = false;
	};
	std::unordered_map<uint32_t, TessellationShaders> tessellationShaders;

	// Set by the water SetupGeometry hook for the pass about to be drawn, consumed by the
	// DrawIndexed hook. Render-thread only.
	bool pendingTessellation = false;
	uint32_t pendingDescriptor = 0;
	uint32_t tessellatedDraws = 0;       // this frame
	uint32_t tessellatedDrawsShown = 0;  // last frame, for the panel
	bool drawHookInstalled = false;

	/** @brief Loads the tiling foam texture, creates the field sampler, installs the draw hook. */
	virtual void SetupResources() override;
	/** @brief Installs the water SetupGeometry hook. */
	virtual void PostPostLoad() override;
	/** @brief Drops compiled hull/domain shaders so they recompile from disk. */
	virtual void ClearShaderCache() override;

	/** @brief Returns the block written to the shared feature constant buffer this frame. */
	PerFrame GetCommonBufferData() const;

	/** @brief Tracks the active worldspace, updates the field, and binds its texture. */
	virtual void Prepass() override;

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	/** @brief Whether water draws are tessellated this frame (setting on, field ready, feature loaded). */
	bool IsTessellationActive() const;
	/** @brief Returns the hull/domain pair for a water vertex descriptor, compiling on first use. */
	TessellationShaders* GetTessellationShaders(uint32_t vertexDescriptor);

	/** @brief Water shader geometry setup: decides whether the coming draw is tessellated. */
	struct BSWaterShader_SetupGeometry
	{
		static void thunk(RE::BSShader* shader, RE::BSRenderPass* pass, uint32_t flags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief D3D11 DrawIndexed: wraps flagged water draws with hull/domain stages and a patch topology. */
	struct ID3D11DeviceContext_DrawIndexed
	{
		static void WINAPI thunk(ID3D11DeviceContext* This, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation);
		static inline REL::Relocation<decltype(thunk)> func;
	};
};
