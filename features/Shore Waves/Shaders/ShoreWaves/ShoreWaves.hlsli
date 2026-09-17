// Shore Waves: shore data and the wave model for the water pixel shader.
//
// Included from Water.hlsl only when SHORE_WAVES_ACTIVE is defined, i.e. the DEPTH permutation
// without VERTEX_ALPHA_DEPTH, above water, not LOD. Relies on Water.hlsl's DepthTex/DepthSampler
// declarations and on SharedData/FrameBuffer/Random being included first.
//
// Sources of shore information:
//  - Screen space (Phase 1 prototype): vertical depth from the depth buffer, direction from its
//    screen derivatives. Depth is used for foam near objects; direction is kept for comparison.
//  - Shore distance field (Phase 2): per-worldspace texture baked from plugin terrain, bound at
//    t66. Signed distance to the shoreline (positive in water), a smoothed direction toward
//    land, and bathymetry (water height minus seabed height). Camera and object independent.
//
// Wave model (Phase 3): crests advance toward land along the field direction. Their phase
// accumulates along the shore distance with the shallow-water wavenumber, so crests bunch up as
// the seabed rises; amplitude follows Green's law and caps at the 0.78 height-to-depth breaking
// ratio. The result reaches the pixel as a world-space parallax offset applied to the normal
// map UVs and as a reoriented lighting normal. No geometry moves.

Texture2D<float4> ShoreFieldTex : register(t66);
Texture2D<float4> ShoreFoamTex : register(t67);  // R foam pattern, G breakup mask, B fine detail (tiling, see tools/make_foam_texture.py)

#include "ShoreWaves/ShoreWaveModel.hlsli"

namespace ShoreWaves
{
	struct ShoreData
	{
		float depth;       // Screen-space vertical water depth in game units (water surface Z minus scene Z).
		float2 shoreDir;   // Screen-space estimate: world XY unit vector toward decreasing depth. Zero when undefined.
		float dirValid;    // 1 where shoreDir is meaningful.
		float fade;        // 1 at zero screen depth, 0 at MaxDepth and beyond.
		float fieldDist;   // Signed distance to the shoreline in game units from the field, positive in water.
		float2 fieldDir;   // Field direction toward land, unit length or zero.
		float fieldDepth;  // Field bathymetry: water height minus seabed height, positive in water.
		float fieldValid;  // 1 where the field has terrain data for this position.
	};

	// Per-invocation results consumed by GetWaterNormal, set by PrepareWaveSurface in main().
	static WaveData g_wave = (WaveData)0;
	static float2 g_waveParallaxOffset = 0.0.xx;  // world XY offset to sample the normal maps at
	static float3 g_waveNormal = float3(0.0, 0.0, 1.0);
	// Foam texture coordinates and their screen derivatives, computed in uniform control flow so
	// SampleGrad can run inside the foam branch.
	static float2 g_foamUV = 0.0.xx;
	static float2 g_foamUVddx = 0.0.xx;
	static float2 g_foamUVddy = 0.0.xx;
	static float2 g_foamMaskUV = 0.0.xx;
	static const float FoamMaskScaleRatio = 3.1;  // breakup mask tiles this many times larger than the pattern

	// Raw depth at or beyond this is "nothing rendered behind the water" (clear value, folded far water).
	// Same margin as HorizonFix::EmptyDepthThreshold; defined here so the check also works without HorizonFix.
	static const float EmptyDepthThreshold = 1.0 - 64.0 / 16777216.0;

	// ---------------------------------------------------------------- Screen-space shore data

	float3 ReconstructScenePosition(float2 screenPosition, float2 screenUV, out float rawDepth)
	{
		rawDepth = DepthTex.Load(float3(screenPosition, 0)).x;
		float4 positionCS = float4((screenUV * 2.0 - 1.0) * float2(1.0, -1.0), rawDepth, 1.0);
		float4 positionWS = mul(FrameBuffer::CameraViewProjInverse, positionCS);
		return positionWS.xyz / positionWS.w;
	}

	float GetVerticalDepth(float3 waterPositionWS, float2 screenPosition, float2 screenUV)
	{
		float rawDepth;
		float3 scenePositionWS = ReconstructScenePosition(screenPosition, screenUV, rawDepth);
		float depth = waterPositionWS.z - scenePositionWS.z;
		if (rawDepth >= EmptyDepthThreshold)
			depth = 1e6;
		return depth;
	}

	// Must be called from uniform control flow (uses ddx/ddy).
	float2 EstimateShoreDirection(float depth, float3 waterPositionWS, out float valid)
	{
		float gx = ddx(depth);
		float gy = ddy(depth);
		float2 px = ddx(waterPositionWS.xy);
		float2 py = ddy(waterPositionWS.xy);
		float det = px.x * py.y - px.y * py.x;
		float2 grad = float2(py.y * gx - px.y * gy, -py.x * gx + px.x * gy) / det;
		float gradLen = length(grad);
		valid = (abs(det) > 1e-6 && gradLen > 1e-4 && gradLen < 0.5) ? 1.0 : 0.0;
		return valid ? -grad / gradLen : 0.0.xx;
	}

	// ---------------------------------------------------------------- Field sampling

	void SampleField(float2 worldXY, out float dist, out float2 dir, out float depth, out float valid)
	{
		FieldSample f = SampleShoreField(ShoreFieldTex, LinearSampler, worldXY);
		dist = f.dist;
		dir = f.dir;
		depth = f.depth;
		valid = f.valid;
	}

	ShoreData GetShoreData(float3 waterPositionWS, float2 screenPosition, float2 screenUV)
	{
		ShoreData data;
		data.depth = GetVerticalDepth(waterPositionWS, screenPosition, screenUV);
		data.shoreDir = EstimateShoreDirection(data.depth, waterPositionWS, data.dirValid);
		data.fade = saturate(1.0 - data.depth / max(SharedData::shoreWavesSettings.MaxDepth, 1.0));
		SampleField(waterPositionWS.xy + FrameBuffer::CameraPosAdjust.xy, data.fieldDist, data.fieldDir, data.fieldDepth, data.fieldValid);
		return data;
	}

	// Evaluates the wave at this pixel and stores the parallax offset and normal for GetWaterNormal.
	void PrepareWaveSurface(ShoreData sd, float3 waterPositionWS)
	{
		float2 worldXY = waterPositionWS.xy + FrameBuffer::CameraPosAdjust.xy;
		float t = SharedData::Timer;

		// Foam UVs and derivatives first, unconditionally: the shoreline foam band works from the
		// depth buffer even where the wave model is inactive, and ddx/ddy need uniform flow.
		float foamScale = max(SharedData::shoreWavesSettings.FoamScale, 16.0);
		g_foamUV = worldXY / foamScale;
		g_foamUVddx = ddx(g_foamUV);
		g_foamUVddy = ddy(g_foamUV);
		// The breakup mask drifts toward land at a fraction of the wave speed.
		g_foamMaskUV = (worldXY + sd.fieldDir * (t * 45.0)) / (foamScale * FoamMaskScaleRatio);

		FieldSample f;
		f.dist = sd.fieldDist;
		f.dir = sd.fieldDir;
		f.depth = sd.fieldDepth;
		f.valid = sd.fieldValid;
		g_wave = EvaluateWaves(f, worldXY, t);
		if (g_wave.active < 0.5)
			return;

		// A point raised by h is seen where the view ray crosses z = h: shift the sampled
		// position back along the ray's XY direction. Clamp the grazing-angle blow-up.
		float3 v = normalize(waterPositionWS);
		float viewDown = max(-v.z, 0.15);
		g_waveParallaxOffset = -g_wave.height * (v.xy / viewDown) * SharedData::shoreWavesSettings.ParallaxScale;
		if (SharedData::shoreWavesSettings.TessellationActive)
			g_waveParallaxOffset = 0.0.xx;

		float2 s = g_wave.slope * SharedData::shoreWavesSettings.NormalStrength;
		g_waveNormal = normalize(float3(-s, 1.0));
	}

	float3 ApplyWaveNormal(float3 detailNormal)
	{
		if (g_wave.active < 0.5)
			return detailNormal;
		return normalize(ReorientNormal(g_waveNormal, detailNormal));
	}

	// ---------------------------------------------------------------- Foam

	// 0..1 foam source: breaking crests and their wash from the wave model, plus a thin band at
	// the waterline from the depth buffer so foam also hugs rocks, piers and hulls.
	float GetFoamMask(ShoreData sd)
	{
		if (!SharedData::shoreWavesSettings.Enabled)
			return 0.0;
		float wave = g_wave.foam * SharedData::shoreWavesSettings.FoamAmount;
		// Waterline band: a bump that peaks at half the width and is zero at the waterline
		// itself, so it never ends on the hard line where the terrain clips the water mesh.
		float shore = 0.0;
		if (sd.depth >= 0.0) {
			float t = saturate(sd.depth / max(SharedData::shoreWavesSettings.ShoreFoamWidth, 1.0));
			shore = 4.0 * t * (1.0 - t);
		}
		// It breathes with the surf instead of sitting there permanently.
		shore *= SharedData::shoreWavesSettings.Intensity * lerp(0.3, 1.0, saturate(g_wave.foam * 2.0));
		return saturate(wave + shore * 0.6);
	}

	// Composites foam over the lit water colour. Foam is a diffuse white surface: sun by N.L
	// with the water's shadow, plus ambient attenuated by the same sky visibility the water
	// uses, then the same distance fog as the water, so it never glows at night. A normal derived
	// from the foam pattern and a small sun sparkle keep it from reading as a flat decal.
	float3 ApplyFoam(float3 color, ShoreData sd, float3 normal, float3 viewDirection, float shadow, float ambientVisibility, float3 fogColor, float fogFactor)
	{
		float mask = GetFoamMask(sd);
		if (mask <= 0.002)
			return color;

		float4 pattern = ShoreFoamTex.SampleGrad(LinearSampler, g_foamUV, g_foamUVddx, g_foamUVddy);
		float breakup = ShoreFoamTex.SampleGrad(LinearSampler, g_foamMaskUV, g_foamUVddx / FoamMaskScaleRatio, g_foamUVddy / FoamMaskScaleRatio).y;

		// Dense foam only where the mask is high; the breakup mask carves holes so it is never a
		// continuous band. Sparse veins as the mask falls off.
		float value = pattern.x * lerp(0.3, 1.5, breakup) + 0.2 * mask;
		float alpha = saturate((value - (1.0 - mask)) / max(SharedData::shoreWavesSettings.FoamSoftness, 0.02));
		alpha *= lerp(0.7, 1.0, pattern.z) * 0.92;  // thin foam lets the water show through
		// Fade out over the last few centimetres of depth so foam dissolves before the terrain
		// clips the water mesh, instead of ending on that hard line.
		alpha *= smoothstep(0.0, 14.0, sd.depth);
		if (alpha <= 0.001)
			return color;

		// Height-field normal from the pattern: two extra texels along U and V.
		float2 texelStep = float2(2.0 / 512.0, 0.0);
		float hx = ShoreFoamTex.SampleGrad(LinearSampler, g_foamUV + texelStep.xy, g_foamUVddx, g_foamUVddy).x - pattern.x;
		float hy = ShoreFoamTex.SampleGrad(LinearSampler, g_foamUV + texelStep.yx, g_foamUVddx, g_foamUVddy).x - pattern.x;
		float3 foamNormal = normalize(normal + float3(-hx, -hy, 0.0) * 3.0);

		// Same pipeline as Lighting.hlsl: albedo through Color::Diffuse, sun through
		// Color::DirectionalLight (inside GetDirectionalLighting), ambient from the sky probe,
		// accumulated as irradiance and converted once at the end. Under Linear Lighting and PBR
		// this is what keeps foam on the same footing as the sand next to it.
		float3 L = SharedData::DirLightDirection.xyz;
		float NdotL = saturate(dot(L, foamNormal));
		float3 dirLight = ShadowSampling::GetDirectionalLighting() * shadow;
		float3 ambient = ShadowSampling::GetAmbientLighting() * ambientVisibility;

		// Veins are brighter than the milky fill; foam scatters, so its ambient response is under 1.
		float3 albedo = Color::Diffuse(float3(0.86, 0.90, 0.92) * SharedData::shoreWavesSettings.FoamBrightness * lerp(0.7, 1.0, pattern.x));
		float3 lit = albedo * (dirLight * NdotL + ambient * 0.8);

		float3 H = normalize(L - viewDirection);
		float sparkle = pow(saturate(dot(foamNormal, H)), 32.0) * 0.12 * pattern.z;
		lit += dirLight * sparkle;

		lit = Color::IrradianceToGamma(lit);
		lit = lerp(lit, fogColor, fogFactor);
		return lerp(color, lit, alpha);
	}

	// Crest translucency: a thin, steep crest lets sky and sun light through, so it reads as a
	// lighter, greener band than the trough. Strongest when the sun is behind the crest as seen
	// from the camera. Uses the water's own shallow colour so it fits any water mod.
	float3 ApplyCrestColor(float3 color, float3 viewDirection, float3 shallowColor)
	{
		float glow = g_wave.crest * SharedData::shoreWavesSettings.CrestGlow;
		if (glow <= 0.002)
			return color;
		float backlight = 0.35 + 0.65 * saturate(dot(SharedData::DirLightDirection.xyz, -viewDirection) * 0.5 + 0.5);
		// Neutral: only lighter, no hue shift. A green tint read as unnatural in testing.
		float3 tint = shallowColor * 1.3 + 0.03;
		return lerp(color, tint, saturate(glow * backlight * 0.5));
	}

	// ---------------------------------------------------------------- Debug views

	float3 HueToRGB(float hue)
	{
		float3 rgb = abs(hue * 6.0 - float3(3.0, 2.0, 4.0)) * float3(1.0, -1.0, -1.0) + float3(-1.0, 2.0, 2.0);
		return saturate(rgb);
	}

	// Red at 0, yellow at 1/4, green at 1/2, blue at 1, dark blue beyond.
	float3 Ramp(float t)
	{
		if (t > 1.0)
			return float3(0.0, 0.0, 0.25);
		float3 c = lerp(float3(1.0, 0.0, 0.0), float3(1.0, 1.0, 0.0), saturate(t * 4.0));
		c = lerp(c, float3(0.0, 1.0, 0.0), saturate(t * 4.0 - 1.0));
		c = lerp(c, float3(0.0, 0.0, 1.0), saturate((t - 0.5) * 2.0));
		return c;
	}

	// Negative depth (scene above the water surface, e.g. a dock plank) shows as magenta.
	float3 DepthRamp(float depth)
	{
		if (depth < 0.0)
			return float3(1.0, 0.0, 1.0);
		return Ramp(depth / max(SharedData::shoreWavesSettings.MaxDepth, 1.0));
	}

	float3 DirectionHue(float2 shoreDir, float valid)
	{
		if (valid < 0.5)
			return 0.35.xxx;
		float angle = atan2(shoreDir.y, shoreDir.x);
		float hue = frac(angle / TwoPi + 1.0);
		return HueToRGB(hue);
	}

	float3 FieldDistanceColor(float dist, float valid)
	{
		if (valid < 0.5)
			return float3(1.0, 0.0, 1.0);
		if (dist < 0.0)
			return lerp(float3(0.45, 0.3, 0.15), float3(0.15, 0.1, 0.05), saturate(-dist / 512.0));
		return Ramp(dist / 2048.0);
	}

	float3 DebugColor(ShoreData data)
	{
		uint mode = SharedData::shoreWavesSettings.DebugMode;
		if (mode == 1)
			return DepthRamp(data.depth);
		if (mode == 2)
			return DirectionHue(data.shoreDir, data.dirValid);
		if (mode == 4)
			return FieldDistanceColor(data.fieldDist, data.fieldValid);
		if (mode == 5)
			return DirectionHue(data.fieldDir, data.fieldValid * (length(data.fieldDir) > 0.5 ? 1.0 : 0.0));
		if (mode == 6) {
			if (data.fieldValid < 0.5)
				return float3(1.0, 0.0, 1.0);
			return DepthRamp(data.fieldDepth);
		}
		if (mode == 7) {
			float amp = max(SharedData::shoreWavesSettings.WaveAmplitude, 1.0);
			return saturate(0.5 + g_wave.height / (2.0 * amp)).xxx;
		}
		if (mode == 8)
			return float3(g_wave.breaking, 0.15 * g_wave.active, 0.0);
		if (mode == 9)
			return GetFoamMask(data).xxx;
		float3 hue = DirectionHue(data.shoreDir, data.dirValid);
		return hue * lerp(0.15, 1.0, data.fade);
	}
}
