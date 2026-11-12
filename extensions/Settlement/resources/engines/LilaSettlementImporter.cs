// Builds a Lila settlement in the open Unity scene from settlementExport(format='unity'). Put this file in an Editor folder
// of your project, then Tools > Lila > Import Settlement and pick the export's .json. Headless:
//   Unity -batchmode -quit -projectPath <project> -executeMethod LilaSettlementImporter.ImportFromCommandLine -lilaSettlement <file.json>
// [-lilaScene Assets/Town.unity]: the settlement goes into that scene (made when missing) and is saved; without it, into
// a new scene saved as Assets/LilaSettlement/<name>.unity.
// Streets and massing come in as the FBX models beside the JSON (copied into Assets/LilaSettlement/<name> when the export
// lies outside Assets). Each placement instantiates the prefab or model whose Assets/ path or name matches its asset, or
// an empty placeholder when none does. Lots, envelopes and junctions become empty objects named by their key and use.

#if UNITY_EDITOR
#pragma warning disable 0649 // JsonUtility fills the data classes
using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;

public static class LilaSettlementImporter
{
    [Serializable]
    class Meshes
    {
        public string streets;
        public string massing;
    }

    [Serializable]
    class Junction
    {
        public string key;
        public string control;
        public int approaches;
        public float[] position;
    }

    [Serializable]
    class Lot
    {
        public int id;
        public string key;
        public string use;
        public float area;
        public float[] outline;
    }

    [Serializable]
    class Envelope
    {
        public int lot;
        public string key;
        public string use;
        public int floors;
        public float height;
        public float[] footprint;
    }

    [Serializable]
    class Placement
    {
        public string id;
        public string asset;
        public float[] position;
        public float[] rotation;
        public float[] scale;
        public string[] tags;
    }

    [Serializable]
    class Data
    {
        public string format;
        public string target;
        public string name;
        public Meshes meshes;
        public Junction[] junctions;
        public Lot[] lots;
        public Envelope[] envelopes;
        public Placement[] placements;
    }

    [MenuItem("Tools/Lila/Import Settlement...")]
    static void ImportFromMenu()
    {
        string path = EditorUtility.OpenFilePanel("Lila settlement export", "", "json");
        if (!string.IsNullOrEmpty(path))
            Import(path);
    }

    public static void ImportFromCommandLine()
    {
        string[] args = Environment.GetCommandLineArgs();
        string json = null, scene = null;
        for (int i = 0; i + 1 < args.Length; ++i)
        {
            if (args[i] == "-lilaSettlement")
                json = args[i + 1];
            else if (args[i] == "-lilaScene")
                scene = args[i + 1];
        }
        if (json == null)
        {
            Debug.LogError("LilaSettlementImporter: pass -lilaSettlement <export .json>");
            EditorApplication.Exit(1);
            return;
        }
        if (scene != null && File.Exists(scene))
            UnityEditor.SceneManagement.EditorSceneManager.OpenScene(scene);
        else
            UnityEditor.SceneManagement.EditorSceneManager.NewScene(UnityEditor.SceneManagement.NewSceneSetup.EmptyScene);
        GameObject root = Import(json);
        // Batch mode keeps nothing it doesn't save.
        string target = scene ?? "Assets/LilaSettlement/" + root.name + ".unity";
        Directory.CreateDirectory(Path.GetDirectoryName(target));
        if (!UnityEditor.SceneManagement.EditorSceneManager.SaveScene(root.scene, target))
        {
            Debug.LogError("LilaSettlementImporter: could not save " + target);
            EditorApplication.Exit(1);
            return;
        }
        Debug.Log("Lila: saved " + target);
    }

    /// <summary>Builds the settlement under a new root object and returns it.</summary>
    public static GameObject Import(string jsonPath)
    {
        Data d = JsonUtility.FromJson<Data>(File.ReadAllText(jsonPath));
        if (d == null || d.format != "lila.settlement.engine" || d.target != "unity")
            throw new Exception(jsonPath + " isn't a Lila Unity settlement export");
        string folder = Path.GetDirectoryName(Path.GetFullPath(jsonPath));
        GameObject root = new GameObject(d.name);
        Undo.RegisterCreatedObjectUndo(root, "Import Lila Settlement");

        if (d.meshes != null)
        {
            AddModel(root, folder, d.name, d.meshes.streets, "Streets");
            AddModel(root, folder, d.name, d.meshes.massing, "Massing");
        }

        GameObject lots = Group(root, "Lots");
        foreach (Lot lot in d.lots ?? new Lot[0])
            Marker(lots, lot.key + "_" + lot.use, Centroid(lot.outline));
        GameObject envelopes = Group(root, "Envelopes");
        foreach (Envelope e in d.envelopes ?? new Envelope[0])
            Marker(envelopes, e.key + "_" + e.use + "_" + e.floors + "F", Centroid(e.footprint));
        GameObject junctions = Group(root, "Junctions");
        foreach (Junction j in d.junctions ?? new Junction[0])
            Marker(junctions, j.key + "_" + j.control, Vec(j.position, 0, Vector3.zero));

        GameObject placed = Group(root, "Placements");
        Dictionary<string, GameObject> found = new Dictionary<string, GameObject>();
        int missing = 0;
        foreach (Placement p in d.placements ?? new Placement[0])
        {
            GameObject source;
            if (!found.TryGetValue(p.asset, out source))
            {
                source = FindAsset(p.asset);
                found[p.asset] = source;
            }
            GameObject go = source != null ? (GameObject)PrefabUtility.InstantiatePrefab(source) : new GameObject();
            if (source == null)
                ++missing;
            go.name = "place_" + p.id;
            go.transform.SetParent(placed.transform, false);
            go.transform.localPosition = Vec(p.position, 0, Vector3.zero);
            if (p.rotation != null && p.rotation.Length == 4)
                go.transform.localRotation = new Quaternion(p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]);
            go.transform.localScale = Vec(p.scale, 0, Vector3.one);
        }
        if (missing > 0)
            Debug.LogWarning("Lila: " + missing + " placements had no prefab or model matching their asset; empty placeholders stand in");
        Debug.Log("Lila: imported settlement " + d.name + " (" + (d.lots ?? new Lot[0]).Length + " lots, " +
                  (d.placements ?? new Placement[0]).Length + " placements)");
        return root;
    }

    static GameObject Group(GameObject root, string name)
    {
        GameObject g = new GameObject(name);
        g.transform.SetParent(root.transform, false);
        return g;
    }

    static void Marker(GameObject parent, string name, Vector3 at)
    {
        GameObject m = new GameObject(name);
        m.transform.SetParent(parent.transform, false);
        m.transform.localPosition = at;
    }

    static Vector3 Vec(float[] v, int at, Vector3 fallback)
    {
        return v != null && v.Length >= at + 3 ? new Vector3(v[at], v[at + 1], v[at + 2]) : fallback;
    }

    static Vector3 Centroid(float[] flat)
    {
        if (flat == null || flat.Length < 3)
            return Vector3.zero;
        Vector3 sum = Vector3.zero;
        int n = flat.Length / 3;
        for (int i = 0; i < n; ++i)
            sum += Vec(flat, i * 3, Vector3.zero);
        return sum / n;
    }

    /// <summary>An FBX beside the JSON as a child; copied into the project first when the export lies outside Assets.</summary>
    static void AddModel(GameObject root, string folder, string name, string file, string label)
    {
        if (string.IsNullOrEmpty(file))
            return;
        string full = Path.Combine(folder, file);
        if (!File.Exists(full))
        {
            Debug.LogWarning("Lila: " + full + " is missing; " + label + " skipped");
            return;
        }
        string assetPath = ProjectPath(full);
        if (assetPath == null)
        {
            string target = "Assets/LilaSettlement/" + name;
            Directory.CreateDirectory(target);
            assetPath = target + "/" + file;
            File.Copy(full, assetPath, true);
            AssetDatabase.ImportAsset(assetPath);
        }
        GameObject model = AssetDatabase.LoadAssetAtPath<GameObject>(assetPath);
        if (model == null)
        {
            Debug.LogWarning("Lila: Unity didn't import " + assetPath + "; " + label + " skipped");
            return;
        }
        GameObject go = (GameObject)PrefabUtility.InstantiatePrefab(model);
        go.name = label;
        go.transform.SetParent(root.transform, false);
    }

    /// <summary>The Assets/ path of a file inside the project, or null.</summary>
    static string ProjectPath(string full)
    {
        string data = Path.GetFullPath(Application.dataPath).Replace('\\', '/');
        string f = Path.GetFullPath(full).Replace('\\', '/');
        return f.StartsWith(data + "/", StringComparison.OrdinalIgnoreCase) ? "Assets" + f.Substring(data.Length) : null;
    }

    /// <summary>A prefab or model by its Assets/ path, else by the asset's last name (an Unreal path's object name works).</summary>
    static GameObject FindAsset(string asset)
    {
        if (string.IsNullOrEmpty(asset))
            return null;
        if (asset.StartsWith("Assets/"))
            return AssetDatabase.LoadAssetAtPath<GameObject>(asset);
        string name = asset.Substring(asset.LastIndexOf('/') + 1);
        int dot = name.IndexOf('.');
        if (dot >= 0)
            name = name.Substring(0, dot);
        foreach (string guid in AssetDatabase.FindAssets(name + " t:GameObject"))
        {
            string path = AssetDatabase.GUIDToAssetPath(guid);
            if (Path.GetFileNameWithoutExtension(path) == name)
                return AssetDatabase.LoadAssetAtPath<GameObject>(path);
        }
        return null;
    }
}
#endif
