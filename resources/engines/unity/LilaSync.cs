#if UNITY_EDITOR
using System;
using System.Collections.Concurrent;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

// Tools > Lila > Sync: reads Lila's change stream (Preferences > General > Change Stream in Lila) and moves the matching
// objects inside every model from Lila: those sent to Assets/Lila and the FBX kept beside each .lila file.
// A starting point to build on, not a supported plugin.
public static class LilaSync
{
    const string MenuPath = "Tools/Lila/Sync";
    public static int Port = 8742;

    // Filled by JsonUtility.
#pragma warning disable 0649
    [Serializable] class Message { public string type; public Node[] nodes; public Change[] changes; }
    [Serializable] class Node { public string path; public float[] world; }
    [Serializable] class Change { public string kind; public string path; public float[] matrix; }
#pragma warning restore 0649

    static TcpClient client;
    static Thread reader;
    static readonly ConcurrentQueue<string> lines = new ConcurrentQueue<string>();

    [MenuItem(MenuPath)]
    static void Toggle()
    {
        if (client == null)
            Start();
        else
            Stop();
        Menu.SetChecked(MenuPath, client != null);
    }

    public static void Start()
    {
        try
        {
            client = new TcpClient("127.0.0.1", Port);
        }
        catch (SocketException)
        {
            Debug.LogWarning("Lila: nothing on port " + Port + ". Turn on Preferences > General > Change Stream in Lila.");
            client = null;
            return;
        }
        Send("{\"op\":\"subscribe\",\"snapshot\":true,\"world\":true,\"live\":true,\"types\":[\"transform\"]}");
        var stream = new StreamReader(client.GetStream(), Encoding.UTF8);
        reader = new Thread(() =>
        {
            try
            {
                string line;
                while ((line = stream.ReadLine()) != null)
                    lines.Enqueue(line);
            }
            catch (IOException) { }
            catch (ObjectDisposedException) { }
        }) { IsBackground = true };
        reader.Start();
        EditorApplication.update += Update;
    }

    public static void Stop()
    {
        EditorApplication.update -= Update;
        client?.Close();
        client = null;
    }

    static void Send(string json)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(json + "\n");
        client.GetStream().Write(bytes, 0, bytes.Length);
    }

    // Applies everything that arrived since the last editor update, on the main thread.
    public static void Update()
    {
        while (lines.TryDequeue(out string line))
        {
            var message = JsonUtility.FromJson<Message>(line);
            if (message.type == "resync")
                Send("{\"op\":\"snapshot\"}");
            if (message.nodes != null)
                foreach (Node node in message.nodes)
                    Move(node.path, node.world);
            if (message.changes != null)
                foreach (Change change in message.changes)
                    if (change.kind == "world")
                        Move(change.path, change.matrix);
        }
    }

    static bool FromLila(string asset)
    {
        return asset.StartsWith("Assets/Lila/") || File.Exists(Path.ChangeExtension(asset, ".lila")) || File.Exists(Path.ChangeExtension(asset, ".lilaa"));
    }

    static void Move(string path, float[] m)
    {
        if (m == null || m.Length != 16)
            return;
        string name = path.Substring(path.LastIndexOf('|') + 1);
        foreach (GameObject root in UnityEngine.Object.FindObjectsByType<GameObject>(FindObjectsInactive.Include))
        {
            if (!PrefabUtility.IsOutermostPrefabInstanceRoot(root))
                continue;
            if (!FromLila(PrefabUtility.GetPrefabAssetPathOfNearestInstanceRoot(root)))
                continue;
            foreach (Transform t in root.GetComponentsInChildren<Transform>(true))
            {
                if (t.name != name)
                    continue;
                // Lila is right-handed Y up in centimetres; Unity mirrors X and works in metres, as its FBX importer does.
                var x = new Vector3(m[0], -m[1], -m[2]);
                var y = new Vector3(-m[4], m[5], m[6]);
                var z = new Vector3(-m[8], m[9], m[10]);
                t.position = new Vector3(-m[12], m[13], m[14]) / 100f;
                t.rotation = Quaternion.LookRotation(z, y);
                t.localScale = new Vector3(x.magnitude, y.magnitude, z.magnitude);
                EditorSceneManager.MarkSceneDirty(t.gameObject.scene);
            }
        }
    }
}
#endif
