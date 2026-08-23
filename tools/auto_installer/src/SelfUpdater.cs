using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace CyberpunkVRPort.AutoInstaller
{
    internal static class SelfUpdater
    {
        private const string ReplaceArgument = "--replace-exe";

        internal static bool CompleteReplacementIfRequested(string[] args, out string[] remainingArguments)
        {
            remainingArguments = args;
            if (args.Length < 3 || !args[0].Equals(ReplaceArgument, StringComparison.OrdinalIgnoreCase)) return false;
            remainingArguments = args.Skip(3).ToArray();
            int oldProcessId;
            if (!int.TryParse(args[1], out oldProcessId)) return true;
            var target = Path.GetFullPath(args[2]);
            InstallerLog.Write("Completing installer self-update for " + target);
            try
            {
                try { Process.GetProcessById(oldProcessId).WaitForExit(30000); } catch (ArgumentException) { }
                var source = Path.GetFullPath(Application.ExecutablePath);
                Exception lastError = null;
                for (var attempt = 0; attempt < 20; attempt++)
                {
                    try
                    {
                        File.Copy(source, target, true);
                        InstallerLog.Write("Installer self-update replaced " + target);
                        Process.Start(new ProcessStartInfo(target, JoinArguments(remainingArguments)) { UseShellExecute = true });
                        return true;
                    }
                    catch (Exception exception)
                    {
                        lastError = exception;
                        Thread.Sleep(250);
                    }
                }
                MessageBox.Show("The updated installer could not replace the original file. The latest copy will run from the local cache.\n\n" +
                    lastError?.Message, "Cyberpunk VR Port Auto Installer", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return false;
            }
            catch (Exception exception)
            {
                MessageBox.Show(exception.Message, "Cyberpunk VR Port Auto Installer", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return true;
            }
        }

        internal static bool IsUpdateRequired(GitHubRelease latest)
        {
            var asset = latest?.InstallerAsset;
            if (asset == null || string.IsNullOrWhiteSpace(asset.Digest))
                throw new InvalidDataException("The latest release does not contain a verifiable Auto Installer asset.");
            var current = GitHubReleaseClient.ComputeSha256(Application.ExecutablePath);
            var expected = asset.Digest.Contains(":") ? asset.Digest.Split(':').Last().Trim() : asset.Digest.Trim();
            return !string.Equals(current, expected, StringComparison.OrdinalIgnoreCase);
        }

        internal static async Task StartUpdateAsync(
            GitHubReleaseClient client, GitHubRelease latest, params string[] resumeArguments)
        {
            if (!IsUpdateRequired(latest)) return;
            var downloaded = await client.DownloadToCacheAsync(latest, latest.InstallerAsset).ConfigureAwait(true);
            InstallerLog.Write("Downloaded installer update to " + downloaded);
            var replacementArguments = new[]
            {
                ReplaceArgument,
                Process.GetCurrentProcess().Id.ToString(),
                Application.ExecutablePath
            }.Concat(resumeArguments ?? Array.Empty<string>());
            Process.Start(new ProcessStartInfo(downloaded, JoinArguments(replacementArguments)) { UseShellExecute = true });
        }

        private static string JoinArguments(System.Collections.Generic.IEnumerable<string> arguments) =>
            string.Join(" ", arguments.Select(Quote));

        private static string Quote(string value) => "\"" + (value ?? string.Empty).Replace("\"", "\\\"") + "\"";
    }
}
