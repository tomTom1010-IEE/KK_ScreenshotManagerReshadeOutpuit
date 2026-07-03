using alphaShot;
using BepInEx.Configuration;
using System;
using System.Collections;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using UnityEngine;
using UnityEngine.Rendering;

namespace Screencap
{
    public partial class ScreenshotManager
    {
        private static ConfigEntry<KeyboardShortcut> KeyExportOfflineReShade { get; set; }
        private static ConfigEntry<string> OfflineReShadeExportDir { get; set; }
        private static ConfigEntry<bool> OfflineReShadeArchiveToScreenshotDir { get; set; }
        private static ConfigEntry<bool> OfflineReShadeLogTimings { get; set; }
        private static ConfigEntry<OfflineDepthOutputFormat> OfflineReShadeDepthOutputFormat { get; set; }
#if KK
        private static ConfigEntry<string> OfflineReShadeD3D11DepthBridgeDllPath { get; set; }
        private static ConfigEntry<string> OfflineReShadeD3D11DepthBridgeLogPath { get; set; }
        private static ConfigEntry<bool> OfflineReShadeD3D11CandidateDiagnosticsEnabled { get; set; }
        private static ConfigEntry<bool> OfflineReShadeKkEnvironmentFingerprintEnabled { get; set; }
        private static ConfigEntry<bool> OfflineReShadeKkCameraStackRenderEnabled { get; set; }
        private static bool _kkDepthMissingHintLogged;
#endif

        /// <summary>
        /// Depth sidecar file format for offline ReShade exports.
        /// </summary>
        public enum OfflineDepthOutputFormat
        {
            /// <summary>
            /// Little-endian float32 device depth values, one value per pixel.
            /// </summary>
            RawRFloat,
            /// <summary>
            /// Raw RGBA8 bytes where R/G/B/A store a little-endian 32-bit normalized device depth integer.
            /// </summary>
            RawRgba8,
            /// <summary>
            /// RGBA8 PNG where R/G/B/A store a little-endian 32-bit normalized device depth integer.
            /// </summary>
            PngRgba8
        }

        private enum OfflineDepthTextureKind
        {
            DeviceRFloat
        }

        private sealed class OfflineCaptureResult
        {
            public RenderTexture Color;
            public Texture2D Depth;
            public string DepthSource;
            public OfflineDepthTextureKind DepthKind;
            public int DepthWidth;
            public int DepthHeight;
        }

#if KK
        private static class D3D11DepthBridge
        {
            [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
            private delegate int OrsInitializeDelegate(string logPath);
            [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
            private delegate int OrsShutdownDelegate();
            [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
            private delegate int OrsBeginCaptureDelegate(int width, int height, string label);
            [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
            private delegate int OrsEndCaptureDelegate();
            [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
            private delegate IntPtr OrsGetLastErrorDelegate();
            [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
            private delegate IntPtr OrsGetRenderEventFuncDelegate();
            [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
            private delegate int OrsSetUnityD3D11TextureDelegate(IntPtr nativeTexture);
            [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
            private delegate int OrsSetDepthRFloatOutputPathDelegate(string path);
            [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
            private delegate int OrsGetLastDepthQueueResultDelegate();
            [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
            private delegate int OrsSetCandidateDiagnosticsEnabledDelegate(int enabled);

            [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
            private static extern IntPtr LoadLibrary(string lpFileName);
            [DllImport("kernel32", CharSet = CharSet.Ansi, SetLastError = true)]
            private static extern IntPtr GetProcAddress(IntPtr hModule, string procName);

            private static IntPtr _module;
            private static bool _loadAttempted;
            private static bool _initialized;
            private static OrsInitializeDelegate _initialize;
            private static OrsShutdownDelegate _shutdown;
            private static OrsBeginCaptureDelegate _beginCapture;
            private static OrsEndCaptureDelegate _endCapture;
            private static OrsGetLastErrorDelegate _getLastError;
            private static OrsGetRenderEventFuncDelegate _getRenderEventFunc;
            private static OrsSetUnityD3D11TextureDelegate _setUnityD3D11Texture;
            private static OrsSetDepthRFloatOutputPathDelegate _setDepthRFloatOutputPath;
            private static OrsGetLastDepthQueueResultDelegate _getLastDepthQueueResult;
            private static OrsSetCandidateDiagnosticsEnabledDelegate _setCandidateDiagnosticsEnabled;
            private static IntPtr _renderEventFunc;

            public static bool TryBegin(int width, int height, string label)
            {
                if (!EnsureInitialized())
                    return false;

                SetCandidateDiagnosticsEnabled(OfflineReShadeD3D11CandidateDiagnosticsEnabled != null && OfflineReShadeD3D11CandidateDiagnosticsEnabled.Value);
                var result = _beginCapture(width, height, label);
                if (result == 0)
                    return true;

                Logger.LogWarning("Offline ReShade D3D11 depth bridge BeginCapture failed: " + GetLastError());
                return false;
            }

            public static void End()
            {
                if (!_initialized || _endCapture == null)
                    return;

                var result = _endCapture();
                if (result != 0)
                    Logger.LogWarning("Offline ReShade D3D11 depth bridge EndCapture failed: " + GetLastError());
            }

            public static void IssueRenderThreadHookEvent()
            {
                if (!_initialized || _renderEventFunc == IntPtr.Zero)
                    return;

                GL.IssuePluginEvent(_renderEventFunc, 1001);
            }

            public static void IssueDepthRFloatReadbackEvent(string path)
            {
                if (!_initialized || _renderEventFunc == IntPtr.Zero || _setDepthRFloatOutputPath == null || string.IsNullOrEmpty(path))
                    return;

                Directory.CreateDirectory(Path.GetDirectoryName(path));
                var result = _setDepthRFloatOutputPath(Path.GetFullPath(path));
                if (result != 0)
                {
                    Logger.LogWarning("Offline ReShade D3D11 depth bridge SetDepthRFloatOutputPath failed: " + GetLastError());
                    return;
                }

                GL.IssuePluginEvent(_renderEventFunc, 2002);
            }

            public static void IssueDepthRFloatFlushEvent()
            {
                if (!_initialized || _renderEventFunc == IntPtr.Zero)
                    return;

                GL.IssuePluginEvent(_renderEventFunc, 2003);
            }

            public static int GetLastDepthQueueResult()
            {
                if (!_initialized || _getLastDepthQueueResult == null)
                    return 0;

                return _getLastDepthQueueResult();
            }

            public static void SetCandidateDiagnosticsEnabled(bool enabled)
            {
                if (!_initialized || _setCandidateDiagnosticsEnabled == null)
                    return;

                var result = _setCandidateDiagnosticsEnabled(enabled ? 1 : 0);
                if (result != 0)
                    Logger.LogWarning("Offline ReShade D3D11 depth bridge SetCandidateDiagnosticsEnabled failed: " + GetLastError());
            }

            public static void SetUnityD3D11Texture(RenderTexture texture)
            {
                if (!_initialized || _setUnityD3D11Texture == null || texture == null)
                    return;

                var nativeTexture = texture.GetNativeTexturePtr();
                var result = _setUnityD3D11Texture(nativeTexture);
                if (result != 0)
                    Logger.LogWarning("Offline ReShade D3D11 depth bridge SetUnityD3D11Texture failed: " + GetLastError());
            }

            private static bool EnsureInitialized()
            {
                if (_initialized)
                    return true;
                if (_loadAttempted)
                    return false;

                _loadAttempted = true;

                var dllPath = GetBridgeDllPath();
                if (string.IsNullOrEmpty(dllPath) || !File.Exists(dllPath))
                {
                    Logger.LogWarning("Offline ReShade D3D11 depth bridge probe requested, but DLL was not found: " + dllPath);
                    return false;
                }

                _module = LoadLibrary(dllPath);
                if (_module == IntPtr.Zero)
                {
                    Logger.LogWarning("Offline ReShade D3D11 depth bridge LoadLibrary failed for " + dllPath + ", Win32=" + Marshal.GetLastWin32Error());
                    return false;
                }

                try
                {
                    _initialize = (OrsInitializeDelegate)Marshal.GetDelegateForFunctionPointer(GetRequiredProc("ORS_Initialize"), typeof(OrsInitializeDelegate));
                    _shutdown = (OrsShutdownDelegate)Marshal.GetDelegateForFunctionPointer(GetRequiredProc("ORS_Shutdown"), typeof(OrsShutdownDelegate));
                    _beginCapture = (OrsBeginCaptureDelegate)Marshal.GetDelegateForFunctionPointer(GetRequiredProc("ORS_BeginCapture"), typeof(OrsBeginCaptureDelegate));
                    _endCapture = (OrsEndCaptureDelegate)Marshal.GetDelegateForFunctionPointer(GetRequiredProc("ORS_EndCapture"), typeof(OrsEndCaptureDelegate));
                    _getLastError = (OrsGetLastErrorDelegate)Marshal.GetDelegateForFunctionPointer(GetRequiredProc("ORS_GetLastError"), typeof(OrsGetLastErrorDelegate));
                    _getRenderEventFunc = (OrsGetRenderEventFuncDelegate)Marshal.GetDelegateForFunctionPointer(GetRequiredProc("ORS_GetRenderEventFunc"), typeof(OrsGetRenderEventFuncDelegate));
                    _setUnityD3D11Texture = (OrsSetUnityD3D11TextureDelegate)Marshal.GetDelegateForFunctionPointer(GetRequiredProc("ORS_SetUnityD3D11Texture"), typeof(OrsSetUnityD3D11TextureDelegate));
                    _setDepthRFloatOutputPath = (OrsSetDepthRFloatOutputPathDelegate)Marshal.GetDelegateForFunctionPointer(GetRequiredProc("ORS_SetDepthRFloatOutputPath"), typeof(OrsSetDepthRFloatOutputPathDelegate));
                    _getLastDepthQueueResult = (OrsGetLastDepthQueueResultDelegate)Marshal.GetDelegateForFunctionPointer(GetRequiredProc("ORS_GetLastDepthQueueResult"), typeof(OrsGetLastDepthQueueResultDelegate));
                    _setCandidateDiagnosticsEnabled = (OrsSetCandidateDiagnosticsEnabledDelegate)Marshal.GetDelegateForFunctionPointer(GetRequiredProc("ORS_SetCandidateDiagnosticsEnabled"), typeof(OrsSetCandidateDiagnosticsEnabledDelegate));

                    var logPath = Path.GetFullPath(OfflineReShadeD3D11DepthBridgeLogPath.Value);
                    Directory.CreateDirectory(Path.GetDirectoryName(logPath));
                    var result = _initialize(logPath);
                    if (result != 0)
                    {
                        Logger.LogWarning("Offline ReShade D3D11 depth bridge initialize failed: " + GetLastError());
                        return false;
                    }

                    _renderEventFunc = _getRenderEventFunc();
                    if (_renderEventFunc == IntPtr.Zero)
                    {
                        Logger.LogWarning("Offline ReShade D3D11 depth bridge returned a null render event function.");
                        return false;
                    }

                    _initialized = true;
                    Logger.LogInfo("Offline ReShade D3D11 depth bridge probe initialized: " + dllPath);
                    return true;
                }
                catch (Exception ex)
                {
                    Logger.LogWarning("Offline ReShade D3D11 depth bridge load failed: " + ex.Message);
                    return false;
                }
            }

            private static IntPtr GetRequiredProc(string name)
            {
                var proc = GetProcAddress(_module, name);
                if (proc == IntPtr.Zero)
                    throw new MissingMethodException("Missing export " + name);
                return proc;
            }

            private static string GetBridgeDllPath()
            {
                if (OfflineReShadeD3D11DepthBridgeDllPath != null && !string.IsNullOrEmpty(OfflineReShadeD3D11DepthBridgeDllPath.Value))
                    return Path.GetFullPath(OfflineReShadeD3D11DepthBridgeDllPath.Value);

                var pluginDir = Path.GetDirectoryName(typeof(ScreenshotManager).Assembly.Location);
                return !string.IsNullOrEmpty(pluginDir) ? Path.Combine(pluginDir, "OfflineDepthD3D11Bridge.dll") : "OfflineDepthD3D11Bridge.dll";
            }

            public static string GetLastError()
            {
                if (_getLastError == null)
                    return "unknown";

                var ptr = _getLastError();
                return ptr != IntPtr.Zero ? Marshal.PtrToStringUni(ptr) : "unknown";
            }
        }

        private static void ForceD3D11DepthBridgeProbeFlush(RenderTexture colorRt, string timingLabel, string timingStep = "D3D11 probe 1x1 ReadPixels flush")
        {
            if (colorRt == null)
                return;

            Texture2D probeTex = null;
            var oldActive = RenderTexture.active;
            var flushSw = Stopwatch.StartNew();
            try
            {
                RenderTexture.active = colorRt;
                probeTex = new Texture2D(1, 1, TextureFormat.RGB24, false, false);
                probeTex.ReadPixels(new Rect(0, 0, 1, 1), 0, 0, false);
                probeTex.Apply(false, false);
            }
            finally
            {
                if (probeTex != null)
                    UnityEngine.Object.DestroyImmediate(probeTex);
                RenderTexture.active = oldActive;
                flushSw.Stop();
                LogOfflineTiming(timingLabel, timingStep, flushSw.Elapsed);
            }
        }

        private static void FlushD3D11DepthBridgePendingReadbacks(string timingLabel)
        {
            var rt = RenderTexture.GetTemporary(1, 1, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Linear);
            try
            {
                var oldActive = RenderTexture.active;
                try
                {
                    RenderTexture.active = rt;
                    GL.Clear(false, true, Color.clear);
                }
                finally
                {
                    RenderTexture.active = oldActive;
                }

                D3D11DepthBridge.IssueDepthRFloatFlushEvent();
                ForceD3D11DepthBridgeProbeFlush(rt, timingLabel, "D3D11 pending depth final flush");
            }
            finally
            {
                RenderTexture.ReleaseTemporary(rt);
            }
        }

        private static object GetPropertyValue(object instance, string propertyName)
        {
            if (instance == null || string.IsNullOrEmpty(propertyName))
                return null;

            try
            {
                var property = instance.GetType().GetProperty(propertyName);
                return property != null ? property.GetValue(instance, null) : null;
            }
            catch
            {
                return null;
            }
        }

        private static string GetTransformPath(Transform transform)
        {
            if (transform == null)
                return "<null>";

            var path = transform.name;
            var parent = transform.parent;
            while (parent != null)
            {
                path = parent.name + "/" + path;
                parent = parent.parent;
            }
            return path;
        }

        private static string FormatVec3(Vector3 value)
        {
            return value.x.ToString("0.###", CultureInfo.InvariantCulture) + "," +
                   value.y.ToString("0.###", CultureInfo.InvariantCulture) + "," +
                   value.z.ToString("0.###", CultureInfo.InvariantCulture);
        }

        private static string FormatRect(Rect value)
        {
            return value.x.ToString("0.###", CultureInfo.InvariantCulture) + "," +
                   value.y.ToString("0.###", CultureInfo.InvariantCulture) + "," +
                   value.width.ToString("0.###", CultureInfo.InvariantCulture) + "x" +
                   value.height.ToString("0.###", CultureInfo.InvariantCulture);
        }

        private static void AppendLine(StringBuilder builder, string key, object value)
        {
            builder.Append("[OfflineReShadeEnv] ").Append(key).Append("=").Append(value ?? "<null>").AppendLine();
        }

        private static void AppendCameraFingerprint(StringBuilder builder, string prefix, Camera cam)
        {
            if (cam == null)
            {
                AppendLine(builder, prefix, "<null>");
                return;
            }

            AppendLine(builder, prefix + ".name", cam.name);
            AppendLine(builder, prefix + ".path", GetTransformPath(cam.transform));
            AppendLine(builder, prefix + ".enabled", cam.enabled);
            AppendLine(builder, prefix + ".activeInHierarchy", cam.gameObject.activeInHierarchy);
            AppendLine(builder, prefix + ".tag", cam.tag);
            AppendLine(builder, prefix + ".actualRenderingPath", cam.actualRenderingPath);
            AppendLine(builder, prefix + ".renderingPath", cam.renderingPath);
            AppendLine(builder, prefix + ".depthTextureMode", cam.depthTextureMode);
            AppendLine(builder, prefix + ".clearFlags", cam.clearFlags);
            AppendLine(builder, prefix + ".cullingMask", "0x" + cam.cullingMask.ToString("X8", CultureInfo.InvariantCulture));
            AppendLine(builder, prefix + ".nearFar", cam.nearClipPlane.ToString("0.######", CultureInfo.InvariantCulture) + "/" + cam.farClipPlane.ToString("0.######", CultureInfo.InvariantCulture));
            AppendLine(builder, prefix + ".fovAspect", cam.fieldOfView.ToString("0.###", CultureInfo.InvariantCulture) + "/" + cam.aspect.ToString("0.######", CultureInfo.InvariantCulture));
            AppendLine(builder, prefix + ".rect", FormatRect(cam.rect));
            AppendLine(builder, prefix + ".pixelRect", FormatRect(cam.pixelRect));
            AppendLine(builder, prefix + ".depth", cam.depth);
            AppendLine(builder, prefix + ".position", FormatVec3(cam.transform.position));
            AppendLine(builder, prefix + ".rotation", FormatVec3(cam.transform.eulerAngles));
            AppendLine(builder, prefix + ".targetTexture", cam.targetTexture != null ? cam.targetTexture.width + "x" + cam.targetTexture.height + " depth=" + cam.targetTexture.depth + " format=" + cam.targetTexture.format : "<null>");
            AppendLine(builder, prefix + ".allowHDR", GetPropertyValue(cam, "allowHDR") ?? "<unavailable>");
            AppendLine(builder, prefix + ".allowMSAA", GetPropertyValue(cam, "allowMSAA") ?? "<unavailable>");

            var effects = cam.GetComponents<MonoBehaviour>();
            var effectList = new StringBuilder();
            for (var i = 0; i < effects.Length; i++)
            {
                var effect = effects[i];
                if (effect == null)
                    continue;

                if (effectList.Length > 0)
                    effectList.Append("; ");
                effectList.Append(effect.GetType().FullName).Append(" enabled=").Append(effect.enabled);
            }
            AppendLine(builder, prefix + ".monoBehaviours", effectList.Length > 0 ? effectList.ToString() : "<none>");
        }

        private static void LogKkEnvironmentFingerprint(string context, int width, int height, int downscaling, int renderWidth, int renderHeight, Camera cam)
        {
            if (OfflineReShadeKkEnvironmentFingerprintEnabled == null || !OfflineReShadeKkEnvironmentFingerprintEnabled.Value)
                return;

            var builder = new StringBuilder();
            AppendLine(builder, "context", context);
            AppendLine(builder, "requested", width + "x" + height + " downscaling=" + downscaling + " render=" + renderWidth + "x" + renderHeight);
            AppendLine(builder, "unity", Application.unityVersion + " platform=" + Application.platform + " product=" + Application.productName + " version=" + Application.version);
            AppendLine(builder, "app", "runInBackground=" + Application.runInBackground + " isFocused=" + Application.isFocused + " targetFrameRate=" + Application.targetFrameRate);
            AppendLine(builder, "system", SystemInfo.operatingSystem + " cpu=" + SystemInfo.processorType + " cores=" + SystemInfo.processorCount + " memMB=" + SystemInfo.systemMemorySize);
            AppendLine(builder, "graphics", SystemInfo.graphicsDeviceName + " vendor=" + SystemInfo.graphicsDeviceVendor + " version=" + SystemInfo.graphicsDeviceVersion + " type=" + SystemInfo.graphicsDeviceType + " memMB=" + SystemInfo.graphicsMemorySize + " multiThreaded=" + SystemInfo.graphicsMultiThreaded + " reversedZ=" + SystemInfo.usesReversedZBuffer);
            AppendLine(builder, "screen", Screen.width + "x" + Screen.height + " fullScreen=" + Screen.fullScreen + " currentResolution=" + Screen.currentResolution.width + "x" + Screen.currentResolution.height + "@" + Screen.currentResolution.refreshRate);
            AppendLine(builder, "quality", "level=" + QualitySettings.GetQualityLevel() + " aa=" + QualitySettings.antiAliasing + " vSync=" + QualitySettings.vSyncCount + " shadows=" + QualitySettings.shadows + " shadowDistance=" + QualitySettings.shadowDistance + " masterTextureLimit=" + QualitySettings.masterTextureLimit);
            AppendLine(builder, "time", "frame=" + Time.frameCount + " timeScale=" + Time.timeScale.ToString("0.###", CultureInfo.InvariantCulture) + " realtime=" + Time.realtimeSinceStartup.ToString("0.###", CultureInfo.InvariantCulture));
            AppendCameraFingerprint(builder, "camera.main", cam);

            var cameras = Camera.allCameras;
            AppendLine(builder, "allCameras.count", cameras != null ? cameras.Length : 0);
            if (cameras != null)
            {
                for (var i = 0; i < cameras.Length; i++)
                    AppendCameraFingerprint(builder, "allCameras[" + i + "]", cameras[i]);
            }

            var text = builder.ToString().TrimEnd();
            Logger.LogInfo(text);

            try
            {
                var logPath = OfflineReShadeD3D11DepthBridgeLogPath != null ? Path.GetFullPath(OfflineReShadeD3D11DepthBridgeLogPath.Value) : null;
                if (!string.IsNullOrEmpty(logPath))
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(logPath));
                    File.AppendAllText(logPath, text + Environment.NewLine, Encoding.UTF8);
                }
            }
            catch (Exception ex)
            {
                Logger.LogWarning("Offline ReShade KK environment fingerprint log write failed: " + ex.Message);
            }
        }

        private sealed class KkCameraState
        {
            public Camera Camera;
            public RenderTexture TargetTexture;
            public Rect Rect;
            public DepthTextureMode DepthTextureMode;
        }

        private static void AppendKkBridgeLogLine(string line)
        {
            Logger.LogInfo(line);
            try
            {
                var logPath = OfflineReShadeD3D11DepthBridgeLogPath != null ? Path.GetFullPath(OfflineReShadeD3D11DepthBridgeLogPath.Value) : null;
                if (!string.IsNullOrEmpty(logPath))
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(logPath));
                    File.AppendAllText(logPath, line + Environment.NewLine, Encoding.UTF8);
                }
            }
            catch (Exception ex)
            {
                Logger.LogWarning("Offline ReShade KK bridge log append failed: " + ex.Message);
            }
        }

        private static bool IsKkStackCamera(Camera root, Camera candidate)
        {
            if (root == null || candidate == null || !candidate.enabled || !candidate.gameObject.activeInHierarchy)
                return false;
            if (candidate == root)
                return true;
            return candidate.transform != null && root.transform != null && candidate.transform.IsChildOf(root.transform);
        }

        private static Camera[] GetKkOfflineCameraStack(Camera root)
        {
            var cameras = Camera.allCameras;
            if (cameras == null || cameras.Length == 0)
                return new[] { root };

            var stack = new System.Collections.Generic.List<Camera>();
            for (var i = 0; i < cameras.Length; i++)
            {
                var candidate = cameras[i];
                if (IsKkStackCamera(root, candidate) && !stack.Contains(candidate))
                    stack.Add(candidate);
            }

            if (!stack.Contains(root))
                stack.Add(root);

            stack.Sort((a, b) =>
            {
                var depthCompare = a.depth.CompareTo(b.depth);
                if (depthCompare != 0)
                    return depthCompare;
                return string.Compare(GetTransformPath(a.transform), GetTransformPath(b.transform), StringComparison.Ordinal);
            });
            return stack.ToArray();
        }

        private static KkCameraState[] ApplyKkCameraStackTarget(Camera[] stack, RenderTexture target)
        {
            var states = new KkCameraState[stack.Length];
            for (var i = 0; i < stack.Length; i++)
            {
                var camera = stack[i];
                states[i] = new KkCameraState
                {
                    Camera = camera,
                    TargetTexture = camera.targetTexture,
                    Rect = camera.rect,
                    DepthTextureMode = camera.depthTextureMode
                };
                camera.targetTexture = target;
                camera.rect = new Rect(0, 0, 1, 1);
                camera.depthTextureMode |= DepthTextureMode.Depth;
            }
            return states;
        }

        private static void RestoreKkCameraStackTarget(KkCameraState[] states)
        {
            if (states == null)
                return;

            for (var i = 0; i < states.Length; i++)
            {
                var state = states[i];
                if (state == null || state.Camera == null)
                    continue;

                state.Camera.targetTexture = state.TargetTexture;
                state.Camera.rect = state.Rect;
                state.Camera.depthTextureMode = state.DepthTextureMode;
            }
        }

        private static void RenderKkCameraStack(Camera root, RenderTexture target, string timingLabel)
        {
            var useStack = OfflineReShadeKkCameraStackRenderEnabled == null || OfflineReShadeKkCameraStackRenderEnabled.Value;
            var stack = useStack ? GetKkOfflineCameraStack(root) : new[] { root };
            KkCameraState[] states = null;
            try
            {
                states = ApplyKkCameraStackTarget(stack, target);
                AppendKkBridgeLogLine("[OfflineReShadeCameraStack] begin context=" + timingLabel + " count=" + stack.Length + " mode=" + (useStack ? "stack" : "main-only"));
                for (var i = 0; i < stack.Length; i++)
                {
                    var camera = stack[i];
                    AppendKkBridgeLogLine("[OfflineReShadeCameraStack] render index=" + i +
                        " name=" + camera.name +
                        " path=" + GetTransformPath(camera.transform) +
                        " depth=" + camera.depth.ToString(CultureInfo.InvariantCulture) +
                        " clearFlags=" + camera.clearFlags +
                        " cullingMask=0x" + camera.cullingMask.ToString("X8", CultureInfo.InvariantCulture) +
                        " actualRenderingPath=" + camera.actualRenderingPath +
                        " depthTextureMode=" + camera.depthTextureMode);
                    camera.Render();
                }
                AppendKkBridgeLogLine("[OfflineReShadeCameraStack] end context=" + timingLabel);
            }
            finally
            {
                RestoreKkCameraStackTarget(states);
            }
        }

        private static void LogKkDepthMissingHint(string context)
        {
            var reason = D3D11DepthBridge.GetLastError();
            Logger.LogWarning("Offline ReShade KK D3D11 depth was not written for " + context + ". Bridge reason: " + reason);
            if (_kkDepthMissingHintLogged)
                return;

            _kkDepthMissingHintLogged = true;
            Logger.LogWarning("Offline ReShade KK depth capture is most reliable from a Studio camera/lens view. If depth is missing, switch from free view to a camera view, disable MSAA, and disable Optimize in Background before retrying.");
        }
#endif

        /// <summary>
        /// Flushes any pending native depth readbacks submitted by offline ReShade exports.
        /// </summary>
        public static void FlushOfflineReShadeDepthReadbacks()
        {
#if KK
            FlushD3D11DepthBridgePendingReadbacks("VE end");
#endif
        }

        private void InitializeOfflineReShadeExport()
        {
            KeyExportOfflineReShade = Config.Bind(
                "Offline ReShade Export",
                "Export color and depth",
                new KeyboardShortcut(KeyCode.F10, KeyCode.LeftControl),
                "Exports coloroutput.png, depthoutput sidecar, and metadata.json for OfflineReShadeCapture.");

            OfflineReShadeExportDir = Config.Bind(
                "Offline ReShade Export",
                "Export directory",
                Path.Combine(ScreenshotDir, "OfflineReShade"),
                "Directory where OfflineReShadeCapture input files are written.");

            OfflineReShadeArchiveToScreenshotDir = Config.Bind(
                "Offline ReShade Export",
                "Archive color/depth in screenshot directory",
                true,
                "Also saves timestamped Color and Depth sidecar files to the normal screenshot directory for offline use.");

            OfflineReShadeLogTimings = Config.Bind(
                "Offline ReShade Export",
                "Log export timings",
                true,
                "Writes detailed color/depth export timing diagnostics to the game log.");

#if !KK
            OfflineReShadeDepthOutputFormat = Config.Bind(
                "Offline ReShade Export",
                "Depth output format",
                OfflineDepthOutputFormat.RawRFloat,
                "Depth sidecar format. RawRFloat writes little-endian float32 device depth. RawRgba8 writes raw little-endian RGBA8 packed device depth. PngRgba8 writes the previous RGBA8 packed PNG.");
#endif

#if KK
            OfflineReShadeD3D11DepthBridgeDllPath = Config.Bind(
                "Offline ReShade Export",
                "D3D11 bridge DLL path",
                "",
                "Optional absolute path to OfflineDepthD3D11Bridge.dll. If empty, the plugin searches next to Screencap.dll.");

            OfflineReShadeD3D11DepthBridgeLogPath = Config.Bind(
                "Offline ReShade Export",
                "D3D11 bridge log path",
                Path.Combine(Path.Combine(ScreenshotDir, "OfflineReShade"), "d3d11_depth_probe.log"),
                "Log file written by the D3D11 depth bridge probe.");

            OfflineReShadeD3D11CandidateDiagnosticsEnabled = Config.Bind(
                "Offline ReShade Export",
                "D3D11 bridge candidate diagnostics",
                false,
                "When enabled, the native D3D11 bridge samples every readable DSV candidate and logs nonzero/min/max stats. This is slow and intended only for debugging empty or wrong depth frames.");

            OfflineReShadeKkEnvironmentFingerprintEnabled = Config.Bind(
                "Offline ReShade Export",
                "KK environment fingerprint log",
                true,
                "Writes a KK-only environment/camera/settings fingerprint to the game log and D3D11 bridge log for comparing machines with unstable depth capture.");

            OfflineReShadeKkCameraStackRenderEnabled = Config.Bind(
                "Offline ReShade Export",
                "KK camera stack render test",
                true,
                "Test build option: render Camera.main and enabled child cameras to the offline target in camera.depth order so Studio camera stacks can write depth.");
#endif
        }

        private IEnumerator TakeOfflineReShadeExportScreenshot()
        {
            if (_currentAlphaShot == null)
            {
                Logger.Log(BepInEx.Logging.LogLevel.Message, "Can't export offline ReShade inputs here, try UI screenshot instead");
                yield break;
            }

            FirePreCapture();
            PlayCaptureSound();
            yield return new WaitForEndOfFrame();

            var width = ResolutionX.Value;
            var height = ResolutionY.Value;
            var downscaling = DownscalingRate.Value;
            var outputDir = Path.GetFullPath(OfflineReShadeExportDir.Value);
            Directory.CreateDirectory(outputDir);

            var colorPath = Path.Combine(outputDir, "coloroutput.png");
            var depthPath = Path.Combine(outputDir, "depthoutput" + GetOfflineReShadeDepthExtension());
            var metadataPath = Path.Combine(outputDir, "metadata.json");
            var archiveBaseTime = DateTime.Now;
            var archiveColorPath = OfflineReShadeArchiveToScreenshotDir.Value ? GetOfflineArchiveFilename("Color", archiveBaseTime) : null;
            var archiveDepthPath = OfflineReShadeArchiveToScreenshotDir.Value ? GetOfflineArchiveFilename("Depth", archiveBaseTime, GetOfflineReShadeDepthExtension()) : null;
            OfflineCaptureResult capture = null;

            try
            {
                capture = CaptureColorAndHardwareDepth(width, height, downscaling, "Ctrl+F10", depthPath);
                if (capture == null || capture.Color == null)
                    yield break;

                yield return WriteRenderTexturePng(capture.Color, colorPath, "Ctrl+F10 coloroutput");
                if (!string.IsNullOrEmpty(archiveColorPath))
                    yield return WriteRenderTexturePng(capture.Color, archiveColorPath, "Ctrl+F10 color archive");

#if KK
                if (capture.Depth == null)
                {
                    D3D11DepthBridge.IssueDepthRFloatFlushEvent();
                    ForceD3D11DepthBridgeProbeFlush(capture.Color, "Ctrl+F10", "D3D11 delayed depth Map flush");
                }
#endif

                if (capture.Depth != null)
                {
                    WriteDepthSidecar(capture.Depth, depthPath, Camera.main, capture.DepthKind, "Ctrl+F10 depthoutput");
                    if (!string.IsNullOrEmpty(archiveDepthPath))
                        WriteDepthSidecar(capture.Depth, archiveDepthPath, Camera.main, capture.DepthKind, "Ctrl+F10 depth archive");
                    UnityEngine.Object.DestroyImmediate(capture.Depth);
                    capture.Depth = null;
                }
#if KK
                else if (!string.IsNullOrEmpty(archiveDepthPath) && File.Exists(depthPath))
                {
                    File.Copy(depthPath, archiveDepthPath, true);
                }
                else if (!File.Exists(depthPath))
                {
                    LogKkDepthMissingHint("Ctrl+F10");
                }
#endif

                WriteOfflineMetadata(metadataPath, width, height, capture.DepthWidth, capture.DepthHeight, Camera.main, capture.DepthSource, downscaling);
                LogScreenshotMessage("offline ReShade export", colorPath);
                if (!string.IsNullOrEmpty(archiveColorPath))
                    LogScreenshotMessage("offline ReShade archive", archiveColorPath);
            }
            finally
            {
                if (capture != null)
                {
                    if (capture.Color != null)
                        RenderTexture.ReleaseTemporary(capture.Color);
                    if (capture.Depth != null)
                        UnityEngine.Object.DestroyImmediate(capture.Depth);
                }

                FirePostCapture();
            }
        }

        private static OfflineCaptureResult CaptureColorAndHardwareDepth(int width, int height, int downscalingRate, string timingLabel = null, string d3d11DepthRFloatPath = null)
        {
            var totalSw = Stopwatch.StartNew();
            var cam = Camera.main;
            if (cam == null) return null;

            var oldTarget = cam.targetTexture;
            var oldRect = cam.rect;
            var oldActive = RenderTexture.active;
            var oldDepthMode = cam.depthTextureMode;
            var safeDownscaling = Mathf.Max(1, downscalingRate);
            var renderWidth = width * safeDownscaling;
            var renderHeight = height * safeDownscaling;

            float oldDofBlurSize = 0.0f;
            var dof = cam.gameObject.GetComponent<UnityStandardAssets.ImageEffects.DepthOfField>();
            if (dof != null)
            {
                oldDofBlurSize = dof.maxBlurSize;
                dof.maxBlurSize = renderWidth * oldDofBlurSize / Screen.width;
            }

#if KK
            LogKkEnvironmentFingerprint(timingLabel ?? "Offline ReShade", width, height, safeDownscaling, renderWidth, renderHeight, cam);
#endif

            var colorRt = RenderTexture.GetTemporary(renderWidth, renderHeight, 24, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Default, 1);
            var depthKind = OfflineDepthTextureKind.DeviceRFloat;
#if !KK
            var depthFormat = RenderTextureFormat.RFloat;
#endif
            var useUnityHardwareDepthCopy = true;
#if KK
            useUnityHardwareDepthCopy = false;
#endif
            var depthBufferBits = useUnityHardwareDepthCopy ? 0 : 24;
            RenderTexture depthRt = null;
#if !KK
            depthRt = RenderTexture.GetTemporary(width, height, depthBufferBits, depthFormat, RenderTextureReadWrite.Linear, 1);
#endif
            colorRt.name = "OfflineReShade_Color";
            if (depthRt != null)
            {
                depthRt.name = GetDepthTextureName(depthKind);
                depthRt.filterMode = FilterMode.Point;
            }

            CommandBuffer depthCopy = null;
            if (useUnityHardwareDepthCopy)
            {
                depthCopy = new CommandBuffer { name = "Offline ReShade depth copy" };
                depthCopy.Blit(BuiltinRenderTextureType.Depth, depthRt);
            }

            try
            {
                cam.depthTextureMode |= DepthTextureMode.Depth;
                if (depthCopy != null)
                    cam.AddCommandBuffer(CameraEvent.AfterEverything, depthCopy);
#if !KK
                cam.targetTexture = colorRt;
                cam.rect = new Rect(0, 0, 1, 1);
#endif

                RenderTexture.active = colorRt;
                GL.Clear(true, true, Color.clear);
                var renderSw = Stopwatch.StartNew();
#if KK
                var d3d11ProbeActive = D3D11DepthBridge.TryBegin(renderWidth, renderHeight, timingLabel);
                try
                {
                    if (d3d11ProbeActive)
                    {
                        D3D11DepthBridge.SetUnityD3D11Texture(colorRt);
                        D3D11DepthBridge.IssueRenderThreadHookEvent();
                        ForceD3D11DepthBridgeProbeFlush(colorRt, timingLabel, "D3D11 render-thread hook flush");
                    }
                    RenderKkCameraStack(cam, colorRt, timingLabel);
                    if (d3d11ProbeActive && !string.IsNullOrEmpty(d3d11DepthRFloatPath))
                        D3D11DepthBridge.IssueDepthRFloatReadbackEvent(d3d11DepthRFloatPath);
                }
                finally
                {
                    if (d3d11ProbeActive)
                        D3D11DepthBridge.End();
                }
#else
                cam.Render();
#endif
                renderSw.Stop();
                LogOfflineTiming(timingLabel, "camera render + depth blit", renderSw.Elapsed);

#if KK
                Texture2D depthTex = null;
#else
                Texture2D depthTex;
                ReadDepthTexture(depthRt, width, height, depthKind, out depthTex, timingLabel, "depth ReadPixels+Apply");
#endif

                if (safeDownscaling > 1)
                {
                    var colorScaleSw = Stopwatch.StartNew();
                    colorRt = AlphaShot2.LanczosTex(colorRt, width, height);
                    colorScaleSw.Stop();
                    LogOfflineTiming(timingLabel, "color Lanczos downscale", colorScaleSw.Elapsed);

#if !KK
                    LogOfflineTiming(timingLabel, "depth GPU blit downscale", TimeSpan.Zero);
#endif
                }

                return new OfflineCaptureResult
                {
                    Color = colorRt,
                    Depth = depthTex,
                    DepthSource = GetDepthCaptureSource(safeDownscaling, depthKind),
                    DepthKind = depthKind,
#if KK
                    DepthWidth = renderWidth,
                    DepthHeight = renderHeight
#else
                    DepthWidth = width,
                    DepthHeight = height
#endif
                };
            }
            catch (Exception ex)
            {
                Logger.LogWarning("Offline ReShade hardware depth export failed: " + ex.Message);
                RenderTexture.ReleaseTemporary(colorRt);
                return null;
            }
            finally
            {
                totalSw.Stop();
                LogOfflineTiming(timingLabel, "capture total before encode/write", totalSw.Elapsed);
                if (depthCopy != null)
                {
                    try { cam.RemoveCommandBuffer(CameraEvent.AfterEverything, depthCopy); }
                    catch { }
                    depthCopy.Release();
                }
                if (depthRt != null)
                    RenderTexture.ReleaseTemporary(depthRt);
                cam.targetTexture = oldTarget;
                cam.rect = oldRect;
                cam.depthTextureMode = oldDepthMode;
                if (dof != null)
                    dof.maxBlurSize = oldDofBlurSize;
                RenderTexture.active = oldActive;
            }
        }

        /// <summary>
        /// Exports a color/depth/metadata set for external offline ReShade tools.
        /// </summary>
        public static bool ExportOfflineReShadeInputs(string colorPath, string depthPath, string metadataPath, int? width = null, int? height = null, int? downscaling = null)
        {
            var captureWidth = width ?? ResolutionX.Value;
            var captureHeight = height ?? ResolutionY.Value;
            var captureDownscaling = downscaling ?? DownscalingRate.Value;
            OfflineCaptureResult capture = null;
            var totalSw = Stopwatch.StartNew();
            var frameLabel = "VE export " + Path.GetFileNameWithoutExtension(colorPath);

            try
            {
                capture = CaptureColorAndHardwareDepth(captureWidth, captureHeight, captureDownscaling, frameLabel, depthPath);
                if (capture == null || capture.Color == null)
                    return false;

                var dirSw = Stopwatch.StartNew();
                Directory.CreateDirectory(Path.GetDirectoryName(colorPath));
                Directory.CreateDirectory(Path.GetDirectoryName(depthPath));
                Directory.CreateDirectory(Path.GetDirectoryName(metadataPath));
                dirSw.Stop();
                LogOfflineTiming(frameLabel, "ensure directories", dirSw.Elapsed);

                WriteRenderTexturePngSync(capture.Color, colorPath, frameLabel + " color png");
                if (capture.Depth != null)
                    WriteDepthSidecar(capture.Depth, depthPath, Camera.main, capture.DepthKind, frameLabel + " depth");
                var metadataSw = Stopwatch.StartNew();
                WriteOfflineMetadata(metadataPath, captureWidth, captureHeight, capture.DepthWidth, capture.DepthHeight, Camera.main, capture.DepthSource, captureDownscaling);
                metadataSw.Stop();
                LogOfflineTiming(frameLabel, "metadata write", metadataSw.Elapsed);
#if KK
                if (capture.Depth == null)
                {
                    var queueResult = D3D11DepthBridge.GetLastDepthQueueResult();
                    if (queueResult == 0)
                    {
                        ForceD3D11DepthBridgeProbeFlush(capture.Color, frameLabel, "D3D11 depth queue event flush");
                        queueResult = D3D11DepthBridge.GetLastDepthQueueResult();
                    }

                    var hasDepthFile = queueResult > 0 || File.Exists(depthPath);
                    if (!hasDepthFile)
                        LogKkDepthMissingHint(frameLabel);
                    return hasDepthFile;
                }
#endif
                return capture.Depth != null || File.Exists(depthPath);
            }
            catch (Exception ex)
            {
                Logger.LogWarning("Offline ReShade explicit input export failed: " + ex.Message);
                return false;
            }
            finally
            {
                totalSw.Stop();
                LogOfflineTiming(frameLabel, "ExportOfflineReShadeInputs total", totalSw.Elapsed);
                if (capture != null)
                {
                    if (capture.Color != null)
                        RenderTexture.ReleaseTemporary(capture.Color);
                    if (capture.Depth != null)
                        UnityEngine.Object.DestroyImmediate(capture.Depth);
                }
            }
        }

        /// <summary>
        /// Gets the current depth sidecar extension used by offline ReShade exports.
        /// </summary>
        public static string GetOfflineReShadeDepthExtension()
        {
            switch (GetDepthOutputFormat())
            {
                case OfflineDepthOutputFormat.RawRFloat:
                    return ".rfloat";
                case OfflineDepthOutputFormat.RawRgba8:
                    return ".rgba8";
                case OfflineDepthOutputFormat.PngRgba8:
                    return ".png";
                default:
                    return ".rfloat";
            }
        }

        private static OfflineDepthOutputFormat GetDepthOutputFormat()
        {
#if KK
            return OfflineDepthOutputFormat.RawRFloat;
#else
            if (OfflineReShadeDepthOutputFormat != null)
                return OfflineReShadeDepthOutputFormat.Value;
            return OfflineDepthOutputFormat.RawRFloat;
#endif
        }

        private static string GetDepthEncoding()
        {
            return GetDepthOutputFormat() == OfflineDepthOutputFormat.RawRFloat
                ? "rfloat32_device_depth_little_endian"
                : "rgba8_unorm_32_device_depth";
        }

        private static string GetDepthTextureName(OfflineDepthTextureKind depthKind)
        {
            return "OfflineReShade_DeviceDepth_RFloat";
        }

        private static string GetDepthCaptureSource(int safeDownscaling, OfflineDepthTextureKind depthKind)
        {
#if KK
            return "d3d11_depth_stencil_bridge_x" + safeDownscaling;
#else
            return "camera_depth_texture_after_everything_gpu_downscaled_x" + safeDownscaling;
#endif
        }

        private static void ReadDepthTexture(RenderTexture depthRt, int width, int height, OfflineDepthTextureKind depthKind, out Texture2D depthTex, string timingLabel, string timingStep)
        {
            RenderTexture.active = depthRt;
            var format = depthKind == OfflineDepthTextureKind.DeviceRFloat ? TextureFormat.RFloat : TextureFormat.RGBA32;
            depthTex = new Texture2D(width, height, format, false, true);
            var depthReadSw = Stopwatch.StartNew();
            depthTex.ReadPixels(new Rect(0, 0, width, height), 0, 0);
            depthTex.Apply(false, false);
            depthReadSw.Stop();
            LogOfflineTiming(timingLabel, timingStep, depthReadSw.Elapsed);
        }

        private static string GetOfflineArchiveFilename(string capType, DateTime timestamp)
        {
            return GetOfflineArchiveFilename(capType, timestamp, ".png");
        }

        private static string GetOfflineArchiveFilename(string capType, DateTime timestamp, string extension)
        {
            var productName = !string.IsNullOrEmpty(ScreenshotNameOverride.Value)
                ? ScreenshotNameOverride.Value
                : Application.productName.Replace(" ", "");

            string filename;
            switch (ScreenshotNameFormat.Value)
            {
                case NameFormat.NameDate:
                    filename = $"{productName}-{timestamp:yyyy-MM-dd-HH-mm-ss}-{capType}{extension}";
                    break;
                case NameFormat.NameTypeDate:
                    filename = $"{productName}-{capType}-{timestamp:yyyy-MM-dd-HH-mm-ss}{extension}";
                    break;
                case NameFormat.NameDateType:
                    filename = $"{productName}-{timestamp:yyyy-MM-dd-HH-mm-ss}-{capType}{extension}";
                    break;
                case NameFormat.TypeDate:
                    filename = $"{capType}-{timestamp:yyyy-MM-dd-HH-mm-ss}{extension}";
                    break;
                case NameFormat.TypeNameDate:
                    filename = $"{capType}-{productName}-{timestamp:yyyy-MM-dd-HH-mm-ss}{extension}";
                    break;
                case NameFormat.Date:
                    filename = $"{timestamp:yyyy-MM-dd-HH-mm-ss}-{capType}{extension}";
                    break;
                default:
                    throw new ArgumentOutOfRangeException("Unhandled screenshot filename format - " + ScreenshotNameFormat.Value);
            }

            return Path.GetFullPath(Path.Combine(ScreenshotDir, filename));
        }

        private static IEnumerator WriteRenderTexturePng(RenderTexture result, string filename, string timingLabel = null)
        {
            WriteRenderTexturePngSync(result, filename, timingLabel);
            yield return null;
        }

        private static void WriteRenderTexturePngSync(RenderTexture result, string filename, string timingLabel = null)
        {
            var currentActiveRT = RenderTexture.active;
            RenderTexture.active = result;
            var tex = new Texture2D(result.width, result.height, TextureFormat.RGBA32, false);
            var readSw = Stopwatch.StartNew();
            tex.ReadPixels(new Rect(0, 0, tex.width, tex.height), 0, 0);
            tex.Apply();
            readSw.Stop();
            RenderTexture.active = currentActiveRT;
            LogOfflineTiming(timingLabel, "color ReadPixels+Apply", readSw.Elapsed);

            var alphaSw = Stopwatch.StartNew();
            var pixels = tex.GetPixels32();
            for (var i = 0; i < pixels.Length; i++)
                pixels[i].a = 255;
            tex.SetPixels32(pixels);
            tex.Apply(false, false);
            alphaSw.Stop();
            LogOfflineTiming(timingLabel, "color alpha normalize", alphaSw.Elapsed);

            var encodeSw = Stopwatch.StartNew();
            var encoded = tex.EncodeToPNG();
            encodeSw.Stop();
            LogOfflineTiming(timingLabel, "color EncodeToPNG", encodeSw.Elapsed);

            var writeSw = Stopwatch.StartNew();
            File.WriteAllBytes(filename, encoded);
            writeSw.Stop();
            LogOfflineTiming(timingLabel, "color file write", writeSw.Elapsed);
            UnityEngine.Object.DestroyImmediate(tex);
        }

        private static void WriteDepthSidecar(Texture2D deviceDepthTexture, string filename, Camera cam, OfflineDepthTextureKind depthKind, string timingLabel = null)
        {
            switch (GetDepthOutputFormat())
            {
                case OfflineDepthOutputFormat.RawRFloat:
                    WriteRawDepthRFloat(deviceDepthTexture, filename, cam, depthKind, timingLabel);
                    break;
                case OfflineDepthOutputFormat.RawRgba8:
                    WriteRawDepthRgba8(deviceDepthTexture, filename, cam, depthKind, timingLabel);
                    break;
                case OfflineDepthOutputFormat.PngRgba8:
                    WriteEncodedDepthPng(deviceDepthTexture, filename, cam, depthKind, timingLabel);
                    break;
            }
        }

        private static void WriteRawDepthRFloat(Texture2D deviceDepthTexture, string filename, Camera cam, OfflineDepthTextureKind depthKind, string timingLabel = null)
        {
            var rawSw = Stopwatch.StartNew();
            var raw = deviceDepthTexture.GetRawTextureData();
            rawSw.Stop();
            LogOfflineTiming(timingLabel, "depth GetRawTextureData", rawSw.Elapsed);

            var writeSw = Stopwatch.StartNew();
            File.WriteAllBytes(filename, raw);
            writeSw.Stop();
            LogOfflineTiming(timingLabel, "depth raw rfloat file write", writeSw.Elapsed);
        }

        private static void WriteRawDepthRgba8(Texture2D deviceDepthTexture, string filename, Camera cam, OfflineDepthTextureKind depthKind, string timingLabel = null)
        {
            var raw = GetPackedRgba8DepthBytes(deviceDepthTexture, cam, depthKind, timingLabel);

            var writeSw = Stopwatch.StartNew();
            File.WriteAllBytes(filename, raw);
            writeSw.Stop();
            LogOfflineTiming(timingLabel, "depth raw rgba8 file write", writeSw.Elapsed);
        }

        private static Color32 PackDeviceDepthToRgba8(float depth)
        {
            if (float.IsNaN(depth) || float.IsInfinity(depth))
                depth = 0f;

            var value = (uint)Math.Round(Mathf.Clamp01(depth) * uint.MaxValue);
            return new Color32(
                (byte)(value & 0xFF),
                (byte)((value >> 8) & 0xFF),
                (byte)((value >> 16) & 0xFF),
                (byte)((value >> 24) & 0xFF));
        }

        private static float[] GetDeviceDepthValues(Texture2D depthTexture, Camera cam, OfflineDepthTextureKind depthKind, string timingLabel)
        {
            var rawSw = Stopwatch.StartNew();
            var raw = depthTexture.GetRawTextureData();
            rawSw.Stop();
            LogOfflineTiming(timingLabel, "depth GetRawTextureData", rawSw.Elapsed);

            var values = new float[depthTexture.width * depthTexture.height];
            Buffer.BlockCopy(raw, 0, values, 0, Math.Min(raw.Length, values.Length * sizeof(float)));
            return values;
        }

        private static byte[] GetPackedRgba8DepthBytes(Texture2D depthTexture, Camera cam, OfflineDepthTextureKind depthKind, string timingLabel)
        {
            var values = GetDeviceDepthValues(depthTexture, cam, depthKind, timingLabel);
            var packSw = Stopwatch.StartNew();
            var rawBytes = new byte[values.Length * 4];
            for (var i = 0; i < values.Length; i++)
            {
                var packed = PackDeviceDepthToRgba8(values[i]);
                var offset = i * 4;
                rawBytes[offset] = packed.r;
                rawBytes[offset + 1] = packed.g;
                rawBytes[offset + 2] = packed.b;
                rawBytes[offset + 3] = packed.a;
            }
            packSw.Stop();
            LogOfflineTiming(timingLabel, "depth RGBA8 pack", packSw.Elapsed);
            return rawBytes;
        }

        private static void WriteEncodedDepthPng(Texture2D deviceDepthTexture, string filename, Camera cam, OfflineDepthTextureKind depthKind, string timingLabel = null)
        {
            var rawBytes = GetPackedRgba8DepthBytes(deviceDepthTexture, cam, depthKind, timingLabel);

            var encoded = new Texture2D(deviceDepthTexture.width, deviceDepthTexture.height, TextureFormat.RGBA32, false, true);
            var encodedPixels = new Color32[deviceDepthTexture.width * deviceDepthTexture.height];

            for (var i = 0; i < encodedPixels.Length; i++)
            {
                var offset = i * 4;
                encodedPixels[i] = new Color32(
                    rawBytes[offset],
                    rawBytes[offset + 1],
                    rawBytes[offset + 2],
                    rawBytes[offset + 3]);
            }

            var setSw = Stopwatch.StartNew();
            encoded.SetPixels32(encodedPixels);
            encoded.Apply(false, false);
            setSw.Stop();
            LogOfflineTiming(timingLabel, "depth SetPixels32+Apply", setSw.Elapsed);

            var encodeSw = Stopwatch.StartNew();
            var bytes = encoded.EncodeToPNG();
            encodeSw.Stop();
            LogOfflineTiming(timingLabel, "depth EncodeToPNG", encodeSw.Elapsed);

            var writeSw = Stopwatch.StartNew();
            File.WriteAllBytes(filename, bytes);
            writeSw.Stop();
            LogOfflineTiming(timingLabel, "depth file write", writeSw.Elapsed);
            UnityEngine.Object.DestroyImmediate(encoded);
        }

        private static void LogOfflineTiming(string label, string step, TimeSpan elapsed)
        {
            if (OfflineReShadeLogTimings == null || !OfflineReShadeLogTimings.Value || string.IsNullOrEmpty(label))
                return;

            Logger.LogInfo($"[OfflineReShadeTiming] {label}: {step} = {elapsed.TotalMilliseconds:0.0} ms");
        }

        private static void WriteOfflineMetadata(string filename, int width, int height, int depthWidth, int depthHeight, Camera cam, string depthSource, int downscaling)
        {
            var invariant = CultureInfo.InvariantCulture;
            var sb = new StringBuilder();
            sb.AppendLine("{");
            sb.AppendLine($"  \"width\": {width},");
            sb.AppendLine($"  \"height\": {height},");
            sb.AppendLine($"  \"depthWidth\": {depthWidth},");
            sb.AppendLine($"  \"depthHeight\": {depthHeight},");
            sb.AppendLine($"  \"nearClip\": {cam.nearClipPlane.ToString(invariant)},");
            sb.AppendLine($"  \"farClip\": {cam.farClipPlane.ToString(invariant)},");
            sb.AppendLine($"  \"fov\": {cam.fieldOfView.ToString(invariant)},");
            sb.AppendLine($"  \"aspect\": {cam.aspect.ToString(invariant)},");
            sb.AppendLine($"  \"downscalingRate\": {downscaling},");
            sb.AppendLine("  \"colorResolve\": \"screenshotmanager_lanczos\",");
            sb.AppendLine($"  \"depthEncoding\": \"{GetDepthEncoding()}\",");
            sb.AppendLine($"  \"depthFileExtension\": \"{GetOfflineReShadeDepthExtension()}\",");
            sb.AppendLine("  \"depthByteOrder\": \"little_endian\",");
            sb.AppendLine("  \"depthRows\": \"bottom_to_top\",");
            sb.AppendLine($"  \"depthCaptureSource\": \"{depthSource}\",");
            sb.AppendLine($"  \"reshadeDepthReversed\": {SystemInfo.usesReversedZBuffer.ToString().ToLowerInvariant()},");
            sb.AppendLine("  \"reshadeDepthUpsideDown\": false,");
            sb.AppendLine("  \"reshadeFarPlane\": 1000");
            sb.AppendLine("}");
            File.WriteAllText(filename, sb.ToString());
        }
    }
}
