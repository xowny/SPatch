#pragma once

#include "Config.h"

#include <algorithm>
#include <cmath>

namespace spatch::hooks {

inline int ClampCutsceneFps(int value) {
    if (value < kCutsceneFpsAuto) {
        return kCutsceneFpsAuto;
    }
    if (value != kCutsceneFpsAuto && value < kCutsceneFpsMin) {
        return kCutsceneFpsMin;
    }
    if (value > kCutsceneFpsMax) {
        return kCutsceneFpsMax;
    }
    return value;
}

inline float CutsceneExpectedDelta(int fps) {
    const int clamped = ClampCutsceneFps(fps);
    return clamped > 0 ? (1.0f / static_cast<float>(clamped)) : 0.0f;
}

inline bool IsSaneCutsceneDelta(float value) {
    // Keep the auto mode broad enough for normal frame pacing and short
    // hitches, while rejecting uninitialised/absurd values.
    return std::isfinite(value) && value >= (0.5f / static_cast<float>(kCutsceneFpsMax)) &&
           value <= (1.5f / static_cast<float>(kCutsceneFpsMin));
}

inline bool IsCutsceneDeltaNear(float value, float expected, float relative_tolerance = 0.25f) {
    if (!std::isfinite(value) || !std::isfinite(expected) || value <= 0.0f || expected <= 0.0f) {
        return false;
    }
    // A 0.5 ms floor is harmless around 30/60 Hz but becomes a 25-50% error
    // at 500-1000 Hz.  Keep only a tiny floating-point floor so the caller's
    // relative tolerance retains its stated meaning at every supported rate.
    const float tolerance = std::max(0.00001f, expected * relative_tolerance);
    return std::fabs(value - expected) <= tolerance;
}

inline bool IsExpectedCutsceneInputDelta(float value, int target_fps) {
    if (!IsSaneCutsceneDelta(value)) {
        return false;
    }
    const int clamped = ClampCutsceneFps(target_fps);
    return clamped == kCutsceneFpsAuto ||
           IsCutsceneDeltaNear(value, CutsceneExpectedDelta(clamped), 0.35f);
}

inline float SelectCutsceneCorrectionDelta(float live_delta, int target_fps) {
    const int clamped = ClampCutsceneFps(target_fps);
    // A configured rate is an expected cadence, not a time-step limiter.  Only
    // remove tiny measurement jitter when the live sample already belongs to
    // that cadence; otherwise keep the measured value so a real hitch is not
    // silently rewritten as a different playback speed.
    if (clamped > 0 && IsCutsceneDeltaNear(live_delta, CutsceneExpectedDelta(clamped), 0.05f)) {
        return CutsceneExpectedDelta(clamped);
    }
    return live_delta;
}

inline float ResolveCutsceneBaseDelta(float live_delta,
                                      int target_fps,
                                      float last_sane_delta = 0.0f) {
    if (IsSaneCutsceneDelta(live_delta)) {
        return SelectCutsceneCorrectionDelta(live_delta, target_fps);
    }
    if (IsSaneCutsceneDelta(last_sane_delta)) {
        return SelectCutsceneCorrectionDelta(last_sane_delta, target_fps);
    }
    return CutsceneExpectedDelta(target_fps);
}

inline bool IsCutsceneCadenceMultiple(float value,
                                      float base_delta,
                                      int min_multiple = 2,
                                      int max_multiple = 16,
                                      float relative_tolerance = 0.08f) {
    if (!IsSaneCutsceneDelta(value) || !IsSaneCutsceneDelta(base_delta) ||
        min_multiple < 2 || max_multiple < min_multiple) {
        return false;
    }

    const float ratio = value / base_delta;
    if (!std::isfinite(ratio) || ratio < static_cast<float>(min_multiple) - 1.0f ||
        ratio > static_cast<float>(max_multiple) + 1.0f) {
        return false;
    }

    const float rounded = std::round(ratio);
    return rounded >= static_cast<float>(min_multiple) &&
           rounded <= static_cast<float>(max_multiple) &&
           std::fabs(ratio - rounded) <= std::max(0.02f, rounded * relative_tolerance);
}

inline bool IsObservedThirtyHzAnomaly(float value, float base_delta) {
    // Reverse-engineering traces show an occasional ~30-Hz sample among
    // otherwise stable high-refresh samples.  Treat it as an anomaly only
    // while an active cadence is at least 60 Hz; this is not a global 30-Hz
    // limiter or an assumption about ordinary gameplay timing.
    constexpr float kThirtyHzDelta = 1.0f / 30.0f;
    return IsSaneCutsceneDelta(base_delta) && base_delta <= (1.0f / 60.0f) * 1.05f &&
           IsCutsceneDeltaNear(value, kThirtyHzDelta, 0.08f);
}

inline bool IsObservedLegacyRateFallback(float value, float base_delta) {
    constexpr float kSixtyHzDelta = 1.0f / 60.0f;
    const bool fell_to_sixty = IsSaneCutsceneDelta(base_delta) &&
                               base_delta < kSixtyHzDelta * 0.95f &&
                               IsCutsceneDeltaNear(value, kSixtyHzDelta, 0.08f);
    return fell_to_sixty || IsObservedThirtyHzAnomaly(value, base_delta);
}

inline bool IsConfiguredLegacyRateFallback(float value, int target_fps) {
    const int clamped = ClampCutsceneFps(target_fps);
    if (clamped <= kCutsceneFpsAuto) {
        return false;
    }

    constexpr float kSixtyHzDelta = 1.0f / 60.0f;
    constexpr float kThirtyHzDelta = 1.0f / 30.0f;
    // A configured rate may sit very close to a legacy rate (31/32 vs 30,
    // 61/62 vs 60).  A broad proximity-only test would classify the actual
    // requested cadence as a fallback and make the resolver suppress its own
    // correction.  Only call it a fallback when the live sample is both near
    // a canonical engine rate and materially closer to that canonical rate
    // than to the configured target.
    const auto is_closer_to_legacy = [clamped](float candidate, float legacy) {
        if (!IsCutsceneDeltaNear(candidate, legacy, 0.08f)) {
            return false;
        }
        const float target_delta = CutsceneExpectedDelta(clamped);
        // A fallback is a slowdown. Canonical 30/60-Hz samples that are equal
        // to or faster than the requested cadence must never seed a slower
        // configured delta and accelerate scene time during hysteresis.
        if (candidate <= target_delta + 0.000001f) {
            return false;
        }
        return std::fabs(candidate - legacy) + 0.000001f <
               std::fabs(candidate - target_delta);
    };
    return is_closer_to_legacy(value, kSixtyHzDelta) ||
           is_closer_to_legacy(value, kThirtyHzDelta);
}

inline float ResolveTrackedCutsceneBaseDelta(float live_delta,
                                             float last_sane_delta,
                                             int target_fps) {
    // Either mode may see the exact legacy 60/30-Hz cadence for one frame. If
    // this timeline already established a faster cadence, retain it until the
    // tracker has enough consecutive evidence to adopt a real slowdown.
    if (IsSaneCutsceneDelta(last_sane_delta) &&
        (IsObservedLegacyRateFallback(live_delta, last_sane_delta) ||
         IsConfiguredLegacyRateFallback(live_delta, target_fps))) {
        return last_sane_delta;
    }
    return ResolveCutsceneBaseDelta(live_delta, target_fps, last_sane_delta);
}

struct CutsceneCadenceTracker {
    float stable_delta = 0.0f;
    float fallback_candidate_delta = 0.0f;
    unsigned int fallback_candidate_count = 0;
    float fallback_candidate_elapsed = 0.0f;
};

inline void ResetCutsceneCadenceTracker(CutsceneCadenceTracker& tracker) {
    tracker = {};
}

inline float TrackCutsceneBaseDelta(CutsceneCadenceTracker& tracker,
                                    float live_delta,
                                    int target_fps) {
    constexpr unsigned int kFallbackAdoptionFrames = 3;
    constexpr float kLegacyFallbackAdoptionSeconds = 0.5f;
    if (!IsSaneCutsceneDelta(live_delta)) {
        tracker.fallback_candidate_delta = 0.0f;
        tracker.fallback_candidate_count = 0;
        tracker.fallback_candidate_elapsed = 0.0f;
        return ResolveCutsceneBaseDelta(live_delta, target_fps, tracker.stable_delta);
    }

    // On a new/reset explicit timeline, the first engine sample can already
    // be the legacy 30/60-Hz fallback.  Seeding stable_delta from that sample
    // makes it indistinguishable from a genuine target and prevents the scene
    // time repair from ever engaging.  Establish the configured cadence as
    // the baseline and treat this first legacy sample as adoption evidence.
    if (!IsSaneCutsceneDelta(tracker.stable_delta) &&
        IsConfiguredLegacyRateFallback(live_delta, target_fps)) {
        tracker.stable_delta = CutsceneExpectedDelta(target_fps);
        tracker.fallback_candidate_delta = live_delta;
        tracker.fallback_candidate_count = 1;
        tracker.fallback_candidate_elapsed = live_delta;
        return tracker.stable_delta;
    }

    if (IsSaneCutsceneDelta(tracker.stable_delta)) {
        const bool known_legacy_fallback =
            IsObservedLegacyRateFallback(live_delta, tracker.stable_delta) ||
            IsConfiguredLegacyRateFallback(live_delta, target_fps);

        // Once a canonical 30/60-Hz fallback has been adopted after the
        // sustained-evidence window, an unambiguously faster sample is a
        // recovery, not a new candidate. Clear the old stable/candidate state
        // immediately so a following zero/tiny sample cannot resurrect the
        // stale legacy cadence.
        const bool stable_is_configured_legacy =
            IsConfiguredLegacyRateFallback(tracker.stable_delta, target_fps);
        // Auto mode has no configured cadence: 30/60 Hz may be the genuine
        // timeline rate.  Do not infer "adopted fallback" from the numeric
        // value alone or a single faster outlier will poison the stable rate
        // when the original cadence returns.  Explicit mode is unambiguous
        // because its requested cadence is independently known.
        if (stable_is_configured_legacy &&
            live_delta < tracker.stable_delta * 0.80f) {
            tracker.stable_delta = SelectCutsceneCorrectionDelta(live_delta, target_fps);
            tracker.fallback_candidate_delta = 0.0f;
            tracker.fallback_candidate_count = 0;
            tracker.fallback_candidate_elapsed = 0.0f;
            return tracker.stable_delta;
        }

        const bool near_stable_cadence =
            IsCutsceneDeltaNear(live_delta, tracker.stable_delta, 0.35f);
        // After the hysteresis window, the adopted canonical rate is now the
        // stable cadence. Treat subsequent matching samples as ordinary
        // continuity even though they still resemble a configured fallback;
        // otherwise every frame would reopen a new pending candidate.
        const bool stable_is_adopted_legacy = stable_is_configured_legacy;
        if (near_stable_cadence && (!known_legacy_fallback || stable_is_adopted_legacy)) {
            tracker.stable_delta = stable_is_adopted_legacy
                                      ? live_delta
                                      : SelectCutsceneCorrectionDelta(live_delta, target_fps);
            tracker.fallback_candidate_delta = 0.0f;
            tracker.fallback_candidate_count = 0;
            tracker.fallback_candidate_elapsed = 0.0f;
            return tracker.stable_delta;
        }

        if (IsCutsceneDeltaNear(live_delta, tracker.fallback_candidate_delta, 0.08f)) {
            ++tracker.fallback_candidate_count;
            tracker.fallback_candidate_elapsed += live_delta;
        } else {
            tracker.fallback_candidate_delta = live_delta;
            tracker.fallback_candidate_count = 1;
            tracker.fallback_candidate_elapsed = live_delta;
        }

        const bool legacy_evidence_sustained =
            tracker.fallback_candidate_elapsed >= kLegacyFallbackAdoptionSeconds;
        if (tracker.fallback_candidate_count < kFallbackAdoptionFrames ||
            (known_legacy_fallback && !legacy_evidence_sustained)) {
            // A known 60/30-Hz regression uses the established cadence for
            // correction until it has remained for a meaningful interval.
            // Other large deviations retain the old three-sample hysteresis:
            // a genuine long frame is returned as live time while the stable
            // estimator remains available if the following frame is zero.
            return known_legacy_fallback ? tracker.stable_delta : live_delta;
        }
    }

    const bool adopted_legacy_fallback =
        IsConfiguredLegacyRateFallback(live_delta, target_fps) ||
        IsObservedLegacyRateFallback(live_delta, tracker.stable_delta);
    // Do not snap a recognized 60/30 fallback to a nearby target (for example
    // 60 -> 62 Hz).  The live cadence is the evidence being adopted; snapping
    // it back to the target would leave the tracker permanently pending.
    tracker.stable_delta = adopted_legacy_fallback
                              ? live_delta
                              : SelectCutsceneCorrectionDelta(live_delta, target_fps);
    tracker.fallback_candidate_delta = 0.0f;
    tracker.fallback_candidate_count = 0;
    tracker.fallback_candidate_elapsed = 0.0f;
    return tracker.stable_delta;
}

inline bool IsLegacyFallbackPending(const CutsceneCadenceTracker& tracker,
                                    int target_fps) {
    if (tracker.fallback_candidate_count == 0 ||
        !IsSaneCutsceneDelta(tracker.fallback_candidate_delta)) {
        return false;
    }
    return IsConfiguredLegacyRateFallback(tracker.fallback_candidate_delta, target_fps) ||
           (IsSaneCutsceneDelta(tracker.stable_delta) &&
            IsObservedLegacyRateFallback(tracker.fallback_candidate_delta,
                                          tracker.stable_delta));
}

inline bool IsObservedLegacyCadenceAnomaly(float value, float base_delta) {
    if (!IsSaneCutsceneDelta(value) || !IsSaneCutsceneDelta(base_delta) ||
        value <= (base_delta * 1.25f)) {
        return false;
    }
    constexpr float kSixtyHzDelta = 1.0f / 60.0f;
    const bool standard_legacy_step = IsCutsceneDeltaNear(value, kSixtyHzDelta, 0.08f) ||
                                      IsObservedThirtyHzAnomaly(value, base_delta);
    // Captured NIS traces also contain short absolute steps around 21–23 ms
    // (and their ~43 ms doubled form), even when the live cadence is 90/120+
    // Hz.  Keep this narrow band state-gated by the caller rather than
    // treating every 1.25x frame hitch as a correction candidate.
    const bool high_refresh_base = base_delta <= (1.0f / 60.0f) * 1.05f;
    const bool observed_short_step =
        high_refresh_base && ((value >= 0.0205f && value <= 0.0235f) ||
                              (value >= 0.0410f && value <= 0.0455f));
    return standard_legacy_step || observed_short_step;
}

inline bool IsTinyCutsceneTransitionDelta(float value, float expected_cutscene_dt) {
    return std::isfinite(value) && value > 0.0f && expected_cutscene_dt > 0.0f &&
           value < (expected_cutscene_dt * 0.20f);
}

// Kept as a source-compatibility alias for older diagnostics/tests. New code
// should use IsExpectedCutsceneInputDelta or IsCutsceneDeltaNear.
inline bool IsApprox60HzCutsceneDelta(float value) {
    return IsCutsceneDeltaNear(value, 1.0f / 60.0f, 0.25f);
}

struct CutscenePauseState {
    bool ui_paused = false;
    bool simtime_paused = false;
};

struct CutsceneFrameflowDecision {
    float forwarded_dt = 0.0f;
    bool applied_zero_dt_fix = false;
    bool applied_while_game_paused = false;
    bool applied_while_simtime_paused = false;
};

inline CutsceneFrameflowDecision ResolveCutsceneFrameflowDelta(bool from_cutscene_scope,
                                                                CutscenePauseState pause_state,
                                                                bool fix_enabled,
                                                                int target_fps,
                                                                float delta_seconds,
                                                                float cutscene_input_dt) {
    CutsceneFrameflowDecision decision{};
    decision.forwarded_dt = delta_seconds;

    const float live_expected_dt = SelectCutsceneCorrectionDelta(cutscene_input_dt, target_fps);
    // A positive setting is authoritative for synthesising a missing/tiny
    // cutscene tick.  Non-zero live samples are still preserved unless they
    // match a narrowly recognised anomaly in the NIS scene-time path.
    const float correction_dt = target_fps > kCutsceneFpsAuto
                                    ? CutsceneExpectedDelta(target_fps)
                                    : live_expected_dt;
    const float zero_threshold =
        std::min(0.0005f, correction_dt > 0.0f ? correction_dt * 0.05f : 0.0005f);
    const bool zero_dt = delta_seconds >= 0.0f && delta_seconds < zero_threshold;
    // The configured FPS is used only to synthesize a missing/tiny tick; a
    // real non-zero frame sample is left untouched by the outer hook.
    const bool valid_cutscene_input = IsSaneCutsceneDelta(cutscene_input_dt) ||
                                      IsSaneCutsceneDelta(correction_dt);
    const bool tiny_transition_dt =
        valid_cutscene_input && IsTinyCutsceneTransitionDelta(delta_seconds, correction_dt);
    // The outer FrameFlow contract is deliberately narrow: only zero/tiny
    // samples are repaired here.  Integer-multiple/30-Hz anomalies are
    // handled by ResolveCutsceneSceneTime, where state-4 NIS evidence exists.
    const bool should_apply_fix =
        from_cutscene_scope && !pause_state.ui_paused && fix_enabled && valid_cutscene_input &&
        (zero_dt || tiny_transition_dt);

    if (should_apply_fix) {
        decision.forwarded_dt = correction_dt;
        decision.applied_zero_dt_fix = zero_dt || tiny_transition_dt;
        decision.applied_while_game_paused = pause_state.ui_paused;
        decision.applied_while_simtime_paused = pause_state.simtime_paused;
    }

    return decision;
}

inline CutsceneFrameflowDecision ResolveCutsceneFrameflowDelta(bool from_cutscene_scope,
                                                                CutscenePauseState pause_state,
                                                                bool fix_enabled,
                                                                float delta_seconds,
                                                                float cutscene_input_dt) {
    return ResolveCutsceneFrameflowDelta(from_cutscene_scope,
                                          pause_state,
                                          fix_enabled,
                                          60,
                                          delta_seconds,
                                          cutscene_input_dt);
}

inline CutsceneFrameflowDecision ResolveCutsceneFrameflowDelta(bool from_cutscene_scope,
                                                                bool game_paused,
                                                                bool fix_enabled,
                                                                int target_fps,
                                                                float delta_seconds,
                                                                float cutscene_input_dt) {
    CutscenePauseState pause_state{};
    pause_state.ui_paused = game_paused;
    pause_state.simtime_paused = game_paused;
    return ResolveCutsceneFrameflowDelta(
        from_cutscene_scope, pause_state, fix_enabled, target_fps, delta_seconds, cutscene_input_dt);
}

inline CutsceneFrameflowDecision ResolveCutsceneFrameflowDelta(bool from_cutscene_scope,
                                                                bool game_paused,
                                                                bool fix_enabled,
                                                                float delta_seconds,
                                                                float cutscene_input_dt) {
    return ResolveCutsceneFrameflowDelta(
        from_cutscene_scope, game_paused, fix_enabled, 60, delta_seconds, cutscene_input_dt);
}

struct CutsceneSceneTimeDecision {
    float applied_scene_time = 0.0f;
    bool corrected = false;
    bool repaired_timing = false;
};

inline bool CanRetainCutsceneTimelineHistory(bool had_history,
                                             float previous_applied_scene_time,
                                             float applied_scene_time) {
    return !had_history ||
           (std::isfinite(previous_applied_scene_time) &&
            std::isfinite(applied_scene_time) &&
            applied_scene_time >= previous_applied_scene_time);
}

inline CutsceneSceneTimeDecision ResolveCutsceneSceneTime(bool active_nis_scope,
                                                           bool ui_paused,
                                                           bool fix_enabled,
                                                           bool sync_scene_time,
                                                           float previous_raw_scene_time,
                                                           float previous_applied_scene_time,
                                                           float scene_time,
                                                           float current_frame_dt,
                                                           int target_fps,
                                                           bool legacy_fallback_pending = false) {
    CutsceneSceneTimeDecision decision{};
    decision.applied_scene_time = scene_time;

    if (!active_nis_scope || ui_paused || !fix_enabled || sync_scene_time ||
        !std::isfinite(previous_raw_scene_time) ||
        !std::isfinite(previous_applied_scene_time) || !std::isfinite(scene_time) ||
        scene_time < previous_raw_scene_time) {
        return decision;
    }

    const float scene_delta = scene_time - previous_raw_scene_time;
    const float correction_dt = target_fps > kCutsceneFpsAuto
                                    ? CutsceneExpectedDelta(target_fps)
                                    : current_frame_dt;
    if (!IsSaneCutsceneDelta(correction_dt)) {
        return decision;
    }

    const float zero_threshold = std::min(0.0005f, correction_dt * 0.05f);
    const bool zero_or_tiny = scene_delta >= 0.0f &&
                              (scene_delta < zero_threshold ||
                               IsTinyCutsceneTransitionDelta(scene_delta, correction_dt));

    const bool configured_target = target_fps > kCutsceneFpsAuto;
    const bool configured_legacy_fallback =
        configured_target && IsConfiguredLegacyRateFallback(scene_delta, target_fps);
    // For a configured target, a broad 8% match is too permissive around
    // neighboring integer rates (60 vs 61/62, 30 vs 31/32).  Use a narrow
    // comparison while the sample is recognized as a canonical fallback;
    // once the tracker adopts the live cadence, the two values are identical
    // and the genuine sustained slowdown remains untouched.
    const float live_match_tolerance = configured_legacy_fallback ? 0.01f : 0.08f;
    const bool live_matches_scene =
        IsSaneCutsceneDelta(current_frame_dt) &&
        IsCutsceneDeltaNear(scene_delta, current_frame_dt, live_match_tolerance);
    const bool cadence_candidate =
        IsCutsceneCadenceMultiple(scene_delta, correction_dt) ||
        IsObservedLegacyCadenceAnomaly(scene_delta, correction_dt) ||
        configured_legacy_fallback;
    // A matching live/scene cadence is a real slowdown. The cadence tracker
    // deliberately withholds that match for the first two 60/30-Hz samples,
    // then exposes it after sustained evidence so explicit mode cannot slow
    // playback indefinitely when rendering genuinely remains at that rate.
    const bool cadence_anomaly =
        cadence_candidate &&
        (!configured_target || !live_matches_scene || legacy_fallback_pending);

    const float applied_delta = (zero_or_tiny || cadence_anomaly) ? correction_dt : scene_delta;
    const float translated_scene_time = previous_applied_scene_time + applied_delta;
    if (std::isfinite(translated_scene_time)) {
        decision.applied_scene_time = translated_scene_time;
        decision.repaired_timing = zero_or_tiny || cadence_anomaly;
        decision.corrected = std::fabs(decision.applied_scene_time - scene_time) > 0.000001f;
    }
    return decision;
}

inline CutsceneSceneTimeDecision ResolveCutsceneSceneTime(bool active_nis_scope,
                                                           bool ui_paused,
                                                           bool fix_enabled,
                                                           bool sync_scene_time,
                                                           float previous_scene_time,
                                                           float scene_time,
                                                           float current_frame_dt,
                                                           int target_fps) {
    return ResolveCutsceneSceneTime(active_nis_scope,
                                    ui_paused,
                                    fix_enabled,
                                    sync_scene_time,
                                    previous_scene_time,
                                    previous_scene_time,
                                    scene_time,
                                    current_frame_dt,
                                    target_fps);
}

inline CutsceneSceneTimeDecision ResolveCutsceneSceneTime(bool active_nis_scope,
                                                           bool ui_paused,
                                                           bool fix_enabled,
                                                           bool sync_scene_time,
                                                           float previous_scene_time,
                                                           float scene_time,
                                                           float current_frame_dt) {
    return ResolveCutsceneSceneTime(active_nis_scope,
                                    ui_paused,
                                    fix_enabled,
                                    sync_scene_time,
                                    previous_scene_time,
                                    previous_scene_time,
                                    scene_time,
                                    current_frame_dt,
                                    kCutsceneFpsAuto);
}

}  // namespace spatch::hooks
