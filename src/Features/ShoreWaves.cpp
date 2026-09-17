#include "ShoreWaves.h"

#include "Globals.h"
#include "I18n/I18n.h"
#include "State.h"
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
	CrestGlow)

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
		ImGui::Text("%s", T(TKEY("parallax_scale_tooltip"), "How far the wave height shifts the water detail under the camera. 0 disables the height cue."));
	}

	ImGui::SliderFloat(T(TKEY("normal_strength"), "Normal Strength"), &settings.NormalStrength, 0.0f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("normal_strength_tooltip"), "Tilt of the lighting normal along the wave slope. 0 disables the lighting cue."));
	}

	ImGui::SliderFloat(T(TKEY("crest_glow"), "Crest Translucency"), &settings.CrestGlow, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("crest_glow_tooltip"), "Lightens and greens the top of each crest as if lit through, strongest with the sun behind the wave."));
	}

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
		ImGui::Text("%s", T(TKEY("shore_foam_width_tooltip"), "Depth below which the constant waterline foam band appears. Uses the depth buffer, so it also hugs rocks and hulls."));
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
							  "Foam mask: white where foam would be drawn before the texture is applied."));
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

ShoreWaves::PerFrame ShoreWaves::GetCommonBufferData() const
{
	PerFrame data{};
	data.settings = settings;
	if (!loaded)
		data.settings.Enabled = 0;
	data.settings.WaveSharpness = std::max(data.settings.WaveSharpness, 1.0f);
	data.settings.WaveTrains = std::clamp(data.settings.WaveTrains, 1u, 3u);

	if (shoreField.IsReady()) {
		const auto& hdr = shoreField.GetActiveHeader();
		data.FieldOrigin = shoreField.GetOrigin();
		data.FieldSize = { static_cast<float>(hdr.width), static_cast<float>(hdr.height) };
		data.FieldTexelSize = static_cast<float>(hdr.texelSize);
		data.FieldMaxRange = hdr.maxRange;
		data.FieldValid = 1;
		data.FieldBathyRange = ShoreField::kBathyRange;
	}
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
}

void ShoreWaves::Prepass()
{
	RE::TESWorldSpace* worldSpace = nullptr;
	if (!Util::IsInterior()) {
		if (auto* tes = RE::TES::GetSingleton())
			worldSpace = tes->GetRuntimeData2().worldSpace;
	}
	shoreField.Update(worldSpace);

	auto context = globals::d3d::context;
	ID3D11ShaderResourceView* srvs[2] = { shoreField.GetSRV(), foamView.get() };
	static_assert(kFoamTextureSlot == kFieldTextureSlot + 1, "field and foam slots must be adjacent");
	context->PSSetShaderResources(kFieldTextureSlot, 2, srvs);
}
