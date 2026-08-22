using System;
using System.IO;
using System.IO.Compression;

namespace CyberpunkVRPort.AutoInstaller
{
    internal sealed class PackageSource : IDisposable
    {
        private static readonly string[] GamePayloadDirectories =
        {
            "archive", "bin", "engine", "r6", "red4ext"
        };
        private readonly string temporaryRoot;
        internal string PayloadRoot { get; }
        internal bool DirectRootLayout { get; }

        private PackageSource(string payloadRoot, bool directRootLayout, string temporaryRoot = null)
        {
            PayloadRoot = Path.GetFullPath(payloadRoot);
            DirectRootLayout = directRootLayout;
            this.temporaryRoot = temporaryRoot;
        }

        internal static PackageSource OpenFolder(string path, string payloadDirectory)
        {
            var root = Path.GetFullPath(path);
            var layout = ResolveLayout(root, payloadDirectory);
            InstallerEngine.AssertNoReparsePoint(root, layout.PayloadRoot);
            return new PackageSource(layout.PayloadRoot, layout.DirectRootLayout);
        }

        internal static PackageSource OpenZip(string path, string payloadDirectory)
        {
            if (!File.Exists(path)) throw new FileNotFoundException("Package ZIP was not found.", path);
            var tempRoot = Path.Combine(Path.GetTempPath(), "CyberpunkVRPort-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(tempRoot);
            try
            {
                using (var archive = ZipFile.OpenRead(path))
                {
                    foreach (var entry in archive.Entries)
                    {
                        var entryPath = entry.FullName.Replace('/', Path.DirectorySeparatorChar);
                        if (string.IsNullOrWhiteSpace(entryPath)) continue;
                        var destination = InstallerEngine.ResolveUnder(tempRoot, entryPath);
                        if (entry.FullName.EndsWith("/", StringComparison.Ordinal) || entry.FullName.EndsWith("\\", StringComparison.Ordinal))
                        {
                            Directory.CreateDirectory(destination);
                            continue;
                        }
                        var parent = Path.GetDirectoryName(destination);
                        if (!string.IsNullOrEmpty(parent)) Directory.CreateDirectory(parent);
                        entry.ExtractToFile(destination, true);
                    }
                }
                var layout = ResolveLayout(tempRoot, payloadDirectory);
                return new PackageSource(layout.PayloadRoot, layout.DirectRootLayout, tempRoot);
            }
            catch
            {
                TryDelete(tempRoot);
                throw;
            }
        }

        private static PackageLayout ResolveLayout(string root, string payloadDirectory)
        {
            var wrappedPayload = Path.Combine(root, payloadDirectory);
            if (Directory.Exists(wrappedPayload)) return new PackageLayout(wrappedPayload, false);
            foreach (var directory in GamePayloadDirectories)
            {
                if (Directory.Exists(Path.Combine(root, directory))) return new PackageLayout(root, true);
            }
            throw new InvalidDataException("The package does not contain a supported Cyberpunk 2077 payload layout.");
        }

        private sealed class PackageLayout
        {
            internal string PayloadRoot { get; }
            internal bool DirectRootLayout { get; }

            internal PackageLayout(string payloadRoot, bool directRootLayout)
            {
                PayloadRoot = payloadRoot;
                DirectRootLayout = directRootLayout;
            }
        }

        internal static bool IsGamePayloadPath(string relativePath)
        {
            var normalized = relativePath.Replace(Path.AltDirectorySeparatorChar, Path.DirectorySeparatorChar)
                .TrimStart(Path.DirectorySeparatorChar);
            var separator = normalized.IndexOf(Path.DirectorySeparatorChar);
            var topLevel = separator < 0 ? normalized : normalized.Substring(0, separator);
            return Array.Exists(GamePayloadDirectories,
                directory => directory.Equals(topLevel, StringComparison.OrdinalIgnoreCase));
        }

        public void Dispose()
        {
            if (!string.IsNullOrEmpty(temporaryRoot)) TryDelete(temporaryRoot);
        }

        private static void TryDelete(string path)
        {
            try { if (Directory.Exists(path)) Directory.Delete(path, true); } catch { }
        }
    }
}
