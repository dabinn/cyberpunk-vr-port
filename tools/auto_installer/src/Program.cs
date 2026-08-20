using System;
using System.Linq;
using System.Reflection;
using System.Windows.Forms;

namespace CyberpunkVRPort.AutoInstaller
{
    internal static class Program
    {
        [STAThread]
        private static void Main(string[] args)
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            string[] launchArguments;
            if (SelfUpdater.CompleteReplacementIfRequested(args, out launchArguments)) return;
            args = launchArguments;
            var devMode = args.Any(argument => argument.Equals("--dev", StringComparison.OrdinalIgnoreCase));
            using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(
                "CyberpunkVRPort.AutoInstaller.config.ini"))
            {
                if (stream == null)
                {
                    MessageBox.Show("Embedded installer configuration is missing.", "Cyberpunk VR Port Auto Installer",
                        MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }
                Application.Run(new MainForm(IniDocument.Load(stream), devMode, args));
            }
        }
    }
}
