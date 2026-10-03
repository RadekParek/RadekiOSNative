package org.radekiosnative.recompiler;

import android.content.Context;
import android.content.SharedPreferences;

/** Persisted controls that are passed to the native guest runtime. */
public final class AppSettings {
    private static final String FILE = "runtime_settings";
    private static final String ARM64_COMPATIBILITY = "arm64.compatibility_fallbacks";
    private static final String ARM64_TRACE_MISSING_APIS = "arm64.trace_missing_apis";
    private static final String REALTIME_LOGGING = "diagnostics.realtime_logging";

    private AppSettings() {}

    private static SharedPreferences preferences(Context context) {
        return context.getApplicationContext().getSharedPreferences(FILE, Context.MODE_PRIVATE);
    }

    /** Compatibility fallbacks are enabled by default, matching the existing launcher behavior. */
    public static boolean useArm64CompatibilityFallbacks(Context context) {
        return preferences(context).getBoolean(ARM64_COMPATIBILITY, true);
    }

    public static void setArm64CompatibilityFallbacks(Context context, boolean enabled) {
        preferences(context).edit().putBoolean(ARM64_COMPATIBILITY, enabled).apply();
    }

    /** Sampled missing-API logs are off by default to avoid adding work to guest calls. */
    public static boolean traceArm64MissingApis(Context context) {
        return preferences(context).getBoolean(ARM64_TRACE_MISSING_APIS, false);
    }

    public static void setArm64TraceMissingApis(Context context, boolean enabled) {
        preferences(context).edit().putBoolean(ARM64_TRACE_MISSING_APIS, enabled).apply();
    }

    /** RTLS is opt-in; ordinary run summaries and fatal diagnostics are always retained. */
    public static boolean useRealtimeLogging(Context context) {
        return preferences(context).getBoolean(REALTIME_LOGGING, false);
    }

    public static void setRealtimeLogging(Context context, boolean enabled) {
        preferences(context).edit().putBoolean(REALTIME_LOGGING, enabled).apply();
    }
}
