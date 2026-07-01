using alphaShot;
using BepInEx.Configuration;
using System;
using System.Collections;
using System.Diagnostics;
using System.Globalization;
using System.IO;
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
            /// RGBA8 PNG where R/G/B/A store a little-endian 32-bit normalized device depth integer.
            /// </summary>
            PngRgba8
        }

        private sealed class OfflineCaptureResult
        {
            public RenderTexture Color;
            public Texture2D Depth;
            public string DepthSource;
        }

        private void InitializeOfflineReShadeExport()
        {
            KeyExportOfflineReShade = Config.Bind(
                "Offline ReShade Export",
                "Export color and depth",
                new KeyboardShortcut(KeyCode.F10, KeyCode.LeftControl),
                "Exports coloroutput.png, depthoutput.png, and metadata.json for OfflineReShadeCapture.");

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

            OfflineReShadeDepthOutputFormat = Config.Bind(
                "Offline ReShade Export",
                "Depth output format",
                OfflineDepthOutputFormat.RawRFloat,
                "Depth sidecar format. RawRFloat writes little-endian float32 device depth and is much faster. PngRgba8 writes the previous RGBA8 packed PNG.");
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
                capture = CaptureColorAndHardwareDepth(width, height, downscaling, "Ctrl+F10");
                if (capture == null || capture.Color == null)
                    yield break;

                yield return WriteRenderTexturePng(capture.Color, colorPath, "Ctrl+F10 coloroutput");
                if (!string.IsNullOrEmpty(archiveColorPath))
                    yield return WriteRenderTexturePng(capture.Color, archiveColorPath, "Ctrl+F10 color archive");

                if (capture.Depth != null)
                {
                    WriteDepthSidecar(capture.Depth, depthPath, "Ctrl+F10 depthoutput");
                    if (!string.IsNullOrEmpty(archiveDepthPath))
                        WriteDepthSidecar(capture.Depth, archiveDepthPath, "Ctrl+F10 depth archive");
                    UnityEngine.Object.DestroyImmediate(capture.Depth);
                    capture.Depth = null;
                }

                WriteOfflineMetadata(metadataPath, width, height, Camera.main, capture.DepthSource, downscaling);
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

        private static OfflineCaptureResult CaptureColorAndHardwareDepth(int width, int height, int downscalingRate, string timingLabel = null)
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
            var depthRt = RenderTexture.GetTemporary(width, height, 0, RenderTextureFormat.RFloat, RenderTextureReadWrite.Linear, 1);
            colorRt.name = "OfflineReShade_Color";
            depthRt.name = "OfflineReShade_DeviceDepth";
            depthRt.filterMode = FilterMode.Point;

            var depthCopy = new CommandBuffer { name = "Offline ReShade depth copy" };
            depthCopy.Blit(BuiltinRenderTextureType.Depth, depthRt);

            try
            {
                cam.depthTextureMode |= DepthTextureMode.Depth;
                cam.AddCommandBuffer(CameraEvent.AfterEverything, depthCopy);
                cam.targetTexture = colorRt;
                cam.rect = new Rect(0, 0, 1, 1);

                RenderTexture.active = colorRt;
                GL.Clear(true, true, Color.clear);
                var renderSw = Stopwatch.StartNew();
                cam.Render();
                renderSw.Stop();
                LogOfflineTiming(timingLabel, "camera render + depth blit", renderSw.Elapsed);

                RenderTexture.active = depthRt;
                var depthTex = new Texture2D(width, height, TextureFormat.RFloat, false, true);
                var depthReadSw = Stopwatch.StartNew();
                depthTex.ReadPixels(new Rect(0, 0, width, height), 0, 0);
                depthTex.Apply(false, false);
                depthReadSw.Stop();
                LogOfflineTiming(timingLabel, "depth ReadPixels+Apply", depthReadSw.Elapsed);

                if (safeDownscaling > 1)
                {
                    var colorScaleSw = Stopwatch.StartNew();
                    colorRt = AlphaShot2.LanczosTex(colorRt, width, height);
                    colorScaleSw.Stop();
                    LogOfflineTiming(timingLabel, "color Lanczos downscale", colorScaleSw.Elapsed);

                    LogOfflineTiming(timingLabel, "depth GPU blit downscale", TimeSpan.Zero);
                }

                return new OfflineCaptureResult
                {
                    Color = colorRt,
                    Depth = depthTex,
                    DepthSource = "camera_depth_texture_after_everything_gpu_downscaled_x" + safeDownscaling
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
                try { cam.RemoveCommandBuffer(CameraEvent.AfterEverything, depthCopy); }
                catch { }
                depthCopy.Release();
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
                capture = CaptureColorAndHardwareDepth(captureWidth, captureHeight, captureDownscaling, frameLabel);
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
                    WriteDepthSidecar(capture.Depth, depthPath, frameLabel + " depth");
                var metadataSw = Stopwatch.StartNew();
                WriteOfflineMetadata(metadataPath, captureWidth, captureHeight, Camera.main, capture.DepthSource, captureDownscaling);
                metadataSw.Stop();
                LogOfflineTiming(frameLabel, "metadata write", metadataSw.Elapsed);
                return capture.Depth != null;
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
            return GetDepthOutputFormat() == OfflineDepthOutputFormat.RawRFloat ? ".rfloat" : ".png";
        }

        private static OfflineDepthOutputFormat GetDepthOutputFormat()
        {
            return OfflineReShadeDepthOutputFormat != null ? OfflineReShadeDepthOutputFormat.Value : OfflineDepthOutputFormat.RawRFloat;
        }

        private static string GetDepthEncoding()
        {
            return GetDepthOutputFormat() == OfflineDepthOutputFormat.RawRFloat
                ? "rfloat32_device_depth_little_endian"
                : "rgba8_unorm_32_device_depth";
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

        private static void WriteDepthSidecar(Texture2D deviceDepthTexture, string filename, string timingLabel = null)
        {
            if (GetDepthOutputFormat() == OfflineDepthOutputFormat.RawRFloat)
                WriteRawDepthRFloat(deviceDepthTexture, filename, timingLabel);
            else
                WriteEncodedDepthPng(deviceDepthTexture, filename, timingLabel);
        }

        private static void WriteRawDepthRFloat(Texture2D deviceDepthTexture, string filename, string timingLabel = null)
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

        private static void WriteEncodedDepthPng(Texture2D deviceDepthTexture, string filename, string timingLabel = null)
        {
            var getSw = Stopwatch.StartNew();
            var pixels = deviceDepthTexture.GetPixels();
            getSw.Stop();
            LogOfflineTiming(timingLabel, "depth GetPixels", getSw.Elapsed);

            var encoded = new Texture2D(deviceDepthTexture.width, deviceDepthTexture.height, TextureFormat.RGBA32, false, true);
            var encodedPixels = new Color32[pixels.Length];

            var packSw = Stopwatch.StartNew();
            for (var i = 0; i < pixels.Length; i++)
            {
                var d = pixels[i].r;
                if (float.IsNaN(d) || float.IsInfinity(d))
                    d = 0f;

                var v = (uint)Math.Round(Mathf.Clamp01(d) * uint.MaxValue);
                encodedPixels[i] = new Color32(
                    (byte)(v & 0xFF),
                    (byte)((v >> 8) & 0xFF),
                    (byte)((v >> 16) & 0xFF),
                    (byte)((v >> 24) & 0xFF));
            }
            packSw.Stop();
            LogOfflineTiming(timingLabel, "depth RGBA8 pack", packSw.Elapsed);

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

        private static void WriteOfflineMetadata(string filename, int width, int height, Camera cam, string depthSource, int downscaling)
        {
            var invariant = CultureInfo.InvariantCulture;
            var sb = new StringBuilder();
            sb.AppendLine("{");
            sb.AppendLine($"  \"width\": {width},");
            sb.AppendLine($"  \"height\": {height},");
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
