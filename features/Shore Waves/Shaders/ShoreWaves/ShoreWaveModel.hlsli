// Shore Waves: the wave model, shared by the water pixel shader (shading, foam) and the
// hull/domain shaders (tessellation and displacement). Stage-agnostic: it only needs
// SharedData (settings, Timer), FrameBuffer (CameraPosAdjust) and Random (perlinNoise), and
// takes the field texture and sampler as parameters.
//
// Crests advance toward land along the field direction. Their phase accumulates along the
// shore distance with the shallow-water wavenumber, so crests bunch up as the seabed rises;
// amplitude follows Green's law and caps at the 0.78 height-to-depth breaking ratio.

#ifndef __SHORE_WAVE_MODEL_HLSLI__
#define __SHORE_WAVE_MODEL_HLSLI__

namespace ShoreWaves
{
	static const float Gravity = 686.7;  // 9.81 m/s^2 in game units (70 units per metre)
	static const float TwoPi = 6.28318530718;

	// Minimum seabed slope assumed near the waterline: the baked bathymetry is bilinear across
	// 256-unit texels, so its zero crossing sits metres out from the true shoreline. The signed
	// distance is exact there, so depth is never allowed below slope * distance.
	static const float MinShoreSlope = 0.06;

	// Waves stay alive this far past the field's shoreline so the water mesh, which can extend
	// a little beyond it, ends the effect instead of a hard isoline.
	static const float ShorelineOverrun = 192.0;

	struct FieldSample
	{
		float dist;   // Signed distance to the shoreline in game units, positive in water.
		float2 dir;   // Unit direction toward land, or zero.
		float depth;  // Bathymetry: water height minus seabed height, positive in water.
		float valid;  // 1 where the field has terrain data for this position.
	};

	struct WaveData
	{
		float active;    // 1 when the wave model contributed
		float height;    // surface height offset in game units (mean zero)
		float2 slope;    // dh/dx, dh/dy in world XY
		float breaking;  // 0..1, 1 where the crest is at the breaking limit
		float fade;      // depth fade applied
		float foam;      // 0..1 foam source from breaking crests and their trailing wash
		float crest;     // 0..1 crest profile of the dominant train, for crest translucency
	};

	// Samples the baked field at an absolute world XY position.
	FieldSample SampleShoreField(Texture2D<float4> fieldTex, SamplerState fieldSampler, float2 worldXY)
	{
		FieldSample f;
		f.dist = 0.0;
		f.dir = 0.0.xx;
		f.depth = 0.0;
		f.valid = 0.0;
		if (!SharedData::shoreWavesSettings.FieldValid)
			return f;

		float2 extent = SharedData::shoreWavesSettings.FieldSize * SharedData::shoreWavesSettings.FieldTexelSize;
		float2 uv = (worldXY - SharedData::shoreWavesSettings.FieldOrigin) / extent;
		if (any(uv < 0.0) || any(uv > 1.0))
			return f;

		float4 texel = fieldTex.SampleLevel(fieldSampler, uv, 0);
		// Valid texels store bathymetry in [-0.5, 0.5]; no-data texels store -1. Bilinear blends
		// toward -1 next to a no-data edge, so anything below -0.75 had a no-data majority.
		f.valid = texel.w > -0.75 ? 1.0 : 0.0;
		f.dist = texel.x * SharedData::shoreWavesSettings.FieldMaxRange;
		f.depth = texel.w * 2.0 * SharedData::shoreWavesSettings.FieldBathyRange;
		float len = length(texel.yz);
		f.dir = len > 0.05 ? texel.yz / len : 0.0.xx;
		return f;
	}

	// Effective seabed depth used for speed, amplitude and fade.
	float EffectiveDepth(FieldSample f)
	{
		float dist = max(f.dist, 4.0);
		return max(max(f.depth, 0.0), dist * MinShoreSlope);
	}

	// 0..1: where the wave model is active, 1 at the waterline, 0 at MaxDepth and beyond.
	float ShoreFade(FieldSample f)
	{
		if (f.valid < 0.5 || f.dist < -ShorelineOverrun)
			return 0.0;
		float fade = saturate(1.0 - EffectiveDepth(f) / max(SharedData::shoreWavesSettings.MaxDepth, 1.0));
		return fade * fade;
	}

	// One wave train at a point `dist` units from shore over seabed depth `depth`.
	// Phase: with the seabed rising linearly from the shore to this point, the integral of the
	// local wavenumber k(s) = w / sqrt(g d(s)) from the shore to here is 2 k dist, so crests are
	// spaced by the local wavelength yet stay continuous across changes in depth.
	// Amplitude: Green's law (d^-1/4) about RefDepth, capped at 0.39 d (breaking ratio 0.78).
	void EvaluateTrain(float dist, float depth, float t, float period, float ampScale, float phaseOffset, float noisePhase,
		out float h, out float dhdDist, out float breakingRatio, out float foam)
	{
		float d = max(depth, SharedData::shoreWavesSettings.MinDepth);
		float omega = TwoPi / period;
		float k = omega / sqrt(Gravity * d);

		float amp = SharedData::shoreWavesSettings.WaveAmplitude * ampScale * rsqrt(sqrt(d / max(SharedData::shoreWavesSettings.RefDepth, 1.0)));  // d^-1/4, Green's law
		float ampCap = 0.39 * d;
		breakingRatio = amp / max(ampCap, 1e-3);
		amp = min(amp, ampCap);

		// + k dist: a crest of constant phase moves toward smaller dist, i.e. toward land.
		float ph = 2.0 * k * dist + omega * t + phaseOffset + noisePhase;
		float sn = sin(ph) * 0.5 + 0.5;
		float sharp = SharedData::shoreWavesSettings.WaveSharpness;
		float p = pow(sn, sharp);
		h = amp * (p - 0.5);
		float dpdph = sharp * pow(max(sn, 1e-4), sharp - 1.0) * 0.5 * cos(ph);
		dhdDist = amp * dpdph * 2.0 * k;

		// Foam: the crest itself where it is breaking, plus a wash that trails it. Phase grows
		// with time at a fixed point, so the fraction of a period since the last crest (crest
		// at phase pi/2) gives the age of the wash directly.
		float breakingFactor = saturate((breakingRatio - 0.85) / 0.3);
		float sinceCrest = frac((ph - 1.5707963) / TwoPi) * period;
		float wash = exp(-sinceCrest / max(SharedData::shoreWavesSettings.FoamDecay, 0.1));
		float crest = pow(sn, sharp * 2.0);
		foam = breakingFactor * max(crest, 0.7 * wash);
	}

	WaveData EvaluateWaves(FieldSample f, float2 worldXY, float t)
	{
		WaveData w = (WaveData)0;
		if (!SharedData::shoreWavesSettings.Enabled)
			return w;

		float fade = ShoreFade(f);
		if (fade <= 0.0)
			return w;
		float dist = max(f.dist, 4.0);
		float depth = EffectiveDepth(f);

		float noisePhase = 0.0;
		if (SharedData::shoreWavesSettings.NoiseStrength > 0.0) {
			float3 p = float3(worldXY * SharedData::shoreWavesSettings.NoiseScale, t * 0.03);
			noisePhase = Random::perlinNoise(p) * SharedData::shoreWavesSettings.NoiseStrength * TwoPi;
		}

		// period multiplier, amplitude weight, phase offset
		const float3 trains[3] = { float3(1.00, 1.00, 0.0), float3(0.83, 0.55, 2.1), float3(1.31, 0.40, 4.4) };
		uint count = clamp(SharedData::shoreWavesSettings.WaveTrains, 1u, 3u);

		float h = 0.0, dh = 0.0, breaking = 0.0, weight = 0.0, foam = 0.0, crest = 0.0;
		[unroll] for (uint i = 0; i < 3; i++)
		{
			if (i < count) {
				float hi, dhi, bri, fi;
				EvaluateTrain(dist, depth, t, SharedData::shoreWavesSettings.WavePeriod * trains[i].x,
					trains[i].y, trains[i].z, noisePhase * (i == 0 ? 1.0 : 0.6), hi, dhi, bri, fi);
				crest = max(crest, saturate(hi / max(abs(hi) + 1e-3, 1e-3)) * trains[i].y * saturate(bri));
				h += hi;
				dh += dhi;
				breaking = max(breaking, bri * trains[i].y);
				foam = max(foam, fi * trains[i].y * trains[i].y);  // secondary trains are smaller and foam less
				weight += trains[i].y;
			}
		}

		float scale = fade * SharedData::shoreWavesSettings.Intensity / max(weight, 1e-3);
		w.active = 1.0;
		w.height = h * scale;
		// d(dist)/d(xy) = -dir (distance shrinks toward land)
		w.slope = -dh * scale * f.dir;
		w.breaking = saturate((breaking - 0.7) / 0.3) * fade;
		w.fade = fade;
		w.foam = foam * sqrt(fade) * SharedData::shoreWavesSettings.Intensity;
		w.crest = crest * fade * SharedData::shoreWavesSettings.Intensity;
		return w;
	}

	// Tessellation factor for one vertex of a water patch: high near the waterline and the
	// camera, 1 (no subdivision) in deep water or far away. Computed per vertex from world
	// position only, so the two patches sharing an edge agree and no cracks open.
	float TessellationFactor(float3 waterPositionWS, FieldSample f)
	{
		if (!SharedData::shoreWavesSettings.TessellationActive)
			return 1.0;
		float shore = ShoreFade(f);
		float camDist = length(waterPositionWS);
		float distFade = saturate(1.0 - camDist / max(SharedData::shoreWavesSettings.TessDistance, 1.0));
		float t = sqrt(saturate(shore)) * distFade;
		return lerp(1.0, max(SharedData::shoreWavesSettings.MaxTessFactor, 1.0), t);
	}
}

#endif  // __SHORE_WAVE_MODEL_HLSLI__
