using Microsoft.Win32;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace CyberpunkVRPort.AutoInstaller
{
    internal static class SteamLocator
    {
        private const string AppId = "1091500";

        internal static IReadOnlyList<string> FindGameRoots()
        {
            var results = new List<string>();
            foreach (var library in FindSteamLibraries())
            {
                var manifest = Path.Combine(library, "steamapps", "appmanifest_" + AppId + ".acf");
                if (!File.Exists(manifest)) continue;
                var installDirectory = ReadVdfValue(manifest, "installdir");
                if (string.IsNullOrWhiteSpace(installDirectory)) installDirectory = "Cyberpunk 2077";
                var candidate = Path.Combine(library, "steamapps", "common", installDirectory);
                if (IsGameRoot(candidate) && !results.Contains(candidate, StringComparer.OrdinalIgnoreCase))
                    results.Add(Path.GetFullPath(candidate));
            }
            return results;
        }

        internal static bool IsGameRoot(string path)
        {
            if (string.IsNullOrWhiteSpace(path)) return false;
            try { return File.Exists(Path.Combine(Path.GetFullPath(path), "bin", "x64", "Cyberpunk2077.exe")); }
            catch { return false; }
        }

        private static IEnumerable<string> FindSteamLibraries()
        {
            var roots = new List<string>();
            AddRegistrySteamPath(roots, Registry.CurrentUser, @"Software\Valve\Steam", "SteamPath");
            AddRegistrySteamPath(roots, Registry.LocalMachine, @"Software\WOW6432Node\Valve\Steam", "InstallPath");
            AddRegistrySteamPath(roots, Registry.LocalMachine, @"Software\Valve\Steam", "InstallPath");

            foreach (var steamRoot in roots.ToArray())
            {
                var libraryFile = Path.Combine(steamRoot, "steamapps", "libraryfolders.vdf");
                if (!File.Exists(libraryFile)) continue;
                try
                {
                    var text = File.ReadAllText(libraryFile);
                    foreach (Match match in Regex.Matches(text, "\"path\"\\s+\"(?<path>(?:\\\\.|[^\"])*)\"", RegexOptions.IgnoreCase))
                        AddUnique(roots, UnescapeVdf(match.Groups["path"].Value));

                    foreach (Match match in Regex.Matches(text, "^\\s*\"\\d+\"\\s+\"(?<path>(?:\\\\.|[^\"])*)\"", RegexOptions.Multiline))
                        AddUnique(roots, UnescapeVdf(match.Groups["path"].Value));
                }
                catch
                {
                    // Manual folder selection remains available when Steam metadata is unreadable.
                }
            }
            return roots.Where(Directory.Exists);
        }

        private static void AddRegistrySteamPath(List<string> roots, RegistryKey hive, string subkey, string valueName)
        {
            try
            {
                using (var key = hive.OpenSubKey(subkey))
                    AddUnique(roots, key?.GetValue(valueName) as string);
            }
            catch { }
        }

        private static void AddUnique(List<string> paths, string path)
        {
            if (string.IsNullOrWhiteSpace(path)) return;
            try
            {
                var full = Path.GetFullPath(path.Replace('/', Path.DirectorySeparatorChar));
                if (!paths.Contains(full, StringComparer.OrdinalIgnoreCase)) paths.Add(full);
            }
            catch { }
        }

        private static string ReadVdfValue(string path, string key)
        {
            try
            {
                var pattern = "\"" + Regex.Escape(key) + "\"\\s+\"(?<value>(?:\\\\.|[^\"])*)\"";
                var match = Regex.Match(File.ReadAllText(path), pattern, RegexOptions.IgnoreCase);
                return match.Success ? UnescapeVdf(match.Groups["value"].Value) : null;
            }
            catch { return null; }
        }

        private static string UnescapeVdf(string value) => value.Replace("\\\\", "\\").Replace("\\\"", "\"");
    }
}
