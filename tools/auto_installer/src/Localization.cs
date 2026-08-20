using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace CyberpunkVRPort.AutoInstaller
{
    internal sealed class LanguagePack
    {
        internal string Code { get; }
        internal string DisplayName { get; }
        private readonly Dictionary<string, string> strings;

        internal LanguagePack(string code, Dictionary<string, string> strings)
        {
            Code = code;
            this.strings = strings;
            DisplayName = Get("Language", code);
        }

        internal string Get(string key, string fallback)
        {
            string value;
            return strings.TryGetValue(key, out value) ? value : fallback;
        }

        public override string ToString() => DisplayName;
    }

    internal static class Localization
    {
        internal static List<LanguagePack> Load(IniDocument ini)
        {
            var codes = ini.Get("Installer", "Languages", "en-US")
                .Split(new[] { ',' }, StringSplitOptions.RemoveEmptyEntries)
                .Select(code => code.Trim());
            var packs = new List<LanguagePack>();
            foreach (var code in codes)
            {
                var values = ini.GetSection(code).ToDictionary(pair => pair.Key, pair => pair.Value, StringComparer.OrdinalIgnoreCase);
                if (values.Count > 0) packs.Add(new LanguagePack(code, values));
            }
            if (packs.All(pack => !pack.Code.Equals("en-US", StringComparison.OrdinalIgnoreCase)))
                packs.Insert(0, EnglishFallback());
            return packs;
        }

        internal static LanguagePack ChooseDefault(IReadOnlyList<LanguagePack> packs)
        {
            var ui = CultureInfo.CurrentUICulture;
            var exact = packs.FirstOrDefault(pack => pack.Code.Equals(ui.Name, StringComparison.OrdinalIgnoreCase));
            if (exact != null) return exact;
            if (ui.TwoLetterISOLanguageName.Equals("zh", StringComparison.OrdinalIgnoreCase))
            {
                var preferred = ui.Name.IndexOf("Hans", StringComparison.OrdinalIgnoreCase) >= 0 ||
                                ui.Name.EndsWith("-CN", StringComparison.OrdinalIgnoreCase) ||
                                ui.Name.EndsWith("-SG", StringComparison.OrdinalIgnoreCase)
                    ? "zh-CN"
                    : "zh-TW";
                var chinese = packs.FirstOrDefault(pack => pack.Code.Equals(preferred, StringComparison.OrdinalIgnoreCase));
                if (chinese != null) return chinese;
            }
            return packs.First(pack => pack.Code.Equals("en-US", StringComparison.OrdinalIgnoreCase));
        }

        private static LanguagePack EnglishFallback()
        {
            return new LanguagePack("en-US", new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
            {
                ["Language"] = "English", ["Title"] = "Cyberpunk VR Port Auto Installer",
                ["GamePath"] = "Cyberpunk 2077 folder", ["Browse"] = "Browse...",
                ["Install"] = "Install", ["Uninstall"] = "Uninstall", ["Ready"] = "Select an action.",
                ["Detected"] = "Cyberpunk 2077 was detected automatically.",
                ["NotDetected"] = "Cyberpunk 2077 was not detected. Select its installation folder.",
                ["InvalidGame"] = "The selected folder does not contain bin\\x64\\Cyberpunk2077.exe.",
                ["MissingConfig"] = "CyberpunkVRPort-Auto-Installer.ini is missing beside this program.",
                ["MissingPayload"] = "The Cyberpunk 2077 payload folder is missing beside this program.",
                ["GameRunning"] = "Close Cyberpunk 2077 before continuing.",
                ["ConfirmInstall"] = "Install Cyberpunk VR Port into the selected folder?",
                ["ConfirmUninstall"] = "Remove the listed Cyberpunk VR Port files from the selected folder?",
                ["InstallComplete"] = "Installation completed: {0} file(s) copied.",
                ["UninstallComplete"] = "Uninstall completed: {0} file(s) removed.",
                ["OperationFailed"] = "Operation failed", ["SelectGameFolder"] = "Select the Cyberpunk 2077 installation folder"
            });
        }
    }
}
