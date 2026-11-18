// Keeps an FBX beside every Lila scene (.lila, .lilaa) in the project: when Unity imports a scene, on first add and on
// every save from Lila, lilabatch exports it to <scene>.fbx and Unity's model importer takes it from there.
// Put this file in an Editor folder. lilabatch is found through Tools > Lila > Set lilabatch Location..., then the
// LILA_BATCH environment variable, then the default install folders, then PATH.
// A starting point to build on, not a supported plugin.

#if UNITY_EDITOR
using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using UnityEditor;
using Debug = UnityEngine.Debug;

public class LilaSceneImporter : AssetPostprocessor
{
    const string BatchPathPref = "Lila.BatchPath";
    const int TimeoutMs = 10 * 60 * 1000;

    static void OnPostprocessAllAssets(string[] imported, string[] deleted, string[] moved, string[] movedFrom)
    {
        foreach (string asset in imported)
        {
            string ext = Path.GetExtension(asset).ToLowerInvariant();
            if (ext == ".lila" || ext == ".lilaa")
                Convert(asset);
        }
    }

    [MenuItem("Tools/Lila/Set lilabatch Location...")]
    static void SetBatchLocation()
    {
        string current = EditorPrefs.GetString(BatchPathPref, "");
        string picked = EditorUtility.OpenFilePanel("lilabatch", current.Length > 0 ? Path.GetDirectoryName(current) : "", "");
        if (!string.IsNullOrEmpty(picked))
            EditorPrefs.SetString(BatchPathPref, picked);
    }

    static string FindBatch()
    {
        string exe = Environment.OSVersion.Platform == PlatformID.Win32NT ? "lilabatch.exe" : "lilabatch";
        string[] candidates =
        {
            EditorPrefs.GetString(BatchPathPref, ""),
            Environment.GetEnvironmentVariable("LILA_BATCH") ?? "",
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", "Lila", exe),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Lila", exe),
        };
        foreach (string candidate in candidates)
            if (candidate.Length > 0 && File.Exists(candidate))
                return candidate;
        return exe;
    }

    static void Convert(string asset)
    {
        string scene = Path.GetFullPath(asset);
        string fbxAsset = Path.ChangeExtension(asset, ".fbx");
        string temp = Path.Combine(Path.GetFullPath("Temp"), "Lila", Guid.NewGuid().ToString("N") + ".fbx");
        Directory.CreateDirectory(Path.GetDirectoryName(temp));

        var info = new ProcessStartInfo
        {
            FileName = FindBatch(),
            // The unity profile is Y up with +Z forward, which Unity imports with no rotation left on the root.
            Arguments = "--quiet --export \"" + scene + "\" \"" + temp + "\" --export-args \"profile='unity', units='m', textures=True\"",
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        info.EnvironmentVariables["LILA_CRASH_NO_UI"] = "1";

        var log = new StringBuilder();
        try
        {
            EditorUtility.DisplayProgressBar("Lila", "Exporting " + asset, 0.5f);
            using (var process = new Process { StartInfo = info })
            {
                process.OutputDataReceived += (s, e) => { if (e.Data != null) lock (log) log.AppendLine(e.Data); };
                process.ErrorDataReceived += (s, e) => { if (e.Data != null) lock (log) log.AppendLine(e.Data); };
                process.Start();
                process.BeginOutputReadLine();
                process.BeginErrorReadLine();
                if (!process.WaitForExit(TimeoutMs))
                {
                    process.Kill();
                    Debug.LogError("Lila: exporting " + asset + " took longer than 10 minutes and was stopped");
                    return;
                }
                // Flushes the output handlers, which the timed overload returns ahead of.
                process.WaitForExit();
                if (process.ExitCode != 0 || !File.Exists(temp))
                {
                    Debug.LogError("Lila: lilabatch couldn't export " + asset + "\n" + log);
                    return;
                }
            }
        }
        catch (System.ComponentModel.Win32Exception)
        {
            Debug.LogWarning("Lila: lilabatch wasn't found, so " + fbxAsset + " wasn't updated. Set it in Tools > Lila > Set lilabatch Location...");
            return;
        }
        finally
        {
            EditorUtility.ClearProgressBar();
        }

        File.Copy(temp, Path.GetFullPath(fbxAsset), true);
        File.Delete(temp);
        AssetDatabase.ImportAsset(fbxAsset);
    }
}
#endif
