using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using Playnite.SDK;
using Playnite.SDK.Models;

namespace LavApolloLink
{
    // Closes the running game when LavApollo asks for it (the user quit the game from a client).
    // Playnite has no "stop game" API, so the game's process tree is killed: first the process
    // Playnite reported at start, then anything still running from the game's install folder
    // (games started through a launcher are often not that process's children).
    public static class GameStopper
    {
        private static readonly ILogger Log = LogManager.GetLogger();

        public static void Stop(Game game, int startedPid)
        {
            if (startedPid > 0)
            {
                KillTree(startedPid);
            }

            var dir = game?.InstallDirectory;
            if (string.IsNullOrEmpty(dir))
            {
                return;
            }

            string root;
            try
            {
                root = Path.GetFullPath(dir).TrimEnd('\\') + "\\";
            }
            catch (Exception)
            {
                return;
            }

            foreach (var process in Process.GetProcesses())
            {
                try
                {
                    var path = process.MainModule?.FileName;
                    if (path != null && path.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                    {
                        KillTree(process.Id);
                    }
                }
                catch (Exception)
                {
                    // Access denied (elevated or protected process) or already gone.
                }
                finally
                {
                    process.Dispose();
                }
            }
        }

        private static void KillTree(int pid)
        {
            try
            {
                using (var taskkill = Process.Start(new ProcessStartInfo("taskkill", $"/PID {pid} /T /F")
                {
                    CreateNoWindow = true,
                    UseShellExecute = false,
                }))
                {
                    taskkill?.WaitForExit(5000);
                }
                Log.Info($"LavApollo Link: closed process tree {pid}");
            }
            catch (Exception e)
            {
                Log.Warn($"LavApollo Link: could not close {pid}: {e.Message}");
            }
        }
    }
}
