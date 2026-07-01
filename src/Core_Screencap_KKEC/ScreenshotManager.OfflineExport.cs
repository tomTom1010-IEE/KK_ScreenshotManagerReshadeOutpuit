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
        private static ConfigEntry<KkDepthCaptureMode> OfflineReShadeKkDepthCaptureMode { get; set; }
        private static ConfigEntry<string> OfflineReShadeKkPackedDepthBundlePath { get; set; }
        private static ConfigEntry<string> OfflineReShadeD3D11DepthBridgeDllPath { get; set; }
        private static ConfigEntry<string> OfflineReShadeD3D11DepthBridgeLogPath { get; set; }
        private static ConfigEntry<bool> OfflineReShadeD3D11CandidateDiagnosticsEnabled { get; set; }
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

#if KK
        private enum KkDepthCaptureMode
        {
            CustomReplacementPackedRgba8,
            UnityDepthNormalsFallback,
            ExperimentalHardwareDepth
        }
#endif

        private enum OfflineDepthTextureKind
        {
            DeviceRFloat,
            DevicePackedRgba8,
            UnityDepthNormals
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
        private static Material KkPackedDepthMaterial;
        private static AssetBundle KkPackedDepthBundle;
        private static Shader KkPackedDepthShader;
        private const string KkPackedDepthShaderName = "Hidden/OfflineReShade/KKPackedDeviceDepth";
        private const string KkPackedDepthBundleFileName = "OfflineReShadePackedDepth.unity3d";

        private static readonly string KkPackedDeviceDepthShaderSource =
            "Shader \"" + KkPackedDepthShaderName + "\"\n" +
            "{\n" +
            "    SubShader\n" +
            "    {\n" +
            "        Tags { \"RenderType\"=\"Opaque\" }\n" +
            "        Pass\n" +
            "        {\n" +
            "            Cull Back\n" +
            "            ZWrite On\n" +
            "            ZTest LEqual\n" +
            "\n" +
            "            CGPROGRAM\n" +
            "            #pragma vertex vert\n" +
            "            #pragma fragment frag\n" +
            "            #include \"UnityCG.cginc\"\n" +
            "\n" +
            "            struct v2f\n" +
            "            {\n" +
            "                float4 pos : SV_POSITION;\n" +
            "                float depth : TEXCOORD0;\n" +
            "            };\n" +
            "\n" +
            "            v2f vert(appdata_base v)\n" +
            "            {\n" +
            "                v2f o;\n" +
            "                o.pos = UnityObjectToClipPos(v.vertex);\n" +
            "                o.depth = o.pos.z / o.pos.w;\n" +
            "                return o;\n" +
            "            }\n" +
            "\n" +
            "            float4 PackDepthRgba8LE(float depth)\n" +
            "            {\n" +
            "                depth = saturate(depth);\n" +
            "                float scaled = floor(depth * 4294967295.0 + 0.5);\n" +
            "                float r = scaled - floor(scaled / 256.0) * 256.0;\n" +
            "                scaled = floor(scaled / 256.0);\n" +
            "                float g = scaled - floor(scaled / 256.0) * 256.0;\n" +
            "                scaled = floor(scaled / 256.0);\n" +
            "                float b = scaled - floor(scaled / 256.0) * 256.0;\n" +
            "                scaled = floor(scaled / 256.0);\n" +
            "                float a = scaled - floor(scaled / 256.0) * 256.0;\n" +
            "                return float4(r, g, b, a) / 255.0;\n" +
            "            }\n" +
            "\n" +
            "            fixed4 frag(v2f i) : SV_Target\n" +
            "            {\n" +
            "                return PackDepthRgba8LE(i.depth);\n" +
            "            }\n" +
            "            ENDCG\n" +
            "        }\n" +
            "    }\n" +
            "    Fallback Off\n" +
            "}";

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

            private static string GetLastError()
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
                "Also saves timestamped Color and Depth PNG files to the normal screenshot directory for offline use.");

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
                cam.targetTexture = colorRt;
                cam.rect = new Rect(0, 0, 1, 1);

                RenderTexture.active = colorRt;
                GL.Clear(true, true, Color.clear);
                var renderSw = Stopwatch.StartNew();
#if KK
                var d3d11ProbeActive = D3D11DepthBridge.TryBegin(renderWidth, renderHeight, timingLabel);
                if (!d3d11ProbeActive)
                    throw new InvalidOperationException("D3D11 depth bridge failed to begin capture.");
                try
                {
                    D3D11DepthBridge.SetUnityD3D11Texture(colorRt);
                    cam.Render();
                    if (!string.IsNullOrEmpty(d3d11DepthRFloatPath))
                        D3D11DepthBridge.IssueDepthRFloatReadbackEvent(d3d11DepthRFloatPath);
                }
                finally
                {
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

#if KK
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

                    return queueResult > 0 || File.Exists(depthPath);
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
            switch (depthKind)
            {
                case OfflineDepthTextureKind.DeviceRFloat:
                    return "OfflineReShade_DeviceDepth_RFloat";
                case OfflineDepthTextureKind.DevicePackedRgba8:
                    return "OfflineReShade_DeviceDepth_PackedRGBA8";
                case OfflineDepthTextureKind.UnityDepthNormals:
                    return "OfflineReShade_DepthNormalsFallback";
                default:
                    return "OfflineReShade_Depth";
            }
        }

        private static string GetDepthCaptureSource(int safeDownscaling, OfflineDepthTextureKind depthKind)
        {
#if KK
            switch (depthKind)
            {
                case OfflineDepthTextureKind.DevicePackedRgba8:
                    return "custom_replacement_packed_rgba8_device_depth_x" + safeDownscaling;
                case OfflineDepthTextureKind.UnityDepthNormals:
                    return "unity_internal_depthnormals_replacement_x" + safeDownscaling;
                case OfflineDepthTextureKind.DeviceRFloat:
                    return "d3d11_depth_stencil_bridge_x" + safeDownscaling;
                default:
                    return "unknown_depth_source_x" + safeDownscaling;
            }
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

#if KK
        private static string[] GetKkPackedDepthBundleCandidates()
        {
            var pluginDir = Path.GetDirectoryName(typeof(ScreenshotManager).Assembly.Location);
            return new[]
            {
                OfflineReShadeKkPackedDepthBundlePath != null ? OfflineReShadeKkPackedDepthBundlePath.Value : null,
                !string.IsNullOrEmpty(pluginDir) ? Path.Combine(pluginDir, KkPackedDepthBundleFileName) : null,
                !string.IsNullOrEmpty(pluginDir) ? Path.Combine(pluginDir, KkPackedDepthBundleFileName.ToLowerInvariant()) : null,
                !string.IsNullOrEmpty(pluginDir) ? Path.Combine(Path.Combine(Path.Combine(pluginDir, "hidden"), "offlinereshade"), KkPackedDepthBundleFileName.ToLowerInvariant()) : null,
                !string.IsNullOrEmpty(pluginDir) ? Path.Combine(Path.Combine(Path.Combine(pluginDir, "Hidden"), "OfflineReShade"), KkPackedDepthBundleFileName) : null
            };
        }

        private static Shader FindKkDepthShaderInBundle(AssetBundle bundle, string shaderName)
        {
            if (bundle == null)
                return null;

            var shaders = bundle.LoadAllAssets(typeof(Shader));
            for (var i = 0; i < shaders.Length; i++)
            {
                var shader = shaders[i] as Shader;
                if (shader != null && shader.name == shaderName)
                    return shader;
            }

            var materials = bundle.LoadAllAssets(typeof(Material));
            for (var i = 0; i < materials.Length; i++)
            {
                var material = materials[i] as Material;
                if (material != null && material.shader != null && material.shader.name == shaderName)
                    return material.shader;
            }

            var prefabs = bundle.LoadAllAssets(typeof(GameObject));
            for (var i = 0; i < prefabs.Length; i++)
            {
                var go = prefabs[i] as GameObject;
                if (go == null)
                    continue;

                var renderers = go.GetComponentsInChildren<Renderer>(true);
                for (var r = 0; r < renderers.Length; r++)
                {
                    var renderer = renderers[r];
                    if (renderer == null || renderer.sharedMaterials == null)
                        continue;

                    var rendererMaterials = renderer.sharedMaterials;
                    for (var m = 0; m < rendererMaterials.Length; m++)
                    {
                        var material = rendererMaterials[m];
                        if (material != null && material.shader != null && material.shader.name == shaderName)
                            return material.shader;
                    }
                }
            }

            return null;
        }

        private static void UnloadKkDepthBundleIfUnused()
        {
            if (KkPackedDepthShader != null || KkPackedDepthBundle == null)
                return;

            KkPackedDepthBundle.Unload(false);
            KkPackedDepthBundle = null;
        }

        private static bool TryLoadKkDepthShaderFromBundle(string shaderName, ref Shader cachedShader, string label, out Shader shader)
        {
            shader = cachedShader;
            if (shader != null && shader.isSupported)
                return true;

            var candidates = GetKkPackedDepthBundleCandidates();
            for (var i = 0; i < candidates.Length; i++)
            {
                var path = candidates[i];
                if (string.IsNullOrEmpty(path))
                    continue;

                path = Path.GetFullPath(path);
                if (!File.Exists(path))
                    continue;

                try
                {
                    if (KkPackedDepthBundle == null)
                        KkPackedDepthBundle = AssetBundle.LoadFromFile(path);

                    shader = FindKkDepthShaderInBundle(KkPackedDepthBundle, shaderName);
                    if (shader != null && shader.isSupported)
                    {
                        cachedShader = shader;
                        Logger.LogInfo("Offline ReShade: loaded KK " + label + " depth shader bundle from " + path);
                        return true;
                    }

                    Logger.LogWarning("Offline ReShade KK depth bundle did not contain supported shader " + shaderName + ": " + path);
                    UnloadKkDepthBundleIfUnused();
                }
                catch (Exception ex)
                {
                    Logger.LogWarning("Offline ReShade KK depth bundle failed to load from " + path + ": " + ex.Message);
                    if (KkPackedDepthBundle != null)
                    {
                        KkPackedDepthBundle.Unload(false);
                        KkPackedDepthBundle = null;
                    }
                }
            }

            return false;
        }

        private static bool TryLoadKkPackedDepthShaderFromBundle(out Shader shader)
        {
            return TryLoadKkDepthShaderFromBundle(KkPackedDepthShaderName, ref KkPackedDepthShader, "packed RGBA8", out shader);
        }

        private static bool TryGetKkPackedDepthShader(out Shader shader)
        {
            shader = null;

            if (TryLoadKkPackedDepthShaderFromBundle(out shader))
                return true;

            try
            {
                if (KkPackedDepthMaterial == null)
                {
                    KkPackedDepthMaterial = new Material(KkPackedDeviceDepthShaderSource);
                    KkPackedDepthMaterial.hideFlags = HideFlags.HideAndDontSave;
                }

                shader = KkPackedDepthMaterial.shader;
                if (shader == null || !shader.isSupported)
                {
                    Logger.LogWarning("Offline ReShade KK custom packed RGBA8 depth shader is not supported.");
                    return false;
                }

                return true;
            }
            catch (Exception ex)
            {
                Logger.LogWarning("Offline ReShade KK custom packed RGBA8 depth shader could not be created: " + ex.Message);
                return false;
            }
        }

        private static bool RenderKkCustomPackedDepth(Camera cam, RenderTexture depthRt, int width, int height)
        {
            Shader packedDepthShader;
            if (!TryGetKkPackedDepthShader(out packedDepthShader))
                return false;

            var oldTarget = cam.targetTexture;
            var oldRect = cam.rect;
            var oldActive = RenderTexture.active;
            var oldClearFlags = cam.clearFlags;
            var oldBackground = cam.backgroundColor;
            var farDepthColor = SystemInfo.usesReversedZBuffer ? Color.clear : Color.white;

            try
            {
                cam.targetTexture = depthRt;
                cam.rect = new Rect(0, 0, 1, 1);
                cam.clearFlags = CameraClearFlags.SolidColor;
                cam.backgroundColor = farDepthColor;
                RenderTexture.active = depthRt;
                GL.Clear(true, true, farDepthColor);
                cam.RenderWithShader(packedDepthShader, "");
                Logger.LogInfo("Offline ReShade: Custom replacement packed RGBA8 depth rendered.");
                return true;
            }
            finally
            {
                cam.targetTexture = oldTarget;
                cam.rect = oldRect;
                cam.clearFlags = oldClearFlags;
                cam.backgroundColor = oldBackground;
                RenderTexture.active = oldActive;
            }
        }

        private static bool IsKkHardwareDepthValid(Texture2D depthTex, string timingLabel)
        {
            if (depthTex == null || depthTex.format != TextureFormat.RFloat)
                return false;

            var rawSw = Stopwatch.StartNew();
            var raw = depthTex.GetRawTextureData();
            rawSw.Stop();
            LogOfflineTiming(timingLabel, "KK hardware depth validation GetRawTextureData", rawSw.Elapsed);

            if (raw == null || raw.Length != depthTex.width * depthTex.height * sizeof(float))
                return false;

            var values = new float[depthTex.width * depthTex.height];
            Buffer.BlockCopy(raw, 0, values, 0, raw.Length);

            var valid = 0;
            var invalid = 0;
            var min = float.PositiveInfinity;
            var max = float.NegativeInfinity;
            var sum = 0.0;

            for (var i = 0; i < values.Length; i++)
            {
                var d = values[i];
                if (float.IsNaN(d) || float.IsInfinity(d) || d < -0.0001f || d > 1.0001f)
                {
                    invalid++;
                    continue;
                }

                valid++;
                if (d < min) min = d;
                if (d > max) max = d;
                sum += d;
            }

            var validRatio = values.Length > 0 ? (double)valid / values.Length : 0.0;
            var mean = valid > 0 ? sum / valid : 0.0;
            Logger.LogInfo($"[OfflineReShadeTiming] {timingLabel}: KK hardware depth stats valid={validRatio:P1}, invalid={invalid}, min={min:0.000000}, max={max:0.000000}, mean={mean:0.000000}");

            return validRatio > 0.95 && max - min > 0.000001f;
        }

        private static bool RenderKkDepthNormalsFallback(Camera cam, RenderTexture depthRt, int width, int height)
        {
            var depthNormalsShader = Shader.Find("Hidden/Internal-DepthNormalsTexture");
            if (depthNormalsShader == null)
            {
                Logger.LogWarning("Offline ReShade KK depth export failed: Hidden/Internal-DepthNormalsTexture shader was not found.");
                return false;
            }

            var oldTarget = cam.targetTexture;
            var oldRect = cam.rect;
            var oldActive = RenderTexture.active;
            var oldClearFlags = cam.clearFlags;
            var oldBackground = cam.backgroundColor;

            try
            {
                cam.targetTexture = depthRt;
                cam.rect = new Rect(0, 0, 1, 1);
                cam.clearFlags = CameraClearFlags.SolidColor;
                cam.backgroundColor = Color.white;
                RenderTexture.active = depthRt;
                GL.Clear(true, true, Color.white);
                cam.RenderWithShader(depthNormalsShader, "RenderType");
                return true;
            }
            finally
            {
                cam.targetTexture = oldTarget;
                cam.rect = oldRect;
                cam.clearFlags = oldClearFlags;
                cam.backgroundColor = oldBackground;
                RenderTexture.active = oldActive;
            }
        }
#endif

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

        private static Texture2D DownscaleDepthNearest(Texture2D source, int width, int height)
        {
            var src = source.GetPixels();
            var dst = new Color[width * height];
            var srcWidth = source.width;
            var srcHeight = source.height;

            for (var y = 0; y < height; y++)
            {
                var srcY = Mathf.Clamp(Mathf.RoundToInt((y + 0.5f) * srcHeight / height - 0.5f), 0, srcHeight - 1);
                for (var x = 0; x < width; x++)
                {
                    var srcX = Mathf.Clamp(Mathf.RoundToInt((x + 0.5f) * srcWidth / width - 0.5f), 0, srcWidth - 1);
                    dst[y * width + x] = src[srcY * srcWidth + srcX];
                }
            }

            var result = new Texture2D(width, height, TextureFormat.RFloat, false, true);
            result.SetPixels(dst);
            result.Apply(false, false);
            return result;
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
            byte[] raw;
            if (depthKind == OfflineDepthTextureKind.DeviceRFloat)
            {
                var rawSw = Stopwatch.StartNew();
                raw = deviceDepthTexture.GetRawTextureData();
                rawSw.Stop();
                LogOfflineTiming(timingLabel, "depth GetRawTextureData", rawSw.Elapsed);
            }
            else
            {
                var values = GetDeviceDepthValues(deviceDepthTexture, cam, depthKind, timingLabel);
                raw = new byte[values.Length * sizeof(float)];
                Buffer.BlockCopy(values, 0, raw, 0, raw.Length);
            }

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

#if KK
        private static float DecodeUnityDepthNormalsDepth(Color32 pixel)
        {
            // UnityCG EncodeDepthNormal stores depth in BA through EncodeFloatRG.
            return Mathf.Clamp01((pixel.b / 255f) + (pixel.a / 255f) / 255f);
        }

        private static float DecodeUnityDepthNormalsDeviceDepth(Color32 pixel, Camera cam)
        {
            var linear01 = DecodeUnityDepthNormalsDepth(pixel);
            var far = cam != null ? Mathf.Max(cam.farClipPlane, 0.001f) : 1000f;
            var near = cam != null ? Mathf.Max(cam.nearClipPlane, 0.001f) : 0.001f;
            if (far <= near)
                far = near + 0.001f;

            var eyeDepth = Mathf.Clamp(linear01 * far, near, far);
            var range = far - near;
            var deviceDepth = SystemInfo.usesReversedZBuffer
                ? near * (far / eyeDepth - 1f) / range
                : far / range - (far * near) / (range * eyeDepth);

            if (float.IsNaN(deviceDepth) || float.IsInfinity(deviceDepth))
                return SystemInfo.usesReversedZBuffer ? 0f : 1f;

            return Mathf.Clamp01(deviceDepth);
        }
#endif

        private static float DecodePackedRgba8DeviceDepth(Color32 pixel)
        {
            var value =
                (uint)pixel.r |
                ((uint)pixel.g << 8) |
                ((uint)pixel.b << 16) |
                ((uint)pixel.a << 24);
            return value / (float)uint.MaxValue;
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
            if (depthKind == OfflineDepthTextureKind.DeviceRFloat)
            {
                var rawSw = Stopwatch.StartNew();
                var raw = depthTexture.GetRawTextureData();
                rawSw.Stop();
                LogOfflineTiming(timingLabel, "depth GetRawTextureData", rawSw.Elapsed);

                var values = new float[depthTexture.width * depthTexture.height];
                Buffer.BlockCopy(raw, 0, values, 0, Math.Min(raw.Length, values.Length * sizeof(float)));
                return values;
            }

            var getSw = Stopwatch.StartNew();
            var pixels32 = depthTexture.GetPixels32();
            getSw.Stop();
            LogOfflineTiming(timingLabel, depthKind == OfflineDepthTextureKind.DevicePackedRgba8 ? "depth GetPixels32 packed RGBA8" : "depth GetPixels32 depthnormals", getSw.Elapsed);

            var packSw = Stopwatch.StartNew();
            var decoded = new float[pixels32.Length];
            for (var i = 0; i < pixels32.Length; i++)
            {
                if (depthKind == OfflineDepthTextureKind.DevicePackedRgba8)
                    decoded[i] = DecodePackedRgba8DeviceDepth(pixels32[i]);
                else
                {
#if KK
                    decoded[i] = DecodeUnityDepthNormalsDeviceDepth(pixels32[i], cam);
#else
                    decoded[i] = 0f;
#endif
                }
            }
            packSw.Stop();
            LogOfflineTiming(timingLabel, depthKind == OfflineDepthTextureKind.DevicePackedRgba8 ? "depth decode packed RGBA8 to rfloat" : "depth decode depthnormals to rfloat", packSw.Elapsed);
            return decoded;
        }

        private static byte[] GetPackedRgba8DepthBytes(Texture2D depthTexture, Camera cam, OfflineDepthTextureKind depthKind, string timingLabel)
        {
            if (depthKind == OfflineDepthTextureKind.DevicePackedRgba8)
            {
                var rawSw = Stopwatch.StartNew();
                var raw = depthTexture.GetRawTextureData();
                rawSw.Stop();
                LogOfflineTiming(timingLabel, "depth GetRawTextureData packed RGBA8", rawSw.Elapsed);
                return raw;
            }

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
