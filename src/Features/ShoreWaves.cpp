#include "ShoreWaves.h"

#include "Globals.h"
#include "I18n/I18n.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#include <DDSTextureLoader.h>

#define I18N_KEY_PREFIX "feature.shore_waves."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ShoreWaves::Settings,
	Enabled,
	DebugMode,
	MaxDepth,
	Intensity,
	WavePeriod,
	WaveAmplitude,
	WaveSharpness,
	RefDepth,
	MinDepth,
	NoiseStrength,
	NoiseScale,
	ParallaxScale,
	NormalStrength,
	WaveTrains,
	FoamAmount,
	FoamScale,
	FoamDecay,
	ShoreFoamWidth,
	FoamSoftness,
	FoamBrightness,
	CrestGlow,
	EnableTessellation,
	MaxTessFactor,
	TessDistance)

void ShoreWaves::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enable"), "Enable Shore Waves"), (bool*)&settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_tooltip"), "Master toggle. Disable to compare against stock water."));
	}

	ImGui::SliderFloat(T(TKEY("intensity"), "Intensity"), &settings.Intensity, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("intensity_tooltip"), "Overall wave strength. Weather scaling multiplies this in a later phase."));
	}

	ImGui::SliderFloat(T(TKEY("max_depth"), "Max Depth"), &settings.MaxDepth, 32.0f, 2048.0f, "%.0f units");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("max_depth_tooltip"),
							  "Seabed depth at which shore waves have completely faded out.\n"
							  "70 units is about 1 metre. Deeper water is untouched."));
	}

	ImGui::SeparatorText(T(TKEY("wave_header"), "Wave Model"));

	ImGui::SliderFloat(T(TKEY("wave_period"), "Period"), &settings.WavePeriod, 2.0f, 16.0f, "%.1f s");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("wave_period_tooltip"), "Seconds between crests. Longer periods give longer, slower swells."));
	}

	ImGui::SliderFloat(T(TKEY("wave_amplitude"), "Amplitude"), &settings.WaveAmplitude, 0.0f, 60.0f, "%.1f units");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("wave_amplitude_tooltip"),
							  "Half the crest-to-trough height at the reference depth.\n"
							  "Waves grow toward the shore (Green's law) until they hit the breaking limit."));
	}

	ImGui::SliderFloat(T(TKEY("wave_sharpness"), "Crest Sharpness"), &settings.WaveSharpness, 1.0f, 6.0f, "%.1f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("wave_sharpness_tooltip"), "1 is a sine wave; higher values make narrow, peaked crests with flat troughs."));
	}

	ImGui::SliderFloat(T(TKEY("ref_depth"), "Reference Depth"), &settings.RefDepth, 20.0f, 1000.0f, "%.0f units");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("ref_depth_tooltip"), "Depth at which Amplitude applies. Shallower water grows the wave, deeper shrinks it."));
	}

	ImGui::SliderFloat(T(TKEY("min_depth"), "Min Depth"), &settings.MinDepth, 1.0f, 50.0f, "%.0f units");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("min_depth_tooltip"), "Floor on the depth used for wave speed, so crests do not bunch infinitely at the waterline."));
	}

	int trains = static_cast<int>(settings.WaveTrains);
	if (ImGui::SliderInt(T(TKEY("wave_trains"), "Wave Trains"), &trains, 1, 3))
		settings.WaveTrains = static_cast<uint32_t>(trains);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("wave_trains_tooltip"), "Number of summed wave trains with different periods. More trains break up the regular pattern."));
	}

	ImGui::SliderFloat(T(TKEY("noise_strength"), "Along-shore Wobble"), &settings.NoiseStrength, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("noise_strength_tooltip"), "Low-frequency phase noise along the shore so crests are not perfectly straight lines."));
	}

	ImGui::SliderFloat(T(TKEY("noise_scale"), "Wobble Scale"), &settings.NoiseScale, 0.0001f, 0.005f, "%.4f", ImGuiSliderFlags_Logarithmic);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("noise_scale_tooltip"), "Spatial frequency of the wobble. Smaller values give broader features."));
	}

	ImGui::SliderFloat(T(TKEY("parallax_scale"), "Parallax"), &settings.ParallaxScale, 0.0f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("parallax_scale_tooltip"), "How far the wave height shifts the water detail under the camera when the surface is flat. Unused while tessellation is active."));
	}

	ImGui::SliderFloat(T(TKEY("normal_strength"), "Normal Strength"), &settings.NormalStrength, 0.0f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("normal_strength_tooltip"), "Tilt of the lighting normal along the wave slope. 0 disables the lighting cue."));
	}

	ImGui::SliderFloat(T(TKEY("crest_glow"), "Crest Translucency"), &settings.CrestGlow, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("crest_glow_tooltip"), "Lightens the top of each crest as if lit through, strongest with the sun behind the wave."));
	}

	ImGui::SeparatorText(T(TKEY("tess_header"), "Tessellation"));

	ImGui::Checkbox(T(TKEY("enable_tessellation"), "Displace Water Surface"), (bool*)&settings.EnableTessellation);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_tessellation_tooltip"),
							  "Subdivides the water mesh near the shore and lifts it with the waves, so crests get real\n"
							  "silhouettes instead of shading alone. Off falls back to the flat, parallax-shaded surface."));
	}

	ImGui::SliderFloat(T(TKEY("max_tess_factor"), "Max Subdivision"), &settings.MaxTessFactor, 1.0f, 64.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("max_tess_factor_tooltip"), "Subdivisions per patch edge at the waterline next to the camera. Higher is smoother and costs more triangles."));
	}

	ImGui::SliderFloat(T(TKEY("tess_distance"), "Subdivision Distance"), &settings.TessDistance, 1000.0f, 20000.0f, "%.0f units");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("tess_distance_tooltip"), "Camera distance at which subdivision has fallen back to flat patches."));
	}

	ImGui::Text("%s: %u", T(TKEY("tess_draws"), "Tessellated water draws last frame"), tessellatedDrawsShown);

	ImGui::SeparatorText(T(TKEY("foam_header"), "Foam"));

	ImGui::SliderFloat(T(TKEY("foam_amount"), "Foam Amount"), &settings.FoamAmount, 0.0f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("foam_amount_tooltip"), "Strength of foam from breaking crests and the wash behind them. 0 leaves only the waterline band."));
	}

	ImGui::SliderFloat(T(TKEY("foam_decay"), "Wash Decay"), &settings.FoamDecay, 0.5f, 15.0f, "%.1f s");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("foam_decay_tooltip"), "Seconds for the foam trailing a crest to fade."));
	}

	ImGui::SliderFloat(T(TKEY("shore_foam_width"), "Waterline Foam"), &settings.ShoreFoamWidth, 0.0f, 100.0f, "%.0f units");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("shore_foam_width_tooltip"), "Depth band of the waterline foam. It peaks at half this depth and is zero at the waterline itself. Uses the depth buffer, so it also hugs rocks and hulls."));
	}

	ImGui::SliderFloat(T(TKEY("foam_scale"), "Foam Texture Scale"), &settings.FoamScale, 64.0f, 1024.0f, "%.0f units");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("foam_scale_tooltip"), "World size of one foam texture tile."));
	}

	ImGui::SliderFloat(T(TKEY("foam_softness"), "Foam Softness"), &settings.FoamSoftness, 0.05f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("foam_softness_tooltip"), "Lower values give crisp, lacy foam edges; higher values give a smooth milky blend."));
	}

	ImGui::SliderFloat(T(TKEY("foam_brightness"), "Foam Brightness"), &settings.FoamBrightness, 0.2f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("foam_brightness_tooltip"), "Foam albedo. It is lit by the sun and sky like any diffuse surface, so it darkens at night."));
	}

	ImGui::SeparatorText(T(TKEY("field_header"), "Shore Distance Field"));

	{
		const auto& hdr = shoreField.GetActiveHeader();
		const char* statusText = "none";
		switch (shoreField.GetStatus()) {
		case ShoreField::Status::Loading:
			statusText = "loading / baking";
			break;
		case ShoreField::Status::Ready:
			statusText = "ready";
			break;
		case ShoreField::Status::Failed:
			statusText = "failed";
			break;
		default:
			break;
		}
		ImGui::Text("%s: %s", T(TKEY("field_status"), "Status"), statusText);
		if (shoreField.IsReady()) {
			ImGui::Text("%s: %s, %d x %d texels (%u units each), %u water / %u land",
				T(TKEY("field_worldspace"), "Worldspace"), shoreField.GetActiveEditorID().c_str(),
				hdr.width, hdr.height, hdr.texelSize, hdr.waterTexels, hdr.landTexels);
		} else if (shoreField.GetStatus() == ShoreField::Status::Failed) {
			ImGui::TextColored(globals::menu->GetSettings().Theme.StatusPalette.Error, "%s", shoreField.GetLastError().c_str());
		}
		if (ImGui::Button(T(TKEY("field_rebuild"), "Rebuild field for this worldspace")))
			shoreField.RequestRebuild();
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("field_rebuild_tooltip"),
								  "Deletes the cached field for the current worldspace and bakes it again from the\n"
								  "loaded plugins. Caches rebuild automatically when the load order changes."));
		}
	}

	ImGui::SeparatorText(T(TKEY("debug_header"), "Debug"));

	const char* debugModes[] = {
		T(TKEY("debug_off"), "Off"),
		T(TKEY("debug_depth"), "Screen: depth ramp"),
		T(TKEY("debug_direction"), "Screen: shore direction (hue)"),
		T(TKEY("debug_both"), "Screen: depth x direction"),
		T(TKEY("debug_field_distance"), "Field: signed distance"),
		T(TKEY("debug_field_direction"), "Field: shore direction (hue)"),
		T(TKEY("debug_field_bathymetry"), "Field: bathymetry"),
		T(TKEY("debug_wave_height"), "Wave: height"),
		T(TKEY("debug_wave_breaking"), "Wave: breaking"),
		T(TKEY("debug_foam_mask"), "Foam: mask"),
		T(TKEY("debug_tess_factor"), "Tess: subdivision factor"),
		T(TKEY("debug_tess_world"), "Tess: domain world XY vs pixel"),
	};
	int debugMode = static_cast<int>(settings.DebugMode);
	if (ImGui::Combo(T(TKEY("debug_mode"), "Debug View"), &debugMode, debugModes, IM_ARRAYSIZE(debugModes))) {
		settings.DebugMode = static_cast<uint32_t>(debugMode);
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("debug_mode_tooltip"),
							  "Depth ramps: red = 0, yellow = 1/4, green = 1/2, blue = Max Depth, dark = deeper.\n"
							  "Direction views: hue encodes the world XY direction toward the shore\n"
							  "(red = +X east, green = +Y north, cyan = -X, purple = -Y); grey where undefined.\n"
							  "Field distance: same ramp over 0..2048 units on the water side, brown on land, magenta = no data.\n"
							  "Wave height: grey = flat, white = crest, black = trough. Breaking: red where crests are at the breaking limit.\n"
							  "Foam mask: white where foam would be drawn before the texture is applied.\n"
							  "Tess factor: grey ramp of the patch subdivision factor, white = 64, black = none.\n"
							  "Tess world XY: checkerboards from both stages; yellow/black = agree, pure red or green = disagree."));
	}
}

#undef I18N_KEY_PREFIX

void ShoreWaves::LoadSettings(json& o_json)
{
	settings = o_json;
}

void ShoreWaves::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ShoreWaves::RestoreDefaultSettings()
{
	settings = {};
}

bool ShoreWaves::IsTessellationActive() const
{
	return loaded && settings.Enabled && settings.EnableTessellation && shoreField.IsReady() && drawHookInstalled;
}

ShoreWaves::PerFrame ShoreWaves::GetCommonBufferData() const
{
	PerFrame data{};
	data.settings = settings;
	if (!loaded)
		data.settings.Enabled = 0;
	data.settings.WaveSharpness = std::max(data.settings.WaveSharpness, 1.0f);
	data.settings.WaveTrains = std::clamp(data.settings.WaveTrains, 1u, 3u);
	data.settings.MaxTessFactor = std::clamp(data.settings.MaxTessFactor, 1.0f, 64.0f);

	if (shoreField.IsReady()) {
		const auto& hdr = shoreField.GetActiveHeader();
		data.FieldOrigin = shoreField.GetOrigin();
		data.FieldSize = { static_cast<float>(hdr.width), static_cast<float>(hdr.height) };
		data.FieldTexelSize = static_cast<float>(hdr.texelSize);
		data.FieldMaxRange = hdr.maxRange;
		data.FieldValid = 1;
		data.FieldBathyRange = ShoreField::kBathyRange;
	}
	data.TessellationActive = IsTessellationActive() ? 1u : 0u;
	data.PreviousTimer = previousTimer;
	return data;
}

void ShoreWaves::SetupResources()
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;
	const HRESULT hr = DirectX::CreateDDSTextureFromFile(device, context, L"Data\\Shaders\\ShoreWaves\\foam.dds", nullptr, foamView.put());
	if (FAILED(hr) || !foamView) {
		logger::error("[Shore Waves] Failed to load Data/Shaders/ShoreWaves/foam.dds (HRESULT {:#x}); foam will not render", static_cast<uint32_t>(hr));
		foamView = nullptr;
	} else {
		Util::SetResourceName(foamView.get(), "ShoreWaves::FoamTexture SRV");
	}

	{
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		if (SUCCEEDED(device->CreateSamplerState(&samplerDesc, tessSampler.put())) && tessSampler) {
			Util::SetResourceName(tessSampler.get(), "ShoreWaves::FieldSampler");
		} else {
			logger::error("[Shore Waves] Failed to create the field sampler; tessellation disabled");
		}
	}

	if (!drawHookInstalled && context && tessSampler) {
		// Patches the DrawIndexed implementation itself (Detours), so every context shares it;
		// the thunk only acts on the immediate context with a flagged water draw.
		stl::detour_vfunc<12, ID3D11DeviceContext_DrawIndexed>(context);
		drawHookInstalled = true;
		logger::info("[Shore Waves] Installed DrawIndexed hook for water tessellation");
	}
}

void ShoreWaves::PostPostLoad()
{
	stl::write_vfunc<0x6, BSWaterShader_SetupGeometry>(RE::VTABLE_BSWaterShader[0]);
	logger::info("[Shore Waves] Installed hooks");
}

void ShoreWaves::ClearShaderCache()
{
	tessellationShaders.clear();
}

ShoreWaves::TessellationShaders* ShoreWaves::GetTessellationShaders(uint32_t vertexDescriptor)
{
	auto it = tessellationShaders.find(vertexDescriptor);
	if (it != tessellationShaders.end())
		return &it->second;

	TessellationShaders stages;
	const auto defines = SIE::ShaderCache::GetWaterShaderDefinesForDescriptor(vertexDescriptor);

	if (auto* hull = reinterpret_cast<ID3D11HullShader*>(Util::CompileShader(L"Data\\Shaders\\Water.hlsl", defines, "hs_5_0")))
		stages.hull.attach(hull);
	if (auto* domain = reinterpret_cast<ID3D11DomainShader*>(Util::CompileShader(L"Data\\Shaders\\Water.hlsl", defines, "ds_5_0")))
		stages.domain.attach(domain);

	stages.failed = !stages.hull || !stages.domain;
	if (stages.failed) {
		logger::error("[Shore Waves] Hull/domain shader compilation failed for water descriptor {:#x}; that permutation stays flat", vertexDescriptor);
		stages.hull = nullptr;
		stages.domain = nullptr;
	} else {
		logger::info("[Shore Waves] Compiled tessellation shaders for water descriptor {:#x}", vertexDescriptor);
	}

	return &tessellationShaders.emplace(vertexDescriptor, std::move(stages)).first->second;
}

void ShoreWaves::BSWaterShader_SetupGeometry::thunk(RE::BSShader* shader, RE::BSRenderPass* pass, uint32_t flags)
{
	auto& self = globals::features::shoreWaves;
	self.pendingTessellation = false;

	if (self.IsTessellationActive() && pass && pass->geometry && globals::state->currentShader == shader) {
		using Flags = SIE::ShaderCache::WaterShaderFlags;
		const uint32_t descriptor = globals::state->modifiedVertexDescriptor;
		const uint32_t technique = (descriptor >> 11) & 0xF;
		const bool mainTechnique = technique == 0;  // SPECULAR with no point lights: the normal water pass
		const bool stencilTechnique = technique == static_cast<uint32_t>(SIE::ShaderCache::WaterShaderTechniques::Stencil);  // water mask + motion vectors
		const bool hasDepth = descriptor & static_cast<uint32_t>(Flags::Depth);
		const bool fakeDepth = descriptor & static_cast<uint32_t>(Flags::VertexAlphaDepth);
		const bool flowmap = descriptor & static_cast<uint32_t>(Flags::Flowmap);
		const bool interior = descriptor & static_cast<uint32_t>(Flags::Interior);

		const bool eligible = (mainTechnique && hasDepth && !fakeDepth) || stencilTechnique;
		if (eligible && !flowmap && !interior) {
			const auto& bound = pass->geometry->worldBound;
			const float distance = bound.center.GetDistance(Util::GetEyePosition()) - bound.radius;
			if (distance < self.settings.TessDistance) {
				self.pendingTessellation = true;
				self.pendingDescriptor = descriptor;
			}
		}
	}

	func(shader, pass, flags);
}

void WINAPI ShoreWaves::ID3D11DeviceContext_DrawIndexed::thunk(ID3D11DeviceContext* This, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
{
	auto& self = globals::features::shoreWaves;

	if (self.pendingTessellation && This == globals::d3d::context) {
		auto* shader = globals::state->currentShader;
		if (shader && shader->shaderType.get() == RE::BSShader::Type::Water) {
			auto* stages = self.GetTessellationShaders(self.pendingDescriptor);
			if (stages && !stages->failed) {
				// The domain shader re-projects with the vertex stage's own PerGeometry buffer, so
				// mirror b0-b2 for this draw. The camera data (FrameBuffer, b12) is the engine's
				// PerFrame buffer, which the vertex stage does not carry at slot 12: bind the engine's
				// object directly, as the deferred passes do for compute. Reading it from the vertex
				// stage left CameraPosAdjust at zero and the waves followed the camera.
				ID3D11Buffer* geometryBuffers[3] = {};
				This->VSGetConstantBuffers(0, 3, geometryBuffers);
				This->DSSetConstantBuffers(0, 3, geometryBuffers);
				ID3D11Buffer* perFrame = *globals::game::perFrame.get();
				This->DSSetConstantBuffers(12, 1, &perFrame);
				This->HSSetConstantBuffers(12, 1, &perFrame);

				D3D11_PRIMITIVE_TOPOLOGY previousTopology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
				This->IAGetPrimitiveTopology(&previousTopology);

				This->HSSetShader(stages->hull.get(), nullptr, 0);
				This->DSSetShader(stages->domain.get(), nullptr, 0);
				This->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

				func(This, IndexCount, StartIndexLocation, BaseVertexLocation);

				This->IASetPrimitiveTopology(previousTopology);
				This->HSSetShader(nullptr, nullptr, 0);
				This->DSSetShader(nullptr, nullptr, 0);

				for (auto* buffer : geometryBuffers)
					if (buffer)
						buffer->Release();

				self.tessellatedDraws++;
				return;
			}
		}
	}

	func(This, IndexCount, StartIndexLocation, BaseVertexLocation);
}

void ShoreWaves::Prepass()
{
	RE::TESWorldSpace* worldSpace = nullptr;
	if (!Util::IsInterior()) {
		if (auto* tes = RE::TES::GetSingleton())
			worldSpace = tes->GetRuntimeData2().worldSpace;
	}
	shoreField.Update(worldSpace);

	tessellatedDrawsShown = tessellatedDraws;
	tessellatedDraws = 0;
	pendingTessellation = false;

	// Stencil-pass motion vectors need the height the previous frame used.
	const float timer = globals::state->timer;
	if (timer != lastSeenTimer) {
		previousTimer = lastSeenTimer;
		lastSeenTimer = timer;
	}

	auto context = globals::d3d::context;
	ID3D11ShaderResourceView* srvs[2] = { shoreField.GetSRV(), foamView.get() };
	static_assert(kFoamTextureSlot == kFieldTextureSlot + 1, "field and foam slots must be adjacent");
	context->PSSetShaderResources(kFieldTextureSlot, 2, srvs);

	// Hull and domain stages read the field and the shared data; nothing else uses those stages.
	ID3D11ShaderResourceView* fieldSRV = shoreField.GetSRV();
	context->HSSetShaderResources(kFieldTextureSlot, 1, &fieldSRV);
	context->DSSetShaderResources(kFieldTextureSlot, 1, &fieldSRV);
	ID3D11SamplerState* sampler = tessSampler.get();
	context->HSSetSamplers(kTessSamplerSlot, 1, &sampler);
	context->DSSetSamplers(kTessSamplerSlot, 1, &sampler);
	ID3D11Buffer* sharedBuffers[2] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->HSSetConstantBuffers(5, 2, sharedBuffers);
	context->DSSetConstantBuffers(5, 2, sharedBuffers);
}
