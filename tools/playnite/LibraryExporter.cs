using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using Playnite.SDK;
using Playnite.SDK.Models;

namespace LavApolloLink
{
    // Writes library.json, one PNG cover and one JPEG background per exported game. LavApollo only
    // serves PNG box art, and Playnite stores covers as JPG/PNG of any size, so each cover is
    // re-encoded once, at most CoverWidth x CoverHeight, and only again when the source file changes.
    // Backgrounds (LavArtemis Dev's hero image) likewise, at most BackgroundWidth x BackgroundHeight.
    public class LibraryExporter
    {
        public const int CoverWidth = 600;
        public const int CoverHeight = 900;
        public const int BackgroundWidth = 1920;
        public const int BackgroundHeight = 1080;
        private const long BackgroundJpegQuality = 85L;

        private static readonly ILogger Log = LogManager.GetLogger();
        private readonly IPlayniteAPI api;
        private readonly object exportLock = new object();

        public LibraryExporter(IPlayniteAPI api)
        {
            this.api = api;
        }

        // Every game not hidden, installed or not (1.1.0): LavArtemis Dev shows the whole library with
        // an "Installé" filter, and launching a game that is not installed makes Playnite install it
        // (GamesEditor.PlayGame calls InstallGame). Before 1.1.0, installed games only, and no
        // "installed" field: LavApollo reads a missing field as installed.
        public static bool IsExported(Game game)
        {
            return game != null && !game.Hidden;
        }

        public int Export()
        {
            lock (exportLock)
            {
                Exchange.EnsureFolders();

                var library = new ExportedLibrary { exportedAt = Exchange.IsoNow() };
                var keptCovers = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                var keptBackgrounds = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

                foreach (var game in api.Database.Games.Where(IsExported).OrderBy(g => g.SortingName ?? g.Name))
                {
                    var id = game.Id.ToString();
                    var cover = ExportCover(game, id);
                    if (!string.IsNullOrEmpty(cover))
                    {
                        keptCovers.Add(cover);
                    }
                    var background = ExportBackground(game, id);
                    if (!string.IsNullOrEmpty(background))
                    {
                        keptBackgrounds.Add(background);
                    }

                    library.games.Add(new ExportedGame
                    {
                        id = id,
                        name = game.Name,
                        source = game.Source?.Name ?? "",
                        platform = game.Platforms?.FirstOrDefault()?.Name ?? "",
                        favorite = game.Favorite,
                        lastActivity = game.LastActivity?.ToUniversalTime().ToString("o") ?? "",
                        playtime = game.Playtime,
                        installed = game.IsInstalled,
                        cover = cover ?? "",
                        background = background ?? "",
                    });
                }

                RemoveStale(Exchange.CoversDir, "*.png", keptCovers);
                RemoveStale(Exchange.BackgroundsDir, "*.jpg", keptBackgrounds);
                Exchange.WriteJson(Exchange.LibraryFile, library);
                Log.Info($"LavApollo Link: exported {library.games.Count} games");
                return library.games.Count;
            }
        }

        private string ExportCover(Game game, string id)
        {
            if (string.IsNullOrEmpty(game.CoverImage))
            {
                return null;
            }

            try
            {
                var source = api.Database.GetFullFilePath(game.CoverImage);
                if (string.IsNullOrEmpty(source) || !File.Exists(source))
                {
                    return null;
                }

                var fileName = id + ".png";
                var target = Path.Combine(Exchange.CoversDir, fileName);
                if (File.Exists(target) && File.GetLastWriteTimeUtc(target) >= File.GetLastWriteTimeUtc(source))
                {
                    return fileName;
                }

                using (var image = Image.FromFile(source))
                {
                    var scale = Math.Min(1.0, Math.Min((double)CoverWidth / image.Width, (double)CoverHeight / image.Height));
                    var width = Math.Max(1, (int)Math.Round(image.Width * scale));
                    var height = Math.Max(1, (int)Math.Round(image.Height * scale));
                    using (var bitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb))
                    {
                        using (var g = Graphics.FromImage(bitmap))
                        {
                            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                            g.CompositingQuality = CompositingQuality.HighQuality;
                            g.DrawImage(image, 0, 0, width, height);
                        }

                        var tmp = target + ".tmp";
                        bitmap.Save(tmp, ImageFormat.Png);
                        if (File.Exists(target))
                        {
                            File.Delete(target);
                        }
                        File.Move(tmp, target);
                    }
                }
                return fileName;
            }
            catch (Exception e)
            {
                // WebP and other formats System.Drawing cannot read: the game is still exported,
                // LavApollo shows its default box art.
                Log.Warn($"LavApollo Link: no cover for {game.Name}: {e.Message}");
                return null;
            }
        }

        private string ExportBackground(Game game, string id)
        {
            if (string.IsNullOrEmpty(game.BackgroundImage))
            {
                return null;
            }

            try
            {
                // A URL that Playnite has not downloaded is not a file: no background
                var source = api.Database.GetFullFilePath(game.BackgroundImage);
                if (string.IsNullOrEmpty(source) || !File.Exists(source))
                {
                    return null;
                }

                var fileName = id + ".jpg";
                var target = Path.Combine(Exchange.BackgroundsDir, fileName);
                if (File.Exists(target) && File.GetLastWriteTimeUtc(target) >= File.GetLastWriteTimeUtc(source))
                {
                    return fileName;
                }

                using (var image = Image.FromFile(source))
                {
                    var scale = Math.Min(1.0, Math.Min((double)BackgroundWidth / image.Width, (double)BackgroundHeight / image.Height));
                    var width = Math.Max(1, (int)Math.Round(image.Width * scale));
                    var height = Math.Max(1, (int)Math.Round(image.Height * scale));
                    using (var bitmap = new Bitmap(width, height, PixelFormat.Format24bppRgb))
                    {
                        using (var g = Graphics.FromImage(bitmap))
                        {
                            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                            g.CompositingQuality = CompositingQuality.HighQuality;
                            g.DrawImage(image, 0, 0, width, height);
                        }

                        var jpeg = ImageCodecInfo.GetImageEncoders().First(c => c.FormatID == ImageFormat.Jpeg.Guid);
                        using (var parameters = new EncoderParameters(1))
                        {
                            parameters.Param[0] = new EncoderParameter(Encoder.Quality, BackgroundJpegQuality);
                            var tmp = target + ".tmp";
                            bitmap.Save(tmp, jpeg, parameters);
                            if (File.Exists(target))
                            {
                                File.Delete(target);
                            }
                            File.Move(tmp, target);
                        }
                    }
                }
                return fileName;
            }
            catch (Exception e)
            {
                // Same as covers: a format System.Drawing cannot read leaves the game without a
                // background (the clients blur its cover instead).
                Log.Warn($"LavApollo Link: no background for {game.Name}: {e.Message}");
                return null;
            }
        }

        private static void RemoveStale(string dir, string pattern, HashSet<string> kept)
        {
            foreach (var file in Directory.GetFiles(dir, pattern))
            {
                if (!kept.Contains(Path.GetFileName(file)))
                {
                    try { File.Delete(file); } catch (IOException) { }
                }
            }
        }
    }
}
