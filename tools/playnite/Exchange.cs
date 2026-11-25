using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using Playnite.SDK.Data;

namespace LavApolloLink
{
    // Files shared with LavApollo, in %ProgramData%\LavApollo\playnite\. LavApollo runs as SYSTEM and
    // Playnite as the user: ProgramData is the one place both can reach. Format documented in
    // LavApollo/docs/superpowers/specs/2026-11-23-playnite-integration-design.md.
    public static class Exchange
    {
        public const int FormatVersion = 1;

        public static string Root => Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "LavApollo", "playnite");

        public static string LibraryFile => Path.Combine(Root, "library.json");
        public static string StateFile => Path.Combine(Root, "state.json");
        public static string CoversDir => Path.Combine(Root, "covers");
        public static string BackgroundsDir => Path.Combine(Root, "backgrounds");

        // Created by this extension, never by LavApollo: the folder's creator owns it, which is what
        // lets the extension delete the command files LavApollo (SYSTEM) writes into it.
        public static string CommandsDir => Path.Combine(Root, "commands");

        public static void EnsureFolders()
        {
            Directory.CreateDirectory(Root);
            Directory.CreateDirectory(CoversDir);
            Directory.CreateDirectory(BackgroundsDir);
            Directory.CreateDirectory(CommandsDir);
        }

        // Write to a temporary file, then rename: LavApollo never reads half a file.
        public static void WriteAtomic(string path, string content)
        {
            var tmp = path + ".tmp";
            File.WriteAllText(tmp, content, new UTF8Encoding(false));
            if (File.Exists(path))
            {
                File.Replace(tmp, path, null);
            }
            else
            {
                File.Move(tmp, path);
            }
        }

        public static void WriteJson(string path, object value)
        {
            WriteAtomic(path, Serialization.ToJson(value, true));
        }

        public static string IsoNow() => DateTime.UtcNow.ToString("o");
    }

    public class ExportedGame
    {
        public string id { get; set; }
        public string name { get; set; }
        public string source { get; set; }
        public string platform { get; set; }
        public bool favorite { get; set; }
        public string lastActivity { get; set; }
        public ulong playtime { get; set; }
        public bool installed { get; set; }
        public string cover { get; set; }
        public string background { get; set; }
    }

    public class ExportedLibrary
    {
        public int version { get; set; } = Exchange.FormatVersion;
        public string exportedAt { get; set; }
        public List<ExportedGame> games { get; set; } = new List<ExportedGame>();
    }

    public class RunningGame
    {
        public string id { get; set; }
        public string name { get; set; }
        public string startedAt { get; set; }
        public int pid { get; set; }
        public bool starting { get; set; }
    }

    public class PlayniteState
    {
        public int version { get; set; } = Exchange.FormatVersion;
        public bool playniteRunning { get; set; }
        public string mode { get; set; }
        public int pid { get; set; }
        public RunningGame game { get; set; }
        public string updatedAt { get; set; }
    }

    public class Command
    {
        public string action { get; set; }
        public string gameId { get; set; }
        public string issuedAt { get; set; }
    }
}
