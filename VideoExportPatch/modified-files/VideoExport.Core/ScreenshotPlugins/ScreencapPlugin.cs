using System;
using System.Diagnostics;
using System.IO;
using HarmonyLib;
using Screencap;
using UnityEngine;

namespace VideoExport.ScreenshotPlugins
{
    using static ScreenshotManager;

    public class ScreencapPlugin : IScreenshotPlugin
    {
        #region Privates
        private enum CaptureType
        {
            Normal,
            NormalDepth,
            ThreeHundredSixty
        }
        private CaptureType _captureType = CaptureType.Normal;
        private string[] _captureTypeNames;
        private bool _in3d;
        private bool uiShow;
        private Traverse PluginInstance;
        private Type _screenshotManagerType;
        private bool _isPluginInstanceInitialized;
        #endregion

        #region Interface
        public string name => "Screenshot Manager";

        public Vector2 currentSize
        {
            get
            {
                switch (_captureType)
                {
                    case CaptureType.Normal:
                    case CaptureType.NormalDepth:
                        Vector3 size = new Vector2(ResolutionX.Value, ResolutionY.Value);
                        if (_in3d) size.x = (size.x - (int)(size.x * ImageSeparationOffset.Value)) * 2;
                        return size;
                    case CaptureType.ThreeHundredSixty:
                        size = new Vector2(Resolution360.Value, Mathf.FloorToInt(Resolution360.Value / 2f));
                        if (_in3d) size.x *= 2;
                        return size;
                    default:
                        throw new NotSupportedException($"Unsupported capture type: {_captureType}");
                }
            }
        }
        public VideoExport.ImgFormat imageFormat => _captureType == CaptureType.NormalDepth ? VideoExport.ImgFormat.PNG : UseJpg.Value ? VideoExport.ImgFormat.JPG : VideoExport.ImgFormat.PNG;

        public bool transparency => CaptureAlphaMode.Value != AlphaMode.None;
        public string extension => _captureType == CaptureType.NormalDepth ? "png" : UseJpg.Value ? "jpg" : "png";
        public byte bitDepth => 8;

        public bool Init(Harmony harmony)
        {
            _captureType = (CaptureType)VideoExport._configFile.AddInt("Screencap_captureType", 0, true);
            _in3d = VideoExport._configFile.AddBool("Screencap_in3d", false, true);

            InitializePluginInstance();

            return true;
        }

        private object _pluginInstanceObject;

        private void InitializePluginInstance()
        {
            if (_isPluginInstanceInitialized) return;

            _screenshotManagerType = AccessTools.TypeByName("Screencap.ScreenshotManager");
            if (_screenshotManagerType != null)
            {
                var allObjects = Resources.FindObjectsOfTypeAll(_screenshotManagerType);
                if (allObjects != null && allObjects.Length > 0)
                {
                    _pluginInstanceObject = allObjects[0];
                    PluginInstance = Traverse.Create(_pluginInstanceObject);
                    _isPluginInstanceInitialized = true;
                }
            }
        }

        public void SaveParams()
        {
            VideoExport._configFile.SetInt("Screencap_captureType", (int)_captureType);
            VideoExport._configFile.SetBool("Screencap_in3d", _in3d);
        }

        public void UpdateLanguage()
        {
            _captureTypeNames = new[]
            {
                VideoExport._currentDictionary.GetString(VideoExport.TranslationKey.ScreencapCaptureTypeNormal),
                "Normal + Depth",
                VideoExport._currentDictionary.GetString(VideoExport.TranslationKey.ScreencapCaptureType360)
            };
        }

        public void OnStartRecording()
        {
            FirePreCapture();
        }

        public byte[] Capture(string path)
        {
            if (_captureType == CaptureType.NormalDepth)
            {
                CaptureToFile(path);
                return null;
            }

            var tex = CaptureTexture();
            var bytes = UseJpg.Value ? tex.EncodeToJPG(JpgQuality.Value) : tex.EncodeToPNG();
            UnityEngine.Object.DestroyImmediate(tex);
            return bytes;
        }

        public bool IsTextureCaptureAvailable()
        {
            if (_captureType == CaptureType.NormalDepth)
                return false;

            return true;
        }

        public bool IsRenderTextureCaptureAvailable()
        {
            if (_captureType == CaptureType.NormalDepth)
                return false;

#if (!KOIKATSU || SUNSHINE)
            return true;
#else
            return false;
#endif
        }
        
        public bool IsVFlipNeeded()
        {
            return true;
        }

        public Texture2D CaptureTexture()
        {
            RenderTexture result;
            switch (_captureType)
            {
                case CaptureType.Normal:
                    result = !_in3d ? CaptureRender() : Do3DCapture(() => CaptureRender());
                    break;
                case CaptureType.NormalDepth:
                    throw new NotSupportedException("Normal + Depth captures must be written through Capture(path).");
                case CaptureType.ThreeHundredSixty:
                    result = !_in3d ? Capture360() : Do3DCapture(() => Capture360(), overlapOffset: 0);
                    break;
                default:
                    throw new NotSupportedException($"Unsupported capture type: {_captureType}");
            }

            if (!result) return null;
            var texture = ToTexture2D(result);
            return texture;
        }

        public RenderTexture CaptureRenderTexture()
        {
            RenderTexture result;
            switch (_captureType)
            {
                case CaptureType.Normal:
                    result = !_in3d ? CaptureRender() : Do3DCapture(() => CaptureRender());
                    break;
                case CaptureType.NormalDepth:
                    throw new NotSupportedException("Normal + Depth captures must be written through Capture(path).");
                case CaptureType.ThreeHundredSixty:
                    result = !_in3d ? Capture360() : Do3DCapture(() => Capture360(), overlapOffset: 0);
                    break;
                default:
                    throw new NotSupportedException($"Unsupported capture type: {_captureType}");
            }

            if (!result) return null;
            return result;
        }

        public void OnEndRecording()
        {
            FirePostCapture();
        }

        public bool CaptureToFile(string path)
        {
            if (_captureType != CaptureType.NormalDepth)
                return false;

            var sw = Stopwatch.StartNew();
            var result = CaptureNormalDepth(path);
            sw.Stop();
            VideoExport.Logger.LogInfo($"[OfflineReShadeTiming] VE CaptureToFile {Path.GetFileNameWithoutExtension(path)} total = {sw.Elapsed.TotalMilliseconds:0.0} ms");
            return result;
        }

        public void DisplayParams()
        {
            GUILayout.BeginHorizontal();
            {
                GUILayout.Label(VideoExport._currentDictionary.GetString(VideoExport.TranslationKey.ScreencapCaptureType), GUILayout.ExpandWidth(false));
                _captureType = (CaptureType)GUILayout.SelectionGrid((int)_captureType, _captureTypeNames, _captureTypeNames.Length);
            }
            GUILayout.EndHorizontal();

            GUILayout.BeginHorizontal();
            _in3d = GUILayout.Toggle(_in3d, VideoExport._currentDictionary.GetString(VideoExport.TranslationKey.Screencap3D));
            GUILayout.EndHorizontal();


            if (!_isPluginInstanceInitialized)
            {
                InitializePluginInstance();
            }

            if (_isPluginInstanceInitialized && PluginInstance != null)
            {
                GUILayout.BeginHorizontal();
                {
                    uiShow = PluginInstance.Field("_uiShow").GetValue<bool>();

                    if (GUILayout.Button(VideoExport._currentDictionary.GetString(VideoExport.TranslationKey.ToggleScreencapUI)))
                    {
                        var keyGuiField = PluginInstance.Field("KeyGui").GetValue();

                        bool currentShow = PluginInstance.Field("_uiShow").GetValue<bool>();
                        Rect currentRect = PluginInstance.Field("_uiRect").GetValue<Rect>();
                        PluginInstance.Field("_uiShow").SetValue(!currentShow);
                        PluginInstance.Field("_uiRect").SetValue(currentRect);
                    }
                }
                GUILayout.EndHorizontal();
            }

        }
        #endregion

        private static bool CaptureNormalDepth(string colorPath)
        {
            var directory = Path.GetDirectoryName(colorPath);
            var name = Path.GetFileNameWithoutExtension(colorPath);
            var depthPath = Path.Combine(directory, name + ".depth" + GetDepthExtension());
            var metadataPath = Path.Combine(directory, "metadata.json");

            var method = AccessTools.Method(typeof(ScreenshotManager), "ExportOfflineReShadeInputs");
            if (method == null)
            {
                VideoExport.Logger.LogWarning("Screenshot Manager does not expose ExportOfflineReShadeInputs. Install the OfflineReShadeCapture ScreenshotManager build.");
                return false;
            }

            try
            {
                return (bool)method.Invoke(null, new object[] { colorPath, depthPath, metadataPath, null, null, null });
            }
            catch (Exception ex)
            {
                VideoExport.Logger.LogWarning("Normal + Depth capture failed: " + ex);
                return false;
            }
        }

        private static string GetDepthExtension()
        {
            var method = AccessTools.Method(typeof(ScreenshotManager), "GetOfflineReShadeDepthExtension");
            if (method == null)
                return ".png";

            try
            {
                return method.Invoke(null, null) as string ?? ".png";
            }
            catch
            {
                return ".png";
            }
        }

        private static Texture2D ToTexture2D(RenderTexture rt)
        {
            var cached = RenderTexture.active;
            RenderTexture.active = rt;

            var texture = new Texture2D(rt.width, rt.height, TextureFormat.ARGB32, false, true);
            texture.ReadPixels(new Rect(0f, 0f, rt.width, rt.height), 0, 0, false);

            RenderTexture.active = cached;

            RenderTexture.ReleaseTemporary(rt);

            texture.Apply();
            return texture;
        }
    }
}
