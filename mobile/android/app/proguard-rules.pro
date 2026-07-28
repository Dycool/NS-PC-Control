# The C JNI exports use the exact com_nscontrol_NativeProtocol_* names, so R8
# must not rename either the object or its native methods in release builds.
-keep class com.nscontrol.NativeProtocol { *; }

# WebView invokes these methods by their JavaScript-visible names.
-keepclassmembers class * {
    @android.webkit.JavascriptInterface <methods>;
}
