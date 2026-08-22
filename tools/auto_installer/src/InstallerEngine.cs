using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;

namespace CyberpunkVRPort.AutoInstaller
{
    internal sealed class InstallerEngine
    {
        private const string StateRelativePath = @"bin\x64\CyberpunkVRPort-Auto-Installer.state.ini";
        private const string BackupRelativeDirectory = @"bin\x64\CyberpunkVRPort-Auto-Installer.backups";
        private const string CorePluginRelativePath =
            @"red4ext\plugins\CyberpunkVR_Stereo\CyberpunkVR_Stereo.dll";
        private const string VrportIniRelativePath = @"bin\x64\vrport.ini";
        private const string DefaultVrportIni =
            "xr_head_offset_x=0.000\n" +
            "xr_head_offset_y=0.000\n" +
            "xr_head_offset_z=0.000\n" +
            "xr_recenter=0\n" +
            "xr_mono_submit=1\n" +
            "xr_force_fov=0\n" +
            "xr_menu_rect=0\n" +
            "xr_menu_fov=65.0\n" +
            "xr_menu_follow_deg=60.0\n" +
            "xr_3dof_movement=0\n" +
            "first_launch=1\n" +
            "xr_motion_predict_ms=0.0\n" +
            "xr_stereo_scale=1.0\n" +
            "xr_world_scale=1.0\n" +
            "xr_ipd_scale=1.0\n" +
            "xr_sharpness=0.0\n" +
            "xr_sharpmix=1.0\n" +
            "xr_reuse_last_frame=0\n" +
            "xr_hmd_smooth=0.35\n" +
            "xr_hand_smooth=0.45\n" +
            "xr_pair_lock=0\n" +
            "xr_render_pose_submit=1\n" +
            "xr_pose_lag=1\n" +
            "xr_runtime=0\n" +
            "xr_depth_submit=1\n" +
            "xr_movement_control=0\n" +
            "xr_disable_mouse_y=1\n" +
            "xr_xinput_hook=1\n" +
            "xr_snap_turn=0\n" +
            "xr_snap_turn_angle_deg=30\n" +
            "xr_movement_source=0\n" +
            "xr_xinput_install=1\n" +
            "xr_input_actions=1\n" +
            "xr_chord_activation=0\n" +
            "xr_extra_chord_actions=1\n" +
            "xr_mono_xqueue_wait=0\n" +
            "xr_snap_turn_pulse_ms=30\n" +
            "xr_mono_depth_capture=1\n";
        private readonly List<string> payloadFilesEver;
        private readonly List<string> generatedFiles;
        private readonly List<string> ownedDirectoriesEver;

        internal InstallerEngine(IniDocument ini)
        {
            payloadFilesEver = ReadPaths(ini, "PayloadFilesEver");
            generatedFiles = ReadPaths(ini, "GeneratedFiles");
            ownedDirectoriesEver = ReadPaths(ini, "OwnedDirectoriesEver");
            if (payloadFilesEver.Count == 0)
                throw new InvalidDataException("The embedded [PayloadFilesEver] catalog is empty.");
        }

        internal int Install(string payloadRoot, bool directRootLayout, string gameRoot,
            string installationSource, string installationVersion, Action<string> progress = null)
        {
            AssertGameClosed();
            var root = NormalizeGameRoot(gameRoot);
            var normalizedPayloadRoot = Path.GetFullPath(payloadRoot);
            if (!Directory.Exists(normalizedPayloadRoot))
                throw new DirectoryNotFoundException("Package payload is missing: " + normalizedPayloadRoot);
            var copies = new List<InstallCopy>();
            foreach (var source in EnumerateFilesSafely(normalizedPayloadRoot, normalizedPayloadRoot))
            {
                AssertNoReparsePoint(normalizedPayloadRoot, source);
                var relative = source.Substring(normalizedPayloadRoot.TrimEnd(Path.DirectorySeparatorChar).Length)
                    .TrimStart(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
                if (directRootLayout && !PackageSource.IsGamePayloadPath(relative)) continue;
                var destination = ResolveUnder(root, relative);
                AssertNoReparsePoint(root, destination);
                copies.Add(new InstallCopy(source, destination, relative));
            }
            if (copies.Count == 0) throw new InvalidDataException("The selected package contains no payload files.");

            var stateExists = StateExists(root);
            var state = LoadState(root);
            var cleanupPaths = stateExists ? state.InstalledFilesEver : payloadFilesEver;
            PreservePreexistingFiles(root, copies, state, cleanupPaths);
            foreach (var copy in copies) state.AddInstalledFile(copy.RelativePath);
            SaveState(root, state);

            progress?.Invoke("Cleaning");
            PreInstallCleanup(root, cleanupPaths, !stateExists);
            progress?.Invoke("Installing");
            foreach (var copy in copies)
            {
                var parent = Path.GetDirectoryName(copy.Destination);
                if (!string.IsNullOrEmpty(parent)) Directory.CreateDirectory(parent);
                File.Copy(copy.Source, copy.Destination, true);
            }
            state.Source = installationSource == null ? string.Empty : installationSource.Trim();
            state.Version = installationVersion == null ? string.Empty : installationVersion.Trim();
            SaveState(root, state);
            return copies.Count;
        }

        internal bool ForceCreateVrportIni(string gameRoot)
        {
            AssertGameClosed();
            return CreateVrportIniIfMissing(NormalizeGameRoot(gameRoot));
        }

        private static bool CreateVrportIniIfMissing(string root)
        {
            var path = ResolveUnder(root, VrportIniRelativePath);
            AssertNoReparsePoint(root, path);
            if (File.Exists(path)) return false;
            var parent = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(parent)) Directory.CreateDirectory(parent);
            try
            {
                using (var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read))
                using (var writer = new StreamWriter(stream, new UTF8Encoding(false)))
                    writer.Write(DefaultVrportIni);
                return true;
            }
            catch (IOException)
            {
                if (!File.Exists(path)) throw;
                return false;
            }
        }

        internal int Uninstall(string gameRoot)
        {
            AssertGameClosed();
            var root = NormalizeGameRoot(gameRoot);
            var stateExists = StateExists(root);
            var state = LoadState(root);
            ValidateBackups(root, state);
            var removed = DeleteListedFiles(root, stateExists
                ? state.InstalledFilesEver
                : payloadFilesEver.Concat(generatedFiles));
            if (!stateExists)
            {
                foreach (var relative in ownedDirectoriesEver.OrderByDescending(path => path.Length))
                {
                    var target = ResolveUnder(root, relative);
                    AssertNoReparsePoint(root, target);
                    if (!Directory.Exists(target)) continue;
                    Directory.Delete(target, true);
                    removed++;
                    RemoveEmptyParents(Path.GetDirectoryName(target), root);
                }
            }
            RestoreBackups(root, state);
            if (stateExists) DeleteState(root);
            return removed;
        }

        internal InstallationStatus GetInstallationStatus(string gameRoot)
        {
            if (string.IsNullOrWhiteSpace(gameRoot))
                return new InstallationStatus(false, false, false, null, null, false);
            try
            {
                var root = NormalizeGameRoot(gameRoot);
                var vrportIni = ResolveUnder(root, VrportIniRelativePath);
                AssertNoReparsePoint(root, vrportIni);
                var vrportIniExists = File.Exists(vrportIni);
                if (!StateExists(root))
                {
                    var corePlugin = ResolveUnder(root, CorePluginRelativePath);
                    AssertNoReparsePoint(root, corePlugin);
                    var externalInstall = File.Exists(corePlugin);
                    return new InstallationStatus(externalInstall, externalInstall, false, null, null, vrportIniExists);
                }
                try
                {
                    var state = LoadState(root);
                    return new InstallationStatus(true, false, true, state.Source, state.Version, vrportIniExists);
                }
                catch (Exception exception)
                {
                    InstallerLog.Write("ERROR Could not read Installer state for status display: " + exception.Message);
                    return new InstallationStatus(true, false, true, null, null, vrportIniExists);
                }
            }
            catch
            {
                return new InstallationStatus(false, false, false, null, null, false);
            }
        }

        private void PreInstallCleanup(string root, IEnumerable<string> cleanupPaths, bool useEmbeddedFallback)
        {
            DeleteListedFiles(root, cleanupPaths);
            if (!useEmbeddedFallback) return;
            foreach (var relative in ownedDirectoriesEver.OrderByDescending(path => path.Length))
            {
                var target = ResolveUnder(root, relative);
                AssertNoReparsePoint(root, target);
                if (Directory.Exists(target) && !Directory.EnumerateFileSystemEntries(target).Any())
                {
                    Directory.Delete(target);
                    RemoveEmptyParents(Path.GetDirectoryName(target), root);
                }
            }
        }

        private static void PreservePreexistingFiles(string root, IEnumerable<InstallCopy> copies,
            InstallerState state, IEnumerable<string> knownOwnedPaths)
        {
            var knownOwned = new HashSet<string>(knownOwnedPaths, StringComparer.OrdinalIgnoreCase);
            foreach (var copy in copies)
            {
                if (!File.Exists(copy.Destination) || knownOwned.Contains(copy.RelativePath) ||
                    state.HasBackup(copy.RelativePath)) continue;

                AssertNoReparsePoint(root, copy.Destination);
                var backupName = GetBackupName(copy.RelativePath);
                var backupRoot = ResolveUnder(root, BackupRelativeDirectory);
                var backupPath = ResolveUnder(backupRoot, backupName);
                AssertNoReparsePoint(root, backupPath);
                Directory.CreateDirectory(backupRoot);
                if (!File.Exists(backupPath)) File.Copy(copy.Destination, backupPath, false);
                state.AddBackup(copy.RelativePath, backupName);
            }
        }

        private static void ValidateBackups(string root, InstallerState state)
        {
            var backupRoot = ResolveUnder(root, BackupRelativeDirectory);
            foreach (var backup in state.Backups)
            {
                ResolveUnder(root, backup.Key);
                var source = ResolveUnder(backupRoot, backup.Value);
                AssertNoReparsePoint(root, source);
                if (!File.Exists(source))
                    throw new FileNotFoundException("A preserved pre-install file is missing: " + source, source);
            }
        }

        private static void RestoreBackups(string root, InstallerState state)
        {
            var backupRoot = ResolveUnder(root, BackupRelativeDirectory);
            foreach (var backup in state.Backups)
            {
                var source = ResolveUnder(backupRoot, backup.Value);
                var destination = ResolveUnder(root, backup.Key);
                AssertNoReparsePoint(root, destination);
                var parent = Path.GetDirectoryName(destination);
                if (!string.IsNullOrEmpty(parent)) Directory.CreateDirectory(parent);
                File.Copy(source, destination, true);
            }
        }

        private static InstallerState LoadState(string root)
        {
            var path = ResolveUnder(root, StateRelativePath);
            AssertNoReparsePoint(root, path);
            if (!File.Exists(path)) return new InstallerState();
            var ini = IniDocument.Load(path);
            var state = new InstallerState();
            state.Source = ini.Get("Installation", "Source");
            state.Version = ini.Get("Installation", "Version");
            foreach (var entry in ini.GetSection("InstalledFilesEver")) state.AddInstalledFile(entry.Value);
            foreach (var entry in ini.GetSection("Backups"))
            {
                var separator = entry.Value.IndexOf('|');
                if (separator <= 0 || separator == entry.Value.Length - 1)
                    throw new InvalidDataException("Invalid Auto Installer backup entry: " + entry.Value);
                state.AddBackup(entry.Value.Substring(0, separator), entry.Value.Substring(separator + 1));
            }
            return state;
        }

        private static bool StateExists(string root)
        {
            var path = ResolveUnder(root, StateRelativePath);
            AssertNoReparsePoint(root, path);
            return File.Exists(path);
        }

        private static void SaveState(string root, InstallerState state)
        {
            var path = ResolveUnder(root, StateRelativePath);
            AssertNoReparsePoint(root, path);
            var parent = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(parent)) Directory.CreateDirectory(parent);
            var temporary = path + ".tmp";
            var builder = new StringBuilder();
            builder.AppendLine("; Generated by CyberpunkVRPort Auto Installer. Do not edit paths manually.");
            builder.AppendLine("[Installation]");
            builder.AppendLine("Source=" + NormalizeStateValue(state.Source));
            builder.AppendLine("Version=" + NormalizeStateValue(state.Version));
            builder.AppendLine();
            builder.AppendLine("[InstalledFilesEver]");
            var index = 1;
            foreach (var relative in state.InstalledFilesEver.OrderBy(value => value, StringComparer.OrdinalIgnoreCase))
                builder.AppendLine(index++.ToString("D4") + "=" + NormalizeStatePath(relative));
            builder.AppendLine();
            builder.AppendLine("[Backups]");
            index = 1;
            foreach (var backup in state.Backups.OrderBy(value => value.Key, StringComparer.OrdinalIgnoreCase))
                builder.AppendLine(index++.ToString("D4") + "=" + NormalizeStatePath(backup.Key) + "|" + backup.Value);
            File.WriteAllText(temporary, builder.ToString(), new UTF8Encoding(false));
            if (File.Exists(path)) File.Replace(temporary, path, null);
            else File.Move(temporary, path);
        }

        private static void DeleteState(string root)
        {
            var statePath = ResolveUnder(root, StateRelativePath);
            if (File.Exists(statePath)) File.Delete(statePath);
            var backupRoot = ResolveUnder(root, BackupRelativeDirectory);
            AssertNoReparsePoint(root, backupRoot);
            if (Directory.Exists(backupRoot)) Directory.Delete(backupRoot, true);
        }

        private static string GetBackupName(string relative)
        {
            using (var sha = SHA256.Create())
            {
                var bytes = sha.ComputeHash(Encoding.UTF8.GetBytes(NormalizeStatePath(relative).ToLowerInvariant()));
                return string.Concat(bytes.Select(value => value.ToString("x2"))) + ".bak";
            }
        }

        private static string NormalizeStatePath(string relative)
        {
            return relative.Replace(Path.DirectorySeparatorChar, '/').Replace(Path.AltDirectorySeparatorChar, '/');
        }

        private static string NormalizeStateValue(string value)
        {
            return (value ?? string.Empty).Replace("\r", " ").Replace("\n", " ").Trim();
        }

        private static int DeleteListedFiles(string root, IEnumerable<string> paths)
        {
            var removed = 0;
            foreach (var relative in paths.Distinct(StringComparer.OrdinalIgnoreCase))
            {
                var target = ResolveUnder(root, relative);
                AssertNoReparsePoint(root, target);
                if (!File.Exists(target)) continue;
                File.SetAttributes(target, FileAttributes.Normal);
                File.Delete(target);
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

        private static IEnumerable<string> EnumerateFilesSafely(string root, string directory)
        {
            AssertNoReparsePoint(root, directory);
            foreach (var file in Directory.EnumerateFiles(directory))
            {
                AssertNoReparsePoint(root, file);
                yield return file;
            }
            foreach (var child in Directory.EnumerateDirectories(directory))
            {
                AssertNoReparsePoint(root, child);
                foreach (var file in EnumerateFilesSafely(root, child)) yield return file;
            }
        }

        private static string NormalizeGameRoot(string gameRoot)
        {
            var root = Path.GetFullPath(gameRoot).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            if (!SteamLocator.IsGameRoot(root)) throw new InvalidDataException("Invalid Cyberpunk 2077 folder: " + root);
            return root;
        }

        internal static string ResolveUnder(string root, string relative)
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

        internal static void AssertNoReparsePoint(string root, string target)
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

        internal static void AssertGameClosed()
        {
            if (Process.GetProcessesByName("Cyberpunk2077").Length > 0)
                throw new InvalidOperationException("Cyberpunk2077.exe is running.");
        }

        private sealed class InstallCopy
        {
            internal string Source { get; private set; }
            internal string Destination { get; private set; }
            internal string RelativePath { get; private set; }

            internal InstallCopy(string source, string destination, string relativePath)
            {
                Source = source;
                Destination = destination;
                RelativePath = relativePath.Replace('/', Path.DirectorySeparatorChar);
            }
        }

        private sealed class InstallerState
        {
            private readonly List<string> installedFilesEver = new List<string>();
            private readonly List<KeyValuePair<string, string>> backups = new List<KeyValuePair<string, string>>();

            internal IReadOnlyList<string> InstalledFilesEver { get { return installedFilesEver; } }
            internal IReadOnlyList<KeyValuePair<string, string>> Backups { get { return backups; } }
            internal string Source { get; set; }
            internal string Version { get; set; }

            internal void AddInstalledFile(string relative)
            {
                var normalized = relative.Trim().Replace('/', Path.DirectorySeparatorChar);
                if (normalized.Length == 0 || installedFilesEver.Contains(normalized, StringComparer.OrdinalIgnoreCase)) return;
                installedFilesEver.Add(normalized);
            }

            internal bool HasBackup(string relative)
            {
                return backups.Any(value => value.Key.Equals(relative, StringComparison.OrdinalIgnoreCase));
            }

            internal void AddBackup(string relative, string backupName)
            {
                var normalized = relative.Trim().Replace('/', Path.DirectorySeparatorChar);
                if (normalized.Length == 0 || Path.GetFileName(backupName) != backupName ||
                    !backupName.EndsWith(".bak", StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("Invalid Auto Installer state path.");
                if (!HasBackup(normalized)) backups.Add(new KeyValuePair<string, string>(normalized, backupName));
            }
        }

        internal sealed class InstallationStatus
        {
            internal bool Installed { get; }
            internal bool ExternalInstall { get; }
            internal bool HasInstallerState { get; }
            internal string Source { get; }
            internal string Version { get; }
            internal bool VrportIniExists { get; }

            internal InstallationStatus(bool installed, bool externalInstall, bool hasInstallerState,
                string source, string version, bool vrportIniExists)
            {
                Installed = installed;
                ExternalInstall = externalInstall;
                HasInstallerState = hasInstallerState;
                Source = source;
                Version = version;
                VrportIniExists = vrportIniExists;
            }
        }
    }
}
