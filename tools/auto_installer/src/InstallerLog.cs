using System;
using System.IO;

namespace CyberpunkVRPort.AutoInstaller
{
    internal static class InstallerLog
    {
        private static readonly string LogPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "CyberpunkVRPort", "AutoInstaller", "CyberpunkVRPort-Auto-Installer.log");

        internal static void Write(string message)
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(LogPath));
                File.AppendAllText(LogPath, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff") + "  " + message + Environment.NewLine);
            }
            catch
            {
                // Logging must never make install or uninstall fail.
            }
        }
    }
}
