using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Playnite.SDK;
using Playnite.SDK.Data;
using Playnite.SDK.Events;
using Playnite.SDK.Models;
using Playnite.SDK.Plugins;

namespace LavApolloLink
{
    // LavApollo Link: tells LavApollo which games are installed (they become apps on the host) and
    // which one is running (so a session ends with its game and clients can show "playing X"), and
    // closes the running game when a client quits it. Games are started by LavApollo through
    // Playnite's own playnite://playnite/start/<id> URI, so this plugin never launches anything.
    public class LavApolloLinkPlugin : GenericPlugin
    {
        private static readonly ILogger Log = LogManager.GetLogger();

        private readonly LibraryExporter exporter;
        private readonly object stateLock = new object();
        private readonly PlayniteState state = new PlayniteState();
        private readonly HashSet<string> handledCommands = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        private Timer exportDebounce;
        private Timer commandPoll;
        private FileSystemWatcher commandWatcher;
        private Game runningGame;

        public override Guid Id { get; } = Guid.Parse("6b1d8f2e-3c4a-4e57-9a3e-2f0c5d7b9a11");

        public LavApolloLinkPlugin(IPlayniteAPI api) : base(api)
        {
            exporter = new LibraryExporter(api);
            Properties = new GenericPluginProperties { HasSettings = false };
        }

        public override void OnApplicationStarted(OnApplicationStartedEventArgs args)
        {
            try
            {
                Exchange.EnsureFolders();
            }
            catch (Exception e)
            {
                Log.Error(e, "LavApollo Link: cannot create " + Exchange.Root);
                return;
            }

            lock (stateLock)
            {
                state.playniteRunning = true;
                state.mode = PlayniteApi.ApplicationInfo.Mode == ApplicationMode.Fullscreen ? "fullscreen" : "desktop";
                state.pid = Process.GetCurrentProcess().Id;
                state.game = null;
                WriteState();
            }

            // A game added, installed, uninstalled or hidden changes what LavApollo must list.
            PlayniteApi.Database.Games.ItemCollectionChanged += (s, e) => ScheduleExport();
            PlayniteApi.Database.Games.ItemUpdated += (s, e) => ScheduleExport();
            ScheduleExport(TimeSpan.FromSeconds(2));

            StartCommandWatcher();
        }

        public override void OnApplicationStopped(OnApplicationStoppedEventArgs args)
        {
            commandWatcher?.Dispose();
            commandPoll?.Dispose();
            exportDebounce?.Dispose();

            lock (stateLock)
            {
                state.playniteRunning = false;
                state.game = null;
                WriteState();
            }
        }

        public override void OnLibraryUpdated(OnLibraryUpdatedEventArgs args) => ScheduleExport();

        public override void OnGameStarting(OnGameStartingEventArgs args)
        {
            SetRunning(args.Game, 0, true);
        }

        public override void OnGameStarted(OnGameStartedEventArgs args)
        {
            SetRunning(args.Game, args.StartedProcessId, false);
        }

        public override void OnGameStartupCancelled(OnGameStartupCancelledEventArgs args) => ClearRunning(args.Game);

        public override void OnGameStopped(OnGameStoppedEventArgs args) => ClearRunning(args.Game);

        public override IEnumerable<MainMenuItem> GetMainMenuItems(GetMainMenuItemsArgs args)
        {
            yield return new MainMenuItem
            {
                MenuSection = "@LavApollo",
                Description = "Exporter la bibliothèque vers LavApollo",
                Action = _ =>
                {
                    var count = 0;
                    PlayniteApi.Dialogs.ActivateGlobalProgress(p => count = exporter.Export(),
                        new GlobalProgressOptions("LavApollo Link", false));
                    PlayniteApi.Dialogs.ShowMessage($"{count} jeux exportés vers LavApollo.", "LavApollo Link");
                },
            };
        }

        private void SetRunning(Game game, int pid, bool starting)
        {
            lock (stateLock)
            {
                runningGame = game;
                state.game = new RunningGame
                {
                    id = game.Id.ToString(),
                    name = game.Name,
                    startedAt = state.game?.id == game.Id.ToString() ? state.game.startedAt : Exchange.IsoNow(),
                    pid = pid,
                    starting = starting,
                };
                WriteState();
            }
        }

        private void ClearRunning(Game game)
        {
            lock (stateLock)
            {
                if (state.game != null && (game == null || state.game.id == game.Id.ToString()))
                {
                    state.game = null;
                    runningGame = null;
                    WriteState();
                }
            }
        }

        // Caller holds stateLock.
        private void WriteState()
        {
            try
            {
                state.updatedAt = Exchange.IsoNow();
                Exchange.WriteJson(Exchange.StateFile, state);
            }
            catch (Exception e)
            {
                Log.Error(e, "LavApollo Link: cannot write state.json");
            }
        }

        private void ScheduleExport(TimeSpan? delay = null)
        {
            // Library updates arrive in bursts (one event per game): export once, shortly after.
            var due = delay ?? TimeSpan.FromSeconds(3);
            if (exportDebounce == null)
            {
                exportDebounce = new Timer(_ => RunExport(), null, due, Timeout.InfiniteTimeSpan);
            }
            else
            {
                exportDebounce.Change(due, Timeout.InfiniteTimeSpan);
            }
        }

        private void RunExport()
        {
            try
            {
                exporter.Export();
            }
            catch (Exception e)
            {
                Log.Error(e, "LavApollo Link: library export failed");
            }
        }

        private void StartCommandWatcher()
        {
            try
            {
                commandWatcher = new FileSystemWatcher(Exchange.CommandsDir, "*.json")
                {
                    NotifyFilter = NotifyFilters.FileName | NotifyFilters.LastWrite,
                    EnableRaisingEvents = true,
                };
                commandWatcher.Created += (s, e) => Task.Run(ProcessCommands);
                commandWatcher.Renamed += (s, e) => Task.Run(ProcessCommands);
            }
            catch (Exception e)
            {
                Log.Warn($"LavApollo Link: no command watcher ({e.Message}), polling only");
            }

            // Watchers miss events across sessions and services: poll as a backstop.
            commandPoll = new Timer(_ => ProcessCommands(), null, TimeSpan.FromSeconds(2), TimeSpan.FromSeconds(2));
        }

        private void ProcessCommands()
        {
            string[] files;
            try
            {
                files = Directory.GetFiles(Exchange.CommandsDir, "*.json");
            }
            catch (Exception)
            {
                return;
            }

            foreach (var file in files.OrderBy(f => f))
            {
                lock (handledCommands)
                {
                    if (!handledCommands.Add(Path.GetFileName(file)))
                    {
                        TryDelete(file);
                        continue;
                    }
                }

                Command command = null;
                try
                {
                    command = Serialization.FromJson<Command>(File.ReadAllText(file));
                }
                catch (Exception e)
                {
                    Log.Warn($"LavApollo Link: unreadable command {file}: {e.Message}");
                }
                TryDelete(file);

                if (command == null || IsStale(command))
                {
                    continue;
                }

                if (command.action == "stop-game")
                {
                    StopGame(command.gameId);
                }
            }
        }

        // A command left behind while Playnite was closed must not close a game started later.
        private static bool IsStale(Command command)
        {
            if (DateTime.TryParse(command.issuedAt, null, System.Globalization.DateTimeStyles.RoundtripKind, out var issued))
            {
                return DateTime.UtcNow - issued.ToUniversalTime() > TimeSpan.FromMinutes(1);
            }
            return false;
        }

        private void StopGame(string gameId)
        {
            Game game;
            int pid;
            lock (stateLock)
            {
                if (state.game == null)
                {
                    return;
                }
                if (!string.IsNullOrEmpty(gameId) && !string.Equals(gameId, state.game.id, StringComparison.OrdinalIgnoreCase))
                {
                    Log.Info($"LavApollo Link: stop asked for {gameId}, but {state.game.id} is running; ignored");
                    return;
                }
                game = runningGame;
                pid = state.game.pid;
            }

            Log.Info($"LavApollo Link: closing {game?.Name} for LavApollo");
            GameStopper.Stop(game, pid);
        }

        private static void TryDelete(string file)
        {
            try { File.Delete(file); } catch (Exception) { }
        }
    }
}
