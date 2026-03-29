# Sidecar Companion — ProGuard rules
# BLE and notification classes must not be stripped
-keep class com.sidecar.companion.** { *; }
-keep class android.bluetooth.** { *; }
-keep class android.service.notification.** { *; }
