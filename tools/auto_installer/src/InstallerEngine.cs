using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;

namespace CyberpunkVRPort.AutoInstaller
{
    internal sealed class InstallerEngine
    {
        private readonly string packageRoot;
        private readonly string payloadRoot;
        private readonly List<string> files;
        private readonly List<string> removeFiles;
        private readonly List<string> removeDirectories;

        internal InstallerEngine(string packageRoot, IniDocument ini)
        {
            this.packageRoot = Path.GetFullPath(packageRoot);
            payloadRoot = Path.Combine(this.packageRoot, ini.Get("Installer", "PayloadDirectory", "Cyberpunk 2077"));
            files = ReadPaths(ini, "Files");
            removeFiles = ReadPaths(ini, "RemoveFiles");
            removeDirectories = ReadPaths(ini, "RemoveDirectories");
            if (files.Count == 0) throw new InvalidDataException("The [Files] section is empty.");
        }

        internal bool PayloadExists => Directory.Exists(payloadRoot);

        internal int Install(string gameRoot)
        {
            AssertGameClosed();
            var root = NormalizeGameRoot(gameRoot);
            var copies = new List<KeyValuePair<string, string>>();
            foreach (var relative in files)
            {
                var source = ResolveUnder(payloadRoot, relative);
                var destination = ResolveUnder(root, relative);
                if (!File.Exists(source)) throw new FileNotFoundException("Payload file is missing: " + relative, source);
                AssertNoReparsePoint(payloadRoot, source);
                AssertNoReparsePoint(root, destination);
                copies.Add(new KeyValuePair<string, string>(source, destination));
            }

            foreach (var copy in copies)
            {
                var parent = Path.GetDirectoryName(copy.Value);
                if (!string.IsNullOrEmpty(parent)) Directory.CreateDirectory(parent);
                File.Copy(copy.Key, copy.Value, true);
            }
            return copies.Count;
        }

        internal int Uninstall(string gameRoot)
        {
            AssertGameClosed();
            var root = NormalizeGameRoot(gameRoot);
            var removed = 0;
            foreach (var relative in files.Concat(removeFiles).Distinct(StringComparer.OrdinalIgnoreCase))
            {
                var target = ResolveUnder(root, relative);
                AssertNoReparsePoint(root, target);
                if (!File.Exists(target)) continue;
                File.SetAttributes(target, FileAttributes.Normal);
                File.Delete(target);
                removed++;
                RemoveEmptyParents(Path.GetDirectoryName(target), root);
            }

            foreach (var relative in removeDirectories.OrderByDescending(path => path.Length))
            {
                var target = ResolveUnder(root, relative);
                AssertNoReparsePoint(root, target);
                if (!Directory.Exists(target)) continue;
                Directory.Delete(target, true);
                removed++;
                RemoveEmptyParents(Path.GetDirectoryName(target), root);
            }
            return removed;
        }

        private static List<string> ReadPaths(IniDocument ini, string section)
        {
            var paths = new List<string>();
            foreach (var entry in ini.GetSection(section))
            {
                var value = entry.Value.Trim().Replace('/', Path.DirectorySeparatorChar);
                if (value.Length > 0 && !paths.Contains(value, StringComparer.OrdinalIgnoreCase)) paths.Add(value);
            }
            return paths;
        }

        private static string NormalizeGameRoot(string gameRoot)
        {
            var root = Path.GetFullPath(gameRoot).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            if (!SteamLocator.IsGameRoot(root)) throw new InvalidDataException("Invalid Cyberpunk 2077 folder: " + root);
            return root;
        }

        private static string ResolveUnder(string root, string relative)
        {
            if (string.IsNullOrWhiteSpace(relative) || Path.IsPathRooted(relative))
                throw new InvalidDataException("Unsafe relative path: " + relative);
            var normalizedRoot = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var target = Path.GetFullPath(Path.Combine(normalizedRoot, relative));
            var prefix = normalizedRoot + Path.DirectorySeparatorChar;
            if (!target.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Path escapes its root: " + relative);
            return target;
        }

        private static void AssertNoReparsePoint(string root, string target)
        {
            var normalizedRoot = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var current = normalizedRoot;
            var relative = target.Substring(normalizedRoot.Length).TrimStart(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            foreach (var part in relative.Split(new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar }, StringSplitOptions.RemoveEmptyEntries))
            {
                current = Path.Combine(current, part);
                if (!File.Exists(current) && !Directory.Exists(current)) return;
                if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("Refusing to operate through a reparse point: " + current);
            }
        }

        private static void RemoveEmptyParents(string directory, string root)
        {
            while (!string.IsNullOrEmpty(directory) && !directory.Equals(root, StringComparison.OrdinalIgnoreCase))
            {
                if (!Directory.Exists(directory) || Directory.EnumerateFileSystemEntries(directory).Any()) return;
                Directory.Delete(directory);
                directory = Path.GetDirectoryName(directory);
            }
        }

        private static void AssertGameClosed()
        {
            if (Process.GetProcessesByName("Cyberpunk2077").Length > 0)
                throw new InvalidOperationException("Cyberpunk2077.exe is running.");
        }
    }
}
